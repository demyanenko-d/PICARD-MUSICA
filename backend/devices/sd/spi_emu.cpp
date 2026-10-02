// SPDX-License-Identifier: MIT
// Эмулятор SD-карты в SPI. Только логика карты: носитель и
// кэш секторов - за арбитром. Своё здесь - два буфера сектора:
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

static_assert(devices::storage::kSectorBytes == sd::kBlockBytes, "a medium sector is an SD data block");

// --- Состояние ---

enum class Tx : uint8_t {
    Idle,       // ждём начала команды
    CmdRx,      // добираем шесть байт команды
    Resp,       // отдаём ответ
    ReadToken,  // 0xff, пока сектор не приехал, потом 0xfe
    ReadData,   // отдаём 512 байт
    ReadCrc,    // два байта CRC16 блока
    WriteToken, // ждём 0xfe или 0xfd от хоста
    WriteData,  // принимаем 512 байт
    WriteCrc,   // принимаем CRC16 хоста
    WriteResp,  // отдаём "accepted/not accepted"
    WriteBusy,  // 0x00, пока сектор не записан
    BlockToken, // 0xfe перед CSD/CID
    BlockData,  // 16 байт CSD/CID
    BlockCrc,   // CRC16 этого блока
};

constexpr uint32_t kOwnerCount = 2;
static_assert(static_cast<uint32_t>(SdOwner::ZController) + 1u == kOwnerCount, "kOwnerCount follows the number of SdOwner values");

// Очередь тактов, пришедших в занятый автомат. Степень двойки: индекс
// берётся маской, без деления в обработчике.
constexpr uint8_t kPendLen     = 4;
constexpr uint16_t kPendSelect = 0x100; // такт - смена выбора, а не байт
constexpr uint16_t kPendOn     = 0x200; // выбрать (иначе снять)
static_assert(kOwnerCount == devices::storage::kEmulatorCount, "the medium owner number is the same SdOwner");

// Автомат карты - свой у каждого эмулятора: общий только носитель, и
// его очередь разбирает sd_spi_task.
struct Card {
    bool card_ready = false; // ACMD41 прошла; CMD0 и init снимают
    Tx tx           = Tx::Idle;
    Tx tx_prev      = Tx::Idle; // для детектора застревания (sd_spi_byte())
    Tx after_resp   = Tx::Idle; // куда уйти, когда ответ отдан
    bool selected   = false;

    bool app_cmd                    = false; // предыдущей была CMD55
    uint8_t cmd[sd::kCmdFrameBytes] = {};
    uint8_t cmd_idx                 = 0;
    uint32_t phase_bytes            = 0;
    bool stuck_reported             = false;

    uint8_t resp[5]  = {};
    uint8_t resp_len = 0;
    uint8_t resp_idx = 0;

    bool multi        = false; // идёт CMD18/CMD25
    uint32_t lba      = 0;     // сектор текущей операции
    uint16_t byte_idx = 0;     // позиция внутри блока
    uint8_t crc_idx   = 0;

    // Два буфера на сторону: при многоблочном чтении номер следующего
    // сектора известен, и его читают, пока хост забирает текущий.
    uint8_t block[2][devices::storage::kSectorBytes] = {};
    uint16_t block_crc_buf[2]                        = {0, 0};
    std::atomic<bool> block_valid[2]                 = {{false}, {false}};
    uint32_t block_lba[2]                            = {0, 0};
    // Сектор, который не прочитался, плюс единица; ноль - "отказа нет".
    // Со сторожем 0xFFFFFFFF весь массив автоматов лежал в .data, то есть
    // три с половиной килобайта нулей во флеше и столько же копирования при
    // загрузке. Смещение на единицу убирает особый случай сектора 0.
    std::atomic<uint32_t> block_failed[2] = {};
    uint8_t cur                           = 0; // из какого буфера отдаём

    // Носитель этой стороны: у хозяев он может быть разным, и ёмкость
    // тоже. CSD поэтому свой, CID один на обоих - он константа.
    bool present                    = false;
    uint32_t sectors                = 0;
    uint8_t csd[sd::kRegisterBytes] = {};
    uint16_t csd_crc                = 0;

    const uint8_t* small = nullptr; // отдаваемый регистр: CSD или CID
    uint16_t small_crc   = 0;
    uint8_t small_idx    = 0;

    std::atomic<uint32_t> req{0};
    // Заказ записи - одним словом: номер сектора и kReqValidBit. Ставит
    // обработчик после CRC блока, гасит цикл, когда блок на носителе.
    // Пока заказ стоит, занята только эта сторона: отдаёт 0x00 и
    // команд не принимает, в том числе после снятия и нового выбора.
    // Подготовленная смена носителя. Готовит поток Core1, принимает
    // обработчик на своём такте: состояние автомата меняет один контекст.
    volatile bool media_dirty           = false;
    bool new_present                    = false;
    uint32_t new_sectors                = 0;
    uint8_t new_csd[sd::kRegisterBytes] = {};
    uint16_t new_csd_crc                = 0;

    std::atomic<uint32_t> write_req{0};
    volatile bool write_ok = true;
    uint8_t* wbuf          = nullptr; // блок этой стороны в s_wblocks
    uint32_t req_posted_us = 0;
    uint32_t token_polls   = 0;

    // Что сейчас разбирается - для следа.
    uint8_t cur_cmd  = 0;
    uint32_t cur_arg = 0;
    bool cur_acmd    = false;

    // Чей это автомат: нужно следу и счётчикам по сторонам.
    SdOwner who = SdOwner::None;

    // Повторный вход в один автомат. Обработчик чтения порта (0x60)
    // вытесняется записью (0x40) посреди такта, и оба такта идут по
    // одному автомату: без очереди второй съедает фазу первого.
    //
    // Маскировать нельзя - прерывания на пути шины не маскируем.
    // Вытеснивший кладёт свой такт сюда и уходит; разбирает очередь тот,
    // кто такт начал. Проверка и взвод признака в одном контексте, а
    // вытесняет его только более приоритетный, который сам очередью и
    // пользуется, - рукопожатие замкнуто.
    //
    // Четыре записи: глубже одного вытеснения вложения не бывает, две на
    // запас. Переполнение считается и видно в строке sd:.
    volatile bool step_busy    = false;
    uint16_t pend[kPendLen]    = {};
    volatile uint8_t pend_head = 0;
    volatile uint8_t pend_tail = 0;
    // Чем отвечать вытеснившему. Умолчание нулевое, а не 0xFF: ненулевое
    // утащило бы весь массив автоматов из .bss в .data, то есть во флеш.
    uint8_t last_rx = 0;
};

Card s_cards[kOwnerCount];

// Писатель один: первая сторона, выдавшая CMD24/CMD25, пишет до перезагрузки.
// У DivMMC и Z-Controller свои картины FAT, запись второй портит FAT первой, а
// бит защиты от записи софт почти не смотрит. Поэтому чужой записи - R1 0x40
// (parameter error): драйвер в фазу данных не идёт. Чтение не запрещено.
SdOwner s_write_owner   = SdOwner::None;
uint32_t s_write_denied = 0; // сколько чужих записей отклонено

// Накопительные счётчики обмена по владельцу.
//
// Строка "owner=..." - снимок раз в 16 секунд, короткий обмен она не
// видит: между печатями карта может быть выбрана и отпущена сотню раз.
// Счётчики отвечают, разговаривает ли сторона с картой вообще: приложение
// жалуется на ошибку чтения, а cmd его стороны не растёт - команды до нас
// не доходят, искать на шине.
uint32_t s_cmds[kOwnerCount]  = {}; // команд принято, по SdOwner
uint32_t s_reads[kOwnerCount] = {}; // из них чтений сектора
// Носитель не отдал заказанный сектор. Ветка отказа молча снимает фазу, и
// хост упирается в таймаут: без счётчика "esxDOS встал" не отличить от
// "карта сыплется" (там растут счётчики строки "sd: носитель:").
uint32_t s_media_fail[kOwnerCount] = {};
uint8_t s_last_cmd[kOwnerCount]    = {}; // последняя команда каждой стороны

// --- След команд карты ---
//
// Кольцо последних команд (kTraceLen), пишется всегда: отказ случайный,
// момент заранее не известен.
//
// Пишется из прерывания: только простые записи в память, разбор строки
// делает основной поток.
// Печатается kTracePrint последних; кольцо чуть больше, чтобы оборот
// не съел хвост между записью и печатью. Держать полсотни записей,
// из которых читаются двенадцать, смысла нет.
constexpr uint32_t kTracePrint = 12;
constexpr uint32_t kTraceLen   = 16; // степень двойки не нужна, индекс с оборотом
static_assert(kTracePrint < kTraceLen, "more is printed than the ring remembers");
struct TraceEntry {
    uint32_t arg;    // аргумент команды (LBA у чтения и записи)
    uint32_t at_kus; // когда, в единицах по 1024 мкс (микросекунды >> 10)
    uint8_t cmd;     // номер команды
    SdOwner owner;   // кто выдал
    uint8_t resp;    // чем ответили (первый байт R1)
    uint8_t acmd;    // была ли перед ней CMD55
};
TraceEntry s_trace[kTraceLen];
// Запись детектора застревания: вместо номера команды и ответа R1.
constexpr uint8_t kTraceStuckCmd  = 0xfe;
constexpr uint8_t kTraceStuckResp = 0xee;
volatile uint32_t s_trace_head    = 0; // всего записей; индекс = head % kTraceLen
uint32_t s_trace_pos              = 0; // куда писать следующую, с оборотом сравнением
volatile uint32_t s_trace_bad     = 0; // сколько ответов было ошибочными

// Без делений: зовётся из обработчика порта на каждый ответ R1. Остаток и
// перевод в миллисекунды - в печати, на основном потоке.
void trace_push(const Card& c, uint8_t cmd, uint32_t arg, uint8_t resp, bool acmd) {
    const uint32_t i  = s_trace_pos;
    s_trace_pos       = (i + 1u < kTraceLen) ? i + 1u : 0u;
    s_trace[i].arg    = arg;
    s_trace[i].at_kus = platform::mono_us() >> 10;
    s_trace[i].cmd    = cmd;
    s_trace[i].owner  = c.who;
    s_trace[i].resp   = resp;
    s_trace[i].acmd   = acmd ? 1u : 0u;
    s_trace_head      = s_trace_head + 1u;
    // Ошибочно всё, кроме "готов" и "в простое": такой ответ драйвер
    // показывает как ошибку чтения.
    if (resp != sd::kR1Ready && resp != sd::kR1Idle) ++s_trace_bad;
}

// --- Разбор команды ---

// Сколько байт держится одна фаза обмена (детектор "оглох" в
// sd_spi_byte()). С запасом больше самой длинной законной фазы (сектор
// 512 байт плюс токен и CRC).
constexpr uint32_t kPhaseStuckBytes = 4096;
uint32_t s_stuck_count              = 0;

// Повторный вход в автомат: сколько тактов ушло в очередь и сколько не
// влезло. Второе обязано быть нулём - очередь глубже вложения.
uint32_t s_pend_used = 0;

// Защита от повторного входа в sd_spi_task: ожидание обмена с флешкой
// обслуживает шину, а то обслуживание зовёт цикл эмулятора снова.
bool s_task_entered = false;
struct TaskGuard {
    ~TaskGuard() { s_task_entered = false; }
};
uint32_t s_pend_lost = 0;

// Отказ носителя публикуется как готовность блока: поток кладёт номер
// непрочитанного сектора, фазу обмена меняет только обработчик. Писать
// s_c->tx и s_c->multi из потока нельзя: устаревший отказ оборвал бы
// свежий обмен.
constexpr uint32_t kNoFailure = 0u;

// Копии свойств носителя в SRAM: с опросом арбитра во флеше на каждом
// байте обработчик чтения доходит до 4.87 мкс при IN каждые ~4.6 мкс.
// CID общий, CSD зависит от ёмкости и лежит в Card; собираются с CRC при
// подъёме и при смене носителя, обработчику CMD9/CMD10 остаётся выбрать
// готовый блок.
constexpr uint32_t kSmallBlockBytes = sd::kRegisterBytes;
uint8_t s_cid[kSmallBlockBytes];
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
constexpr uint32_t kReqSlotLsb  = 28;
constexpr uint32_t kReqValidBit = 1u << 31;
constexpr uint32_t kReqSlotBit  = 1u << kReqSlotLsb;
constexpr uint32_t kReqLbaMask  = kReqSlotBit - 1u;
// Сколько заказ ждал, пока цикл Core1 его заберёт: это задержка, которую
// видит хост, сверх чтения самого сектора. Наибольшее - за период строки
// лога, число дольше kReqWaitSlowUs - с запуска.
uint32_t s_req_wait_max_us        = 0;
uint32_t s_req_wait_slow          = 0;
constexpr uint32_t kReqWaitSlowUs = 5000;
// Терпение хоста: байты опроса в ожидании токена чтения. Хост снял выбор,
// не дождавшись, - чтение брошено: сколько опрашивал и сколько прошло от
// заказа. Наибольший опрос, после которого токен пришёл, - для сравнения.
uint32_t s_token_polls_max_ok = 0;
uint32_t s_read_abandoned     = 0;
uint32_t s_abandon_polls      = 0;
uint32_t s_abandon_us         = 0;

// Блок записи - в своём буфере: чтение, которое цикл взял до CMD24,
// дописало бы сектор упреждения поверх блока хоста. Буфер свой у
// каждой стороны: общий значил бы, что запись одной ждёт другую.
uint8_t s_wblocks[kOwnerCount][devices::storage::kSectorBytes];

// Поколение снимка носителей, на котором сверка отработала в последний
// раз. Заводится в sd_spi_emu_init тем, что было на момент подъёма карт.
uint32_t s_media_gen_seen = 0;

SOUNDSINTH_ALWAYS_INLINE bool write_pending(const Card& c) {
    return c.write_req.load(std::memory_order_acquire) != 0u;
}

// --- Ответы ---

void SOUNDSINTH_HOT_PATH(respond)(Card& c, const uint8_t* bytes, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) {
        c.resp[i] = bytes[i];
    }
    c.resp_len = n;
    c.resp_idx = 0;
    c.tx       = Tx::Resp;
}

// Что сейчас разбирается - для следа: respond1 не знает, на какую
// команду отвечает.

void SOUNDSINTH_HOT_PATH(respond1)(Card& c, uint8_t r1) {
    trace_push(c, c.cur_cmd, c.cur_arg, r1, c.cur_acmd);
    respond(c, &r1, 1);
}

// CSD версии 2: ёмкость прямо, в единицах по 512 КБ, как у настоящего
// носителя - по ней софт узнаёт размер карты. Зовётся из sd_spi_emu_init.
void build_csd(uint8_t* out, uint32_t sectors) {
    std::memset(out, 0, kSmallBlockBytes);
    const uint32_t c_size = (sectors >= 1024u) ? (sectors / 1024u - 1u) : 0u;

    out[0]  = 0x40; // CSD_STRUCTURE = 1
    out[1]  = 0x0e; // TAAC
    out[2]  = 0x00; // NSAC
    out[3]  = 0x32; // TRAN_SPEED = 25 МГц
    out[4]  = 0x5b; // CCC
    out[5]  = 0x59; // CCC | READ_BL_LEN = 9
    out[6]  = 0x00;
    out[7]  = static_cast<uint8_t>((c_size >> 16) & 0x3f);
    out[8]  = static_cast<uint8_t>(c_size >> 8);
    out[9]  = static_cast<uint8_t>(c_size);
    out[10] = 0x7f;
    out[11] = 0x80;
    out[12] = 0x0a;
    out[13] = 0x40;
    out[14] = 0x00;
    out[15] = sd::crc7(out, 15); // настоящий CRC7
}

void build_cid(uint8_t* out) {
    std::memset(out, 0, kSmallBlockBytes);
    out[0]  = 0x00; // MID
    out[1]  = 'S';
    out[2]  = 'S'; // OID
    out[3]  = 'S';
    out[4]  = 'N';
    out[5]  = 'D';
    out[6]  = 'S';
    out[7]  = 'Y';
    out[8]  = 0x10; // PRV
    out[9]  = 0x00;
    out[10] = 0x00;
    out[11] = 0x00;
    out[12] = 0x01; // PSN
    out[13] = 0x01; // MDT
    out[14] = 0x50;
    out[15] = sd::crc7(out, 15);
}

// Отдать готовый блок CSD или CID вслед за R1.
void SOUNDSINTH_HOT_PATH(arm_small_block)(Card& c, const uint8_t* block, uint16_t crc) {
    c.small      = block;
    c.small_crc  = crc;
    c.small_idx  = 0;
    c.after_resp = Tx::BlockToken;
}

// Заказать сектор в слот. Уже лежит - заказывать не надо (выигрыш
// упреждающего чтения).
void SOUNDSINTH_HOT_PATH(request_sector)(Card& c, uint32_t lba, uint8_t slot) {
    if (c.block_valid[slot].load(std::memory_order_acquire) && c.block_lba[slot] == lba) return;
    c.block_valid[slot].store(false, std::memory_order_relaxed);
    c.block_failed[slot].store(kNoFailure, std::memory_order_relaxed);
    if (lba > kReqLbaMask) return; // за 128 ГБ не ходим
    c.req_posted_us = platform::mono_us();
    c.req.store(kReqValidBit | (slot ? kReqSlotBit : 0u) | lba, std::memory_order_release);
}

// Готовность выводится из буферов, а не хранится отдельным признаком.
bool SOUNDSINTH_HOT_PATH(cur_ready)(const Card& c) {
    return c.block_valid[c.cur].load(std::memory_order_acquire) && c.block_lba[c.cur] == c.lba;
}

void SOUNDSINTH_HOT_PATH(start_read)(Card& c, uint32_t lba, bool multi) {
    // Потолок номера в заказе - тоже отказ, как и у записи: иначе
    // request_sector молча не кладёт заказ, и хост ждёт токена до своего
    // предела. 28 разрядов - это 128 ГБ, карты такого размера уже обычны.
    if (lba >= c.sectors || lba > kReqLbaMask) {
        respond1(c, sd::kR1ParamErr); // parameter error
        return;
    }
    c.lba      = lba;
    c.multi    = multi;
    c.byte_idx = 0;

    // Попадание в заготовленный сектор: если нужный уже готов, ответ сразу,
    // 489 мкс чтения с карты исчезают. Безусловное гашение обоих буферов
    // выбрасывало прочитанное заранее, и упреждение не работало.
    if (c.block_valid[0].load(std::memory_order_acquire) && c.block_lba[0] == lba) {
        c.cur = 0;
    } else if (c.block_valid[1].load(std::memory_order_acquire) && c.block_lba[1] == lba) {
        c.cur = 1;
    } else {
        c.cur = 0;
        c.block_valid[0].store(false, std::memory_order_relaxed);
        c.block_valid[1].store(false, std::memory_order_relaxed);
        request_sector(c, lba, 0);
    }
    c.after_resp = Tx::ReadToken;
    respond1(c, sd::kR1Ready);
}

void SOUNDSINTH_HOT_PATH(process_cmd)(Card& c) {
    const uint8_t cmd  = static_cast<uint8_t>(c.cmd[0] & 0x3f);
    const uint32_t arg = (static_cast<uint32_t>(c.cmd[1]) << 24) | (static_cast<uint32_t>(c.cmd[2]) << 16) | (static_cast<uint32_t>(c.cmd[3]) << 8) |
                         static_cast<uint32_t>(c.cmd[4]);

    const bool app = c.app_cmd;
    c.app_cmd      = false;
    c.cur_cmd      = cmd;
    c.cur_arg      = arg;
    c.cur_acmd     = app;

    // Счёт до разбора: важно, что команда пришла, а не принята ли.
    {
        const uint32_t oi = static_cast<uint32_t>(c.who);
        if (oi < kOwnerCount) {
            ++s_cmds[oi];
            s_last_cmd[oi] = cmd;
            if (cmd == sd::kCmd17 || cmd == sd::kCmd18) ++s_reads[oi];
        }
    }

    if (app && cmd == sd::kAcmd41) {
        // Инициализация сразу: карта уже поднята драйвером.
        c.card_ready = true;
        respond1(c, sd::kR1Ready);
        return;
    }

    switch (cmd) {
        case sd::kCmd0:
            c.card_ready = false;
            c.multi      = false;
            respond1(c, sd::kR1Idle);
            return;

        case sd::kCmd8: {
            // R7: R1 + эхо напряжения и проверочного узора
            const uint8_t r7[5] = {sd::kR1Idle, 0x00, 0x00, sd::kIfCondVoltage, static_cast<uint8_t>(arg & 0xff)};
            respond(c, r7, 5);
            return;
        }

        case sd::kCmd9:
            arm_small_block(c, c.csd, c.csd_crc);
            respond1(c, sd::kR1Ready);
            return;

        case sd::kCmd10:
            arm_small_block(c, s_cid, s_cid_crc);
            respond1(c, sd::kR1Ready);
            return;

        case sd::kCmd12:
            // Остановка многоблочного чтения. Ответ R1b: за ним busy, но мы всегда
            // готовы и сразу отпускаем.
            c.multi = false;
            c.req.store(0, std::memory_order_relaxed);
            respond1(c, sd::kR1Ready);
            return;

        case sd::kCmd13: {
            // R2: два байта состояния. Драйверы опрашивают её после записи; запись,
            // не легшая на носитель, - общая ошибка во втором байте.
            const uint8_t r2[2] = {sd::kR1Ready, c.write_ok ? uint8_t{0} : sd::kR2Error};
            respond(c, r2, 2);
            return;
        }

        case sd::kCmd16:
            respond1(c, arg == devices::storage::kSectorBytes ? sd::kR1Ready : sd::kR1IllegalCmd);
            return;

        case sd::kCmd17:
            start_read(c, arg, false);
            return;

        case sd::kCmd18:
            start_read(c, arg, true);
            return;

        case sd::kCmd24:
        case sd::kCmd25:
            // Чужому писателю - отказ (s_write_owner).
            if (s_write_owner != SdOwner::None && s_write_owner != c.who) {
                ++s_write_denied;
                respond1(c, sd::kR1ParamErr);
                return;
            }
            if (arg >= c.sectors || arg > kReqLbaMask) {
                respond1(c, sd::kR1ParamErr);
                return;
            }
            s_write_owner = c.who;
            c.req.store(0, std::memory_order_relaxed); // упреждение уже не нужно
            c.lba        = arg;
            c.multi      = (cmd == sd::kCmd25);
            c.byte_idx   = 0;
            c.after_resp = Tx::WriteToken;
            respond1(c, sd::kR1Ready);
            return;

        case sd::kCmd55:
            c.app_cmd = true;
            respond1(c, c.card_ready ? sd::kR1Ready : sd::kR1Idle);
            return;

        case sd::kCmd58: {
            // OCR: CCS=1 (адресация секторами); "питание поднято" и R1 готов - только
            // после ACMD41, до неё idle, как у настоящей карты ("CMD58 0x01, window
            // 40FF8000").
            const uint8_t r3[5] = {c.card_ready ? sd::kR1Ready : sd::kR1Idle, static_cast<uint8_t>((c.card_ready ? sd::kOcrPowerUpBits : 0u) | sd::kOcrCcsBits),
                                   0xff, 0x80, 0x00};
            respond(c, r3, 5);
            return;
        }

        case sd::kCmd59:
            respond1(c, sd::kR1Ready); // CRC считается всегда, включать нечего
            return;

        default:
            respond1(c, sd::kR1IllegalCmd);
            return;
    }
}

// Карта бросила операцию: снятие выбора или отъём шины у ушедшего
// владельца. Как у настоящей карты: застряв в любой фазе, кроме приёма
// команды, эмулятор оглох бы - из не-Idle начало команды не распознаётся.
// Заказ на запись (write_req) не трогается: принятый блок дописывается
// на носитель, чем бы хост ни занялся.
SOUNDSINTH_ALWAYS_INLINE void abort_transfer(Card& c) {
    if (c.tx == Tx::ReadToken) {
        ++s_read_abandoned;
        s_abandon_polls = c.token_polls;
        s_abandon_us    = platform::mono_us() - c.req_posted_us;
    }
    c.token_polls = 0;
    c.cmd_idx     = 0;
    c.resp_len    = 0;
    c.resp_idx    = 0;
    c.after_resp  = Tx::Idle;
    c.multi       = false;
    c.req.store(0, std::memory_order_relaxed);
    c.tx = Tx::Idle;
}

const char* owner_name(SdOwner o) {
    return o == SdOwner::None ? "-" : (o == SdOwner::DivMmc ? "divmmc" : "zctrl");
}

// Один такт обмена: что карта отдаёт и что принимает. Ответ считается до
// продвижения состояния, иначе на границах состояний отдавался бы байт
// следующей фазы. Для zcontroller это один такт: там защёлка держит
// результат уже случившегося обмена.
uint8_t SOUNDSINTH_HOT_PATH(card_out)(const Card& c) {
    if (!c.selected || !c.present) return sd::kIdleByte;

    switch (c.tx) {
        case Tx::Resp:
            return c.resp[c.resp_idx];
        case Tx::ReadToken:
            return cur_ready(c) ? sd::kTokenStartBlock : sd::kIdleByte;
        case Tx::ReadData:
            return c.block[c.cur][c.byte_idx];
        case Tx::ReadCrc:
            return (c.crc_idx == 0) ? static_cast<uint8_t>(c.block_crc_buf[c.cur] >> 8) : static_cast<uint8_t>(c.block_crc_buf[c.cur]);
        case Tx::WriteResp:
            return sd::kDataRespAccepted; // принято
        case Tx::WriteBusy:
            return write_pending(c) ? sd::kBusyByte : sd::kIdleByte;
        case Tx::BlockToken:
            return sd::kTokenStartBlock;
        case Tx::BlockData:
            return c.small[c.small_idx];
        case Tx::BlockCrc:
            return (c.crc_idx == 0) ? static_cast<uint8_t>(c.small_crc >> 8) : static_cast<uint8_t>(c.small_crc);
        default:
            return sd::kIdleByte;
    }
}

void SOUNDSINTH_HOT_PATH(card_in)(Card& c, uint8_t mosi) {
    if (!c.selected || !c.present) return;

    // --- Детектор "оглох" ---
    //
    // Застрявший эмулятор ошибочных ответов не даёт, и след в лог не
    // печатается: фаза не Idle дольше 4096 байт (блок - 512 плюс токен и CRC)
    // - зависание, в след идёт запись с плохим ответом. В Idle хост вправе
    // гонять 0xFF сколько угодно.
    if (c.tx != c.tx_prev) {
        c.tx_prev        = c.tx;
        c.phase_bytes    = 0;
        c.stuck_reported = false;
    } else if (c.tx != Tx::Idle) {
        if (++c.phase_bytes > kPhaseStuckBytes && !c.stuck_reported) {
            c.stuck_reported = true;
            ++s_stuck_count;
            // Запись с плохим ответом: след печатается в лог тем же путём, что
            // при настоящей ошибке.
            trace_push(c, kTraceStuckCmd, static_cast<uint32_t>(c.tx), kTraceStuckResp, false);
        }
    }

    // --- Команда посреди многоблочного чтения ---
    //
    // CMD12 приходит, пока карта отдаёт секторы: другого способа прервать
    // поток нет, настоящая карта принимает её в любой момент. Команду от
    // данных отличает старшая пара бит: при чтении хост тактирует шину
    // единицами, у команды это 01.
    if (c.multi && (c.tx == Tx::ReadToken || c.tx == Tx::ReadData || c.tx == Tx::ReadCrc) && (mosi & sd::kCmdStartMask) == sd::kCmdStartBits) {
        c.multi = false;
        c.req.store(0, std::memory_order_relaxed);
        c.cmd[0]  = mosi;
        c.cmd_idx = 1;
        c.tx      = Tx::CmdRx;
        return;
    }

    switch (c.tx) {
        case Tx::Idle:
            // Команда узнаётся по двум старшим битам: 01xxxxxx.
            if ((mosi & sd::kCmdStartMask) == sd::kCmdStartBits) {
                c.cmd[0]  = mosi;
                c.cmd_idx = 1;
                c.tx      = Tx::CmdRx;
            }
            break;

        case Tx::CmdRx:
            c.cmd[c.cmd_idx++] = mosi;
            if (c.cmd_idx >= sd::kCmdFrameBytes) {
                c.cmd_idx    = 0;
                c.tx         = Tx::Idle;
                c.after_resp = Tx::Idle;
                process_cmd(c);
            }
            break;

        case Tx::Resp:
            if (++c.resp_idx >= c.resp_len) {
                c.resp_idx   = 0;
                c.tx         = c.after_resp;
                c.after_resp = Tx::Idle;
            }
            break;

        case Tx::ReadToken:
            // Пока сектор не приехал, такт съедает 0xFF, состояние стоит.
            ++c.token_polls;
            if (c.block_failed[c.cur].load(std::memory_order_acquire) == c.lba + 1u) {
                c.token_polls = 0;
                // Носитель не отдал сектор: операция снимается, хост упрётся в
                // таймаут и переспросит.
                c.block_failed[c.cur].store(kNoFailure, std::memory_order_relaxed);
                c.multi = false;
                c.tx    = Tx::Idle;
                break;
            }
            if (cur_ready(c)) {
                if (c.token_polls > s_token_polls_max_ok) s_token_polls_max_ok = c.token_polls;
                c.token_polls = 0;
                // Следующий сектор заказывается в начале блока: пока хост забирает эти
                // 512 байт, цикл успевает его прочитать. И при одиночном чтении -
                // esxDOS ходит по файлу подряд командами CMD17, следующий почти всегда
                // угадывается; промах стоит одного лишнего чтения в холостом цикле.
                if ((c.lba + 1u) < c.sectors) {
                    request_sector(c, c.lba + 1u, static_cast<uint8_t>(1u - c.cur));
                }
                // CRC здесь не считается, она посчитана в цикле при чтении сектора:
                // четыре тысячи итераций в обработчике - десятки микросекунд, а Z80 на 3.5
                // МГц выдаёт INI каждые 4.5 мкс, несколько чтений остались бы
                // необслуженными.
                c.byte_idx = 0;
                c.tx       = Tx::ReadData;
            }
            break;

        case Tx::ReadData:
            if (++c.byte_idx >= devices::storage::kSectorBytes) {
                c.crc_idx = 0;
                c.tx      = Tx::ReadCrc;
            }
            break;

        case Tx::ReadCrc:
            if (++c.crc_idx >= 2) {
                if (c.multi) {
                    // На второй буфер - тот, что читался, пока хост забирал этот.
                    ++c.lba;
                    c.cur = static_cast<uint8_t>(1u - c.cur);
                    if (!cur_ready(c)) request_sector(c, c.lba, c.cur);
                    c.tx = Tx::ReadToken;
                } else {
                    c.tx = Tx::Idle;
                }
            }
            break;

        case Tx::WriteToken:
            if (mosi == sd::kTokenStartBlock) {
                c.byte_idx = 0;
                c.tx       = Tx::WriteData;
            } else if (mosi == sd::kTokenStopTran) {
                c.multi = false;
                c.tx    = Tx::Idle;
            }
            break;

        case Tx::WriteData:
            if (c.wbuf != nullptr) c.wbuf[c.byte_idx] = mosi;
            ++c.byte_idx;
            if (c.byte_idx >= devices::storage::kSectorBytes) {
                c.crc_idx = 0;
                c.tx      = Tx::WriteCrc;
            }
            break;

        case Tx::WriteCrc:
            // CRC хоста принимается без проверки: в SPI он не обязателен, отказ из-за
            // него ломал бы рабочие драйверы.
            if (++c.crc_idx >= 2) {
                c.write_req.store(kReqValidBit | c.lba, std::memory_order_release);
                c.tx = Tx::WriteResp;
            }
            break;

        case Tx::WriteResp:
            c.tx = Tx::WriteBusy;
            break;

        case Tx::WriteBusy:
            // Ноль - "занята". Отпускается, когда task записал сектор.
            if (!write_pending(c)) {
                if (c.multi) {
                    ++c.lba;
                    c.tx = Tx::WriteToken;
                } else {
                    c.tx = Tx::Idle;
                }
            }
            break;

        case Tx::BlockToken:
            c.small_idx = 0;
            c.tx        = Tx::BlockData;
            break;

        case Tx::BlockData:
            if (++c.small_idx >= kSmallBlockBytes) {
                c.crc_idx = 0;
                c.tx      = Tx::BlockCrc;
            }
            break;

        case Tx::BlockCrc:
            if (++c.crc_idx >= 2) c.tx = Tx::Idle;
            break;
    }
}

} // namespace

// --- Публичное ---

void sd_spi_emu_init() {
    build_cid(s_cid);
    s_cid_crc = sd::crc16(s_cid, kSmallBlockBytes);
    // Автоматы поднимаются все: у каждой стороны своя карта, и после
    // загрузки обе обязаны быть в состоянии "только что включили".
    for (uint32_t i = 0; i < kOwnerCount; ++i) {
        Card& c = s_cards[i];
        c.who   = static_cast<SdOwner>(i);
        // Свойства носителя снимаются здесь, обработчик прерывания во флеш
        // за ними не ходит.
        c.present = devices::storage::storage_emulator_present(static_cast<uint8_t>(i));
        c.sectors = devices::storage::storage_emulator_sectors(static_cast<uint8_t>(i));
        build_csd(c.csd, c.sectors);
        c.csd_crc    = sd::crc16(c.csd, kSmallBlockBytes);
        c.small      = c.csd;
        c.card_ready = false;
        c.selected   = false;
        c.app_cmd    = false;
        c.byte_idx   = 0;
        c.crc_idx    = 0;
        c.small_idx  = 0;
        c.cur        = 0;
        c.block_valid[0].store(false, std::memory_order_relaxed);
        c.block_valid[1].store(false, std::memory_order_relaxed);
        c.block_failed[0].store(kNoFailure, std::memory_order_relaxed);
        c.block_failed[1].store(kNoFailure, std::memory_order_relaxed);
        c.last_rx   = sd::kIdleByte;
        c.pend_head = 0;
        c.pend_tail = 0;
        c.step_busy = false;
        c.write_req.store(0, std::memory_order_relaxed);
        c.write_ok    = true;
        c.media_dirty = false;
        c.wbuf        = s_wblocks[i];
        abort_transfer(c);
    }
    // Состояние только что снято прямо из снимка: сверке на этом поколении
    // делать нечего.
    s_media_gen_seen = devices::storage::storage_media_generation();
}

uint32_t sd_spi_media_failures(SdOwner who) {
    const auto i = static_cast<uint32_t>(who);
    return i < kOwnerCount ? s_media_fail[i] : 0u;
}

void sd_spi_log_stats() {
    // Выбор карты - снимок в момент печати, короткий обмен его не видит;
    // рядом накопительные cmd/rd по сторонам и их прирост за период. Под
    // работающим приложением прироста divmmc быть не должно: ненулевой -
    // к карте лезут мимо ожиданий.
    static uint32_t s_last_cmds[kOwnerCount] = {};
    const auto zc                            = static_cast<uint32_t>(SdOwner::ZController);
    const auto dm                            = static_cast<uint32_t>(SdOwner::DivMmc);
    const uint32_t d_zc                      = s_cmds[zc] - s_last_cmds[zc];
    const uint32_t d_dm                      = s_cmds[dm] - s_last_cmds[dm];
    s_last_cmds[zc]                          = s_cmds[zc];
    s_last_cmds[dm]                          = s_cmds[dm];
    // 320: строка выбора в худшем случае 264 байта, в буфер debug_logf на 192
    // не влезает.
    char sm[320];
    std::snprintf(
        sm, sizeof(sm),
        "sd: selected zc=%u dm=%u writer=%s write_denied=%" PRIu32 " stuck=%" PRIu32 " pend=%" PRIu32 "/%" PRIu32 " | zctrl cmd=%" PRIu32 " (+%" PRIu32
        ") rd=%" PRIu32 " nomedia=%" PRIu32 " last=%u | divmmc cmd=%" PRIu32 " (+%" PRIu32 ") rd=%" PRIu32 " nomedia=%" PRIu32 " last=%u\n",
        s_cards[zc].selected ? 1u : 0u, s_cards[dm].selected ? 1u : 0u, owner_name(s_write_owner), s_write_denied, s_stuck_count, s_pend_used, s_pend_lost,
        s_cmds[zc], d_zc, s_reads[zc], s_media_fail[zc], s_last_cmd[zc], s_cmds[dm], d_dm, s_reads[dm], s_media_fail[dm], s_last_cmd[dm]);
    debug_log(sm);
    std::snprintf(sm, sizeof(sm), "sd: sector request waited for the loop max %" PRIu32 " us this period, over 5 ms %" PRIu32 " since boot\n",
                  s_req_wait_max_us, s_req_wait_slow);
    debug_log(sm);
    std::snprintf(sm, sizeof(sm),
                  "sd: reads abandoned by host %" PRIu32 " (last: polls %" PRIu32 ", %" PRIu32 " us since request), token awaited max %" PRIu32 " polls\n",
                  s_read_abandoned, s_abandon_polls, s_abandon_us, s_token_polls_max_ok);
    debug_log(sm);
    s_req_wait_max_us = 0;
}

void sd_spi_log_trace_if_new() {
    static uint32_t s_last_bad = 0;
    const uint32_t bad         = s_trace_bad;
    if (bad == s_last_bad) return;
    s_last_bad = bad;
    char t[96];
    std::snprintf(t, sizeof(t), "sd TRACE (%" PRIu32 " errors total), last commands:\n", bad);
    debug_log(t);
    // resp - первый байт ответа R1: всё, кроме 0x00 и 0x01, драйвер покажет как
    // ошибку чтения.
    const uint32_t total = s_trace_head;
    for (uint32_t back = kTracePrint; back-- > 0;) {
        if (back >= kTraceLen || back >= total) continue;
        const TraceEntry& e  = s_trace[(total - 1u - back) % kTraceLen];
        const uint32_t at_ms = static_cast<uint32_t>((static_cast<uint64_t>(e.at_kus) * 1024u) / 1000u);
        std::snprintf(t, sizeof(t), "  %" PRIu32 " ms %-7s%s%u arg=%" PRIu32 " -> %02X\n", at_ms, owner_name(e.owner), e.acmd ? "ACMD" : "CMD", e.cmd, e.arg,
                      e.resp);
        debug_log(t);
    }
}

// Свой автомат по стороне. Автомата нет у None и у любого значения вне
// диапазона - тогда нулевой указатель, а не чужой автомат: подмена
// отдала бы обмен соседней стороне и выглядела бы как порча карты.
SOUNDSINTH_ALWAYS_INLINE Card* card_of(SdOwner who) {
    const uint32_t i = static_cast<uint32_t>(who);
    return (i < kOwnerCount) ? &s_cards[i] : nullptr;
}

// Положить такт в очередь автомата. Не влезло - такт потерян, и это
// видно счётчиком: молчаливая потеря выглядела бы как порча обмена.
SOUNDSINTH_ALWAYS_INLINE void pend_push(Card& c, uint16_t entry) {
    const uint8_t head = c.pend_head;
    const uint8_t next = static_cast<uint8_t>((head + 1u) & (kPendLen - 1u));
    if (next == c.pend_tail) {
        ++s_pend_lost;
        return;
    }
    c.pend[head] = entry;
    c.pend_head  = next;
}

void SOUNDSINTH_HOT_PATH(select_step)(Card& c, bool select) {
    // Снятие выбора обрывает операцию, как у настоящей карты: так драйвер
    // выходит из рассинхронизации. Выбранная снова, карта занята, пока
    // принятый блок не записан.
    if (c.selected && !select) abort_transfer(c);
    if (!c.selected && select && write_pending(c)) c.tx = Tx::WriteBusy;
    c.selected = select;
}

// Разобрать очередь до пустоты и снять признак занятости. Повтор цикла -
// на случай такта, пришедшего между последней проверкой и снятием.
// Принять подготовленную смену носителя. Всё готовое - копирование
// шестнадцати байт и двух слов: счёт CSD и CRC остаётся в потоке, во флеш
// за ними обработчик не ходит.
void SOUNDSINTH_HOT_PATH(take_media)(Card& c) {
    c.present = c.new_present;
    c.sectors = c.new_sectors;
    for (uint8_t i = 0; i < sd::kRegisterBytes; ++i) {
        c.csd[i] = c.new_csd[i];
    }
    c.csd_crc = c.new_csd_crc;
    c.small   = c.csd;
    // Носитель сменился - карта как только что вставленная: хост обязан
    // пройти CMD0 и ACMD41 заново, иначе он считает готовой ту, которой уже нет.
    c.card_ready  = false;
    c.media_dirty = false;
    abort_transfer(c);
}

void SOUNDSINTH_HOT_PATH(drain_pending)(Card& c) {
    do {
        while (c.pend_tail != c.pend_head) {
            if (c.media_dirty) take_media(c);
            const uint16_t e = c.pend[c.pend_tail];
            c.pend_tail      = static_cast<uint8_t>((c.pend_tail + 1u) & (kPendLen - 1u));
            if (e & kPendSelect) {
                select_step(c, (e & kPendOn) != 0u);
            } else if (c.selected) {
                c.last_rx = card_out(c);
                card_in(c, static_cast<uint8_t>(e & 0xffu));
            }
        }
        c.step_busy = false;
        if (c.pend_tail == c.pend_head) return;
        c.step_busy = true;
    } while (true);
}

// Свой такт тоже идёт через очередь. Так код такта остаётся в одном
// месте: вторая копия в горячем пути стоила бы полутора килобайт SRAM,
// потому что обе живут в памяти, а не во флеше.
void SOUNDSINTH_HOT_PATH(sd_spi_select)(SdOwner who, bool select) {
    // Арбитра шины нет: у каждой стороны свой кристалл. Держит одна
    // выбранной сколько угодно - второй это не мешает ничем.
    Card* const pc = card_of(who);
    if (pc == nullptr) return;
    Card& c = *pc;
    pend_push(c, static_cast<uint16_t>(kPendSelect | (select ? kPendOn : 0u)));
    if (c.step_busy) {
        ++s_pend_used;
        return;
    }
    c.step_busy = true;
    drain_pending(c);
}

uint8_t SOUNDSINTH_HOT_PATH(sd_spi_byte)(SdOwner who, uint8_t mosi) {
    Card* const pc = card_of(who);
    // Автомата у стороны нет - 0xFF, как на пустой шине.
    if (pc == nullptr) return sd::kIdleByte;
    Card& c = *pc;
    // Не выбрана - 0xFF, как ведущий на невыбранной карте.
    if (!c.selected) return sd::kIdleByte;

    // Быстрый путь потоковых фаз: байт из буфера, инкремент индекса - и всё.
    //
    // На сектор это 511 тактов из 512, а при быстром носителе сто тысяч
    // тактов в секунду (замер платы). Цена общего пути - 2 мкс, и она
    // складывается с обработчиком соседнего порта: два подряд не влезают
    // в три микросекунды простого цикла IN, и хост читает прошлый байт.
    //
    // Последний байт фазы идёт общим путём: там смена фазы, заказ
    // следующего сектора и прочие решения. Счётчик байтов фазы тут не
    // двигается: детектор "оглох" срабатывает после 4096 байт одной
    // фазы, а здесь их заведомо не больше 512.
    if (!c.step_busy && !c.media_dirty && c.pend_tail == c.pend_head && c.present && c.byte_idx + 1u < devices::storage::kSectorBytes) {
        // CMD12 приходит посреди потока и отличается от данных старшей парой
        // бит: её быстрый путь обязан пропустить к общему, иначе она уйдёт в буфер.
        if (c.tx == Tx::ReadData && !(c.multi && (mosi & sd::kCmdStartMask) == sd::kCmdStartBits)) {
            const uint8_t rx = c.block[c.cur][c.byte_idx];
            ++c.byte_idx;
            c.last_rx = rx;
            return rx;
        }
        if (c.tx == Tx::WriteData && c.wbuf != nullptr) {
            c.wbuf[c.byte_idx] = mosi;
            ++c.byte_idx;
            c.last_rx = sd::kIdleByte;
            return sd::kIdleByte;
        }
    }
    pend_push(c, mosi);
    // Такт пришёл в занятый автомат: он ждёт в очереди, а вытеснивший
    // получает прежнее содержимое защёлки. У Z-Controller защёлка и так
    // конвейерная, у DivMMC байт доедет следующим обменом.
    if (c.step_busy) {
        ++s_pend_used;
        return c.last_rx;
    }
    c.step_busy = true;
    drain_pending(c);
    // После разбора очереди отдаётся защёлка, а не свой байт: вытеснивший
    // такт уже случился, и его ответ - последний. Иначе он пропал бы, а
    // хост получил бы байт предыдущего обмена дважды.
    return c.last_rx;
}

// Сама сверка - во флеше: заходят сюда считанные разы за работу, только
// когда носитель и правда сменился. В SRAM живёт лишь проверка поколения
// ниже.
namespace {
SOUNDSINTH_NOINLINE void refresh_media_changed(uint32_t gen) {
    // Сторона, чья прошлая смена ещё не принята обработчиком, пропускается
    // ниже - тогда поколение не запоминается, и следующий виток зайдёт
    // сюда снова. Иначе её новое состояние не дошло бы никогда.
    bool deferred = false;
    for (uint32_t i = 0; i < kOwnerCount; ++i) {
        Card& c                = s_cards[i];
        const bool present     = devices::storage::storage_emulator_present(static_cast<uint8_t>(i));
        const uint32_t sectors = devices::storage::storage_emulator_sectors(static_cast<uint8_t>(i));
        if (present == c.present && sectors == c.sectors) continue;
        if (c.media_dirty) { // прошлая смена ещё не принята
            deferred = true;
            continue;
        }

        // Поток только готовит смену, а применяет её обработчик на своём
        // такте. Проверить "не выбрана" и переписать CSD нельзя: между
        // проверкой и записью хост опускает CS и шлёт CMD9, и ему уходит
        // половина старого блока с несходящимся CRC.
        c.new_present = present;
        c.new_sectors = sectors;
        build_csd(c.new_csd, sectors);
        c.new_csd_crc = sd::crc16(c.new_csd, kSmallBlockBytes);
        c.media_dirty = true;
    }
    if (!deferred) s_media_gen_seen = gen;
}
} // namespace

// Зовут с каждого витка цикла Core1, а носитель меняется считанные разы за
// работу: одно слово поколения из SRAM, и почти всегда возврат. Раньше тут
// на каждом витке опрашивались драйверы карты и USB, и это был уход во
// флеш по пяти участкам.
void SOUNDSINTH_HOT_PATH(sd_spi_emu_refresh_media)() {
    const uint32_t gen = devices::storage::storage_media_generation();
    if (gen == s_media_gen_seen) return;
    refresh_media_changed(gen);
}

void SOUNDSINTH_HOT_PATH(sd_spi_task)() {
    // Повторный вход закрыт (s_task_entered): обращение к носителю ждёт, а ожидание
    // обслуживает шину и снова зовёт эту функцию. Один контекст (цикл
    // Core1), поэтому простой признак, а не атомик.
    if (s_task_entered) return;
    s_task_entered = true;
    const TaskGuard guard;

    // Единственное место, где читается и пишется сектор.
    //
    // Свойства носителя сверяются здесь же: флешка встаёт позже подъёма
    // эмулятора, карту вынимают на ходу, а эмулятор с пустым носителем
    // молча отбрасывает байты хоста. Сверка - два чтения готовых признаков,
    // работа только при изменении.
    sd_spi_emu_refresh_media();
    //
    // Запись - первой: заказ чтения, найденный после неё, обязан увидеть уже
    // записанный сектор. Номер сектора - из заказа, а не из lba автомата: её меняет
    // следующая же команда хоста.
    for (uint32_t w = 0; w < kOwnerCount; ++w) {
        Card& wc          = s_cards[w];
        const uint32_t wr = wc.write_req.load(std::memory_order_acquire);
        if (wr == 0u || wc.wbuf == nullptr) continue;
        const uint32_t lba = wr & kReqLbaMask;
        wc.write_ok        = devices::storage::storage_write(static_cast<uint8_t>(w), lba, wc.wbuf);
        // Слот со старыми данными этого сектора больше не годен. Занята
        // только писавшая сторона, вторая в этот момент вправе ждать токен
        // на тот же сектор - ей заказ переставляется заново, иначе она
        // ждала бы готовности, которой уже никто не даст.
        //
        // Носитель у сторон бывает разный: гасится слот только там, где
        // он тот же, иначе чужое чтение теряло бы упреждение зря.
        const devices::hal::Medium wm = devices::storage::storage_emulator_medium(static_cast<uint8_t>(w));
        for (uint32_t i = 0; i < kOwnerCount; ++i) {
            Card& c = s_cards[i];
            if (i != w && devices::storage::storage_emulator_medium(static_cast<uint8_t>(i)) != wm) continue;
            for (uint8_t slot = 0; slot < 2; ++slot) {
                if (c.block_lba[slot] != lba) continue;
                c.block_valid[slot].store(false, std::memory_order_relaxed);
                if (c.tx == Tx::ReadToken && c.lba == lba && c.cur == slot) {
                    c.req.store(kReqValidBit | (slot ? kReqSlotBit : 0u) | lba, std::memory_order_release);
                }
            }
        }
        wc.write_req.store(0, std::memory_order_release);
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
            any                   = true;
            const uint32_t waited = platform::mono_us() - c.req_posted_us;
            if (waited > s_req_wait_max_us) s_req_wait_max_us = waited;
            if (waited > kReqWaitSlowUs) ++s_req_wait_slow;
            const uint32_t lba = req & kReqLbaMask;
            const uint8_t slot = (req & kReqSlotBit) ? 1u : 0u;
            if (devices::storage::storage_emulator_read(static_cast<uint8_t>(&c - s_cards), lba, c.block[slot])) {
                // Считается здесь, в цикле, до объявления блока годным:
                // обработчику остаётся только отдавать байты.
                c.block_crc_buf[slot] = sd::crc16(c.block[slot], devices::storage::kSectorBytes);
                c.block_lba[slot]     = lba;
                // Последним действием, с барьером: увидев признак,
                // обработчик видит байты, CRC и номер сектора.
                c.block_valid[slot].store(true, std::memory_order_release);
            } else {
                // Фазу снимет обработчик.
                c.block_failed[slot].store(lba + 1u, std::memory_order_release);
                ++s_media_fail[static_cast<uint32_t>(&c - s_cards)];
            }
        }
    }

    // Свободное время: заказов больше нет, и можно подтянуть следующий
    // блок. Здесь, а не из цикла, потому что защита от повторного входа
    // выше не даёт начать обмен с носителем, пока идёт этот. Начни
    // предвыборку снаружи - вложенное чтение получило бы отказ занятости
    // и хост увидел бы ошибку сектора.
    devices::storage::storage_prefetch_step();
}

} // namespace devices::sd
