// SPDX-License-Identifier: MIT
#include "devices/gs/gs_device.h"

#include <cstring>

#include "platform/hot_path.h"

namespace devices::gs {

namespace {

// Коды команд по руководству GS v1.04 ред.006.
constexpr uint8_t kCmdResetFlags      = 0x00u;
constexpr uint8_t kCmdResetFlags2     = 0x08u; // то же, что #00
constexpr uint8_t kCmdOutPort         = 0x10u;
constexpr uint8_t kCmdInPort          = 0x11u;
constexpr uint8_t kCmdOutPort0        = 0x12u;
constexpr uint8_t kCmdJump            = 0x13u;
constexpr uint8_t kCmdLoadBlock       = 0x14u;
constexpr uint8_t kCmdGetBlock        = 0x15u;
constexpr uint8_t kCmdPoke            = 0x16u;
constexpr uint8_t kCmdPeek            = 0x17u;
constexpr uint8_t kCmdLoadDe          = 0x18u;
constexpr uint8_t kCmdPokeDe          = 0x19u;
constexpr uint8_t kCmdPeekDe          = 0x1au;
constexpr uint8_t kCmdIncDe           = 0x1bu;
constexpr uint8_t kCmdPoke20          = 0x1cu;
constexpr uint8_t kCmdPeek20          = 0x1du;
constexpr uint16_t kPeek20Base        = 0x2000u; // #1D читает 0x2000 + аргумент
constexpr uint8_t kCmdTotalRam        = 0x20u;
constexpr uint8_t kCmdFreeRam         = 0x21u;
constexpr uint8_t kCmdPages           = 0x23u;
constexpr uint8_t kCmdSetCurFx        = 0x2eu;
constexpr uint8_t kCmdLoadModule      = 0x30u;
constexpr uint8_t kCmdPlayModule      = 0x31u;
constexpr uint8_t kCmdStopModule      = 0x32u;
constexpr uint8_t kCmdContModule      = 0x33u;
constexpr uint8_t kCmdDataOn          = 0x36u; // положить в регистр вывода 0xff
constexpr uint8_t kCmdReinit          = 0x37u;
constexpr uint8_t kCmdSongPosition    = 0x60u;
constexpr uint8_t kCmdPatternPosition = 0x61u;
constexpr uint8_t kCmdMixPosition     = 0x62u;
constexpr uint8_t kCmdPlayerMode      = 0x6au;
constexpr uint8_t kCmdRelooper        = 0x6bu;
constexpr uint8_t kCmdStreamOpen      = 0xd1u;
constexpr uint8_t kCmdStreamClose     = 0xd2u;
constexpr uint8_t kCmdWarmReset       = 0xf3u;
constexpr uint8_t kCmdColdReset       = 0xf4u;
constexpr uint8_t kCmdBusyOn          = 0xf5u;
constexpr uint8_t kCmdBusyOff         = 0xf6u;
constexpr uint8_t kCmdGetHx           = 0xf7u;

// Внутренние порты карты.
constexpr uint8_t kInnerPortMask = GsDevice::kInnerPortCount - 1u;
static_assert((GsDevice::kInnerPortCount & kInnerPortMask) == 0, "the number of internal ports is a power of two");
constexpr uint8_t kInnerPortPages             = 0x00u;
constexpr uint8_t kInnerPortCommand           = 0x01u;
constexpr uint8_t kInnerPortData              = 0x02u;
constexpr uint8_t kInnerPortOut               = 0x03u;
constexpr uint8_t kInnerPortStatus            = 0x04u;
constexpr uint8_t kInnerPortClearCommand      = 0x05u;
constexpr uint8_t kInnerPortVolume4           = 0x09u;
constexpr uint8_t kInnerPortDataFromPages     = 0x0au;
constexpr uint8_t kInnerPortCommandFromVolume = 0x0bu;
// Биты, которые читают #0A и #0B.
constexpr uint8_t kPagesDataBit     = 0x01u; // порт #00, D0
constexpr uint8_t kVolumeCommandBit = 0x20u; // порт #09, D5

constexpr uint8_t kHxBusyBit = 0x80u; // регистр HX (#F7), D7

// Ответ #62: биты 7-6 - младшие два бита позиции, биты 5-0 - строка.
constexpr uint8_t kMixOrderBits = 0x03u;
constexpr uint8_t kMixOrderLsb  = 6;
constexpr uint8_t kMixRowBits   = 0x3fu;

// Длина хвоста #14/#15: LEN.H, ADR.L, ADR.H (LEN.L - аргумент до кода).
constexpr uint8_t kBlockTailBytes = 3;

// Переменные ПЗУ карты: значения, которые ПЗУ GS 1.04 и 1.05a пишет при
// инициализации. Z-Player 4.1 читает #4151 и при нуле ждёт бит данных вечно.
struct RomVar {
    uint16_t addr;
    uint8_t value;
};
SOUNDSINTH_HOT_PATH_ATTR("gs_rom_vars")
constexpr RomVar kRomVars[] = {
    {0x409fu, 0x0fu},
    {0x40a4u, 0x40u},
    {0x4151u, 0xc3u},
};

// Память карты не эмулируется: известные переменные ПЗУ - из таблицы,
// остальное - ноль.
SOUNDSINTH_HOT_PATH_ATTR("gs_mem_peek")
uint8_t mem_peek(uint16_t addr) {
    for (const RomVar& v : kRomVars) {
        if (v.addr == addr) return v.value;
    }
    return 0;
}

// Слово из младшего и старшего байта хвоста.
constexpr uint16_t make_word(uint8_t lo, uint8_t hi) {
    return static_cast<uint16_t>(lo | (hi << 8));
}

// Счётчик гистограммы команд насыщается.
constexpr uint16_t kHistMax = 0xffffu;

} // namespace

SOUNDSINTH_HOT_PATH_ATTR("gs_reset")
void GsDevice::reset() {
    status_       = kStatusIdle;
    last_cmd_     = 0;
    arg_          = 0;
    has_arg_      = false;
    tail_left_    = 0;
    de_           = 0;
    sink_left_    = 0;
    zeros_left_   = 0;
    resp_n_       = 0;
    resp_pos_     = 0;
    stream_       = Stream::None;
    stream_bytes_ = 0;
    out_reg_      = 0;
    hx_busy_      = false;
    cur_fx_       = 0;
    // unknown_* не сбрасываются: это мера покрытия за всё время работы.
}

SOUNDSINTH_HOT_PATH_ATTR("gs_refresh_data_status")
void GsDevice::refresh_data_status() {
    status_ = (zeros_left_ > 0 || resp_pos_ < resp_n_) ? kStatusData : kStatusIdle;
}

SOUNDSINTH_HOT_PATH_ATTR("gs_respond")
void GsDevice::respond(const uint8_t* bytes, uint8_t n) {
    if (n > sizeof(resp_)) {
        n = sizeof(resp_);
    }
    std::memcpy(resp_, bytes, n);
    resp_n_   = n;
    resp_pos_ = 0;
    refresh_data_status(); // софт ждёт на бите данных (WN)
}

SOUNDSINTH_HOT_PATH_ATTR("gs_respond_u24")
void GsDevice::respond_u24(uint32_t v) {
    const uint8_t b[3] = {static_cast<uint8_t>(v & 0xffu), static_cast<uint8_t>((v >> 8) & 0xffu), static_cast<uint8_t>((v >> 16) & 0xffu)};
    respond(b, 3);
}

SOUNDSINTH_HOT_PATH_ATTR("gs_respond_u8")
void GsDevice::respond_u8(uint8_t v) {
    respond(&v, 1);
}

SOUNDSINTH_HOT_PATH_ATTR("gs_write_data")
bool GsDevice::write_data(uint8_t value) {
    last_data_ = value; // порт связи #02 отдаёт его обратно
    if (stream_ != Stream::None) {
        // Бит данных остаётся снятым: "байт принят, давай следующий".
        // Обратного давления (взвести бит при забитом кольце приёма)
        // нет.
        ++stream_bytes_;
        return true;
    }
    if (sink_left_ > 0) {
        --sink_left_;
        return false;
    }
    if (tail_left_ > 0) {
        take_tail(value);
        return false;
    }
    // Вне потока запись в регистр данных - аргумент будущей команды:
    // софт кладёт их до подачи кода (SD ... SC).
    if (!has_arg_) {
        arg_     = value;
        has_arg_ = true;
    }
    return false;
}

// Очередной байт хвоста команды.
SOUNDSINTH_HOT_PATH_ATTR("gs_take_tail")
void GsDevice::take_tail(uint8_t value) {
    const bool first_tail_byte = tail_left_ == kBlockTailBytes;
    --tail_left_;
    switch (last_cmd_) {
        case kCmdOutPort:
            // Вторая фаза #10: SD Port / SC #10 / WC / SD Data.
            inner_write(static_cast<uint8_t>(tail_word_), value);
            break;
        case kCmdLoadBlock:
        case kCmdGetBlock:
            // Первый байт хвоста - LEN.H, адрес не нужен.
            if (first_tail_byte) tail_word_ = make_word(static_cast<uint8_t>(tail_word_), value);
            if (tail_left_ == 0) {
                if (last_cmd_ == kCmdLoadBlock) {
                    sink_left_ = tail_word_;
                } else {
                    zeros_left_ = tail_word_;
                    refresh_data_status();
                }
            }
            break;
        case kCmdPeek:
            // SD ADR.L / SC #17 / WD / SD ADR.H / GD Byte.
            respond_u8(mem_peek(make_word(static_cast<uint8_t>(tail_word_), value)));
            break;
        case kCmdLoadDe:
            // SD E / SC #18 / WC / SD D.
            de_ = make_word(static_cast<uint8_t>(de_), value);
            break;
        default:
            // #6B (старший байт длины), #13 (ADR.H), #16 (адрес), #1C (байт):
            // значение не нужно.
            break;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_peek_data")
uint8_t GsDevice::peek_data() const {
    if (zeros_left_ > 0) {
        return 0;
    }
    return resp_pos_ < resp_n_ ? resp_[resp_pos_] : out_reg_;
}

SOUNDSINTH_HOT_PATH_ATTR("gs_read_data")
uint8_t GsDevice::read_data() {
    // Байт - тот же, что показал peek_data; дальше - продвинуть источник.
    // Отдавать нечего - читается регистр вывода: у настоящей карты он
    // физический, содержимое переживает конец ответа.
    const uint8_t byte = peek_data();
    if (zeros_left_ > 0) {
        --zeros_left_; // блок #15: вместо памяти карты нули
        out_reg_ = 0;
    } else if (resp_pos_ < resp_n_) {
        out_reg_ = resp_[resp_pos_++];
    }
    refresh_data_status();
    return byte;
}

// --- Внутренние порты ---
//
// Два из них связаны с битами состояния, на этом построен детект: софт
// пишет известное значение в порт страниц, трогает #0A и проверяет, что
// бит данных стал его инверсией. Без #10/#11 такой детект объявляет карту
// отсутствующей.
SOUNDSINTH_HOT_PATH_ATTR("gs_inner_write")
void GsDevice::inner_write(uint8_t port, uint8_t value) {
    port         &= kInnerPortMask;
    inner_[port]  = value;
    switch (port) {
        case kInnerPortOut:
            // Регистр вывода в Spectrum: записанное отдаётся через GSDAT
            // (детект плеера). Бит данных взводится: так
            // карта сообщает, что байт готов (WN).
            respond_u8(value);
            break;
        case kInnerPortClearCommand:
            // Сброс бита команд - "команда выполнена".
            status_ &= static_cast<uint8_t>(~kCommandBit);
            break;
        case kInnerPortDataFromPages:
        case kInnerPortCommandFromVolume:
            apply_status_link(port);
            break;
        default:
            break;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_apply_status_link")
void GsDevice::apply_status_link(uint8_t port) {
    if (port == kInnerPortDataFromPages) {
        // Бит данных = инверсия бита 0 порта страниц.
        if ((inner_[kInnerPortPages] & kPagesDataBit) != 0u) {
            status_ &= static_cast<uint8_t>(~kDataBit);
        } else {
            status_ |= kDataBit;
        }
    } else {
        // Бит команд = бит 5 громкости четвёртого канала.
        if ((inner_[kInnerPortVolume4] & kVolumeCommandBit) != 0u) {
            status_ |= kCommandBit;
        } else {
            status_ &= static_cast<uint8_t>(~kCommandBit);
        }
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_inner_read")
uint8_t GsDevice::inner_read(uint8_t port) {
    port &= kInnerPortMask;
    switch (port) {
        case kInnerPortCommand:
            return last_cmd_; // код последней команды от ZX
        case kInnerPortData:
            return last_data_; // последний байт данных от ZX
        case kInnerPortStatus:
            return status_; // слово состояния целиком
        case kInnerPortDataFromPages:
        case kInnerPortCommandFromVolume:
            apply_status_link(port); // эти два двигают биты и при чтении
            return inner_[port];
        default:
            return inner_[port];
    }
}

// Состояние по умолчанию для любой команды: выполнена, данных нет, бит
// команды не взводится. Недособранный хвост и блок прошлой команды
// бросаются.
SOUNDSINTH_HOT_PATH_ATTR("gs_drop_pending_exchange")
void GsDevice::drop_pending_exchange() {
    status_     = kStatusIdle;
    resp_n_     = 0;
    resp_pos_   = 0;
    tail_left_  = 0;
    sink_left_  = 0;
    zeros_left_ = 0;
}

// Память и процессор карты, #13..#1D. Процессора и ОЗУ карты нет: команда
// съедает ровно свои байты, записанное пропадает, чтение отдаёт известные
// переменные ПЗУ, прочее - ноль. Следующая команда получит свои аргументы.
SOUNDSINTH_HOT_PATH_ATTR("gs_card_memory_command")
void GsDevice::card_memory_command(uint8_t value) {
    switch (value) {
        case kCmdJump:   // SD ADR.L / SC / WC / SD ADR.H
        case kCmdPoke20: // SD ADR.L / SC / WC / SD Byte
            tail_left_ = 1;
            break;
        case kCmdLoadDe: // SD E / SC / WC / SD D
            de_        = arg_;
            tail_left_ = 1;
            break;
        case kCmdPeek: // SD ADR.L / SC / WD / SD ADR.H / GD Byte
            tail_word_ = arg_;
            tail_left_ = 1;
            break;
        case kCmdPoke: // SD Byte / SC / WC / SD ADR.L / SD ADR.H
            tail_left_ = 2;
            break;
        case kCmdLoadBlock: // SD LEN.L / SC / SD LEN.H, ADR.L, ADR.H / SD x LEN
        case kCmdGetBlock:  // SD LEN.L / SC / SD LEN.H, ADR.L, ADR.H / GD x LEN
            tail_word_ = arg_;
            tail_left_ = kBlockTailBytes;
            break;
        case kCmdPokeDe: // SD Byte / SC
            break;
        case kCmdIncDe: // SC
            ++de_;
            break;
        case kCmdPeekDe: // SC / GD Byte
            respond_u8(mem_peek(de_));
            break;
        case kCmdPeek20: // SD ADR.L / SC / GD Byte
            respond_u8(mem_peek(static_cast<uint16_t>(kPeek20Base | arg_)));
            break;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_write_command")
Event GsDevice::write_command(uint8_t value) {
    last_cmd_ = value;
    if (cmd_hist_[value] != kHistMax) ++cmd_hist_[value];
    drop_pending_exchange();

    Event event = Event::None;
    if (value >= kCmdJump && value <= kCmdPeek20) {
        card_memory_command(value);
    } else {
        switch (value) {
            case kCmdResetFlags:
            case kCmdResetFlags2:
            case kCmdReinit:
                // Сброс флагов и переинициализация: поток закрыть.
                stream_ = Stream::None;
                break;

            case kCmdWarmReset:
            case kCmdColdReset:
                reset();
                event = Event::Reset;
                break;

            case kCmdBusyOn:
                hx_busy_ = true;
                break;
            case kCmdBusyOff:
                hx_busy_ = false;
                break;

            case kCmdGetHx:
                // Регистр HX. По руководству значим только бит 7 - флаг
                // занятости.
                respond_u8(hx_busy_ ? kHxBusyBit : 0u);
                break;

            case kCmdTotalRam:
            case kCmdFreeRam:
                // Свободно столько же, сколько всего: модуль ложится в свой
                // приёмный буфер, объявленную память ничто не расходует.
                respond_u24(kDeclaredRamBytes);
                break;
            case kCmdPages:
                respond_u8(kDeclaredPages);
                break;

            case kCmdDataOn:
                // Установить регистр данных в 0xFF: проверка связи, софт
                // подаёт команду и ждёт 0xFF в GSDAT.
                respond_u8(0xffu);
                break;

            // --- Прямой вывод в ЦАП и громкости, #01..#0B ---
            //
            // Низкоуровневая группа: тишина, громкость канала, байт в ЦАП.
            // Принимаются молча, звука по ним нет (нужен отдельный источник в
            // микшере). Аргументы съедаются, состояние "выполнено". В счётчик
            // неопознанных не идут, сколько их пришло - видно в command_count.
            case 0x01u: // Set silence
            case 0x02u: // Set low volume
            case 0x03u: // Set high volume
            case 0x04u: // Set E 3 bits
            case 0x05u: // Out volume port
            case 0x06u: // Send to DAC
            case 0x07u: // Send to DAC and volume
            case 0x09u: // Set one byte volume, ccvvvvvv
            case 0x0au: // DAC output
            case 0x0bu: // DAC and volume output
                break;

            case kCmdOutPort:
                // Порт назван аргументом, значение придёт хвостом.
                tail_word_ = static_cast<uint8_t>(arg_ & kInnerPortMask);
                tail_left_ = 1;
                break;
            case kCmdInPort:
                // Порт назван аргументом, ответ читается сразу; ответ перекрывает
                // биты, выставленные чтением #0A/#0B.
                respond_u8(inner_read(arg_));
                break;
            case kCmdOutPort0:
                // Частный случай записи в регистр страниц.
                inner_write(kInnerPortPages, arg_);
                break;

            case kCmdSetCurFx:
                cur_fx_ = arg_;
                break;

            case kCmdStreamOpen:
                // Ничего не делает: поток открыт уже командой #30. #D1 без #30 -
                // не ошибка, софт иногда шлёт его вхолостую.
                break;

            case kCmdLoadModule:
                // Порядок по руководству: SC #30 / WC / GD handle, потом #D1.
                // Поток открывается уже здесь, #D1 ничего не делает: запись в
                // GSDAT между #30 и #D1 идёт в поток. Номер модуля всегда 1, у GS
                // их не бывает больше одного.
                stream_       = Stream::Module;
                stream_bytes_ = 0;
                respond_u8(1);
                event = Event::StreamBegin;
                break;

            // --- Обратная связь плееру ---
            //
            // Позиция в order-листе, строка паттерна и их смесь. Софт делает
            // AND #3F для строки и пару RLCA для позиции.
            case kCmdSongPosition:
                respond_u8(order_);
                break;
            case kCmdPatternPosition:
                respond_u8(row_);
                break;
            case kCmdMixPosition:
                respond_u8(static_cast<uint8_t>(((order_ & kMixOrderBits) << kMixOrderLsb) | (row_ & kMixRowBits)));
                break;

            // --- Настройки плеера, #6A и #6B ---
            //
            // Из дополнения руководства: #6A - не останавливаться по F00 в модуле,
            // #6B - релупер (обход зависания GS на очень коротких петлях). Трекеру
            // не нужны, разбор съедает аргументы.
            case kCmdPlayerMode:
                break;
            case kCmdRelooper:
                // Младший байт длины приходит до кода, старший может прийти
                // хвостом. Не пришёл - хвост бросает следующая команда. Её
                // аргумент, записанный раньше неё, будет принят за старший байт.
                // Короткая форма без аргументов - хвоста нет.
                tail_left_ = has_arg_ ? 1u : 0u;
                break;

            case kCmdPlayModule:
                event = Event::Play;
                break;
            case kCmdStopModule:
                event = Event::Stop;
                break;
            case kCmdContModule:
                event = Event::Resume;
                break;

            case kCmdStreamClose:
                if (stream_ != Stream::None) {
                    // Долгая команда: снос прежнего движка - десятки
                    // миллисекунд, разбор модуля до мегабайта - единицы; плеер
                    // обязан дождаться. Отпустит платформа, через
                    // hold_parse_done().
                    hold_parse_begin();
                    stream_ = Stream::None;
                    event   = Event::StreamEnd;
                }
                break;

            default:
                // Неопознанная команда запоминается: по ним видно, чего плеерам
                // под GS ещё не хватает.
                unknown_cmd_ = value;
                ++unknown_count_;
                break;
        }
    }

    // Аргументы съедены командой: следующая получила бы чужие.
    if (value != kCmdStreamOpen) {
        arg_     = 0;
        has_arg_ = false;
    }
    return event;
}

} // namespace devices::gs
