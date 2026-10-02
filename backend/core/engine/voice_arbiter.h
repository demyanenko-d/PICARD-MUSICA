// SPDX-License-Identifier: MIT
#pragma once

// Арбитр голосов: кому из голосов звучать, когда их больше, чем можно. Пул
// фоновых голосов NNA (слоты, выбор, кража), сброс хвостов сверх потолка
// SOUNDSINTH_MAX_VOICES, потолок полифонии по загрузке и сброс лишних.
// Музыкальных правил не знает: DCT/DCA и действие NNA решает управляющая
// часть, здесь только слоты и отбор. Работает раз в тик.

#include <cstdint>

#include "core/config.h"
#include "core/engine/channel_state.h"
#include "core/engine/voice_link.h"

namespace soundsinth::engine {

class VoiceArbiter {
public:
    // Пул NNA: слот i - channels[channel_count + i] и голос микшера с тем же
    // индексом. Каналов не больше SOUNDSINTH_MAX_VOICES, поэтому за ними
    // всегда есть kPool слотов.
    static constexpr uint8_t kPool = SOUNDSINTH_MAX_NNA_VOICES;
    static_assert(SOUNDSINTH_MAX_SLOTS >= SOUNDSINTH_MAX_VOICES + SOUNDSINTH_MAX_NNA_VOICES, "the full NNA pool comes after the channels");
    static_assert(SOUNDSINTH_MAX_SLOTS <= UINT8_MAX, "the slot index is a byte");
    static_assert(SOUNDSINTH_MAX_NNA_VOICES >= 1, "the NNA pool is not empty: slot selection always finds room");

    // channels и link - общие с управляющей частью, живут не меньше арбитра.
    // wave_tail - волновое гашение (.mid), envelope_db - огибающая в децибелах.
    void init(ChannelState* channels, VoiceLink* link, uint8_t channel_count, bool wave_tail, bool envelope_db) {
        channels_      = channels;
        link_          = link;
        channel_count_ = channel_count;
        wave_tail_     = wave_tail;
        envelope_db_   = envelope_db;
    }

    bool slot_active(uint8_t slot) const { return slots_[slot].active; }
    // Канал, с которого голос слота ушёл в фон.
    uint8_t origin_channel(uint8_t slot) const { return slots_[slot].origin_channel; }
    bool is_tail_of(uint8_t slot, uint8_t channel) const { return slots_[slot].active && slots_[slot].origin_channel == channel; }

    // Голос слота idx (канал или пул) снят: флаги канала, голоса и пула.
    // Гашение - у вызывающего или на пересборке списка.
    void stop_slot(uint8_t idx) {
        channels_[idx].voice_active = false;
        link_->stop(idx);
        if (idx >= channel_count_) slots_[idx - channel_count_].active = false;
    }

    // Живой голос канала - в фон: прежний хвост этого канала снимается (не
    // больше одного на канал), слот выбирается или отбирается, состояние
    // канала и голос переезжают в него. Возвращает индекс слота.
    uint8_t to_background(uint8_t channel);

    // Хвосты NNA - в список голосов на остаток потолка
    // SOUNDSINTH_MAX_VOICES. Лишние гасятся, самые тихие первыми;
    // set_gains(idx) ставит голосу усиления этого тика для сравнения.
    template <typename SetGains>
    void list_tails(SetGains&& set_gains);

    // Потолок полифонии по загрузке load_q8, раз в тик: спуск на голос за тик,
    // подъём на голос за SOUNDSINTH_VOICE_CULL_RISE_TICKS тиков низкой нагрузки.
    void update_budget(uint32_t load_q8);

    // Гасит лишние голоса списка, пока их больше бюджета. Усиления голосов
    // этого тика уже стоят. Жертва - по ярусам (фоновый NNA, тихий живой,
    // основной), внутри яруса - по отношению цены голоса к его слышимости.
    void cull_over_budget();

    // Сброс по перегрузке. Выключен по умолчанию.
    void set_cull_enabled(bool on) { cull_enabled_ = on; }

    uint8_t budget() const { return budget_; }
    uint32_t culled() const { return culled_; }
    uint32_t tails_dropped() const { return tails_dropped_; }
    uint32_t steals() const { return steals_; }

private:
    // Учёт одного слота пула. Поля для DCT (инструмент, сэмпл, нота старого
    // голоса) не дублируются: они в скопированном ChannelState слота.
    struct SlotInfo {
        bool active            = false;
        uint8_t origin_channel = 0; // канал-источник: DCT/DCA ищут только среди его фоновых голосов
        // Номер выделения; при краже слота из равных по громкости уходит самый старый.
        uint32_t alloc_seq = 0;
    };

    // Слот пула под новый фоновый голос: свободный, у .mid - гаснущий
    // последним, иначе самый тихий. killed - слот, снятый только что.
    uint8_t pick_slot(int32_t killed) const;

    ChannelState* channels_ = nullptr;
    VoiceLink* link_        = nullptr;
    uint8_t channel_count_  = 0;
    bool wave_tail_         = false;
    bool envelope_db_       = false;

    SlotInfo slots_[kPool];
    uint32_t alloc_seq_counter_ = 0;
    uint32_t tails_dropped_     = 0;
    uint32_t steals_            = 0;

    // Сколько голосов разрешено держать активными. Опускается под перегрузкой
    // и поднимается, когда отпустило. При выключенном сбросе равен
    // SOUNDSINTH_MAX_VOICES и ни на что не влияет.
    uint8_t budget_    = SOUNDSINTH_MAX_VOICES;
    uint32_t culled_   = 0;
    bool cull_enabled_ = false;
    // Тики низкой нагрузки с последней перегрузки или подъёма.
    uint8_t budget_rise_ticks_ = 0;
};

template <typename SetGains>
void VoiceArbiter::list_tails(SetGains&& set_gains) {
    // Замирать лишним нельзя - волна оборвалась бы без гашения и потом
    // продолжилась бы с того же места вторым разрывом. Отбор тихого фонового с
    // анти-кликом, как у libxmp. Громкость - усиления этого тика: у хвоста, не
    // бывшего в списке, прежние недействительны.
    uint8_t tails[kPool];
    uint8_t tail_count = 0;
    for (uint8_t slot = 0; slot < kPool; ++slot) {
        const uint8_t bg = static_cast<uint8_t>(channel_count_ + slot);
        if (slots_[slot].active && link_->playing(bg)) {
            tails[tail_count++] = bg; // двойная проверка: слот и голос
        }
    }
    const uint8_t room = static_cast<uint8_t>(SOUNDSINTH_MAX_VOICES - link_->list_size());
    if (tail_count > room) {
        for (uint8_t t = 0; t < tail_count; ++t) {
            set_gains(tails[t]);
        }
        while (tail_count > room) {
            uint8_t q = 0;
            for (uint8_t t = 1; t < tail_count; ++t) {
                if (link_->loudness(tails[t]) < link_->loudness(tails[q])) q = t;
            }
            // Гашение - те же действия, что у сброса по перегрузке.
            const uint8_t idx = tails[q];
            link_->fade(idx);
            stop_slot(idx);
            tails[q] = tails[--tail_count];
            ++tails_dropped_;
        }
    }
    for (uint8_t t = 0; t < tail_count; ++t) {
        link_->list_push(tails[t]);
    }
}

} // namespace soundsinth::engine
