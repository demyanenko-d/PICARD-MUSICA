#pragma once

// Кольца строк лога без зависимостей от pico-sdk: их же собирают тесты ПК.
//
// Кадр - два байта длины (little-endian), затем строка. Строка кладётся
// целиком или не кладётся: по полстроки ставят неверные диагнозы. head
// пишет только производитель, tail - только потребитель; оба монотонно
// растут и заворачиваются согласованно (uint32_t).

#include <atomic>
#include <cstdint>

namespace debug_ring {

// Степень двойки: индексы монотонные, маска вместо деления. Штатный вывод
// (heap/plug/proto/diag) - десятки байт в секунду, 2 КБ хватает; для
// подробной трассировки поднимать до 4096 (трассировка NMI в 2 КБ теряет
// тысячи строк).
inline constexpr uint32_t kRingBytes = 2048;
inline constexpr uint32_t kRingMask = kRingBytes - 1;
static_assert((kRingBytes & kRingMask) == 0, "размер кольца - степень двойки");

inline constexpr uint32_t kHeaderBytes = 2;
// Длиннее не влезет и в пустое кольцо.
inline constexpr uint32_t kMaxLineBytes = kRingBytes - kHeaderBytes;

struct Ring {
    uint8_t data[kRingBytes];
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
    std::atomic<uint32_t> dropped;
};

// Положить строку. Нет места - false, строка не записана, dropped растёт.
// Пустая строка не пишется.
inline bool push(Ring& r, const char* msg, uint32_t len) {
    if (len == 0) return true;
    const uint32_t head = r.head.load(std::memory_order_relaxed);
    const uint32_t tail = r.tail.load(std::memory_order_acquire);
    if (head - tail + len + kHeaderBytes > kRingBytes) {
        r.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t pos = head;
    r.data[pos++ & kRingMask] = static_cast<uint8_t>(len);
    r.data[pos++ & kRingMask] = static_cast<uint8_t>(len >> 8);
    for (uint32_t i = 0; i < len; ++i) {
        r.data[(pos + i) & kRingMask] = static_cast<uint8_t>(msg[i]);
    }
    // release: увидев новый head, потребитель обязан увидеть и байты.
    r.head.store(head + len + kHeaderBytes, std::memory_order_release);
    return true;
}

// Отдать одну строку - начатую (left != 0) или следующую - до её конца,
// пока put принимает и не больше max_bytes. put(байт) - false, если принять
// некуда, байт тогда не взят и full = true. left - сколько байт строки ещё
// не отдано, живёт между вызовами. 0 байт без full - строки нет.
template <typename Put>
uint32_t drain_line(Ring& r, uint32_t max_bytes, uint32_t& left, Put&& put, bool& full) {
    full = false;
    const uint32_t tail0 = r.tail.load(std::memory_order_relaxed);
    uint32_t tail = tail0;
    if (left == 0) {
        const uint32_t head = r.head.load(std::memory_order_acquire);
        if (head - tail < kHeaderBytes) return 0;
        const uint32_t lo = r.data[tail & kRingMask];
        const uint32_t hi = r.data[(tail + 1u) & kRingMask];
        left = lo | (hi << 8);
        tail += kHeaderBytes;
    }
    uint32_t sent = 0;
    while (left != 0 && sent < max_bytes) {
        if (!put(r.data[tail & kRingMask])) {
            full = true;
            break;
        }
        ++tail;
        ++sent;
        --left;
    }
    if (tail != tail0) r.tail.store(tail, std::memory_order_release);
    return sent;
}

// Два кольца (по одному на ядро) и один потребитель: номер кольца, которое
// сейчас выдаётся, и остатки недоданных строк.
struct DrainState {
    uint32_t active = 0;
    uint32_t left[2] = {0, 0};
};

// Строки двух колец по очереди, по одной: начатая доводится до конца,
// затем очередь другого кольца. Иначе ядро, которое печатает больше,
// задерживало бы строки другого на весь свой поток. Не больше max_bytes.
template <typename Put>
uint32_t drain_pair(Ring (&rings)[2], DrainState& st, uint32_t max_bytes, Put&& put) {
    uint32_t sent = 0;
    uint32_t idle = 0; // колец подряд без строки
    while (sent < max_bytes && idle < 2) {
        uint32_t& left = st.left[st.active];
        bool full = false;
        const uint32_t n = drain_line(rings[st.active], max_bytes - sent, left, put, full);
        sent += n;
        if (full || left != 0) break; // приёмник полон или предел - строка продолжится
        idle = (n == 0) ? idle + 1 : 0;
        st.active ^= 1u;
    }
    return sent;
}

// Осталось ли что выдать: строка в кольце или недоданный хвост.
inline bool pending(Ring (&rings)[2], const DrainState& st) {
    for (uint32_t i = 0; i < 2; ++i) {
        if (st.left[i] != 0 ||
            rings[i].head.load(std::memory_order_acquire) != rings[i].tail.load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

} // namespace debug_ring
