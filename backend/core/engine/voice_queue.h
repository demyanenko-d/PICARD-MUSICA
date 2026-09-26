#pragma once

// Очередь команд от управляющей части к звуковой: тик кладёт, сведение
// разбирает перед батчем. Один писатель, один читатель, индексы атомарные -
// кольцо годится и через ядра. Общего у сторон только кольцо и публикуемое
// состояние голосов; указатель в команде - на неизменное (дескриптор сэмпла
// в резидентной арене).
//
// Запись короткая (на плате 20 байт): в Trigger едет уже разобранное начало
// ноты - звучит ли она, отсчёт, петля, интерполяция, - а не квирки формата.

#include <atomic>
#include <cstdint>

#include "core/config.h"
#include "core/model/instrument.h"

namespace soundsinth::engine {

enum class VoiceOp : uint8_t {
    Stop,              // slot
    Trigger,           // slot, сэмпл и начало ноты
    SetStep,           // slot, z - шаг Q16.16
    FadeBeforeMissing, // slot
    Move,              // slot -> arg
    Fade,              // slot
    FadeStopped,       // flags - волновое гашение
    SetGains,          // slot, x - левое, y - правое
    SetGainsRamp,      // то же плюс сглаживание к ним: их всегда ставят вместе
    FilterOff,         // slot
    SetFilter,         // slot, коэффициенты
    SetRoute,          // slot, flags - в discard, arg - посыл
    ListClear,
    ListPush,     // arg - сколько номеров слотов в записи, сами номера - в slots
    ListRemoveAt, // arg - номер в списке
};

// Сколько номеров слотов влезает в одну запись списка.
inline constexpr uint8_t kListPerCommand = 12;

// Флаги команды Trigger.
inline constexpr uint8_t kTriggerSounds = 1u << 0;  // нота звучит
inline constexpr uint8_t kTriggerLoop = 1u << 1;    // петля годная
inline constexpr uint8_t kTriggerHermite = 1u << 2; // эрмитова интерполяция

struct VoiceCommand {
    VoiceOp op = VoiceOp::Stop;
    uint8_t slot = 0;
    uint8_t arg = 0;
    uint8_t flags = 0;
    union Payload {
        struct {
            int32_t x;
            int32_t y;
            uint32_t z;
        } val;
        // Коэффициенты фильтра: fir121 и active - в flags и arg команды.
        struct {
            int32_t a0;
            int32_t b0;
            int32_t b1;
        } filter;
        // Номера слотов в список: пачкой, иначе каждый голос - своя запись.
        uint8_t slots[12];
        struct {
            const soundsinth::model::SampleDescriptor* sample;
            uint32_t offset; // отсчёт начала, правила формата уже применены
            uint16_t first_page;
            uint16_t checkpoint_first_page;
        } trig;
    } u{};
};
static_assert(sizeof(VoiceCommand) <= 24, "команда голоса - короткая запись"); // на плате 20 байт

// Ёмкость кольца. Самый плотный тик кладёт 315 команд - строка .mid, где
// разом стартуют 32 ноты. Пока обе стороны на одном ядре, переполнение
// команд не теряет: писатель разбирает кольцо сам и кладёт снова. Когда
// стороны разъедутся по ядрам, ёмкость обязана покрывать тик целиком.
inline constexpr uint32_t kVoiceQueueCapacity = 128;
static_assert((kVoiceQueueCapacity & (kVoiceQueueCapacity - 1)) == 0, "ёмкость - степень двойки: индекс маской");

// Кольцо на одного писателя и одного читателя. Писатель двигает write_ после
// записи (release), читатель читает его acquire - запись команды не обгонит
// её появления в кольце на другом ядре.
class VoiceQueue {
public:
    bool empty() const { return read_ == write_.load(std::memory_order_acquire); }
    uint32_t pending() const { return write_.load(std::memory_order_acquire) - read_; }

    // false - кольцо полно, команда не принята: разбирать и повторять - за
    // вызывающим.
    bool push(const VoiceCommand& cmd) {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= kVoiceQueueCapacity) return false;
        ring_[w & (kVoiceQueueCapacity - 1)] = cmd;
        write_.store(w + 1, std::memory_order_release);
        if (pending() > peak_) peak_ = pending();
        return true;
    }

    // Все накопленные команды по порядку, apply(cmd) - применение.
    template <typename Apply>
    void drain(Apply&& apply) {
        const uint32_t w = write_.load(std::memory_order_acquire);
        uint32_t r = read_.load(std::memory_order_relaxed);
        while (r != w) {
            apply(ring_[r & (kVoiceQueueCapacity - 1)]);
            ++r;
        }
        read_.store(r, std::memory_order_release);
    }

    // Наибольшее заполнение кольца за трек: по нему видно, хватает ли ёмкости.
    uint32_t peak() const { return peak_; }

private:
    VoiceCommand ring_[kVoiceQueueCapacity];
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    uint32_t peak_ = 0;
};

} // namespace soundsinth::engine
