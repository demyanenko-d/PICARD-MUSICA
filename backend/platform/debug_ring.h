// SPDX-License-Identifier: MIT
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

// Степень двойки: индексы монотонные, маска вместо деления. Отчёт раз в 10 с
// (sd, usb) - около 4 КБ разом, в 2 КБ теряется до 110 строк.
// Данные кольца - снаружи (на плате блок PSRAM), счётчики - в самом кольце:
// им нужны атомарные операции, а на PSRAM их нет.
inline constexpr uint32_t kRingBytes = 8192;
static_assert((kRingBytes & (kRingBytes - 1)) == 0, "the ring size is a power of two");

inline constexpr uint32_t kHeaderBytes = 2;
// Длиннее не влезет и в пустое кольцо в kRingBytes.
inline constexpr uint32_t kMaxLineBytes = kRingBytes - kHeaderBytes;

struct Ring {
    uint8_t* data = nullptr;
    uint32_t mask = 0; // размер минус один, размер - степень двойки
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
    std::atomic<uint32_t> dropped{0};
};

// Буфер кольца; bytes - степень двойки. Только пока кольцо пусто и в него
// не пишут.
inline void attach(Ring& r, uint8_t* buf, uint32_t bytes) {
    r.data = buf;
    r.mask = bytes - 1u;
}

// Самая длинная строка, которая влезет в пустое кольцо.
inline uint32_t max_line(const Ring& r) {
    return r.mask + 1u - kHeaderBytes;
}

// Положить строку. Нет места - false, строка не записана, dropped растёт.
// Пустая строка не пишется.
inline bool push(Ring& r, const char* msg, uint32_t len) {
    if (len == 0) return true;
    const uint32_t head = r.head.load(std::memory_order_relaxed);
    const uint32_t tail = r.tail.load(std::memory_order_acquire);
    if (head - tail + len + kHeaderBytes > r.mask + 1u) {
        r.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint32_t pos           = head;
    r.data[pos++ & r.mask] = static_cast<uint8_t>(len);
    r.data[pos++ & r.mask] = static_cast<uint8_t>(len >> 8);
    for (uint32_t i = 0; i < len; ++i) {
        r.data[(pos + i) & r.mask] = static_cast<uint8_t>(msg[i]);
    }
    // release: увидев новый head, потребитель обязан увидеть и байты.
    r.head.store(head + len + kHeaderBytes, std::memory_order_release);
    return true;
}

// Отдать одну строку - начатую (open) или следующую - до её конца,
// пока put принимает и не больше max_bytes. put(байт) - false, если принять
// некуда, байт тогда не взят и full = true. end - абсолютный конец текущей
// строки, живёт между вызовами. 0 байт без full - строки нет.
//
// Конец, а не остаток: пара (остаток, tail) рвалась бы посреди прохода, и
// сброс журнала на отказе (проход вытеснен и брошен) пошёл бы по середине
// текста как по заголовкам. Здесь tail фиксируется сразу за заголовком:
// брошенный проход повторит начало строки, рамка цела.
template <typename Put>
uint32_t drain_line(Ring& r, uint32_t max_bytes, uint32_t& end, bool& open, Put&& put, bool& full) {
    full          = false;
    uint32_t tail = r.tail.load(std::memory_order_relaxed);
    if (!open || end - tail > r.mask + 1u) { // строки нет или кольцо сброшено
        const uint32_t head = r.head.load(std::memory_order_acquire);
        if (head - tail < kHeaderBytes) return 0;
        const uint32_t lo  = r.data[tail & r.mask];
        const uint32_t hi  = r.data[(tail + 1u) & r.mask];
        tail              += kHeaderBytes;
        end                = tail + (lo | (hi << 8));
        open               = true;
        r.tail.store(tail, std::memory_order_release);
    }
    const uint32_t tail0 = tail;
    uint32_t sent        = 0;
    while (tail != end && sent < max_bytes) {
        if (!put(r.data[tail & r.mask])) {
            full = true;
            break;
        }
        ++tail;
        ++sent;
    }
    if (tail != tail0) r.tail.store(tail, std::memory_order_release);
    if (tail == end) open = false;
    return sent;
}

// Два кольца (по одному на ядро) и один потребитель: номер кольца, которое
// сейчас выдаётся, и концы начатых строк.
struct DrainState {
    uint32_t active = 0;
    uint32_t end[2] = {0, 0};
    bool open[2]    = {false, false};
};

// Строки двух колец по очереди, по одной: начатая доводится до конца,
// затем очередь другого кольца. Иначе ядро, которое печатает больше,
// задерживало бы строки другого на весь свой поток. Не больше max_bytes.
template <typename Put>
uint32_t drain_pair(Ring (&rings)[2], DrainState& st, uint32_t max_bytes, Put&& put) {
    uint32_t sent = 0;
    uint32_t idle = 0; // колец подряд без строки
    while (sent < max_bytes && idle < 2) {
        uint32_t& end     = st.end[st.active];
        bool full         = false;
        const uint32_t n  = drain_line(rings[st.active], max_bytes - sent, end, st.open[st.active], put, full);
        sent             += n;
        if (full || st.open[st.active]) break; // приёмник полон или предел - строка продолжится
        idle       = (n == 0) ? idle + 1 : 0;
        st.active ^= 1u;
    }
    return sent;
}

// Осталось ли что выдать: строка в кольце или недоданный хвост.
inline bool pending(Ring (&rings)[2], const DrainState& st) {
    for (uint32_t i = 0; i < 2; ++i) {
        if (st.open[i] || rings[i].head.load(std::memory_order_acquire) != rings[i].tail.load(std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

} // namespace debug_ring
