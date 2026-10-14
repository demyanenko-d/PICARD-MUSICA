// SPDX-License-Identifier: MIT
#pragma once

// Настройки платы: что включено и куда подключено.
//
// Платформы не касаются: разбор, сборка и выбор блока проверяются на ПК.
// Где лежат блоки и чем они пишутся, знает порт.

#include <cstddef>
#include <cstdint>

namespace soundsinth::config {

// Номер формата. Поднимать, когда меняется СМЫСЛ уже записанного поля или
// его место: блок с чужим номером считается непригодным, и на его месте
// оказываются умолчания, то есть прежние настройки теряются.
//
// Дописанные в конец поля номера не требуют: длина payload лежит в
// заголовке блока, и короткий блок читается с умолчаниями в новых полях.
// Расширение набора значений поля тоже не требует, пока прежние значения
// означают прежнее.
inline constexpr uint16_t kFormatVersion = 1;

// Разведение каналов MOD, проценты от аппаратной разводки Amiga.
inline constexpr uint8_t kModPanningMax = 100;

// Носитель, с которым работает эмулятор карты.
enum class Media : uint8_t { Sd = 0, Usb = 1 };

// Дисковая система машины. Две сразу не работают: DivMMC подставляет
// память и ПЗУ, TR-DOS ведёт сигнал DOS_N по выборке команды.
//
// Нули и единицы в уже записанных блоках означают то же самое, поэтому
// номера формата эти значения не меняли.
enum class DiskSys : uint8_t { None = 0, DivMmc = 1, TrDos = 2 };

// Откуда приходит живой MIDI. Hook - записи машины в порт AY, Wire -
// провод на входе UART.
//
// Нули и единицы в уже записанных блоках означают то же самое.
enum class LiveMidi : uint8_t { None = 0, Hook = 1, Wire = 2 };

// Раскладка клавиатуры. None выключает клавиатуру: отдельного признака
// "включена" нет, раскладка и есть выбор.
enum class Layout : uint8_t { None = 0, Picard = 1 };

// Чем модификатор становится на матрице ZX. Числа совпадают с
// devices::hid::ModRole, сверка стоит там, где настройки применяются.
enum class ModRole : uint8_t { None = 0, CapsShift = 1, SymbolShift = 2 };

// Скорость мыши - множитель приращения в четвертях: 4 значит один к
// одному, 2 - вдвое медленнее, 8 - вдвое быстрее.
inline constexpr uint8_t kMouseSpeedUnit = 4;
inline constexpr uint8_t kMouseSpeedMin  = 1;
inline constexpr uint8_t kMouseSpeedMax  = 32;

// Действия джойстика Kempston, по разряду порта: вправо, влево, вниз,
// вверх, огонь и три кнопки восьмиразрядного варианта.
inline constexpr uint32_t kJoyActions = 8;
// Клавиш на действие. Код ноль - клавиши нет.
inline constexpr uint32_t kJoyKeysPerAction = 2;
// Esc не привязывается: в конфигураторе он отменяет действие, в том числе
// саму привязку.
inline constexpr uint8_t kKeyEsc = 0x29;
// Коды HID, которые привязываются: от A до точки цифровой клавиатуры.
inline constexpr uint8_t kKeyCodeMin = 0x04;
inline constexpr uint8_t kKeyCodeMax = 0x63;

// Умолчания - набор для TS-Config: у неё есть своя память, свой интерфейс
// карты, своя клавиатура и мышь, и всё это плата трогать не должна.
// Остаётся то, ради чего её ставят: синтезатор, General Sound и живой
// MIDI. Машине с DivMMC или TR-DOS остальное включают файлом.
//
// Первые kLegacyBytes байт лежат во флеше одним куском: переставленное или
// переосмысленное поле там требует нового kFormatVersion и стирает
// настройки у всех. Поля за ними хранятся записями по номеру.
struct Settings {
    uint8_t disksys           = static_cast<uint8_t>(DiskSys::None);
    uint8_t disksys_media     = static_cast<uint8_t>(Media::Sd);
    uint8_t zcontroller       = 0;
    uint8_t zcontroller_media = static_cast<uint8_t>(Media::Sd);
    uint8_t gs                = 1;
    uint8_t live_midi         = static_cast<uint8_t>(LiveMidi::Hook);
    uint8_t reset_signal      = 0;

    uint8_t keyboard_layout = static_cast<uint8_t>(Layout::None);
    // Левого Shift здесь нет и не будет: он выбирает верхний знак клавиши,
    // а не роль на матрице.
    uint8_t mod_right_shift = static_cast<uint8_t>(ModRole::CapsShift);
    uint8_t mod_left_ctrl   = static_cast<uint8_t>(ModRole::SymbolShift);
    uint8_t mod_right_ctrl  = static_cast<uint8_t>(ModRole::SymbolShift);
    uint8_t mod_left_alt    = static_cast<uint8_t>(ModRole::None);
    uint8_t mod_right_alt   = static_cast<uint8_t>(ModRole::None);

    uint8_t usb_mouse          = 0;
    uint8_t mouse_speed        = kMouseSpeedUnit;
    uint8_t mouse_swap_buttons = 0;
    uint8_t usb_gamepad        = 0;
    // Поведения за этим ключом пока нет: он хранится и печатается, чтобы
    // формат не пришлось менять, когда регулировка появится.
    uint8_t auto_volume = 0;

    // Журнал платы, 115200 бод.
    uint8_t log = 1;
    // Wi-Fi забирает GPIO34, 38 и 39, а с ними блок UART1 целиком.
    uint8_t wifi = 0;

    // Глубина разведения каналов MOD, проценты от аппаратной разводки
    // Amiga: каналы 0 и 3 до упора налево, 1 и 2 направо. 100 - как
    // железо, 0 - всё по центру. Прочие форматы задают панораму в файле.
    uint8_t mod_panning = kModPanningMax;

    // Эффекты General Sound: отнимать канал у модуля, как настоящая карта
    // (четыре канала на всё, эффект глушит игравшее в канале), или играть
    // своими голосами. Умолчание - как у карты: игры на это рассчитаны.
    uint8_t gs_fx_separate = 0;

    // Разрешение обмена кнопок мыши жестом: обе зажаты полсекунды - левая и
    // правая меняются местами до выключения. По умолчанию выключен: обе
    // кнопки разом нажимают и в играх.
    uint8_t mouse_autoswap = 0;

    // Свободный байт неподвижной части.
    uint8_t reserved_[1] = {};

    // --- Записи переменной длины ---

    // Kempston восьмиразрядный: разряды 5..7 - ещё три кнопки. В
    // пятиразрядном они всегда нули, как у простого интерфейса.
    uint8_t joy_8bit = 0;

    // Клавиши действий джойстика в режиме Win+F2: коды HID, по строке на
    // действие в порядке разрядов порта.
    uint8_t joy_keys[kJoyActions][kJoyKeysPerAction] = {
        {0x4f, 0}, {0x50, 0}, {0x51, 0}, {0x52, 0}, {0x2c, 0x28}, {0, 0}, {0, 0}, {0, 0},
    };
};
inline constexpr uint32_t kLegacyBytes = 24;
static_assert(offsetof(Settings, joy_8bit) == kLegacyBytes, "the fixed part of the block keeps its 24 bytes");

// Клавиатура включена, когда выбрана раскладка.
inline bool keyboard_enabled(const Settings& s) {
    return s.keyboard_layout != static_cast<uint8_t>(Layout::None);
}

// Нужен ли носитель USB: его берёт включённый эмулятор. Ключ носителя у
// выключенного эмулятора остаётся в настройках (конфигуратор поле прячет,
// но значение хранит) и сам по себе флешку не требует.
inline bool usb_medium_needed(const Settings& s) {
    return (s.disksys == static_cast<uint8_t>(DiskSys::DivMmc) && s.disksys_media == static_cast<uint8_t>(Media::Usb)) ||
           (s.zcontroller != 0 && s.zcontroller_media == static_cast<uint8_t>(Media::Usb));
}

// Пользуется ли кто-нибудь USB по настройкам. В обычном режиме стек
// поднимается только тогда: его обработка гасит прерывания на ядре шины. В
// конфигураторе стек поднимается всегда - управление с клавиатуры USB
// нужно и при выключенной в настройках клавиатуре.
inline bool usb_host_used(const Settings& s) {
    return keyboard_enabled(s) || s.usb_mouse != 0 || s.usb_gamepad != 0 || usb_medium_needed(s);
}

// Живой MIDI по проводу и Wi-Fi упираются в один вывод приёма: UART1
// целиком уходит Wi-Fi, а единственный оставшийся приёмник UART0 - это
// GPIO33, вход MIDI.
inline bool settings_conflict(const Settings& s) {
    return s.wifi != 0 && s.live_midi == static_cast<uint8_t>(LiveMidi::Wire);
}

// DivMMC подставляет машине ПЗУ и память, и настройки эмулятора карты
// имеют смысл только при нём.
inline bool divmmc_selected(const Settings& s) {
    return s.disksys == static_cast<uint8_t>(DiskSys::DivMmc);
}

// Привести значения к допустимым. Блок из флеша мог быть записан прошивкой
// с другими пределами, а файл пишет человек.
//
// Здесь же принуждение по зависимостям: поле, которое при нынешнем наборе
// не имеет смысла или обязано стоять в одном значении, приводится к нему.
// Правила одни для файла с карты и для правки с экрана - разойдись они,
// плата принимала бы из двух источников разное.
void settings_clamp(Settings& s);

} // namespace soundsinth::config
