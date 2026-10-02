// SPDX-License-Identifier: MIT
#include "core/engine/voice_arbiter.h"

#include "platform/hot_path.h"
#include "core/engine/engine_defs.h"
#include "core/engine/envelope_gain.h"

namespace soundsinth::engine {

namespace {

// Ярусы жертвы сброса по возрастанию ценности для слушателя. Ярус решает
// всё: голос хвоста уходит раньше любого живого, каким бы дорогим тот ни
// был, иначе резалась бы мелодия при гаснущих фоновых голосах.
enum class CullTier : int32_t {
    NnaTail,   // фоновый голос NNA: нота отпущена, пропажа наименее заметна
    QuietLive, // тихий живой голос: подкладка, эхо
    MainLive,  // основной живой голос: мелодия, бас - только когда больше нечего
    None,      // жертвы нет
};

} // namespace

uint8_t VoiceArbiter::pick_slot(int32_t killed) const {
    // У .mid слоты, где ещё гаснет снятый голос (в том числе только что снятый
    // фоновый голос этого канала), берутся последними: перезапись оборвала бы
    // волновое гашение. Среди занятых - самый тихий (при равенстве - меньший
    // alloc_seq): volume * огибающая * громкость канала * затухание, при
    // tremor_muted 0.
    int32_t target    = -1;
    int32_t busy_tail = -1;
    for (uint8_t i = 0; i < kPool; ++i) {
        if (slots_[i].active) continue;
        if (wave_tail_ && (i == killed || link_->fading(static_cast<uint8_t>(channel_count_ + i)))) {
            if (busy_tail < 0) busy_tail = i;
            continue;
        }
        target = i;
        break;
    }
    if (target < 0) target = busy_tail;
    if (target < 0) {
        float best_loudness = 0.0f;
        uint32_t best_seq   = 0;
        for (uint8_t i = 0; i < kPool; ++i) {
            const ChannelState& cand = channels_[channel_count_ + i];
            // У .mid огибающая хранит децибелы: как амплитуду её брать нельзя, -48 дБ
            // выглядело бы половиной громкости. Шкала 0..64, как у трекеров.
            const float env = static_cast<float>(envelope_gain_q16(cand, envelope_db_)) * (64.0f / 65536.0f);
            const float loudness =
                cand.tremor_muted ? 0.0f
                                  : static_cast<float>(cand.volume) * env * static_cast<float>(cand.channel_volume) * static_cast<float>(cand.fadeout_level);
            if (target < 0 || loudness < best_loudness || (loudness == best_loudness && slots_[i].alloc_seq < best_seq)) {
                target        = i;
                best_loudness = loudness;
                best_seq      = slots_[i].alloc_seq;
            }
        }
    }
    return static_cast<uint8_t>(target);
}

uint8_t VoiceArbiter::to_background(uint8_t channel) {
    // Не больше одного хвоста NNA на канал (итого не больше двух голосов на
    // канал). Без этого канал с частыми ретриггерами при nna != Cut копил бы
    // хвосты до исчерпания всего пула. Ограничение ресурсное, не из IT: старый
    // хвост музыкально менее важен нового, глушится без разбора громкости.
    int32_t killed = -1;
    for (uint8_t i = 0; i < kPool; ++i) {
        if (slots_[i].active && slots_[i].origin_channel == channel) {
            stop_slot(static_cast<uint8_t>(channel_count_ + i));
            killed = i;
            break; // больше одного не бывает - это и поддерживается проверкой
        }
    }

    const uint8_t slot = pick_slot(killed);
    if (slots_[slot].active) ++steals_; // пул полон - слот отобран у звучащего
    const uint8_t bg = static_cast<uint8_t>(channel_count_ + slot);
    channels_[bg]    = channels_[channel];
    link_->move(channel, bg);
    slots_[slot] = SlotInfo{true, channel, ++alloc_seq_counter_};
    return bg;
}

// Не успевает рендер - DMA повторяет прошлый буфер, заикается вся музыка.
// Спуск - по величине перегруза: один голос на SOUNDSINTH_VOICE_CULL_STEP_PCT
// загрузки сверх порога, столько он примерно и стоит. Подъём остаётся
// медленным: мерцание полифонии на слух хуже ровно уменьшенной.
SOUNDSINTH_HOT_PATH_ATTR("va_update_budget")
void VoiceArbiter::update_budget(uint32_t load_q8) {
    if (!cull_enabled_) return;
    constexpr uint32_t kHighQ8 = pct_to_q8(SOUNDSINTH_VOICE_CULL_HIGH_PCT);
    constexpr uint32_t kLowQ8  = pct_to_q8(SOUNDSINTH_VOICE_CULL_LOW_PCT);
    constexpr uint8_t kMin     = static_cast<uint8_t>(SOUNDSINTH_VOICE_CULL_MIN_VOICES);
    constexpr uint32_t kStepQ8 = pct_to_q8(SOUNDSINTH_VOICE_CULL_STEP_PCT);

    if (load_q8 > kHighQ8) {
        budget_            = voice_budget_after_overload(budget_, load_q8, kHighQ8, kMin, kStepQ8, SOUNDSINTH_VOICE_CULL_MAX_STEP);
        budget_rise_ticks_ = 0; // перегрузка обнуляет накопленное разрешение подняться
    } else if (load_q8 < kLowQ8) {
        if (++budget_rise_ticks_ >= SOUNDSINTH_VOICE_CULL_RISE_TICKS) {
            budget_rise_ticks_ = 0;
            if (budget_ < SOUNDSINTH_MAX_VOICES) ++budget_;
        }
    }
}

// Громкость - |mix_gain_l| + |mix_gain_r| (модуль из-за surround), в ней все
// множители и панорама. Погашенный голос остаётся в списке и гаснет за длину
// сглаживания, волнового гашения нет.
SOUNDSINTH_HOT_PATH_ATTR("va_cull_over_budget")
void VoiceArbiter::cull_over_budget() {
    if (!cull_enabled_) return;

    // Слоты с волновым гашением в списке тика не лежат, но читают сэмпл и
    // стоят как живые голоса, поэтому входят в потолок наравне с ними.
    // Замороженные хвосты не в счёт - против бюджета они ничего не весят.
    // Считается раз: за время сброса хвосты не кончаются.
    const uint8_t fading = link_->fading_count();

    while (static_cast<uint16_t>(link_->list_size()) + fading > budget_) {
        // Порог "тихого" живого голоса - доля самого громкого живого. Считается на
        // каждом шаге заново: убрали голос - картина поменялась.
        int64_t loudest_live = 0;
        for (uint8_t k = 0; k < link_->list_size(); ++k) {
            const uint8_t idx = link_->list_at(k);
            if (!link_->playing(idx) || idx >= channel_count_) continue;
            const int64_t l = link_->loudness(idx);
            if (l > loudest_live) loudest_live = l;
        }
        const int64_t quiet_limit = (loudest_live * SOUNDSINTH_VOICE_CULL_QUIET_NUM) / SOUNDSINTH_VOICE_CULL_QUIET_DEN;

        int32_t victim_k        = -1;
        CullTier victim_tier    = CullTier::None;
        uint32_t victim_cost    = 0;
        int64_t victim_loudness = 0;

        for (uint8_t k = 0; k < link_->list_size(); ++k) {
            const uint8_t idx = link_->list_at(k);
            if (!link_->playing(idx)) continue; // уже гаснет

            const int64_t loudness = link_->loudness(idx);
            const CullTier tier    = (idx >= channel_count_) ? CullTier::NnaTail : (loudness <= quiet_limit) ? CullTier::QuietLive : CullTier::MainLive;
            const uint32_t cost    = link_->cost_ns(idx);

            bool better = false;
            if (victim_k < 0 || tier < victim_tier) {
                better = true;
            } else if (tier == victim_tier) {
                // Внутри яруса - худшее отношение цены к громкости: cost/loudness больше -
                // голос дороже или тише. Перекрёстное умножение вместо деления, +1 у
                // громкости страхует от нуля.
                better = static_cast<int64_t>(cost) * (victim_loudness + 1) > static_cast<int64_t>(victim_cost) * (loudness + 1);
            }
            if (better) {
                victim_k        = k;
                victim_tier     = tier;
                victim_cost     = cost;
                victim_loudness = loudness;
            }
        }
        if (victim_k < 0) break; // все оставшиеся уже гаснут

        const uint8_t idx = link_->list_at(static_cast<uint8_t>(victim_k));
        link_->fade(idx);
        stop_slot(idx);
        link_->list_remove_at(static_cast<uint8_t>(victim_k));
        ++culled_;
    }
}

} // namespace soundsinth::engine
