// SPDX-License-Identifier: MIT
// Подставной контракт связи с хостом (player/hal/host_link.h) и блокирующий
// вывод журнала.
//
// Без них в тестовый двоичный не линкуется сеанс трека, а с ним и мост
// General Sound (мост зовёт publish_file_info из сеанса). Тесты зовут мост
// и сеанс напрямую; счётчики показывают, что вызов состоялся.

#include "platform/log_rings.h"
#include "player/hal/host_link.h"

#include <cstdio>

namespace test_host {

uint32_t g_service      = 0;
uint32_t g_service_wait = 0;
uint32_t g_file_info    = 0;
uint32_t g_release      = 0;
uint32_t g_hold         = 0;
uint32_t g_idle_wait    = 0;
// Последнее, что сеанс сказал хосту о треке.
uint8_t g_minutes      = 0;
uint8_t g_seconds      = 0;
uint16_t g_samples     = 0;
uint16_t g_patterns    = 0;
uint16_t g_instruments = 0;
// Что отдавать на вопрос "когда хост читал состояние". Тест двигает сам:
// по давности сеанс решает, ждать ли от хоста данных.
uint32_t g_last_seen_us = 0;
// Потери на шине: тест подставляет их, чтобы проверить разбор диагностики
// принятого модуля.
uint32_t g_lost_port_writes = 0;

void reset() {
    g_service          = 0;
    g_service_wait     = 0;
    g_file_info        = 0;
    g_release          = 0;
    g_hold             = 0;
    g_idle_wait        = 0;
    g_minutes          = 0;
    g_seconds          = 0;
    g_samples          = 0;
    g_patterns         = 0;
    g_instruments      = 0;
    g_last_seen_us     = 0;
    g_lost_port_writes = 0;
}

} // namespace test_host

namespace player::hal {

void host_service() {
    ++test_host::g_service;
}

void host_service_and_wait() {
    ++test_host::g_service_wait;
}

void host_set_file_info(uint8_t minutes, uint8_t seconds, uint16_t samples, uint16_t patterns, uint16_t instruments) {
    ++test_host::g_file_info;
    test_host::g_minutes     = minutes;
    test_host::g_seconds     = seconds;
    test_host::g_samples     = samples;
    test_host::g_patterns    = patterns;
    test_host::g_instruments = instruments;
}

void host_release_reset() {
    ++test_host::g_release;
}

void host_hold_for_reboot() {
    ++test_host::g_hold;
}

uint32_t host_last_seen_us() {
    return test_host::g_last_seen_us;
}

uint32_t bus_lost_port_writes() {
    return test_host::g_lost_port_writes;
}

void idle_wait_init() {}

void idle_wait(uint32_t) {
    ++test_host::g_idle_wait;
}

} // namespace player::hal

// Журнал без буферизации: на плате им печатают отказ, когда кольцо уже не
// обслуживается. В тесте разницы с обычным выводом нет.
void platform::log_put_blocking(const char* msg) {
    std::fputs(msg, stdout);
}
