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

// Носитель устройства карты; None - устройство выключено. Числа хранятся в
// блоке настроек: Sd и Usb - прежние, новые носители дописываются в конец.
enum class Media : uint8_t { Sd = 0, Usb = 1, None = 2 };

// Печатный знак ASCII: пробел..тильда.
inline constexpr bool ascii_printable(uint8_t c) {
    return c >= 0x20 && c <= 0x7e;
}

// Дисковая система машины, одна на выбор. DivMMC подставляет память и ПЗУ,
// TR-DOS ведёт DOS_N по выборке команды, BDI - то же и ВГ93 с #FF на плате.
// Числа хранятся в блоке настроек и не меняются.
enum class DiskSys : uint8_t { None = 0, DivMmc = 1, TrDos = 2 };

// Частота SD-карты: номер в kSdSpeedHz. Делители SPI 12..4 от clk_peri 150 МГц.
inline constexpr uint32_t kSdSpeedHz[]   = {12500000u, 15000000u, 18750000u, 25000000u, 37500000u};
inline constexpr uint8_t kSdSpeedDefault = 3; // 25 МГц
inline constexpr uint8_t kSdSpeedCount   = 5;

// Откуда машина берёт ПЗУ: своё или образ из области флеша платы.
enum class RomSource : uint8_t { Board = 0, Slot1 = 1, Slot2 = 2, Slot3 = 3 };
inline constexpr uint32_t kRomSlots = 3;
// Имя файла образа в корне карты, знаков без завершающего нуля.
inline constexpr uint32_t kRomFileMax = 31;

// Откуда приходит живой MIDI. Hook - записи машины в порт AY, Wire -
// провод на входе UART.
//
// Нули и единицы в уже записанных блоках означают то же самое.
enum class LiveMidi : uint8_t { None = 0, Hook = 1, Wire = 2 };
// Живой AY: чип и TurboSound - два YM2149 с выбором чипа #FE/#FF в #FFFD.
enum class AyEmulation : uint8_t { None = 0, Ay8910 = 1, Ym2149 = 2, TsYm2149 = 3, TsfmYm2203 = 4 };
// Раскладка каналов живого AY. ACB - как у Пентагона.
enum class AyStereo : uint8_t { Acb = 0, Abc = 1, Mono = 2 };
// Бипер: разряд 4 порта #FE (EAR) или с ним разряд 3 (MIC), четыре уровня ULA.
// Бипер порта #FE: однобитный - бит 4 (EAR); двухбитный - бит 4 и бит 3
// (MIC, выход на магнитофон) тише в 10 раз, как на выводе ULA 48K.
enum class Beeper : uint8_t { None = 0, OneBit = 1, TwoBit = 2 };

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
// Esc и забой не привязываются: в конфигураторе они отменяют действие, в
// том числе саму привязку.
inline constexpr uint8_t kKeyEsc       = 0x29;
inline constexpr uint8_t kKeyBackspace = 0x2a;
// Коды HID, которые привязываются: от A до точки цифровой клавиатуры.
inline constexpr uint8_t kKeyCodeMin = 0x04;
inline constexpr uint8_t kKeyCodeMax = 0x63;

// Сеть Wi-Fi: имя до 32 знаков, пароль WPA2 до 63. Печатный ASCII,
// пустая строка - не задано.
inline constexpr uint32_t kWifiSsidMax = 32;
inline constexpr uint32_t kWifiPassMax = 63;

// Умолчания - набор для TS-Config: у неё есть своя память, свой интерфейс
// карты, своя клавиатура и мышь, и всё это плата трогать не должна.
// Остаётся то, ради чего её ставят: синтезатор, General Sound и живой
// MIDI. Машине с DivMMC или TR-DOS остальное включают файлом.
//
// Первые kLegacyBytes байт лежат во флеше одним куском: переставленное или
// переосмысленное поле там требует нового kFormatVersion и стирает
// настройки у всех. Поля за ними хранятся записями по номеру.
struct Settings {
    uint8_t disksys = static_cast<uint8_t>(DiskSys::None);
    // Носитель DivMMC, флаг и носитель Z-Controller прежних прошивок. Не
    // ключи: из них выводятся divmmc_card1 и zcontroller, когда блок
    // записан без них (config_store).
    uint8_t legacy_divmmc_media      = static_cast<uint8_t>(Media::Sd);
    uint8_t legacy_zcontroller       = 0;
    uint8_t legacy_zcontroller_media = static_cast<uint8_t>(Media::Sd);
    uint8_t gs                       = 1;
    uint8_t live_midi                = static_cast<uint8_t>(LiveMidi::Hook);
    uint8_t reset_signal             = 0;

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
    // Авторегулировка громкости треков из файла: шина ведёт средний уровень
    // трека к одной цели в пределах от -12 до +18 дБ.
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

    // 0 - ПЗУ машины, 1..3 - образ Пентагона 64 КБ из области флеша с этим
    // номером. Только без DISKSYS=DIVMMC. В блоках прежних версий здесь ноль.
    uint8_t rom = static_cast<uint8_t>(RomSource::Board);

    // --- Записи переменной длины ---

    // Kempston восьмиразрядный: разряды 5..7 - ещё три кнопки. В
    // пятиразрядном они всегда нули, как у простого интерфейса.
    uint8_t joy_8bit = 0;

    // Клавиши действий джойстика в режиме Win+F2: коды HID, по строке на
    // действие в порядке разрядов порта.
    uint8_t joy_keys[kJoyActions][kJoyKeysPerAction] = {
        {0x4f, 0}, {0x50, 0}, {0x51, 0}, {0x52, 0}, {0x2c, 0x28}, {0, 0}, {0, 0}, {0, 0},
    };

    // Сеть для WIFI=1. Строки с завершающим нулём.
    char wifi_ssid[kWifiSsidMax + 1] = {};
    char wifi_pass[kWifiPassMax + 1] = {};

    // Файлы своих ПЗУ в корне карты, по области. Плата сверяет файл с
    // областью при загрузке и при расхождении прошивает её.
    char rom_file[kRomSlots][kRomFileMax + 1] = {};

    // Интерполяция сэмплов MOD: 1 - линейная, как у PC-трекеров; 0 -
    // лесенка, как у Paula Амиги: отсчёт держится до следующего, верх ярче.
    // Прочие форматы - всегда линейная.
    uint8_t mod_interpolation = 1;

    // Частота SD-карты, номер в kSdSpeedHz.
    uint8_t sd_speed = kSdSpeedDefault;

    // Эмуляция AY на плате по записям в #FFFD/#BFFD (AyEmulation): один чип,
    // TurboSound из двух YM2149 или TurboSound FM из двух YM2203. На чтение
    // #FFFD отвечает плата - регистром выбранного чипа. Родной AY машины
    // звучит дальше и на чтении ведёт шину вместе с платой.
    uint8_t ay_emulation = 0;
    uint8_t ay_stereo    = static_cast<uint8_t>(AyStereo::Acb);

    // Эмуляция SAA1099 по схеме MultiSound: записи в #1FF и #FF, такт чипа
    // разрешает запись 1111 0xxx в #FFFD. На шину не отвечает.
    uint8_t saa1099 = 0;

    // Цифровой звук машины, на шину не отвечает. Covox - #FB в оба канала;
    // Soundrive 1.05 режима 1 - #0F, #1F слева, #4F, #5F справа; бипер -
    // Beeper.
    uint8_t covox     = 0;
    uint8_t soundrive = 0;
    uint8_t beeper    = static_cast<uint8_t>(Beeper::None);

    // Сброс машины останавливает музыку, играющую без хоста: GS (карта
    // сбрасывается), трекерную, MIDI с карты и живой MIDI.
    uint8_t stop_music_on_reset = 1;

    // Устройства карты - носитель (Media), None выключает. Карты DivMMC на
    // портах 0xE7/0xEB, выбор битами 0 и 1 порта 0xE7, при любой DISKSYS;
    // при DIVMMC хотя бы одна включена (settings_clamp). Z-Controller - порты
    // 0x57/0x77.
    uint8_t divmmc_card1 = static_cast<uint8_t>(Media::None);
    uint8_t divmmc_card2 = static_cast<uint8_t>(Media::None);
    uint8_t zcontroller  = static_cast<uint8_t>(Media::None);
};
// Тактовая эмулируемого AY: Пентагон.
inline constexpr uint32_t kAyClockHz   = 1750000;
inline constexpr uint32_t kLegacyBytes = 24;
static_assert(offsetof(Settings, joy_8bit) == kLegacyBytes, "the fixed part of the block keeps its 24 bytes");

// Клавиатура включена, когда выбрана раскладка.
inline bool keyboard_enabled(const Settings& s) {
    return s.keyboard_layout != static_cast<uint8_t>(Layout::None);
}

// Устройство карты включено: носитель выбран.
inline bool device_on(uint8_t media) {
    return media != static_cast<uint8_t>(Media::None);
}

// Нужен ли носитель USB: его выбрало хотя бы одно устройство карты.
inline bool usb_medium_needed(const Settings& s) {
    constexpr auto kUsb = static_cast<uint8_t>(Media::Usb);
    return s.divmmc_card1 == kUsb || s.divmmc_card2 == kUsb || s.zcontroller == kUsb;
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

// Своё ПЗУ платы - при любой дисковой системе, кроме DivMMC: тот подставляет
// своё ПЗУ сам.
inline bool custom_rom_allowed(const Settings& s) {
    return s.disksys != static_cast<uint8_t>(DiskSys::DivMmc);
}

// Номер области флеша с выбранным ПЗУ, 0 - ПЗУ машины.
inline uint32_t custom_rom_slot(const Settings& s) {
    return custom_rom_allowed(s) ? s.rom : 0u;
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
