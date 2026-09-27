#include "devices/hid/usb_map.h"

#include <array>

#include "devices/hal/host_signals.h"
#include "devices/hid/joystick.h"
#include "devices/hid/keyboard.h"
#include "devices/hid/mouse.h"

namespace devices::hid {
namespace {

constexpr uint8_t kNoKey = 0xFF;

// Клавиша матрицы одним байтом: строка в старших разрядах, разряд в
// младших.
constexpr uint8_t zx(uint8_t row, uint8_t bit) {
    return static_cast<uint8_t>((row << 3) | bit);
}

constexpr uint8_t kCapsShift = zx(0, 0);
constexpr uint8_t kSymShift = zx(7, 1);

// Две клавиши на код: у ZX половина знаков набирается с CAPS или SYMBOL
// SHIFT, и нажимать их надо вместе.
struct Combo {
    uint8_t first;
    uint8_t second;
};

// Таблица вдвое длиннее числа кодов: вторая половина - то же с нажатым
// ЛЕВЫМ Shift. Так отображается не клавиша, а набранный знак, и верхний
// регистр берётся сам, без ручных комбинаций на каждый случай.
constexpr uint16_t kShifted = 0x100;

constexpr std::array<Combo, 512> build_keymap() {
    std::array<Combo, 512> m{};
    for (Combo& c : m) c = Combo{kNoKey, kNoKey};

    // Буквы: коды 0x04..0x1D идут по латинскому алфавиту.
    m[0x04] = {zx(1, 0), kNoKey}; // A
    m[0x05] = {zx(7, 4), kNoKey}; // B
    m[0x06] = {zx(0, 3), kNoKey}; // C
    m[0x07] = {zx(1, 2), kNoKey}; // D
    m[0x08] = {zx(2, 2), kNoKey}; // E
    m[0x09] = {zx(1, 3), kNoKey}; // F
    m[0x0A] = {zx(1, 4), kNoKey}; // G
    m[0x0B] = {zx(6, 4), kNoKey}; // H
    m[0x0C] = {zx(5, 2), kNoKey}; // I
    m[0x0D] = {zx(6, 3), kNoKey}; // J
    m[0x0E] = {zx(6, 2), kNoKey}; // K
    m[0x0F] = {zx(6, 1), kNoKey}; // L
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
    m[0x1A] = {zx(2, 1), kNoKey}; // W
    m[0x1B] = {zx(0, 2), kNoKey}; // X
    m[0x1C] = {zx(5, 4), kNoKey}; // Y
    m[0x1D] = {zx(0, 1), kNoKey}; // Z

    // Цифры: 1..5 в своей строке слева направо, 6..0 в соседней справа
    // налево - как на самой машине.
    m[0x1E] = {zx(3, 0), kNoKey}; // 1
    m[0x1F] = {zx(3, 1), kNoKey}; // 2
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
    m[0x2A] = {kCapsShift, zx(4, 0)};  // Backspace - DELETE
    m[0x2B] = {kCapsShift, kSymShift}; // Tab - расширенный режим
    m[0x2C] = {zx(7, 0), kNoKey};      // пробел

    // Знаки набираются с SYMBOL SHIFT.
    m[0x2D] = {kSymShift, zx(6, 3)}; // минус
    m[0x2E] = {kSymShift, zx(6, 1)}; // равно
    m[0x33] = {kSymShift, zx(5, 1)}; // точка с запятой
    m[0x34] = {kSymShift, zx(4, 3)}; // апостроф
    m[0x36] = {kSymShift, zx(7, 3)}; // запятая
    m[0x37] = {kSymShift, zx(7, 2)}; // точка
    m[0x38] = {kSymShift, zx(0, 4)}; // косая черта

    // Стрелки - с CAPS SHIFT, как на резиновой клавиатуре.
    m[0x4F] = {kCapsShift, zx(4, 2)}; // вправо - 8
    m[0x50] = {kCapsShift, zx(3, 4)}; // влево - 5
    m[0x51] = {kCapsShift, zx(4, 4)}; // вниз - 6
    m[0x52] = {kCapsShift, zx(4, 3)}; // вверх - 7

    // Скобки и обратная косая.
    m[0x2F] = {kSymShift, zx(5, 4)}; // [ - SS+Y
    m[0x30] = {kSymShift, zx(5, 3)}; // ] - SS+U
    m[0x31] = {kSymShift, zx(1, 2)}; // \ - SS+D

    // Цифровая клавиатура: цифры сами собой, знаки как на основной.
    m[0x59] = {zx(3, 0), kNoKey}; // 1
    m[0x5A] = {zx(3, 1), kNoKey}; // 2
    m[0x5B] = {zx(3, 2), kNoKey}; // 3
    m[0x5C] = {zx(3, 3), kNoKey}; // 4
    m[0x5D] = {zx(3, 4), kNoKey}; // 5
    m[0x5E] = {zx(4, 4), kNoKey}; // 6
    m[0x5F] = {zx(4, 3), kNoKey}; // 7
    m[0x60] = {zx(4, 2), kNoKey}; // 8
    m[0x61] = {zx(4, 1), kNoKey}; // 9
    m[0x62] = {zx(4, 0), kNoKey}; // 0
    m[0x58] = {zx(6, 0), kNoKey}; // Enter
    m[0x63] = {kSymShift, zx(7, 2)}; // . - SS+M
    m[0x54] = {kSymShift, zx(0, 4)}; // / - SS+V
    m[0x55] = {kSymShift, zx(7, 4)}; // * - SS+B
    m[0x56] = {kSymShift, zx(6, 3)}; // - - SS+J
    m[0x57] = {kSymShift, zx(6, 2)}; // + - SS+K

    // Клавиши правки: как на Spectrum+.
    m[0x39] = {kCapsShift, zx(3, 1)}; // Caps Lock - CS+2
    m[0x4B] = {kCapsShift, zx(3, 2)}; // PgUp - CS+3, TRUE VIDEO
    m[0x4E] = {kCapsShift, zx(3, 3)}; // PgDn - CS+4, INV VIDEO
    m[0x4C] = {kCapsShift, zx(4, 1)}; // Delete - CS+9, GRAPH
    m[0x35] = {kCapsShift, zx(3, 0)}; // ` - CS+1, EDIT

    // Знаки сравнения: на ПК их нечем набрать одной клавишей, поэтому они
    // сидят на трёх клавишах правки, которые машине иначе не нужны.
    m[0x4A] = {kSymShift, zx(2, 0)}; // Home - SS+Q, <=
    m[0x49] = {kSymShift, zx(2, 1)}; // Insert - SS+W, <>
    m[0x4D] = {kSymShift, zx(2, 2)}; // End - SS+E, >=

    // --- Верхний регистр: то же с левым Shift ---
    //
    // Знаки взяты по раскладке US: что напечатано на верхней грани
    // клавиши, то и набирается. Без этой половины знаки верхнего регистра
    // недостижимы вовсе.
    m[kShifted | 0x1E] = {kSymShift, zx(3, 0)}; // ! - SS+1
    m[kShifted | 0x1F] = {kSymShift, zx(3, 1)}; // @ - SS+2
    m[kShifted | 0x20] = {kSymShift, zx(3, 2)}; // # - SS+3
    m[kShifted | 0x21] = {kSymShift, zx(3, 3)}; // $ - SS+4
    m[kShifted | 0x22] = {kSymShift, zx(3, 4)}; // % - SS+5
    m[kShifted | 0x23] = {kSymShift, zx(6, 4)}; // ^ - SS+H
    m[kShifted | 0x24] = {kSymShift, zx(4, 4)}; // & - SS+6
    m[kShifted | 0x25] = {kSymShift, zx(7, 4)}; // * - SS+B
    m[kShifted | 0x26] = {kSymShift, zx(4, 2)}; // ( - SS+8
    m[kShifted | 0x27] = {kSymShift, zx(4, 1)}; // ) - SS+9
    m[kShifted | 0x2D] = {kSymShift, zx(4, 0)}; // _ - SS+0
    m[kShifted | 0x2E] = {kSymShift, zx(6, 2)}; // + - SS+K
    m[kShifted | 0x2F] = {kSymShift, zx(1, 3)}; // { - SS+F
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

// Разряды байта модификаторов загрузочного отчёта: слева Ctrl, Shift,
// Alt, Gui, затем те же справа.
constexpr uint8_t kModLeftCtrl = 0x01;
constexpr uint8_t kModLeftShift = 0x02;
constexpr uint8_t kModLeftAlt = 0x04;
constexpr uint8_t kModRightCtrl = 0x10;
constexpr uint8_t kModRightShift = 0x20;
constexpr uint8_t kModRightAlt = 0x40;

// Назначения HID тех же клавиш.
constexpr uint8_t kUsageLeftShift = 0xE1;

ModifierRoles s_roles = ModifierRoles{};

// --- Сигнальные клавиши ---
//
// Стоят выше раскладки: в матрице машины их нет ни в одной, поэтому
// назначить их клавишам нельзя, а дёрнуть линии больше нечем. Сигнал
// уходит на нажатие; пока клавиша держится, повтора нет.
constexpr uint8_t kCodeNmi = 0x44;    // F11
constexpr uint8_t kCodeReset = 0x45;  // F12
constexpr uint8_t kCodeDelete = 0x4C; // в тройке Ctrl+Alt+Delete

struct Signals {
    bool nmi;
    bool reset;
    bool del;
    bool ctrl;
    bool alt;
};

Signals s_signals = {false, false, false, false, false};

// Пока не видели отчёта без сигнальных клавиш, ни один сигнал не подаётся.
//
// Полный сброс перезапускает плату, состояние не переживает перезапуск, и
// клавиатура перечисляется заново. Держи пользователь тройку до конца -
// первый же отчёт после подъёма подал бы сброс снова, и так по кругу.
// Поэтому сигнал взводится только отпусканием.
bool s_signals_armed = false;

// Полный сброс - тройкой, как на ПК: одной клавишей такое не вешают.
bool hard_combo(const Signals& s) {
    return s.ctrl && s.alt && s.del;
}


void note_signal(Signals& now, uint8_t code) {
    if (code == kCodeNmi) now.nmi = true;
    if (code == kCodeReset) now.reset = true;
    if (code == kCodeDelete) now.del = true;
}

void fire_signals(const Signals& now) {
    const bool quiet = !now.nmi && !now.reset && !hard_combo(now);
    if (!s_signals_armed) {
        // Клавиши ещё не отпускали с подключения: сигнал не подаём, но
        // состояние помним - иначе отпускание не станет фронтом.
        s_signals_armed = quiet;
        s_signals = now;
        return;
    }
    if (hard_combo(now) && !hard_combo(s_signals)) hal::host_hard_reset_request();
    if (now.nmi && !s_signals.nmi) hal::host_nmi_request();
    if (now.reset && !s_signals.reset) hal::host_reset_request();
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
    rows[row] = static_cast<uint8_t>(rows[row] & ~(1u << bit));
}

bool pressed(const uint8_t rows[kKeyboardRows], uint8_t key) {
    return (rows[key >> 3] & (1u << (key & 0x07))) == 0u;
}

void press_role(uint8_t rows[kKeyboardRows], ModRole role) {
    if (role == ModRole::CapsShift) press(rows, kCapsShift);
    if (role == ModRole::SymbolShift) press(rows, kSymShift);
}

} // namespace

namespace {

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
    press(rows, c.first);
    press(rows, c.second);
    return true;
}

// Модификатор в матрицу по настроенной роли. Левый Shift сюда не
// попадает: он не клавиша, а выбор верхнего знака.
void press_modifier(uint8_t rows[kKeyboardRows], uint8_t usage) {
    switch (usage) {
        case 0xE0: press_role(rows, s_roles.left_ctrl); break;
        case 0xE2: press_role(rows, s_roles.left_alt); break;
        case 0xE4: press_role(rows, s_roles.right_ctrl); break;
        case 0xE5: press_role(rows, s_roles.right_shift); break;
        case 0xE6: press_role(rows, s_roles.right_alt); break;
        default: break; // клавиши Gui машине неизвестны
    }
}

// Левый Shift, которому не нашлось верхнего знака, становится CAPS SHIFT:
// выходит заглавная буква, как и ждёшь. С SYMBOL SHIFT он не складывается
// - это уже набранный знак.
void finish_shift(uint8_t rows[kKeyboardRows], bool shifted) {
    if (shifted && !pressed(rows, kSymShift)) press(rows, kCapsShift);
}

// Разбор по дескриптору: модификаторы разрядами, коды слотами массива.
// Так читаются и клавиатуры, которые не перешли в загрузочный протокол, и
// те, у кого слотов больше шести.
bool keyboard_from_map(const ReportMap& map, const uint8_t* report, uint16_t len) {
    const KeyboardMap& k = map.keyboard;
    if (!k.present()) return false;
    // Обычная клавиатура читается загрузочной раскладкой: она задана
    // спецификацией и у всех одна. Дескриптор нужен только тем, кто шлёт
    // отчёты с номерами - к таким загрузочная раскладка неприменима, там
    // первый байт номер, а не модификаторы.
    if (!map.uses_report_ids) return false;

    uint8_t rows[kKeyboardRows];
    for (uint8_t& r : rows) r = 0xFF;

    bool shifted = false;
    Signals signals = {false, false, false, false, false};
    for (uint8_t i = 0; i < k.modifier_count; ++i) {
        if (!report_button_read(k.modifier[i], report, len)) continue;
        const uint8_t usage = k.modifier_usage[i];
        if (usage == 0xE0u || usage == 0xE4u) signals.ctrl = true;
        if (usage == 0xE2u || usage == 0xE6u) signals.alt = true;
        if (usage == kUsageLeftShift) {
            shifted = true;
            continue;
        }
        press_modifier(rows, usage);
    }

    bool any = false;
    for (uint8_t slot = 0; slot < k.slot_count; ++slot) {
        uint8_t code = 0;
        if (!report_key_read(k, slot, report, len, code)) continue;
        any = true;
        note_signal(signals, code);
        // Тройка полного сброса в матрицу не идёт: Delete сам по себе
        // нужен, а в тройке он часть сигнала.
        if (code == kCodeDelete && signals.ctrl && signals.alt) continue;
        if (!press_code(rows, code, shifted)) return true; // переполнение: отчёт пропускаем
    }
    // Ни один слот не прочитался - отчёт не этого устройства.
    if (!any) return true;

    fire_signals(signals);
    finish_shift(rows, shifted);
    keyboard_set_rows(rows);
    return true;
}

} // namespace

void usb_map_keyboard(const ReportMap& map, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return;
    if (keyboard_from_map(map, report, len)) return;

    // Дескриптор не разобрался: остаётся загрузочная раскладка - байт
    // модификаторов, байт запаса, шесть кодов. Она задана спецификацией.
    if (len < 8) return;

    uint8_t rows[kKeyboardRows];
    for (uint8_t& r : rows) r = 0xFF;

    const uint8_t mods = report[0];
    const bool shifted = (mods & kModLeftShift) != 0u;
    if ((mods & kModRightShift) != 0u) press_role(rows, s_roles.right_shift);
    if ((mods & kModLeftCtrl) != 0u) press_role(rows, s_roles.left_ctrl);
    if ((mods & kModRightCtrl) != 0u) press_role(rows, s_roles.right_ctrl);
    if ((mods & kModLeftAlt) != 0u) press_role(rows, s_roles.left_alt);
    if ((mods & kModRightAlt) != 0u) press_role(rows, s_roles.right_alt);

    Signals signals = {false, false, false, false, false};
    signals.ctrl = (mods & (kModLeftCtrl | kModRightCtrl)) != 0u;
    signals.alt = (mods & (kModLeftAlt | kModRightAlt)) != 0u;
    for (uint16_t i = 2; i < 8; ++i) {
        note_signal(signals, report[i]);
        if (report[i] == kCodeDelete && signals.ctrl && signals.alt) continue;
        if (!press_code(rows, report[i], shifted)) return;
    }

    fire_signals(signals);
    finish_shift(rows, shifted);
    keyboard_set_rows(rows);
}

ModifierRoles usb_map_default_modifiers() {
    return ModifierRoles{};
}

void usb_map_set_modifiers(const ModifierRoles& roles) {
    s_roles = roles;
}

ModifierRoles usb_map_modifiers() {
    return s_roles;
}

void usb_map_keyboard_release_all() {
    // Клавиатуру выдернули с зажатой F11 - следующее нажатие обязано
    // сработать, иначе сигнал пропадёт до перезагрузки.
    s_signals = Signals{false, false, false, false, false};
    // Подключат снова - сигнал подаст только отпускание: иначе зажатая
    // тройка при перечислении сбрасывала бы плату по кругу.
    s_signals_armed = false;
    keyboard_reset();
}

void usb_map_mouse(const ReportMap& map, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return;

    // Кнопки по карте: порядок HID - левая, правая, средняя.
    uint8_t zx_buttons = kMouseButtonsIdle;
    if (map.button_count > 0 && report_button_read(map.button[0], report, len)) {
        zx_buttons &= static_cast<uint8_t>(~kMouseButtonLeft);
    }
    if (map.button_count > 1 && report_button_read(map.button[1], report, len)) {
        zx_buttons &= static_cast<uint8_t>(~kMouseButtonRight);
    }
    if (map.button_count > 2 && report_button_read(map.button[2], report, len)) {
        zx_buttons &= static_cast<uint8_t>(~kMouseButtonMiddle);
    }

    int32_t dx = 0;
    int32_t dy = 0;
    const bool got_x = report_field_read(map.axis[static_cast<uint8_t>(Axis::X)], report, len, dx);
    const bool got_y = report_field_read(map.axis[static_cast<uint8_t>(Axis::Y)], report, len, dy);
    // Отчёт не того номера: он не про мышь, состояние не трогаем.
    if (!got_x && !got_y) return;

    mouse_set_buttons(zx_buttons);
    // Y у Kempston растёт вверх, у USB вниз. Приращение обрезается байтом:
    // счётчик у Kempston восьмиразрядный.
    mouse_move(clamp_i8(dx), clamp_i8(-dy));
}

namespace {

// Направление по оси: середина считается из логических границ, мёртвая
// зона - их восьмая часть. Поэтому правило одно на ось любой разрядности.
uint8_t axis_bits(const Field& f, int32_t value, uint8_t low_bit, uint8_t high_bit) {
    if (!f.present() || f.logical_max <= f.logical_min) return 0;
    const int32_t centre = f.logical_min + (f.logical_max - f.logical_min) / 2;
    const int32_t dead = (f.logical_max - f.logical_min) / 8;
    if (value < centre - dead) return low_bit;
    if (value > centre + dead) return high_bit;
    return 0;
}

// Шляпка: восемь положений по часовой стрелке от "вверх", всё прочее -
// покой. Логический минимум сдвигает отсчёт: у одних пультов он ноль, у
// других единица.
uint8_t hat_bits(const Field& f, int32_t value) {
    const int32_t dir = value - f.logical_min;
    if (dir < 0 || dir > 7) return 0;
    switch (dir) {
        case 0: return kJoyUp;
        case 1: return static_cast<uint8_t>(kJoyUp | kJoyRight);
        case 2: return kJoyRight;
        case 3: return static_cast<uint8_t>(kJoyDown | kJoyRight);
        case 4: return kJoyDown;
        case 5: return static_cast<uint8_t>(kJoyDown | kJoyLeft);
        case 6: return kJoyLeft;
        default: return static_cast<uint8_t>(kJoyUp | kJoyLeft);
    }
}

} // namespace

void usb_map_gamepad(const ReportMap& map, const uint8_t* report, uint16_t len) {
    if (report == nullptr) return;

    uint8_t bits = 0;
    bool seen = false;

    const Field& fx = map.axis[static_cast<uint8_t>(Axis::X)];
    const Field& fy = map.axis[static_cast<uint8_t>(Axis::Y)];
    int32_t v = 0;
    if (report_field_read(fx, report, len, v)) {
        bits |= axis_bits(fx, v, kJoyLeft, kJoyRight);
        seen = true;
    }
    if (report_field_read(fy, report, len, v)) {
        bits |= axis_bits(fy, v, kJoyUp, kJoyDown);
        seen = true;
    }
    // Шляпка дополняет оси, а не заменяет: на пультах с обоими работают оба.
    if (report_field_read(map.hat, report, len, v)) {
        bits |= hat_bits(map.hat, v);
        seen = true;
    }

    for (uint8_t i = 0; i < map.button_count; ++i) {
        if (!report_button_read(map.button[i], report, len)) continue;
        bits |= kJoyFire;
        seen = true;
        break;
    }

    // Отчёт не того номера: ни одно поле не прочиталось, состояние прежнее.
    if (!seen) return;
    joystick_set(bits);
}

void usb_map_gamepad_release_all() {
    joystick_set(0);
}

} // namespace devices::hid
