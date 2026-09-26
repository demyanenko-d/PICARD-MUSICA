#pragma once

// Живой вход MIDI: события потока -> строки ячеек тем же конвертером, что у
// загрузчика .mid (midi_convert.h). Песня без паттернов: инструменты и
// записи сэмплов банка добавляются в неё при первой ноте программы, PCM
// подгружается по запросу (player::load::progressive_request).
//
// Инструмент строится сразу при первой ноте. Запись сэмпла песни - по
// первой ноте в его зоне: keymap живых инструментов дописывается её
// номером. Номеров инструментов kLiveMaxInstruments: когда они или память
// кончаются, номер отдаётся дольше всех не звучавшему инструменту. Записи,
// на которые не ссылается ни один живой keymap, отдаются заранее, с
// подтверждением от загрузчика: PCM записи держит другое ядро.
//
// Память инструментов. Огибающие - места постоянного размера, место не
// двигается: канал движка держит указатель на огибающую звучащей ноты.
// Инструменты с общей огибающей банка делят одно место. Keymap - общий пул:
// кончился - все живые keymap переписываются из банка подряд. Keymap
// читается только при выборе сэмпла под ноту, поэтому переписывать можно
// между строками на ядре рендера.

#include <cstdint>

#include "core/bank/bank_reader.h"
#include "core/model/song.h"
#include "core/formats/midi.h"
#include "core/formats/midi_convert.h"
#include "core/memory/track_memory.h"

namespace soundsinth::formats::midi {

// Потолки живой песни - в резидентной арене вместе с конвертером и таблицей
// распаковки банка; пул keymap - весь остаток арены.
inline constexpr uint16_t kLiveMaxInstruments = 128;
inline constexpr uint16_t kLiveMaxSamples = 384;
inline constexpr uint16_t kLiveEnvelopes = 112;
// Столько записей держать свободными или уходящими: отдача идёт заранее,
// нота не ждёт подтверждения с другого ядра.
inline constexpr uint16_t kLiveRecordReserve = 32;
// Сколько строк запись считается свежей. Строка живого режима - тик, то есть
// десять миллисекунд; нота ждёт своей форы столько же, сколько эти двадцать
// строк. Отдать запись, к которой обращались в этом окне, - значит рискнуть
// нотой, которая на неё уже едет.
inline constexpr uint32_t kRecentRows = 20;

class LiveMidi {
public:
    // Нота взяла запись сэмпла песни song_sample: подгрузить PCM. Зовётся на
    // каждой ноте, повтор отсекает очередь загрузчика.
    using RequestFn = void (*)(void* user, uint16_t song_sample);
    // Запись song_sample уходит: выбросить её PCM, когда голоса его не
    // держат, и подтвердить record_retired. До подтверждения номер не
    // выдаётся: PCM записи - у загрузчика на другом ядре.
    using RetireFn = void (*)(void* user, uint16_t song_sample);

    // Песня заново поверх памяти трека: вызывающий уже сбросил память
    // (track_memory_reset_for_new_track). ticks_per_row и tempo - сетка и
    // темп живого режима. nullptr - готово, иначе причина отказа.
    const char* begin(soundsinth::model::Song& song, memory::TrackMemory& mem, const bank::Bank& bank, uint8_t ticks_per_row,
                      uint8_t tempo, RequestFn request, RetireFn retire, void* user);

    // PCM записи выброшен, номер свободен.
    void record_retired(uint16_t song_sample);

    // Канал ударный (GS/XG SysEx назначения); после begin - десятый.
    void set_drum_channel(uint8_t channel, bool drums) {
        if (channel < 16) cv_->drum_channel[channel] = drums;
    }
    bool drum_channel(uint8_t channel) const { return channel < 16 && cv_->drum_channel[channel]; }

    // Поток прислал ненулевой посыл на ревербератор (CC91). Линии заведены
    // всегда, но шина молчит, пока этого не случилось.
    bool reverb_used() const { return cv_->reverb_used; }

    // Упреждение: нота ещё не звучит, но её инструмент уже построен, записи
    // сэмплов выданы и PCM заказан - к своему тику сэмпл успеет доехать.
    // Идемпотентно: та же нота на своём тике пройдёт этот путь ещё раз.
    // bank_no и program - какими они будут к моменту ноты, их ведёт
    // вызывающий: программа могла смениться событием, которое ещё ждёт
    // своего тика.
    void prefetch_note(uint8_t bank_no, uint8_t program, uint8_t note, uint8_t velocity);

    // Строка: begin_row, события строки, finish_row; ячейки - row() до
    // следующего begin_row.
    void begin_row();
    // Событие канала; delay - тиков от начала строки, меньше ticks_per_row.
    void event(uint8_t status, uint8_t d1, uint8_t d2, uint8_t delay);
    void finish_row();
    const soundsinth::model::PatternCell* row() const { return cv_->cells; }

    // Инструмент банка за номером песни (для проверок).
    uint16_t bank_instrument(uint16_t song_inst) const { return cv_->used_instruments[song_inst]; }

    // Потери раскладки (кражи каналов, ноты сверх потолков); инструменты, не
    // вставшие и после вытеснения; слои без свободной записи сэмпла;
    // вытеснено инструментов; отдано записей.
    const LoadStats& stats() const { return stats_; }
    uint32_t instruments_failed() const { return instruments_failed_; }
    uint32_t samples_capped() const { return samples_capped_; }
    uint32_t instruments_evicted() const { return instruments_evicted_; }
    uint32_t records_retired() const { return records_retired_; }

    // Работа, которая растёт с числом живых инструментов и делается в тике
    // рендера: перестановка ссылок keymap при заведении записи и поиск давних
    // записей. Считается в просмотренных элементах, а не во времени: время
    // меряет тот, кто зовёт.
    uint32_t instruments_built() const { return instruments_built_; }
    uint32_t records_allocated() const { return records_allocated_; }

    // Молчащая нота (движок считает их как no_sample) - дефект звука, и
    // причин у неё три. Эти счётчики их и разделяют.
    // Заведено не в упреждении, а на тике самой ноты: заказать PCM уже
    // поздно, подгрузка живёт на другой задаче.
    uint32_t instruments_late() const { return instruments_late_; }
    uint32_t records_late() const { return records_late_; }
    // Запись отдана, хотя к ней обращались меньше kRecentRows строк назад:
    // её PCM выбросили, а нота на неё могла уже ехать в очереди.
    uint32_t records_retired_recent() const { return records_retired_recent_; }
    uint32_t keymap_visits() const { return keymap_visits_; }
    uint32_t retire_calls() const { return retire_calls_; }
    uint32_t retire_keymap_visits() const { return retire_keymap_visits_; }
    uint32_t retire_record_visits() const { return retire_record_visits_; }

private:
    static uint16_t alloc_instrument(void* self);
    static bool on_new_instrument(void* self, uint16_t song_inst);
    static uint16_t alloc_sample(void* self, uint16_t bank_sample);
    static void on_sample(void* self, uint16_t song_inst, uint16_t bank_sample);

    // Отдать давние записи, на которые не ссылается ни один живой keymap,
    // пока свободных и уходящих меньше kLiveRecordReserve.
    void retire_records();

    bool slot_live(uint16_t slot) const;
    bool slot_sounding(uint16_t slot) const;
    // Дольше всех не звучавший живой инструмент, не except; 0xffff - нет.
    uint16_t pick_victim(uint16_t except) const;
    void evict(uint16_t slot);
    bool build(uint16_t slot);
    const soundsinth::model::Envelope* envelope_acquire(uint16_t bank_envelope);
    void envelope_release(const soundsinth::model::Envelope* env);
    soundsinth::model::KeymapRange* keymap_alloc(uint16_t count, uint16_t building);
    void keymap_compact(uint16_t building);

    Converter* cv_ = nullptr;
    soundsinth::model::Song* song_ = nullptr;
    memory::TrackMemory* mem_ = nullptr;
    RequestFn request_ = nullptr;
    RetireFn retire_ = nullptr;
    void* user_ = nullptr;
    // Записи сэмплов: сэмпл банка (kNoSample - свободна), состояние, строка
    // последнего звучания. Уходящая ждёт record_retired.
    uint16_t* record_bank_ = nullptr;
    uint8_t* record_state_ = nullptr;
    uint32_t* record_row_ = nullptr;
    uint16_t records_free_ = 0;
    uint16_t records_leaving_ = 0;
    uint32_t records_retired_ = 0;
    uint32_t row_ = 0;
    uint32_t ticks_per_second_rounded_ = 0;
    uint32_t instruments_failed_ = 0;
    uint32_t samples_capped_ = 0;
    uint32_t instruments_evicted_ = 0;
    uint32_t instruments_built_ = 0;
    uint32_t records_allocated_ = 0;
    uint32_t instruments_late_ = 0;
    uint32_t records_late_ = 0;
    uint32_t records_retired_recent_ = 0;
    // Идёт упреждающая подготовка: заведённое сейчас заведено заранее.
    bool in_prefetch_ = false;
    uint32_t keymap_visits_ = 0;
    uint32_t retire_calls_ = 0;
    uint32_t retire_keymap_visits_ = 0;
    uint32_t retire_record_visits_ = 0;
    LoadStats stats_{};
    // Строка, когда инструмент звучал последним (нота взяла его сэмпл).
    uint32_t last_row_[kLiveMaxInstruments] = {};

    // Пул огибающих: место, номер огибающей банка (kNoIndex - пусто), число
    // инструментов на месте и когда место освободилось (занимать давно
    // освобождённые: доигрывающий хвост ещё держит недавнее).
    soundsinth::model::Envelope* envelopes_ = nullptr;
    uint16_t* envelope_bank_ = nullptr;
    uint8_t* envelope_refs_ = nullptr;
    uint32_t* envelope_freed_ = nullptr;
    uint32_t envelope_clock_ = 0;

    soundsinth::model::KeymapRange* keymap_pool_ = nullptr;
    uint32_t keymap_capacity_ = 0;
    uint32_t keymap_used_ = 0;
};

} // namespace soundsinth::formats::midi
