// SPDX-License-Identifier: MIT
// Печатает файл настроек тем же кодом, каким его пишет плата. Отсюда
// берутся образцы для релиза: разойтись с прошивкой они не могут.
//
//   config_example [набор] [файл]
//
// Наборы: default (он же tsconfig), trdos, divmmc. Без имени файла пишет
// на стандартный вывод.

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/config/config.h"
#include "core/config/config_ini.h"

namespace {

using soundsinth::config::Layout;
using soundsinth::config::Settings;

// TS-Config: у машины своя память, свой интерфейс карты, своя клавиатура и
// мышь. Это и есть умолчания прошивки.
Settings preset_tsconfig() {
    return Settings{};
}

// TR-DOS: своего интерфейса карты у машины нет, поэтому Z-Controller
// нужен; подстановка памяти - нет, диск у неё свой. Клавиатуры и мыши USB
// тоже своих нет.
Settings preset_trdos() {
    Settings s;
    s.zcontroller     = 1;
    s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    s.usb_mouse       = 1;
    s.usb_gamepad     = 1;
    s.reset_signal    = 1;
    return s;
}

// Машина без своего диска: плата даёт ей всё, что умеет.
Settings preset_divmmc() {
    Settings s;
    s.disksys         = static_cast<uint8_t>(soundsinth::config::DiskSys::DivMmc);
    s.zcontroller     = 1;
    s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    s.usb_mouse       = 1;
    s.usb_gamepad     = 1;
    s.reset_signal    = 1;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    const char* preset = argc > 1 ? argv[1] : "default";
    const char* path   = argc > 2 ? argv[2] : nullptr;

    Settings s;
    if (std::strcmp(preset, "default") == 0 || std::strcmp(preset, "tsconfig") == 0) {
        s = preset_tsconfig();
    } else if (std::strcmp(preset, "trdos") == 0) {
        s = preset_trdos();
    } else if (std::strcmp(preset, "divmmc") == 0) {
        s = preset_divmmc();
    } else {
        std::fprintf(stderr, "config_example: unknown set \"%s\"; available are default, trdos, divmmc\n", preset);
        return 2;
    }

    std::vector<char> text(soundsinth::config::kIniMaxBytes);
    const uint32_t len = soundsinth::config::ini_render(s, text.data(), static_cast<uint32_t>(text.size()));
    if (len == 0 || len >= text.size()) {
        std::fprintf(stderr, "config_example: did not fit, %u B needed\n", static_cast<unsigned>(len));
        return 1;
    }

    if (path == nullptr) {
        std::fwrite(text.data(), 1, len, stdout);
        return 0;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "config_example: cannot open %s\n", path);
        return 1;
    }
    std::fwrite(text.data(), 1, len, f);
    std::fclose(f);
    std::fprintf(stderr, "config_example: %s (%s), %u B\n", path, preset, static_cast<unsigned>(len));
    return 0;
}
