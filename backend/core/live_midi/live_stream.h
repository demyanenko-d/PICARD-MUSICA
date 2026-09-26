#pragma once

// Поток живого MIDI: очередь событий с отметкой времени и строка на тик.
//
// События кладёт разбор потока (порт MIDI или подслушанный порт A AY), а
// забирает рендер - но не сразу, а с упреждением: событие ждёт в очереди
// заданное время, и за эту фору инструмент строится, а PCM его сэмплов
// успевает доехать из флеша. Нота, чей сэмпл ещё едет, молчала бы.
//
// Кольцо - на одного писателя и одного читателя, как у команд голосам:
// стороны живут на разных ядрах. Упреждающий проход и разбор строки - у
// читателя, чтобы живую песню (инструменты, записи сэмплов) трогала одна
// сторона.

#include <atomic>
#include <cstdint>

#include "core/config.h"
#include "core/formats/midi_live.h"

namespace soundsinth::midi_in {

// Событие потока: время прибытия и три байта сообщения канала.
struct StampedEvent {
    uint32_t at_ms = 0;
    uint8_t status = 0;
    uint8_t d1 = 0;
    uint8_t d2 = 0;
};

// Ёмкость кольца: в очереди ждут все события, пришедшие за фору. Линия
// 31250 бод, кадр 8N1 - десять бит на байт, 3125 байт в секунду. Самое
// короткое сообщение канала - два байта (смена программы), то есть 1.6
// события на миллисекунду; за фору и тик их набирается столько.
inline constexpr uint32_t kLiveStreamPerMs = 2; // 1.6 вверх до целого
inline constexpr uint32_t kLiveStreamNeeded =
    (SOUNDSINTH_LIVE_MIDI_LOOKAHEAD_MS + 2500u / SOUNDSINTH_LIVE_MIDI_TEMPO) * kLiveStreamPerMs;
inline constexpr uint32_t kLiveStreamCapacity = 256;
// Построек инструмента за одну строку. Подготовка ноты строит инструмент, и
// это самая дорогая работа строки: на плате пачка из девяти построек (все с
// вытеснением) дала 4.2 мс при тике 10 мс и три провала буфера. Такие пачки
// приходят на стыке вещей, когда плеер разом шлёт смену программы по всем
// каналам. У события есть фора - десять строк, за них пачка успеет
// разойтись; нота, чей инструмент ещё не готов, строит его сама на своём
// тике, как и раньше.
inline constexpr uint32_t kLiveBuildsPerRow = 2;
// Запас меньше этого - подготовка успела впритык: PCM такой ноты обязан
// доехать за считаные тики, а чтение сэмпла из флеша стоит 2 мс типичное и
// 13 мс самое большое.
inline constexpr uint32_t kLeadTightMs = 30;
static_assert((kLiveStreamCapacity & (kLiveStreamCapacity - 1)) == 0, "ёмкость - степень двойки: индекс маской");
static_assert(kLiveStreamCapacity >= kLiveStreamNeeded, "фора не помещается в кольцо: события будут теряться");

class LiveStream {
public:
    // live уже начат (LiveMidi::begin). lookahead_ms - фора событию.
    // Накопленное в очереди сохраняется: по этим событиям режим и распознан,
    // а песне они нужны (выбор банка, программы, первые ноты).
    void begin(formats::midi::LiveMidi* live, uint32_t lookahead_ms) {
        live_ = live;
        lookahead_ms_ = lookahead_ms;
        prefetched_ = read_.load(std::memory_order_relaxed); // упреждение - заново по всей очереди
        lost_ = 0;
        deferred_ = 0;
        lead_min_ms_ = 0xFFFFFFFFu;
        lead_tight_ = 0;
        last_event_ms_ = 0;
        have_event_ = false;
        for (uint32_t c = 0; c < 16; ++c) {
            ahead_program_[c] = 0;
            ahead_bank_[c] = 0;
            ahead_drum_[c] = live->drum_channel(static_cast<uint8_t>(c));
        }
    }

    // --- Писатель ---

    // Сообщение канала (status уже с каналом). false - кольцо полно, событие
    // потеряно: рендер не успел разобрать очередь.
    bool push(uint32_t at_ms, uint8_t status, uint8_t d1, uint8_t d2) {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= kLiveStreamCapacity) {
            ++lost_;
            return false;
        }
        StampedEvent& e = ring_[w & (kLiveStreamCapacity - 1)];
        e.at_ms = at_ms;
        e.status = status;
        e.d1 = d1;
        e.d2 = d2;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    uint32_t lost() const { return lost_; }

    // --- Читатель ---

    // Строка этого тика: события, чья фора истекла. Перед этим - упреждающий
    // проход по ещё не сыгранным: их инструменты и сэмплы заказываются сейчас.
    const soundsinth::model::PatternCell* tick(uint32_t now_ms) {
        now_ms_ = now_ms;
        prefetch_ahead();
        live_->begin_row();
        const uint32_t w = write_.load(std::memory_order_acquire);
        uint32_t r = read_.load(std::memory_order_relaxed);
        while (r != w) {
            const StampedEvent& e = ring_[r & (kLiveStreamCapacity - 1)];
            // Знаковая разность: отметка события не бывает позже now, но
            // округление часов не должно превращать это в вечную фору.
            if (static_cast<int32_t>(now_ms - e.at_ms) < static_cast<int32_t>(lookahead_ms_)) break;
            live_->event(e.status, e.d1, e.d2, /*delay=*/0);
            last_event_ms_ = now_ms;
            have_event_ = true;
            ++r;
        }
        read_.store(r, std::memory_order_release);
        live_->finish_row();
        return live_->row();
    }

    // Сколько миллисекунд не было ни одного сыгранного события. Пока не было
    // ни одного - 0: режим только начался. Отметку ставит читатель, а спрашивают
    // из другой задачи: её время бывает свежее спрошенного, и беззнаковая
    // разность дала бы четыре миллиарда вместо нуля.
    uint32_t silent_ms(uint32_t now_ms) const {
        if (!have_event_) return 0;
        const int32_t d = static_cast<int32_t>(now_ms - last_event_ms_);
        return d > 0 ? static_cast<uint32_t>(d) : 0;
    }

    // Событий в очереди (ждут своей форы).
    uint32_t pending() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed); }

    // Сколько раз подготовка упёрлась в предел построек за строку и отложила
    // остаток. Само по себе не беда - у события есть фора; беда, если рядом
    // растёт число нот без сэмпла.
    uint32_t deferred() const { return deferred_; }

    // Докуда дошла подготовка (для проверок): не позади чтения.
    uint32_t prefetched() const { return prefetched_; }

    // Сколько времени оставалось у ноты до звука, когда её готовили. Это и
    // есть запас, за который PCM обязан доехать: полная фора, если подготовка
    // идёт вровень с приходом событий, и почти ноль, если она отстала.
    uint32_t prefetch_lead_min_ms() const { return lead_min_ms_; }
    // Нот, подготовленных впритык: запаса осталось меньше kLeadTightMs.
    uint32_t prefetch_tight() const { return lead_tight_; }

private:
    // Упреждение: пройти по событиям, которые ещё ждут, и подготовить их
    // ноты. Программу и банк канала ведём сами: событие смены программы
    // тоже ещё ждёт своей форы, а ноте нужна та программа, что будет к ней.
    void prefetch_ahead() {
        const uint32_t w = write_.load(std::memory_order_acquire);
        // Подготовка не отстаёт от чтения. Отложить остаток она вправе, но
        // сыгранные события готовить незачем, а их места в кольце писатель
        // уже вправе занять заново.
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if (static_cast<int32_t>(prefetched_ - r) < 0) prefetched_ = r;
        const uint32_t built0 = live_->instruments_built();
        for (; prefetched_ != w; ++prefetched_) {
            const StampedEvent& e = ring_[prefetched_ & (kLiveStreamCapacity - 1)];
            if (e.status == formats::midi::kDrumChannelStatus) {
                // Ударным канал объявляют по ходу игры, и ноте нужен тот
                // набор, который будет к ней, а не нынешний.
                if (e.d1 < 16) ahead_drum_[e.d1] = e.d2 != 0;
                continue;
            }
            const uint8_t kind = e.status & 0xf0u;
            const uint8_t ch = e.status & 0x0fu;
            if (kind == 0xc0u) {
                ahead_program_[ch] = e.d1;
            } else if (kind == 0xb0u && e.d1 == 0) {
                ahead_bank_[ch] = e.d2;
            } else if (kind == 0x90u && e.d2 != 0) {
                // Запас ноты: сколько ещё ждать её форы. Знаковая разность -
                // событие бывает и старше форы, если подготовка отстала.
                const int32_t waited = static_cast<int32_t>(now_ms_ - e.at_ms);
                const int32_t lead = static_cast<int32_t>(lookahead_ms_) - waited;
                const uint32_t lead_ms = lead > 0 ? static_cast<uint32_t>(lead) : 0u;
                if (lead_ms < lead_min_ms_) lead_min_ms_ = lead_ms;
                if (lead_ms < kLeadTightMs) ++lead_tight_;
                const uint8_t bank_no = ahead_drum_[ch] ? 128u : ahead_bank_[ch];
                live_->prefetch_note(bank_no, ahead_program_[ch], e.d1, e.d2);
                if (live_->instruments_built() - built0 >= kLiveBuildsPerRow) {
                    ++prefetched_; // это событие уже подготовлено
                    ++deferred_;
                    break;
                }
            }
        }
    }

    StampedEvent ring_[kLiveStreamCapacity];
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
    // Докуда дошло упреждение: всегда не позади read_.
    uint32_t prefetched_ = 0;
    uint32_t lost_ = 0;
    uint32_t deferred_ = 0;
    uint32_t now_ms_ = 0;         // время этого тика: подготовка считает по нему запас
    uint32_t lead_min_ms_ = 0xFFFFFFFFu;
    uint32_t lead_tight_ = 0;
    uint32_t last_event_ms_ = 0;
    bool have_event_ = false;
    uint32_t lookahead_ms_ = 0;
    formats::midi::LiveMidi* live_ = nullptr;
    // Программа, банк и ударность канала на конец очереди, а не на сыгранное.
    uint8_t ahead_program_[16] = {};
    uint8_t ahead_bank_[16] = {};
    bool ahead_drum_[16] = {};
};

} // namespace soundsinth::midi_in
