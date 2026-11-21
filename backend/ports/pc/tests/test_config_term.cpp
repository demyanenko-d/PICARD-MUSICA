// SPDX-License-Identifier: MIT
// Конфигуратор-терминал: ПЗУ, выводящее только отрезки кадра, получает
// ровно сетку платы; меню платы доходит до правки и выхода с сохранением.
#include "testing.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "player/config/config_page.h"
#include "player/config/config_service.h"
#include "player/config/config_term.h"
#include "player/config/config_term_service.h"
#include "player/config/config_ui.h"

#include "core/util/fnv1a.h"

using namespace soundsinth::config;

namespace {

uint8_t g_page[kConfigPageBytes];
TermGrid g_screen; // экран машины: то, что вывело ПЗУ

// ПЗУ: вывести выложенный кадр и отметить показанным.
void rom_show() {
    auto& h = *reinterpret_cast<TermHeader*>(g_page);
    if (h.shown == h.upd) return;
    const uint8_t* ch = g_page + kTermCharsAt;
    const uint8_t* at = g_page + kTermAttrsAt;
    const auto* sp    = reinterpret_cast<const TermSpan*>(g_page + sizeof(TermHeader));
    auto put          = [&](uint32_t r, uint32_t c, uint32_t len, bool chars) {
        for (uint32_t k = c; k < c + len; ++k) {
            g_screen.at[r][k] = at[r * kTermCols + k];
            if (chars) g_screen.ch[r][k] = ch[r * kTermCols + k];
        }
    };
    if (h.count == kTermAll) {
        for (uint32_t r = 1; r < kTermRows; ++r)
            put(r, 0, kTermCols, true);
    } else {
        for (uint32_t i = 0; i < h.count; ++i)
            put(sp[i].row, sp[i].col, sp[i].len, sp[i].kind != kTermSpanAttr);
    }
    h.shown = h.upd;
}

bool screen_is(const TermGrid& g) {
    for (uint32_t r = 1; r < kTermRows; ++r) {
        if (std::memcmp(g_screen.ch[r], g.ch[r], kTermCols) != 0 || std::memcmp(g_screen.at[r], g.at[r], kTermCols) != 0) return false;
    }
    return true;
}

void spans_rebuild_the_grid() {
    term_page_init(g_page);
    std::memset(&g_screen, 0, sizeof(g_screen));
    TermGrid g{};
    uint32_t seed = 12345, frames = 0, whole = 0;
    for (int round = 0; round < 400; ++round) {
        // Правки разного размера: точки, строки, полосы цвета.
        const uint32_t edits = round % 7 == 0 ? 300u : round % 3u + 1u;
        for (uint32_t e = 0; e < edits; ++e) {
            seed             = seed * 1103515245u + 12345u;
            const uint32_t r = (seed >> 8) % kTermRows, c = (seed >> 16) % kTermCols;
            if ((seed & 1u) != 0) g.ch[r][c] = static_cast<uint8_t>(seed >> 24);
            g.at[r][c] = static_cast<uint8_t>(seed >> 20);
        }
        CHECK(term_publish(g_page, g));
        if (reinterpret_cast<TermHeader*>(g_page)->count == kTermAll) ++whole;
        rom_show();
        ++frames;
        CHECK(screen_is(g));
        // ПЗУ ещё выводит: кадр ждёт.
        if (round == 10) {
            g.ch[5][5] ^= 1u;
            CHECK(term_publish(g_page, g));
            CHECK(!term_publish(g_page, g));
            rom_show();
            CHECK(screen_is(g));
        }
    }
    std::printf("  frames %u, whole screen %u\n", frames, whole);
}

void menu_edits_and_saves() {
    term_page_init(g_page);
    std::memset(&g_screen, 0, sizeof(g_screen));
    static ConfigUi ui;
    static const ConfigPageFiles files{};
    Settings s{};
    ui.start(s, files, ConfigUiHost{}, nullptr);
    TermGrid g{};
    auto step = [&](uint8_t k) {
        ui.key(k);
        ui.render(g);
        CHECK(term_publish(g_page, g));
        rom_show();
        CHECK(screen_is(g));
    };
    ui.render(g);
    CHECK(term_publish(g_page, g));
    rom_show();
    // Подменю AUDIO (девятый пункт), флаг GS - первым после BACK.
    for (int i = 0; i < 8; ++i)
        step(10);
    step(13);
    const uint8_t gs = s.gs;
    step(13);
    CHECK(ui.draft().gs != gs);
    CHECK(ui.exit() == ConfigUiExit::None);
    // Назад и сохранение по S.
    step(27);
    step('s');
    CHECK(ui.exit() == ConfigUiExit::Save);
}

// ПЗУ: нажатие в кольцо страницы.
void rom_key(uint8_t k) {
    auto& h                       = *reinterpret_cast<TermHeader*>(g_page);
    h.keys[h.req % kTermKeySlots] = k;
    h.req                         = static_cast<uint8_t>(h.req + 1u);
}

// Служба платы целиком: reserve, begin, serve по оборотам цикла, а ПЗУ
// показывает кадр раз в K оборотов. После затишья экран машины - тот же,
// что у меню, которое получило те же нажатия напрямую.
void service_serves_frames() {
    using player::config::config_term_begin;
    using player::config::config_term_on;
    using player::config::config_term_reserve;
    using player::config::config_term_serve;
    // Нет памяти - нет экрана.
    config_term_reserve(nullptr);
    CHECK(!config_term_begin(g_page, nullptr));
    CHECK(!config_term_on());
    CHECK(config_term_serve(g_page) == ConfigUiExit::None);

    static std::vector<uint64_t> mem(player::config::config_term_mem_bytes() / sizeof(uint64_t) + 1u);
    static const uint8_t kKeys[] = {10, 10, 10, 10, 10, 10, 10, 10, 13, 13, 27};
    for (const uint32_t every : {1u, 3u, 10u}) {
        std::memset(&g_screen, 0, sizeof(g_screen));
        config_term_reserve(mem.data());
        CHECK(config_term_begin(g_page, nullptr));
        CHECK(config_term_on());
        static ConfigUi ref;
        static const ConfigPageFiles files{};
        ref.start(player::config::settings(), files, ConfigUiHost{}, nullptr);
        TermGrid g{};
        uint32_t turn = 0;
        auto serve    = [&](uint32_t n) {
            for (uint32_t i = 0; i < n; ++i, ++turn) {
                CHECK(config_term_serve(g_page) == ConfigUiExit::None);
                if (turn % every == 0) rom_show();
            }
        };
        // Первый кадр - до любого нажатия.
        serve(3u * every + 3u);
        ref.render(g);
        CHECK(screen_is(g));
        // Нажатия чаще показа: кадр ждёт, пока ПЗУ выводит прошлый.
        for (const uint8_t k : kKeys) {
            rom_key(k);
            ref.key(k);
            serve(2);
        }
        serve(3u * every + 3u);
        ref.render(g);
        std::printf("  ROM shows every %u turns: screen matches %d\n", every, screen_is(g));
        CHECK(screen_is(g));
        rom_key('s');
        CHECK(config_term_serve(g_page) == ConfigUiExit::Save);
    }
}

// Экраны меню по сценариям render_all.sh (стенд cfgrun): нажатия от
// главного экрана, хеш FNV-1a сетки после них. Настройки и файлы - как у
// стенда. Экраны инструментов рисует tools_view, их здесь нет. Хеш сменился
// - тест называет сценарий и новое число; обновлять после просмотра снимка
// (sh tmp/claude/cfgrun/render_all.sh).
// Клавиши: D вниз, U вверх, L влево, R вправо, E ввод, X ESC.
struct Screen {
    const char* name;
    const char* keys;
    uint32_t hash;
};
// clang-format off
const Screen kScreens[] = {
    {"01_main",                "", 0x68BA8367u},
    {"02_disk_system",         "E", 0x20565A69u},
    {"03_machine_rom",         "DE", 0x4143A32Bu},
    {"04_wifi",                "DDDE", 0x843C0EDAu},
    {"05_wifi_network",        "DDDEDE", 0x061C7EB4u},
    {"06_keyboard",            "DDDDE", 0x4B0A5062u},
    {"07_keyboard_mod",        "DDDDEDE", 0xC7677D61u},
    {"08_mouse",               "DDDDDE", 0x03A0E1CAu},
    {"09_mouse_speed",         "DDDDDEDE", 0xD12AA8CFu},
    {"10_joystick",            "DDDDDDE", 0xB11A350Au},
    {"11_joystick_keys",       "DDDDEDDDDDDE", 0xAF5DF6DCu},
    {"13_disks",               "DDDDDDDE", 0x7D2F9E80u},
    {"14b_disks_sd_speed",     "DDDDDDDEDDDDE", 0xC03F5842u},
    {"15_audio",               "DDDDDDDDE", 0xF0C218D8u},
    {"16_audio_live_midi",     "DDDDDDDDEDDE", 0x8FF65A6Eu},
    {"16b_audio_ay_emulation", "DDDDDDDDEDDDE", 0x2CBBA010u},
    {"16c_audio_ay_stereo",    "DDDDDDDDEDDDDE", 0xEBEACDD5u},
    {"16d_audio_digital",      "DDDDDDDDEDDDDDDE", 0x68B0E937u},
    {"16e_audio_digital_beep", "DDDDDDDDEDDDDDDEDDE", 0xE9F9000Eu},
    {"17_audio_mod_panning",   "DDDDDDDDEDDDDDDDDDE", 0xDE33B1F3u},
    {"18_rom_slots",           "DDDDDDDDDE", 0x3EE5AFA7u},
    {"19_rom_slot_file",       "DDDDDDDDDEE", 0x94F069DBu},
    {"20_rom_slot_file_scroll","DDDDDDDDDEEDDDDD", 0xE0AF8F87u},
    {"21_tools",               "DDDDDDDDDDE", 0x844BB7D8u},
};
// clang-format on

uint8_t key_code(char c) {
    switch (c) {
        case 'D':
            return 10;
        case 'U':
            return 11;
        case 'L':
            return 8;
        case 'R':
            return 9;
        case 'X':
            return 27;
        default:
            return 13;
    }
}

void menu_screens_golden() {
    static const char* const kNames[] = {"PENTAGON.ROM", "QUORUM.ROM",   "ATM_TURBO_2.ROM", "SCORPION.ROM",  "KAY_1024.ROM", "PROFI.ROM",
                                         "BYTE.ROM",     "BASIC48.ROM",  "TRDOS_504T.ROM",  "TRDOS_506.ROM", "GLUK.ROM",     "MAGIC.ROM",
                                         "SHADOW.ROM",   "NEMO_ROM.ROM", "RAMEX.ROM",       "TURBO_128.ROM"};
    static ConfigPageFiles files{};
    for (const char* n : kNames) {
        std::strcpy(files.name[files.count], n);
        ++files.count;
    }
    Settings s{};
    s.disksys         = static_cast<uint8_t>(DiskSys::TrDos);
    s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    s.wifi            = 1;
    s.usb_mouse       = 1;
    std::strcpy(s.wifi_ssid, "HOME");
    std::strcpy(s.rom_file[0], "PENTAGON.ROM");
    static ConfigUi ui;
    static TermGrid g;
    uint32_t bad = 0;
    for (const Screen& sc : kScreens) {
        ui.start(s, files, ConfigUiHost{}, nullptr);
        for (const char* k = sc.keys; *k != 0; ++k) {
            ui.key(key_code(*k));
            ui.poll();
        }
        std::memset(&g, 0, sizeof(g));
        ui.render(g);
        const uint32_t h = soundsinth::util::fnv1a(soundsinth::util::kFnvSeed, reinterpret_cast<const uint8_t*>(&g), sizeof(g));
        if (h != sc.hash) {
            std::printf("  screen %s: hash %08X, expected %08X\n", sc.name, h, sc.hash);
            ++bad;
        }
    }
    CHECK_EQ(bad, 0u);
}

// Сброс машины кнопкой в конфигураторе: ПЗУ стартует заново, чистит экран,
// поднимает счётчик стартов и ждёт кадр. Служба обязана прислать экран
// целиком, а не разность к следующему нажатию.
void service_resends_after_rom_restart() {
    using player::config::config_term_begin;
    using player::config::config_term_reserve;
    using player::config::config_term_serve;
    static std::vector<uint64_t> mem(player::config::config_term_mem_bytes() / sizeof(uint64_t) + 1u);
    std::memset(&g_screen, 0, sizeof(g_screen));
    config_term_reserve(mem.data());
    CHECK(config_term_begin(g_page, nullptr));
    for (int i = 0; i < 4; ++i) {
        CHECK(config_term_serve(g_page) == ConfigUiExit::None);
        rom_show();
    }
    static ConfigUi ref;
    static const ConfigPageFiles files{};
    ref.start(player::config::settings(), files, ConfigUiHost{}, nullptr);
    TermGrid g{};
    ref.render(g);
    CHECK(screen_is(g));
    // Сброс: экран пуст, ПЗУ отметилось стартом.
    std::memset(&g_screen, 0, sizeof(g_screen));
    ++g_page[kTermStartsAt];
    for (int i = 0; i < 4; ++i) {
        CHECK(config_term_serve(g_page) == ConfigUiExit::None);
        rom_show();
    }
    std::printf("  after a ROM restart the screen is whole: %d\n", screen_is(g));
    CHECK(screen_is(g));
}

} // namespace

void run_config_term_tests() {
    std::printf("config: terminal page and the board menu\n");
    spans_rebuild_the_grid();
    menu_edits_and_saves();
    service_serves_frames();
    service_resends_after_rom_restart();
    menu_screens_golden();
}
