#pragma once

// Протокол SD-карты в режиме SPI: номера команд, ответы, токены и CRC.
// Общий для драйвера настоящей карты и эмулятора карты: обе стороны
// обязаны считать одно и то же.
//
// Платформонезависим: только constexpr.

#include <cstdint>

namespace devices::sd {

// --- Команды (номер из спецификации - в имени, как в строках лога) ---

inline constexpr uint8_t kCmd0 = 0;    // GO_IDLE_STATE
inline constexpr uint8_t kCmd1 = 1;    // SEND_OP_COND: инициализация MMC
inline constexpr uint8_t kCmd8 = 8;    // SEND_IF_COND
inline constexpr uint8_t kCmd9 = 9;    // SEND_CSD
inline constexpr uint8_t kCmd10 = 10;  // SEND_CID
inline constexpr uint8_t kCmd12 = 12;  // STOP_TRANSMISSION
inline constexpr uint8_t kCmd13 = 13;  // SEND_STATUS
inline constexpr uint8_t kCmd16 = 16;  // SET_BLOCKLEN
inline constexpr uint8_t kCmd17 = 17;  // READ_SINGLE_BLOCK
inline constexpr uint8_t kCmd18 = 18;  // READ_MULTIPLE_BLOCK
inline constexpr uint8_t kCmd24 = 24;  // WRITE_BLOCK
inline constexpr uint8_t kCmd25 = 25;  // WRITE_MULTIPLE_BLOCK
inline constexpr uint8_t kCmd55 = 55;  // APP_CMD
inline constexpr uint8_t kCmd58 = 58;  // READ_OCR
inline constexpr uint8_t kCmd59 = 59;  // CRC_ON_OFF
inline constexpr uint8_t kAcmd41 = 41; // SD_SEND_OP_COND

// Первый байт кадра команды: старшие биты 01, дальше номер.
inline constexpr uint8_t kCmdStartMask = 0xc0;
inline constexpr uint8_t kCmdStartBits = 0x40;

// --- Размеры ---

inline constexpr uint32_t kCmdFrameBytes = 6;  // номер, четыре байта аргумента, CRC7
inline constexpr uint32_t kRegisterBytes = 16; // CSD и CID
inline constexpr uint32_t kBlockBytes = 512;   // блок данных чтения и записи

// --- Ответы ---

inline constexpr uint8_t kR1Ready = 0x00;
inline constexpr uint8_t kR1Idle = 0x01;
inline constexpr uint8_t kR1IllegalCmd = 0x04;
inline constexpr uint8_t kR1ParamErr = 0x40;
// У R1 старший бит всегда ноль: пока он единица, линия ещё в простое.
inline constexpr uint8_t kR1StartBit = 0x80;
// Второй байт R2 (CMD13): общая ошибка - например, запись не легла на носитель.
inline constexpr uint8_t kR2Error = 0x04;

// Ответ карты на блок данных записи: младшие пять бит, 0x05 - принят.
inline constexpr uint8_t kDataRespMask = 0x1f;
inline constexpr uint8_t kDataRespAccepted = 0x05;
// Карта занята (пишет блок): линия в нуле.
inline constexpr uint8_t kBusyByte = 0x00;

// --- Токены ---

inline constexpr uint8_t kTokenStartBlock = 0xfe; // начало блока (CMD9/10/17/18/24)
inline constexpr uint8_t kTokenStopTran = 0xfd;   // конец многоблочной записи
inline constexpr uint8_t kIdleByte = 0xff;        // линия в простое
// Токен ошибки данных: 0000xxxx.
inline constexpr uint8_t kTokenErrorMask = 0xf0;

// --- Биты регистров и аргументов ---

inline constexpr uint32_t kOpCondHcsBits = 0x40000000u; // ACMD41/CMD1: хост понимает SDHC
inline constexpr uint8_t kOcrPowerUpBits = 0x80;        // старший байт OCR: питание поднято
inline constexpr uint8_t kOcrCcsBits = 0x40;            // старший байт OCR: карта SDHC/SDXC
// CMD8: напряжение 2.7-3.6 В и проверочный узор; карта возвращает оба в R7.
inline constexpr uint8_t kIfCondVoltage = 0x01;
inline constexpr uint8_t kIfCondPattern = 0xaa;
inline constexpr uint32_t kIfCondArg = (static_cast<uint32_t>(kIfCondVoltage) << 8) | kIfCondPattern;

// --- CRC ---

// CRC7 кадра команды и регистров CSD/CID, с концевой единицей. Сдвиг перед
// сравнением: при сравнении до сдвига выходит 0x6F для CMD0 вместо 0x95 и
// 0x81 для CMD8 вместо 0x87. CRC7 обязателен для CMD0 и CMD8 (пока карта не
// разрешит его не считать), но считается всегда - проще, чем помнить
// исключения.
//
// Таблицей на байт: побитный цикл компилятор разворачивал в каждом месте
// вызова (около 1 КБ кода на кадр команды). Таблица строится тем же
// побитным шагом.
namespace detail {
struct Crc7Table {
    uint8_t t[256];
    constexpr Crc7Table() : t{} {
        for (uint32_t i = 0; i < 256; ++i) {
            uint8_t crc = 0;
            uint8_t b = static_cast<uint8_t>(i);
            for (uint32_t bit = 0; bit < 8; ++bit) {
                crc = static_cast<uint8_t>(crc << 1);
                if (((b ^ crc) & 0x80) != 0) crc ^= 0x09;
                b = static_cast<uint8_t>(b << 1);
            }
            t[i] = crc;
        }
    }
};
inline constexpr Crc7Table kCrc7Table{};
} // namespace detail

constexpr uint8_t crc7(const uint8_t* d, uint32_t n) {
    uint8_t crc = 0;
    for (uint32_t i = 0; i < n; ++i) crc = detail::kCrc7Table.t[static_cast<uint8_t>((crc << 1) ^ d[i])];
    return static_cast<uint8_t>((crc << 1) | 1u);
}

// CRC-16/XMODEM - тот, которым карта закрывает блок данных. В SPI карта его
// не требует, но драйвер, который CRC проверяет, отверг бы каждый блок.
//
// Побайтно, без таблицы: 9 команд на байт против 36 у побитного - сектор
// около 15 мкс вместо 61 при 300 МГц; считается на каждом секторе с карты
// и на каждом, отданном хосту.
constexpr uint16_t crc16(const uint8_t* d, uint32_t n) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t x = ((crc >> 8) ^ d[i]) & 0xffu;
        x ^= x >> 4;
        crc = ((crc << 8) ^ (x << 12) ^ (x << 5) ^ x) & 0xffffu;
    }
    return static_cast<uint16_t>(crc);
}

namespace detail {
inline constexpr uint8_t kFrameCmd0[5] = {0x40, 0x00, 0x00, 0x00, 0x00};
inline constexpr uint8_t kFrameCmd8[5] = {0x48, 0x00, 0x00, 0x01, 0xaa};
inline constexpr uint8_t kCheck[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
} // namespace detail

// Эталонные векторы: полные кадры CMD0 и CMD8 одинаковы у любого
// SD-драйвера, "123456789" - контрольное значение CRC-16/XMODEM.
static_assert(crc7(detail::kFrameCmd0, 5) == 0x95, "CRC7 кадра CMD0 обязан быть 0x95");
static_assert(crc7(detail::kFrameCmd8, 5) == 0x87, "CRC7 кадра CMD8 обязан быть 0x87");
static_assert(crc16(detail::kCheck, 9) == 0x31c3, "CRC-16/XMODEM строки 123456789 обязан быть 0x31C3");

} // namespace devices::sd
