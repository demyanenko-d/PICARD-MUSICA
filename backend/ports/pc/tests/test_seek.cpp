// SPDX-License-Identifier: MIT
// Перемотка вперёд (TrackerEngine::seek_to_frame).
//
// Прыжком по строкам состояние канала не восстановить: инструмент,
// громкость, панорама, бенд и темп приходят эффектами предыдущих строк.
// Поэтому перемотка прогоняет тики - те же, что при игре, только без
// сведения. Проверяется ровно это: после перемотки секвенсор обязан
// стоять там же, где он оказался бы, доиграв до этого кадра обычным
// путём.

#include "testing.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/engine/tracker_engine.h"
#include "core/formats/it.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "core/audio/mixbus.h"
#include "core/memory/track_memory.h"

namespace {

using namespace soundsinth;

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

using LoadFn = bool (*)(formats::ByteSource, memory::TrackMemory&, soundsinth::model::Song&, const char**, bool);

// Где стоит секвенсор и на чём он играет: этим сравниваются два пути.
struct Spot {
    uint16_t order_pos   = 0;
    uint16_t pattern_idx = 0;
    uint16_t row         = 0;
    uint16_t tick_in_row = 0;
    uint16_t tempo       = 0;
    uint16_t speed       = 0;
    uint32_t frames      = 0;
};

Spot spot_of(const engine::TrackerEngine& eng) {
    const engine::PlayState& ps = eng.play_state();
    return Spot{ps.order_pos, ps.pattern_idx, ps.row, ps.tick_in_row, ps.tempo, ps.speed, eng.elapsed_frames()};
}

bool same_spot(const Spot& a, const Spot& b) {
    return a.order_pos == b.order_pos && a.pattern_idx == b.pattern_idx && a.row == b.row && a.tick_in_row == b.tick_in_row && a.tempo == b.tempo &&
           a.speed == b.speed && a.frames == b.frames;
}

void check_seek_matches_playback(LoadFn load, const char* path, uint32_t target_frames) {
    // Рендер идёт буферами и в цель попадает только кратной им: иначе он
    // перелетает на остаток буфера, и пути разойдутся по числу кадров, а не
    // по существу.
    target_frames                    -= target_frames % SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    const std::vector<uint8_t> bytes  = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }

    // Два движка на одной памяти трека подряд: первый доигрывает до кадра
    // обычным рендером, второй перематывается туда же.
    Spot played{};
    Spot sought{};
    for (int pass = 0; pass < 2; ++pass) {
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
        engine::TrackerEngine eng(song, mem);

        if (pass == 0) {
            constexpr uint32_t kChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
            std::vector<int32_t> mix_l(kChunk), mix_r(kChunk);
            mixbus::SoundSource* s = eng.as_sound_source();
            // Ровно до кадра: буфер кратен цели, иначе пути разойдутся на
            // остаток батча, а не по существу.
            for (uint32_t done = 0; done < target_frames; done += kChunk) {
                s->render_add(s->self, mix_l.data(), mix_r.data(), kChunk);
            }
            played = spot_of(eng);
        } else {
            CHECK(eng.seek_to_frame(target_frames));
            sought = spot_of(eng);
        }
        memory::track_memory_destroy(mem);
    }

    const bool ok = same_spot(played, sought);
    CHECK(ok);
    std::printf("  %s: play order=%u patt=%u row=%u tick=%u tempo=%u/%u frames=%u | seek %u/%u/%u/%u %u/%u/%u %s\n", path, played.order_pos, played.pattern_idx,
                played.row, played.tick_in_row, played.tempo, played.speed, played.frames, sought.order_pos, sought.pattern_idx, sought.row, sought.tick_in_row,
                sought.tempo, sought.speed, sought.frames, ok ? "matched" : "DIFFERS");
}

// Перематывать назад и в уже сыгранное нечего: движок для этого
// пересоздают. Отказ обязан быть явным, а не тихим прыжком не туда.
void check_seek_refuses_backwards(LoadFn load, const char* path) {
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
    engine::TrackerEngine eng(song, mem);
    CHECK(eng.seek_to_frame(44100u * 5u));
    // Назад - отказ; в ту же точку - тоже, идти некуда.
    CHECK(!eng.seek_to_frame(44100u));
    CHECK(!eng.seek_to_frame(eng.elapsed_frames()));
    // И позиция после отказа не сдвинулась.
    CHECK_EQ(eng.elapsed_frames(), 44100u * 5u);
    memory::track_memory_destroy(mem);
}

// Кадр конца трека шина считает своим счётчиком, а перемотка двигает
// движок мимо неё. Не сказать шине о прыжке - значит отодвинуть её
// затухание ровно на прыжок: на плате конец трека тогда гасила только
// страховка Core1, и начало второго прохода звучало поверх конца.
//
// skip_frames берётся с двух сторон: с ним выход на кадре конца - нули,
// без него на том же кадре ещё звучит. Второе - не придирка, а
// доказательство, что проверка ловит.
void check_seek_keeps_end_fade(LoadFn load, const char* path, uint32_t end_frame, uint32_t play_frames, uint32_t seek_frames) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    constexpr uint32_t kChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    uint32_t tail_peak[2]     = {0, 0};
    uint32_t heard[2]         = {0, 0};
    for (int with_skip = 0; with_skip < 2; ++with_skip) {
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
        engine::TrackerEngine eng(song, mem);
        mixbus::MixBus bus;
        bus.start_track();
        bus.set_end_frame(end_frame);
        CHECK(bus.add_source(eng.as_sound_source()));

        std::vector<int16_t> out(kChunk * 2);
        uint32_t rendered = 0;
        for (; rendered < play_frames; rendered += kChunk) {
            bus.render(out.data(), kChunk);
            for (int16_t v : out)
                heard[with_skip] += static_cast<uint32_t>(v < 0 ? -v : v);
        }
        CHECK(eng.seek_to_frame(rendered + seek_frames));
        if (with_skip) bus.skip_frames(seek_frames);

        // Обе стороны рендерят одно и то же: столько кадров, сколько
        // осталось движку до конца трека. Разница только в том, знает ли
        // шина о прыжке, то есть попадёт ли её затухание в этот кадр.
        const uint32_t until = end_frame - seek_frames;
        for (; rendered < until; rendered += kChunk) {
            bus.render(out.data(), kChunk);
        }
        // Затухание кончается на кадре конца; следующие буферы - нули.
        for (uint32_t k = 0; k < 4; ++k) {
            bus.render(out.data(), kChunk);
            for (int16_t v : out) {
                const uint32_t a = static_cast<uint32_t>(v < 0 ? -v : v);
                if (a > tail_peak[with_skip]) tail_peak[with_skip] = a;
            }
        }
        memory::track_memory_destroy(mem);
    }
    CHECK(heard[1] > 0);        // трек вообще звучал
    CHECK_EQ(tail_peak[1], 0u); // с прыжком, отданным шине, конец - тишина
    CHECK(tail_peak[0] > 0);    // без него на том же кадре ещё звучит
    std::printf("  %s: past the end frame peak %u (jump announced) against %u (not announced)\n", path, tail_peak[1], tail_peak[0]);
}

// Перемотка в самый конец: цель обрезана по кадру конца трека, и выход
// после неё не возвращается - возвращать нечего. Вторым случаем идёт
// прыжок не до конца: там выход обязан зазвучать снова, иначе проверка
// доказывала бы только то, что шина умеет молчать.
void check_seek_to_end_stays_silent(LoadFn load, const char* path, uint32_t end_frame, uint32_t play_frames) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    constexpr uint32_t kChunk = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    uint32_t peak[2]          = {0, 0};
    for (int to_end = 0; to_end < 2; ++to_end) {
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
        engine::TrackerEngine eng(song, mem);
        mixbus::MixBus bus;
        bus.start_track();
        bus.set_end_frame(end_frame);
        CHECK(bus.add_source(eng.as_sound_source()));

        std::vector<int16_t> out(kChunk * 2);
        uint32_t rendered = 0;
        for (; rendered < play_frames; rendered += kChunk)
            bus.render(out.data(), kChunk);
        // Порядок платы: на время перемотки выход гасится паузой, прыжок
        // делается по тихому проходу, затем пауза снимается.
        bus.set_paused(true);
        bus.render(out.data(), kChunk); // буфер затухания, счётчик растёт
        rendered += kChunk;
        bus.render(out.data(), kChunk); // тихий проход, источники не зовутся
        CHECK(bus.silent_renders() > 0u);
        // Не до конца - за секунду до него: там ещё идёт обычная игра, а не
        // затухание, и тихий кусок трека не сойдёт за молчащий выход.
        const uint32_t target = to_end ? end_frame : end_frame - 44100u;
        CHECK(eng.seek_to_frame(target));
        bus.skip_frames(target - rendered);
        bus.set_paused(false);
        // Буферов на несколько строк: перемотка снимает все голоса, и до
        // первой новой ноты выход молчит законно. Одной строки мало -
        // тогда тишина после прыжка сошла бы за погашенный выход.
        for (uint32_t k = 0; k < 64; ++k) {
            bus.render(out.data(), kChunk);
            for (int16_t v : out) {
                const uint32_t a = static_cast<uint32_t>(v < 0 ? -v : v);
                if (a > peak[to_end]) peak[to_end] = a;
            }
        }
        memory::track_memory_destroy(mem);
    }
    CHECK_EQ(peak[1], 0u); // прыжок в конец - выход не возвращается
    CHECK(peak[0] > 0);    // прыжок не до конца - трек играет дальше
    std::printf("  %s: after the jump to the end peak %u, short of the end %u\n", path, peak[1], peak[0]);
}
void test_seek_lands_where_playback_would() {
    std::printf("test_seek_lands_where_playback_would\n");
    check_seek_matches_playback(&formats::it::load, "SD/test_music/it/00009.it", 44100u * 8u);
    check_seek_matches_playback(&formats::it::load, "SD/test_music/it/ivi-lite__v61.it", 44100u * 12u);
    check_seek_matches_playback(&formats::xm::load, "SD/test_music/xm/final_fantasy.xm", 44100u * 12u);
}

void test_seek_keeps_end_fade() {
    std::printf("test_seek_keeps_end_fade\n");
    check_seek_keeps_end_fade(&formats::it::load, "SD/test_music/it/ivi-lite__v61.it", 44100u * 18u, 44100u * 4u, 44100u * 5u);
    check_seek_keeps_end_fade(&formats::xm::load, "SD/test_music/xm/final_fantasy.xm", 44100u * 18u, 44100u * 4u, 44100u * 5u);
}

void test_seek_to_end_stays_silent() {
    std::printf("test_seek_to_end_stays_silent\n");
    check_seek_to_end_stays_silent(&formats::it::load, "SD/test_music/it/ivi-lite__v61.it", 44100u * 18u, 44100u * 4u);
    check_seek_to_end_stays_silent(&formats::xm::load, "SD/test_music/xm/final_fantasy.xm", 44100u * 18u, 44100u * 4u);
}

void test_seek_refuses_backwards() {
    std::printf("test_seek_refuses_backwards\n");
    check_seek_refuses_backwards(&formats::it::load, "SD/test_music/it/00009.it");
}

} // namespace

void run_seek_tests() {
    test_seek_lands_where_playback_would();
    test_seek_keeps_end_fade();
    test_seek_to_end_stays_silent();
    test_seek_refuses_backwards();
}
