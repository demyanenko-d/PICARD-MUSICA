// SPDX-License-Identifier: MIT
#include "platform/log_rings.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "platform/compiler.h"
#include "platform/debug_ring.h"
#include "platform/log.h"

namespace {

// Нулевые до main (статическое хранилище обнуляет старт на любой
// платформе): отдельной инициализации колец нет.
debug_ring::Ring s_ring[2];

// Номер выдаваемого кольца и недоданные хвосты строк: строка длиннее
// приёмника выдаётся за несколько проходов, до её конца кольцо не меняется.
debug_ring::DrainState s_drain;

// Вызовы debug_log из обработчиков прерываний - строки не записаны.
std::atomic<uint32_t> s_isr_calls{0};

// Строки, не влезшие в буфер debug_logf: буфер короток, а не лог занят.
std::atomic<uint32_t> s_truncated{0};

// true - печать напрямую: потребителя ещё (или уже) нет. Пишется на старте
// и в обработчике отказа, читается обоими ядрами.
std::atomic<bool> s_direct{true};

// Взаимоисключение только для прямого режима: в нём оба ядра пишут в
// консоль сами, без него строки перемешались бы посимвольно. В режиме
// колец блокировки нет, писатель в железо один.
std::atomic_flag s_direct_busy = ATOMIC_FLAG_INIT;

uint32_t drain_rings(uint32_t max_bytes) {
    return debug_ring::drain_pair(s_ring, s_drain, max_bytes, platform::log_put);
}

// Ожидание флага ограничено: отказ ядра посреди выгрузки колец оставил бы
// флаг взятым навсегда, и строки отказа не было бы вовсе. Не дождались -
// печать без флага.
constexpr uint32_t kDirectWaitSpins = 100000;

} // namespace

void platform::debug_log(const char* msg) {
    if (s_direct.load(std::memory_order_acquire)) {
        // Потребителя нет - иначе строка пропала бы.
        bool owned = false;
        for (uint32_t i = 0; i < kDirectWaitSpins; ++i) {
            if (!s_direct_busy.test_and_set(std::memory_order_acquire)) {
                owned = true;
                break;
            }
            std::atomic_signal_fence(std::memory_order_seq_cst); // ждём другое ядро, прерывания разрешены
        }
        platform::log_put_blocking(msg);
        if (owned) s_direct_busy.clear(std::memory_order_release);
        return;
    }
    // Обработчик прерывания мог прервать запись своего ядра посреди строки,
    // а запрет переключения из него звать нельзя: строка не пишется, вызов
    // считается.
    if (platform::in_isr()) {
        s_isr_calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t len = static_cast<uint32_t>(std::strlen(msg));
    if (len == 0) return;
    const uint32_t core = platform::core_id();
    // Строка длиннее кольца не влезет никогда: потеря сразу, без запрета
    // переключения ради заведомого отказа push.
    if (len > debug_ring::kMaxLineBytes) {
        s_ring[core].dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    platform::log_writer_lock();
    debug_ring::push(s_ring[core], msg, len);
    platform::log_writer_unlock();
}

// Без встраивания: буфер 192 байта лежит здесь, а не в кадре каждого места
// печати.
SOUNDSINTH_NOINLINE void platform::debug_logf(const char* fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || static_cast<unsigned>(n) >= sizeof(buf)) {
        s_truncated.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    platform::debug_log(buf);
}

uint32_t platform::log_drain(uint32_t max_bytes) {
    // Флаг прямой печати - на проход: строка отказа другого ядра не
    // перемешается с выгрузкой, а выгрузка после отказа продолжается (в
    // кольцах след перед ним). Флаг занят - проход пропускается.
    if (s_direct_busy.test_and_set(std::memory_order_acquire)) return 0;
    const uint32_t sent = drain_rings(max_bytes);
    s_direct_busy.clear(std::memory_order_release);
    return sent;
}

bool platform::log_pending() {
    return debug_ring::pending(s_ring, s_drain);
}

bool platform::log_service(uint32_t max_bytes) {
    log_drain(max_bytes);

    // По неполному журналу без пометки ставят неверный диагноз.
    static uint32_t s_last_dropped   = 0;
    static uint32_t s_last_isr_calls = 0;
    static uint32_t s_last_truncated = 0;

    const uint32_t dropped = log_dropped();
    if (dropped != s_last_dropped) {
        debug_logf("log: LOST %u lines (ring overflowed)\n", static_cast<unsigned>(dropped));
        s_last_dropped = dropped;
    }
    const uint32_t isr_calls = log_isr_calls();
    if (isr_calls != s_last_isr_calls) {
        debug_logf("log: %u lines not written from ISR\n", static_cast<unsigned>(isr_calls));
        s_last_isr_calls = isr_calls;
    }
    // Обрезанные строки - отдельно от потерь кольца: там журнал не успевал,
    // здесь буфер debug_logf короток для этой строки.
    const uint32_t truncated = log_truncated();
    if (truncated != s_last_truncated) {
        debug_logf("log: TRUNC %u lines did not fit the buffer\n", static_cast<unsigned>(truncated));
        s_last_truncated = truncated;
    }
    return log_pending();
}

void platform::log_flush_blocking() {
    // Предел - два кольца: другое ядро может дописывать и дальше.
    uint32_t budget = 2u * (debug_ring::kRingBytes + debug_ring::kHeaderBytes);
    while (budget > 0) {
        platform::log_wait_ready();
        const uint32_t sent = drain_rings(budget);
        if (sent == 0) return; // приёмник свободен, а выдать нечего - кольца пусты
        budget -= sent;
    }
}

uint32_t platform::log_dropped() {
    return s_ring[0].dropped.load(std::memory_order_relaxed) + s_ring[1].dropped.load(std::memory_order_relaxed);
}

uint32_t platform::log_isr_calls() {
    return s_isr_calls.load(std::memory_order_relaxed);
}

uint32_t platform::log_truncated() {
    return s_truncated.load(std::memory_order_relaxed);
}

void platform::log_go_direct() {
    s_direct.store(true, std::memory_order_release);
}

void platform::log_use_rings() {
    s_direct.store(false, std::memory_order_release);
}
