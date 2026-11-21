// SPDX-License-Identifier: MIT
#pragma once

// Отвод записей в порты живых устройств: обработчик записи шины (Core1)
// кладёт, читают на другом ядре. Кольцо одно на все устройства - живой
// MIDI через порт A AY, живой AY и те, что добавятся. У каждого читателя
// свой хвост; писатель никого не ждёт, читатель, отставший на круг, сам
// видит потерю и считает её.
//
// Запись - слово: штамп шины (младшие 20 разрядов полумикросекунд, оборот
// 0.5 с), порт, байт.

#include <atomic>
#include <cstdint>

#include "platform/compiler.h"

namespace player::live {

enum class TapPort : uint8_t {
    AySelect = 0, // #FFFD - выбор регистра AY
    AyData   = 1, // #BFFD - данные AY
    SaaAddr  = 2, // #1FF - номер регистра SAA1099
    SaaData  = 3, // #FF - данные SAA1099
};

// Ёмкость кольца. Задаёт живой MIDI: до ~3100 байт/с, по 10 записей на
// байт - 2048 записей переживают 66 мс плотного потока без читателя; по
// журналам платы за один разбор копится до 340.
inline constexpr uint32_t kTapRing            = 2048; // степень двойки
inline constexpr uint32_t kTapStampBits       = 20;
inline constexpr uint32_t kTapStampMask       = (1u << kTapStampBits) - 1u;
inline constexpr uint32_t kTapMidiBytesPerSec = 3100;
inline constexpr uint32_t kTapWritesPerByte   = 10;
inline constexpr uint32_t kTapSpanMs          = kTapRing * 1000u / (kTapMidiBytesPerSec * kTapWritesPerByte);

inline uint32_t tap_stamp(uint32_t e) {
    return e >> 12;
}
inline TapPort tap_port(uint32_t e) {
    return static_cast<TapPort>((e >> 8) & 0x0fu);
}
inline uint8_t tap_data(uint32_t e) {
    return static_cast<uint8_t>(e);
}

struct TapReader {
    uint32_t tail = 0;
    uint32_t lost = 0; // записей, ушедших из кольца непрочитанными
};

// Кольцо отвода на N записей (степень двойки): общее - kTapRing, у
// цифрового звука своё (dac_live.h). Запись - как у общего: штамп, порт,
// байт.
template <uint32_t N>
struct TapRing {
    static_assert((N & (N - 1u)) == 0u, "the ring size is a power of two");
    uint32_t buf[N];
    std::atomic<uint32_t> head{0}; // пишет Core1

    // Из обработчика записи шины: встраивается в него, во флеш не ходит.
    SOUNDSINTH_ALWAYS_INLINE void push(uint32_t stamp, uint8_t port, uint8_t data) {
        const uint32_t h  = head.load(std::memory_order_relaxed);
        buf[h & (N - 1u)] = ((stamp & kTapStampMask) << 12) | (static_cast<uint32_t>(port) << 8) | data;
        head.store(h + 1u, std::memory_order_release);
    }
    // Отставший на круг читатель перескакивает на самое старое, что ещё
    // лежит в кольце: остальное потеряно.
    bool peek(TapReader& r, uint32_t& e) const {
        const uint32_t h = head.load(std::memory_order_acquire);
        if (r.tail == h) return false;
        if (h - r.tail > N) {
            r.lost += h - r.tail - N;
            r.tail  = h - N;
        }
        e = buf[r.tail & (N - 1u)];
        return true;
    }
    uint32_t head_now() const { return head.load(std::memory_order_acquire); }
};

// Из обработчика записи шины.
void tap_push(uint32_t stamp, TapPort port, uint8_t data);

// Следующая запись читателя, не забирая её; false - новых нет.
bool tap_peek(TapReader& r, uint32_t& e);
inline void tap_next(TapReader& r) {
    ++r.tail;
}
inline bool tap_pop(TapReader& r, uint32_t& e) {
    if (!tap_peek(r, e)) return false;
    tap_next(r);
    return true;
}

// Голова кольца: читатель с этого места видит только новые записи.
uint32_t tap_head();

// Сколько записей отвод увидел с загрузки.
uint32_t tap_seen();

// Сброс машины у читателя отвода: голову кольца на момент сброса и номер
// запроса пишет Core1 (request), номер исполненного - рендер (take).
struct TapReset {
    uint32_t head = 0;
    std::atomic<uint32_t> req{0};
    uint32_t done  = 0;
    uint32_t count = 0; // сбросов исполнено

    void request() {
        head = tap_head();
        req.store(req.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
    }
    // Запрошен ли сброс; если да - записи до него выброшены из r. Сколько бы
    // запросов ни пришло до вызова, сброс один.
    bool take(TapReader& r) {
        const uint32_t q = req.load(std::memory_order_acquire);
        if (q == done) return false;
        done = q;
        if (static_cast<int32_t>(head - r.tail) > 0) r.tail = head;
        ++count;
        return true;
    }
};

} // namespace player::live
