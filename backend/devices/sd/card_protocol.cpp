// SPDX-License-Identifier: MIT
// Протокол карты SD/SDHC по SPI. Железа здесь нет: шина
// приходит из порта.

#include "devices/sd/card_protocol.h"

#include <cinttypes>
#include <cstdio>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "core/util/fnv1a.h"

#include "devices/hal/media.h"
#include "devices/hal/spi.h"
#include "devices/sd/sd_protocol.h"

namespace devices::sd {
namespace {

// До инициализации спецификация разрешает 100-400 кГц, здесь верхняя
// граница; фактическую возвращает порт и она печатается в лог.
constexpr uint32_t kBaudInit = SOUNDSINTH_SD_BAUD_INIT;
constexpr uint32_t kBaudWork = SOUNDSINTH_SD_BAUD_WORK;

// Рабочая частота по запросу: её ставит и рукопожатие, и переинициализация.
// kBaudWork - частота сборки (-D), kDefaultSpeedMax - предел спецификации
// без High Speed: выше него нужен CMD6, он держится до следующего
// рукопожатия.
constexpr uint32_t kDefaultSpeedMax = 25000000u;
uint32_t s_work_req                 = kBaudWork;
bool s_high_speed                   = false;
// CMD6, режим 1 (переключить): группа 1 - функция 1 (High Speed), прочие
// без изменений. Ответ - блок состояния 64 байта; выбранная функция
// группы 1 - младшая тетрада байта 16, 0xF - не умеет.
constexpr uint32_t kSwitchHighSpeed = 0x80fffff1u;
constexpr uint32_t kSwitchStatus    = 64;
constexpr uint32_t kSwitchGroup1    = 16;
constexpr uint8_t kSwitchFnMask     = 0x0f; // функция группы - младшая тетрада

// --- Сведения о карте ---

SdInfo s_info;

// --- Итог рукопожатия: числа для sd_card_log_handshake ---
enum class HandshakeStep : uint8_t {
    Ok = 0,
    Cmd0,      // карта не вошла в состояние покоя
    Cmd8,      // отозвалась не тем напряжением
    LeaveIdle, // не вышла из покоя ни по ACMD41, ни по CMD1
    Ocr,       // CMD58 не отдал регистр
    BlockLen,  // CMD16 не принял длину блока
    Csd,       // CMD9 не прислал регистр
};

struct HandshakeReport {
    HandshakeStep step   = HandshakeStep::Cmd0;
    uint8_t r0           = 0xff; // ответ на CMD0
    uint8_t miso_idle    = 0xff; // уровень MISO до первой команды
    uint8_t r8           = 0xff; // ответ на CMD8
    uint8_t r58          = 0xff; // ответ на CMD58 в покое
    uint8_t last_r41     = 0xff; // последний ответ ACMD41 или CMD1
    bool v2              = false;
    bool mmc             = false;
    SdKind kind          = SdKind::None;
    uint32_t sectors     = 0;
    uint32_t volt_window = 0; // окно напряжений, как его сообщила карта
    uint32_t init_khz    = 0; // частота в режиме опознания
    uint32_t work_khz    = 0; // рабочая частота
    uint32_t acmd41_ms   = 0; // сколько карта выходила из покоя
};
HandshakeReport s_hs;

// --- Обслуживание хоста из ожиданий карты ---
hal::WaitServiceFn s_wait_service = nullptr;
void* s_wait_service_user         = nullptr;
// Драйвер внутри своей транзакции: вложенные чтение и запись сектора
// отказывают, иначе новая команда ушла бы посреди чужой при опущенном CS.
bool s_in_transaction = false;

// Сроки ожидания карты: токен данных и конец занятости (запись, CMD12).
constexpr uint32_t kTokenTimeoutMs = 200;
constexpr uint32_t kBusyTimeoutMs  = 500;
// Попыток чтения сектора с неверной CRC и байт опроса ответа R1.
constexpr uint32_t kCrcAttempts = 3;
constexpr uint8_t kR1PollBytes  = 10;

// Зовётся из циклов ожидания, не чаще этого срока. Время - в микросекундах
// 32 бит: опрос токена крутится на каждом читаемом секторе, а миллисекунды
// стоили бы там деления 64-битного времени. Сам срок - в заголовке, его
// сверяет тест.
constexpr uint32_t kServicePeriodUs = kCardServicePeriodUs;
void SOUNDSINTH_HOT_PATH(serve_while_waiting)(uint32_t& last_service_us) {
    if (s_wait_service == nullptr) return;
    const uint32_t now = platform::mono_us();
    if (now - last_service_us < kServicePeriodUs) return;
    last_service_us = now;
    s_wait_service(s_wait_service_user);
}

// Пауза, которая отдаёт время шине: занятое ожидание кусками по сроку
// обслуживания. Пауз по десять миллисекунд за проход рукопожатия до
// трёхсот, и всё это время хост обслуживается.
void serve_during_pause(uint32_t total_us, uint32_t& last_service_us) {
    const uint32_t t0 = platform::mono_us();
    while (platform::mono_us() - t0 < total_us) {
        platform::busy_wait(kServicePeriodUs);
        serve_while_waiting(last_service_us);
    }
}

// --- Примитивы ---

uint8_t SOUNDSINTH_HOT_PATH(xfer)(uint8_t out) {
    return hal::spi_xfer(out);
}

void SOUNDSINTH_HOT_PATH(cs_low)() {
    hal::spi_select(true);
}
void SOUNDSINTH_HOT_PATH(cs_high)() {
    hal::spi_select(false);
    xfer(sd::kIdleByte); // карте нужен такт после снятия CS, чтобы отпустить линию
}

// Ответ R1 приходит не сразу: по спецификации до восьми байт линия в
// 0xFF, опрос - kR1PollBytes, с запасом.
uint8_t SOUNDSINTH_HOT_PATH(wait_r1)() {
    for (uint8_t i = 0; i < kR1PollBytes; ++i) {
        const uint8_t r = xfer(sd::kIdleByte);
        if ((r & sd::kR1StartBit) == 0) return r;
    }
    return sd::kIdleByte; // не ответила
}

SOUNDSINTH_NOCLONE uint8_t SOUNDSINTH_HOT_PATH(send_cmd)(uint8_t cmd, uint32_t arg) {
    uint8_t frame[sd::kCmdFrameBytes];
    frame[0] = static_cast<uint8_t>(sd::kCmdStartValue | cmd);
    frame[1] = static_cast<uint8_t>(arg >> 24);
    frame[2] = static_cast<uint8_t>(arg >> 16);
    frame[3] = static_cast<uint8_t>(arg >> 8);
    frame[4] = static_cast<uint8_t>(arg);
    frame[5] = sd::crc7(frame, 5);

    // Холостой байт перед кадром - интервал N_CS из спецификации: без него
    // 4-гигабайтная карта этой платы отвечает на CMD8 0x7F (сдвиг на бит).
    // Восемь тактов на команду обмен не замедляют.
    xfer(sd::kIdleByte);
    for (uint32_t i = 0; i < sd::kCmdFrameBytes; ++i) {
        xfer(frame[i]);
    }
    return wait_r1();
}

// Дождаться токена начала блока. 0xFE - данные; 0000xxxx - сообщение об
// ошибке от карты, ждать дальше бессмысленно.
// Срок - по 32-битному таймеру: 64-битное время читается функцией из флеша,
// а ожидание идёт, пока Core1 обслуживает шину.
bool SOUNDSINTH_HOT_PATH(wait_data_token)(uint32_t timeout_ms) {
    const uint32_t start_us  = platform::mono_us();
    uint32_t last_service_us = start_us;
    for (;;) {
        const uint8_t t = xfer(sd::kIdleByte);
        if (t == sd::kTokenStartBlock) return true;
        if ((t & sd::kTokenErrorMask) == 0) return false; // токен ошибки
        if (platform::mono_us() - start_us > timeout_ms * 1000u) return false;
        serve_while_waiting(last_service_us);
    }
}

// После записи карта держит MISO в нуле, пока пишет физически.
bool SOUNDSINTH_HOT_PATH(wait_not_busy)(uint32_t timeout_ms) {
    const uint32_t t0        = platform::mono_us();
    uint32_t last_service_us = t0;
    for (;;) {
        if (xfer(sd::kIdleByte) == sd::kIdleByte) return true;
        if (platform::mono_us() - t0 >= timeout_ms * 1000u) return false;
        serve_while_waiting(last_service_us);
    }
}

// n байт ответа карты, по байту за обмен.
void read_bytes(uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = xfer(sd::kIdleByte);
    }
}

// Два байта CRC16 блока тактируются 0xFF: при чтении CSD, CID и CMD6 их
// не проверяем, при записи карта их не проверяет.
void SOUNDSINTH_HOT_PATH(clock_crc)() {
    xfer(sd::kIdleByte);
    xfer(sd::kIdleByte);
}

// Адрес в командах чтения и записи: у SDHC номер сектора, у SDSC байты.
uint32_t block_arg(uint32_t lba) {
    return (s_info.kind == SdKind::Sdhc) ? lba : (lba * sd::kBlockBytes);
}

// Перевод карты в High Speed. true - карта переключилась.
bool switch_high_speed() {
    cs_low();
    bool ok = send_cmd(sd::kCmd6, kSwitchHighSpeed) == sd::kR1Ready && wait_data_token(kTokenTimeoutMs);
    if (ok) {
        uint8_t st[kSwitchStatus];
        hal::spi_read(sd::kIdleByte, st, kSwitchStatus);
        clock_crc();
        ok = (st[kSwitchGroup1] & kSwitchFnMask) == 1u;
    }
    cs_high();
    return ok;
}

// Частота по запросу; High Speed карта не приняла - kDefaultSpeedMax.
uint32_t apply_speed() {
    if (s_work_req > kDefaultSpeedMax && !s_high_speed) s_high_speed = switch_high_speed();
    const uint32_t hz = s_work_req > kDefaultSpeedMax && !s_high_speed ? kDefaultSpeedMax : s_work_req;
    s_hs.work_khz     = hal::spi_set_hz(hz) / 1000u;
    return s_hs.work_khz;
}

// Отказ рукопожатия на шаге step: выбор снят, шаг - в итог.
bool handshake_fail(HandshakeStep step) {
    cs_high();
    s_hs.step = step;
    return false;
}

// Опрос выхода из покоя: ACMD41 (app) или CMD1, пока карта не ответит
// "готова" или не выйдет limit_ms. Пауза между опросами обязательна: карта
// отвечает 0x01 мгновенно, без задержки она получает тысячи команд в
// секунду, и часть карт от такого темпа не продвигает внутреннюю
// инициализацию. Исправная карта выходит из idle за десятки шагов по 10 мс.
SOUNDSINTH_NOINLINE bool poll_op_cond(bool app, uint32_t arg, uint32_t limit_ms, uint32_t& last_service_us) {
    const uint32_t t0 = platform::mono_us();
    for (;;) {
        if (app) send_cmd(sd::kCmd55, 0);
        s_hs.last_r41 = send_cmd(app ? sd::kAcmd41 : sd::kCmd1, arg);
        if (s_hs.last_r41 == sd::kR1Ready) return true;
        if (platform::mono_us() - t0 >= limit_ms * 1000u) return false;
        serve_during_pause(10u * 1000u, last_service_us);
    }
}

// Выводы и блок SPI заново (и при переинициализации - как при старте),
// затем обмен с картой: CMD0 ... CSD и рабочая частота. Сведения - в
// out, s_info не трогается: при неудачной переинициализации остаются
// прежние. Номер карты в out - прежний, пока CID не прочитан: отказ CMD10
// не объявляет машине смену носителя.
// Вход в состояние покоя: CMD0 с растущей промывкой. true - карта в idle,
// ответ последней попытки в s_hs.r0.
bool enter_idle(uint32_t& last_service_us) {
    // CMD0 с повторами, число холостых тактов между ними растёт.
    //
    // 80 тактов хватает только что включённой карте. Тёплый сброс платы
    // карту не обесточивает: если её застали посреди блока данных, она держит
    // линию и досылает 512 байт, не слушая команд, а восемь попыток по 80
    // тактов блок не доматывают.
    //
    // Поэтому холостых байт перед CMD0 на каждой попытке вдвое больше: 10,
    // 20, 40 и так до 640 (5120 тактов) - карта успевает дослать блок с
    // контрольной суммой и снова слушает. Исправная карта отвечает с первой
    // попытки.
    for (uint8_t attempt = 0; attempt < 8; ++attempt) {
        cs_high();
        // 10 байт на первой попытке, дальше вдвое больше, потолок 640.
        const uint16_t flush = static_cast<uint16_t>(10u << (attempt < 6u ? attempt : 6u));
        for (uint16_t i = 0; i < flush; ++i) {
            xfer(sd::kIdleByte);
            // Порция до 640 байт, на 400 кГц это 12.8 мс: шина обслуживается
            // каждые 32 байта.
            if ((i & 0x1fu) == 0x1fu) serve_while_waiting(last_service_us);
        }
        cs_low();
        s_hs.r0 = send_cmd(sd::kCmd0, 0);
        if (s_hs.r0 == sd::kR1Idle) break;
    }
    return s_hs.r0 == sd::kR1Idle;
}

// CSD обязателен, CID - нет: без него номер носителя остаётся прежним.
// false - CSD не прочитан.
bool read_csd_cid(SdInfo& out, uint8_t* csd) {
    if (send_cmd(sd::kCmd9, 0) != sd::kR1Ready || !wait_data_token(kTokenTimeoutMs)) return false;
    read_bytes(csd, sd::kRegisterBytes);
    clock_crc();
    if (send_cmd(sd::kCmd10, 0) == sd::kR1Ready && wait_data_token(kTokenTimeoutMs)) {
        read_bytes(out.cid, sd::kRegisterBytes);
        clock_crc();
        out.cid_valid = true;
        // Свёртка CID: другая карта - другой номер.
        out.serial = soundsinth::util::fnv1a(soundsinth::util::kFnvSeed, out.cid, sd::kRegisterBytes);
    }
    return true;
}

bool card_handshake(SdInfo& out) {
    out        = SdInfo{};
    out.serial = s_info.serial;
    s_hs       = HandshakeReport{};
    // Рукопожатие идёт под живой шиной: отметка нужна с первой же паузы.
    uint32_t last_service_us = platform::mono_us();

    // Не меньше 74 тактов при снятом CS - так карта переходит в SPI-режим.
    //
    // Частота - фактическая, которую вернул SDK: делитель SPI целый и чётный.
    // В режиме идентификации нужно 100-400 кГц; промах дал бы неотвечающую
    // карту, по виду неотличимую от обрыва.
    s_hs.init_khz = hal::spi_open(kBaudInit) / 1000u;
    hal::spi_select(false);
    for (uint8_t i = 0; i < 10; ++i) {
        xfer(sd::kIdleByte);
    }

    // Уровень MISO до первой команды: 0xFF без карты (подтяжка держит
    // единицу), 0x00 - линия посажена, иное - карта отвечает не тем.
    // Значение идёт в сообщение об отказе: по нему непропай отличается от
    // беды с протоколом.
    for (uint8_t i = 0; i < 8; ++i) {
        s_hs.miso_idle = xfer(sd::kIdleByte);
    }

    if (!enter_idle(last_service_us)) return handshake_fail(HandshakeStep::Cmd0);

    // CMD8 отделяет карты v2 от v1: v1 отвечает "нет такой команды".
    s_hs.r8       = send_cmd(sd::kCmd8, sd::kIfCondArg);
    const bool v2 = (s_hs.r8 & sd::kR1IllegalCmd) == 0;
    if (v2) {
        uint8_t tail[4];
        read_bytes(tail, 4);
        if (tail[2] != sd::kIfCondVoltage || tail[3] != sd::kIfCondPattern) return handshake_fail(HandshakeStep::Cmd8);
    }
    s_hs.v2 = v2;

    // OCR прямо в idle, до инициализации (CMD58 в idle разрешён). Осмысленное
    // окно напряжений (обычно 0x00FF8000, 2.7-3.6 В) - шина в порядке и карта
    // жива; мусор или нули - проблема в обмене, а не в инициализации.
    const uint32_t acmd41_t0_us = platform::mono_us();
    uint8_t ocr_idle[4]         = {0, 0, 0, 0};
    s_hs.r58                    = send_cmd(sd::kCmd58, 0);
    if (s_hs.r58 <= sd::kR1Idle) read_bytes(ocr_idle, 4);
    // Младшие три байта OCR - окно напряжений, ниже возвращается карте в
    // аргументе ACMD41/CMD1. Спецификация разрешает нулевое окно, но часть
    // карт на нём не встаёт.
    const uint32_t volt_window = (static_cast<uint32_t>(ocr_idle[1]) << 16) | (static_cast<uint32_t>(ocr_idle[2]) << 8) | static_cast<uint32_t>(ocr_idle[3]);
    s_hs.volt_window           = (static_cast<uint32_t>(ocr_idle[0]) << 24) | volt_window;

    // ACMD41 - оба варианта HCS: сначала по ответу на CMD8, потом
    // противоположный. SDHC, принятая за v1, без HCS из idle не выходит (так
    // ведёт себя 4-гигабайтная карта); ошибка версии стоит 1.5 с на старте, а
    // не отказа. Запасной путь - MMC: внешне как SD, но ACMD41 не понимает и
    // остаётся в idle; ей CMD1.
    last_service_us    = platform::mono_us();
    const uint32_t hcs = v2 ? sd::kOpCondHcsBit : 0u;
    bool ready =
        poll_op_cond(true, hcs | volt_window, 1500u, last_service_us) || poll_op_cond(true, (hcs ^ sd::kOpCondHcsBit) | volt_window, 1500u, last_service_us);
    if (!ready) {
        ready    = poll_op_cond(false, sd::kOpCondHcsBit | volt_window, 1000u, last_service_us);
        s_hs.mmc = ready;
    }
    s_hs.acmd41_ms = (platform::mono_us() - acmd41_t0_us) / 1000u;
    if (!ready) return handshake_fail(HandshakeStep::LeaveIdle);

    // Ёмкость по OCR всегда, а не только у v2: версия могла определиться
    // неверно, а CCS в OCR - ответ самой карты.
    out.kind = SdKind::Sdsc;
    if (send_cmd(sd::kCmd58, 0) == sd::kR1Ready) {
        uint8_t ocr[4];
        read_bytes(ocr, 4);
        if (ocr[0] & sd::kOcrCcsBit) out.kind = SdKind::Sdhc;
    } else if (v2) {
        return handshake_fail(HandshakeStep::Ocr);
    }

    if (out.kind == SdKind::Sdsc && send_cmd(sd::kCmd16, sd::kBlockBytes) != sd::kR1Ready) return handshake_fail(HandshakeStep::BlockLen);

    uint8_t csd[sd::kRegisterBytes];
    if (!read_csd_cid(out, csd)) return handshake_fail(HandshakeStep::Csd);
    cs_high();

    out.sector_count = sectors_from_csd(csd);
    out.present      = true;
    s_high_speed     = false;
    (void)apply_speed();

    s_hs.step    = HandshakeStep::Ok;
    s_hs.kind    = out.kind;
    s_hs.sectors = out.sector_count;
    return true;
}

// Счётчики карты: сбои чтения по причинам, время чтений и записей,
// сбои записи.
struct SdCardStats {
    uint32_t r1            = 0; // карта не приняла CMD17 (ответ не ноль)
    uint32_t token         = 0; // не пришёл токен данных
    uint32_t crc           = 0; // CRC не сошлась во всех трёх попытках
    uint32_t crc_retry     = 0; // попытки с неверной CRC, в том числе снятые повтором
    uint8_t last_r1        = 0; // ответ последнего сбоя по R1
    uint32_t last_lba      = 0; // сектор последнего сбоя
    uint32_t reinit        = 0; // переинициализаций карты после сбоя
    uint32_t reinit_failed = 0; // из них неудачных
    // Сколько цикл Core1 стоит в чтении сектора: наибольшее время, чтений
    // дольше kSlowReadUs и чтений, отклонённых окном переинициализации без
    // обращения к карте (один сектор без токена отключает носитель всем
    // клиентам на время интервала).
    uint32_t read_max_us    = 0;
    uint32_t slow_reads     = 0;
    uint32_t window_rejects = 0;
    uint32_t reads          = 0;
    // Сумма копится в микросекундах: чтение короче миллисекунды при
    // округлении на месте давало бы ноль, и тысяча чтений - тоже ноль.
    uint64_t read_total_us = 0;
    // То же про запись. Меряется, потому что карта после приёма данных
    // держит линию занятой, пока пишет физически, и на большой карте
    // одиночная запись обходится в сотни миллисекунд.
    uint32_t writes         = 0;
    uint32_t write_max_us   = 0;
    uint64_t write_total_us = 0;
    // Неудачные записи по причине: карта не приняла CMD24, отвергла данные,
    // не освободила линию за 500 мс. Хост видит их только через CMD13.
    uint32_t write_r1   = 0;
    uint32_t write_data = 0;
    uint32_t write_busy = 0;
};
constexpr uint32_t kSlowReadUs = SOUNDSINTH_SD_SLOW_READ_US;
SdCardStats s_card_stats;

// Карта вышла из рабочего режима: не приняла команду чтения или замолчала
// посреди него. На плате так бывает после самопроизвольного сброса карты:
// R1 0x07 - бит "в состоянии покоя" стоит, карта ждёт инициализации, и
// без неё не отдаёт больше ни одного сектора.
bool s_need_reinit  = false;
bool s_reinit_tried = false;
// Инициализация блокирует цикл Core1: карту, застрявшую в idle, - до около
// 4 с (два прохода ACMD41 и CMD1), мёртвую - на десятки мс. Интервал
// считается от конца попытки и удваивается на каждой неудаче подряд: иначе
// следующее же чтение начинало новую попытку, и Core1 стоял в них почти
// всё время. После пяти неудач доля времени - около 11%.
uint32_t s_last_reinit_ms               = 0; // конец прошлой попытки
constexpr uint32_t kReinitIntervalMs    = SOUNDSINTH_SD_REINIT_MS;
constexpr uint32_t kReinitIntervalMaxMs = SOUNDSINTH_SD_REINIT_MAX_MS;
uint32_t s_reinit_interval_ms           = kReinitIntervalMs;

enum class ReadResult : uint8_t { Ok, Lost, BadCrc };

// Переинициализировать карту. Сведения о карте при неудаче остаются
// прежними: карта та же, а с "нет карты" слой storage перестал бы к ней
// обращаться до перезагрузки платы.
bool reinit_card() {
    // Разностью: момент у оборота 2^32 мс не блокирует карту надолго.
    if (s_reinit_tried && platform::mono_ms() - s_last_reinit_ms < s_reinit_interval_ms) {
        ++s_card_stats.window_rejects;
        return false;
    }
    s_reinit_tried = true;
    ++s_card_stats.reinit;
    SdInfo info;
    const bool ok    = card_handshake(info);
    s_last_reinit_ms = platform::mono_ms();
    if (ok) {
        s_info = info;
        // Снимок носителя - событием, а не опросом: горячий путь читает
        // только его, и он обязан гаснуть не позже настоящего признака.
        hal::media_state_publish(hal::Medium::Card);
        s_need_reinit        = false;
        s_reinit_interval_ms = kReinitIntervalMs;
        return true;
    }
    ++s_card_stats.reinit_failed;
    if (s_reinit_interval_ms < kReinitIntervalMaxMs) s_reinit_interval_ms *= 2u;
    return false;
}

// Остановить многоблочное чтение. В режиме SPI у CMD12 перед R1 приходит
// байт-заполнитель, его глотает wait_r1 внутри send_cmd; дальше карта
// держит занятость, пока не закончит, и снимать выбор до этого нельзя -
// следующая команда придёт на занятую карту.
void stop_transmission() {
    send_cmd(sd::kCmd12, 0);
    wait_not_busy(kBusyTimeoutMs);
}

// Блоки одной командой: CMD17 - один, CMD18 - count подряд и CMD12 в
// конце. CRC-16/XMODEM проверяется у каждого блока: без неё битый блок
// уходит хосту молча (при 2 тактах clk_peri на бит - около 1% секторов).
// Первый битый или непришедший блок бросает чтение. Битый блок CMD17 -
// попытка (crc_retry, повтор у read_once), CMD18 - отказ пачки (crc).
ReadResult SOUNDSINTH_HOT_PATH(read_blocks)(uint8_t cmd, uint32_t lba, uint32_t count, uint8_t* dst) {
    cs_low();
    const uint8_t r1 = send_cmd(cmd, block_arg(lba));
    if (r1 != sd::kR1Ready) {
        cs_high();
        ++s_card_stats.r1;
        s_card_stats.last_r1  = r1;
        s_card_stats.last_lba = lba;
        return ReadResult::Lost;
    }
    ReadResult r = ReadResult::Ok;
    for (uint32_t i = 0; i < count && r == ReadResult::Ok; ++i) {
        uint8_t* const p = dst + i * sd::kBlockBytes;
        if (!wait_data_token(kTokenTimeoutMs)) {
            ++s_card_stats.token;
            s_card_stats.last_lba = lba + i;
            r                     = ReadResult::Lost;
            break;
        }
        // Одним обменом: блочное чтение держит на выходе 0xFF.
        hal::spi_read(sd::kIdleByte, p, sd::kBlockBytes);
        const uint8_t hi = xfer(sd::kIdleByte);
        const uint8_t lo = xfer(sd::kIdleByte);
        if (sd::crc16(p, sd::kBlockBytes) == static_cast<uint16_t>((hi << 8) | lo)) continue;
        r = ReadResult::BadCrc;
        if (cmd == sd::kCmd17) {
            ++s_card_stats.crc_retry;
        } else {
            ++s_card_stats.crc;
            s_card_stats.last_lba = lba + i;
        }
    }
    if (cmd == sd::kCmd18) stop_transmission();
    cs_high();
    return r;
}

// Одиночное чтение - до трёх попыток: ошибка CRC случайная, повтор её
// снимает. Не снял - неудача: заведомо испорченный сектор хуже никакого.
// Пачка не повторяется: вызывающий перечитает по одному и разберётся,
// какой сектор испорчен.
ReadResult SOUNDSINTH_HOT_PATH(read_once)(uint8_t cmd, uint32_t lba, uint32_t count, uint8_t* dst) {
    if (cmd == sd::kCmd18) return read_blocks(cmd, lba, count, dst);
    for (uint32_t attempt = 0; attempt < kCrcAttempts; ++attempt) {
        const ReadResult r = read_blocks(cmd, lba, 1, dst);
        if (r != ReadResult::BadCrc) return r;
    }
    ++s_card_stats.crc;
    s_card_stats.last_lba = lba;
    return ReadResult::BadCrc;
}

// Карта не приняла команду или не прислала данные: одна
// переинициализация и повтор того же чтения.
bool SOUNDSINTH_HOT_PATH(read_recovering)(uint8_t cmd, uint32_t lba, uint32_t count, uint8_t* dst) {
    if (s_need_reinit && !reinit_card()) return false;
    const ReadResult r = read_once(cmd, lba, count, dst);
    if (r != ReadResult::Lost) return r == ReadResult::Ok;
    s_need_reinit = true;
    return reinit_card() && read_once(cmd, lba, count, dst) == ReadResult::Ok;
}

// Чтение под защитой транзакции, со счётом времени. Обслуживание хоста из
// ожидания карты может само попроситься к карте: команда посреди команды
// при опущенном CS сломала бы обе. Пачка считается одним чтением: среднее
// тогда прямо сравнимо с ценой команды, ради которой она и заведена.
SOUNDSINTH_NOINLINE bool SOUNDSINTH_HOT_PATH(read_timed)(uint8_t cmd, uint32_t lba, uint32_t count, uint8_t* dst) {
    if (!s_info.present || dst == nullptr || s_in_transaction) return false;
    s_in_transaction  = true;
    const uint32_t t0 = platform::mono_us();
    const bool ok     = read_recovering(cmd, lba, count, dst);
    const uint32_t dt = platform::mono_us() - t0;
    if (dt > s_card_stats.read_max_us) s_card_stats.read_max_us = dt;
    if (dt > kSlowReadUs) ++s_card_stats.slow_reads;
    ++s_card_stats.reads;
    s_card_stats.read_total_us += dt;
    s_in_transaction            = false;
    return ok;
}

bool SOUNDSINTH_HOT_PATH(write_sector_locked)(uint32_t lba, const uint8_t* src) {
    // Писать в карту, вышедшую из рабочего режима, бессмысленно: сначала
    // вернуть её (попытки не чаще интервала переинициализации).
    if (s_need_reinit && !reinit_card()) return false;

    cs_low();
    if (send_cmd(sd::kCmd24, block_arg(lba)) != sd::kR1Ready) {
        cs_high();
        ++s_card_stats.write_r1;
        return false;
    }

    xfer(sd::kIdleByte); // пустой такт перед токеном, требование спецификации
    xfer(sd::kTokenStartBlock);
    hal::spi_write(src, sd::kBlockBytes);
    clock_crc(); // карта его не проверяет, пока CRC не включён

    // Ответ на данные: младшие пять бит, 0b00101 - принято.
    const uint8_t resp = static_cast<uint8_t>(xfer(sd::kIdleByte) & sd::kDataRespMask);
    if (resp != sd::kDataRespAccepted) {
        cs_high();
        ++s_card_stats.write_data;
        return false;
    }

    // Конец физической записи - до снятия CS, иначе следующая команда придёт
    // на занятую карту.
    const bool ok = wait_not_busy(kBusyTimeoutMs);
    cs_high();
    if (!ok) ++s_card_stats.write_busy;
    return ok;
}

// Запись сектора с учётом времени - тем же видом, что read_timed.
bool SOUNDSINTH_HOT_PATH(write_timed)(uint32_t lba, const uint8_t* src) {
    if (!s_info.present || src == nullptr || s_in_transaction) return false;
    s_in_transaction  = true;
    const uint32_t t0 = platform::mono_us();
    const bool ok     = write_sector_locked(lba, src);
    const uint32_t dt = platform::mono_us() - t0;
    ++s_card_stats.writes;
    if (dt > s_card_stats.write_max_us) s_card_stats.write_max_us = dt;
    s_card_stats.write_total_us += dt;
    s_in_transaction             = false;
    return ok;
}

} // namespace

// --- Публичное ---

bool sd_card_init() {
    if (s_in_transaction) return false;
    s_in_transaction = true;
    SdInfo info;
    const bool ok    = card_handshake(info);
    s_info           = info;
    s_in_transaction = false;
    return ok;
}

const SdInfo& sd_card_info() {
    return s_info;
}

uint32_t sd_card_set_speed(uint32_t hz) {
    s_work_req = hz;
    if (!s_info.present || s_in_transaction) return 0;
    s_in_transaction   = true;
    const uint32_t khz = apply_speed();
    s_in_transaction   = false;
    return khz * 1000u;
}

uint32_t sd_card_faults() {
    const SdCardStats& s = s_card_stats;
    return s.r1 + s.token + s.crc + s.crc_retry + s.reinit;
}

void sd_card_log_handshake() {
    const HandshakeReport hs = s_hs;
    char m[192];
    if (hs.step == HandshakeStep::Ok) {
        std::snprintf(m, sizeof(m),
                      "sd: %s%s, sectors %" PRIu32 " (%" PRIu32 " MB), SCK %" PRIu32 " kHz at init -> %" PRIu32 " kHz, OCR %08" PRIX32 ", init %" PRIu32
                      " ms\n",
                      (hs.kind == SdKind::Sdhc) ? "SDHC" : "SDSC", hs.mmc ? " (MMC)" : "", hs.sectors, hs.sectors / 2048u, hs.init_khz, hs.work_khz,
                      hs.volt_window, hs.acmd41_ms);
        debug_log(m);
        return;
    }
    // Отказ: шаг, на котором карта замолчала, и числа этого шага. Уровень
    // MISO до первой команды отличает непропай от ошибки протокола.
    const char* why = "";
    switch (hs.step) {
        case HandshakeStep::Cmd0:
            why = (hs.miso_idle == 0xff)   ? "CMD0 gave no idle -- line free, no card or not connected"
                  : (hs.miso_idle == 0x00) ? "CMD0 gave no idle -- line pulled down, power, CS or soldering"
                                           : "CMD0 gave no idle -- card answers, but wrongly";
            break;
        case HandshakeStep::Cmd8:
            why = "CMD8 echoed something else -- voltage not supported";
            break;
        case HandshakeStep::LeaveIdle:
            why = "card did not leave idle";
            break;
        case HandshakeStep::Ocr:
            why = "CMD58 (OCR) failed";
            break;
        case HandshakeStep::BlockLen:
            why = "CMD16 (block length 512) failed";
            break;
        case HandshakeStep::Csd:
            why = "CMD9 (CSD) returned no block";
            break;
        case HandshakeStep::Ok:
            break;
    }
    // Причина и числа шага - двумя строками: одной строкой худший случай
    // длиннее m, и обрезанная строка теряет перевод.
    std::snprintf(m, sizeof(m), "sd: %s\n", why);
    debug_log(m);
    std::snprintf(m, sizeof(m),
                  "sd:   MISO at rest 0x%02X, SCK %" PRIu32 " kHz, CMD0 0x%02X, CMD8 0x%02X, version %s, CMD58 "
                  "0x%02X, OCR %08" PRIX32 ", last ACMD41/CMD1 0x%02X after %" PRIu32 " ms\n",
                  hs.miso_idle, hs.init_khz, hs.r0, hs.r8, hs.v2 ? "v2" : "v1", hs.r58, hs.volt_window, hs.last_r41, hs.acmd41_ms);
    debug_log(m);
}

void sd_card_set_wait_service(hal::WaitServiceFn fn, void* user) {
    s_wait_service      = fn;
    s_wait_service_user = user;
}

void sd_card_log_health() {
    // Прошлый период - только поля приростов, не весь SdCardStats.
    static struct {
        uint32_t reinit, r1, token, crc_retry;
    } last{};
    const SdCardStats re = s_card_stats;
    char sm[320];
    std::snprintf(sm, sizeof(sm),
                  "sd: media: reinits %" PRIu32 " (+%" PRIu32 "), failed %" PRIu32 " | r1 errors %" PRIu32 " (+%" PRIu32 ", last %02X), token %" PRIu32
                  " (+%" PRIu32 "), crc %" PRIu32 " | bad crc %" PRIu32 " (+%" PRIu32 ") | last failed sector %" PRIu32 "\n",
                  re.reinit, re.reinit - last.reinit, re.reinit_failed, re.r1, re.r1 - last.r1, re.last_r1, re.token, re.token - last.token, re.crc,
                  re.crc_retry, re.crc_retry - last.crc_retry, re.last_lba);
    debug_log(sm);
    std::snprintf(sm, sizeof(sm),
                  "sd: sector read: total %" PRIu32 ", max %" PRIu32 " us, sum %" PRIu32 " ms, over 100 ms %" PRIu32 ", rejected by reinit window %" PRIu32
                  "\n",
                  re.reads, re.read_max_us, static_cast<uint32_t>(re.read_total_us / 1000u), re.slow_reads, re.window_rejects);
    debug_log(sm);
    if (re.writes != 0) {
        std::snprintf(sm, sizeof(sm),
                      "sd: sector write: total %" PRIu32 ", max %" PRIu32 " us, sum %" PRIu32 " ms | failed: write r1 %" PRIu32 ", data %" PRIu32
                      ", busy %" PRIu32 "\n",
                      re.writes, re.write_max_us, static_cast<uint32_t>(re.write_total_us / 1000u), re.write_r1, re.write_data, re.write_busy);
        debug_log(sm);
    }
    // Была переинициализация - её итог.
    if (re.reinit != last.reinit) sd_card_log_handshake();
    last = {re.reinit, re.r1, re.token, re.crc_retry};
}

bool SOUNDSINTH_HOT_PATH(sd_card_read_sector)(uint32_t lba, uint8_t* dst) {
    return read_timed(sd::kCmd17, lba, 1, dst);
}

bool SOUNDSINTH_HOT_PATH(sd_card_read_run)(uint32_t lba, uint32_t count, uint8_t* dst) {
    return count != 0 && read_timed(sd::kCmd18, lba, count, dst);
}

bool SOUNDSINTH_HOT_PATH(sd_card_write_sector)(uint32_t lba, const uint8_t* src) {
    return write_timed(lba, src);
}

} // namespace devices::sd
