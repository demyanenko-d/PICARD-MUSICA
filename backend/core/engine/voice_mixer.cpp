#include "core/engine/voice_mixer.h"

#include "platform/hot_path.h"

// __smlawb (ACLE) - заголовок компилятора GCC, есть только в сборке под
// RP2350 (-march=...+dsp определяет __ARM_FEATURE_DSP). На PC (MSVC) этого
// пути нет, запасной вариант ниже даёт ту же формулу побитово.
#if defined(__GNUC__) && defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#define SOUNDSINTH_HAVE_SMLAWB 1
#endif

namespace soundsinth::engine {

namespace {

#if !defined(SOUNDSINTH_HAVE_SMLAWB)
int32_t q16_scale(int32_t a, int32_t b_q16) { // знаковое: у surround правое отрицательное
    return static_cast<int32_t>((static_cast<int64_t>(a) * b_q16) >> kQ16Bits);
}
#endif

// Ограничение промаха интерполяции: рампа не должна перескочить через ноль
// в чужой знак. У surround-канала правое усиление законно отрицательное, и
// ограничение "g < 0 ? 0 : g" глушило бы правый канал на всё время
// сглаживания.
int32_t clamp_ramp_gain(int32_t g, int32_t toward_sign_of) {
    if (toward_sign_of >= 0) return g < 0 ? 0 : g;
    return g > 0 ? 0 : g;
}

// Вклад одного голоса в mix_l/mix_r за один выходной отсчёт; gain_l_q24/
// gain_r_q24 готовы (раз за тик). mix_l/mix_r - int32.
// На RP2350 (__ARM_FEATURE_DSP) "умножить на усиление и прибавить к
// аккумулятору" - одна инструкция SMLAWB: (Rn * SignExtend16(Rm)) >> 16 + Ra,
// где Rn - усиление (все 32 бита, до 134 млн), Rm - отсчёт int16.
// Результат побитово совпадает с q16_scale. Запасной вариант для PC - та
// же формула развёрнуто.
void SOUNDSINTH_HOT_PATH(mix_native_sample)(int16_t native_sample, int32_t gain_l_q24, int32_t gain_r_q24,
                                            int32_t* mix_l, int32_t* mix_r) {
#if defined(SOUNDSINTH_HAVE_SMLAWB)
    *mix_l = __smlawb(gain_l_q24, static_cast<int32_t>(native_sample), *mix_l);
    *mix_r = __smlawb(gain_r_q24, static_cast<int32_t>(native_sample), *mix_r);
#else
    *mix_l += q16_scale(static_cast<int32_t>(native_sample), gain_l_q24);
    *mix_r += q16_scale(static_cast<int32_t>(native_sample), gain_r_q24);
#endif
}

// Гладкая кривая гашения: s(x) = x*x*(3 - 2x), x = позиция/длина.
// Производная на обоих концах нулевая, изломов нет. Прямая даёт
// треугольник с изломами на концах, а излом - широкополосный всплеск,
// слышимый как тихий щелчок.
int32_t tail_curve_q15(uint32_t pos, uint32_t len) {
    if (pos >= len) return 32768;
    const int32_t x = static_cast<int32_t>((pos * 32768u) / len);
    const int32_t xx = (x * x) >> 15;
    return (xx * (98304 - 2 * x)) >> 15; // 98304 == 3 << 15
}

// Усиление хвоста на кривую гашения Q15.
int32_t scale_by_curve_q15(int32_t gain, int32_t curve_q15) {
    return static_cast<int32_t>((static_cast<int64_t>(gain) * curve_q15) >> 15);
}

} // namespace

void VoiceMixer::start_tail(VoiceRamp& r, int32_t from_l, int32_t from_r) const {
    if (ramp_samples == 0) return; // сглаживание выключено
    r.tail_remaining = static_cast<uint8_t>(ramp_samples);
    r.fading_sample = r.last_output; // замораживаем: дальше голос волен писать в last_output своё
    r.tail_start_gain_l = from_l;
    r.tail_start_gain_r = from_r;
}

void VoiceMixer::fade_out(VoiceRamp& r) const {
    if (r.last_output != 0 && (r.gain_l != 0 || r.gain_r != 0)) start_tail(r, r.gain_l, r.gain_r);
    r.gain_l = 0;
    r.gain_r = 0;
}

// Антиклик на снятии голоса. Голос гаснет из десятка мест - NoteCut,
// KeyOff, DCA=Cut, конец сэмпла, отбор голоса под новую ноту, конец хвоста
// NNA. Поэтому ловится сам переход: был в списке прошлого тика, перестал
// звучать - последнее значение гасится за ramp отсчётов тем же механизмом,
// что и перезапуск ноты. Без этого волна обрывается на ненулевом значении.
SOUNDSINTH_HOT_PATH_ATTR("vm_fade_stopped")
void VoiceMixer::fade_stopped(bool wave_tail) {
    for (uint8_t k = 0; k < active_count; ++k) {
        const uint8_t idx = active[k];
        if (voices[idx].active) continue; // ещё звучит
        VoiceRamp& r = ramp[idx];
        if (wave_tail && r.last_output != 0 && (r.gain_l != 0 || r.gain_r != 0) && r.wave_remaining == 0 &&
            r.tail_remaining == 0) {
            r.wave_remaining = static_cast<uint8_t>(ramp_samples);
            r.tail_start_gain_l = r.gain_l;
            r.tail_start_gain_r = r.gain_r;
            r.gain_l = 0;
            r.gain_r = 0;
            ++wave_tail_started;
        } else {
            fade_out(r);
        }
    }
}

// Считается раз в тик, в горячем цикле остаётся сложение.
SOUNDSINTH_HOT_PATH_ATTR("vm_ramp_to_gains")
void VoiceMixer::ramp_to_gains(uint8_t slot) {
    VoiceRamp& r = ramp[slot];
    if (r.restart) {
        // Перезапущенный голос стартует с тишины, иначе первый отсчёт новой ноты
        // выходит на громкости прежней.
        // Прежняя нота на этом голосе обрывается здесь, на произвольном месте
        // волны. Её последнее значение гасится параллельно разгону новой: ступенька
        // сидит в самом разрыве, и сглаживание громкости новой ноты её не уберёт.
        if (r.last_output != 0 && (r.gain_l != 0 || r.gain_r != 0)) {
            start_tail(r, r.gain_l, r.gain_r);
        }
        // Волновой хвост продолжал старый сэмпл, а голос занят новым - продолжать
        // нечего. Стык закрывает разгон новой ноты.
        if (r.wave_remaining != 0) ++wave_tail_cut_note;
        r.wave_remaining = 0;
        r.gain_l = 0;
        r.gain_r = 0;
        r.restart = false;
    }
    const int32_t target_l = gain_l_q24[slot];
    const int32_t target_r = gain_r_q24[slot];
    if (ramp_samples == 0 || (r.gain_l == target_l && r.gain_r == target_r)) {
        r.ramp_remaining = 0; // ничего не изменилось - сглаживать нечего
    } else {
        const int32_t n = static_cast<int32_t>(ramp_samples);
        r.step_l = (target_l - r.gain_l) / n;
        r.step_r = (target_r - r.gain_r) / n;
        r.ramp_remaining = static_cast<uint8_t>(n);
    }
}

// Нота .mid без сэмпла: voice_trigger обнулит голос, волновой хвост
// продолжать нечем, а на перезапуске усиления уже нулевые. Редкая ветка,
// отдельной функцией: встроенная, размножала подготовку вызова voice_trigger.
SOUNDSINTH_HOT_PATH_ATTR("vm_fade_before_missing")
void VoiceMixer::fade_before_missing(uint8_t slot) {
    VoiceRamp& r = ramp[slot];
    if (!voices[slot].active && r.wave_remaining != 0) {
        r.fading_sample = r.last_output;
        r.tail_remaining = r.wave_remaining;
        r.wave_remaining = 0;
    } else if (r.last_output != 0 && (r.gain_l != 0 || r.gain_r != 0)) {
        start_tail(r, r.gain_l, r.gain_r);
        r.gain_l = 0;
        r.gain_r = 0;
    }
}

void VoiceMixer::move(uint8_t from, uint8_t to) {
    // Слот ещё звучит: хвост этого канала, только что снятый правилом "один
    // хвост на канал", снятый DCA или украденный. Его последнее значение
    // гасится, как у ловца перехода, - иначе волна обрывается на
    // произвольном отсчёте. Гашение идёт параллельно новому голосу слота.
    // Волновое гашение .mid так не гасится: его last_output - отсчёт до смерти
    // голоса, устаревший.
    {
        VoiceRamp& r = ramp[to];
        if (r.last_output != 0 && (r.gain_l != 0 || r.gain_r != 0) && r.wave_remaining == 0) {
            start_tail(r, r.gain_l, r.gain_r);
            r.gain_l = 0;
            r.gain_r = 0;
        }
    }
    voices[to] = voices[from];
    // Память фильтра переезжает с голосом: иначе он продолжает волну с памятью
    // прежнего хозяина слота - щелчок на каждом уводе в фон. Коэффициенты -
    // тоже: фильтр с открытым срезом держит прежние, и у слота они были бы
    // чужими.
    filter_state[to] = filter_state[from];
    filter_coeffs[to] = filter_coeffs[from];
    // Сглаживание громкости переезжает с голосом, последнее значение на канале
    // не гасится - голос не оборван, а звучит в слоте. Иначе на каждом уводе в
    // фон слышен удар (гашение плюс разгон с нуля).
    VoiceRamp& src = ramp[from];
    VoiceRamp& dst = ramp[to];
    dst.gain_l = src.gain_l;
    dst.gain_r = src.gain_r;
    dst.step_l = src.step_l;
    dst.step_r = src.step_r;
    dst.ramp_remaining = src.ramp_remaining;
    dst.last_output = src.last_output;
    if (dst.wave_remaining != 0) ++wave_tail_cut_move;
    dst.wave_remaining = 0;
    dst.restart = false;
    src.last_output = 0;
}

// Гаснущий слот обязан попасть в сведение, иначе затухание оборвётся на
// границе батча тем самым щелчком. Голоса в нём нет: тик снял его и в свой
// список не положил, а гашение живёт дальше - хвост NNA, сброшенный по
// перегрузке голос, конец незацикленного сэмпла.
SOUNDSINTH_HOT_PATH_ATTR("vm_collect_tails")
void VoiceMixer::collect_tails() {
    uint32_t listed[(SOUNDSINTH_MAX_SLOTS + 31u) / 32u] = {};
    for (uint8_t k = 0; k < active_count; ++k) {
        listed[active[k] / 32u] |= 1u << (active[k] % 32u);
    }
    tail_count = 0;
    for (uint16_t s = 0; s < SOUNDSINTH_MAX_SLOTS; ++s) {
        if (!ramp[s].fading()) continue;
        if ((listed[s / 32u] & (1u << (s % 32u))) != 0) continue; // сведётся вместе с голосом
        tails[tail_count++] = static_cast<uint8_t>(s);
    }
}

// Горячий путь каждого батча.
SOUNDSINTH_HOT_PATH_ATTR("vm_mix")
void VoiceMixer::mix(memory::PsramStore& psram, int32_t* mix_l, int32_t* mix_r, uint32_t i, uint32_t batch,
                     uint32_t reverb_frames) {
    // Голос снаружи, отсчёт внутри: чтения одного голоса из PSRAM идут подряд,
    // у кэша XIP меньше конфликтных промахов. Сумма та же побитово.
    constexpr uint32_t kVoiceRenderChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES; // обычный батч и так не длиннее
    int16_t voice_scratch[kVoiceRenderChunk];
    collect_tails();
    const uint16_t total = static_cast<uint16_t>(active_count) + tail_count;
    for (uint16_t k = 0; k < total; ++k) {
        const uint8_t idx = (k < active_count) ? active[k] : tails[k - active_count];
        Voice& v = voices[idx];
        // Мёртвый голос пропускаем, но не пока он догашивается.
        if (!v.active && ramp[idx].tail_remaining == 0 && ramp[idx].wave_remaining == 0 &&
            ramp[idx].last_output == 0) {
            continue;
        }

        // Маршрут и посыл поставил тик: solo чужого канала - в discard.
        int32_t* out_l = discard[idx] ? discard_l : mix_l;
        int32_t* out_r = discard[idx] ? discard_r : mix_r;

        VoiceRamp& r = ramp[idx];
        // Подмешать хвост с позиции from внутри батча и не дальше конца: голос
        // может кончиться посреди батча, и гасить надо с того же места.
        auto mix_tail = [&](uint32_t from) {
            if (r.tail_remaining == 0 || from >= batch) return;
            const uint32_t room = batch - from;
            const uint32_t n = (r.tail_remaining < room) ? r.tail_remaining : room;
            for (uint32_t j = 0; j < n; ++j) {
                const int32_t c = tail_curve_q15(r.tail_remaining - j - 1, ramp_samples);
                mix_native_sample(r.fading_sample, scale_by_curve_q15(r.tail_start_gain_l, c),
                                  scale_by_curve_q15(r.tail_start_gain_r, c), &out_l[i + from + j],
                                  &out_r[i + from + j]);
            }
            r.tail_remaining = static_cast<uint8_t>(r.tail_remaining - n);
            if (r.tail_remaining == 0) {
                r.fading_sample = 0;
                r.last_output = 0;
            } // отработало, второй раз не начинать
        };

        // Волновой хвост: голос умер, но данные остались. Догашивается его
        // собственное продолжение, а не замороженный отсчёт - в миксе ни разрыва,
        // ни постоянной составляющей. Голос на время рендера возвращается в живые,
        // иначе voice_render не отдаст ни отсчёта.
        {
            VoiceRamp& wr = r;
            // Именно !v.active: голос, взятый новой нотой, иначе попадал бы сюда с
            // недоигранным хвостом прошлой, отдавал отсчёты гаснущей кривой и тут же
            // гасился - нота пропадала целиком.
            if (!v.active && wr.wave_remaining != 0) {
                const uint32_t want = wr.wave_remaining < batch ? wr.wave_remaining : batch;
                v.active = true;
                const uint32_t got = voice_render(v, psram, voice_scratch, want);
                v.active = false;
                for (uint32_t j = 0; j < got; ++j) {
                    const int32_t c = tail_curve_q15(wr.wave_remaining - j - 1, ramp_samples);
                    mix_native_sample(voice_scratch[j], scale_by_curve_q15(wr.tail_start_gain_l, c),
                                      scale_by_curve_q15(wr.tail_start_gain_r, c), &out_l[i + j], &out_r[i + j]);
                }
                if (got > 0) wr.last_output = voice_scratch[got - 1];
                wr.wave_remaining = static_cast<uint8_t>(wr.wave_remaining - got);
                if (got < want && wr.wave_remaining != 0) {
                    // Данные кончились посреди гашения: последнее значение волны
                    // гаснет той же кривой с того же места, в этом же батче.
                    wr.fading_sample = wr.last_output;
                    wr.tail_remaining = wr.wave_remaining;
                    wr.wave_remaining = 0;
                    mix_tail(got);
                } else if (wr.wave_remaining == 0) {
                    wr.fading_sample = 0;
                    wr.last_output = 0;
                }
                continue; // мёртвый голос
            }
        }

        // Усиления готовы; здесь только умножение на отсчёт.
        const int32_t gain_l = gain_l_q24[idx];
        const int32_t gain_r = gain_r_q24[idx];

        // Сумма громкостей канала: посыл идёт от того, что слышно, а не от сырого
        // сэмпла - тихий голос и в реверберацию отдаёт тихо. Усиления в масштабе
        // шины (Q24.8), шина реверберации - в целых единицах int16: дробь
        // снимается делителем, иначе умножение на посыл переполнило бы int32.
        const uint8_t rev_send = reverb_send[idx];
        const int32_t send_q16 =
            rev_send ? (((gain_l + gain_r) / (2 << (kGainQ24Bits - kQ16Bits))) * static_cast<int32_t>(rev_send)) / 127
                     : 0;

        // Затухание подмешивается параллельно, с начала батча, а не после голоса.
        // Иначе оно работало бы только для мёртвых голосов, а самый частый случай -
        // перезапуск ноты на живом канале: старая гаснет, новая разгорается, и они
        // накладываются.
        if (r.tail_remaining != 0) {
            uint32_t n = (r.tail_remaining < batch) ? r.tail_remaining : batch;
            for (uint32_t j = 0; j < n; ++j) {
                const int32_t c = tail_curve_q15(r.tail_remaining - j - 1, ramp_samples);
                mix_native_sample(r.fading_sample, scale_by_curve_q15(r.tail_start_gain_l, c),
                                  scale_by_curve_q15(r.tail_start_gain_r, c), &out_l[i + j], &out_r[i + j]);
            }
            r.tail_remaining = static_cast<uint8_t>(r.tail_remaining - n);
            if (r.tail_remaining == 0) {
                r.fading_sample = 0;
                r.last_output = 0;
            } // отработало, второй раз не начинать
        }

        uint32_t done = 0;
        while (done < batch && v.active) {
            uint32_t chunk = batch - done;
            if (chunk > kVoiceRenderChunk) chunk = kVoiceRenderChunk;
            const uint32_t produced = voice_render(v, psram, voice_scratch, chunk);

            // Фильтр - над готовым буфером голоса, до громкости и панорамы: в IT он
            // часть тракта сэмпла, а не мастер-шины (у OpenMPT тот же порядок:
            // интерполяция, фильтр, громкость, сведение). Ветка на голос, а не
            // проверка на отсчёт: голосов с фильтром единицы.
            if (filter_coeffs[idx].active && produced > 0) {
                filter_apply(voice_scratch, produced, filter_coeffs[idx], filter_state[idx]);
            }

            uint32_t j = 0;
            // Сглаживаемый кусок: два сложения и два ограничения на отсчёт. Он короткий
            // (ramp_samples отсчётов) и бывает, только когда громкость изменилась.
            if (r.ramp_remaining != 0) {
                uint32_t n = (r.ramp_remaining < produced - j) ? r.ramp_remaining : (produced - j);
                int32_t gl = r.gain_l;
                int32_t gr = r.gain_r;
                for (uint32_t e = j + n; j < e; ++j) {
                    mix_native_sample(voice_scratch[j], clamp_ramp_gain(gl, gain_l), clamp_ramp_gain(gr, gain_r),
                                      &out_l[i + done + j], &out_r[i + done + j]);
                    gl += r.step_l;
                    gr += r.step_r;
                }
                r.ramp_remaining = static_cast<uint8_t>(r.ramp_remaining - n);
                // Доехали - садимся точно на цель: деление на ramp с остатком иначе
                // оставило бы голос чуть тише или громче навсегда.
                r.gain_l = (r.ramp_remaining == 0) ? gain_l : gl;
                r.gain_r = (r.ramp_remaining == 0) ? gain_r : gr;
            }
            // Остальное - постоянная громкость.
            for (; j < produced; ++j) {
                mix_native_sample(voice_scratch[j], gain_l, gain_r, &out_l[i + done + j], &out_r[i + done + j]);
            }

            // Только голоса с посылом; не дальше reverb_frames - иначе запись за буфер.
            if (send_q16 != 0 && i + done < reverb_frames) {
                const uint32_t base = i + done;
                uint32_t take = reverb_frames - base;
                if (take > produced) take = produced;
                int32_t* bus = reverb_bus + base;
                for (uint32_t e = 0; e < take; ++e) {
                    bus[e] += (static_cast<int32_t>(voice_scratch[e]) * send_q16) >> kQ16Bits;
                }
            }

            if (produced > 0) r.last_output = voice_scratch[produced - 1];
            done += produced;
            if (produced < chunk) break; // незацикленный сэмпл кончился посреди куска
        }

        // Голос только что кончился - его последнее значение гасится до тишины:
        // незацикленный сэмпл почти никогда не кончается нулём, а обрыв на
        // ненулевом отсчёте слышен.
        if (!v.active && r.tail_remaining == 0 && r.last_output != 0) {
            start_tail(r, r.gain_l, r.gain_r);
            // Обнулить, как после каждого start_tail: иначе пересборка списка на
            // границе тика снова запустит кривую с полной громкости (скачок до +3941 у
            // голоса уровня 5000).
            r.gain_l = 0;
            r.gain_r = 0;
            // Сразу, с места обрыва: иначе до конца батча тишина, и обрыв слышен.
            mix_tail(done);
        }
    }
}

} // namespace soundsinth::engine
