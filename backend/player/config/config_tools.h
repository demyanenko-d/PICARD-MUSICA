// SPDX-License-Identifier: MIT
#pragma once

// Раздел TOOLS конфигуратора: экран инструмента в странице настроек.
//
// ПЗУ только рисует: 17 строк по 30 знаков, цвет на строку, курсор.
// Нажатия оно пересылает плате кольцом на восемь ячеек. Плата
// пишет текст и двигает счётчик обновлений, по нему ПЗУ перерисовывает
// экран. Новый инструмент - код платы и пункт меню, ПЗУ не меняется.
//
// Писатель экрана один: ядро, которое обслуживает открытый инструмент.
// Сведения и проверка дисков - Core1, модуль Wi-Fi - Core0.

#include <cstdarg>
#include <cstdint>

#include "platform/usb_host.h"

#include "player/config/config_page.h"

namespace soundsinth::config {

// Номер инструмента. Числа уходят в страницу как есть.
enum class ToolId : uint8_t {
    None       = 0,
    CardInfo   = 1,
    BoardInfo  = 2,
    UsbInfo    = 3,
    CardCheck  = 4,
    UsbCheck   = 5,
    AtTerminal = 6,
    CardSpeed  = 7,
    UsbHid     = 8,
};
inline constexpr uint8_t kToolCount = 9;

// Терминал модуля Wi-Fi живёт в задаче модуля на Core0.
inline constexpr bool tool_on_wifi_core(ToolId t) {
    return t == ToolId::AtTerminal;
}

// Ready - ESC закрывает экран. Busy - ESC уходит плате, экран не
// закрывается. Closed - плата сама закрыла экран.
enum class ToolState : uint8_t { Ready = 0, Busy = 1, Closed = 2 };

// Коды нажатий: коды клавиш ПЗУ, плюс два служебных.
inline constexpr uint8_t kToolKeyLeft  = 8;
inline constexpr uint8_t kToolKeyRight = 9;
inline constexpr uint8_t kToolKeyDown  = 10;
inline constexpr uint8_t kToolKeyUp    = 11;
inline constexpr uint8_t kToolKeyDel   = 12;
inline constexpr uint8_t kToolKeyEnter = 13;
inline constexpr uint8_t kToolKeyEsc   = 27;
inline constexpr uint8_t kToolKeyOpen  = 0xf0; // экран открыт, номер в tool
inline constexpr uint8_t kToolKeyClose = 0xf1; // экран закрыт по ESC

inline constexpr uint32_t kToolOffset = 0x1dc0; // от начала страницы; машина видит 0x3dc0, конец страницы
inline constexpr uint32_t kToolCols   = 30;
// Текста в строке: по клетке отступа от рамки слева и справа.
inline constexpr uint32_t kToolTextCols  = kToolCols - 2u;
inline constexpr uint32_t kToolLines     = 17;
inline constexpr uint32_t kToolBodyLines = 14; // над чертой; под ней три строки подсказки
inline constexpr uint32_t kToolHintLine  = kToolBodyLines;
inline constexpr uint32_t kToolKeySlots  = 8;
inline constexpr uint8_t kToolNoCursor   = 0xff;

// Цвета строк - атрибуты Спектрума на синей бумаге.
inline constexpr uint8_t kToolInk    = 0x0f; // белый
inline constexpr uint8_t kToolYellow = 0x0e;
inline constexpr uint8_t kToolCyan   = 0x0d;
inline constexpr uint8_t kToolGreen  = 0x0c;
inline constexpr uint8_t kToolBar    = 0x32; // выбранная строка: красный по жёлтому
inline constexpr uint8_t kToolAlert  = 0x17; // белый по красному

struct ToolArea {
    uint8_t req;     // ПЗУ: сколько нажатий положено, по модулю 256
    uint8_t ack;     // плата: сколько взято
    uint8_t tool;    // ПЗУ: ToolId открытого экрана
    uint8_t state;   // плата: ToolState
    uint8_t upd;     // плата: растёт после записи текста
    uint8_t cur_row; // плата: курсор, kToolNoCursor - нет
    uint8_t cur_col;
    uint8_t pad0;
    uint8_t keys[kToolKeySlots];
    uint8_t attr[kToolLines];
    uint8_t pad1[7];
    char text[kToolLines][kToolCols]; // без нулей, добито пробелами
};
static_assert(sizeof(ToolArea) == 0x28 + kToolLines * kToolCols, "the tool area layout is frozen");
static_assert(kToolOffset + sizeof(ToolArea) <= kConfigPageBytes, "the tool area lies inside the page");

// Писатель экрана поверх страницы.
class ToolScreen {
public:
    explicit ToolScreen(uint8_t* page) : a_(reinterpret_cast<ToolArea*>(page + kToolOffset)) {}

    ToolId tool() const;
    // Следующее нажатие экрана mine. false - нажатий нет или открыт уже
    // другой инструмент: его нажатия берёт его владелец, ack пишет один.
    bool key(uint8_t& k, ToolId mine);
    // Пустой экран: пробелы, белый, без курсора.
    void clear();
    // Строка с отступом в клетку, не длиннее kToolTextCols.
    void put(uint32_t line, const char* s, uint8_t attr = kToolInk);
    void putf(uint32_t line, uint8_t attr, const char* fmt, ...);
    void vputf(uint32_t line, uint8_t attr, const char* fmt, va_list ap);
    // Полоса хода: [#####.....] 45%.
    void progress(uint32_t line, uint32_t done, uint32_t total);
    // col - знак строки, отступ учтён.
    void cursor(uint32_t line, uint32_t col);
    void no_cursor();
    void state(ToolState s);
    ToolState state() const;
    // Текст готов: ПЗУ перерисует экран.
    void publish();

    ToolArea& area() { return *a_; }

private:
    ToolArea* a_;
};

// Обнулить область: зовёт сборка страницы.
void tool_area_reset(uint8_t* page);

// --- Сведения: числа собирает порт, вывод общий ---

// Ёмкость в байтах коротко: 512 B, 512 KB, 7580 MB, 29.7 GB.
void tool_size_text(char* out, uint32_t cap, uint64_t bytes);

struct CardFacts {
    bool present;
    bool sdhc;
    uint32_t sectors;
    bool cid_valid;
    uint8_t cid[16];
};

// Сведения о файловой системе носителя: из загрузочного сектора и FSINFO.
struct VolumeFacts {
    bool valid;
    uint8_t fat_bits; // 12, 16, 32
    uint32_t cluster_bytes;
    uint32_t clusters;
    uint32_t fsinfo_free; // 0xffffffff - неизвестно
    char label[12];
};

void tool_render_card(ToolScreen& scr, const CardFacts& card, const VolumeFacts& vol);

struct BoardFacts {
    const char* version;
    uint8_t chip_revision;
    uint32_t sys_hz;
    uint32_t flash_bytes;
    uint32_t psram_bytes;
    uint32_t uptime_s;
    uint8_t rom_slots_used; // занятых слотов своего ПЗУ из трёх
};

void tool_render_board(ToolScreen& scr, const BoardFacts& b);

void tool_render_usb(ToolScreen& scr, const platform::UsbDevice* dev, uint32_t count);

} // namespace soundsinth::config
