// SPDX-License-Identifier: MIT
// Терминал модуля Wi-Fi в конфигураторе. В этом режиме задача модуля к
// сети не подключается и занята только им. Состояние экрана лежит в
// кольце тела ответа в PSRAM: без сети оно свободно.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>

#include "FreeRTOS.h"
#include "task.h"

#include "platform/boot_mode.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "player/config/config_tools.h"
#include "player/config/tool_wifi.h"

#include "wifi/esp_link_internal.h"

namespace rp2350::wifi {
namespace {

using namespace soundsinth::config;

constexpr uint32_t kTickMs    = 5;
constexpr uint32_t kRedrawMs  = 50;
constexpr uint32_t kPollBytes = 64;

AtTerminal* s_term = nullptr;
bool s_open        = false;
uint32_t s_drawn   = 0;

void term_draw(ToolScreen& scr) {
    // Открытие идёт секунды (подбор скорости модуля): за это время на Core1
    // могли закрыть терминал и открыть другой инструмент - его экран не
    // трогать.
    if (!tool_on_wifi_core(scr.tool())) return;
    s_term->render(scr);
    scr.publish();
    s_term->clean();
    s_drawn = platform::mono_ms();
}

// Загрузчик ПЗУ (GPIO0 у ESP8266 или GPIO9 у ESP32-C3 на земле при старте)
// на AT молчит, но отвечает на синхронизацию esptool: пакет SLIP, ответ
// начинается с C0 01 08. Скорость он подбирает по пакету; если подбор не
// вышел, слушает на скорости своего стартового сообщения - 74880 у ESP8266
// с кварцем 26 МГц. Ждёт команд до сброса модуля. 0 - не ответил.
bool rom_sync();

uint32_t rom_loader_baud() {
    for (const uint32_t baud : {115200u, 74880u}) {
        link_uart_baud(baud);
        if (rom_sync()) return baud;
    }
    link_uart_baud(115200u);
    return 0;
}

bool rom_sync() {
    static constexpr uint8_t kSync[] = {0xc0, 0x00, 0x08, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x07, 0x12, 0x20, 0x55, 0x55, 0x55,
                                        0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
                                        0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xc0};
    for (int attempt = 0; attempt < 3; ++attempt) {
        link_uart_drop();
        link_uart_put(kSync, sizeof(kSync));
        const uint32_t t0 = platform::mono_ms();
        uint32_t tail     = 0; // три последних байта
        while (platform::mono_ms() - t0 < 100u) {
            uint8_t b = 0;
            if (!link_uart_poll(b)) {
                vTaskDelay(1);
                continue;
            }
            tail = (tail << 8 | b) & 0xffffffu;
            if (tail == 0xc00108u) return true;
        }
    }
    return false;
}

void term_open(ToolScreen& scr) {
    AtTerminal& t = *s_term;
    t.reset();
    t.note("LOOKING FOR THE MODULE...", kToolCyan);
    term_draw(scr);
    // Загрузчик - первым: до него модулю ничего не шлётся, и мусор AT на
    // чужих скоростях не собьёт ему подбор скорости.
    const uint32_t rom  = rom_loader_baud();
    const uint32_t baud = rom != 0u ? 0u : link_at_probe();
    char line[kToolTextCols + 1];
    if (rom != 0u) {
        std::snprintf(line, sizeof(line), "ROM LOADER AT %u", static_cast<unsigned>(rom));
        t.status(line, kToolCyan);
    } else if (baud != 0) {
        std::snprintf(line, sizeof(line), "MODULE ANSWERS AT %u", static_cast<unsigned>(baud));
        t.status(line, kToolGreen);
        // Без эха: отправленное и так видно строкой выше ответа.
        static constexpr uint8_t kEchoOff[] = {'A', 'T', 'E', '0', '\r', '\n'};
        link_uart_put(kEchoOff, sizeof(kEchoOff));
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        t.status("MODULE DOES NOT ANSWER", kToolAlert);
    }
    link_uart_drop();
    debug_logf("wifi tools: terminal, module at %" PRIu32 ", ROM loader at %" PRIu32 "\n", baud, rom);
    term_draw(scr);
}

void term_key(uint8_t k) {
    if (!s_term->key(k)) return;
    const char* cmd = s_term->command();
    link_uart_put(reinterpret_cast<const uint8_t*>(cmd), static_cast<uint32_t>(std::strlen(cmd)));
    static constexpr uint8_t kEol[] = {'\r', '\n'};
    link_uart_put(kEol, sizeof(kEol));
}

// Кольцо UART - до дна за виток: на 921600 модуль даёт 92 байта в мс, а
// виток раз в kTickMs.
void term_poll(ToolScreen& scr) {
    uint8_t buf[kPollBytes];
    uint32_t n;
    do {
        n = 0;
        while (n < kPollBytes && link_uart_poll(buf[n])) {
            ++n;
        }
        if (n != 0) s_term->feed(buf, n);
    } while (n == kPollBytes);
    if (s_term->dirty() && platform::mono_ms() - s_drawn >= kRedrawMs) term_draw(scr);
}

void park() {
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

} // namespace

void esp_tools_task(void*) {
    uint32_t bytes      = 0;
    uint8_t* const ring = link_ring(bytes);
    uint8_t* const page = platform::config_rom_page();
    if (ring == nullptr || page == nullptr || bytes < sizeof(AtTerminal)) {
        debug_logf("wifi tools: no room (ring %" PRIu32 " B), terminal off\n", bytes);
        park();
    }
    s_term = new (ring) AtTerminal();
    ToolScreen scr(page);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
        const ToolId tool = scr.tool();
        if (!tool_on_wifi_core(tool)) {
            s_open = false;
            continue;
        }
        uint8_t k = 0;
        while (scr.key(k, tool)) {
            if (k == kToolKeyOpen) {
                s_open = true;
                scr.state(ToolState::Ready);
                term_open(scr);
            } else if (k == kToolKeyClose) {
                s_open = false;
            } else if (s_open) {
                term_key(k);
            }
        }
        if (s_open) term_poll(scr);
    }
}

} // namespace rp2350::wifi
