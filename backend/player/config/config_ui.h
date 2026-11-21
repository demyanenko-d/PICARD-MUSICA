// SPDX-License-Identifier: MIT
#pragma once

// Меню конфигуратора на плате: дерево экранов, окна выбора, правка полей.
// Рисует в сетку терминала (config_term), нажатия берёт кодами клавиш ПЗУ.
//
// Живёт только в режиме конфигуратора: память - зона TrackScratch, код -
// во флеше.

#include <cstdint>

#include "core/config/config.h"
#include "player/config/config_page.h"
#include "player/config/config_term.h"
#include "player/config/config_tools.h"

namespace soundsinth::config {

// Чего меню хочет от платы: клавишу USB для привязки и уход в перезагрузку.
struct ConfigUiHost {
    void (*capture_arm)();
    void (*capture_cancel)();
    bool (*capture_take)(uint8_t& code);
};

enum class ConfigUiExit : uint8_t { None, Discard, Save };

class ConfigUi {
public:
    // saved - настройки из флеша (по ним помечаются изменённые), files -
    // файлы ПЗУ на карте, tools - экран инструментов в странице (его
    // пишет служба инструментов), nullptr - раздела TOOLS нет.
    void start(const Settings& saved, const ConfigPageFiles& files, const ConfigUiHost& host, ToolArea* tools);
    // Код клавиши ПЗУ: 8..11 стрелки, 12 забой, 13 ввод, 27 ESC, знаки.
    void key(uint8_t k);
    // Обслуживание без нажатий: захват клавиши USB, закрытие инструмента.
    void poll();
    // Меняется, когда меняется экран: кадр стоит рисовать заново.
    uint32_t stamp() const;
    void render(TermGrid& g) const;
    ConfigUiExit exit() const { return exit_; }
    const Settings& draft() const { return draft_; }

private:
    enum class Dlg : uint8_t { None, List, Number, Key, Text, Tool };
    static constexpr uint32_t kDepthMax = 4;
    static constexpr uint32_t kTextMax  = 64;

    void open_menu(uint8_t m);
    void first_sel();
    void move_cursor(int step);
    void activate();
    void go_back();
    void edit_field(uint8_t f);
    void set_value(uint8_t f, uint8_t v);
    void set_text(uint8_t f, const char* s);
    void list_key(uint8_t k);
    void number_key(uint8_t k);
    void text_key(uint8_t k);
    void key_dialog_key(uint8_t k);
    void close_dialog();
    void tool_key(uint8_t k);

    bool active(uint8_t f) const;
    bool changed(uint8_t f) const;
    const char* option(uint32_t n) const;

    Settings saved_;
    Settings draft_;
    const ConfigPageFiles* files_ = nullptr;
    ConfigUiHost host_{};
    ConfigUiExit exit_ = ConfigUiExit::None;
    ToolArea* tools_   = nullptr;
    uint32_t changes_  = 0;

    uint8_t depth_ = 0;
    uint8_t st_menu_[kDepthMax]{};
    uint8_t st_cursor_[kDepthMax]{};
    uint8_t menu_   = 0;
    uint8_t cursor_ = 0;
    uint8_t col_    = 0;

    Dlg dlg_       = Dlg::None;
    uint8_t field_ = 0; // правимое поле
    uint8_t value_ = 0; // число в окне числа
    // Окно списка: варианты перечисления или файлы.
    uint8_t d_count_ = 0;
    uint8_t d_sel_   = 0;
    uint8_t d_top_   = 0;
    bool d_files_    = false;
    char text_[kTextMax + 1]{};
    uint8_t text_len_ = 0;
};

} // namespace soundsinth::config
