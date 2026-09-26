#pragma once

// Заказы PCM живого потока между ядрами: тик рендера кладёт номер записи
// сэмпла, загрузка на другом ядре забирает и тянет прогон из банка.
//
// Грузить прямо в тике нельзя: типичный сэмпл банка - 2 мс чтения из
// флеша, крупный - 13 мс, а тик длится десять. Один писатель, один
// читатель, индексы атомарные - как у команд голосам.
//
// Повтор заказа безобиден: загрузка идемпотентна, уже резидентный сэмпл
// возвращает успех сразу.

#include <atomic>
#include <cstdint>

namespace player::live {

// Ёмкость кольца заказов. На тик приходится до нескольких десятков записей
// (аккорд со слоями), читатель разбирает кольцо каждый свой проход.
inline constexpr uint32_t kLiveRequestCapacity = 128;
static_assert((kLiveRequestCapacity & (kLiveRequestCapacity - 1)) == 0, "ёмкость - степень двойки: индекс маской");

class LiveRequestRing {
public:
    void clear() {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
        lost_ = 0;
    }

    // Писатель (рендер). false - кольцо полно: заказ потерян, нота
    // промолчит, пока её сэмпл не закажут снова.
    bool push(uint16_t sample_index) {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= kLiveRequestCapacity) {
            ++lost_;
            return false;
        }
        ring_[w & (kLiveRequestCapacity - 1)] = sample_index;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Читатель (загрузка). false - кольцо пусто.
    bool pop(uint16_t& sample_index) {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        sample_index = ring_[r & (kLiveRequestCapacity - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    uint32_t lost() const { return lost_; }
    uint32_t pending() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed); }

private:
    uint16_t ring_[kLiveRequestCapacity] = {};
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    uint32_t lost_ = 0;
};

} // namespace player::live
