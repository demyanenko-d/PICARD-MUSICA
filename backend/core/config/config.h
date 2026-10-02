// SPDX-License-Identifier: MIT
#pragma once

// Настройки платы: что включено и куда подключено.
//
// Платформы не касаются: разбор, сборка и выбор блока проверяются на ПК.
// Где лежат блоки и чем они пишутся, знает порт.

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

// Умолчания - набор для TS-Config: у неё есть своя память, свой интерфейс
// карты, своя клавиатура и мышь, и всё это плата трогать не должна.
// Остаётся то, ради чего её ставят: синтезатор, General Sound и живой
// MIDI. Машине с DivMMC или TR-DOS остальное включают файлом.
//
// Поля только добавлять в конец: переставленное или переосмысленное поле
// требует нового kFormatVersion и стирает настройки у всех.
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
};
static_assert(sizeof(Settings) == 20, "settings fields are only appended at the end");

// Клавиатура включена, когда выбрана раскладка.
inline bool keyboard_enabled(const Settings& s) {
    return s.keyboard_layout != static_cast<uint8_t>(Layout::None);
}

// Пользуется ли кто-нибудь USB по настройкам. Подъём стека этим больше не
// управляет - он безусловный, иначе выключенная клавиатура закрывала бы и
// вход в конфигуратор, то есть единственный способ её вернуть. Остаётся
// как ответ на вопрос "зачем он поднят" в журнале и в проверках.
inline bool usb_host_used(const Settings& s) {
    return keyboard_enabled(s) || s.usb_mouse != 0 || s.usb_gamepad != 0 || s.disksys_media == static_cast<uint8_t>(Media::Usb) ||
           s.zcontroller_media == static_cast<uint8_t>(Media::Usb);
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
