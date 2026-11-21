// SPDX-License-Identifier: MIT
// Эмулятор SD-карты в SPI. Только логика карты: носитель и
// кэш секторов - за арбитром. Своё здесь - два буфера сектора:
// следующий сектор читается, пока хост забирает текущий.

#include "devices/sd/spi_emu.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "devices/sd/sd_protocol.h"
#include "devices/storage/storage.h"
#include "devices/storage/storage_host.h"

namespace devices::sd {
namespace {

static_assert(storage::kSectorBytes == sd::kBlockBytes, "a medium sector is an SD data block");

// --- Состояние ---

enum class Phase : uint8_t {
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
    RegToken,   // 0xfe перед CSD/CID
    RegData,    // 16 байт CSD/CID
    RegCrc,     // CRC16 этого блока
};

// Чтение и запись сектора до инициализации карты.
constexpr uint8_t kR1NotReady = sd::kR1Idle | sd::kR1IllegalCmd;

constexpr uint32_t kCardCount = kSdOwnerCount;

// Очередь тактов, пришедших в занятый автомат. Степень двойки: индекс
// берётся маской, без деления в обработчике.
constexpr uint8_t kPendLen     = 4;
constexpr uint16_t kPendSelect = 0x100; // такт - смена выбора, а не байт
constexpr uint16_t kPendOn     = 0x200; // выбрать (иначе снять)
static_assert(kCardCount <= storage::kEmulatorCount, "the medium owner number is the same SdOwner");

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

SOUNDSINTH_ALWAYS_INLINE constexpr uint32_t make_req(uint32_t lba, uint8_t slot) {
    return kReqValidBit | (slot ? kReqSlotBit : 0u) | lba;
}
SOUNDSINTH_ALWAYS_INLINE constexpr uint32_t req_lba(uint32_t req) {
    return req & kReqLbaMask;
}
SOUNDSINTH_ALWAYS_INLINE constexpr uint8_t req_slot(uint32_t req) {
    return (req & kReqSlotBit) ? 1u : 0u;
}

// Отказ носителя публикуется как готовность блока: поток кладёт номер
// непрочитанного сектора, фазу обмена меняет только обработчик. Писать
// c.phase и c.multi из потока нельзя: устаревший отказ оборвал бы свежий
// обмен.
constexpr uint32_t kNoFailure = 0u;

// Состояние буфера сектора. Годность ставит цикл последней, с барьером:
// увидев её, обработчик видит байты, CRC и номер.
struct Slot {
    uint16_t crc = 0;
    std::atomic<bool> valid{false};
    uint32_t lba = 0;
    // Сектор, который не прочитался, плюс единица; ноль - "отказа нет":
    // умолчание нулевое, и сектор 0 не особый случай.
    std::atomic<uint32_t> failed{0};
};

// Носитель стороны: у хозяев он может быть разным, и ёмкость тоже. CSD
// поэтому свой, CID один на обоих - он константа.
struct Media {
    bool present                    = false;
    uint32_t sectors                = 0;
    uint32_t serial                 = 0; // какой носитель: другая карта той же ёмкости
    uint8_t csd[sd::kRegisterBytes] = {};
    uint16_t csd_crc                = 0;
};

// Автомат карты - свой у каждого эмулятора: общий только носитель, и
// его очередь разбирает sd_spi_task.
struct Card {
    bool card_ready  = false; // ACMD41 прошла; CMD0 и init снимают
    Phase phase      = Phase::Idle;
    Phase after_resp = Phase::Idle; // куда уйти, когда ответ отдан
    bool selected    = false;

    bool app_cmd                    = false; // предыдущей была CMD55
    uint8_t cmd[sd::kCmdFrameBytes] = {};
    uint8_t cmd_idx                 = 0;

    uint8_t resp[5]  = {};
    uint8_t resp_len = 0;
    uint8_t resp_idx = 0;

    bool multi        = false; // идёт CMD18/CMD25
    uint32_t lba      = 0;     // сектор текущей операции
    uint16_t byte_idx = 0;     // позиция внутри блока
    uint8_t crc_idx   = 0;

    // Два буфера на сторону: при многоблочном чтении номер следующего
    // сектора известен, и его читают, пока хост забирает текущий.
    // Байты - отдельным массивом: шаг 512 обработчик берёт сдвигом.
    uint8_t block[2][storage::kSectorBytes] = {};
    Slot slot[2];
    uint8_t cur = 0; // из какого буфера отдаём

    Media media;

    const uint8_t* reg = nullptr; // отдаваемый регистр: CSD или CID
    uint16_t reg_crc   = 0;
    uint8_t reg_idx    = 0;

    std::atomic<uint32_t> req{0};
    // Подготовленная смена носителя. Готовит поток Core1, принимает
    // обработчик на своём такте: состояние автомата меняет один контекст.
    volatile bool media_dirty = false;
    Media next;

    // Заказ записи - одним словом: номер сектора и kReqValidBit. Ставит
    // обработчик после CRC блока, гасит цикл, когда блок на носителе.
    // Пока заказ стоит, занята только эта сторона: отдаёт 0x00 и
    // команд не принимает, в том числе после снятия и нового выбора.
    std::atomic<uint32_t> write_req{0};
    volatile bool write_failed = false; // последняя запись не легла: CMD13 отдаёт ошибку
    bool write_reject          = false; // блок за концом носителя: ответ 0x0d

    // Что сейчас разбирается - для следа.
    uint8_t cur_cmd  = 0;
    uint32_t cur_arg = 0;
    bool cur_acmd    = false;

    // Чей это автомат: нужно следу и счётчикам по сторонам. Ставит
    // sd_spi_emu_init; умолчание нулевое, как у всех полей, - иначе массив
    // автоматов ушёл бы из .bss.
    SdOwner who{};

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
    uint8_t miso = 0;

    // Блок записи - в своём буфере: чтение, которое цикл взял до CMD24,
    // дописало бы сектор упреждения поверх блока хоста. Свой у каждой
    // стороны: общий значил бы, что запись одной ждёт другую.
    uint8_t wblock[storage::kSectorBytes] = {};
};

// Своя секция: в общей .bss модуля 2.4 КБ автоматов вставали между
// базой и малыми переменными, и обработчик доставал их длинными
// командами (step_one +176 байт).
SOUNDSINTH_BSS_SECTION("sd_cards") Card s_cards[kCardCount];

// Накопительные счётчики обмена по владельцу.
//
// Строка "owner=..." - снимок раз в 16 секунд, короткий обмен она не
// видит: между печатями карта может быть выбрана и отпущена сотню раз.
// Счётчики отвечают, разговаривает ли сторона с картой вообще: приложение
// жалуется на ошибку чтения, а cmd его стороны не растёт - команды до нас
// не доходят, искать на шине.
uint32_t s_cmds[kCardCount]  = {}; // команд принято, по SdOwner
uint32_t s_reads[kCardCount] = {}; // из них чтений сектора
// Носитель не отдал заказанный сектор. Ветка отказа молча снимает фазу, и
// хост упирается в таймаут: без счётчика "esxDOS встал" не отличить от
// "карта сыплется" (там растут счётчики строки "sd: носитель:").
uint32_t s_media_fail[kCardCount] = {};
// Носитель не принял сектор записи. Хост узнаёт это только через CMD13,
// которую esxDOS не спрашивает.
uint32_t s_write_fail[kCardCount] = {};
uint8_t s_last_cmd[kCardCount]    = {}; // последняя команда каждой стороны

// --- След команд карты ---
//
// Кольцо последних команд (kTraceLen), пишется всегда: отказ случайный,
// момент заранее не известен.
//
// Пишется из прерывания: только простые записи в память, разбор строки
// делает основной поток.
// Печатается kTracePrint последних; кольцо чуть больше, чтобы оборот
// не съел хвост между записью и печатью.
constexpr uint32_t kTracePrint = 12;
constexpr uint32_t kTraceLen   = 16; // степень двойки: место записи - её номер маской
static_assert(kTracePrint < kTraceLen, "more is printed than the ring remembers");
static_assert((kTraceLen & (kTraceLen - 1u)) == 0, "the trace slot is the entry number masked");
struct TraceEntry {
    uint32_t arg;    // аргумент команды (LBA у чтения и записи)
    uint32_t at_kus; // когда, в единицах по 1024 мкс (микросекунды >> 10)
    uint8_t cmd;     // номер команды
    SdOwner owner;   // кто выдал
    uint8_t resp;    // чем ответили (первый байт R1)
    uint8_t acmd;    // была ли перед ней CMD55
};
TraceEntry s_trace[kTraceLen];
volatile uint32_t s_trace_head = 0; // всего записей; место следующей - head маской
volatile uint32_t s_trace_bad  = 0; // сколько ответов было ошибочными

// Без делений: зовётся из обработчика порта на каждый ответ R1. Остаток и
// перевод в миллисекунды - в печати, на основном потоке.
void trace_push(const Card& c, uint8_t cmd, uint32_t arg, uint8_t resp, bool acmd) {
    const uint32_t head = s_trace_head;
    const uint32_t i    = head & (kTraceLen - 1u);
    s_trace[i].arg      = arg;
    s_trace[i].at_kus   = platform::mono_us() >> 10;
    s_trace[i].cmd      = cmd;
    s_trace[i].owner    = c.who;
    s_trace[i].resp     = resp;
    s_trace[i].acmd     = acmd ? 1u : 0u;
    s_trace_head        = head + 1u;
    // Ошибочно всё, кроме "готов" и "в простое": такой ответ драйвер
    // показывает как ошибку чтения.
    if (resp != sd::kR1Ready && resp != sd::kR1Idle) ++s_trace_bad;
}

// --- Помощники автомата ---

// Поколение снимка носителей, на котором сверка отработала в последний
// раз. Заводится в sd_spi_emu_init тем, что было на момент подъёма карт.
uint32_t s_media_gen_seen = 0;
// Сторона приняла смену носителя: отложенная из-за неё вторая смена
// перепроверяется на следующем витке. Ставит обработчик, гасит сверка.
volatile bool s_media_recheck = false;

SOUNDSINTH_ALWAYS_INLINE bool write_pending(const Card& c) {
    return c.write_req.load(std::memory_order_acquire) != 0u;
}

// Слот s держит годный сектор lba.
SOUNDSINTH_ALWAYS_INLINE bool slot_holds(const Card& c, uint8_t s, uint32_t lba) {
    return c.slot[s].valid.load(std::memory_order_acquire) && c.slot[s].lba == lba;
}

// Оба слота негодны и без отказа: подъём и смена носителя.
SOUNDSINTH_ALWAYS_INLINE void drop_slots(Card& c) {
    for (Slot& sl : c.slot) {
        sl.valid.store(false, std::memory_order_relaxed);
        sl.failed.store(kNoFailure, std::memory_order_relaxed);
    }
}

// Начало кадра команды: первый байт принят, дальше CmdRx.
SOUNDSINTH_ALWAYS_INLINE void begin_cmd(Card& c, uint8_t mosi) {
    c.cmd[0]  = mosi;
    c.cmd_idx = 1;
    c.phase   = Phase::CmdRx;
}

// Байт CRC16 блока: старший, затем младший.
SOUNDSINTH_ALWAYS_INLINE uint8_t crc_byte(uint16_t crc, uint8_t idx) {
    return static_cast<uint8_t>(idx == 0 ? crc >> 8 : crc);
}

// --- Ответы ---

void SOUNDSINTH_HOT_PATH(respond)(Card& c, const uint8_t* bytes, uint8_t n) {
    for (uint8_t i = 0; i < n; ++i) {
        c.resp[i] = bytes[i];
    }
    c.resp_len = n;
    c.resp_idx = 0;
    c.phase    = Phase::Resp;
}

void SOUNDSINTH_HOT_PATH(respond1)(Card& c, uint8_t r1) {
    trace_push(c, c.cur_cmd, c.cur_arg, r1, c.cur_acmd);
    respond(c, &r1, 1);
}

// --- Регистры CSD и CID ---

// Копии свойств носителя в SRAM: с опросом арбитра во флеше на каждом
// байте обработчик чтения доходит до 4.87 мкс при IN каждые ~4.6 мкс.
// CID общий, CSD зависит от ёмкости и лежит в Card; собираются с CRC при
// подъёме и при смене носителя, обработчику CMD9/CMD10 остаётся выбрать
// готовый блок.
uint8_t s_cid[sd::kRegisterBytes];
uint16_t s_cid_crc = 0;

// CSD версии 2: ёмкость прямо, в единицах по 512 КБ, как у настоящего
// носителя - по ней софт узнаёт размер карты. Зовётся из sd_spi_emu_init.
void build_csd(uint8_t* out, uint32_t sectors) {
    std::memset(out, 0, sd::kRegisterBytes);
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
    out[10] = 0x7f;              // ERASE_BLK_EN = 1, SECTOR_SIZE (старшие)
    out[11] = 0x80;              // SECTOR_SIZE = 0x7f, WP_GRP_SIZE = 0
    out[12] = 0x0a;              // R2W_FACTOR = 2, WRITE_BL_LEN = 9 (старшие)
    out[13] = 0x40;              // WRITE_BL_LEN (младшие)
    out[14] = 0x00;              // COPY, WP, FILE_FORMAT = 0
    out[15] = sd::crc7(out, 15); // настоящий CRC7
}

void build_cid(uint8_t* out) {
    std::memset(out, 0, sd::kRegisterBytes);
    out[0]  = 0x00; // MID
    out[1]  = 'S';  // OID, 1-2
    out[2]  = 'S';
    out[3]  = 'S'; // PNM, 3-7
    out[4]  = 'N';
    out[5]  = 'D';
    out[6]  = 'S';
    out[7]  = 'Y';
    out[8]  = 0x10; // PRV
    out[9]  = 0x00; // PSN, 9-12
    out[10] = 0x00;
    out[11] = 0x00;
    out[12] = 0x01;
    out[13] = 0x01; // MDT, 13-14
    out[14] = 0x50;
    out[15] = sd::crc7(out, 15);
}

// Свойства носителя из снимка; CSD с CRC считаются здесь, в потоке.
void fill_media(Media& m, const hal::MediaState& st) {
    m.present = st.present;
    m.sectors = st.sectors;
    m.serial  = st.serial;
    build_csd(m.csd, st.sectors);
    m.csd_crc = sd::crc16(m.csd, sd::kRegisterBytes);
}

// Отдать готовый блок CSD или CID вслед за R1.
void SOUNDSINTH_HOT_PATH(arm_register)(Card& c, const uint8_t* block, uint16_t crc) {
    c.reg        = block;
    c.reg_crc    = crc;
    c.reg_idx    = 0;
    c.after_resp = Phase::RegToken;
}

// Заказать сектор в слот. Уже лежит - заказывать не надо (выигрыш
// упреждающего чтения).
void SOUNDSINTH_HOT_PATH(request_sector)(Card& c, uint32_t lba, uint8_t slot) {
    if (slot_holds(c, slot, lba)) return;
    c.slot[slot].valid.store(false, std::memory_order_relaxed);
    c.slot[slot].failed.store(kNoFailure, std::memory_order_relaxed);
    if (lba > kReqLbaMask) return; // за 128 ГБ не ходим
    c.req.store(make_req(lba, slot), std::memory_order_release);
}

// Готовность выводится из буферов, а не хранится отдельным признаком.
bool SOUNDSINTH_HOT_PATH(cur_ready)(const Card& c) {
    return slot_holds(c, c.cur, c.lba);
}

// --- Разбор команды ---

// Сектор вне носителя или карта не поднята: ответ отказом, true. До
// инициализации - idle и недопустимая команда, как у настоящей карты (так
// драйвер узнаёт о смене носителя); за концом носителя и за 28 разрядами
// заказа (128 ГБ) - неверный параметр, иначе хост ждал бы токена до своего
// предела.
SOUNDSINTH_ALWAYS_INLINE bool reject_bad_address(Card& c, uint32_t lba) {
    if (c.card_ready && lba < c.media.sectors && lba <= kReqLbaMask) return false;
    respond1(c, c.card_ready ? sd::kR1ParamErr : kR1NotReady);
    return true;
}

void SOUNDSINTH_HOT_PATH(start_read)(Card& c, uint32_t lba, bool multi) {
    if (reject_bad_address(c, lba)) return;
    c.lba      = lba;
    c.multi    = multi;
    c.byte_idx = 0;

    // Попадание в заготовленный сектор: если нужный уже готов, ответ сразу,
    // 489 мкс чтения с карты исчезают. Буферы гасятся только при промахе:
    // иначе пропадало бы прочитанное заранее.
    if (slot_holds(c, 0, lba)) {
        c.cur = 0;
    } else if (slot_holds(c, 1, lba)) {
        c.cur = 1;
    } else {
        c.cur = 0;
        c.slot[0].valid.store(false, std::memory_order_relaxed);
        c.slot[1].valid.store(false, std::memory_order_relaxed);
        request_sector(c, lba, 0);
    }
    c.after_resp = Phase::ReadToken;
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
        if (oi < kCardCount) {
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
            arm_register(c, c.media.csd, c.media.csd_crc);
            respond1(c, sd::kR1Ready);
            return;

        case sd::kCmd10:
            arm_register(c, s_cid, s_cid_crc);
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
            const uint8_t r2[2] = {sd::kR1Ready, c.write_failed ? sd::kR2Error : uint8_t{0}};
            respond(c, r2, 2);
            return;
        }

        case sd::kCmd16:
            respond1(c, arg == storage::kSectorBytes ? sd::kR1Ready : sd::kR1IllegalCmd);
            return;

        case sd::kCmd17:
            start_read(c, arg, false);
            return;

        case sd::kCmd18:
            start_read(c, arg, true);
            return;

        case sd::kCmd24:
        case sd::kCmd25:
            // Готовность и адрес - до замка: запись до инициализации и за
            // концом носителя не делает сторону писателем. Чужому писателю
            // того же носителя - отказ R1 0x40: драйвер в фазу данных не
            // идёт.
            if (reject_bad_address(c, arg)) return;
            if (!storage::storage_claim_writer(c.who)) {
                respond1(c, sd::kR1ParamErr);
                return;
            }
            c.req.store(0, std::memory_order_relaxed); // упреждение уже не нужно
            c.lba        = arg;
            c.multi      = (cmd == sd::kCmd25);
            c.byte_idx   = 0;
            c.after_resp = Phase::WriteToken;
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
            const uint8_t r3[5] = {c.card_ready ? sd::kR1Ready : sd::kR1Idle, static_cast<uint8_t>((c.card_ready ? sd::kOcrPowerUpBit : 0u) | sd::kOcrCcsBit),
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
// владельца. Как у настоящей карты: из фазы не Idle начало команды не
// распознаётся, и застрявший эмулятор перестаёт принимать команды.
// Заказ на запись (write_req) не трогается: принятый блок дописывается
// на носитель, чем бы хост ни занялся.
SOUNDSINTH_ALWAYS_INLINE void abort_transfer(Card& c) {
    c.cmd_idx    = 0;
    c.resp_len   = 0;
    c.resp_idx   = 0;
    c.after_resp = Phase::Idle;
    c.multi      = false;
    c.req.store(0, std::memory_order_relaxed);
    c.phase = Phase::Idle;
}

const char* owner_name(SdOwner o) {
    switch (o) {
        case SdOwner::DivMmc:
            return "divmmc";
        case SdOwner::ZController:
            return "zctrl";
        case SdOwner::DivMmc2:
            return "divmmc2";
        default:
            return "-";
    }
}

// Один такт обмена: что карта отдаёт и что принимает. Ответ считается до
// продвижения состояния, иначе на границах состояний отдавался бы байт
// следующей фазы. Для zcontroller это один такт: там защёлка держит
// результат уже случившегося обмена.
uint8_t SOUNDSINTH_HOT_PATH(card_out)(const Card& c) {
    if (!c.selected || !c.media.present) return sd::kIdleByte;

    switch (c.phase) {
        case Phase::Resp:
            return c.resp[c.resp_idx];
        case Phase::ReadToken:
            return cur_ready(c) ? sd::kTokenStartBlock : sd::kIdleByte;
        case Phase::ReadData:
            return c.block[c.cur][c.byte_idx];
        case Phase::ReadCrc:
            return crc_byte(c.slot[c.cur].crc, c.crc_idx);
        case Phase::WriteResp:
            return c.write_reject ? sd::kDataRespWriteErr : sd::kDataRespAccepted;
        case Phase::WriteBusy:
            return write_pending(c) ? sd::kBusyByte : sd::kIdleByte;
        case Phase::RegToken:
            return sd::kTokenStartBlock;
        case Phase::RegData:
            return c.reg[c.reg_idx];
        case Phase::RegCrc:
            return crc_byte(c.reg_crc, c.crc_idx);
        default:
            return sd::kIdleByte;
    }
}

void SOUNDSINTH_HOT_PATH(card_in)(Card& c, uint8_t mosi) {
    if (!c.selected || !c.media.present) return;

    // --- Команда посреди многоблочного чтения ---
    //
    // CMD12 приходит, пока карта отдаёт секторы: другого способа прервать
    // поток нет, настоящая карта принимает её в любой момент. Команду от
    // данных отличает старшая пара бит: при чтении хост тактирует шину
    // единицами, у команды это 01.
    if (c.multi && (c.phase == Phase::ReadToken || c.phase == Phase::ReadData || c.phase == Phase::ReadCrc) &&
        (mosi & sd::kCmdStartMask) == sd::kCmdStartValue) {
        c.multi = false;
        c.req.store(0, std::memory_order_relaxed);
        begin_cmd(c, mosi);
        return;
    }

    switch (c.phase) {
        case Phase::Idle:
            // Команда узнаётся по двум старшим битам: 01xxxxxx.
            if ((mosi & sd::kCmdStartMask) == sd::kCmdStartValue) begin_cmd(c, mosi);
            break;

        case Phase::CmdRx:
            c.cmd[c.cmd_idx++] = mosi;
            if (c.cmd_idx >= sd::kCmdFrameBytes) {
                c.cmd_idx    = 0;
                c.phase      = Phase::Idle;
                c.after_resp = Phase::Idle;
                process_cmd(c);
            }
            break;

        case Phase::Resp:
            if (++c.resp_idx >= c.resp_len) {
                c.resp_idx   = 0;
                c.phase      = c.after_resp;
                c.after_resp = Phase::Idle;
            }
            break;

        case Phase::ReadToken:
            // Пока сектор не приехал, такт съедает 0xFF, состояние стоит.
            if (c.slot[c.cur].failed.load(std::memory_order_acquire) == c.lba + 1u) {
                // Носитель не отдал сектор: операция снимается, хост упрётся в
                // таймаут и переспросит.
                c.slot[c.cur].failed.store(kNoFailure, std::memory_order_relaxed);
                c.multi = false;
                c.phase = Phase::Idle;
                break;
            }
            if (cur_ready(c)) {
                // Следующий сектор заказывается в начале блока: пока хост забирает эти
                // 512 байт, цикл успевает его прочитать. И при одиночном чтении -
                // esxDOS ходит по файлу подряд командами CMD17, следующий почти всегда
                // угадывается; промах стоит одного лишнего чтения в холостом цикле.
                if ((c.lba + 1u) < c.media.sectors) {
                    request_sector(c, c.lba + 1u, static_cast<uint8_t>(1u - c.cur));
                }
                // CRC посчитан в цикле при чтении сектора: в обработчике это
                // около 15 мкс, а Z80 выдаёт INI каждые 4.5 мкс.
                c.byte_idx = 0;
                c.phase    = Phase::ReadData;
            }
            break;

        case Phase::ReadData:
            if (++c.byte_idx >= storage::kSectorBytes) {
                c.crc_idx = 0;
                c.phase   = Phase::ReadCrc;
            }
            break;

        case Phase::ReadCrc:
            if (++c.crc_idx >= 2) {
                if (c.multi) {
                    // На второй буфер - тот, что читался, пока хост забирал этот.
                    ++c.lba;
                    c.cur = static_cast<uint8_t>(1u - c.cur);
                    if (!cur_ready(c)) request_sector(c, c.lba, c.cur);
                    c.phase = Phase::ReadToken;
                } else {
                    c.phase = Phase::Idle;
                }
            }
            break;

        case Phase::WriteToken:
            // Токен блока у CMD24 - 0xfe, у CMD25 - 0xfc. Для CMD25 0xfe тоже
            // берётся: в этой фазе он данными быть не может.
            if (mosi == sd::kTokenStartBlock || (c.multi && mosi == sd::kTokenStartMulti)) {
                c.byte_idx = 0;
                c.phase    = Phase::WriteData;
            } else if (mosi == sd::kTokenStopTran) {
                c.multi = false;
                c.phase = Phase::Idle;
            }
            break;

        case Phase::WriteData:
            c.wblock[c.byte_idx] = mosi;
            ++c.byte_idx;
            if (c.byte_idx >= storage::kSectorBytes) {
                c.crc_idx = 0;
                c.phase   = Phase::WriteCrc;
            }
            break;

        case Phase::WriteCrc:
            // CRC хоста принимается без проверки: в SPI он не обязателен, отказ из-за
            // него ломал бы рабочие драйверы.
            if (++c.crc_idx >= 2) {
                // CMD25 дошёл до конца носителя: блок не пишется, ответ на
                // данные - ошибка записи, CMD13 отдаёт её во втором байте.
                c.write_reject = c.lba >= c.media.sectors;
                if (c.write_reject) {
                    c.write_failed = true;
                } else {
                    c.write_req.store(kReqValidBit | c.lba, std::memory_order_release);
                }
                c.phase = Phase::WriteResp;
            }
            break;

        case Phase::WriteResp:
            c.phase = Phase::WriteBusy;
            break;

        case Phase::WriteBusy:
            // Ноль - "занята". Отпускается, когда task записал сектор.
            if (!write_pending(c)) {
                if (c.multi) {
                    ++c.lba;
                    c.phase = Phase::WriteToken;
                } else {
                    c.phase = Phase::Idle;
                }
            }
            break;

        case Phase::RegToken:
            c.reg_idx = 0;
            c.phase   = Phase::RegData;
            break;

        case Phase::RegData:
            if (++c.reg_idx >= sd::kRegisterBytes) {
                c.crc_idx = 0;
                c.phase   = Phase::RegCrc;
            }
            break;

        case Phase::RegCrc:
            if (++c.crc_idx >= 2) c.phase = Phase::Idle;
            break;
    }
}

// Свой автомат по стороне. Автомата нет у None и у любого значения вне
// диапазона - тогда нулевой указатель, а не чужой автомат: подмена
// отдала бы обмен соседней стороне и выглядела бы как порча карты.
SOUNDSINTH_ALWAYS_INLINE Card* card_of(SdOwner who) {
    const uint32_t i = static_cast<uint32_t>(who);
    return (i < kCardCount) ? &s_cards[i] : nullptr;
}

// Повторный вход в автомат: сколько тактов ушло в очередь и сколько не
// влезло. Второе обязано быть нулём - очередь глубже вложения.
uint32_t s_pend_used = 0;
uint32_t s_pend_lost = 0;

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
    if (!c.selected && select && write_pending(c)) c.phase = Phase::WriteBusy;
    c.selected = select;
}

// Принять подготовленную смену носителя. Всё готовое - копирование
// 32 байт: счёт CSD и CRC остаётся в потоке, во флеш за ними обработчик
// не ходит.
void SOUNDSINTH_HOT_PATH(take_media)(Card& c) {
    c.media = c.next;
    c.reg   = c.media.csd;
    // Носитель сменился - карта как только что вставленная: хост обязан
    // пройти CMD0 и ACMD41 заново, иначе он считает готовой ту, которой уже нет.
    c.card_ready    = false;
    c.media_dirty   = false;
    s_media_recheck = true;
    // Слоты держат сектора прежнего носителя: попадание в них после смены
    // отдало бы хосту чужие данные. Заказ, взятый циклом до смены, объявит
    // слот годным только со своим номером сектора.
    drop_slots(c);
    abort_transfer(c);
}

// Один такт автомата: выбор или байт. Общий для хозяина и для разбора
// очереди - копия в горячем пути стоила бы полутора килобайт SRAM.
SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(step_one)(Card& c, uint16_t e) {
    if (c.media_dirty) take_media(c);
    if (e & kPendSelect) {
        select_step(c, (e & kPendOn) != 0u);
    } else if (c.selected) {
        c.miso = card_out(c);
        card_in(c, static_cast<uint8_t>(e & 0xffu));
    }
}

// Разобрать очередь до пустоты и снять признак занятости. Повтор цикла -
// на случай такта, пришедшего между последней проверкой и снятием.
SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(drain_pending)(Card& c) {
    do {
        while (c.pend_tail != c.pend_head) {
            const uint16_t e = c.pend[c.pend_tail];
            c.pend_tail      = static_cast<uint8_t>((c.pend_tail + 1u) & (kPendLen - 1u));
            step_one(c, e);
        }
        c.step_busy = false;
        if (c.pend_tail == c.pend_head) return;
        c.step_busy = true;
    } while (true);
}

// Свободный автомат: такт исполняет пришедший сам, затем разбирает то, что
// за это время положили вытеснившие. Занятый: такт в очередь. В очередь
// кладёт только вытеснивший, а его самого никто не вытесняет - писатель
// головы один, и вытеснение посреди pend_push невозможно.
SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(run_or_queue)(Card& c, uint16_t e) {
    if (c.step_busy) {
        pend_push(c, e);
        ++s_pend_used;
        return;
    }
    c.step_busy = true;
    step_one(c, e);
    drain_pending(c);
}

// Сама сверка - во флеше: заходят сюда считанные разы за работу, только
// когда носитель и правда сменился. В SRAM живёт лишь проверка поколения
// ниже.
SOUNDSINTH_NOINLINE void refresh_media_changed(uint32_t gen) {
    // Сторона, чья прошлая смена ещё не принята обработчиком, пропускается
    // ниже; её новое состояние сверка возьмёт после приёма прошлой смены
    // (s_media_recheck), а не на каждом витке.
    s_media_recheck  = false;
    s_media_gen_seen = gen;
    for (uint32_t i = 0; i < kCardCount; ++i) {
        Card& c                   = s_cards[i];
        const hal::MediaState& st = storage::storage_emulator_state(static_cast<SdOwner>(i));
        // Сверка - с тем, что машина увидит следующим: подготовленная, но
        // не принятая смена уже ждёт её такта. Носитель вынули и вставили,
        // пока машина не тактировала карту: смена "нет носителя" ждёт, а
        // снимок совпал с прежним - без этого вторая смена потерялась бы.
        const bool dirty  = c.media_dirty;
        const Media& next = dirty ? c.next : c.media;
        if (st.present == next.present && st.sectors == next.sectors && st.serial == next.serial) continue;
        if (dirty) continue; // прошлая смена ещё не принята

        // Поток только готовит смену, а применяет её обработчик на своём
        // такте. Проверить "не выбрана" и переписать CSD нельзя: между
        // проверкой и записью хост опускает CS и шлёт CMD9, и ему уходит
        // половина старого блока с несходящимся CRC.
        fill_media(c.next, st);
        c.media_dirty = true;
    }
}

// Сектор переписан на носителе m: упреждённые копии этого сектора у сторон
// на том же носителе больше не годны. Занята только писавшая сторона, вторая
// вправе ждать токен на тот же сектор - ей заказ переставляется заново, иначе
// она ждала бы готовности, которой никто не даст. Носитель у сторон бывает
// разный: гасится слот только там, где он тот же.
void drop_written_sector(hal::Medium m, uint32_t lba) {
    for (uint32_t i = 0; i < kCardCount; ++i) {
        Card& c = s_cards[i];
        if (storage::storage_emulator_medium(static_cast<SdOwner>(i)) != m) continue;
        for (uint8_t slot = 0; slot < 2; ++slot) {
            if (c.slot[slot].lba != lba) continue;
            c.slot[slot].valid.store(false, std::memory_order_relaxed);
            if (c.phase == Phase::ReadToken && c.lba == lba && c.cur == slot) {
                c.req.store(make_req(lba, slot), std::memory_order_release);
            }
        }
    }
}

// Защита от повторного входа в sd_spi_task: ожидание обмена с флешкой
// обслуживает шину, а то обслуживание зовёт цикл эмулятора снова. Один
// контекст (цикл Core1), поэтому простой признак, а не атомик.
bool s_task_entered = false;

} // namespace

// --- Публичное ---

void sd_spi_emu_init() {
    storage::storage_on_sector_written(&drop_written_sector);
    build_cid(s_cid);
    s_cid_crc = sd::crc16(s_cid, sd::kRegisterBytes);
    // Автоматы поднимаются все: у каждой стороны своя карта, и после
    // загрузки обе обязаны быть в состоянии "только что включили".
    for (uint32_t i = 0; i < kCardCount; ++i) {
        Card& c = s_cards[i];
        c.who   = static_cast<SdOwner>(i);
        // Свойства носителя снимаются здесь, обработчик прерывания во флеш
        // за ними не ходит.
        fill_media(c.media, storage::storage_emulator_state(c.who));
        c.reg        = c.media.csd;
        c.card_ready = false;
        c.selected   = false;
        c.app_cmd    = false;
        c.byte_idx   = 0;
        c.crc_idx    = 0;
        c.reg_idx    = 0;
        c.cur        = 0;
        drop_slots(c);
        c.miso      = sd::kIdleByte;
        c.pend_head = 0;
        c.pend_tail = 0;
        c.step_busy = false;
        c.write_req.store(0, std::memory_order_relaxed);
        c.write_failed = false;
        c.write_reject = false;
        c.media_dirty  = false;
        abort_transfer(c);
    }
    // Состояние только что снято прямо из снимка: сверке на этом поколении
    // делать нечего.
    s_media_gen_seen = hal::media_state_generation();
}

// Признак подъёма - хозяин автомата Z-Controller: его ставит только
// sd_spi_emu_init, до неё поле нулевое (DivMmc).
void sd_spi_emu_ensure_up() {
    constexpr uint32_t kZc = static_cast<uint32_t>(SdOwner::ZController);
    if (s_cards[kZc].who != SdOwner::ZController) sd_spi_emu_init();
}

uint32_t sd_spi_media_failures(SdOwner who) {
    const auto i = static_cast<uint32_t>(who);
    return i < kCardCount ? s_media_fail[i] : 0u;
}

void sd_spi_log_stats() {
    // Выбор карты - снимок в момент печати, короткий обмен его не видит;
    // рядом накопительные cmd/rd по сторонам и их прирост за период. Под
    // работающим приложением прироста divmmc быть не должно: ненулевой -
    // к карте лезут мимо ожиданий.
    static uint32_t s_last_cmds[kCardCount] = {};
    const auto zc                           = static_cast<uint32_t>(SdOwner::ZController);
    const auto dm                           = static_cast<uint32_t>(SdOwner::DivMmc);
    const uint32_t d_zc                     = s_cmds[zc] - s_last_cmds[zc];
    const uint32_t d_dm                     = s_cmds[dm] - s_last_cmds[dm];
    s_last_cmds[zc]                         = s_cmds[zc];
    s_last_cmds[dm]                         = s_cmds[dm];
    // 320: строка выбора в худшем случае 288 байт, в буфер debug_logf на 192
    // не влезает.
    char sm[320];
    std::snprintf(sm, sizeof(sm),
                  "sd: selected zc=%u dm=%u writer card=%s usb=%s write_denied=%" PRIu32 " pend=%" PRIu32 "/%" PRIu32 " | zctrl cmd=%" PRIu32 " (+%" PRIu32
                  ") rd=%" PRIu32 " nomedia=%" PRIu32 " wfail=%" PRIu32 " last=%u | divmmc cmd=%" PRIu32 " (+%" PRIu32 ") rd=%" PRIu32 " nomedia=%" PRIu32
                  " wfail=%" PRIu32 " last=%u\n",
                  s_cards[zc].selected ? 1u : 0u, s_cards[dm].selected ? 1u : 0u, owner_name(storage::storage_writer(hal::Medium::Card)),
                  owner_name(storage::storage_writer(hal::Medium::Usb)), storage::storage_writes_denied(), s_pend_used, s_pend_lost, s_cmds[zc], d_zc,
                  s_reads[zc], s_media_fail[zc], s_write_fail[zc], s_last_cmd[zc], s_cmds[dm], d_dm, s_reads[dm], s_media_fail[dm], s_write_fail[dm],
                  s_last_cmd[dm]);
    debug_log(sm);
    // Вторая карта DivMMC - своей строкой и только когда к ней обращались.
    const auto d2 = static_cast<uint32_t>(SdOwner::DivMmc2);
    if (s_cmds[d2] != 0) {
        debug_logf("sd: divmmc2 selected=%u cmd=%" PRIu32 " (+%" PRIu32 ") rd=%" PRIu32 " nomedia=%" PRIu32 " wfail=%" PRIu32 " last=%u\n",
                   s_cards[d2].selected ? 1u : 0u, s_cmds[d2], s_cmds[d2] - s_last_cmds[d2], s_reads[d2], s_media_fail[d2], s_write_fail[d2], s_last_cmd[d2]);
    }
    s_last_cmds[d2] = s_cmds[d2];
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
        if (back >= total) continue;
        const TraceEntry& e  = s_trace[(total - 1u - back) & (kTraceLen - 1u)];
        const uint32_t at_ms = static_cast<uint32_t>((static_cast<uint64_t>(e.at_kus) * 1024u) / 1000u);
        std::snprintf(t, sizeof(t), "  %" PRIu32 " ms %-7s%s%u arg=%" PRIu32 " -> %02X\n", at_ms, owner_name(e.owner), e.acmd ? "ACMD" : "CMD", e.cmd, e.arg,
                      e.resp);
        debug_log(t);
    }
}

void SOUNDSINTH_HOT_PATH(sd_spi_select)(SdOwner who, bool select) {
    // Арбитра шины нет: у каждой стороны свой автомат. Держит одна
    // выбранной сколько угодно - второй это не мешает ничем.
    Card* const pc = card_of(who);
    if (pc == nullptr) return;
    run_or_queue(*pc, static_cast<uint16_t>(kPendSelect | (select ? kPendOn : 0u)));
}

uint8_t SOUNDSINTH_HOT_PATH(sd_spi_latch)(SdOwner who) {
    const Card* const pc = card_of(who);
    return pc != nullptr ? *const_cast<const volatile uint8_t*>(&pc->miso) : sd::kIdleByte;
}

uint8_t SOUNDSINTH_HOT_PATH(sd_spi_byte)(SdOwner who, uint8_t mosi) {
    Card* const pc = card_of(who);
    // Автомата у стороны нет - 0xFF, как на пустой шине.
    if (pc == nullptr) return sd::kIdleByte;
    Card& c = *pc;
    // Не выбрана - 0xFF, как ведущий на невыбранной карте. Защёлка тоже:
    // это ответ последнего обмена, и по ней пишется ячейка порта.
    if (!c.selected) {
        c.miso = sd::kIdleByte;
        return sd::kIdleByte;
    }

    // Быстрый путь потоковых фаз: байт из буфера и инкремент. Общий путь стоит
    // 2 мкс и с обработчиком соседнего порта не влезает в 3 мкс цикла IN.
    // Последний байт фазы - общим путём: смена фазы и заказ сектора.
    // Индекс - под step_busy: иначе вытеснивший продвинет его второй раз.
    if (!c.step_busy && !c.media_dirty && c.pend_tail == c.pend_head && c.media.present && c.byte_idx + 1u < storage::kSectorBytes) {
        // CMD12 приходит посреди потока и отличается от данных старшей парой
        // бит: её быстрый путь обязан пропустить к общему, иначе она уйдёт в буфер.
        if (c.phase == Phase::ReadData && !(c.multi && (mosi & sd::kCmdStartMask) == sd::kCmdStartValue)) {
            c.step_busy      = true;
            const uint8_t rx = c.block[c.cur][c.byte_idx];
            ++c.byte_idx;
            c.miso      = rx;
            c.step_busy = false;
            if (c.pend_tail != c.pend_head) {
                c.step_busy = true;
                drain_pending(c);
            }
            return rx;
        }
        if (c.phase == Phase::WriteData) {
            c.step_busy          = true;
            c.wblock[c.byte_idx] = mosi;
            ++c.byte_idx;
            c.miso      = sd::kIdleByte;
            c.step_busy = false;
            if (c.pend_tail != c.pend_head) {
                c.step_busy = true;
                drain_pending(c);
            }
            return sd::kIdleByte;
        }
    }
    // Такт пришёл в занятый автомат: он ждёт в очереди, а вытеснивший
    // получает прежнее содержимое защёлки. У Z-Controller защёлка и так
    // конвейерная, у DivMMC байт доедет следующим обменом.
    //
    // Свободный автомат: после разбора очереди отдаётся защёлка, а не свой
    // байт: вытеснивший такт уже случился, и его ответ - последний. Иначе он
    // пропал бы, а хост получил бы байт предыдущего обмена дважды.
    run_or_queue(c, mosi);
    return c.miso;
}

// Зовут с каждого витка цикла Core1, а носитель меняется считанные разы за
// работу: одно слово поколения из SRAM, и почти всегда возврат.
void SOUNDSINTH_HOT_PATH(sd_spi_emu_refresh_media)() {
    const uint32_t gen = hal::media_state_generation();
    if (gen == s_media_gen_seen && !s_media_recheck) return;
    refresh_media_changed(gen);
}

void SOUNDSINTH_HOT_PATH(sd_spi_task)() {
    if (s_task_entered) return;
    s_task_entered = true;

    // Единственное место, где читается и пишется сектор. Носитель
    // сверяется здесь же: флешка встаёт позже подъёма эмулятора, карту
    // вынимают на ходу.
    sd_spi_emu_refresh_media();

    // Запись первой: заказ чтения после неё видит записанный сектор. Номер
    // - из заказа, lba автомата меняет следующая команда хоста.
    for (uint32_t w = 0; w < kCardCount; ++w) {
        Card& wc          = s_cards[w];
        const uint32_t wr = wc.write_req.load(std::memory_order_acquire);
        // Носитель занят обменом, из ожидания которого пришёл этот виток:
        // запись ждёт освобождения, а не получает отказ.
        if (wr == 0u || hal::media_busy(storage::storage_emulator_medium(static_cast<SdOwner>(w)))) continue;
        const uint32_t lba = req_lba(wr);
        wc.write_failed    = !storage::storage_write(static_cast<SdOwner>(w), lba, wc.wblock);
        if (wc.write_failed) ++s_write_fail[w];
        // Слоты со старыми данными гасит drop_written_sector из записи
        // хранилища - и для записей платы.
        wc.write_req.store(0, std::memory_order_release);
    }

    // Заказы - по кругу всех сторон, пока есть: за чтение сектора обработчик
    // успевает заказать следующий. Заказ забирается обменом слова на ноль.
    // Носитель стороны занят - идёт его обмен (виток пришёл из ожидания)
    // или брошенный ещё может дописать в слот: заказ остаётся, хост ждёт
    // токен, а не получает ошибку исправного носителя.
    for (bool any = true; any;) {
        any = false;
        for (uint32_t i = 0; i < kCardCount; ++i) {
            Card& c           = s_cards[i];
            const SdOwner who = static_cast<SdOwner>(i);
            if (hal::media_busy(storage::storage_emulator_medium(who))) continue;
            const uint32_t req = c.req.exchange(0, std::memory_order_acquire);
            if ((req & kReqValidBit) == 0u) continue;
            any                = true;
            const uint32_t lba = req_lba(req);
            const uint8_t slot = req_slot(req);
            // Сектор уже в слоте (заказ повторён, CMD18 на промахе блока):
            // второе чтение в буфер, из которого обработчик льёт, не нужно.
            if (slot_holds(c, slot, lba)) continue;
            // Слот негоден до конца чтения: отказ не оставит признак годности
            // с чужим буфером, и брошенный CMD17 не отдаст полблока другого.
            // Именно в этом порядке: погасить до проверки - погасить годный
            // слот под отдачей.
            c.slot[slot].valid.store(false, std::memory_order_release);
            if (storage::storage_emulator_read(who, lba, c.block[slot])) {
                // Считается здесь, в цикле, до объявления блока годным:
                // обработчику остаётся только отдавать байты.
                c.slot[slot].crc = sd::crc16(c.block[slot], storage::kSectorBytes);
                c.slot[slot].lba = lba;
                // Последним действием, с барьером: увидев признак,
                // обработчик видит байты, CRC и номер сектора.
                c.slot[slot].valid.store(true, std::memory_order_release);
            } else {
                // Фазу снимет обработчик.
                c.slot[slot].failed.store(lba + 1u, std::memory_order_release);
                ++s_media_fail[i];
            }
        }
    }

    // Свободное время: заказов больше нет, и можно подтянуть следующий
    // блок. Здесь, а не из цикла, потому что защита от повторного входа
    // выше не даёт начать обмен с носителем, пока идёт этот. Начни
    // предвыборку снаружи - вложенное чтение получило бы отказ занятости
    // и хост увидел бы ошибку сектора.
    storage::storage_prefetch_step();
    s_task_entered = false;
}

} // namespace devices::sd
