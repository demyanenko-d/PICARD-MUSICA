// SPDX-License-Identifier: MIT
#pragma once

// Ядро AY-3-8910 / YM2149. Счёт идёт событиями на сетке тактов clock/8 - на
// ней все периоды целые: полупериод тона TP, сдвиг шума 2*NP, шаг огибающей
// EP (32 шага у обоих чипов; у AY соседние уровни ЦАП равны, и выходит 16
// шагов по 2*EP). Между событиями чип не считается: перепад уровня канала
// ложится ступенькой (blep.h) в дробную позицию выходного отсчёта, позиция -
// точной дробью от абсолютного такта, без накопления ошибки.
//
// Тон выше половины выходной частоты не переключается, а звучит средним:
// половиной уровня. Ступенька срезала бы его целиком, а переключения стоили
// бы сотни тысяч перепадов в секунду. Громкость при этом работает.
//
// Таймер, которого не слышно (тон выключен микшером или громкость ноль,
// шум не пропускает ни один канал, огибающая ни у кого не включена),
// событий не ставит: его счёт догоняется разом на следующей записи. Чип без
// звука не стоит ничего, кроме сведения батча.
//
// Тактовая - параметр экземпляра: у машин и у чипов VGM она своя. Объект
// без глобального состояния: TurboSound и SSG внутри YM2203 - экземпляры.
//
// Батч: begin(n), затем run_to(такт) и write() по порядку записей, затем
// end(out) - отсчёты каналов без смешивания, int16: тишина 0, полный
// уровень 16384, сверху запас под выброс ступеньки.

#include <cstdint>

#include "core/chips/blep.h"

namespace soundsinth::chips {

class AyChip {
public:
    static constexpr uint32_t kChannels = 3;
    static constexpr uint32_t kMaxBatch = 64;
    static constexpr uint32_t kRegs     = 16;

    // clock_hz - вход чипа (у YM2149 с SEL в нуле - уже делённый на 2).
    void init(uint32_t clock_hz, bool ym, uint32_t rate_hz);
    // Регистры в ноль, счётчики с начала, выход - тишина.
    void reset();
    // Все периоды - в 2^shift раз длиннее: делитель входа внутри микросхемы
    // (SSG YM2203), который переключают на ходу. Сетка тактов не меняется;
    // действует с текущего такта.
    void set_period_shift(uint32_t shift);

    // Строки разностей выхода на время батча, kChannels штук (blep.h).
    using Row = BlepOut<kChannels, kMaxBatch>::Row;
    void begin(uint32_t n, Row* rows);
    // Досчитать события до такта units включительно; units - внутри батча
    // (не больше last_units()).
    void run_to(uint64_t units);
    // Запись в текущий момент - после событий этого такта.
    void write(uint8_t reg, uint8_t value);
    // true - в батче был хоть один ненулевой отсчёт; false - все каналы
    // молчали весь батч, сводить нечего.
    bool end(int16_t* const out[kChannels]);

    // Последний такт текущего батча.
    uint64_t last_units() const { return out_.last(); }
    // Отсчёт текущего батча, в который ложится такт units.
    uint32_t sample_at(uint64_t units) const { return out_.at(units).idx; }
    // Первый такт отсчёта sample батча: запись по номеру отсчёта.
    uint64_t units_at(uint32_t sample) const { return out_.first(sample); }
    uint64_t now() const { return now_; }
    uint8_t reg(uint8_t r) const { return regs_[r & 0x0fu]; }
    // Каналов с уровнем ЦАП выше нуля (уровни 0 и 1 молчат), для телеметрии.
    uint32_t heard() const {
        uint32_t n = 0;
        for (uint32_t c = 0; c < kChannels; ++c) {
            const uint8_t v  = regs_[8u + c];
            n               += ((v & 0x10u) != 0 ? env_value_ > 1 : (v & 0x0fu) != 0) ? 1u : 0u;
        }
        return n;
    }
    // Неслышные таймеры - к текущему такту (для сверки состояния).
    void settle();

private:
    static constexpr uint64_t kNever = ~0ull;

    void step_to(uint64_t t);
    void emit(uint64_t t);
    void set_tone_period(uint32_t c);
    void set_envelope_shape();
    void reset_segment();
    void env_step();
    void env_advance(uint64_t k);
    void catch_up();
    void update_activity();
    int32_t level(uint32_t c) const;
    void set_noise_period();
    void set_env_period();
    static uint64_t retime(uint64_t last, uint32_t period, uint64_t now, uint64_t next);

    // Поля в нуле до init(): объект с ненулевыми начальными значениями лёг бы
    // в .data целиком.
    uint64_t now_        = 0;
    const uint16_t* dac_ = nullptr;

    uint8_t regs_[kRegs] = {};
    uint8_t shift_       = 0; // множитель периодов, степень двойки

    uint32_t tone_period_[kChannels] = {};
    uint64_t tone_last_[kChannels]   = {};
    uint64_t tone_next_[kChannels]   = {};
    uint8_t tone_[kChannels]         = {};
    bool tone_avg_[kChannels]        = {};
    bool tone_on_[kChannels]         = {}; // слышен: события ставятся
    bool noise_on_                   = false;
    bool env_on_                     = false;

    uint32_t noise_period_ = 0; // в тактах, 2 * NP
    uint64_t noise_last_   = 0;
    uint64_t noise_next_   = 0;
    uint32_t lfsr_         = 0;

    uint32_t env_period_ = 0;
    uint64_t env_last_   = 0;
    uint64_t env_next_   = 0;
    uint8_t env_shape_   = 0;
    uint8_t env_segment_ = 0;
    int8_t env_value_    = 0;

    BlepOut<kChannels, kMaxBatch> out_;
};

} // namespace soundsinth::chips
