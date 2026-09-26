#include "testing.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/model/song.h"
#include "core/formats/it.h"
#include "core/formats/mod.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "player/load/session_loader.h"
#include "core/memory/psram_store.h"
#include "core/memory/track_memory.h"

#include "song_compare.h"

using namespace soundsinth;

namespace {

enum class Format { Mod, S3m, Xm, It };

struct CorpusEntry {
    const char* path;
    Format format;
};

// Корпус из репозитория (SD/test_music) - воспроизводимо на любой
// машине; форматы и размеры вперемешку, от 12 КБ до 9.5 МБ. Стресс-тест
// смены трека: один процесс, один TrackMemory, N файлов подряд, между
// загрузками - заливка (track_memory_poison) и полный сброс
// (track_memory_reset_for_new_track), чтобы заливка-"отрава" имела шанс поймать
// указатель, оставшийся от предыдущего трека, если загрузчик забыл его
// переустановить.
constexpr CorpusEntry kCorpus[] = {
    {"SD/test_music/s3m/41096877.s3m", Format::S3m},
    {"SD/test_music/xm/_testdri.xm", Format::Xm},
    {"SD/test_music/mod/ptiswap.mod", Format::Mod},
    {"SD/test_music/xm/_lsd_.xm", Format::Xm},
    {"SD/test_music/mod/00_00_00.mod", Format::Mod},
    {"SD/test_music/mod/00.mod", Format::Mod},
    {"SD/test_music/xm/unreeeal_superhero_3.xm", Format::Xm},
    {"SD/test_music/mod/1pattern.mod", Format::Mod},
    {"SD/test_music/mod/12ako.mod", Format::Mod},
    {"SD/test_music/mod/11_1.mod", Format::Mod},
    {"SD/test_music/s3m/4matrmx.s3m", Format::S3m},
    {"SD/test_music/s3m/4channel.s3m", Format::S3m},
    {"SD/test_music/xm/_install_tune_1_.xm", Format::Xm},
    {"SD/test_music/it/00009.it", Format::It},
    {"SD/test_music/s3m/2nd_reality.s3m", Format::S3m},
    {"SD/test_music/it/0700.it", Format::It},
    {"SD/test_music/xm/unreal_3.xm", Format::Xm},
    {"SD/test_music/xm/2djs10.xm", Format::Xm},
    {"SD/test_music/s3m/2nd_pm.s3m", Format::S3m},
    {"SD/test_music/it/00013.it", Format::It},
    {"SD/test_music/it/00012 ladda upp denna.it", Format::It},
    {"SD/test_music/xm/001.xm", Format::Xm},
    {"SD/test_music/it/038djzjack_littlerock.it", Format::It},
    {"SD/test_music/xm/000h_cara_mia.xm", Format::Xm},
    {"SD/test_music/it/bombls16.it", Format::It},
    {"SD/test_music/it/bz_ult9.it", Format::It},
    // Второй проход по части файлов: тот же трек, загруженный повторно после
    // нескольких других, ловит состояние, зависящее от того, что было
    // загружено перед ним.
    {"SD/test_music/mod/ptiswap.mod", Format::Mod},
    {"SD/test_music/it/00009.it", Format::It},
    {"SD/test_music/s3m/41096877.s3m", Format::S3m},
    // Последовательность, после которой плата не перевела плеер в finished на
    // втором 2nd_reality.s3m. filt_ace-light.it вне репозитория - без него
    // пара всё равно проверяется.
    {"music/src/it/smoke/filt_ace-light.it", Format::It},
    {"SD/test_music/s3m/2nd_reality.s3m", Format::S3m},
};

bool load_by_format(formats::ByteSource src, memory::TrackMemory& mem, soundsinth::model::Song& song, Format fmt,
                     const char** error_out) {
    switch (fmt) {
        case Format::Mod: return formats::mod::load(src, mem, song, error_out);
        case Format::S3m: return formats::s3m::load(src, mem, song, error_out);
        case Format::Xm: return formats::xm::load(src, mem, song, error_out);
        case Format::It: return formats::it::load(src, mem, song, error_out);
    }
    return false;
}

void test_sequential_loads_with_poison_fill() {
    std::printf("test_track_transition_stress_sequential_loads_with_poison_fill\n");

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    bool any_missing = false;
    int loaded = 0;
    int repeats_checked = 0;

    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); ++i) {
        const CorpusEntry& entry = kCorpus[i];
        std::ifstream in(entry.path, std::ios::binary);
        if (!in) {
            any_missing = true;
            continue;
        }
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        // Сброс перед загрузкой нового трека (кроме первого), а не после.
        // Заливка 0xDEADBEEF перезаписывает всё, что осталось от предыдущего
        // трека, до того, как новый загрузчик начнёт писать поверх.
        if (i > 0) {
            memory::track_memory_poison(mem);
            memory::track_memory_reset_for_new_track(mem);
        }

        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        soundsinth::model::Song song;
        const char* error = nullptr;
        const bool ok = load_by_format(mbs.as_byte_source(), mem, song, entry.format, &error);
        if (!ok) {
            std::printf("  [%zu] %s: load() failed: %s\n", i, entry.path, error ? error : "(no message)");
        }
        CHECK(ok);
        CHECK(!mem.psram.free_list_broken); // ни одна цепочка не возвращена дважды
        // Повтор файла: песня и байты сэмплов - те же, что у загрузки в свежую
        // память, иначе от прошлых треков что-то перешло.
        bool repeat = false;
        for (size_t j = 0; j < i; ++j) repeat = repeat || std::strcmp(kCorpus[j].path, entry.path) == 0;
        if (ok && repeat) {
            static memory::TrackMemory fresh;
            memory::track_memory_create(fresh);
            formats::MemoryByteSource fresh_src(file.data(), static_cast<uint32_t>(file.size()));
            soundsinth::model::Song fresh_song;
            const bool fresh_ok = load_by_format(fresh_src.as_byte_source(), fresh, fresh_song, entry.format, nullptr);
            CHECK(fresh_ok);
            if (fresh_ok) {
                song_compare::check_songs_equal(song, mem.psram, fresh_song, fresh.psram, &mem.sample_cache,
                                                &fresh.sample_cache);
                ++repeats_checked;
            }
            memory::track_memory_destroy(fresh);
        }
        if (ok) {
            CHECK(song.pattern_count >= 1);
            CHECK(song.order_count >= 1);
            for (uint16_t p = 0; p < song.pattern_count; ++p) {
                CHECK(song.patterns[p].psram_offset != soundsinth::model::Pattern::kInvalidOffset);
            }
            ++loaded;
        }
    }

    if (any_missing) {
        std::printf("  часть файлов корпуса не найдена — пропущены (запуск не из корня репозитория?)\n");
    }
    std::printf("  успешно загружено %d/%zu файлов подряд в одном TrackMemory, повторов сверено %d\n", loaded,
                sizeof(kCorpus) / sizeof(kCorpus[0]), repeats_checked);

    memory::track_memory_destroy(mem);
}

// Путь платы: run_session_load по метаданным, затем сэмплы по одному
// (load_track_sample), как префетч и фоновая догрузка. Сэмпл, который не
// лёг, - не провал трека, как на плате.
bool board_load(formats::ByteSource src, memory::TrackMemory& mem, soundsinth::model::Song& song,
                player::load::SessionLoadResult& result) {
    if (!player::load::run_session_load(src, mem, song, result, /*metadata_only=*/true)) return false;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        player::load::load_track_sample(result.format, src, mem, song, i);
    }
    return true;
}

// Тот же корпус путём платы, между загрузками через одну - резерв GS:
// хранилище ужимается на мегабайт и возвращается (psram_set_track_bytes
// пересобирает список страниц), как #30 плеера GS и следующая сессия платы.
// Повтор файла сверяется с загрузкой в свежую память тем же путём: песня,
// байты сэмплов, формат и длительность прохода.
void test_board_path_loads_with_reserve_flip() {
    std::printf("test_track_transition_stress_board_path_with_reserve_flip\n");

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    const uint32_t full = mem.psram.track_bytes;

    int loaded = 0;
    int repeats_checked = 0;
    int flips = 0;
    for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); ++i) {
        const CorpusEntry& entry = kCorpus[i];
        std::ifstream in(entry.path, std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        // Память трека сбрасывает run_session_load; заливка - чтобы указатель,
        // оставшийся от прошлого трека, читал мусор, а не старые данные.
        if (i > 0) memory::track_memory_poison(mem);
        if (i % 2 == 1) {
            memory::psram_set_track_bytes(mem.psram, full - memory::kGsReceiveBytes);
            memory::psram_set_track_bytes(mem.psram, full);
            ++flips;
        }

        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        soundsinth::model::Song song;
        player::load::SessionLoadResult result;
        const bool ok = board_load(mbs.as_byte_source(), mem, song, result);
        if (!ok) {
            std::printf("  [%zu] %s: run_session_load failed: %s\n", i, entry.path,
                        result.error ? result.error : "(no message)");
        }
        CHECK(ok);
        CHECK(!mem.psram.free_list_broken);
        if (!ok) continue;
        ++loaded;

        bool repeat = false;
        for (size_t j = 0; j < i; ++j) repeat = repeat || std::strcmp(kCorpus[j].path, entry.path) == 0;
        if (!repeat) continue;
        static memory::TrackMemory fresh;
        memory::track_memory_create(fresh);
        formats::MemoryByteSource fresh_src(file.data(), static_cast<uint32_t>(file.size()));
        soundsinth::model::Song fresh_song;
        player::load::SessionLoadResult fresh_result;
        const bool fresh_ok = board_load(fresh_src.as_byte_source(), fresh, fresh_song, fresh_result);
        CHECK(fresh_ok);
        if (fresh_ok) {
            CHECK(result.format == fresh_result.format);
            CHECK_EQ(result.total_frames, fresh_result.total_frames);
            song_compare::check_songs_equal(song, mem.psram, fresh_song, fresh.psram, &mem.sample_cache,
                                            &fresh.sample_cache);
            ++repeats_checked;
        }
        memory::track_memory_destroy(fresh);
    }
    std::printf("  путём платы загружено %d, повторов сверено %d, резерв GS туда и обратно %d раз\n", loaded,
                repeats_checked, flips);

    memory::track_memory_destroy(mem);
}

} // namespace

void run_track_transition_stress_tests() {
    test_sequential_loads_with_poison_fill();
    test_board_path_loads_with_reserve_flip();
}
