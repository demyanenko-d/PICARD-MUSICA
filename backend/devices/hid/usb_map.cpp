// SPDX-License-Identifier: MIT
#include "devices/hid/usb_map.h"

#include <array>
#include <cstring>

#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "devices/hal/host_signals.h"
#include "devices/hid/joystick.h"
#include "devices/hid/keyboard.h"
#include "devices/hid/mouse.h"

namespace devices::hid {
namespace {

constexpr uint8_t kNoKey = 0xff;

// Клавиша матрицы одним байтом: строка в старших разрядах, разряд в
// младших.
constexpr uint8_t zx(uint8_t row, uint8_t bit) {
    return static_cast<uint8_t>((row << 3) | bit);
}

constexpr uint8_t kCapsShift = zx(0, 0);
constexpr uint8_t kSymShift  = zx(7, 1);

// Две клавиши на код: у ZX половина знаков набирается с CAPS или SYMBOL
// SHIFT, и нажимать их надо вместе.
struct Combo {
    uint8_t first;
    uint8_t second;
};

// Таблица вдвое длиннее числа кодов: вторая половина - то же с нажатым
// левым Shift, то есть отображается набранный знак, а не клавиша.
constexpr uint16_t kShifted = 0x100;

constexpr std::array<Combo, 512> build_keymap() {
    std::array<Combo, 512> m{};
    for (Combo& c : m) {
        c = Combo{kNoKey, kNoKey};
    }

    // Буквы: коды 0x04..0x1D идут по латинскому алфавиту.
    m[0x04] = {zx(1, 0), kNoKey}; // A
    m[0x05] = {zx(7, 4), kNoKey}; // B
    m[0x06] = {zx(0, 3), kNoKey}; // C
    m[0x07] = {zx(1, 2), kNoKey}; // D
    m[0x08] = {zx(2, 2), kNoKey}; // E
    m[0x09] = {zx(1, 3), kNoKey}; // F
    m[0x0a] = {zx(1, 4), kNoKey}; // G
    m[0x0b] = {zx(6, 4), kNoKey}; // H
    m[0x0c] = {zx(5, 2), kNoKey}; // I
    m[0x0d] = {zx(6, 3), kNoKey}; // J
    m[0x0e] = {zx(6, 2), kNoKey}; // K
    m[0x0f] = {zx(6, 1), kNoKey}; // L
    m[0x10] = {zx(7, 2), kNoKey}; // M
    m[0x11] = {zx(7, 3), kNoKey}; // N
    m[0x12] = {zx(5, 1), kNoKey}; // O
    m[0x13] = {zx(5, 0), kNoKey}; // P
    m[0x14] = {zx(2, 0), kNoKey}; // Q
    m[0x15] = {zx(2, 3), kNoKey}; // R
    m[0x16] = {zx(1, 1), kNoKey}; // S
    m[0x17] = {zx(2, 4), kNoKey}; // T
    m[0x18] = {zx(5, 3), kNoKey}; // U
    m[0x19] = {zx(0, 4), kNoKey}; // V
    m[0x1a] = {zx(2, 1), kNoKey}; // W
    m[0x1b] = {zx(0, 2), kNoKey}; // X
    m[0x1c] = {zx(5, 4), kNoKey}; // Y
    m[0x1d] = {zx(0, 1), kNoKey}; // Z

    // Цифры: 1..5 в своей строке слева направо, 6..0 в соседней справа
    // налево - как на самой машине.
    m[0x1e] = {zx(3, 0), kNoKey}; // 1
    m[0x1f] = {zx(3, 1), kNoKey}; // 2
    m[0x20] = {zx(3, 2), kNoKey}; // 3
    m[0x21] = {zx(3, 3), kNoKey}; // 4
    m[0x22] = {zx(3, 4), kNoKey}; // 5
    m[0x23] = {zx(4, 4), kNoKey}; // 6
    m[0x24] = {zx(4, 3), kNoKey}; // 7
    m[0x25] = {zx(4, 2), kNoKey}; // 8
    m[0x26] = {zx(4, 1), kNoKey}; // 9
    m[0x27] = {zx(4, 0), kNoKey}; // 0

    m[0x28] = {zx(6, 0), kNoKey};      // Enter
    m[0x29] = {kCapsShift, zx(7, 0)};  // Escape - BREAK
    m[0x2a] = {kCapsShift, zx(4, 0)};  // Backspace - DELETE
    m[0x2b] = {kCapsShift, kSymShift}; // Tab - расширенный режим
    m[0x2c] = {zx(7, 0), kNoKey};      // пробел

    // Знаки набираются с SYMBOL SHIFT.
    m[0x2d] = {kSymShift, zx(6, 3)}; // минус
    m[0x2e] = {kSymShift, zx(6, 1)}; // равно
    m[0x33] = {kSymShift, zx(5, 1)}; // точка с запятой
    m[0x34] = {kSymShift, zx(4, 3)}; // апостроф
    m[0x36] = {kSymShift, zx(7, 3)}; // запятая
    m[0x37] = {kSymShift, zx(7, 2)}; // точка
    m[0x38] = {kSymShift, zx(0, 4)}; // косая черта

    // Стрелки - с CAPS SHIFT, как на резиновой клавиатуре.
    m[0x4f] = {kCapsShift, zx(4, 2)}; // вправо - 8
    m[0x50] = {kCapsShift, zx(3, 4)}; // влево - 5
    m[0x51] = {kCapsShift, zx(4, 4)}; // вниз - 6
    m[0x52] = {kCapsShift, zx(4, 3)}; // вверх - 7

    // Скобки и обратная косая.
    m[0x2f] = {kSymShift, zx(5, 4)}; // [ - SS+Y
    m[0x30] = {kSymShift, zx(5, 3)}; // ] - SS+U
    m[0x31] = {kSymShift, zx(1, 2)}; // \ - SS+D

    // Цифровая клавиатура: цифры сами собой, знаки как на основной.
    m[0x59] = {zx(3, 0), kNoKey};    // 1
    m[0x5a] = {zx(3, 1), kNoKey};    // 2
    m[0x5b] = {zx(3, 2), kNoKey};    // 3
    m[0x5c] = {zx(3, 3), kNoKey};    // 4
    m[0x5d] = {zx(3, 4), kNoKey};    // 5
    m[0x5e] = {zx(4, 4), kNoKey};    // 6
    m[0x5f] = {zx(4, 3), kNoKey};    // 7
    m[0x60] = {zx(4, 2), kNoKey};    // 8
    m[0x61] = {zx(4, 1), kNoKey};    // 9
    m[0x62] = {zx(4, 0), kNoKey};    // 0
    m[0x58] = {zx(6, 0), kNoKey};    // Enter
    m[0x63] = {kSymShift, zx(7, 2)}; // . - SS+M
    m[0x54] = {kSymShift, zx(0, 4)}; // / - SS+V
    m[0x55] = {kSymShift, zx(7, 4)}; // * - SS+B
    m[0x56] = {kSymShift, zx(6, 3)}; // - - SS+J
    m[0x57] = {kSymShift, zx(6, 2)}; // + - SS+K

    // Клавиши правки: как на Spectrum+.
    m[0x39] = {kCapsShift, zx(3, 1)}; // Caps Lock - CS+2
    m[0x4b] = {kCapsShift, zx(3, 2)}; // PgUp - CS+3, TRUE VIDEO
    m[0x4e] = {kCapsShift, zx(3, 3)}; // PgDn - CS+4, INV VIDEO
    m[0x4c] = {kCapsShift, zx(4, 1)}; // Delete - CS+9, GRAPH
    m[0x35] = {kCapsShift, zx(3, 0)}; // ` - CS+1, EDIT

    // Знаки сравнения: на ПК их нечем набрать одной клавишей, поэтому они
    // сидят на трёх клавишах правки, которые машине иначе не нужны.
    m[0x4a] = {kSymShift, zx(2, 0)}; // Home - SS+Q, <=
    m[0x49] = {kSymShift, zx(2, 1)}; // Insert - SS+W, <>
    m[0x4d] = {kSymShift, zx(2, 2)}; // End - SS+E, >=

    // --- Верхний регистр: то же с левым Shift ---
    //
    // Знаки по раскладке US: что напечатано на верхней грани клавиши.
    m[kShifted | 0x1e] = {kSymShift, zx(3, 0)}; // ! - SS+1
    m[kShifted | 0x1f] = {kSymShift, zx(3, 1)}; // @ - SS+2
    m[kShifted | 0x20] = {kSymShift, zx(3, 2)}; // # - SS+3
    m[kShifted | 0x21] = {kSymShift, zx(3, 3)}; // $ - SS+4
    m[kShifted | 0x22] = {kSymShift, zx(3, 4)}; // % - SS+5
    m[kShifted | 0x23] = {kSymShift, zx(6, 4)}; // ^ - SS+H
    m[kShifted | 0x24] = {kSymShift, zx(4, 4)}; // & - SS+6
    m[kShifted | 0x25] = {kSymShift, zx(7, 4)}; // * - SS+B
    m[kShifted | 0x26] = {kSymShift, zx(4, 2)}; // ( - SS+8
    m[kShifted | 0x27] = {kSymShift, zx(4, 1)}; // ) - SS+9
    m[kShifted | 0x2d] = {kSymShift, zx(4, 0)}; // _ - SS+0
    m[kShifted | 0x2e] = {kSymShift, zx(6, 2)}; // + - SS+K
    m[kShifted | 0x2f] = {kSymShift, zx(1, 3)}; // { - SS+F
    m[kShifted | 0x30] = {kSymShift, zx(1, 4)}; // } - SS+G
    m[kShifted | 0x31] = {kSymShift, zx(1, 1)}; // | - SS+S
    m[kShifted | 0x33] = {kSymShift, zx(0, 1)}; // : - SS+Z
    m[kShifted | 0x34] = {kSymShift, zx(5, 0)}; // " - SS+P
    m[kShifted | 0x35] = {kSymShift, zx(1, 0)}; // ~ - SS+A
    m[kShifted | 0x36] = {kSymShift, zx(2, 3)}; // < - SS+R
    m[kShifted | 0x37] = {kSymShift, zx(2, 4)}; // > - SS+T
    m[kShifted | 0x38] = {kSymShift, zx(0, 3)}; // ? - SS+C
    return m;
}

constexpr std::array<Combo, 512> kKeymap = build_keymap();

// Назначения HID модификаторов: слева Ctrl, Shift, Alt, Gui, затем те же
// справа. Разряд i байта модификаторов загрузочного отчёта - назначение
// kUsageLeftCtrl + i.
constexpr uint8_t kUsageLeftCtrl     = 0xe0;
constexpr uint8_t kUsageLeftShift    = 0xe1;
constexpr uint8_t kUsageLeftAlt      = 0xe2;
constexpr uint8_t kUsageLeftGui      = 0xe3;
constexpr uint8_t kUsageRightCtrl    = 0xe4;
constexpr uint8_t kUsageRightShift   = 0xe5;
constexpr uint8_t kUsageRightAlt     = 0xe6;
constexpr uint8_t kUsageRightGui     = 0xe7;
constexpr uint8_t kBootModifierCount = 8;
static_assert(kUsageLeftCtrl + kBootModifierCount - 1u == kUsageRightGui, "boot modifier bits map onto 0xE0..0xE7");

ModifierRoles s_roles = ModifierRoles{};
// Нажатий по кодам без записи в раскладке. Строка журнала с ненулевым
// числом сразу говорит, что дело в раскладке, а не в дескрипторе.
uint32_t s_unmapped = 0;
MouseTuning s_mouse;
// Остаток деления по каждой оси. Без него медленное движение
// пропадает целиком: датчик шлёт приращения по единице, а при скорости
// ниже четырёх четвертей каждое из них округляется в ноль.
int32_t s_mouse_rem_x = 0;
int32_t s_mouse_rem_y = 0;

// --- Обмен кнопок жестом ---
//
// Обе кнопки, зажатые полсекунды, меняют левую и правую местами. Полсекунды
// нужны, чтобы жест не ловился случайно: обе кнопки разом нажимают и в
// играх, но коротко.
//
// Срок считается по отчётам, а не отдельными часами: мышь шлёт их, пока
// кнопки держат, и этого довольно. Повторно жест не срабатывает, пока
// кнопки не отпустят, иначе обмен щёлкал бы без конца.
constexpr uint32_t kAutoSwapHoldUs = 500000;
uint32_t s_both_since_us           = 0;
bool s_both_held                   = false;
bool s_autoswap_done               = false;

void note_both_buttons(bool both) {
    if (!both) {
        s_both_held     = false;
        s_autoswap_done = false;
        return;
    }
    const uint32_t now = platform::mono_us();
    if (!s_both_held) {
        s_both_held     = true;
        s_both_since_us = now;
        return;
    }
    if (s_autoswap_done || !s_mouse.autoswap) return;
    if (now - s_both_since_us < kAutoSwapHoldUs) return;
    s_autoswap_done      = true;
    s_mouse.swap_buttons = !s_mouse.swap_buttons;
    debug_logf("hid: mouse buttons %s\n", s_mouse.swap_buttons ? "swapped" : "normal");
}

// Приращение в четвертях с накоплением остатка. Остаток того же
// знака, что и частное: тогда движение туда-сюда не накапливает сдвиг.
int32_t scale_with_remainder(int32_t delta, int32_t& rem) {
    const int32_t total = delta * static_cast<int32_t>(s_mouse.speed) + rem;
    const int32_t step  = total / 4;
    rem                 = total - step * 4;
    return step;
}

// --- Сигнальные клавиши ---
//
// Стоят выше раскладки: в матрице машины их нет ни в одной, поэтому
// назначить их клавишам нельзя, а дёрнуть линии больше нечем. Сигнал
// уходит на нажатие; пока клавиша держится, повтора нет.
constexpr uint8_t kCodeNmi          = 0x44; // F11
constexpr uint8_t kCodeReset        = 0x45; // F12
constexpr uint8_t kCodeDelete       = 0x4c; // в тройке Ctrl+Alt+Delete
constexpr uint8_t kCodeModeNormal   = 0x3a; // F1
constexpr uint8_t kCodeModeKempston = 0x3b; // F2

struct Signals {
    bool nmi;
    bool reset;
    bool del;
    bool ctrl;
    bool alt;
    bool gui;
    bool mode_normal;
    bool mode_kempston;
};

// --- Быстрый режим управления ---
//
// Клавиши из таблицы привязок - либо клавиши машины, либо Kempston. Режим
// живёт до выключения: поля в настройках нет.
KeyMode s_key_mode = KeyMode::Normal;

// Клавиши действий джойстика, по строке на разряд порта. Код ноль - клавиши
// нет.
uint8_t s_joy_keys[kJoyKeyActions][kJoyKeySlots] = {
    {0x4f, 0}, {0x50, 0}, {0x51, 0}, {0x52, 0}, {0x2c, 0x28}, {0, 0}, {0, 0}, {0, 0},
};

// Разряды Kempston для клавиши. Ноль - клавиша не из этих, идёт в матрицу
// обычным порядком.
uint8_t kempston_bits(uint8_t code) {
    if (code == 0) return 0;
    uint8_t bits = 0;
    for (uint32_t a = 0; a < kJoyKeyActions; ++a) {
        for (const uint8_t k : s_joy_keys[a]) {
            if (k == code) bits = static_cast<uint8_t>(bits | (1u << a));
        }
    }
    return bits;
}

Signals s_signals = {};

// --- Захват клавиши для конфигуратора ---

enum class Capture : uint8_t { Off, WaitRelease, Listen, Done };
Capture s_capture      = Capture::Off;
uint8_t s_capture_code = 0;
bool s_capture_seen    = false; // в отчёте была клавиша

// Коды ниже 4 - не клавиши: пусто и переполнение.
constexpr uint8_t kUsageFirstKey = 0x04;

void capture_report_begin() {
    s_capture_seen = false;
}

void capture_code(uint8_t code) {
    if (code < kUsageFirstKey) return;
    s_capture_seen = true;
    if (s_capture == Capture::Listen) {
        s_capture_code = code;
        s_capture      = Capture::Done;
    }
}

void capture_report_end() {
    if (s_capture == Capture::WaitRelease && !s_capture_seen) s_capture = Capture::Listen;
}

// Win+F12 - вход в конфигуратор настроек. Одиночный F12 при этом машину
// не сбрасывает: так же тройка Ctrl+Alt+Delete не отдаёт ей одиночный
// Delete.
bool config_combo(const Signals& s) {
    return s.gui && s.reset;
}

void set_key_mode(KeyMode mode) {
    if (mode == s_key_mode) return;
    s_key_mode = mode;
    // Порт Kempston режим берёт сам, при любой настройке пульта: режим
    // включает пользователь, и он нужен там, где пульта нет.
    if (mode == KeyMode::KempstonArrows) {
        joystick_keys_attach();
    } else {
        joystick_keys_detach();
    }
    debug_logf("hid: keyboard mode %s\n", mode == KeyMode::KempstonArrows ? "kempston arrows" : "normal");
}

// Сигнал взводится отпусканием: полный сброс перезапускает плату, и
// зажатая тройка иначе сбрасывала бы её по кругу.
bool s_signals_armed = false;

// Полный сброс - тройкой, как на ПК: одной клавишей такое не вешают.
bool hard_combo(const Signals& s) {
    return s.ctrl && s.alt && s.del;
}

void note_signal(Signals& now, uint8_t code) {
    if (code == kCodeNmi) now.nmi = true;
    if (code == kCodeReset) now.reset = true;
    if (code == kCodeDelete) now.del = true;
    if (code == kCodeModeNormal) now.mode_normal = true;
    if (code == kCodeModeKempston) now.mode_kempston = true;
}

// Смена режима - только с Win: одиночные F1 и F2 в матрице машины ничего
// не значат, но занимать их молча всё равно нельзя.
bool mode_normal_combo(const Signals& s) {
    return s.gui && s.mode_normal;
}
bool mode_kempston_combo(const Signals& s) {
    return s.gui && s.mode_kempston;
}

void fire_signals(const Signals& now) {
    const bool quiet = !now.nmi && !now.reset && !hard_combo(now) && !config_combo(now);
    if (!s_signals_armed) {
        // Клавиши ещё не отпускали с подключения: сигнал не подаём, но
        // состояние помним - иначе отпускание не станет фронтом.
        s_signals_armed = quiet;
        s_signals       = now;
        return;
    }
    // Любой сброс и NMI снимают переназначение стрелок, Enter и пробела:
    // после них машина ждёт обычную клавиатуру. Сброс по Ctrl+Alt+Del и вход в
    // конфигуратор перезапускают плату, и режим обычный и так; F12 плату не
    // перезапускает. Снимается и при RESET_SIGNAL=0, когда сброса не будет:
    // нажатие - намерение пользователя сбросить.
    const bool any_reset = (hard_combo(now) && !hard_combo(s_signals)) || (config_combo(now) && !config_combo(s_signals)) ||
                           (now.reset && !s_signals.reset && !now.gui) || (now.nmi && !s_signals.nmi);
    if (any_reset) set_key_mode(KeyMode::Normal);
    if (hard_combo(now) && !hard_combo(s_signals)) hal::host_hard_reset_request();
    if (now.nmi && !s_signals.nmi) hal::host_nmi_request();
    if (config_combo(now) && !config_combo(s_signals)) {
        hal::host_configurator_request();
    } else if (now.reset && !s_signals.reset && !now.gui) {
        hal::host_reset_request();
    }
    if (mode_normal_combo(now) && !mode_normal_combo(s_signals)) set_key_mode(KeyMode::Normal);
    if (mode_kempston_combo(now) && !mode_kempston_combo(s_signals)) set_key_mode(KeyMode::KempstonArrows);
    s_signals = now;
}

// Приращение мыши обрезается байтом: счётчик Kempston восьмиразрядный,
// а поле в отчёте бывает и шире.
int8_t clamp_i8(int32_t v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return static_cast<int8_t>(v);
}

void press(uint8_t rows[kKeyboardRows], uint8_t key) {
    if (key == kNoKey) return;
    const uint8_t row = static_cast<uint8_t>(key >> 3);
    const uint8_t bit = static_cast<uint8_t>(key & 0x07);
    rows[row]         = static_cast<uint8_t>(rows[row] & ~(1u << bit));
}

bool pressed(const uint8_t rows[kKeyboardRows], uint8_t key) {
    return (rows[key >> 3] & (1u << (key & 0x07))) == 0u;
}

void press_role(uint8_t rows[kKeyboardRows], ModRole role) {
    if (role == ModRole::CapsShift) press(rows, kCapsShift);
    if (role == ModRole::SymbolShift) press(rows, kSymShift);
}

// Код клавиши в матрицу. При нажатом левом Shift сперва ищется запись
// верхнего знака, и только если её нет - обычная. Возвращает false на
// признаке переполнения массива: что нажато на самом деле, из такого
// отчёта не узнать.
bool press_code(uint8_t rows[kKeyboardRows], uint8_t code, bool shifted) {
    if (code == 0u) return true;
    if (code == 1u) return false; // ErrorRollOver
    uint16_t index = code;
    if (shifted && kKeymap[kShifted | code].first != kNoKey) {
        index = static_cast<uint16_t>(kShifted | code);
    }
    const Combo& c = kKeymap[index];
    // Записи нет - клавиша не нажимается, и это неотличимо от промаха
    // дескриптора и от потери отчёта. Таких кодов много: F1..F10, Pause,
    // PrintScreen, Menu, часть цифровой клавиатуры, всё выше 0x63.
    if (c.first == kNoKey && c.second == kNoKey) {
        ++s_unmapped;
        return true;
    }
    press(rows, c.first);
    press(rows, c.second);
    return true;
}

// Модификатор в матрицу по настроенной роли. Левый Shift сюда не
// попадает: он не клавиша, а выбор верхнего знака.
void press_modifier(uint8_t rows[kKeyboardRows], uint8_t usage) {
    switch (usage) {
        case kUsageLeftCtrl:
            press_role(rows, s_roles.left_ctrl);
            break;
        case kUsageLeftAlt:
            press_role(rows, s_roles.left_alt);
            break;
        case kUsageRightCtrl:
            press_role(rows, s_roles.right_ctrl);
            break;
        case kUsageRightShift:
            press_role(rows, s_roles.right_shift);
            break;
        case kUsageRightAlt:
            press_role(rows, s_roles.right_alt);
            break;
        default:
            break; // клавиши Gui машине неизвестны
    }
}

// Нажатый модификатор: сигналы, выбор верхнего знака (левый Shift) или
// роль в матрице.
SOUNDSINTH_ALWAYS_INLINE void apply_modifier(uint8_t rows[kKeyboardRows], Signals& signals, bool& shifted, uint8_t usage) {
    if (usage == kUsageLeftCtrl || usage == kUsageRightCtrl) signals.ctrl = true;
    if (usage == kUsageLeftAlt || usage == kUsageRightAlt) signals.alt = true;
    if (usage == kUsageLeftGui || usage == kUsageRightGui) signals.gui = true;
    if (usage == kUsageLeftShift) {
        shifted = true;
        return;
    }
    press_modifier(rows, usage);
}

// Левый Shift, которому не нашлось верхнего знака, становится CAPS SHIFT.
// С SYMBOL SHIFT он не складывается - это уже набранный знак.
void finish_shift(uint8_t rows[kKeyboardRows], bool shifted) {
    if (shifted && !pressed(rows, kSymShift)) press(rows, kCapsShift);
}

// Код клавиши из отчёта: сигналы, Kempston на стрелках, матрица. false -
// признак переполнения, отчёт пропускается.
SOUNDSINTH_ALWAYS_INLINE bool apply_key_code(uint8_t rows[kKeyboardRows], Signals& signals, uint8_t& joy, uint8_t code, bool shifted) {
    if (s_capture != Capture::Off) capture_code(code);
    note_signal(signals, code);
    // Тройка полного сброса в матрицу не идёт: Delete сам по себе
    // нужен, а в тройке он часть сигнала.
    if (code == kCodeDelete && signals.ctrl && signals.alt) return true;
    if (s_key_mode == KeyMode::KempstonArrows) {
        const uint8_t jb = kempston_bits(code);
        if (jb != 0) {
            joy = static_cast<uint8_t>(joy | jb);
            return true; // в матрицу такая клавиша не идёт вовсе
        }
    }
    return press_code(rows, code, shifted);
}

// Разобранный отчёт - машине: джойстик, сигналы, матрица.
SOUNDSINTH_ALWAYS_INLINE void finish_report(uint8_t rows[kKeyboardRows], const Signals& signals, uint8_t joy, bool shifted) {
    if (s_capture != Capture::Off) capture_report_end();
    if (s_key_mode == KeyMode::KempstonArrows) joystick_set_keys(joy);
    fire_signals(signals);
    finish_shift(rows, shifted);
    keyboard_set_rows(rows);
}

// Разбор по дескриптору: модификаторы разрядами, коды слотами массива.
// Так читаются и клавиатуры, которые не перешли в загрузочный протокол, и
// те, у кого слотов больше шести.
bool keyboard_from_map(const ReportMap& map, const uint8_t* report, uint16_t len) {
    const KeyboardMap& k = map.keyboard;
    if (!k.present()) return false;
    // Загрузочная раскладка задана спецификацией и у всех одна. Дескриптор
    // нужен только тем, кто шлёт отчёты с номерами: там первый байт номер,
    // а не модификаторы.
    if (!map.uses_report_ids) return false;

    uint8_t rows[kKeyboardRows];
    for (uint8_t& r : rows) {
        r = 0xff;
    }

    bool shifted    = false;
    Signals signals = {};
    for (uint8_t i = 0; i < k.modifier_count; ++i) {
        if (!report_button_read(k.modifier[i], report, len)) continue;
        apply_modifier(rows, signals, shifted, k.modifier_usage[i]);
    }

    bool any    = false;
    uint8_t joy = 0;
    for (uint8_t slot = 0; slot < k.slot_count; ++slot) {
        uint8_t code = 0;
        if (!report_key_read(k, slot, report, len, code)) continue;
        any = true;
        if (!apply_key_code(rows, signals, joy, code, shifted)) return true; // переполнение: отчёт пропускаем
    }
    // Ни один слот не прочитался - отчёт не этого устройства.
    if (!any) return true;

    finish_report(rows, signals, joy, shifted);
    return true;
}

} // namespace

void usb_map_keyboard(const ReportMap& map, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return;
    capture_report_begin();
    if (keyboard_from_map(map, report, len)) return;

    // Дескриптор не разобрался: остаётся загрузочная раскладка - байт
    // модификаторов, байт запаса, шесть кодов. Она задана спецификацией.
    if (len < 8) return;

    uint8_t rows[kKeyboardRows];
    for (uint8_t& r : rows) {
        r = 0xff;
    }

    // Байт модификаторов - в назначения: разбор тот же, что у дескриптора.
    const uint8_t mods = report[0];
    bool shifted       = false;
    Signals signals    = {};
    for (uint8_t i = 0; i < kBootModifierCount; ++i) {
        if ((mods & (1u << i)) != 0u) apply_modifier(rows, signals, shifted, static_cast<uint8_t>(kUsageLeftCtrl + i));
    }

    uint8_t joy = 0;
    for (uint16_t i = 2; i < 8; ++i) {
        if (!apply_key_code(rows, signals, joy, report[i], shifted)) return;
    }

    finish_report(rows, signals, joy, shifted);
}

ModifierRoles usb_map_default_modifiers() {
    return ModifierRoles{};
}

void usb_map_capture_arm() {
    s_capture = Capture::WaitRelease;
}

void usb_map_capture_cancel() {
    s_capture = Capture::Off;
}

bool usb_map_capture_take(uint8_t& code) {
    if (s_capture != Capture::Done) return false;
    code      = s_capture_code;
    s_capture = Capture::Off;
    return true;
}

void usb_map_set_joy_keys(const uint8_t (&keys)[kJoyKeyActions][kJoyKeySlots]) {
    std::memcpy(s_joy_keys, keys, sizeof(s_joy_keys));
}

void usb_map_set_modifiers(const ModifierRoles& roles) {
    s_roles = roles;
}

void usb_map_set_mouse(const MouseTuning& t) {
    s_mouse = t;
    if (s_mouse.speed == 0) s_mouse.speed = 1;
    s_mouse_rem_x   = 0;
    s_mouse_rem_y   = 0;
    s_both_held     = false;
    s_autoswap_done = false;
}

bool usb_map_mouse_swapped() {
    return s_mouse.swap_buttons;
}

ModifierRoles usb_map_modifiers() {
    return s_roles;
}

uint32_t usb_map_unmapped_presses() {
    return s_unmapped;
}

KeyMode usb_map_key_mode() {
    return s_key_mode;
}

void usb_map_set_key_mode(KeyMode mode) {
    set_key_mode(mode);
}

void usb_map_keyboard_release_all() {
    // Устройство отключено: состояние сигнальных клавиш снимается, а
    // подать сигнал снова сможет только отпускание.
    s_signals       = Signals{};
    s_signals_armed = false;
    // Зажатое направление иначе осталось бы висеть на джойстике.
    joystick_set_keys(0);
    keyboard_reset();
}

void SOUNDSINTH_HOT_PATH(usb_map_mouse)(const ReportMap& map, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return;

    // Кнопки по карте: порядок HID - левая, правая, средняя.
    const bool down_0 = map.button_count > 0 && report_button_read(map.button[0], report, len);
    const bool down_1 = map.button_count > 1 && report_button_read(map.button[1], report, len);
    const bool down_2 = map.button_count > 2 && report_button_read(map.button[2], report, len);

    int32_t dx       = 0;
    int32_t dy       = 0;
    const bool got_x = report_field_read(map.field(Axis::X), report, len, dx);
    const bool got_y = report_field_read(map.field(Axis::Y), report, len, dy);
    // Отчёт не того номера: он не про мышь, состояние не трогаем. Жест тоже:
    // чужой отчёт не значит, что кнопки отпустили.
    if (!got_x && !got_y) return;

    // Жест смотрит нажатое на самом деле, до обмена: иначе он опознавался бы
    // по тому, что сам и меняет.
    note_both_buttons(down_0 && down_1);

    const uint8_t first  = s_mouse.swap_buttons ? kMouseButtonRight : kMouseButtonLeft;
    const uint8_t second = s_mouse.swap_buttons ? kMouseButtonLeft : kMouseButtonRight;
    uint8_t zx_buttons   = kMouseButtonsIdle;
    if (down_0) zx_buttons &= static_cast<uint8_t>(~first);
    if (down_1) zx_buttons &= static_cast<uint8_t>(~second);
    if (down_2) zx_buttons &= static_cast<uint8_t>(~kMouseButtonMiddle);

    mouse_set_buttons(zx_buttons);
    // Y у Kempston растёт вверх, у USB вниз. Приращение обрезается байтом:
    // счётчик у Kempston восьмиразрядный.
    dx = scale_with_remainder(dx, s_mouse_rem_x);
    dy = scale_with_remainder(dy, s_mouse_rem_y);
    mouse_move(clamp_i8(dx), clamp_i8(-dy));

    // Колесо - в старшую половину порта кнопок.
    int32_t wheel = 0;
    if (report_field_read(map.field(Axis::Wheel), report, len, wheel)) {
        mouse_wheel(clamp_i8(wheel));
    }
}

namespace {

// Направление по оси: середина считается из логических границ, мёртвая
// зона - их восьмая часть. Поэтому правило одно на ось любой разрядности.
uint8_t axis_bits(const Field& f, int32_t value, uint8_t low_bit, uint8_t high_bit) {
    if (!f.present() || f.logical_max <= f.logical_min) return 0;
    const int32_t centre = f.logical_min + (f.logical_max - f.logical_min) / 2;
    const int32_t dead   = (f.logical_max - f.logical_min) / 8;
    if (value < centre - dead) return low_bit;
    if (value > centre + dead) return high_bit;
    return 0;
}

// Шляпка: восемь положений по часовой стрелке от "вверх", всё прочее -
// покой. Логический минимум сдвигает отсчёт: у одних геймпадов он ноль, у
// других единица.
uint8_t hat_bits(const Field& f, int32_t value) {
    const int32_t dir = value - f.logical_min;
    if (dir < 0 || dir > 7) return 0;
    switch (dir) {
        case 0:
            return kJoyUp;
        case 1:
            return static_cast<uint8_t>(kJoyUp | kJoyRight);
        case 2:
            return kJoyRight;
        case 3:
            return static_cast<uint8_t>(kJoyDown | kJoyRight);
        case 4:
            return kJoyDown;
        case 5:
            return static_cast<uint8_t>(kJoyDown | kJoyLeft);
        case 6:
            return kJoyLeft;
        default:
            return static_cast<uint8_t>(kJoyUp | kJoyLeft);
    }
}

} // namespace

void usb_map_gamepad(const ReportMap& map, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return;

    uint8_t bits = 0;
    bool seen    = false;

    const Field& fx = map.field(Axis::X);
    const Field& fy = map.field(Axis::Y);
    int32_t v       = 0;
    if (report_field_read(fx, report, len, v)) {
        bits |= axis_bits(fx, v, kJoyLeft, kJoyRight);
        seen  = true;
    }
    if (report_field_read(fy, report, len, v)) {
        bits |= axis_bits(fy, v, kJoyUp, kJoyDown);
        seen  = true;
    }
    // Шляпка дополняет оси, а не заменяет: на геймпадах с обоими работают оба.
    if (report_field_read(map.hat, report, len, v)) {
        bits |= hat_bits(map.hat, v);
        seen  = true;
    }

    for (uint8_t i = 0; i < map.button_count; ++i) {
        if (!report_button_read(map.button[i], report, len)) continue;
        bits |= kJoyFire;
        seen  = true;
        break;
    }

    // Отчёт не того номера: ни одно поле не прочиталось, состояние прежнее.
    if (!seen) return;
    joystick_set_pad(bits);
}

void usb_map_gamepad_release_all() {
    joystick_set_pad(0);
}

} // namespace devices::hid
