// SPDX-License-Identifier: MIT
#include "core/engine/voice_link.h"

#include "platform/hot_path.h"

namespace soundsinth::engine {

// Команды, которые случаются на ноте и на тике, а не на каждый голос: тела
// во флеше, чтобы не раздувать горячие функции тика в SRAM.
void VoiceLink::trigger(uint8_t slot, const soundsinth::model::SampleDescriptor& sample, uint16_t first_page, uint8_t note,
                        soundsinth::model::FrequencyModel frequency_model, uint32_t start_offset, soundsinth::model::QuirkFlags quirks,
                        uint16_t checkpoint_first_page) {
    (void)note;
    (void)frequency_model;
    // Начало ноты разбирает тик: вниз уходит отсчёт, годность петли и
    // интерполяция, а не квирки формата. Нота может и не зазвучать.
    const TriggerStart start = voice_trigger_start(sample, first_page, start_offset, quirks);
    // Голос собирается заново: шаг ему поставит высота этого же тика, кэш
    // входов питча сбрасывается вместе с ним.
    restart_[slot] = true;
    // Модель звучания: начальное состояние ровно как у voice_trigger_prepared -
    // петля и её границы, позиция после подготовки интерполяции (один отсчёт,
    // второй - если сэмпл не кончился), дробная часть с нуля.
    sounding_[slot] = start.sounds;
    if (start.sounds) {
        sounding_loop_[slot]       = start.loop_ok;
        sounding_loop_start_[slot] = sample.loop_start;
        sounding_loop_end_[slot]   = start.loop_ok ? sample.loop_end : sample.length_samples;
        uint32_t decoded           = start.offset + 1u;
        if (start.loop_ok && decoded >= sounding_loop_end_[slot]) decoded = sample.loop_start;
        if (decoded < sample.length_samples) ++decoded;
        sounding_decoded_[slot] = decoded;
        sounding_frac_[slot]    = 0;
    }
    step_[slot]          = 0;
    enc_[slot]           = sample.resident_encoding;
    pitch_memo_[slot]    = 0;
    pitch_memo_c5_[slot] = 0;

    VoiceCommand cmd{VoiceOp::Trigger, slot, 0, 0, {}};
    if (start.sounds) cmd.flags |= kTriggerSounds;
    if (start.loop_ok) cmd.flags |= kTriggerLoop;
    if (voice_hermite(sample, quirks)) cmd.flags |= kTriggerHermite;
    cmd.u.trig.sample                = &sample;
    cmd.u.trig.offset                = start.offset;
    cmd.u.trig.first_page            = first_page;
    cmd.u.trig.checkpoint_first_page = checkpoint_first_page;
    push(cmd);
}

void VoiceLink::move(uint8_t from, uint8_t to) {
    // Слот-приёмник гасит своё последнее значение; гашение идёт параллельно
    // приехавшему голосу, дальше состояние и сглаживание переезжают целиком.
    model_tail(to);
    filter_on_[to] = filter_on_[from]; // фильтр переезжает вместе с голосом
    restart_[to]   = false;            // переехавший голос уже разогнан, как и у микшера
    // Голос переезжает целиком - вместе с моделью его звучания.
    sounding_[to]            = sounding_[from];
    sounding_frac_[to]       = sounding_frac_[from];
    sounding_decoded_[to]    = sounding_decoded_[from];
    sounding_loop_[to]       = sounding_loop_[from];
    sounding_loop_start_[to] = sounding_loop_start_[from];
    sounding_loop_end_[to]   = sounding_loop_end_[from];
    // Исходный слот не гасится: микшер копирует голос, а не переносит, и
    // voices[from] остаётся звучащим до следующего запуска на этом слоте.
    step_[to]          = step_[from];
    enc_[to]           = enc_[from];
    pitch_memo_[to]    = pitch_memo_[from];
    pitch_memo_c5_[to] = pitch_memo_c5_[from];
    VoiceCommand cmd{VoiceOp::Move, from, to, 0, {}};
    push(cmd);
}

void VoiceLink::fade_before_missing(uint8_t slot) {
    model_tail(slot);
    send(VoiceOp::FadeBeforeMissing, slot);
}

void VoiceLink::fade_stopped(bool wave_tail) {
    for (uint8_t k = 0; k < list_count_; ++k) {
        const uint8_t idx = list_[k];
        if (sounding_[idx]) continue;
        // Волновое гашение начинается только здесь и только у снятого голоса,
        // на котором не идёт другое гашение: у доигравшего сэмпла продолжать
        // нечего, и его последнее значение уже заморожено.
        if (wave_tail && tail_frames_[idx] == 0) {
            model_wave_tail(idx);
        } else {
            model_tail(idx);
        }
    }
    VoiceCommand cmd{VoiceOp::FadeStopped, 0, 0, static_cast<uint8_t>(wave_tail ? 1 : 0), {}};
    push(cmd);
}

void VoiceLink::set_filter(uint8_t slot, const FilterCoeffs& c) {
    filter_on_[slot] = c.active;
    VoiceCommand cmd{VoiceOp::SetFilter, slot, static_cast<uint8_t>(c.active ? 1 : 0), static_cast<uint8_t>(c.fir121 ? 1 : 0), {}};
    cmd.u.filter.a0 = c.a0;
    cmd.u.filter.b0 = c.b0;
    cmd.u.filter.b1 = c.b1;
    push(cmd);
}

// Звуковая сторона кольца: команды тика - в состояние голосов. Порядок -
// строго как их положили: гашение снятых читает список прошлого тика, а
// список этого тика приходит после него.
SOUNDSINTH_HOT_PATH_ATTR("vl_flush")
void VoiceLink::flush() {
    VoiceMixer& mixer         = *mixer_;
    memory::PsramStore& psram = *psram_;
    queue_.drain([&](const VoiceCommand& cmd) {
        switch (cmd.op) {
            case VoiceOp::Stop:
                mixer.stop(cmd.slot);
                break;
            case VoiceOp::Trigger: {
                TriggerStart start;
                start.sounds  = (cmd.flags & kTriggerSounds) != 0;
                start.loop_ok = (cmd.flags & kTriggerLoop) != 0;
                start.offset  = cmd.u.trig.offset;
                mixer.trigger(cmd.slot, psram, *cmd.u.trig.sample, cmd.u.trig.first_page, cmd.u.trig.checkpoint_first_page, start,
                              (cmd.flags & kTriggerHermite) != 0);
                break;
            }
            case VoiceOp::SetStep:
                mixer.set_step(cmd.slot, cmd.u.val.z);
                break;
            case VoiceOp::FadeBeforeMissing:
                mixer.fade_before_missing(cmd.slot);
                break;
            case VoiceOp::Move:
                mixer.move(cmd.slot, cmd.arg);
                break;
            case VoiceOp::Fade:
                mixer.fade(cmd.slot);
                break;
            case VoiceOp::FadeStopped:
                mixer.fade_stopped(cmd.flags != 0);
                break;
            case VoiceOp::SetGains:
                mixer.set_gains(cmd.slot, cmd.u.val.x, cmd.u.val.y);
                break;
            case VoiceOp::SetGainsRamp:
                mixer.set_gains(cmd.slot, cmd.u.val.x, cmd.u.val.y);
                mixer.ramp_to_gains(cmd.slot);
                break;
            case VoiceOp::FilterOff:
                mixer.filter_off(cmd.slot);
                break;
            case VoiceOp::SetFilter: {
                FilterCoeffs c;
                c.a0     = cmd.u.filter.a0;
                c.b0     = cmd.u.filter.b0;
                c.b1     = cmd.u.filter.b1;
                c.fir121 = cmd.flags != 0;
                c.active = cmd.arg != 0;
                mixer.set_filter(cmd.slot, c);
                break;
            }
            case VoiceOp::SetRoute:
                mixer.set_route(cmd.slot, cmd.flags != 0, cmd.arg);
                break;
            case VoiceOp::ListClear:
                mixer.list_clear();
                break;
            case VoiceOp::ListPush:
                for (uint8_t k = 0; k < cmd.arg; ++k) {
                    mixer.list_push(cmd.u.slots[k]);
                }
                break;
            case VoiceOp::ListRemoveAt:
                mixer.list_remove_at(cmd.arg);
                break;
        }
    });
}

} // namespace soundsinth::engine
