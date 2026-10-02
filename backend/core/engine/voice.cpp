// SPDX-License-Identifier: MIT
#include "core/engine/voice.h"

#include <cmath>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "core/codec/block_locate.h"
#include "core/engine/engine_defs.h"
#include "core/model/amiga_period.h"
#include "core/audio/saturate.h"

namespace soundsinth::engine {

namespace {

using soundsinth::model::ResidentEncoding;

// Период ноты 48 (kAmigaPeriodTable[12]).
constexpr uint32_t kC4Period = 428;

// Отдельные переменные, а не структура: структура меняет распределение
// регистров во всех вариантах цикла рендера; без замера на плате не менять.
uint32_t s_decode_calls     = 0;
uint32_t s_discarded_dpcm8  = 0;
uint32_t s_discarded_direct = 0;
uint32_t s_direct_jumps     = 0;
uint32_t s_step_clamps      = 0;
uint32_t s_pitch_recalcs    = 0;

// Продвинуть page/byte_offset на stride байт: Raw8 и Dpcm8 - 1, Raw16 - 2.
void advance_byte_position(uint16_t& page, uint16_t& byte_offset, memory::PsramStore& psram, uint32_t stride = 1) {
    byte_offset = static_cast<uint16_t>(byte_offset + stride);
    if (byte_offset >= memory::kPsramPageBytes) {
        byte_offset = static_cast<uint16_t>(byte_offset - memory::kPsramPageBytes);
        page        = memory::psram_page_next(psram, page);
    }
}

// Raw16 читает два байта подряд, значение не должно разрываться границей
// страницы. Не разорвётся: страница чётная, поток Raw16 начинается с
// нулевого смещения и растёт по два байта. Инвариант держится, пока в
// потоке Raw16 только значения (контрольные точки Dpcm8 дописываются после
// данных). Ассерт ломает сборку, а не звук, при смене размера страницы.
static_assert(memory::kPsramPageBytes % 2 == 0, "Raw16: the page must be even, otherwise a value is torn across the boundary");

// Резидентно Raw8 хранит исходную 8-битную шкалу, до 16 бит раскрывается при
// чтении.
inline int16_t raw8_to_s16(uint8_t byte) {
    return static_cast<int16_t>(static_cast<int32_t>(static_cast<int8_t>(byte)) * 256);
}

// Raw16: два байта, младший первым - как кладёт упаковщик.
inline int16_t raw16_from_bytes(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

// Точка петли: позиция декодера на отсчёте loop_start, ставится один раз.
inline void mark_loop_checkpoint(Voice& v, uint16_t page, uint16_t byte_offset) {
    v.loop_checkpoint_page        = page;
    v.loop_checkpoint_byte_offset = byte_offset;
    v.loop_checkpoint_captured    = true;
}

// Линейная интерполяция между prev и next, t = frac_pos.
inline int16_t interpolate_linear(const Voice& v) {
    const int32_t delta = static_cast<int32_t>(v.next_sample) - static_cast<int32_t>(v.prev_sample);
    const int32_t interp =
        static_cast<int32_t>(v.prev_sample) + static_cast<int32_t>((static_cast<int64_t>(delta) * static_cast<int64_t>(v.frac_pos)) >> kQ16Bits);
    return static_cast<int16_t>(interp);
}

// Кубическая эрмитова (Катмулла-Рома) между older и prev, t = frac_pos:
// c = (x1 - x-1)/2, v = x0 - x1, w = c + v, a = w + v + (x2 - x0)/2,
// b = w + a, y = ((a*t - b)*t + c)*t + x0. Коэффициенты удвоены, чтобы
// остаться в целых; деление на 2 - в конце. При t < 0x10000 |2a| <= 262140,
// |2b| <= 393210, acc между шагами меньше 2^20 - int32; произведение acc * t
// до 2^36 - в int64.
inline int16_t interpolate_hermite(const Voice& v) {
    const int32_t xm1     = v.oldest_sample;
    const int32_t x0      = v.older_sample;
    const int32_t x1      = v.prev_sample;
    const int32_t x2      = v.next_sample;
    const int32_t twice_c = x1 - xm1;
    const int32_t twice_v = 2 * (x0 - x1);
    const int32_t twice_w = twice_c + twice_v;
    const int32_t twice_a = twice_w + twice_v + (x2 - x0);
    const int32_t twice_b = twice_w + twice_a;
    const int32_t t       = static_cast<int32_t>(v.frac_pos);
    int32_t acc           = static_cast<int32_t>((static_cast<int64_t>(twice_a) * t) >> kQ16Bits);
    acc                   = static_cast<int32_t>((static_cast<int64_t>(acc - twice_b) * t) >> kQ16Bits);
    acc                   = static_cast<int32_t>((static_cast<int64_t>(acc + twice_c) * t) >> kQ16Bits);
    // Кубика перелетает соседние отсчёты; буфер голоса 16-битный.
    return static_cast<int16_t>(sat_s16(x0 + (acc >> 1)));
}

// Самый горячий путь движка: в SRAM, чтобы не делить QMI с кодом из флеша на
// другом ядре.
template <ResidentEncoding kEnc>
int16_t SOUNDSINTH_HOT_PATH(decode_next_native)(Voice& v, memory::PsramStore& psram) {
    ++s_decode_calls; // один вызов == один новый родной отсчёт
    if constexpr (kEnc == ResidentEncoding::Raw16) {
        const int16_t sample16 = raw16_from_bytes(memory::psram_page_ptr(psram, v.page) + v.byte_offset);
        advance_byte_position(v.page, v.byte_offset, psram, 2);
        return sample16;
    }

    const uint8_t byte = memory::psram_page_ptr(psram, v.page)[v.byte_offset];
    int16_t sample;
    if constexpr (kEnc == ResidentEncoding::Raw8) {
        sample = raw8_to_s16(byte);
    } else {
        sample = dpcm8::decode_delta(byte, v.dpcm_state);
    }
    advance_byte_position(v.page, v.byte_offset, psram);
    return sample;
}

// Пропуск выбрасываемых отсчётов Dpcm8: указатель страницы - раз на
// страницу, в теле только decode_delta. Через decode_and_advance отсчёт
// стоит 281 нс (84 такта), распаковка из них - единицы. Пока точка петли не
// захвачена, через loop_start пропуск не идёт.
void SOUNDSINTH_HOT_PATH(dpcm_skip_forward)(Voice& v, memory::PsramStore& psram, uint32_t count) {
    int32_t predictor = v.dpcm_state.predictor;
    while (count > 0) {
        const uint8_t* page = memory::psram_page_ptr(psram, v.page);
        // Сколько влезает до конца страницы - столько идём без проверки границы.
        const uint32_t in_page = memory::kPsramPageBytes - v.byte_offset;
        const uint32_t n       = count < in_page ? count : in_page;
        const uint8_t* q       = page + v.byte_offset;
        for (uint32_t i = 0; i < n; ++i) {
            predictor = dpcm8::decode_step(predictor, q[i]);
        }
        v.byte_offset    = static_cast<uint16_t>(v.byte_offset + n);
        v.decoded_count += n;
        count           -= n;
        if (v.byte_offset >= memory::kPsramPageBytes) {
            v.byte_offset = 0;
            v.page        = memory::psram_page_next(psram, v.page);
        }
    }
    v.dpcm_state.predictor = static_cast<int16_t>(predictor);
}

// Декодирует родной отсчёт v.decoded_count и продвигает decoded_count -
// единая точка всех декодов (voice_trigger и voice_render), чтобы
// контрольная точка петли захватывалась один раз, при первом проходе через
// loop_start.
//
// Только Dpcm8: прямым кодекам точку ставит direct_loop_checkpoint на первом
// завороте, и лишнее сравнение на каждом их декоде не нужно.
template <ResidentEncoding kEnc>
int16_t SOUNDSINTH_HOT_PATH(decode_and_advance)(Voice& v, memory::PsramStore& psram) {
    if constexpr (kEnc == ResidentEncoding::Dpcm8) {
        if (v.loop_enabled && !v.loop_checkpoint_captured && v.decoded_count == v.loop_start) {
            v.loop_checkpoint_dpcm_state = v.dpcm_state;
            mark_loop_checkpoint(v, v.page, v.byte_offset);
        }
    }
    const int16_t sample = decode_next_native<kEnc>(v, psram);
    ++v.decoded_count;
    return sample;
}

// Обёртка для холодного пути (voice_trigger, раз на ноту).
int16_t decode_and_advance_rt(Voice& v, memory::PsramStore& psram) {
    switch (v.resident_encoding) {
        case ResidentEncoding::Raw8:
            return decode_and_advance<ResidentEncoding::Raw8>(v, psram);
        case ResidentEncoding::Raw16:
            return decode_and_advance<ResidentEncoding::Raw16>(v, psram);
        case ResidentEncoding::Dpcm8:
            return decode_and_advance<ResidentEncoding::Dpcm8>(v, psram);
    }
    return 0;
}

struct SeekPosition {
    uint16_t page        = memory::kPageChainEnd;
    uint16_t byte_offset = 0;
    dpcm8::Dpcm8State state; // только для Dpcm8
};

// Позиция декодера Dpcm8 ровно на sample_pos-м родном отсчёте: от ближайшей
// предыдущей контрольной точки и декодирование остатка - O(страниц) +
// O(sample_pos % kCheckpointIntervalSamples). checkpoint_first_page обязан
// быть валиден.
SeekPosition seek_dpcm8(memory::PsramStore& psram, uint16_t first_page, uint16_t checkpoint_first_page, uint32_t sample_pos) {
    SeekPosition pos;
    const uint32_t block_index           = sample_pos / dpcm8::kCheckpointIntervalSamples;
    const uint32_t remainder             = sample_pos % dpcm8::kCheckpointIntervalSamples;
    const dpcm8::BlockPosition block_pos = dpcm8::locate_block(psram, first_page, checkpoint_first_page, block_index);
    pos.page                             = block_pos.page;
    pos.byte_offset                      = block_pos.byte_offset;
    pos.state                            = block_pos.state;

    int32_t predictor = pos.state.predictor;
    for (uint32_t i = 0; i < remainder; ++i) {
        predictor = dpcm8::decode_step(predictor, memory::psram_page_ptr(psram, pos.page)[pos.byte_offset]);
        advance_byte_position(pos.page, pos.byte_offset, psram);
    }
    pos.state.predictor = static_cast<int16_t>(predictor);
    return pos;
}

// Позиция прямого кодека от известной страницы якоря, а не от first_page:
// обходит только границы страниц между якорем и целью, без заворота петли
// почти всегда ни одной. Только для target_pos >= anchor_pos.
SeekPosition seek_direct_forward(memory::PsramStore& psram, uint16_t anchor_page, uint32_t anchor_pos, uint32_t target_pos, uint32_t stride) {
    SeekPosition pos;
    // Позиции в отсчётах, страницы - в байтах; у Raw16 это разные величины,
    // переводятся обе, иначе якорь и цель разъедутся.
    const uint32_t anchor_byte = anchor_pos * stride;
    const uint32_t target_byte = target_pos * stride;
    const uint32_t page_delta  = target_byte / memory::kPsramPageBytes - anchor_byte / memory::kPsramPageBytes;
    pos.page                   = memory::psram_page_advance(psram, anchor_page, page_delta);
    pos.byte_offset            = static_cast<uint16_t>(target_byte % memory::kPsramPageBytes);
    return pos;
}

// Точка петли прямого кодека: позиция loop_start, при первом обращении
// посчитанная от начала сэмпла (один обход страниц до loop_start на голос),
// дальше - готовая. Заворот и прыжок через конец петли идут от неё, а не
// поиском от first_page на каждом витке: у петли далеко от начала это сотни
// обходов page_next[] на виток.
SOUNDSINTH_NOINLINE void SOUNDSINTH_HOT_PATH(direct_loop_checkpoint)(Voice& v, memory::PsramStore& psram, uint32_t stride) {
    if (v.loop_checkpoint_captured) return;
    const SeekPosition p = seek_direct_forward(psram, v.first_page, 0, v.loop_start, stride);
    mark_loop_checkpoint(v, p.page, p.byte_offset);
}

// Значение отсчёта по готовой позиции - для кодеков без состояния.
template <ResidentEncoding kEnc>
int16_t read_direct_sample(memory::PsramStore& psram, const SeekPosition& pos) {
    const uint8_t* p = memory::psram_page_ptr(psram, pos.page) + pos.byte_offset;
    if constexpr (kEnc == ResidentEncoding::Raw16) {
        return raw16_from_bytes(p);
    } else {
        return raw8_to_s16(p[0]);
    }
}

// Шаг Q16.16 из периода Amiga, не меньше 1 (иначе голос не продвинется).
uint32_t amiga_period_to_step_impl(uint16_t period, uint32_t c5_speed) {
    const uint64_t numerator   = static_cast<uint64_t>(kC4Period) * c5_speed * kQ16One;
    const uint64_t denominator = static_cast<uint64_t>(kSampleRateHz) * period;
    const uint32_t step        = static_cast<uint32_t>(numerator / denominator);
    return step != 0 ? step : 1;
}

// double -> uint32_t без UB: неограниченное портаменто доводит pow до +inf,
// cast вне диапазона - UB (Cortex-M33 насыщает до 0xFFFFFFFF). Потолок
// 0xFFFF0000: frac_pos < 0x10000, сумма с шагом не переполняет uint32.
constexpr double kMaxStepF = 4294901760.0; // 0xFFFF0000
uint32_t step_from_double(double step_f) {
    if (!(step_f > 0.0)) return 0; // ловит и NaN: сравнение с NaN всегда false
    if (step_f > kMaxStepF) {
        step_f = kMaxStepF;
        ++s_step_clamps;
    }
    return static_cast<uint32_t>(step_f + 0.5);
}

// Шаг Q16.16 модели Linear: c5_speed * 2^(полутоны/12) в выходных отсчётах,
// не меньше 1 (иначе голос не продвинется). Порядок операций над double -
// тот же в обоих вызывающих: побитовость рендеров XM/IT держится на нём.
uint32_t step_from_semitones(uint32_t c5_speed, double semitones) {
    const double native_hz = static_cast<double>(c5_speed) * std::pow(2.0, semitones / 12.0);
    const double step_f    = native_hz * static_cast<double>(kQ16One) / static_cast<double>(kSampleRateHz);
    const uint32_t step    = step_from_double(step_f);
    return step != 0 ? step : 1;
}

uint32_t compute_step(const soundsinth::model::SampleDescriptor& sample, uint8_t note, soundsinth::model::FrequencyModel frequency_model,
                      soundsinth::model::QuirkFlags quirks) {
    const uint8_t effective_note = soundsinth::model::apply_relative_note(note, sample.relative_note);

    if (frequency_model == soundsinth::model::FrequencyModel::Linear) {
        // Экспонента, а не таблица FT2 (768 на октаву): расхождение в долях
        // цента. Finetune только в стиле XM (128 на полутон), подстройка
        // MOD/S3M не применяется.
        const uint8_t reference_note = linear_reference_note(quirks);
        const double semitones = (static_cast<double>(effective_note) - static_cast<double>(reference_note)) + static_cast<double>(sample.finetune) / 128.0;
        return step_from_semitones(sample.c5_speed, semitones);
    }

    const uint16_t period = soundsinth::model::amiga_note_to_period(effective_note);
    return amiga_period_to_step_impl(period, sample.c5_speed);
}

// SampleOffset в отсчётах хранимого потока. За концом (у зацикленного - за
// концом петли): XM - нота не звучит, S3M и MOD - заворот внутрь петли (за
// концом незацикленного - не звучит), IT - играет с начала, со старыми
// эффектами - с последнего отсчёта; на этом у IT пишут музыку (сэмпл в 20
// отсчётов с Oxx до 0xFE). false - нота не звучит.
bool resolve_start_offset(const soundsinth::model::SampleDescriptor& sample, soundsinth::model::QuirkFlags quirks, bool loop_ok, uint32_t& offset) {
    // Прореженный сэмпл вдвое короче: смещение делится до проверки.
    if (sample.decimated) offset /= 2;
    // Конец петли для смещения - исходный, до разворота ping-pong.
    const uint32_t offset_loop_end = soundsinth::model::loop_end_before_unroll(sample);
    if (loop_ok && offset >= offset_loop_end) {
        if (quirks & soundsinth::model::kQuirkS3mOffsetWrapInLoop) {
            offset = (offset - sample.loop_start) % (offset_loop_end - sample.loop_start) + sample.loop_start;
        } else if (quirks & soundsinth::model::kQuirkModOffsetPastLoopEnd) {
            offset = sample.loop_start;
        }
    }
    const bool past_end = offset >= sample.length_samples || (sample.loop_enabled && offset_loop_end > 0 && offset >= offset_loop_end);
    if (!past_end) return true;
    if ((quirks & soundsinth::model::kQuirkItOffsetPastEndRestarts) == 0) return false;
    // Позиция ровно на длине снова была бы за концом - берётся последний отсчёт.
    offset = (quirks & soundsinth::model::kQuirkItOldEffects) ? (sample.length_samples - 1) : 0;
    return true;
}

// Позиция голоса на отсчёте offset > 0 до выдачи звука. Прямые кодеки и Dpcm8
// с контрольными точками - по страницам, Dpcm8 без точек (синтетика в тестах)
// - линейным проходом. Точку петли Dpcm8, перепрыгнутую смещением, ставит
// здесь же; прямым кодекам её ставит direct_loop_checkpoint при первом
// завороте или прыжке.
void seek_voice_to(Voice& voice, memory::PsramStore& psram, uint16_t checkpoint_first_page, uint32_t offset) {
    if (soundsinth::model::resident_is_direct(voice.resident_encoding)) {
        const SeekPosition target =
            seek_direct_forward(psram, voice.first_page, 0, offset, soundsinth::model::resident_bytes_per_sample(voice.resident_encoding));
        voice.page          = target.page;
        voice.byte_offset   = target.byte_offset;
        voice.decoded_count = offset;
        return;
    }
    if (checkpoint_first_page == memory::kPageChainEnd) {
        for (uint32_t i = 0; i < offset; ++i) {
            decode_and_advance_rt(voice, psram);
        }
        return;
    }
    const SeekPosition target = seek_dpcm8(psram, voice.first_page, checkpoint_first_page, offset);
    voice.page                = target.page;
    voice.byte_offset         = target.byte_offset;
    voice.dpcm_state          = target.state;
    voice.decoded_count       = offset;
    // loop_start почти никогда не выровнен по kCheckpointIntervalSamples:
    // seek_dpcm8, а не locate_block.
    if (voice.loop_enabled && offset > voice.loop_start) {
        const SeekPosition loop_pos      = seek_dpcm8(psram, voice.first_page, checkpoint_first_page, voice.loop_start);
        voice.loop_checkpoint_dpcm_state = loop_pos.state;
        mark_loop_checkpoint(voice, loop_pos.page, loop_pos.byte_offset);
    }
}

// Первые два отсчёта окна интерполяции. Первый на конце петли или за ним
// (смещение loop_end - 1, у старых эффектов IT - последний отсчёт сэмпла):
// следующий - с loop_start, как у последовательного пути рендера. Иначе
// next брался бы за петлёй, а decoded_count за loop_end разводил бы прыжок
// прямых кодеков и последовательный путь.
void prime_interpolation(Voice& voice, memory::PsramStore& psram, uint32_t length_samples) {
    voice.prev_sample = decode_and_advance_rt(voice, psram);
    if (voice.loop_enabled && voice.decoded_count >= voice.loop_end) {
        if (soundsinth::model::resident_is_direct(voice.resident_encoding)) {
            direct_loop_checkpoint(voice, psram, soundsinth::model::resident_bytes_per_sample(voice.resident_encoding));
        } else {
            // Dpcm8: точка захвачена - проходом через loop_start или в seek_voice_to.
            voice.dpcm_state = voice.loop_checkpoint_dpcm_state;
        }
        voice.page          = voice.loop_checkpoint_page;
        voice.byte_offset   = voice.loop_checkpoint_byte_offset;
        voice.decoded_count = voice.loop_start;
    }
    if (voice.decoded_count < length_samples) {
        voice.next_sample = decode_and_advance_rt(voice, psram);
    } else {
        voice.next_sample = voice.prev_sample;
    }
    voice.frac_pos = 0;
}

} // namespace

TriggerStart voice_trigger_start(const soundsinth::model::SampleDescriptor& sample, uint16_t first_page, uint32_t start_offset,
                                 soundsinth::model::QuirkFlags quirks) {
    TriggerStart s{};
    s.offset = start_offset;
    if (sample.length_samples == 0 || first_page == memory::kPageChainEnd) {
        return s; // пустой или нерезидентный сэмпл (в том числе не влезший в PSRAM)
    }
    // Битые границы (loop_start >= loop_end или loop_end > length_samples) -
    // без петли. Заворот смещения - только по проверенной петле: у битой
    // длина петли ноль или "отрицательная".
    s.loop_ok = sample.loop_enabled && sample.loop_start < sample.loop_end && sample.loop_end <= sample.length_samples;
    s.sounds  = resolve_start_offset(sample, quirks, s.loop_ok, s.offset);
    return s;
}

void voice_trigger_prepared(Voice& voice, memory::PsramStore& psram, const soundsinth::model::SampleDescriptor& sample, uint16_t first_page,
                            uint16_t checkpoint_first_page, const TriggerStart& start, bool hermite) {
    voice = Voice{};
    if (!start.sounds) return; // нота не звучит: пустой или нерезидентный сэмпл, смещение за концом
    const uint32_t offset = start.offset;

    voice.resident_encoding = sample.resident_encoding;
    voice.hermite           = hermite;
    voice.first_page        = first_page;
    voice.page              = first_page;

    // Петля всегда прямая, ping-pong развёрнут при упаковке.
    voice.loop_enabled = start.loop_ok;
    voice.loop_start   = sample.loop_start;
    voice.loop_end     = voice.loop_enabled ? sample.loop_end : sample.length_samples;

    voice.decoded_count = 0;
    if (offset > 0) seek_voice_to(voice, psram, checkpoint_first_page, offset);
    prime_interpolation(voice, psram, sample.length_samples);

    voice.active = true;
}

void voice_trigger(Voice& voice, memory::PsramStore& psram, const soundsinth::model::SampleDescriptor& sample, uint16_t first_page, uint8_t note,
                   soundsinth::model::FrequencyModel frequency_model, uint32_t start_offset, soundsinth::model::QuirkFlags quirks,
                   uint16_t checkpoint_first_page, bool set_step) {
    const TriggerStart start = voice_trigger_start(sample, first_page, start_offset, quirks);
    voice_trigger_prepared(voice, psram, sample, first_page, checkpoint_first_page, start, voice_hermite(sample, quirks));
    if (set_step) voice.step = compute_step(sample, note, frequency_model, quirks);
}

uint32_t voice_step_amiga(uint16_t period, uint32_t c5_speed) {
    ++s_pitch_recalcs;
    return amiga_period_to_step_impl(period, c5_speed);
}

uint32_t voice_step_linear(int32_t amount_units, uint32_t c5_speed) {
    ++s_pitch_recalcs;
    return step_from_semitones(c5_speed, static_cast<double>(amount_units) / static_cast<double>(kLinearAmountUnitsPerSemitone));
}

void voice_recompute_amiga_step(Voice& voice, uint16_t period, uint32_t c5_speed) {
    voice.step          = voice_step_amiga(period, c5_speed);
    voice.pitch_memo    = period;
    voice.pitch_memo_c5 = c5_speed;
}

void voice_recompute_linear_step(Voice& voice, int32_t amount_units, uint32_t c5_speed) {
    voice.step          = voice_step_linear(amount_units, c5_speed);
    voice.pitch_memo    = amount_units;
    voice.pitch_memo_c5 = c5_speed;
}

// Вне безымянного namespace намеренно: с внутренним связыванием GCC встраивает
// варианты в voice_render и выносит decode_and_advance<Dpcm8> из цикла.
//
// Кодек - параметр шаблона, а не поле, читаемое в цикле: иначе на каждый
// выходной отсчёт три сравнения resident_encoding с ветвлениями и чтение
// поля, хотя кодек голоса не меняется.
//
// kHermite - интерполяция по четырём точкам (Voice::hermite). Сколько
// последних родных отсчётов она берёт (kUsed), столько быстрые пути ниже
// оставляют обычному проходу, который сдвигает окно.
template <ResidentEncoding kEnc, bool kHermite>
uint32_t SOUNDSINTH_HOT_PATH(voice_render_impl)(Voice& voice, memory::PsramStore& psram, int16_t* out, uint32_t n_frames) {
    constexpr uint32_t kUsed   = kHermite ? 4u : 2u;
    constexpr bool kDirect     = soundsinth::model::resident_is_direct(kEnc);
    constexpr uint32_t kStride = soundsinth::model::resident_bytes_per_sample(kEnc);
    // Быстрые пути - от kUsed + 1 целых шагов.
    constexpr uint32_t kFastPathMinFrac = (kUsed + 1u) << kQ16Bits;
    uint32_t produced                   = 0;
    while (produced < n_frames && voice.active) {
        if constexpr (kHermite) {
            out[produced++] = interpolate_hermite(voice);
        } else {
            out[produced++] = interpolate_linear(voice);
        }

        voice.frac_pos += voice.step;

        // Прыжок прямых кодеков (Raw8, Raw16): у Dpcm8 значение байта зависит от
        // всей предыдущей цепочки, прямой кодек читается без состояния - значение
        // на любой позиции берётся по индексу. При высоком питче последовательный
        // путь декодирует N родных отсчётов, а используются только последние kUsed;
        // прыжок считает их сразу.
        // Результат побитово тот же.
        // Порог - kUsed + 1 целых шагов: при меньшем последовательный путь
        // ничего не выбрасывает. Конец незацикленного сэмпла внутри пачки -
        // последовательным путём: голос гаснет на точном шаге.
        bool fast_path_done = false;
        if constexpr (kDirect) {
            if (voice.active && voice.frac_pos >= kFastPathMinFrac) {
                const uint32_t n_steps = voice.frac_pos >> kQ16Bits;
                const uint32_t start   = voice.decoded_count;
                // Позиция после ровно steps продвижений от start - та же арифметика, что
                // последовательный цикл за steps итераций, с учётом нескольких витков петли
                // (%). Годится, пока путь в зацикленной области: конец незацикленного
                // сэмпла внутри пачки уже отсечён (fast_ok).
                auto advance_wrapped = [&](uint32_t steps) -> uint32_t {
                    if (start + steps < voice.loop_end) return start + steps;
                    const uint32_t loop_len       = voice.loop_end - voice.loop_start; // > 0 гарантировано в voice_trigger
                    const uint32_t past_first_leg = steps - (voice.loop_end - start);
                    return voice.loop_start + past_first_leg % loop_len;
                };
                const bool fast_ok = (start + n_steps < voice.loop_end) || voice.loop_enabled;
                if (fast_ok) {
                    // Цели, обернувшиеся к loop_start, ищутся от точки петли.
                    const bool wraps = start + n_steps >= voice.loop_end;
                    if (wraps) direct_loop_checkpoint(voice, psram, kStride);
                    auto seek = [&](uint32_t t) -> SeekPosition {
                        return t >= start ? seek_direct_forward(psram, voice.page, start, t, kStride)
                                          : seek_direct_forward(psram, voice.loop_checkpoint_page, voice.loop_start, t, kStride);
                    };
                    // decoded_count/page/byte_offset после пачки указывают на следующую
                    // непрочитанную позицию - после n_steps продвижений. prev и next - два
                    // последних отсчёта пачки: после n_steps-2 и n_steps-1 продвижений.
                    const uint32_t target         = advance_wrapped(n_steps);
                    const uint32_t next_target    = advance_wrapped(n_steps - 1);
                    const uint32_t prev_target    = advance_wrapped(n_steps - 2);
                    const SeekPosition target_pos = seek(target);
                    const SeekPosition next_pos   = seek(next_target);
                    const SeekPosition prev_pos   = seek(prev_target);
                    voice.prev_sample             = read_direct_sample<kEnc>(psram, prev_pos);
                    voice.next_sample             = read_direct_sample<kEnc>(psram, next_pos);
                    if constexpr (kHermite) {
                        // Окну нужны ещё два отсчёта перед prev - позиции n_steps-3 и n_steps-4
                        // (порог выше гарантирует n_steps >= 5).
                        const uint32_t older_target   = advance_wrapped(n_steps - 3);
                        const uint32_t oldest_target  = advance_wrapped(n_steps - 4);
                        const SeekPosition older_pos  = seek(older_target);
                        const SeekPosition oldest_pos = seek(oldest_target);
                        voice.older_sample            = read_direct_sample<kEnc>(psram, older_pos);
                        voice.oldest_sample           = read_direct_sample<kEnc>(psram, oldest_pos);
                    }
                    voice.decoded_count  = target;
                    voice.page           = target_pos.page;
                    voice.byte_offset    = target_pos.byte_offset;
                    voice.frac_pos      &= (kQ16One - 1);
                    fast_path_done       = true;
                    ++s_direct_jumps;
                }
            }
        }

        // --- Пропуск выбрасываемых отсчётов тесной петлёй ---
        // У прямых кодеков свой прыжок (resident_is_direct); здесь закрывается
        // Dpcm8, где прыгнуть нельзя, но можно пройти дёшево. Условия: не меньше
        // kUsed + 1 целых шагов, весь путь до loop_end, через loop_start - только
        // после захвата его точки. Последние kUsed отсчётов - обычным путём, они
        // идут в окно интерполяции.
        if constexpr (kEnc == ResidentEncoding::Dpcm8) {
            if (!fast_path_done && voice.active && voice.frac_pos >= kFastPathMinFrac) {
                const uint32_t whole = voice.frac_pos >> kQ16Bits;
                const uint32_t skip  = whole - kUsed; // последние kUsed - обычным путём
                if (voice.decoded_count + whole <= voice.loop_end &&
                    !(voice.loop_enabled && !voice.loop_checkpoint_captured && voice.decoded_count <= voice.loop_start &&
                      voice.loop_start < voice.decoded_count + skip)) {
                    dpcm_skip_forward(voice, psram, skip);
                    voice.frac_pos    -= static_cast<uint32_t>(skip) << kQ16Bits;
                    s_discarded_dpcm8 += skip;
                }
            }
        }

        // burst - сколько декодов сделал последовательный цикл за этот выходной
        // отсчёт (пропуск Dpcm8 выше считается отдельно); интерполяцией
        // используются только последние kUsed, остальные - ради состояния
        // предиктора и позиции. Если сработал прыжок, цикл не выполняется.
        uint32_t burst = 0;
        while (!fast_path_done && voice.frac_pos >= kQ16One && voice.active) {
            voice.frac_pos -= kQ16One;
            if constexpr (kHermite) {
                voice.oldest_sample = voice.older_sample;
                voice.older_sample  = voice.prev_sample;
            }
            voice.prev_sample = voice.next_sample;
            if (voice.decoded_count >= voice.loop_end) {
                if (!voice.loop_enabled) {
                    voice.active = false; // конец незацикленного сэмпла
                    break;
                }
                if constexpr (kDirect) {
                    // Точка петли ставится здесь или раньше прыжком; без неё - чтение
                    // kPageChainEnd и падение.
                    direct_loop_checkpoint(voice, psram, kStride);
                } else {
                    // Dpcm8: точка захвачена гарантированно - прыжка у этого кодека нет, и
                    // decoded_count не мог дойти до loop_end, не пройдя loop_start.
                    voice.dpcm_state = voice.loop_checkpoint_dpcm_state;
                }
                voice.page          = voice.loop_checkpoint_page;
                voice.byte_offset   = voice.loop_checkpoint_byte_offset;
                voice.decoded_count = voice.loop_start;
            }
            voice.next_sample = decode_and_advance<kEnc>(voice, psram);
            ++burst;
        }
        if (burst > kUsed) {
            if constexpr (kEnc == ResidentEncoding::Dpcm8) {
                s_discarded_dpcm8 += (burst - kUsed);
            } else {
                s_discarded_direct += (burst - kUsed);
            }
        }
    }
    return produced;
}

// Диспетчер: один switch на вызов вместо трёх сравнений на отсчёт.
// Эрмитовы варианты - только Raw8 и Dpcm8 (у банков .mid других кодеков
// нет); voice_trigger не ставит hermite голосу с Raw16.
uint32_t SOUNDSINTH_HOT_PATH(voice_render)(Voice& voice, memory::PsramStore& psram, int16_t* out, uint32_t n_frames) {
    if (voice.hermite) {
        if (voice.resident_encoding == ResidentEncoding::Dpcm8) {
            return voice_render_impl<ResidentEncoding::Dpcm8, true>(voice, psram, out, n_frames);
        }
        return voice_render_impl<ResidentEncoding::Raw8, true>(voice, psram, out, n_frames);
    }
    switch (voice.resident_encoding) {
        case ResidentEncoding::Raw8:
            return voice_render_impl<ResidentEncoding::Raw8, false>(voice, psram, out, n_frames);
        case ResidentEncoding::Raw16:
            return voice_render_impl<ResidentEncoding::Raw16, false>(voice, psram, out, n_frames);
        case ResidentEncoding::Dpcm8:
            return voice_render_impl<ResidentEncoding::Dpcm8, false>(voice, psram, out, n_frames);
    }
    return 0;
}

void voice_reset_debug_counters() {
    s_decode_calls     = 0;
    s_discarded_dpcm8  = 0;
    s_discarded_direct = 0;
    s_direct_jumps     = 0;
    s_step_clamps      = 0;
    s_pitch_recalcs    = 0;
}

VoiceDebugCounters voice_debug_counters() {
    VoiceDebugCounters c{};
    c.decode_calls     = s_decode_calls;
    c.discarded_dpcm8  = s_discarded_dpcm8;
    c.discarded_direct = s_discarded_direct;
    c.direct_jumps     = s_direct_jumps;
    c.step_clamps      = s_step_clamps;
    c.pitch_recalcs    = s_pitch_recalcs;
    return c;
}

} // namespace soundsinth::engine
