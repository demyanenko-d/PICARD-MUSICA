// SPDX-License-Identifier: MIT
// TickObserver движка (engine/tracker_engine.h) для прогрессивной
// загрузки. Это единственный канал, по которому Core0 (звук) сообщает
// Core1 (загрузка), где сейчас играет секвенсор и какие сэмплы держат
// живые голоса. Ошибка здесь означала бы вытеснение звучащего сэмпла из
// PSRAM, то есть мусор в звуке, поэтому проверяется отдельно; тест
// заведён до появления вытеснения.

#include "testing.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <vector>

#include "core/engine/song_duration.h"
#include "core/engine/tracker_engine.h"
#include "core/formats/it.h"
#include "core/formats/mod.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "player/load/sample_prefetch.h"
#include "core/memory/track_memory.h"

namespace {

using namespace soundsinth;

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Capture {
    const soundsinth::model::Song* song   = nullptr;
    const std::vector<uint16_t>* last_use = nullptr;

    uint32_t ticks               = 0;
    uint32_t bad_sample_index    = 0; // сэмпл вне диапазона Song::samples
    uint32_t bad_order_pos       = 0; // позиция вне order-листа
    uint32_t never_used_in_voice = 0; // звучит сэмпл, которого планировщик не нашёл вовсе
    uint32_t outlived_last_use   = 0; // звучит сэмпл, чей last_use уже позади (NNA-хвост)
    uint32_t max_voices          = 0;
    std::set<uint16_t> seen_samples;
    std::set<uint16_t> seen_order_pos;
};

void on_tick(void* user, uint16_t order_pos, const uint16_t* sample_indices, uint8_t count) {
    auto* cap = static_cast<Capture*>(user);
    ++cap->ticks;
    if (count > cap->max_voices) cap->max_voices = count;
    if (order_pos >= cap->song->order_count) ++cap->bad_order_pos;
    cap->seen_order_pos.insert(order_pos);

    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t idx = sample_indices[i];
        if (idx >= cap->song->sample_count) {
            ++cap->bad_sample_index;
            continue;
        }
        cap->seen_samples.insert(idx);
        const uint16_t last_use = (*cap->last_use)[idx];
        if (last_use == player::load::kSampleNeverUsed) {
            ++cap->never_used_in_voice;
        } else if (last_use < order_pos) {
            ++cap->outlived_last_use;
        }
    }
}

using LoadFn = bool (*)(formats::ByteSource, memory::TrackMemory&, soundsinth::model::Song&, const char**, bool);

void check_tick_observer(LoadFn load, const char* path, uint32_t frames) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    if (!load(src.as_byte_source(), mem, song, &err, false)) {
        CHECK(false);
        memory::track_memory_destroy(mem);
        return;
    }

    std::vector<uint16_t> plan(song.sample_count == 0 ? 1 : song.sample_count);
    std::vector<uint16_t> last_use(plan.size());
    const uint16_t prefetch =
        player::load::plan_playback_order(song, mem.psram, plan.data(), static_cast<uint16_t>(plan.size()), last_use.data()).prefetch_count;

    Capture cap;
    cap.song     = &song;
    cap.last_use = &last_use;

    if (frames == 0) {
        frames = engine::compute_song_total_frames(
            song, mem.psram, memory::scratch_take(mem.scratch, memory::Scratch::DurationPass, memory::kDurationPassBytes), memory::kDurationPassBytes);
        memory::scratch_leave(mem.scratch, memory::Scratch::DurationPass);
    }
    engine::TrackerEngine eng(song, mem);
    eng.set_tick_observer(&on_tick, &cap);

    // Кусками по буферу, как на плате.
    constexpr uint32_t kChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    std::vector<int32_t> mix_l(kChunk), mix_r(kChunk);
    mixbus::SoundSource* s = eng.as_sound_source();
    for (uint32_t done = 0; done < frames; done += kChunk) {
        std::fill(mix_l.begin(), mix_l.end(), 0);
        std::fill(mix_r.begin(), mix_r.end(), 0);
        s->render_add(s->self, mix_l.data(), mix_r.data(), kChunk);
    }

    // Наблюдатель срабатывал, и не один раз за рендер.
    CHECK(cap.ticks > 0);
    // Всё, что он отдал, - валидные индексы и валидные позиции.
    CHECK_EQ(cap.bad_sample_index, 0u);
    CHECK_EQ(cap.bad_order_pos, 0u);
    // Планировщик должен знать каждый сэмпл, который зазвучал: иначе
    // фоновая загрузка его не потянула бы, и нота молчала бы.
    CHECK_EQ(cap.never_used_in_voice, 0u);
    // Карта сэмплов - по слотам: не больше числа слотов движка.
    CHECK(cap.max_voices <= SOUNDSINTH_MAX_SLOTS);

    std::printf("  %s: ticks %u, voice peak %u, samples sounded %u, order positions %u, tails past last_use %u\n", path, cap.ticks, cap.max_voices,
                static_cast<unsigned>(cap.seen_samples.size()), static_cast<unsigned>(cap.seen_order_pos.size()), cap.outlived_last_use);

    memory::track_memory_destroy(mem);
}

// Голос может пережить last_use своего сэмпла: NNA-хвост (и обычный
// длинный незацикленный сэмпл) звучит после того, как секвенсор ушёл с
// последней позиции, где этот сэмпл был в паттерне. Поэтому вытеснению
// мало одного last_use - нужна ещё живая карта samples_in_use. Счётчик
// outlived_last_use здесь только печатается, без утверждения: на этих
// трёх файлах за 20 секунд рендера он равен нулю (секвенсор не успевает
// уйти дальше первых order-позиций), так что проверка на > 0 была бы
// неверной, а на == 0 - бессмысленной. Ненулевое значение - не ошибка,
// а тот случай, ради которого карта заводится.
void test_tick_observer_reports_live_voices_and_position() {
    std::printf("test_tick_observer_reports_live_voices_and_position\n");
    check_tick_observer(&formats::it::load, "SD/test_music/it/ivi-lite__v61.it", 44100 * 20);
    check_tick_observer(&formats::it::load, "SD/test_music/it/00009.it", 44100 * 20);
    check_tick_observer(&formats::xm::load, "SD/test_music/xm/final_fantasy.xm", 44100 * 20);
    // Упирается в потолок голосов: 64 канала и фоновые голоса NNA. На всю длину.
    check_tick_observer(&formats::it::load, "music/src/it/smoke/filt_ace-light.it", 0);
}

// Поканальная телеметрия: нота и громкость каждого канала. Их показывают
// экраны внешних плееров (General Sound, связь с хостом), и там они должны
// меняться, а не стоять на одном значении.
struct NoteCapture {
    const engine::TrackerEngine* eng = nullptr;
    std::set<uint8_t> notes[4];
    std::set<uint8_t> vols[4];
    uint32_t ticks = 0;
};

void on_tick_notes(void* user, uint16_t, const uint16_t*, uint8_t) {
    auto* c = static_cast<NoteCapture*>(user);
    ++c->ticks;
    const uint8_t n = c->eng->channel_count() < 4 ? c->eng->channel_count() : 4;
    for (uint8_t ch = 0; ch < n; ++ch) {
        c->notes[ch].insert(c->eng->channel_note(ch));
        c->vols[ch].insert(c->eng->channel_volume(ch));
    }
}

// Возвращает сумму числа РАЗНЫХ нот по четырём каналам за прогон. Ноль -
// файла нет.
uint32_t check_channel_telemetry(LoadFn load, const char* path, uint32_t frames) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return 0;
    }
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    if (!load(src.as_byte_source(), mem, song, &err, false)) {
        CHECK(false);
        memory::track_memory_destroy(mem);
        return 0;
    }
    engine::TrackerEngine eng(song, mem);
    NoteCapture cap;
    cap.eng = &eng;
    eng.set_tick_observer(&on_tick_notes, &cap);

    constexpr uint32_t kChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    std::vector<int32_t> mix_l(kChunk), mix_r(kChunk);
    mixbus::SoundSource* s = eng.as_sound_source();
    for (uint32_t done = 0; done < frames; done += kChunk) {
        std::fill(mix_l.begin(), mix_l.end(), 0);
        std::fill(mix_r.begin(), mix_r.end(), 0);
        s->render_add(s->self, mix_l.data(), mix_r.data(), kChunk);
    }

    std::printf("  %s: ticks %u, каналов %u | ноты", path, cap.ticks, static_cast<unsigned>(eng.channel_count()));
    for (uint8_t ch = 0; ch < 4; ++ch) {
        std::printf(" [");
        for (uint8_t n : cap.notes[ch])
            std::printf("%u ", static_cast<unsigned>(n));
        std::printf("]");
    }
    std::printf(" | числом");
    for (uint8_t ch = 0; ch < 4; ++ch)
        std::printf(" %u", static_cast<unsigned>(cap.notes[ch].size()));
    std::printf(" | громкости");
    for (uint8_t ch = 0; ch < 4; ++ch)
        std::printf(" %u", static_cast<unsigned>(cap.vols[ch].size()));
    std::printf(" (разных значений за прогон)\n");

    CHECK(cap.ticks > 0);
    uint32_t total = 0;
    for (uint8_t ch = 0; ch < 4; ++ch)
        total += static_cast<uint32_t>(cap.notes[ch].size());
    memory::track_memory_destroy(mem);
    return total;
}

// Глубина разводки каналов MOD. У Amiga каналы разведены до упора, и это
// характер формата; настройкой её можно сузить к центру, прочих форматов
// это не касается. Меряется сведением: при нуле левый и правый выходят
// побайтно одинаковыми, при сотне - расходятся.
void check_mod_panning(const char* path, uint32_t frames) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    struct Run {
        uint8_t percent;
        uint64_t diff = 0; // суммарное расхождение каналов
        uint64_t sum  = 0; // суммарный уровень, чтобы знать, что звук был
    };
    Run runs[2] = {{100}, {0}};
    for (Run& r : runs) {
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        soundsinth::model::Song song;
        formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
        const char* err = nullptr;
        if (!formats::mod::load(src.as_byte_source(), mem, song, &err, false)) {
            CHECK(false);
            memory::track_memory_destroy(mem);
            return;
        }
        engine::TrackerEngine eng(song, mem, r.percent);
        constexpr uint32_t kChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        std::vector<int32_t> mix_l(kChunk), mix_r(kChunk);
        mixbus::SoundSource* s = eng.as_sound_source();
        for (uint32_t done = 0; done < frames; done += kChunk) {
            std::fill(mix_l.begin(), mix_l.end(), 0);
            std::fill(mix_r.begin(), mix_r.end(), 0);
            s->render_add(s->self, mix_l.data(), mix_r.data(), kChunk);
            for (uint32_t i = 0; i < kChunk; ++i) {
                const int64_t d  = static_cast<int64_t>(mix_l[i]) - mix_r[i];
                r.diff          += static_cast<uint64_t>(d < 0 ? -d : d);
                r.sum           += static_cast<uint64_t>(mix_l[i] < 0 ? -mix_l[i] : mix_l[i]);
            }
        }
        memory::track_memory_destroy(mem);
    }
    std::printf("  %s: 100%% -> расхождение каналов %llu, 0%% -> %llu (уровень %llu)\n", path, (unsigned long long)runs[0].diff,
                (unsigned long long)runs[1].diff, (unsigned long long)runs[0].sum);
    CHECK(runs[0].sum > 0);       // звук вообще был
    CHECK(runs[0].diff > 0);      // при аппаратной разводке каналы разные
    CHECK_EQ(runs[1].diff, 0ull); // при нуле - ровно моно
}

void test_mod_panning_depth() {
    std::printf("test_mod_panning_depth\n");
    check_mod_panning("SD/test_music/mod/00.mod", 44100 * 5);
}

void test_channel_telemetry_moves() {
    std::printf("test_channel_telemetry_moves\n");
    // На обычном материале нота по каналам ходит. Одно значение на все
    // четыре канала за двадцать секунд означало бы мёртвую телеметрию, а
    // её показывают экраны внешних плееров.
    const uint32_t mod_total = check_channel_telemetry(&formats::mod::load, "SD/test_music/mod/00.mod", 44100 * 20);
    if (mod_total != 0) CHECK(mod_total > 8);
    const uint32_t it_total = check_channel_telemetry(&formats::it::load, "SD/test_music/it/00009.it", 44100 * 20);
    if (it_total != 0) CHECK(it_total > 8);

    // Здесь по одной ноте на канал - свойство файла: во всём модуле один-единственный период 214 и нота стоит в
    // 178 ячейках из 5120. Собран из длинных кусков, высоту им не меняют.
    // Экран плеера на нём стоит, и это не поломка.
    const uint32_t one_note = check_channel_telemetry(&formats::mod::load, "music/src/mod/is_there_anything.mod", 44100 * 20);
    if (one_note != 0) CHECK_EQ(one_note, 4u);
}

} // namespace

void run_tick_observer_tests() {
    test_tick_observer_reports_live_voices_and_position();
    test_channel_telemetry_moves();
    test_mod_panning_depth();
}
