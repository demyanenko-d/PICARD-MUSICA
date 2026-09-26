#include "core/formats/midi_live.h"

#include <cstring>

#include "core/memory/psram_store.h"

namespace soundsinth::formats::midi {

namespace {

// Служебная карта конвертера в зоне паттернов PSRAM: паттернов у живой
// песни нет, карты живут до смены трека. nullptr - зона кончилась.
template <typename T>
T* psram_map(memory::PsramStore& psram, uint32_t count) {
    const uint32_t off = memory::psram_pattern_alloc(psram, count * static_cast<uint32_t>(sizeof(T)));
    if (off == memory::kPatternAllocFailed) return nullptr;
    return reinterpret_cast<T*>(memory::psram_pattern_ptr(psram, off));
}

// Сколько раз на одну постройку вытеснить и повторить.
constexpr uint32_t kMaxEvictions = 8;

} // namespace

const char* LiveMidi::begin(soundsinth::model::Song& song, memory::TrackMemory& mem, const bank::Bank& bank,
                            uint8_t ticks_per_row, uint8_t tempo, RequestFn request, RetireFn retire, void* user) {
    if (!bank.valid()) return "банк инструментов не загружен";
    song_ = &song;
    mem_ = &mem;
    request_ = request;
    retire_ = retire;
    user_ = user;
    records_free_ = kLiveMaxSamples;
    records_leaving_ = 0;
    records_retired_ = 0;
    row_ = 0;
    instruments_failed_ = 0;
    samples_capped_ = 0;
    instruments_evicted_ = 0;
    instruments_built_ = 0;
    records_allocated_ = 0;
    instruments_late_ = 0;
    records_late_ = 0;
    records_retired_recent_ = 0;
    in_prefetch_ = false;
    keymap_visits_ = 0;
    retire_calls_ = 0;
    retire_keymap_visits_ = 0;
    retire_record_visits_ = 0;
    stats_ = LoadStats{};
    std::memset(last_row_, 0, sizeof(last_row_));
    envelope_clock_ = 0;
    keymap_used_ = 0;

    init_song_header(song, Grid{0, ticks_per_row}, tempo);
    song.channel_count = kMaxChannels;
    // Ревербератор живому режиму включается всегда: линии заводятся при сборке
    // движка, а будет ли поток слать CC91, заранее не знает никто. Пока посылы
    // каналов нулевые, шина молчит - слышно то же, что и без него. У файла
    // .mid это решает разбор, он видит весь трек.
    song.reverb_enabled = true;
    song.instruments = memory::arena_new<Instrument>(mem.resident, kLiveMaxInstruments);
    song.samples = memory::arena_new<SampleDescriptor>(mem.resident, kLiveMaxSamples);
    cv_ = memory::arena_new<Converter>(mem.resident);
    envelopes_ = memory::arena_new<Envelope>(mem.resident, kLiveEnvelopes);
    envelope_bank_ = memory::arena_new<uint16_t>(mem.resident, kLiveEnvelopes);
    envelope_refs_ = memory::arena_new<uint8_t>(mem.resident, kLiveEnvelopes);
    envelope_freed_ = memory::arena_new<uint32_t>(mem.resident, kLiveEnvelopes);
    record_bank_ = memory::arena_new<uint16_t>(mem.resident, kLiveMaxSamples);
    record_state_ = memory::arena_new<uint8_t>(mem.resident, kLiveMaxSamples);
    record_row_ = memory::arena_new<uint32_t>(mem.resident, kLiveMaxSamples);
    if (!song.instruments || !song.samples || !cv_ || !envelopes_ || !envelope_bank_ || !envelope_refs_ ||
        !envelope_freed_ || !record_bank_ || !record_state_ || !record_row_) {
        return "резидентная память переполнена (живая песня)";
    }
    for (uint16_t i = 0; i < kLiveEnvelopes; ++i) envelope_bank_[i] = bank::kNoIndex;
    for (uint16_t i = 0; i < kLiveMaxSamples; ++i) record_bank_[i] = bank::kNoSample;
    song.instrument_count = 0;
    song.sample_count = 0;

    // Таблица распаковки сэмплов банка - в арене, как у загрузчика файла.
    if (bank.packed && mem.bank_table == nullptr) {
        bank::BankDecodeTable* table = memory::arena_new<bank::BankDecodeTable>(mem.resident);
        if (!table) return "резидентная память переполнена (таблица распаковки банка)";
        bank::bank_build_decode_table(*bank.model, *table);
        mem.bank_table = table;
    }
    // Пул keymap - остаток арены за вычетом места ревербератора: линии
    // садятся на вершину того же пула, но позже, при сборке движка. Забрать
    // весь остаток - оставить движок без них.
    const size_t reserve = song.reverb_enabled ? memory::kPlayScratchBytes : 0u;
    const size_t free_now = memory::arena_free(mem.resident);
    const size_t left = free_now > reserve ? free_now - reserve : 0u;
    keymap_capacity_ = static_cast<uint32_t>(left / sizeof(KeymapRange));
    if (keymap_capacity_ > 0) keymap_capacity_ -= 1; // запас на выравнивание
    keymap_pool_ = static_cast<KeymapRange*>(
        memory::arena_alloc_array(mem.resident, keymap_capacity_, sizeof(KeymapRange), alignof(KeymapRange)));
    if (!keymap_pool_ || keymap_capacity_ == 0) return "резидентная память переполнена (keymap живой песни)";

    constexpr uint32_t kHeldBytes = 16u * 128u * kMaxLayers;
    constexpr uint32_t kPendingBytes = 16u * 128u;
    Converter& cv = *cv_;
    cv.song_inst_of = psram_map<uint16_t>(mem.psram, bank.header->instrument_count);
    cv.bank_to_song_sample = psram_map<uint16_t>(mem.psram, bank.header->sample_count);
    cv.held = psram_map<uint8_t>(mem.psram, kHeldBytes);
    cv.pending = psram_map<uint8_t>(mem.psram, kPendingBytes);
    cv.layer_cache = psram_map<LayerCacheEntry>(mem.psram, kLayerCacheSlots);
    if (!cv.song_inst_of || !cv.bank_to_song_sample || !cv.held || !cv.pending || !cv.layer_cache) {
        return "служебные карты живого входа не влезают в PSRAM";
    }
    std::memset(cv.song_inst_of, 0xff, bank.header->instrument_count * sizeof(uint16_t));
    std::memset(cv.bank_to_song_sample, 0xff, bank.header->sample_count * sizeof(uint16_t));
    std::memset(cv.held, 0xff, kHeldBytes);
    std::memset(cv.pending, 0, kPendingBytes);
    std::memset(cv.layer_cache, 0, sizeof(LayerCacheEntry) * kLayerCacheSlots);
    // Остаток PSRAM - сэмплам.
    (void)memory::psram_freeze_pattern_zone(mem.psram);

    cv.bank = bank;
    cv.stats = &stats_;
    cv.grid = Grid{0, ticks_per_row};
    cv.drum_channel[9] = true;
    cv.cur_tempo = tempo;
    cv.vibrato_speed = vibrato_speed_at(tempo);
    cv.want_global = song.default_global_volume;
    cv.max_instruments = kLiveMaxInstruments;
    cv.max_samples = kLiveMaxSamples;
    cv.alloc_instrument = &LiveMidi::alloc_instrument;
    cv.on_new_instrument = &LiveMidi::on_new_instrument;
    cv.alloc_sample = &LiveMidi::alloc_sample;
    cv.on_sample = &LiveMidi::on_sample;
    cv.hook_user = this;
    // Тиков в секунду для огибающих и затухания - как у загрузчика файла.
    ticks_per_second_rounded_ = (2u * tempo + 2u) / 5u;
    return nullptr;
}

void LiveMidi::begin_row() {
    cv_->begin_row();
}

void LiveMidi::event(uint8_t status, uint8_t d1, uint8_t d2, uint8_t delay) {
    Event e{};
    e.status = status;
    e.d1 = d1;
    e.d2 = d2;
    cv_->handle(e, row_ * cv_->grid.ticks_per_row + delay, delay);
}

void LiveMidi::prefetch_note(uint8_t bank_no, uint8_t program, uint8_t note, uint8_t velocity) {
    bank::NoteLayer picked[bank::kMaxNoteLayers];
    bool capped = false;
    // Всё, что заведено отсюда, заведено заранее; заведённое мимо этого флага
    // родилось на тике самой ноты, и PCM за неё заказать уже поздно.
    in_prefetch_ = true;
    cv_->select_layers(bank_no, program, note, velocity, picked, capped);
    in_prefetch_ = false;
}

void LiveMidi::finish_row() {
    cv_->finish_row(static_cast<uint64_t>(row_) * cv_->grid.ticks_per_row);
    ++row_;
}

// --- Номера инструментов и вытеснение ---

bool LiveMidi::slot_live(uint16_t slot) const {
    return slot < cv_->used_instrument_count && cv_->song_inst_of[cv_->used_instruments[slot]] == slot;
}

// Звучит нота инструмента или доигрывает релиз - с запасом в секунду: голос
// гаснет по затуханию, а не ровно в расчётный тик.
bool LiveMidi::slot_sounding(uint16_t slot) const {
    const uint32_t now = row_ * cv_->grid.ticks_per_row;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        const TrackerChannel& t = cv_->tch[c];
        if (t.inst != slot) continue;
        if (t.note != 0xff || t.quiet_at + ticks_per_second_rounded_ > now) return true;
    }
    return false;
}

uint16_t LiveMidi::pick_victim(uint16_t except) const {
    uint16_t victim = 0xffff;
    for (uint16_t s = 0; s < cv_->used_instrument_count; ++s) {
        if (s == except || !slot_live(s) || slot_sounding(s)) continue;
        if (victim == 0xffff || last_row_[s] < last_row_[victim]) victim = s;
    }
    return victim;
}

// Номер и огибающие освобождаются, keymap уходит при уплотнении, записи
// сэмплов остаются: вернётся программа - PCM, может быть, ещё в памяти.
void LiveMidi::evict(uint16_t slot) {
    Instrument& ins = song_->instruments[slot];
    envelope_release(ins.volume_envelope);
    envelope_release(ins.filter_envelope);
    ins = Instrument{};
    cv_->song_inst_of[cv_->used_instruments[slot]] = 0xffff;
    ++instruments_evicted_;
}

uint16_t LiveMidi::alloc_instrument(void* self) {
    LiveMidi& lm = *static_cast<LiveMidi*>(self);
    Converter& cv = *lm.cv_;
    if (cv.used_instrument_count < cv.max_instruments) return cv.used_instrument_count++;
    const uint16_t victim = lm.pick_victim(0xffff);
    if (victim == 0xffff) return 0xffff;
    lm.evict(victim);
    return victim;
}

// Инструмент получил номер: строится сразу; зоны без записи сэмпла в
// keymap пусты до первой ноты (alloc_sample). Не хватило памяти - вытеснить
// молчащий и повторить.
bool LiveMidi::on_new_instrument(void* self, uint16_t song_inst) {
    LiveMidi& lm = *static_cast<LiveMidi*>(self);
    soundsinth::model::Song& song = *lm.song_;
    lm.last_row_[song_inst] = lm.row_;
    ++lm.instruments_built_;
    if (!lm.in_prefetch_) ++lm.instruments_late_;
    for (uint32_t tries = 0;; ++tries) {
        if (lm.build(song_inst)) break;
        const uint16_t victim = tries < kMaxEvictions ? lm.pick_victim(song_inst) : 0xffff;
        if (victim == 0xffff) {
            song.instruments[song_inst] = Instrument{};
            ++lm.instruments_failed_;
            return false;
        }
        lm.evict(victim);
    }
    if (song.instrument_count < song_inst + 1u) song.instrument_count = static_cast<uint16_t>(song_inst + 1u);
    return true;
}

void LiveMidi::on_sample(void* self, uint16_t song_inst, uint16_t bank_sample) {
    LiveMidi& lm = *static_cast<LiveMidi*>(self);
    if (song_inst < kLiveMaxInstruments) lm.last_row_[song_inst] = lm.row_;
    const uint16_t rec = lm.cv_->bank_to_song_sample[bank_sample];
    if (rec < kLiveMaxSamples) lm.record_row_[rec] = lm.row_;
    if (lm.request_) lm.request_(lm.user_, lm.cv_->bank_to_song_sample[bank_sample]);
}

// --- Записи сэмплов ---

namespace {
constexpr uint8_t kRecordFree = 0, kRecordUsed = 1, kRecordLeaving = 2;
}

// Запись сэмплу банка: свободный номер, запись из банка, зоны этого сэмпла
// в keymap живых инструментов получают номер. Свободных нет - слой отпадает,
// отдача записей уже идёт.
uint16_t LiveMidi::alloc_sample(void* self, uint16_t bank_sample) {
    LiveMidi& lm = *static_cast<LiveMidi*>(self);
    Converter& cv = *lm.cv_;
    soundsinth::model::Song& song = *lm.song_;
    uint16_t rec = 0xffff;
    for (uint16_t r = 0; r < kLiveMaxSamples && rec == 0xffff; ++r) {
        if (lm.record_state_[r] == kRecordFree) rec = r;
    }
    if (rec == 0xffff) {
        ++lm.samples_capped_;
        lm.retire_records();
        return 0xffff;
    }
    sample_from_bank(cv.bank, bank_sample, song.samples[rec]);
    lm.record_bank_[rec] = bank_sample;
    lm.record_state_[rec] = kRecordUsed;
    lm.record_row_[rec] = lm.row_;
    --lm.records_free_;
    ++lm.records_allocated_;
    if (!lm.in_prefetch_) ++lm.records_late_;
    // Запись - до счётчика: читатель счётчика видит готовую запись.
    if (song.sample_count < rec + 1u) song.sample_count = static_cast<uint16_t>(rec + 1u);
    if (cv.used_sample_count < song.sample_count) cv.used_sample_count = song.sample_count;
    for (uint16_t s = 0; s < cv.used_instrument_count; ++s) {
        if (!lm.slot_live(s)) continue;
        Instrument& ins = song.instruments[s];
        if (ins.note_to_sample_ranges == nullptr) continue;
        const bank::BankInstrument& bi = cv.bank.instruments[cv.used_instruments[s]];
        auto* km = const_cast<KeymapRange*>(ins.note_to_sample_ranges);
        lm.keymap_visits_ += bi.keymap_count;
        for (uint16_t k = 0; k < bi.keymap_count; ++k) {
            if (cv.bank.keymap[bi.keymap_first + k].sample_index == bank_sample) km[k].sample_index = rec;
        }
    }
    if (lm.records_free_ + lm.records_leaving_ < kLiveRecordReserve) lm.retire_records();
    return rec;
}

void LiveMidi::retire_records() {
    Converter& cv = *cv_;
    ++retire_calls_;
    // Записи, на которые ссылаются keymap живых инструментов.
    uint32_t referenced[(kLiveMaxSamples + 31u) / 32u] = {};
    for (uint16_t s = 0; s < cv.used_instrument_count; ++s) {
        if (!slot_live(s)) continue;
        const Instrument& ins = song_->instruments[s];
        if (ins.note_to_sample_ranges == nullptr) continue;
        const bank::BankInstrument& bi = cv.bank.instruments[cv.used_instruments[s]];
        retire_keymap_visits_ += bi.keymap_count;
        for (uint16_t k = 0; k < bi.keymap_count; ++k) {
            const uint16_t r = ins.note_to_sample_ranges[k].sample_index;
            if (r < kLiveMaxSamples) referenced[r / 32u] |= 1u << (r % 32u);
        }
    }
    while (records_free_ + records_leaving_ < 2u * kLiveRecordReserve) {
        uint16_t oldest = 0xffff;
        retire_record_visits_ += kLiveMaxSamples;
        for (uint16_t r = 0; r < kLiveMaxSamples; ++r) {
            if (record_state_[r] != kRecordUsed || (referenced[r / 32u] & (1u << (r % 32u)))) continue;
            if (oldest == 0xffff || record_row_[r] < record_row_[oldest]) oldest = r;
        }
        if (oldest == 0xffff) return;
        // Отдаём запись, к которой обращались только что: её PCM выбросят, а
        // нота на неё, возможно, уже едет в очереди. Это и есть подозреваемый
        // в молчащих нотах.
        if (row_ - record_row_[oldest] < kRecentRows) ++records_retired_recent_;
        // Новая нота этого сэмпла получит другую запись.
        cv.bank_to_song_sample[record_bank_[oldest]] = 0xffff;
        record_state_[oldest] = kRecordLeaving;
        ++records_leaving_;
        ++records_retired_;
        if (retire_) {
            retire_(user_, oldest);
        } else {
            record_retired(oldest);
        }
    }
}

void LiveMidi::record_retired(uint16_t song_sample) {
    if (song_sample >= kLiveMaxSamples || record_state_[song_sample] != kRecordLeaving) return;
    record_state_[song_sample] = kRecordFree;
    record_bank_[song_sample] = bank::kNoSample;
    --records_leaving_;
    ++records_free_;
}

// --- Память инструмента ---

// Те же поля, что у загрузчика файла (instrument_from_bank, огибающие и
// keymap из банка); память - пулы живой песни. false - пул кончился,
// занятое возвращено.
bool LiveMidi::build(uint16_t slot) {
    Converter& cv = *cv_;
    const bank::BankInstrument& bi = cv.bank.instruments[cv.used_instruments[slot]];
    Instrument ins{};
    instrument_from_bank(bi, ticks_per_second_rounded_, ins);
    if (bi.env_volume != bank::kNoIndex) {
        ins.volume_envelope = envelope_acquire(bi.env_volume);
        if (!ins.volume_envelope) return false;
    }
    if (bi.env_filter != bank::kNoIndex) {
        ins.filter_envelope = envelope_acquire(bi.env_filter);
        if (!ins.filter_envelope) {
            envelope_release(ins.volume_envelope);
            return false;
        }
    }
    KeymapRange* km = keymap_alloc(bi.keymap_count, slot);
    if (!km) {
        envelope_release(ins.volume_envelope);
        envelope_release(ins.filter_envelope);
        return false;
    }
    fill_keymap(cv.bank, bi, cv.bank_to_song_sample, km);
    ins.note_to_sample_ranges = km;
    ins.note_to_sample_range_count = static_cast<uint8_t>(bi.keymap_count > 255 ? 255 : bi.keymap_count);
    ins.default_sample_index = 0;
    song_->instruments[slot] = ins;
    return true;
}

const soundsinth::model::Envelope* LiveMidi::envelope_acquire(uint16_t bank_envelope) {
    uint16_t pick = 0xffff;
    for (uint16_t i = 0; i < kLiveEnvelopes; ++i) {
        if (envelope_bank_[i] == bank_envelope) {
            ++envelope_refs_[i];
            return &envelopes_[i];
        }
        if (envelope_refs_[i] != 0) continue;
        // Свободное: пустое раньше занятого когда-то, из занятых - давнее.
        if (pick == 0xffff || (envelope_bank_[pick] != bank::kNoIndex &&
                               (envelope_bank_[i] == bank::kNoIndex || envelope_freed_[i] < envelope_freed_[pick]))) {
            pick = i;
        }
    }
    if (pick == 0xffff) return nullptr;
    Envelope& env = envelopes_[pick];
    env = Envelope{};
    envelope_from_bank(cv_->bank.envelopes[bank_envelope], ticks_per_second_rounded_, env);
    envelope_bank_[pick] = bank_envelope;
    envelope_refs_[pick] = 1;
    return &env;
}

void LiveMidi::envelope_release(const soundsinth::model::Envelope* env) {
    if (env == nullptr) return;
    const uint32_t i = static_cast<uint32_t>(env - envelopes_);
    if (i >= kLiveEnvelopes || envelope_refs_[i] == 0) return;
    if (--envelope_refs_[i] == 0) envelope_freed_[i] = ++envelope_clock_;
}

soundsinth::model::KeymapRange* LiveMidi::keymap_alloc(uint16_t count, uint16_t building) {
    if (keymap_used_ + count > keymap_capacity_) keymap_compact(building);
    if (keymap_used_ + count > keymap_capacity_) return nullptr;
    KeymapRange* km = keymap_pool_ + keymap_used_;
    keymap_used_ += count;
    return km;
}

// Живые keymap - заново из банка подряд; дыры вытесненных уходят.
void LiveMidi::keymap_compact(uint16_t building) {
    Converter& cv = *cv_;
    keymap_used_ = 0;
    for (uint16_t s = 0; s < cv.used_instrument_count; ++s) {
        if (s == building || !slot_live(s)) continue;
        Instrument& ins = song_->instruments[s];
        if (ins.note_to_sample_ranges == nullptr) continue;
        const bank::BankInstrument& bi = cv.bank.instruments[cv.used_instruments[s]];
        KeymapRange* km = keymap_pool_ + keymap_used_;
        fill_keymap(cv.bank, bi, cv.bank_to_song_sample, km);
        ins.note_to_sample_ranges = km;
        keymap_used_ += bi.keymap_count;
    }
}

} // namespace soundsinth::formats::midi
