// Эмулятор SD-карты в SPI (sd_spi_emu.h). Только логика карты: носитель и
// кэш секторов - за арбитром (storage.h). Своё здесь - два буфера сектора:
// следующий сектор читается, пока хост забирает текущий.

#include "devices/sd/spi_emu.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "platform/log.h"
#include "platform/mono_time.h"
#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "devices/sd/sd_protocol.h"
#include "devices/storage/storage.h"
#include "devices/storage/storage_host.h"

namespace devices::sd {
namespace {

namespace sd = devices::sd;

static_assert(devices::storage::kSectorBytes == sd::kBlockBytes, "сектор носителя - блок данных SD");

// --- Состояние ---


enum class Tx : uint8_t {
    Idle,          // ждём начала команды
    CmdRx,         // добираем шесть байт команды
    Resp,          // отдаём ответ
    ReadToken,     // 0xFF, пока сектор не приехал, потом 0xFE
    ReadData,      // отдаём 512 байт
    ReadCrc,       // два байта CRC16 блока
    WriteToken,    // ждём 0xFE или 0xFD от хоста
    WriteData,     // принимаем 512 байт
    WriteCrc,      // принимаем CRC16 хоста
    WriteResp,     // отдаём "accepted/not accepted"
    WriteBusy,     // 0x00, пока сектор не записан
    BlockToken,    // 0xFE перед CSD/CID
    BlockData,     // 16 байт CSD/CID
    BlockCrc,      // CRC16 этого блока
};

constexpr uint32_t kOwnerCount = 3;
static_assert(static_cast<uint32_t>(SdOwner::ZController) + 1u == kOwnerCount, "kOwnerCount - по числу SdOwner");

// Автомат карты - свой у каждого эмулятора.
//
// Общая у них только сама карта: две стороны видят один носитель, но
// каждая разговаривает со своим экземпляром протокола. Держит одна
// выбранной - второй этого не видно никак, как если бы к каждой был
// подключён свой кристалл. Отсюда отсутствие владельца шины: делить
// нечего, кроме носителя, а его очередь разбирает sd_spi_task.
struct Card {
    bool card_ready = false; // ACMD41 прошла; CMD0 и init снимают
    Tx   tx = Tx::Idle;
    Tx   tx_prev = Tx::Idle;    // для детектора застревания (sd_spi_byte())
    Tx   after_resp = Tx::Idle; // куда уйти, когда ответ отдан
    bool selected = false;

    bool     app_cmd = false; // предыдущей была CMD55
    uint8_t  cmd[sd::kCmdFrameBytes] = {};
    uint8_t  cmd_idx = 0;
    uint32_t phase_bytes = 0;
    bool     stuck_reported = false;

    uint8_t  resp[5] = {};
    uint8_t  resp_len = 0;
    uint8_t  resp_idx = 0;

    bool     multi = false; // идёт CMD18/CMD25
    uint32_t lba = 0;       // сектор текущей операции
    uint16_t byte_idx = 0;  // позиция внутри блока
    uint8_t  crc_idx = 0;

    // Два буфера на сторону: при многоблочном чтении номер следующего
    // сектора известен, и его читают, пока хост забирает текущий.
    uint8_t  block[2][devices::storage::kSectorBytes] = {};
    uint16_t block_crc_buf[2] = {0, 0};
    std::atomic<bool> block_valid[2] = {{false}, {false}};
    uint32_t block_lba[2] = {0, 0};
    std::atomic<uint32_t> block_failed[2] = {{0xFFFFFFFFu}, {0xFFFFFFFFu}};
    uint8_t  cur = 0; // из какого буфера отдаём

    const uint8_t* small = nullptr; // отдаваемый регистр: CSD или CID
    uint16_t small_crc = 0;
    uint8_t  small_idx = 0;

    std::atomic<uint32_t> req{0};
    uint32_t req_posted_us = 0;
    uint32_t token_polls = 0;

    // Что сейчас разбирается - для следа.
    uint8_t  cur_cmd = 0;
    uint32_t cur_arg = 0;
    bool     cur_acmd = false;

    // Чей это автомат: нужно следу и счётчикам по сторонам.
    SdOwner  who = SdOwner::None;
};

Card s_cards[kOwnerCount];

// Чей автомат разбирается прямо сейчас. Ставится на входе в обработчик
// порта; sd_spi_task этим не пользуется - он обходит экземпляры сам.
Card* s_c = &s_cards[0];

// Писатель один: первая сторона, выдавшая CMD24/CMD25, пишет до перезагрузки.
// У DivMMC и Z-Controller свои картины FAT, запись второй портит FAT первой, а
// бит защиты от записи софт почти не смотрит. Поэтому чужой записи - R1 0x40
// (parameter error): драйвер в фазу данных не идёт. Чтение не запрещено.
SdOwner s_write_owner = SdOwner::None;
uint32_t s_write_denied = 0;   // сколько чужих записей отклонено

// Накопительные счётчики обмена по владельцу.
//
// Строка "owner=..." - снимок раз в 16 секунд, короткий обмен она не
// видит: между печатями карта может быть выбрана и отпущена сотню раз.
// Счётчики отвечают, разговаривает ли сторона с картой вообще: приложение
// жалуется на ошибку чтения, а cmd его стороны не растёт - команды до нас
// не доходят, искать на шине.
uint32_t s_cmds[kOwnerCount] = {};     // команд принято, по SdOwner
uint32_t s_reads[kOwnerCount] = {};    // из них чтений сектора
uint8_t s_last_cmd[kOwnerCount] = {};  // последняя команда каждой стороны

// --- След команд карты ---
//
// Кольцо последних команд (kTraceLen), пишется всегда: отказ случайный,
// момент заранее не известен.
//
// Пишется из прерывания: только простые записи в память, разбор строки
// делает основной поток.
constexpr uint32_t kTraceLen = 48;    // степень двойки не нужна, индекс с оборотом
struct TraceEntry {
    uint32_t arg;      // аргумент команды (LBA у чтения и записи)
    uint32_t at_kus;   // когда, в единицах по 1024 мкс (микросекунды >> 10)
    uint8_t  cmd;      // номер команды
    SdOwner  owner;    // кто выдал
    uint8_t  resp;     // чем ответили (первый байт R1)
    uint8_t  acmd;     // была ли перед ней CMD55
};
TraceEntry s_trace[kTraceLen];
// Запись детектора застревания: вместо номера команды и ответа R1.
constexpr uint8_t kTraceStuckCmd = 0xfe;
constexpr uint8_t kTraceStuckResp = 0xee;
volatile uint32_t s_trace_head = 0;   // всего записей; индекс = head % kTraceLen
uint32_t s_trace_pos = 0;             // куда писать следующую, с оборотом сравнением
volatile uint32_t s_trace_bad = 0;    // сколько ответов было ошибочными

// Без делений: зовётся из обработчика порта на каждый ответ R1. Остаток и
// перевод в миллисекунды - в печати, на основном потоке.
void trace_push(uint8_t cmd, uint32_t arg, uint8_t resp, bool acmd) {
    const uint32_t i = s_trace_pos;
    s_trace_pos = (i + 1u < kTraceLen) ? i + 1u : 0u;
    s_trace[i].arg = arg;
    s_trace[i].at_kus = platform::mono_us() >> 10;
    s_trace[i].cmd = cmd;
    s_trace[i].owner = s_c->who;
    s_trace[i].resp = resp;
    s_trace[i].acmd = acmd ? 1u : 0u;
    s_trace_head = s_trace_head + 1u;
    // Ошибочно всё, кроме "готов" и "в простое": такой ответ драйвер
    // показывает как ошибку чтения.
    if (resp != sd::kR1Ready && resp != sd::kR1Idle) ++s_trace_bad;
}

// --- Разбор команды ---


// Сколько байт держится одна фаза обмена (детектор "оглох" в
// sd_spi_byte()). С запасом больше самой длинной законной фазы (сектор
// 512 байт плюс токен и CRC).
constexpr uint32_t kPhaseStuckBytes = 4096;
uint32_t s_stuck_count = 0;


// Два буфера: при многоблочном чтении (CMD18, им Z80-драйвер платы читает
// файл) номер следующего сектора известен, его можно читать,
// пока хост забирает текущий. Иначе на каждой границе блока Z80 крутится в
// ожидании токена сотни микросекунд.
// Публикация готовности атомарная, с барьером: основной поток заполняет
// буфер, считает CRC, пишет номер сектора и только потом выставляет
// признак; обработчик читает его с парным барьером и, увидев признак,
// видит все байты. Пока признака нет, обработчик отдаёт 0xFF - по
// протоколу SD карта шлёт 0xFF до признака начала данных 0xFE.
//
// Сейчас оба конца на Core1, барьеры нужны, чтобы перенос задачи на Core0
// ничего не сломал молча.
// Отказ носителя публикуется так же: основной поток кладёт номер сектора,
// который не прочитался, фазу обмена меняет только обработчик - и только
// если отказ про сектор текущей операции. Писать s_c->tx и s_c->multi из
// основного потока, пока их меняет обработчик, нельзя: устаревший отказ
// оборвал бы свежий обмен.
constexpr uint32_t kNoFailure = 0xFFFFFFFFu;

// Свойства носителя, снятые один раз. Копии в SRAM: арбитр во флеше, и с
// опросом на каждом байте обработчик чтения доходит до 4.87 мкс при IN
// каждые ~4.6 мкс.
bool     s_present = false;
uint32_t s_sectors = 0;
// CSD и CID постоянны: CSD зависит только от s_sectors, CID - константа.
// Собираются с CRC один раз в sd_spi_emu_init, обработчику CMD9/CMD10
// остаётся выбрать готовый блок. Сборка в обработчике - сотни команд CRC на
// команду при IN каждые ~4.5 мкс.
constexpr uint32_t kSmallBlockBytes = sd::kRegisterBytes;
uint8_t s_csd[kSmallBlockBytes];
uint8_t s_cid[kSmallBlockBytes];
uint16_t s_csd_crc = 0;
uint16_t s_cid_crc = 0;

// Заказ носителю - одним словом:
//
//   разряды 0..27 - номер сектора (до 128 ГБ)
//   разряд  28    - номер буфера
//   разряд  31    - признак "заказ есть"
//
// Одним словом, а не тремя переменными: поток мог бы вклиниться между
// записями, взять старый номер и погасить признак за оба заказа, новый
// заказ пропал бы. Слово обработчик кладёт атомарно, поток забирает
// обменом на ноль.
constexpr uint32_t kReqSlotLsb = 28;
constexpr uint32_t kReqValidBit = 1u << 31;
constexpr uint32_t kReqSlotBit = 1u << kReqSlotLsb;
constexpr uint32_t kReqLbaMask = kReqSlotBit - 1u;
// Сколько заказ ждал, пока цикл Core1 его заберёт: это задержка, которую
// видит хост, сверх чтения самого сектора. Наибольшее - за период строки
// лога, число дольше kReqWaitSlowUs - с запуска.
uint32_t s_req_wait_max_us = 0;
uint32_t s_req_wait_slow = 0;
constexpr uint32_t kReqWaitSlowUs = 5000;
// Терпение хоста: байты опроса в ожидании токена чтения. Хост снял выбор,
// не дождавшись, - чтение брошено: сколько опрашивал и сколько прошло от
// заказа. Наибольший опрос, после которого токен пришёл, - для сравнения.
uint32_t s_token_polls_max_ok = 0;
uint32_t s_read_abandoned = 0;
uint32_t s_abandon_polls = 0;
uint32_t s_abandon_us = 0;

// Блок записи - в своём буфере: чтение, которое цикл взял до CMD24,
// дописало бы сектор упреждения поверх блока хоста.
uint8_t s_wblock[devices::storage::kSectorBytes];
// Заказ записи - тоже одним словом: номер сектора и kReqValidBit. Ставит
// обработчик после CRC блока, гасит цикл, когда блок на носителе. Пока
// заказ стоит, карта занята, как настоящая во время программирования:
// отдаёт 0x00 и команд не принимает, в том числе после снятия и нового
// выбора. Хост, снявший выбор сразу после ответа на блок, при следующем
// выборе ждёт 0xFF, и вторая запись не затирает первую.
std::atomic<uint32_t> s_write_req{0};
volatile bool s_write_ok = true;

SOUNDSINTH_ALWAYS_INLINE bool write_pending() {
    return s_write_req.load(std::memory_order_acquire) != 0u;
}

// --- Ответы ---

void SOUNDSINTH_HOT_PATH(respond)(const uint8_t* bytes, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) s_c->resp[i] = bytes[i];
    s_c->resp_len = n;
    s_c->resp_idx = 0;
    s_c->tx = Tx::Resp;
}

// Что сейчас разбирается - для следа: respond1 не знает, на какую
// команду отвечает.

void SOUNDSINTH_HOT_PATH(respond1)(uint8_t r1) {
    trace_push(s_c->cur_cmd, s_c->cur_arg, r1, s_c->cur_acmd);
    respond(&r1, 1);
}

// CSD версии 2: ёмкость прямо, в единицах по 512 КБ, как у настоящего
// носителя - по ней софт узнаёт размер карты. Зовётся из sd_spi_emu_init.
void build_csd(uint8_t* out) {
    std::memset(out, 0, kSmallBlockBytes);
    const uint32_t sectors = s_sectors;
    const uint32_t c_size = (sectors >= 1024u) ? (sectors / 1024u - 1u) : 0u;

    out[0]  = 0x40;             // CSD_STRUCTURE = 1
    out[1]  = 0x0E;             // TAAC
    out[2]  = 0x00;             // NSAC
    out[3]  = 0x32;             // TRAN_SPEED = 25 МГц
    out[4]  = 0x5B;             // CCC
    out[5]  = 0x59;             // CCC | READ_BL_LEN = 9
    out[6]  = 0x00;
    out[7]  = static_cast<uint8_t>((c_size >> 16) & 0x3F);
    out[8]  = static_cast<uint8_t>(c_size >> 8);
    out[9]  = static_cast<uint8_t>(c_size);
    out[10] = 0x7F;
    out[11] = 0x80;
    out[12] = 0x0A;
    out[13] = 0x40;
    out[14] = 0x00;
    out[15] = sd::crc7(out, 15);    // настоящий CRC7
}

void build_cid(uint8_t* out) {
    std::memset(out, 0, kSmallBlockBytes);
    out[0]  = 0x00;             // MID
    out[1]  = 'S'; out[2] = 'S'; // OID
    out[3]  = 'S'; out[4] = 'N'; out[5] = 'D'; out[6] = 'S'; out[7] = 'Y';
    out[8]  = 0x10;             // PRV
    out[9]  = 0x00; out[10] = 0x00; out[11] = 0x00; out[12] = 0x01; // PSN
    out[13] = 0x01;             // MDT
    out[14] = 0x50;
    out[15] = sd::crc7(out, 15);
}

// Отдать готовый блок CSD или CID вслед за R1.
void SOUNDSINTH_HOT_PATH(arm_small_block)(const uint8_t* block, uint16_t crc) {
    s_c->small = block;
    s_c->small_crc = crc;
    s_c->small_idx = 0;
    s_c->after_resp = Tx::BlockToken;
}

// Заказать сектор в слот. Уже лежит - заказывать не надо (выигрыш
// упреждающего чтения).
void SOUNDSINTH_HOT_PATH(request_sector)(uint32_t lba, uint8_t slot) {
    if (s_c->block_valid[slot].load(std::memory_order_acquire) && s_c->block_lba[slot] == lba) return;
    s_c->block_valid[slot].store(false, std::memory_order_relaxed);
    s_c->block_failed[slot].store(kNoFailure, std::memory_order_relaxed);
    if (lba > kReqLbaMask) return;   // за 128 ГБ не ходим
    s_c->req_posted_us = platform::mono_us();
    s_c->req.store(kReqValidBit | (slot ? kReqSlotBit : 0u) | lba, std::memory_order_release);
}

// Готовность выводится из буферов, а не хранится отдельным признаком.
bool SOUNDSINTH_HOT_PATH(cur_ready)() {
    return s_c->block_valid[s_c->cur].load(std::memory_order_acquire) && s_c->block_lba[s_c->cur] == s_c->lba;
}

void SOUNDSINTH_HOT_PATH(start_read)(uint32_t lba, bool multi) {
    if (lba >= s_sectors) {
        respond1(sd::kR1ParamErr); // parameter error
        return;
    }
    s_c->lba = lba;
    s_c->multi = multi;
    s_c->byte_idx = 0;

    // Попадание в заготовленный сектор: если нужный уже готов, ответ сразу,
    // 489 мкс чтения с карты исчезают. Безусловное гашение обоих буферов
    // выбрасывало прочитанное заранее, и упреждение не работало.
    if (s_c->block_valid[0].load(std::memory_order_acquire) && s_c->block_lba[0] == lba) {
        s_c->cur = 0;
    } else if (s_c->block_valid[1].load(std::memory_order_acquire) && s_c->block_lba[1] == lba) {
        s_c->cur = 1;
    } else {
        s_c->cur = 0;
        s_c->block_valid[0].store(false, std::memory_order_relaxed);
        s_c->block_valid[1].store(false, std::memory_order_relaxed);
        request_sector(lba, 0);
    }
    s_c->after_resp = Tx::ReadToken;
    respond1(sd::kR1Ready);
}

void SOUNDSINTH_HOT_PATH(process_cmd)() {
    const uint8_t cmd = static_cast<uint8_t>(s_c->cmd[0] & 0x3F);
    const uint32_t arg = (static_cast<uint32_t>(s_c->cmd[1]) << 24) |
                          (static_cast<uint32_t>(s_c->cmd[2]) << 16) |
                          (static_cast<uint32_t>(s_c->cmd[3]) << 8) |
                          static_cast<uint32_t>(s_c->cmd[4]);

    const bool app = s_c->app_cmd;
    s_c->app_cmd = false;
    s_c->cur_cmd = cmd;
    s_c->cur_arg = arg;
    s_c->cur_acmd = app;

    // Счёт до разбора: важно, что команда пришла, а не принята ли.
    {
        const uint32_t oi = static_cast<uint32_t>(s_c->who);
        if (oi < kOwnerCount) {
            ++s_cmds[oi];
            s_last_cmd[oi] = cmd;
            if (cmd == sd::kCmd17 || cmd == sd::kCmd18) ++s_reads[oi];
        }
    }

    if (app && cmd == sd::kAcmd41) {
        // Инициализация сразу: карта уже поднята драйвером.
        s_c->card_ready = true;
        respond1(sd::kR1Ready);
        return;
    }

    switch (cmd) {
    case sd::kCmd0:
        s_c->card_ready = false;
        s_c->multi = false;
        respond1(sd::kR1Idle);
        return;

    case sd::kCmd8: {
        // R7: R1 + эхо напряжения и проверочного узора
        const uint8_t r7[5] = {sd::kR1Idle, 0x00, 0x00, sd::kIfCondVoltage, static_cast<uint8_t>(arg & 0xFF)};
        respond(r7, 5);
        return;
    }

    case sd::kCmd9:
        arm_small_block(s_csd, s_csd_crc);
        respond1(sd::kR1Ready);
        return;

    case sd::kCmd10:
        arm_small_block(s_cid, s_cid_crc);
        respond1(sd::kR1Ready);
        return;

    case sd::kCmd12:
        // Остановка многоблочного чтения. Ответ R1b: за ним busy, но мы всегда
        // готовы и сразу отпускаем.
        s_c->multi = false;
        s_c->req.store(0, std::memory_order_relaxed);
        respond1(sd::kR1Ready);
        return;

    case sd::kCmd13: {
        // R2: два байта состояния. Драйверы опрашивают её после записи; запись,
        // не легшая на носитель, - общая ошибка во втором байте.
        const uint8_t r2[2] = {sd::kR1Ready, s_write_ok ? uint8_t{0} : sd::kR2Error};
        respond(r2, 2);
        return;
    }

    case sd::kCmd16:
        respond1(arg == devices::storage::kSectorBytes ? sd::kR1Ready : sd::kR1IllegalCmd);
        return;

    case sd::kCmd17:
        start_read(arg, false);
        return;

    case sd::kCmd18:
        start_read(arg, true);
        return;

    case sd::kCmd24:
    case sd::kCmd25:
        // Чужому писателю - отказ (s_write_owner).
        if (s_write_owner != SdOwner::None && s_write_owner != s_c->who) {
            ++s_write_denied;
            respond1(sd::kR1ParamErr);
            return;
        }
        if (arg >= s_sectors || arg > kReqLbaMask) {
            respond1(sd::kR1ParamErr);
            return;
        }
        s_write_owner = s_c->who;
        s_c->req.store(0, std::memory_order_relaxed);   // упреждение уже не нужно
        s_c->lba = arg;
        s_c->multi = (cmd == sd::kCmd25);
        s_c->byte_idx = 0;
        s_c->after_resp = Tx::WriteToken;
        respond1(sd::kR1Ready);
        return;

    case sd::kCmd55:
        s_c->app_cmd = true;
        respond1(s_c->card_ready ? sd::kR1Ready : sd::kR1Idle);
        return;

    case sd::kCmd58: {
        // OCR: CCS=1 (адресация секторами); "питание поднято" и R1 готов - только
        // после ACMD41, до неё idle, как у настоящей карты ("CMD58 0x01, window
        // 40FF8000").
        const uint8_t r3[5] = {s_c->card_ready ? sd::kR1Ready : sd::kR1Idle,
                               static_cast<uint8_t>((s_c->card_ready ? sd::kOcrPowerUpBits : 0u) | sd::kOcrCcsBits), 0xFF,
                               0x80, 0x00};
        respond(r3, 5);
        return;
    }

    case sd::kCmd59:
        respond1(sd::kR1Ready); // CRC считается всегда, включать нечего
        return;

    default:
        respond1(sd::kR1IllegalCmd);
        return;
    }
}


// Карта бросила операцию: снятие выбора или отъём шины у ушедшего
// владельца. Как у настоящей карты: застряв в любой фазе, кроме приёма
// команды, эмулятор оглох бы - из не-Idle начало команды не распознаётся.
// Заказ на запись (s_write_req) не трогается: принятый блок дописывается
// на носитель, чем бы хост ни занялся.
SOUNDSINTH_ALWAYS_INLINE void abort_transfer() {
    if (s_c->tx == Tx::ReadToken) {
        ++s_read_abandoned;
        s_abandon_polls = s_c->token_polls;
        s_abandon_us = platform::mono_us() - s_c->req_posted_us;
    }
    s_c->token_polls = 0;
    s_c->cmd_idx = 0;
    s_c->resp_len = 0;
    s_c->resp_idx = 0;
    s_c->after_resp = Tx::Idle;
    s_c->multi = false;
    s_c->req.store(0, std::memory_order_relaxed);
    s_c->tx = Tx::Idle;
}

const char* owner_name(SdOwner o) {
    return o == SdOwner::None ? "-" : (o == SdOwner::DivMmc ? "divmmc" : "zctrl");
}

// Один такт обмена: что карта отдаёт и что принимает. Ответ считается до
// продвижения состояния, иначе на границах состояний отдавался бы байт
// следующей фазы. Для zcontroller это один такт: там защёлка держит
// результат уже случившегося обмена.
uint8_t SOUNDSINTH_HOT_PATH(card_out)() {
    if (!s_c->selected || !s_present) return sd::kIdleByte;

    switch (s_c->tx) {
    case Tx::Resp:       return s_c->resp[s_c->resp_idx];
    case Tx::ReadToken:  return cur_ready() ? sd::kTokenStartBlock : sd::kIdleByte;
    case Tx::ReadData:   return s_c->block[s_c->cur][s_c->byte_idx];
    case Tx::ReadCrc:    return (s_c->crc_idx == 0) ? static_cast<uint8_t>(s_c->block_crc_buf[s_c->cur] >> 8)
                                                  : static_cast<uint8_t>(s_c->block_crc_buf[s_c->cur]);
    case Tx::WriteResp:  return sd::kDataRespAccepted; // принято
    case Tx::WriteBusy:  return write_pending() ? sd::kBusyByte : sd::kIdleByte;
    case Tx::BlockToken: return sd::kTokenStartBlock;
    case Tx::BlockData:  return s_c->small[s_c->small_idx];
    case Tx::BlockCrc:   return (s_c->crc_idx == 0) ? static_cast<uint8_t>(s_c->small_crc >> 8)
                                                  : static_cast<uint8_t>(s_c->small_crc);
    default:             return sd::kIdleByte;
    }
}

void SOUNDSINTH_HOT_PATH(card_in)(uint8_t mosi) {
    if (!s_c->selected || !s_present) return;

    // --- Детектор "оглох" ---
    //
    // Застрявший эмулятор ошибочных ответов не даёт, и след в лог не
    // печатается: фаза не Idle дольше 4096 байт (блок - 512 плюс токен и CRC)
    // - зависание, в след идёт запись с плохим ответом. В Idle хост вправе
    // гонять 0xFF сколько угодно.
    if (s_c->tx != s_c->tx_prev) {
        s_c->tx_prev = s_c->tx;
        s_c->phase_bytes = 0;
        s_c->stuck_reported = false;
    } else if (s_c->tx != Tx::Idle) {
        if (++s_c->phase_bytes > kPhaseStuckBytes && !s_c->stuck_reported) {
            s_c->stuck_reported = true;
            ++s_stuck_count;
            // Запись с плохим ответом: след печатается в лог тем же путём, что
            // при настоящей ошибке.
            trace_push(kTraceStuckCmd, static_cast<uint32_t>(s_c->tx), kTraceStuckResp, false);
        }
    }

    // --- Команда посреди многоблочного чтения ---
    //
    // CMD12 приходит, пока карта отдаёт секторы: другого способа прервать
    // поток нет, настоящая карта принимает её в любой момент. Команду от
    // данных отличает старшая пара бит: при чтении хост тактирует шину
    // единицами, у команды это 01.
    if (s_c->multi && (s_c->tx == Tx::ReadToken || s_c->tx == Tx::ReadData || s_c->tx == Tx::ReadCrc) &&
        (mosi & sd::kCmdStartMask) == sd::kCmdStartBits) {
        s_c->multi = false;
        s_c->req.store(0, std::memory_order_relaxed);
        s_c->cmd[0] = mosi;
        s_c->cmd_idx = 1;
        s_c->tx = Tx::CmdRx;
        return;
    }

    switch (s_c->tx) {
    case Tx::Idle:
        // Команда узнаётся по двум старшим битам: 01xxxxxx.
        if ((mosi & sd::kCmdStartMask) == sd::kCmdStartBits) {
            s_c->cmd[0] = mosi;
            s_c->cmd_idx = 1;
            s_c->tx = Tx::CmdRx;
        }
        break;

    case Tx::CmdRx:
        s_c->cmd[s_c->cmd_idx++] = mosi;
        if (s_c->cmd_idx >= sd::kCmdFrameBytes) {
            s_c->cmd_idx = 0;
            s_c->tx = Tx::Idle;
            s_c->after_resp = Tx::Idle;
            process_cmd();
        }
        break;

    case Tx::Resp:
        if (++s_c->resp_idx >= s_c->resp_len) {
            s_c->resp_idx = 0;
            s_c->tx = s_c->after_resp;
            s_c->after_resp = Tx::Idle;
        }
        break;

    case Tx::ReadToken:
        // Пока сектор не приехал, такт съедает 0xFF, состояние стоит.
        ++s_c->token_polls;
        if (s_c->block_failed[s_c->cur].load(std::memory_order_acquire) == s_c->lba) {
            s_c->token_polls = 0;
            // Носитель не отдал сектор: операция снимается, хост упрётся в
            // таймаут и переспросит.
            s_c->block_failed[s_c->cur].store(kNoFailure, std::memory_order_relaxed);
            s_c->multi = false;
            s_c->tx = Tx::Idle;
            break;
        }
        if (cur_ready()) {
            if (s_c->token_polls > s_token_polls_max_ok) s_token_polls_max_ok = s_c->token_polls;
            s_c->token_polls = 0;
            // Следующий сектор заказывается в начале блока: пока хост забирает эти
            // 512 байт, цикл успевает его прочитать. И при одиночном чтении -
            // esxDOS ходит по файлу подряд командами CMD17, следующий почти всегда
            // угадывается; промах стоит одного лишнего чтения в холостом цикле.
            if ((s_c->lba + 1u) < s_sectors) {
                request_sector(s_c->lba + 1u, static_cast<uint8_t>(1u - s_c->cur));
            }
            // CRC здесь не считается, она посчитана в цикле при чтении сектора:
            // четыре тысячи итераций в обработчике - десятки микросекунд, а Z80 на 3.5
            // МГц выдаёт INI каждые 4.5 мкс, несколько чтений остались бы
            // необслуженными.
            s_c->byte_idx = 0;
            s_c->tx = Tx::ReadData;
        }
        break;

    case Tx::ReadData:
        if (++s_c->byte_idx >= devices::storage::kSectorBytes) {
            s_c->crc_idx = 0;
            s_c->tx = Tx::ReadCrc;
        }
        break;

    case Tx::ReadCrc:
        if (++s_c->crc_idx >= 2) {
            if (s_c->multi) {
                // На второй буфер - тот, что читался, пока хост забирал этот.
                ++s_c->lba;
                s_c->cur = static_cast<uint8_t>(1u - s_c->cur);
                if (!cur_ready()) request_sector(s_c->lba, s_c->cur);
                s_c->tx = Tx::ReadToken;
            } else {
                s_c->tx = Tx::Idle;
            }
        }
        break;

    case Tx::WriteToken:
        if (mosi == sd::kTokenStartBlock) {
            s_c->byte_idx = 0;
            s_c->tx = Tx::WriteData;
        } else if (mosi == sd::kTokenStopTran) {
            s_c->multi = false;
            s_c->tx = Tx::Idle;
        }
        break;

    case Tx::WriteData:
        s_wblock[s_c->byte_idx++] = mosi;
        if (s_c->byte_idx >= devices::storage::kSectorBytes) {
            s_c->crc_idx = 0;
            s_c->tx = Tx::WriteCrc;
        }
        break;

    case Tx::WriteCrc:
        // CRC хоста принимается без проверки: в SPI он не обязателен, отказ из-за
        // него ломал бы рабочие драйверы.
        if (++s_c->crc_idx >= 2) {
            s_write_req.store(kReqValidBit | s_c->lba, std::memory_order_release);
            s_c->tx = Tx::WriteResp;
        }
        break;

    case Tx::WriteResp:
        s_c->tx = Tx::WriteBusy;
        break;

    case Tx::WriteBusy:
        // Ноль - "занята". Отпускается, когда task записал сектор.
        if (!write_pending()) {
            if (s_c->multi) {
                ++s_c->lba;
                s_c->tx = Tx::WriteToken;
            } else {
                s_c->tx = Tx::Idle;
            }
        }
        break;

    case Tx::BlockToken:
        s_c->small_idx = 0;
        s_c->tx = Tx::BlockData;
        break;

    case Tx::BlockData:
        if (++s_c->small_idx >= kSmallBlockBytes) {
            s_c->crc_idx = 0;
            s_c->tx = Tx::BlockCrc;
        }
        break;

    case Tx::BlockCrc:
        if (++s_c->crc_idx >= 2) s_c->tx = Tx::Idle;
        break;
    }
}

} // namespace

// --- Публичное ---

void sd_spi_emu_init() {
    // Свойства носителя снимаются здесь, обработчик прерывания во флеш за ними
    // не ходит.
    s_present = devices::storage::storage_present(devices::storage::Client::Host);
    s_sectors = devices::storage::storage_sector_count(devices::storage::Client::Host);
    build_csd(s_csd);
    s_csd_crc = sd::crc16(s_csd, kSmallBlockBytes);
    build_cid(s_cid);
    s_cid_crc = sd::crc16(s_cid, kSmallBlockBytes);
    // Автоматы поднимаются все: у каждой стороны своя карта, и после
    // загрузки обе обязаны быть в состоянии "только что включили".
    for (uint32_t i = 0; i < kOwnerCount; ++i) {
        Card& c = s_cards[i];
        c.who = static_cast<SdOwner>(i);
        c.small = s_csd;
        c.card_ready = false;
        c.selected = false;
        c.app_cmd = false;
        c.byte_idx = 0;
        c.crc_idx = 0;
        c.small_idx = 0;
        c.cur = 0;
        c.block_valid[0].store(false, std::memory_order_relaxed);
        c.block_valid[1].store(false, std::memory_order_relaxed);
        c.block_failed[0].store(kNoFailure, std::memory_order_relaxed);
        c.block_failed[1].store(kNoFailure, std::memory_order_relaxed);
        Card* const keep = s_c;
        s_c = &c;
        abort_transfer();
        s_c = keep;
    }
    s_write_req.store(0, std::memory_order_relaxed);
    s_write_ok = true;
}

void sd_spi_log_stats() {
    // Выбор карты - снимок в момент печати, короткий обмен его не видит;
    // рядом накопительные cmd/rd по сторонам и их прирост за период. Под
    // работающим приложением прироста divmmc быть не должно: ненулевой -
    // к карте лезут мимо ожиданий.
    static uint32_t s_last_cmds[kOwnerCount] = {};
    const auto zc = static_cast<uint32_t>(SdOwner::ZController);
    const auto dm = static_cast<uint32_t>(SdOwner::DivMmc);
    const uint32_t d_zc = s_cmds[zc] - s_last_cmds[zc];
    const uint32_t d_dm = s_cmds[dm] - s_last_cmds[dm];
    s_last_cmds[zc] = s_cmds[zc];
    s_last_cmds[dm] = s_cmds[dm];
    // 320: строка с кириллицей (два байта на букву) не влезает в 192.
    char sm[320];
    std::snprintf(sm, sizeof(sm),
                  "sd: выбраны zc=%u dm=%u writer=%s write_denied=%" PRIu32 " stuck=%" PRIu32
                  " | zctrl cmd=%" PRIu32 " (+%" PRIu32 ") rd=%" PRIu32 " last=%u | divmmc cmd=%" PRIu32 " (+%" PRIu32
                  ") rd=%" PRIu32 " last=%u\n",
                  s_cards[zc].selected ? 1u : 0u, s_cards[dm].selected ? 1u : 0u, owner_name(s_write_owner),
                  s_write_denied, s_stuck_count, s_cmds[zc], d_zc, s_reads[zc], s_last_cmd[zc], s_cmds[dm], d_dm,
                  s_reads[dm], s_last_cmd[dm]);
    platform::debug_log(sm);
    std::snprintf(sm, sizeof(sm), "sd: заказ сектора ждал цикл макс %" PRIu32 " мкс за период, дольше 5 мс %" PRIu32
                  " с запуска\n", s_req_wait_max_us, s_req_wait_slow);
    platform::debug_log(sm);
    std::snprintf(sm, sizeof(sm), "sd: чтение брошено хостом %" PRIu32 " (последнее: опросов %" PRIu32 ", %" PRIu32
                  " мкс от заказа), дождался токена макс после %" PRIu32 " опросов\n",
                  s_read_abandoned, s_abandon_polls, s_abandon_us, s_token_polls_max_ok);
    platform::debug_log(sm);
    s_req_wait_max_us = 0;
}

void sd_spi_log_trace_if_new() {
    static uint32_t s_last_bad = 0;
    const uint32_t bad = s_trace_bad;
    if (bad == s_last_bad) return;
    s_last_bad = bad;
    char t[96];
    std::snprintf(t, sizeof(t), "sd TRACE (ошибок всего %" PRIu32 "), последние команды:\n", bad);
    platform::debug_log(t);
    // resp - первый байт ответа R1: всё, кроме 0x00 и 0x01, драйвер покажет как
    // ошибку чтения.
    const uint32_t total = s_trace_head;
    for (uint32_t back = 12; back-- > 0;) {
        if (back >= kTraceLen || back >= total) continue;
        const TraceEntry& e = s_trace[(total - 1u - back) % kTraceLen];
        const uint32_t at_ms = static_cast<uint32_t>((static_cast<uint64_t>(e.at_kus) * 1024u) / 1000u);
        std::snprintf(t, sizeof(t), "  %" PRIu32 " ms %-7s%s%u arg=%" PRIu32 " -> %02X\n", at_ms,
                      owner_name(e.owner), e.acmd ? "ACMD" : "CMD", e.cmd, e.arg, e.resp);
        platform::debug_log(t);
    }
}

// Свой автомат по стороне. Индекс проверен: чужое значение сюда прийти
// не может, но лишняя ветка дешевле порчи памяти.
SOUNDSINTH_ALWAYS_INLINE Card* card_of(SdOwner who) {
    const uint32_t i = static_cast<uint32_t>(who);
    return (i < kOwnerCount) ? &s_cards[i] : &s_cards[0];
}

void SOUNDSINTH_HOT_PATH(sd_spi_select)(SdOwner who, bool select) {
    // Арбитра шины нет: у каждой стороны свой кристалл. Держит одна
    // выбранной сколько угодно - второй это не мешает ничем.
    s_c = card_of(who);
    // Снятие выбора обрывает операцию, как у настоящей карты: так драйвер
    // выходит из рассинхронизации. Выбранная снова, карта занята, пока
    // принятый блок не записан.
    if (s_c->selected && !select) abort_transfer();
    if (!s_c->selected && select && write_pending()) s_c->tx = Tx::WriteBusy;
    s_c->selected = select;
}

uint8_t SOUNDSINTH_HOT_PATH(sd_spi_byte)(SdOwner who, uint8_t mosi) {
    s_c = card_of(who);
    // Не выбрана - 0xFF, как ведущий на невыбранной карте.
    if (!s_c->selected) return sd::kIdleByte;
    const uint8_t rx = card_out();
    card_in(mosi);
    return rx;
}

void SOUNDSINTH_HOT_PATH(sd_spi_task)() {
    // Единственное место, где трогается носитель.
    //
    // Запись - первой: заказ чтения, найденный после неё, обязан увидеть уже
    // записанный сектор. Номер сектора - из заказа, а не из s_c->lba: ту меняет
    // следующая же команда хоста.
    const uint32_t wr = s_write_req.load(std::memory_order_acquire);
    if (wr != 0u) {
        const uint32_t lba = wr & kReqLbaMask;
        s_write_ok = devices::storage::storage_write(lba, s_wblock);
        // Слот со старыми данными этого сектора больше не годен. Обработчик
        // слоты сейчас не отдаёт: карта занята, команд не принимает.
        // Носитель один на всех, поэтому негодным становится слот у ЛЮБОЙ
        // стороны: вторая иначе отдала бы то, что было до записи.
        for (Card& c : s_cards) {
            for (uint8_t slot = 0; slot < 2; ++slot) {
                if (c.block_lba[slot] == lba) c.block_valid[slot].store(false, std::memory_order_relaxed);
            }
        }
        s_write_req.store(0, std::memory_order_release);
    }

    // Заказы чтения берутся, пока есть: пока читается сектор, обработчик может
    // заказать следующий. Заказ забирается обменом слова на ноль; положенный
    // после обмена возьмёт следующий виток.
    // По кругу и по сторонам: пока читается сектор, обработчик может
    // заказать следующий, и обе стороны обслуживаются поровну - носитель
    // общий, а очередь к нему разбирает этот виток.
    for (bool any = true; any;) {
        any = false;
        for (Card& c : s_cards) {
            const uint32_t req = c.req.exchange(0, std::memory_order_acquire);
            if ((req & kReqValidBit) == 0u) continue;
            any = true;
            const uint32_t waited = platform::mono_us() - c.req_posted_us;
            if (waited > s_req_wait_max_us) s_req_wait_max_us = waited;
            if (waited > kReqWaitSlowUs) ++s_req_wait_slow;
            const uint32_t lba = req & kReqLbaMask;
            const uint8_t slot = (req & kReqSlotBit) ? 1u : 0u;
            if (devices::storage::storage_read(lba, c.block[slot])) {
                // Считается здесь, в цикле, до объявления блока годным:
                // обработчику остаётся только отдавать байты.
                c.block_crc_buf[slot] = sd::crc16(c.block[slot], devices::storage::kSectorBytes);
                c.block_lba[slot] = lba;
                // Последним действием, с барьером: увидев признак,
                // обработчик видит байты, CRC и номер сектора.
                c.block_valid[slot].store(true, std::memory_order_release);
            } else {
                // Фазу снимет обработчик.
                c.block_failed[slot].store(lba, std::memory_order_release);
            }
        }
    }
}

} // namespace devices::sd
