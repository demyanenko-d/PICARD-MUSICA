// SPDX-License-Identifier: MIT
#include "player/config/config_ui.h"

#include <atomic>
#include <cstring>

#include "core/config/config_fields.h"
#include "player/config/config_menu.h"

namespace soundsinth::config {

namespace {

using T = MenuItemType;

// Цвета: чернила | бумага << 3.
constexpr uint8_t A_N       = 0x0f; // светло-серый по синему
constexpr uint8_t A_DIM     = 0x08; // недоступное: чёрный по синему
constexpr uint8_t A_CHG     = 0x0e; // изменённое значение: жёлтый
constexpr uint8_t A_SUB     = 0x0d; // знак подменю: голубой
constexpr uint8_t A_BAR     = 0x32; // полоса курсора: красный по жёлтому
constexpr uint8_t A_BAR_DIM = 0x30; // полоса на недоступном: чёрный по жёлтому
constexpr uint8_t A_CELL    = 0x17; // выбранная ячейка, строка окна: белый по красному
constexpr uint8_t A_ARROW   = 0x10; // стрелки выбранной строки окна: чёрный по красному
constexpr uint8_t A_DLG     = 0x38; // окно: чёрный по серому
constexpr uint8_t A_HDR     = 0x0d; // шапка таблицы

// Строки экрана.
constexpr uint32_t kRowFrameTop = 1;
constexpr uint32_t kRowList     = 2;
constexpr uint32_t kListRows    = 16;
constexpr uint32_t kRowSep      = 18;
constexpr uint32_t kRowHelp     = 19;
constexpr uint32_t kHelpRows    = 3;
constexpr uint32_t kRowFrameBot = 22;
constexpr uint32_t kRowFoot     = 23;
constexpr uint32_t kInner       = 30;

// Знаки CP866.
constexpr uint8_t CH_SEL_L = 0x10, CH_SEL_R = 0x11;
constexpr uint8_t CH_UP = 0x18, CH_DOWN = 0x19, CH_RIGHT = 0x1a, CH_LEFT = 0x1b;
constexpr uint8_t CH_V = 0xba, CH_H = 0xcd, CH_TL = 0xc9, CH_TR = 0xbb, CH_BL = 0xc8, CH_BR = 0xbc;
constexpr uint8_t CH_VL = 0xc7, CH_VR = 0xb6, CH_H1 = 0xc4;

constexpr uint8_t KEY_LEFT = 8, KEY_RIGHT = 9, KEY_DOWN = 10, KEY_UP = 11, KEY_DEL = 12, KEY_ENTER = 13, KEY_ESC = 27, KEY_SPACE = 32;
constexpr uint8_t kHidEsc = 0x29, kHidBackspace = 0x2a;

// Таблица клавиш: индексы в строке от x = 1.
constexpr uint32_t TB_CELL1 = 8, TB_CELL2 = 19, TB_CELL_W = 9;

// Окно.
constexpr uint32_t DLG_BODY  = 3;
constexpr uint32_t DLG_W_MAX = kInner - 2;
constexpr uint32_t DLG_ROWS  = 5;
constexpr uint32_t TEXT_VIEW = DLG_W_MAX - 4;

const char kFootMain[]  = "\x18\x19-MOVE  ENTER/SPACE-SELECT";
const char kFootSub[]   = "\x18\x19-MOVE  ENTER-SELECT  ESC-BACK";
const char kFootTable[] = "\x18\x19\x1b\x1a ENTER-SET DEL-CLR ESC-BACK";
const char kFootDlg[]   = "\x18\x19-MOVE  ENTER-OK  ESC-CANCEL";
const char kFootNum[]   = "\x1b\x1a\x18\x19-CHANGE ENTER-OK ESC-CANCEL";
const char kFootText[]  = "TYPE  ENTER-OK  ESC-CANCEL";
const char kFootKey[]   = "PRESS A KEY ON USB  ESC-CANCEL";

// Вывод в сетку: место, цвет, сдвиг вправо без переноса.
struct Pen {
    TermGrid& g;
    uint32_t x = 0, y = 0;
    uint8_t attr = A_N;
    void at(uint32_t nx, uint32_t ny) {
        x = nx;
        y = ny;
    }
    void putc(uint8_t c) {
        if (x < kTermCols && y < kTermRows) {
            g.ch[y][x] = c;
            g.at[y][x] = attr;
        }
        ++x;
    }
    void repeat(uint8_t c, uint32_t n) {
        while (n-- != 0) {
            putc(c);
        }
    }
    void spaces(uint32_t n) { repeat(' ', n); }
    // Не длиннее n; возвращает выведенное.
    uint32_t puts_max(const char* s, uint32_t n) {
        uint32_t k = 0;
        for (; s != nullptr && s[k] != 0 && k < n; ++k) {
            putc(static_cast<uint8_t>(s[k]));
        }
        return k;
    }
    void puts_pad(const char* s, uint32_t n) { spaces(n - puts_max(s, n)); }
    void attr_run(uint32_t nx, uint32_t ny, uint32_t n, uint8_t a) {
        for (uint32_t i = 0; i < n && nx + i < kTermCols; ++i) {
            g.at[ny][nx + i] = a;
        }
    }
};

uint32_t len_of(const char* s) {
    return s == nullptr ? 0u : static_cast<uint32_t>(std::strlen(s));
}

void frame_row(Pen& p, uint32_t y, uint8_t l, uint8_t r, uint8_t fill) {
    p.at(0, y);
    p.putc(l);
    p.repeat(fill, kInner);
    p.putc(r);
}

// Строка списка: 30 знаков с x = 1.
struct RowBuf {
    char b[kInner];
    void clear() { std::memset(b, ' ', sizeof(b)); }
    // Строку с индекса e, не длиннее n; возвращает легло.
    uint32_t left(const char* s, uint32_t e, uint32_t n) {
        uint32_t k = 0;
        for (; s != nullptr && s[k] != 0 && k < n && e + k < kInner; ++k) {
            b[e + k] = s[k];
        }
        return k;
    }
    void print(Pen& p, uint32_t y, uint8_t a) {
        p.attr = a;
        p.at(1, y);
        for (char c : b) {
            p.putc(static_cast<uint8_t>(c));
        }
    }
};

// Окно: рамка, заголовок по центру, черта, пустые строки, низ.
struct Box {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    void place(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        x = (32u - w) / 2u;
        y = (24u - h) / 2u;
    }
    void draw(Pen& p, const char* title) const {
        p.attr = A_DLG;
        p.at(x, y);
        p.putc(CH_TL);
        p.repeat(CH_H, w - 2u);
        p.putc(CH_TR);
        p.at(x, y + 1u);
        p.putc(CH_V);
        const uint32_t len  = len_of(title);
        const uint32_t room = w - 2u > len ? w - 2u - len : 0u;
        p.spaces(room / 2u);
        p.puts_max(title, w - 2u);
        p.spaces(room - room / 2u);
        p.putc(CH_V);
        p.at(x, y + 2u);
        p.putc(CH_VL);
        p.repeat(CH_H1, w - 2u);
        p.putc(CH_VR);
        uint32_t r = y + DLG_BODY;
        for (; r < y + h - 1u; ++r) {
            p.at(x, r);
            p.putc(CH_V);
            p.spaces(w - 2u);
            p.putc(CH_V);
        }
        p.at(x, r);
        p.putc(CH_BL);
        p.repeat(CH_H, w - 2u);
        p.putc(CH_BR);
    }
};

void footer(Pen& p, const char* s) {
    p.attr = A_N;
    p.at(0, kRowFoot);
    p.puts_pad(s, 32);
}

const MenuItemDesc& item_of(uint8_t menu, uint8_t i) {
    return kMenus[menu].items[i];
}

uint8_t item_field(const MenuItemDesc& it) {
    return it.key == nullptr ? kNoField : config_field_by_name(it.key);
}

// На пункт курсор встаёт: не пустая строка и не шапка.
bool stops(const MenuItemDesc& it) {
    return it.type != T::Blank && it.type != T::Header;
}

// Значение поля текстом.
const char* value_text(const Settings& s, uint8_t f, char* buf) {
    const PageFieldInfo fi = config_field_info(f);
    const uint8_t v        = config_field_value(s, f);
    switch (fi.page_kind) {
        case PageKind::Flag:
            return v != 0 ? "ON" : "OFF";
        case PageKind::Text:
        case PageKind::File: {
            const char* t = config_field_text(s, f);
            if (t[0] == 0) return "-";
            if (fi.min == 0) return t;
            // Секрет - звёздочками по длине.
            const uint32_t n = len_of(t);
            std::memset(buf, '*', n);
            buf[n] = 0;
            return buf;
        }
        case PageKind::Key:
            if (v == 0) return "-";
            [[fallthrough]];
        case PageKind::Choice: {
            const NameTable t = names_of(fi.kind);
            for (uint32_t n = 0; n < t.count; ++n) {
                if (t.items[n].value == v) return config_choice_name(s, fi.kind, n);
            }
            break;
        }
        default:
            break;
    }
    // Число десятичным без ведущих нулей.
    char* q = buf;
    if (v >= 100) *q++ = static_cast<char>('0' + v / 100);
    if (v >= 10) *q++ = static_cast<char>('0' + v / 10 % 10);
    *q++ = static_cast<char>('0' + v % 10);
    *q   = 0;
    return buf;
}

} // namespace

void ConfigUi::start(const Settings& saved, const ConfigPageFiles& files, const ConfigUiHost& host, ToolArea* tools) {
    tools_ = tools;
    ++changes_;
    saved_ = saved;
    draft_ = saved;
    files_ = &files;
    host_  = host;
    exit_  = ConfigUiExit::None;
    depth_ = 0;
    col_   = 0;
    dlg_   = Dlg::None;
    open_menu(kMenuMain);
    first_sel();
}

bool ConfigUi::active(uint8_t f) const {
    return f == kNoField || (f < 64u && ((settings_active_mask(draft_) >> f) & 1u) != 0);
}

bool ConfigUi::changed(uint8_t f) const {
    const char* a = config_field_text(draft_, f);
    if (a != nullptr) return std::strcmp(a, config_field_text(saved_, f)) != 0;
    return config_field_value(draft_, f) != config_field_value(saved_, f);
}

void ConfigUi::open_menu(uint8_t m) {
    menu_ = m;
}

// Первый пункт, на котором можно стоять при входе: не пустая строка, не
// шапка и не назад.
void ConfigUi::first_sel() {
    const MenuDesc& m = kMenus[menu_];
    for (uint8_t i = 0; i < m.count; ++i) {
        const T t = m.items[i].type;
        if (t != T::Blank && t != T::Header && t != T::Back) {
            cursor_ = i;
            return;
        }
    }
    cursor_ = 0;
}

void ConfigUi::move_cursor(int step) {
    const MenuDesc& m = kMenus[menu_];
    for (int i = cursor_ + step; i >= 0 && i < m.count; i += step) {
        if (stops(m.items[i])) {
            cursor_ = static_cast<uint8_t>(i);
            return;
        }
    }
}

void ConfigUi::go_back() {
    if (depth_ == 0) return;
    --depth_;
    open_menu(st_menu_[depth_]);
    cursor_ = st_cursor_[depth_];
    col_    = 0;
}

void ConfigUi::set_value(uint8_t f, uint8_t v) {
    config_field_set(draft_, f, v);
    settings_clamp(draft_);
}

void ConfigUi::set_text(uint8_t f, const char* s) {
    char* dst              = config_field_text(draft_, f);
    const PageFieldInfo fi = config_field_info(f);
    std::memset(dst, 0, fi.max + 1u);
    std::strncpy(dst, s, fi.max);
    settings_clamp(draft_);
}

void ConfigUi::activate() {
    const MenuItemDesc& it = item_of(menu_, cursor_);
    switch (it.type) {
        case T::Back:
            go_back();
            break;
        case T::Submenu:
            if (depth_ >= kDepthMax) break;
            st_menu_[depth_]   = menu_;
            st_cursor_[depth_] = cursor_;
            ++depth_;
            open_menu(it.menu);
            first_sel();
            col_ = 0;
            break;
        case T::Save:
            exit_ = ConfigUiExit::Save;
            break;
        case T::Exit:
            exit_ = ConfigUiExit::Discard;
            break;
        case T::Tool:
            // Как ПЗУ: номер и состояние, затем нажатие "открыт".
            if (tools_ == nullptr) break;
            tools_->tool  = it.menu;
            tools_->state = static_cast<uint8_t>(ToolState::Ready);
            field_        = cursor_;
            dlg_          = Dlg::Tool;
            tool_key(kToolKeyOpen);
            break;
        case T::KeyRow:
            edit_field(static_cast<uint8_t>(item_field(it) + col_));
            break;
        case T::Field:
            edit_field(item_field(it));
            break;
        default:
            break;
    }
}

void ConfigUi::edit_field(uint8_t f) {
    if (!active(f)) return;
    field_                 = f;
    const PageFieldInfo fi = config_field_info(f);
    const uint8_t v        = config_field_value(draft_, f);
    switch (fi.page_kind) {
        case PageKind::Flag:
            set_value(f, static_cast<uint8_t>(v ^ 1u));
            break;
        case PageKind::Choice: {
            const NameTable t = names_of(fi.kind);
            d_count_          = static_cast<uint8_t>(t.count);
            d_sel_            = 0;
            for (uint32_t n = 0; n < t.count; ++n) {
                if (t.items[n].value == v) d_sel_ = static_cast<uint8_t>(n);
            }
            d_files_ = false;
            dlg_     = Dlg::List;
            break;
        }
        case PageKind::File: {
            d_count_        = static_cast<uint8_t>(files_->count + 1u);
            d_sel_          = 0;
            const char* cur = config_field_text(draft_, f);
            for (uint32_t n = 1; n < d_count_; ++n) {
                if (std::strcmp(files_->name[n - 1u], cur) == 0) {
                    d_sel_ = static_cast<uint8_t>(n);
                    break;
                }
            }
            d_files_ = true;
            dlg_     = Dlg::List;
            break;
        }
        case PageKind::Number:
            value_ = v;
            dlg_   = Dlg::Number;
            break;
        case PageKind::Key:
            if (host_.capture_arm != nullptr) host_.capture_arm();
            dlg_ = Dlg::Key;
            break;
        case PageKind::Text: {
            const char* t = config_field_text(draft_, f);
            text_len_     = static_cast<uint8_t>(len_of(t) < kTextMax ? len_of(t) : kTextMax);
            std::memcpy(text_, t, text_len_);
            dlg_ = Dlg::Text;
            break;
        }
    }
    if (dlg_ != Dlg::List) return;
    // Первая видимая - так, чтобы текущая была в окне.
    const uint32_t vis = d_count_ < DLG_ROWS ? d_count_ : DLG_ROWS;
    d_top_             = d_sel_ >= vis ? static_cast<uint8_t>(d_sel_ - vis + 1u) : 0u;
}

const char* ConfigUi::option(uint32_t n) const {
    if (d_files_) return n == 0 ? "NONE" : files_->name[n - 1u];
    return config_choice_name(draft_, config_field_info(field_).kind, n);
}

void ConfigUi::close_dialog() {
    dlg_ = Dlg::None;
}

void ConfigUi::list_key(uint8_t k) {
    const uint32_t vis = d_count_ < DLG_ROWS ? d_count_ : DLG_ROWS;
    if (k == KEY_UP && d_sel_ > 0) {
        --d_sel_;
        if (d_sel_ < d_top_) d_top_ = d_sel_;
    } else if (k == KEY_DOWN && d_sel_ + 1u < d_count_) {
        ++d_sel_;
        if (d_sel_ >= d_top_ + vis) ++d_top_;
    } else if (k == KEY_ENTER || k == KEY_SPACE) {
        if (d_files_) {
            set_text(field_, d_sel_ == 0 ? "" : files_->name[d_sel_ - 1u]);
        } else {
            const NameTable t = names_of(config_field_info(field_).kind);
            if (t.items != nullptr && d_sel_ < t.count) set_value(field_, t.items[d_sel_].value);
        }
        close_dialog();
    } else if (k == KEY_ESC || k == KEY_DEL) {
        close_dialog();
    }
}

// Число: стрелки влево и вправо по одному, вверх и вниз по десять.
void ConfigUi::number_key(uint8_t k) {
    const PageFieldInfo fi = config_field_info(field_);
    const uint32_t step    = k == KEY_LEFT || k == KEY_RIGHT ? 1u : 10u;
    if (k == KEY_RIGHT || k == KEY_UP) {
        const uint32_t v = value_ + step;
        value_           = static_cast<uint8_t>(v > 255u || v > fi.max ? fi.max : v);
    } else if (k == KEY_LEFT || k == KEY_DOWN) {
        value_ = static_cast<uint8_t>(value_ < step || value_ - step < fi.min ? fi.min : value_ - step);
    } else if (k == KEY_ENTER || k == KEY_SPACE) {
        set_value(field_, value_);
        close_dialog();
    } else if (k == KEY_ESC || k == KEY_DEL) {
        close_dialog();
    }
}

void ConfigUi::text_key(uint8_t k) {
    const PageFieldInfo fi = config_field_info(field_);
    if (k == KEY_ESC) {
        close_dialog();
    } else if (k == KEY_ENTER) {
        text_[text_len_] = 0;
        set_text(field_, text_);
        close_dialog();
    } else if (k == KEY_DEL) {
        if (text_len_ != 0) --text_len_;
    } else if (k >= 0x20 && k < 0x7f && text_len_ < fi.max && text_len_ < kTextMax) {
        text_[text_len_++] = static_cast<char>(k);
    }
}

// Клавишу отменяют ESC и забой и с клавиатуры машины, и с USB.
void ConfigUi::key_dialog_key(uint8_t k) {
    if (k != KEY_ESC && k != KEY_DEL) return;
    if (host_.capture_cancel != nullptr) host_.capture_cancel();
    close_dialog();
}

// Нажатие инструменту: ячейка кольца, потом счётчик. Кольцо полно -
// нажатие теряется, как у ПЗУ.
void ConfigUi::tool_key(uint8_t k) {
    volatile ToolArea& a = *tools_;
    if (static_cast<uint8_t>(a.req - a.ack) >= kToolKeySlots) return;
    a.keys[a.req % kToolKeySlots] = k;
    // Инструмент, его состояние и ячейка видны потребителю раньше счётчика.
    std::atomic_thread_fence(std::memory_order_release);
    a.req = static_cast<uint8_t>(a.req + 1u);
}

uint32_t ConfigUi::stamp() const {
    if (dlg_ != Dlg::Tool) return changes_;
    const volatile ToolArea& a = *tools_;
    return changes_ + (static_cast<uint32_t>(a.upd) << 16) + (static_cast<uint32_t>(a.state) << 24);
}

void ConfigUi::poll() {
    if (dlg_ == Dlg::Tool) {
        // Плата сама закрыла экран.
        if (static_cast<volatile ToolArea*>(tools_)->state == static_cast<uint8_t>(ToolState::Closed)) {
            close_dialog();
            ++changes_;
        }
        return;
    }
    uint8_t code = 0;
    if (dlg_ != Dlg::Key || host_.capture_take == nullptr || !host_.capture_take(code)) return;
    if (code != kHidEsc && code != kHidBackspace) set_value(field_, code);
    close_dialog();
    ++changes_;
}

void ConfigUi::key(uint8_t k) {
    ++changes_;
    switch (dlg_) {
        case Dlg::Tool:
            // ESC закрывает экран, пока плата не занята; занятой уходит ей.
            if (k == KEY_ESC && static_cast<volatile ToolArea*>(tools_)->state != static_cast<uint8_t>(ToolState::Busy)) {
                tool_key(kToolKeyClose);
                close_dialog();
                return;
            }
            tool_key(k);
            return;
        case Dlg::List:
            list_key(k);
            return;
        case Dlg::Number:
            number_key(k);
            return;
        case Dlg::Text:
            text_key(k);
            return;
        case Dlg::Key:
            key_dialog_key(k);
            return;
        case Dlg::None:
            break;
    }
    const bool table = kMenus[menu_].kind == MenuKind::KeyTable;
    switch (k) {
        case KEY_UP:
            move_cursor(-1);
            return;
        case KEY_DOWN:
            move_cursor(1);
            return;
        case KEY_LEFT:
        case KEY_RIGHT:
            if (table) col_ = k == KEY_RIGHT ? 1u : 0u;
            return;
        case KEY_ENTER:
        case KEY_SPACE:
            activate();
            return;
        case KEY_ESC:
            go_back();
            return;
        case KEY_DEL: {
            // В таблице забой чистит клавишу, в остальных местах - назад.
            if (!table) {
                go_back();
                return;
            }
            const MenuItemDesc& it = item_of(menu_, cursor_);
            const uint8_t f        = static_cast<uint8_t>(item_field(it) + col_);
            if (it.type == T::KeyRow && active(f)) set_value(f, 0);
            return;
        }
        default:
            break;
    }
    if ((k | 0x20) == 's') exit_ = ConfigUiExit::Save;
    if ((k | 0x20) == 'q') exit_ = ConfigUiExit::Discard;
}

void ConfigUi::render(TermGrid& g) const {
    Pen p{g};
    const MenuDesc& m = kMenus[menu_];

    // Рамка, строка 0 - за ПЗУ.
    std::memset(g.ch[0], ' ', kTermCols);
    std::memset(g.at[0], A_N, kTermCols);
    p.attr = A_N;
    frame_row(p, kRowFrameTop, CH_TL, CH_TR, CH_H);
    frame_row(p, kRowFrameBot, CH_BL, CH_BR, CH_H);
    frame_row(p, kRowSep, CH_VL, CH_VR, CH_H1);
    for (uint32_t y = kRowList; y < kRowFrameBot; ++y) {
        if (y == kRowSep) continue;
        p.at(0, y);
        p.putc(CH_V);
        p.at(31, y);
        p.putc(CH_V);
    }
    RowBuf rb;
    rb.clear();
    if (depth_ == 0) {
        rb.print(p, kRowList, A_N);
    } else {
        // Имя подменю по центру, под ним черта и пустая строка.
        const char* title   = dlg_ == Dlg::Tool ? m.items[cursor_].label : m.title;
        const uint32_t len  = len_of(title) < kInner ? len_of(title) : kInner;
        const uint32_t left = (kInner - len) / 2u;
        p.at(1, kRowList);
        p.spaces(left);
        p.puts_max(title, kInner);
        p.spaces(kInner - len - left);
        frame_row(p, kRowList + 1u, CH_VL, CH_VR, CH_H1);
        rb.print(p, kRowList + 2u, A_N);
    }

    // Экран инструмента: 14 строк над чертой, 3 под ней, клетка курсора.
    if (dlg_ == Dlg::Tool) {
        // Пара барьеру publish у писателя экрана: upd прочитан в stamp.
        std::atomic_thread_fence(std::memory_order_acquire);
        const volatile ToolArea& a = *tools_;
        auto row                   = [](uint32_t i) { return i < kToolBodyLines ? kRowList + 2u + i : kRowHelp + i - kToolBodyLines; };
        for (uint32_t i = 0; i < kToolLines; ++i) {
            p.attr = a.attr[i];
            p.at(1, row(i));
            for (uint32_t c = 0; c < kToolCols; ++c) {
                p.putc(static_cast<uint8_t>(a.text[i][c]));
            }
        }
        if (a.cur_row < kToolLines) p.attr_run(a.cur_col + 1u, row(a.cur_row), 1, A_CELL);
        footer(p, "ESC-BACK");
        return;
    }

    // Список.
    const uint32_t top = depth_ == 0 ? kRowList + 1u : kRowList + 3u;
    char vb[kTextMax + 8];
    for (uint8_t i = 0; i < m.count; ++i) {
        const MenuItemDesc& it = m.items[i];
        const uint32_t y       = top + i;
        rb.clear();
        if (it.type == T::Header) {
            rb.left("KEY 1", TB_CELL1, TB_CELL_W);
            rb.left("KEY 2", TB_CELL2, TB_CELL_W);
            rb.print(p, y, A_HDR);
            continue;
        }
        const bool sel   = i == cursor_;
        const uint8_t f  = item_field(it);
        const bool act   = active(f);
        const uint8_t ra = sel ? (act ? A_BAR : A_BAR_DIM) : (act ? A_N : A_DIM);
        uint32_t v_start = 0, v_len = 0;
        if (it.type != T::Blank) {
            if (sel) {
                rb.b[0]          = static_cast<char>(CH_SEL_L);
                rb.b[kInner - 1] = static_cast<char>(CH_SEL_R);
            }
            const uint32_t lab = rb.left(it.label, 1, 22);
            const bool sub     = it.type == T::Submenu || it.type == T::Tool;
            if (sub) rb.b[kInner - 2] = '>';
            if (it.type == T::Field || (sub && f != kNoField)) {
                // Значение прижато вправо к краю перед ">", не левее метки.
                const char* v    = value_text(draft_, f, vb);
                uint32_t len     = len_of(v);
                const uint32_t c = kInner - 3u, e = lab + 2u;
                uint32_t start = c >= len ? c - len : 0u;
                if (c < len || start < e) {
                    len   = c - e;
                    start = e;
                }
                v_start = start;
                v_len   = len;
                std::memcpy(rb.b + start, v, len);
            } else if (it.type == T::KeyRow) {
                rb.left(value_text(draft_, f, vb), TB_CELL1, TB_CELL_W);
                rb.left(value_text(draft_, static_cast<uint8_t>(f + 1u), vb), TB_CELL2, TB_CELL_W);
                if (sel && act) {
                    const uint32_t b         = col_ == 0 ? TB_CELL1 - 1u : TB_CELL2 - 1u;
                    rb.b[b]                  = '[';
                    rb.b[b + TB_CELL_W + 1u] = ']';
                }
            }
        }
        rb.print(p, y, ra);
        // Пометки поверх: ячейка, изменённое значение, знак подменю.
        if (it.type == T::KeyRow) {
            if (sel) {
                if (act) p.attr_run(col_ == 0 ? TB_CELL1 : TB_CELL2, y, TB_CELL_W + 2u, A_CELL);
            } else {
                if (changed(f)) p.attr_run(TB_CELL1 + 1u, y, TB_CELL_W, A_CHG);
                if (changed(static_cast<uint8_t>(f + 1u))) p.attr_run(TB_CELL2 + 1u, y, TB_CELL_W, A_CHG);
            }
            continue;
        }
        if (sel) continue;
        if (it.type == T::Tool || it.type == T::Submenu) p.attr_run(kInner - 1u, y, 1, A_SUB);
        if (v_len != 0 && changed(f)) p.attr_run(v_start + 1u, y, v_len, A_CHG);
    }
    rb.clear();
    for (uint32_t y = top + m.count; y < kRowList + kListRows; ++y) {
        rb.print(p, y, A_N);
    }

    // Подсказка к пункту под курсором: три строки по 30, перенос по пробелу.
    const char* h = item_of(menu_, cursor_).help;
    if (h == nullptr) h = "";
    p.attr = A_N;
    for (uint32_t y = kRowHelp; y < kRowHelp + kHelpRows; ++y) {
        uint32_t b = 0, e = 0;
        for (; h[b] != 0; ++b) {
            if (b == kInner) {
                if (e != 0) b = e;
                break;
            }
            if (h[b] == ' ') e = b;
        }
        p.at(1, y);
        for (uint32_t i = 0; i < b; ++i) {
            p.putc(static_cast<uint8_t>(h[i]));
        }
        p.spaces(kInner - b);
        h += b;
        while (*h == ' ') {
            ++h;
        }
    }

    // Нижняя строка по месту.
    const char* foot = depth_ == 0 ? kFootMain : m.kind == MenuKind::KeyTable ? kFootTable : kFootSub;

    // Окно поверх.
    const MenuItemDesc& it = item_of(menu_, cursor_);
    Box b;
    switch (dlg_) {
        case Dlg::None:
        case Dlg::Tool:
            break;
        case Dlg::List: {
            uint32_t w = len_of(it.label) + 2u;
            for (uint32_t n = 0; n < d_count_; ++n) {
                if (len_of(option(n)) > w) w = len_of(option(n));
            }
            w += 4u;
            if (w > DLG_W_MAX) w = DLG_W_MAX;
            const uint32_t vis = d_count_ < DLG_ROWS ? d_count_ : DLG_ROWS;
            const uint32_t ind = d_count_ != vis ? 1u : 0u;
            b.place(w, 2u * ind + vis + DLG_BODY + 1u);
            b.draw(p, it.label);
            const uint32_t y0 = b.y + DLG_BODY + ind;
            p.attr            = A_DLG;
            for (uint32_t r = 0; r < vis; ++r) {
                p.at(b.x + 1u, y0 + r);
                p.putc(' ');
                p.puts_pad(option(d_top_ + r), b.w - 3u);
            }
            if (ind != 0) {
                // Выше есть ещё - черта над списком, ниже - под ним.
                const uint8_t marks[2] = {static_cast<uint8_t>(d_top_ != 0 ? '-' : ' '), static_cast<uint8_t>(d_top_ + vis < d_count_ ? '-' : ' ')};
                const uint32_t rows[2] = {y0 - 1u, y0 + vis};
                for (uint32_t k = 0; k < 2; ++k) {
                    p.at(b.x + 1u, rows[k]);
                    const uint32_t room = b.w - 5u;
                    p.spaces(room / 2u);
                    p.repeat(marks[k], 3);
                    p.spaces(room - room / 2u);
                }
            }
            const uint32_t ys = y0 + d_sel_ - d_top_;
            p.attr_run(b.x + 1u, ys, b.w - 2u, A_CELL);
            p.attr = A_ARROW;
            p.at(b.x + 1u, ys);
            p.putc(CH_SEL_L);
            p.at(b.x + b.w - 2u, ys);
            p.putc(CH_SEL_R);
            foot = kFootDlg;
            break;
        }
        case Dlg::Number: {
            b.place(22, DLG_BODY + 2u);
            b.draw(p, it.label);
            p.attr = A_DLG;
            p.at(b.x + 8u, b.y + DLG_BODY);
            p.putc(CH_LEFT);
            char num[4];
            char* q = num;
            if (value_ >= 100) *q++ = static_cast<char>('0' + value_ / 100);
            if (value_ >= 10) *q++ = static_cast<char>('0' + value_ / 10 % 10);
            *q++ = static_cast<char>('0' + value_ % 10);
            *q   = 0;
            p.spaces(4u - len_of(num));
            p.puts_max(num, 3);
            p.putc(' ');
            p.putc(CH_RIGHT);
            foot = kFootNum;
            break;
        }
        case Dlg::Key: {
            b.place(26, DLG_BODY + 2u);
            b.draw(p, "PRESS A KEY FOR");
            p.attr = A_DLG;
            p.at(b.x + 8u, b.y + DLG_BODY);
            p.puts_max(it.label, 10);
            p.putc(' ');
            p.putc(static_cast<uint8_t>('1' + col_));
            foot = kFootKey;
            break;
        }
        case Dlg::Text: {
            // Строка видна целиком, и секрет тоже; длинная - хвостом.
            b.place(DLG_W_MAX, DLG_BODY + 2u);
            b.draw(p, it.label);
            p.attr = A_DLG;
            p.at(b.x + 1u, b.y + DLG_BODY);
            uint32_t from = 0;
            if (text_len_ >= TEXT_VIEW) {
                from = text_len_ - TEXT_VIEW + 1u;
                p.putc('<');
            } else {
                p.putc(' ');
            }
            uint32_t d = TEXT_VIEW + 1u;
            for (uint32_t i = from; i < text_len_; ++i, --d) {
                p.putc(static_cast<uint8_t>(text_[i]));
            }
            p.putc('_');
            p.spaces(d - 1u);
            foot = kFootText;
            break;
        }
    }
    footer(p, foot);
}

} // namespace soundsinth::config
