// SPDX-License-Identifier: MIT
#include "player/config/config_term_service.h"

#include <new>

#include "devices/hid/usb_map.h"

#include "player/config/config_service.h"

namespace player::config {

using namespace devices;

namespace {

using namespace soundsinth::config;

struct TermMem {
    ConfigUi ui;
    TermGrid grid;
    uint32_t drawn;     // stamp() выложенного кадра
    uint8_t rom_starts; // счётчик стартов ПЗУ, при котором выложен кадр
};

// Блок до config_term_begin - сырой, после - меню.
TermMem* s_mem = nullptr;
bool s_on      = false;
const ConfigPageFiles kNoFiles{};

const ConfigUiHost kHost{
    &devices::hid::usb_map_capture_arm,
    &devices::hid::usb_map_capture_cancel,
    &devices::hid::usb_map_capture_take,
};

} // namespace

uint32_t config_term_mem_bytes() {
    return sizeof(TermMem);
}

void config_term_reserve(void* mem) {
    s_mem = static_cast<TermMem*>(mem);
}

bool config_term_begin(uint8_t* page, const ConfigPageFiles* files) {
    if (s_mem == nullptr) return false;
    term_page_init(page);
    tool_area_reset(page);
    s_mem = new (s_mem) TermMem();
    s_on  = true;
    s_mem->ui.start(settings(), files != nullptr ? *files : kNoFiles, kHost, reinterpret_cast<ToolArea*>(page + kToolOffset));
    s_mem->drawn      = s_mem->ui.stamp() - 1u;
    s_mem->rom_starts = page[kTermStartsAt];
    return true;
}

bool config_term_on() {
    return s_on;
}

ConfigUiExit config_term_serve(uint8_t* page) {
    if (!s_on) return ConfigUiExit::None;
    ConfigUi& ui = s_mem->ui;
    uint8_t k    = 0;
    while (term_key(page, k)) {
        ui.key(k);
    }
    ui.poll();
    // Машину сбросили: ПЗУ стартовало заново с пустым экраном - кадр целиком.
    const uint8_t starts = *static_cast<volatile uint8_t*>(page + kTermStartsAt);
    if (starts != s_mem->rom_starts) {
        s_mem->rom_starts = starts;
        s_mem->drawn      = ui.stamp() - 1u;
        term_resend(page);
    }
    // Кадр рисуется, когда экран сменился; не выложен (ПЗУ ещё выводит
    // прошлый) - повтор на следующем обороте.
    const uint32_t stamp = ui.stamp();
    if (stamp != s_mem->drawn) {
        ui.render(s_mem->grid);
        if (term_publish(page, s_mem->grid)) s_mem->drawn = stamp;
    }
    return ui.exit();
}

const Settings& config_term_draft() {
    return s_mem->ui.draft();
}

} // namespace player::config
