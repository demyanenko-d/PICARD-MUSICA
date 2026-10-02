// SPDX-License-Identifier: MIT
// Протокол карты SD/SDHC по SPI. Железа здесь нет: шина
// приходит из порта.

#include "devices/sd/card_protocol.h"

#include <cinttypes>
#include <cstdio>

#include "devices/hal/media.h"
#include "devices/hal/spi.h"
#include "devices/sd/sd_protocol.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

namespace devices::sd {
namespace {

namespace sd = devices::sd;

namespace hal = devices::hal;

// До инициализации спецификация разрешает 100-400 кГц, здесь верхняя
// граница; фактическую возвращает порт и она печатается в лог.
constexpr uint32_t kBaudInit = SOUNDSINTH_SD_BAUD_INIT;
constexpr uint32_t kBaudWork = SOUNDSINTH_SD_BAUD_WORK; // стандартная скорость SD; понизить, если провода длинные

// --- Сведения о карте ---

SdInfo s_info;

// --- Итог рукопожатия ---
//
// Рукопожатие само ничего не печатает, а складывает числа сюда; строки
// собирает sd_card_log_handshake. Печать стоит на этом пути дороже всего:
// рукопожатие идёт и при переинициализации, то есть с Core1 под
// загрузчиком, а буфер строки с цепочкой snprintf занимают там 640 байт
// стека - больше всего остального в этой области.
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

// --- Обслуживание хоста, пока драйвер ждёт карту ---
//
// Ждать приходится долго: опрос ACMD41 и CMD1 при инициализации до 4 с,
// токен данных 200 мс, конец записи 500 мс. Всё это время протокол хоста
// не обслуживается, а его кольцо команд держит два кадра.
void (*s_wait_service)(void*) = nullptr;
void* s_wait_service_user     = nullptr;
// Драйвер внутри своей транзакции: вложенные чтение и запись сектора
// отказывают, иначе новая команда ушла бы посреди чужой при опущенном CS.
bool s_in_transaction = false;

// Зовётся из циклов ожидания, не чаще этого срока. Время - в микросекундах
// 32 бит: опрос токена крутится на каждом читаемом секторе, а миллисекунды
// стоили бы там деления 64-битного времени. Сам срок - в заголовке, его
// сверяет тест.
constexpr uint32_t kServicePeriodUs = kCardServicePeriodUs;
// Пауза, которая отдаёт время шине: занятое ожидание кусками по сроку
// обслуживания. Прямой busy_wait на десять миллисекунд оставлял шину без
// обслуживания целиком, а таких пауз за проход рукопожатия до трёхсот.
void serve_during_pause(uint32_t total_us, uint32_t& last_service_us);

void SOUNDSINTH_HOT_PATH(serve_while_waiting)(uint32_t& last_service_us) {
    if (s_wait_service == nullptr) return;
    const uint32_t now = platform::mono_us();
    if (now - last_service_us < kServicePeriodUs) return;
    last_service_us = now;
    s_wait_service(s_wait_service_user);
}

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

// Ответ R1 приходит не сразу: до восьми байт линия в 0xFF.
uint8_t SOUNDSINTH_HOT_PATH(wait_r1)() {
    for (uint8_t i = 0; i < 10; ++i) {
        const uint8_t r = xfer(sd::kIdleByte);
        if ((r & sd::kR1StartBit) == 0) return r;
    }
    return sd::kIdleByte; // не ответила
}

uint8_t SOUNDSINTH_HOT_PATH(send_cmd)(uint8_t cmd, uint32_t arg) {
    uint8_t frame[sd::kCmdFrameBytes];
    frame[0] = static_cast<uint8_t>(sd::kCmdStartBits | cmd);
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

// Адрес в командах чтения и записи: у SDHC номер сектора, у SDSC байты.
uint32_t block_arg(uint32_t lba) {
    return (s_info.kind == SdKind::Sdhc) ? lba : (lba * sd::kBlockBytes);
}

// Выводы и блок SPI заново (и при переинициализации - как при старте),
// затем разговор с картой: CMD0 ... CSD и рабочая частота. Сведения - в
// out, s_info не трогается: при неудачной переинициализации остаются
// прежние.
bool card_handshake(SdInfo& out) {
    out  = SdInfo{};
    s_hs = HandshakeReport{};
    // Рукопожатие идёт под живой шиной: отметка нужна с первой же паузы.
    uint32_t last_service_us = platform::mono_us();

    // Не меньше 74 тактов при снятом CS - так карта переходит в SPI-режим.
    //
    // Частота - фактическая, которую вернул SDK: делитель SPI целый и чётный.
    // В режиме идентификации нужно 100-400 кГц; промах дал бы неотвечающую
    // карту, по виду неотличимую от обрыва.
    const uint32_t init_hz = hal::spi_open(kBaudInit);
    s_hs.init_khz          = init_hz / 1000u;
    hal::spi_select(false);
    for (uint8_t i = 0; i < 10; ++i) {
        xfer(sd::kIdleByte);
    }

    // Уровень MISO до первой команды: 0xFF без карты (подтяжка держит
    // единицу), 0x00 - линия посажена, иное - карта отвечает не тем.
    // Значение идёт в сообщение об отказе: по нему непропай отличается от
    // беды с протоколом.
    uint8_t miso_idle = 0xff;
    for (uint8_t i = 0; i < 8; ++i) {
        miso_idle = xfer(sd::kIdleByte);
    }
    s_hs.miso_idle = miso_idle;

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
    uint8_t r0 = 0xff;
    for (uint8_t attempt = 0; attempt < 8; ++attempt) {
        cs_high();
        // 10 байт на первой попытке, дальше вдвое больше, потолок 640.
        const uint16_t flush = static_cast<uint16_t>(10u << (attempt < 6u ? attempt : 6u));
        for (uint16_t i = 0; i < flush; ++i) {
            xfer(sd::kIdleByte);
            // Самая длинная порция - 640 байт, на 400 кГц это 12.8 мс в одном
            // куске, и всё это время шину не обслуживает никто.
            if ((i & 0x1fu) == 0x1fu) serve_while_waiting(last_service_us);
        }
        cs_low();
        r0 = send_cmd(sd::kCmd0, 0);
        if (r0 == sd::kR1Idle) break;
    }
    s_hs.r0 = r0;
    if (r0 != sd::kR1Idle) {
        cs_high();
        s_hs.step = HandshakeStep::Cmd0;
        return false;
    }

    // CMD8 отделяет карты v2 от v1: v1 отвечает "нет такой команды".
    bool v2          = false;
    const uint8_t r8 = send_cmd(sd::kCmd8, sd::kIfCondArg);
    s_hs.r8          = r8;
    if ((r8 & sd::kR1IllegalCmd) == 0) {
        uint8_t tail[4];
        for (uint8_t i = 0; i < 4; ++i) {
            tail[i] = xfer(sd::kIdleByte);
        }
        if (tail[2] != sd::kIfCondVoltage || tail[3] != sd::kIfCondPattern) {
            cs_high();
            s_hs.step = HandshakeStep::Cmd8;
            return false;
        }
        v2 = true;
    }
    s_hs.v2 = v2;

    // ACMD41 до выхода из idle - оба варианта HCS.
    //
    // HCS сообщает карте, что хост умеет высокую ёмкость; ставится картам
    // v2 по ответу на CMD8. Если карта объявила CMD8 недопустимой, хост
    // считает её v1 и HCS не ставит, а SDHC без заявленной поддержки высокой
    // ёмкости из idle не выходит никогда: бесконечный ACMD41 (так ведёт себя
    // 4-гигабайтная карта). Поэтому сначала вариант по CMD8, потом
    // противоположный: ошибка в версии стоит секунды на старте, а не отказа.
    const uint32_t acmd41_t0_us = platform::mono_us();
    bool hcs                    = v2;
    uint8_t last_r41            = 0xff;
    bool acmd41_ok              = false;
    // OCR прямо в idle, до инициализации (CMD58 в idle разрешён). Осмысленное
    // окно напряжений (обычно 0x00FF8000, 2.7-3.6 В) - шина в порядке и карта
    // жива; мусор или нули - проблема в обмене, а не в инициализации.
    uint32_t volt_window = 0;
    {
        uint8_t ocr_idle[4] = {0, 0, 0, 0};
        const uint8_t r58   = send_cmd(sd::kCmd58, 0);
        if (r58 <= sd::kR1Idle) {
            for (uint8_t i = 0; i < 4; ++i) {
                ocr_idle[i] = xfer(sd::kIdleByte);
            }
        }
        // Младшие три байта OCR - окно напряжений, ниже возвращается карте в
        // аргументе ACMD41/CMD1.
        volt_window      = (static_cast<uint32_t>(ocr_idle[1]) << 16) | (static_cast<uint32_t>(ocr_idle[2]) << 8) | static_cast<uint32_t>(ocr_idle[3]);
        s_hs.r58         = r58;
        s_hs.volt_window = (static_cast<uint32_t>(ocr_idle[0]) << 24) | volt_window;
    }

    // Пауза между опросами обязательна: карта отвечает 0x01 мгновенно, без
    // задержки она получает тысячи команд в секунду, и часть карт от такого
    // темпа не продвигает внутреннюю инициализацию. Исправная карта выходит
    // из idle за десятки шагов по 10 мс.
    last_service_us = platform::mono_us();
    for (uint8_t pass = 0; pass < 2 && !acmd41_ok; ++pass) {
        if (pass != 0) hcs = !hcs;
        const uint32_t pass_t0 = platform::mono_us();
        for (;;) {
            send_cmd(sd::kCmd55, 0);
            // Окно напряжений в аргументе, а не только HCS. Спецификация
            // разрешает нулевое окно, но часть карт на нём не встаёт; берётся
            // окно, которое карта сама сообщила в OCR.
            last_r41 = send_cmd(sd::kAcmd41, (hcs ? sd::kOpCondHcsBits : 0u) | volt_window);
            if (last_r41 == sd::kR1Ready) {
                acmd41_ok = true;
                break;
            }
            if (platform::mono_us() - pass_t0 >= 1500u * 1000u) break;
            serve_during_pause(10u * 1000u, last_service_us);
        }
    }

    // Запасной путь - MMC: внешне как SD, но ACMD41 не понимает и остаётся
    // в idle. Сперва ACMD41, при неудаче CMD1.
    bool mmc = false;
    if (!acmd41_ok) {
        const uint32_t mmc_t0 = platform::mono_us();
        for (;;) {
            last_r41 = send_cmd(sd::kCmd1, sd::kOpCondHcsBits | volt_window);
            if (last_r41 == sd::kR1Ready) {
                acmd41_ok = true;
                mmc       = true;
                break;
            }
            if (platform::mono_us() - mmc_t0 >= 1000u * 1000u) break;
            serve_during_pause(10u * 1000u, last_service_us);
        }
    }
    const uint32_t acmd41_ms = (platform::mono_us() - acmd41_t0_us) / 1000u;
    s_hs.acmd41_ms           = acmd41_ms;
    s_hs.last_r41            = last_r41;
    s_hs.mmc                 = mmc;
    if (!acmd41_ok) {
        cs_high();
        s_hs.step = HandshakeStep::LeaveIdle;
        return false;
    }

    // Ёмкость по OCR всегда, а не только у v2: версия могла определиться
    // неверно, а CCS в OCR - ответ самой карты.
    out.kind = SdKind::Sdsc;
    if (send_cmd(sd::kCmd58, 0) == sd::kR1Ready) {
        uint8_t ocr[4];
        for (uint8_t i = 0; i < 4; ++i) {
            ocr[i] = xfer(sd::kIdleByte);
        }
        if (ocr[0] & sd::kOcrCcsBits) out.kind = SdKind::Sdhc;
    } else if (v2) {
        cs_high();
        s_hs.step = HandshakeStep::Ocr;
        return false;
    }

    if (out.kind == SdKind::Sdsc && send_cmd(sd::kCmd16, sd::kBlockBytes) != sd::kR1Ready) {
        cs_high();
        s_hs.step = HandshakeStep::BlockLen;
        return false;
    }

    if (send_cmd(sd::kCmd9, 0) != sd::kR1Ready || !wait_data_token(200)) {
        cs_high();
        s_hs.step = HandshakeStep::Csd;
        return false;
    }
    uint8_t csd[sd::kRegisterBytes];
    for (uint32_t i = 0; i < sd::kRegisterBytes; ++i) {
        csd[i] = xfer(sd::kIdleByte);
    }
    xfer(sd::kIdleByte); // CRC16 блока CSD в SPI не проверяется
    xfer(sd::kIdleByte);
    cs_high();

    out.sector_count       = sectors_from_csd(csd);
    out.present            = true;
    const uint32_t work_hz = hal::spi_set_hz(kBaudWork);

    s_hs.step     = HandshakeStep::Ok;
    s_hs.kind     = out.kind;
    s_hs.sectors  = out.sector_count;
    s_hs.work_khz = work_hz / 1000u;
    return true;
}

// Сбои чтения носителя по причинам.
struct SdReadErrors {
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
};
constexpr uint32_t kSlowReadUs = SOUNDSINTH_SD_SLOW_READ_US;
SdReadErrors s_read_err;

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
    if (s_reinit_tried && (platform::mono_us() / 1000u) - s_last_reinit_ms < s_reinit_interval_ms) {
        ++s_read_err.window_rejects;
        return false;
    }
    s_reinit_tried = true;
    ++s_read_err.reinit;
    SdInfo info;
    const bool ok    = card_handshake(info);
    s_last_reinit_ms = platform::mono_us() / 1000u;
    if (ok) {
        s_info = info;
        // Снимок носителя - событием, а не опросом: горячий путь читает
        // только его, и он обязан гаснуть не позже настоящего признака.
        devices::hal::media_state_publish(devices::hal::Medium::Card);
        s_need_reinit        = false;
        s_reinit_interval_ms = kReinitIntervalMs;
        return true;
    }
    ++s_read_err.reinit_failed;
    if (s_reinit_interval_ms < kReinitIntervalMaxMs) s_reinit_interval_ms *= 2u;
    return false;
}

ReadResult SOUNDSINTH_HOT_PATH(read_sector_once)(uint32_t lba, uint8_t* dst) {
    // Чтение с проверкой CRC-16/XMODEM: без неё битая середина блока уходит
    // хосту молча (при 24 МГц от clk_peri 48 МГц - около 1% секторов).
    //
    // Три попытки: ошибка случайная, повтор её снимает. Не снял - неудача:
    // заведомо испорченный сектор хуже никакого.
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        cs_low();
        const uint8_t r1 = send_cmd(sd::kCmd17, block_arg(lba));
        if (r1 != sd::kR1Ready) {
            cs_high();
            ++s_read_err.r1;
            s_read_err.last_r1  = r1;
            s_read_err.last_lba = lba;
            return ReadResult::Lost;
        }
        if (!wait_data_token(200)) {
            cs_high();
            ++s_read_err.token;
            s_read_err.last_lba = lba;
            return ReadResult::Lost;
        }
        // Одним обменом: блочное чтение держит на выходе 0xFF.
        hal::spi_read(sd::kIdleByte, dst, sd::kBlockBytes);
        const uint8_t hi = xfer(sd::kIdleByte);
        const uint8_t lo = xfer(sd::kIdleByte);
        cs_high();

        const uint16_t want = static_cast<uint16_t>((hi << 8) | lo);
        if (sd::crc16(dst, sd::kBlockBytes) == want) return ReadResult::Ok;
        ++s_read_err.crc_retry;
    }
    ++s_read_err.crc;
    s_read_err.last_lba = lba;
    return ReadResult::BadCrc;
}

// Остановить многоблочное чтение. В режиме SPI у CMD12 перед R1 приходит
// байт-заполнитель, его глотает wait_r1 внутри send_cmd; дальше карта
// держит занятость, пока не закончит, и снимать выбор до этого нельзя -
// следующая команда придёт на занятую карту.
void stop_transmission() {
    send_cmd(sd::kCmd12, 0);
    wait_not_busy(500);
}

// Пачка подряд идущих секторов одной командой CMD18. Экономится не
// передача, а обвязка: команда, ожидание готовности и разрыв выбора на
// каждом секторе. У одиночного чтения они и составляют большую часть
// цены - 512 байт на 25 МГц идут 164 мкс, а сектор стоит около 1060.
//
// CRC проверяется у каждого блока. Испортился любой - пачка бросается
// целиком: вызывающий перечитает по одному и разберётся, какой именно.
ReadResult SOUNDSINTH_HOT_PATH(read_run_once)(uint32_t lba, uint32_t count, uint8_t* dst) {
    cs_low();
    const uint8_t r1 = send_cmd(sd::kCmd18, block_arg(lba));
    if (r1 != sd::kR1Ready) {
        cs_high();
        ++s_read_err.r1;
        s_read_err.last_r1  = r1;
        s_read_err.last_lba = lba;
        return ReadResult::Lost;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!wait_data_token(200)) {
            stop_transmission();
            cs_high();
            ++s_read_err.token;
            s_read_err.last_lba = lba + i;
            return ReadResult::Lost;
        }
        uint8_t* const p = dst + i * sd::kBlockBytes;
        hal::spi_read(sd::kIdleByte, p, sd::kBlockBytes);
        const uint8_t hi = xfer(sd::kIdleByte);
        const uint8_t lo = xfer(sd::kIdleByte);
        if (sd::crc16(p, sd::kBlockBytes) != static_cast<uint16_t>((hi << 8) | lo)) {
            stop_transmission();
            cs_high();
            ++s_read_err.crc;
            s_read_err.last_lba = lba + i;
            return ReadResult::BadCrc;
        }
    }
    stop_transmission();
    cs_high();
    return ReadResult::Ok;
}

bool SOUNDSINTH_HOT_PATH(read_sector_recovering)(uint32_t lba, uint8_t* dst) {
    if (s_need_reinit && !reinit_card()) return false;
    const ReadResult r = read_sector_once(lba, dst);
    if (r == ReadResult::Ok) return true;
    if (r == ReadResult::BadCrc) return false;
    // Карта не приняла команду или не прислала данные: одна
    // переинициализация и повтор того же сектора.
    s_need_reinit = true;
    if (!reinit_card()) return false;
    return read_sector_once(lba, dst) == ReadResult::Ok;
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
    // MISO до первой команды отличает непропай от беды с протоколом.
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
    std::snprintf(m, sizeof(m),
                  "sd: %s (MISO at rest 0x%02X, SCK %" PRIu32 " kHz, CMD0 0x%02X, CMD8 0x%02X, version %s, CMD58 "
                  "0x%02X, OCR %08" PRIX32 ", last ACMD41/CMD1 0x%02X after %" PRIu32 " ms)\n",
                  why, hs.miso_idle, hs.init_khz, hs.r0, hs.r8, hs.v2 ? "v2" : "v1", hs.r58, hs.volt_window, hs.last_r41, hs.acmd41_ms);
    debug_log(m);
}

void sd_card_set_wait_service(void (*fn)(void* user), void* user) {
    s_wait_service      = fn;
    s_wait_service_user = user;
}

void sd_card_log_health() {
    static SdReadErrors last{};
    const SdReadErrors re = s_read_err;
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
        std::snprintf(sm, sizeof(sm), "sd: sector write: total %" PRIu32 ", max %" PRIu32 " us, sum %" PRIu32 " ms\n", re.writes, re.write_max_us,
                      static_cast<uint32_t>(re.write_total_us / 1000u));
        debug_log(sm);
    }
    // Была переинициализация - её итог. Рукопожатие само не печатает: оно
    // идёт с Core1 под загрузчиком, на пике его стека.
    if (re.reinit != last.reinit) sd_card_log_handshake();
    last = re;
}

bool SOUNDSINTH_HOT_PATH(sd_card_read_sector)(uint32_t lba, uint8_t* dst) {
    if (!s_info.present || dst == nullptr) return false;
    // Обслуживание хоста из ожидания карты может само попроситься к карте:
    // команда посреди команды при опущенном CS сломала бы обе.
    if (s_in_transaction) return false;
    s_in_transaction  = true;
    const uint32_t t0 = platform::mono_us();
    const bool ok     = read_sector_recovering(lba, dst);
    const uint32_t dt = platform::mono_us() - t0;
    if (dt > s_read_err.read_max_us) s_read_err.read_max_us = dt;
    if (dt > kSlowReadUs) ++s_read_err.slow_reads;
    ++s_read_err.reads;
    s_read_err.read_total_us += dt;
    s_in_transaction          = false;
    return ok;
}

bool SOUNDSINTH_HOT_PATH(sd_card_read_run)(uint32_t lba, uint32_t count, uint8_t* dst) {
    if (!s_info.present || dst == nullptr || count == 0) return false;
    if (s_in_transaction) return false;
    s_in_transaction  = true;
    const uint32_t t0 = platform::mono_us();
    bool ok           = false;
    if (!s_need_reinit || reinit_card()) {
        ReadResult r = read_run_once(lba, count, dst);
        if (r != ReadResult::Ok && r != ReadResult::BadCrc) {
            // Карта не приняла команду или не прислала данные: одна
            // переинициализация и повтор той же пачки, как у одиночного.
            s_need_reinit = true;
            if (reinit_card()) r = read_run_once(lba, count, dst);
        }
        ok = r == ReadResult::Ok;
    }
    const uint32_t dt = platform::mono_us() - t0;
    if (dt > s_read_err.read_max_us) s_read_err.read_max_us = dt;
    if (dt > kSlowReadUs) ++s_read_err.slow_reads;
    // Считается пачка как одно чтение: среднее тогда прямо сравнимо с
    // ценой команды, ради которой она и заведена.
    ++s_read_err.reads;
    s_read_err.read_total_us += dt;
    s_in_transaction          = false;
    return ok;
}

namespace {
bool SOUNDSINTH_HOT_PATH(write_sector_locked)(uint32_t lba, const uint8_t* src) {
    // Писать в карту, вышедшую из рабочего режима, бессмысленно: сначала
    // вернуть её (попытки не чаще интервала переинициализации).
    if (s_need_reinit && !reinit_card()) return false;

    cs_low();
    if (send_cmd(sd::kCmd24, block_arg(lba)) != sd::kR1Ready) {
        cs_high();
        return false;
    }

    xfer(sd::kIdleByte); // пустой такт перед токеном, требование спецификации
    xfer(sd::kTokenStartBlock);
    hal::spi_write(src, sd::kBlockBytes);
    xfer(sd::kIdleByte); // CRC16: карта его не проверяет, пока CRC не включён
    xfer(sd::kIdleByte);

    // Ответ на данные: младшие пять бит, 0b00101 - принято.
    const uint8_t resp = static_cast<uint8_t>(xfer(sd::kIdleByte) & sd::kDataRespMask);
    if (resp != sd::kDataRespAccepted) {
        cs_high();
        return false;
    }

    // Конец физической записи - до снятия CS, иначе следующая команда придёт
    // на занятую карту.
    const bool ok = wait_not_busy(500);
    cs_high();
    return ok;
}
} // namespace

bool SOUNDSINTH_HOT_PATH(sd_card_write_sector)(uint32_t lba, const uint8_t* src) {
    if (!s_info.present || src == nullptr) return false;
    if (s_in_transaction) return false;
    s_in_transaction  = true;
    const uint32_t t0 = platform::mono_us();
    const bool ok     = write_sector_locked(lba, src);
    const uint32_t dt = platform::mono_us() - t0;
    ++s_read_err.writes;
    if (dt > s_read_err.write_max_us) s_read_err.write_max_us = dt;
    s_read_err.write_total_us += dt;
    s_in_transaction           = false;
    return ok;
}

} // namespace devices::sd
