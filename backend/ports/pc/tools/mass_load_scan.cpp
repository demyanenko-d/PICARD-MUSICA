// SPDX-License-Identifier: MIT
// Массовый прогон загрузчиков по реальной библиотеке: ловит падения и
// assert'ы на большом объёме. Не входит в автоматический набор
// soundsinth_tests: запускается вручную и зависит от внешних данных
// (каталог или FAT-образ), которых на другой машине может не быть.
//
// Два режима источника файлов:
//   --dir <путь>     - обычный каталог ОС, файлы читаются напрямую
//                      (MemoryByteSource); быстрее, но не проверяет
//                      FatFsByteSource.
//   --image <путь>   - FAT-образ (например build/sd.img), монтируется
//                      настоящим FatFs (как на плате), файлы читаются
//                      через FatFsByteSource - тот же путь доступа, что
//                      на МК. Можно указать несколько --image подряд,
//                      они обрабатываются по очереди (FatFs держит только
//                      один смонтированный том).
// Если не указано ни то, ни другое, по умолчанию --dir E:\ModArchive
// (совместимость с ранней версией инструмента).
//
// Использование:
//   mass_load_scan.exe --dir E:\ModArchive [--limit N]
//   mass_load_scan.exe --image E:\ModArchive\IT.img --image E:\ModArchive\MOD.img --image E:\ModArchive\S3M.img [--limit N]

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
// См. backend/ports/pc/app/main.cpp - ff.h на MSVC подключает <windows.h>
// внутри extern "C" { ... }.
#include <windows.h>
#endif
#include "ff.h"

#include "pc/fatfs_disk.h"
#include "core/formats/it.h"
#include "core/formats/midi.h"
#include "core/formats/mod.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"
#include "pc/fatfs_byte_source.h"
#include "core/formats/memory_byte_source.h"
#include "core/bank/bank_reader.h"
#include "core/engine/song_duration.h"
#include "core/memory/track_memory.h"
#include "core/memory/scratch_arena.h"
#include "core/codec/pattern_packer.h"

namespace fs = std::filesystem;
using namespace soundsinth;

namespace {

enum class Format { Mod, S3m, Xm, It, Midi, Unknown };

// Банк .mid: без него файлы .mid пропускаются (--bank не задан).
std::vector<uint8_t> g_bank_bytes;
bank::Bank g_bank;

// Расширение всегда ASCII, поэтому суффикс сравнивается побайтно, а не
// через fs::path::extension().string(): на Windows narrow string()
// конвертирует через текущую ANSI-кодовую страницу и бросает исключение
// на именах файлов вне неё (встретилось в E:\ModArchive\XM; это баг
// сканера, не загрузчика).
bool ends_with_ci(const std::string& path, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (path.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(path[path.size() - n + i])) != suffix[i]) return false;
    }
    return true;
}

Format format_from_extension(const std::string& path) {
    if (ends_with_ci(path, ".mod")) return Format::Mod;
    if (ends_with_ci(path, ".s3m")) return Format::S3m;
    if (ends_with_ci(path, ".xm")) return Format::Xm;
    if (ends_with_ci(path, ".it")) return Format::It;
    // .mid берётся только с банком: без него разбирать нечего.
    if (g_bank.valid() && (ends_with_ci(path, ".mid") || ends_with_ci(path, ".midi"))) return Format::Midi;
    return Format::Unknown;
}

bool load_by_format(formats::ByteSource src, uint32_t file_length, memory::TrackMemory& mem, soundsinth::model::Song& song, Format fmt,
                    const char** error_out) {
    switch (fmt) {
        case Format::Mod:
            return formats::mod::load(src, mem, song, error_out);
        case Format::S3m:
            return formats::s3m::load(src, mem, song, error_out);
        case Format::Xm:
            return formats::xm::load(src, mem, song, error_out);
        case Format::It:
            return formats::it::load(src, mem, song, error_out);
        // metadata_only: сэмплы банка в замер арены не входят, а читать их
        // на сотнях тысяч файлов незачем.
        case Format::Midi:
            return formats::midi::load(src, file_length, mem, g_bank, song, error_out, /*metadata_only=*/true);
        case Format::Unknown:
            return false;
    }
    return false;
}

const char* format_name(Format fmt) {
    switch (fmt) {
        case Format::Mod:
            return "MOD";
        case Format::S3m:
            return "S3M";
        case Format::Xm:
            return "XM";
        case Format::It:
            return "IT";
        case Format::Midi:
            return "MIDI";
        case Format::Unknown:
            return "?";
    }
    return "?";
}

struct Stats {
    uint64_t attempted               = 0;
    uint64_t per_format_attempted[5] = {};
    uint64_t per_format_ok[5]        = {};
    // Рекордсмены по буферам загрузчика: размер и файл.
    uint32_t pattern_bytes = 0, dict_bytes = 0, rows_bytes = 0, scratch_bytes = 0;
    std::string pattern_file, dict_file, rows_file, scratch_file;
    // Худшие по scratch файлы каждого формата, по убыванию.
    static constexpr size_t kScratchTop = 8;
    std::vector<std::pair<uint32_t, std::string>> scratch_top[5];

    // Арена метаданных и пик суммы "арена + буфер сценариев": по нему
    // выбирается размер общего пула, если их объединять. Сумма берётся с
    // ареной на конец загрузки - она только растёт, поэтому оценка сверху.
    uint32_t arena_bytes = 0, sum_bytes = 0;
    std::string arena_file, sum_file;
    std::vector<uint32_t> arena_hist, sum_hist; // по файлу, для процентилей
    // --per-file: строка на файл, пороги считаются снаружи.
    std::FILE* per_file = nullptr;
};

struct ScanState {
    memory::TrackMemory* mem;
    Stats* stats;
    std::vector<std::string>* failures;
    bool first                                 = true;
    bool stop                                  = false;
    uint64_t limit                             = 0;
    static constexpr size_t kMaxFailuresListed = 100;

    // Общая часть для обоих режимов: сброс с отравляющей заливкой перед
    // каждой загрузкой, кроме самой первой в прогоне - тот же протокол, что
    // в test_track_transition_stress.cpp, но на гораздо большем объёме.
    void process(const std::string& display_path, formats::ByteSource src, uint32_t file_length, Format fmt) {
        if (!first) {
            memory::track_memory_poison(*mem);
            memory::track_memory_reset_for_new_track(*mem);
        }
        first = false;

        soundsinth::model::Song song;
        const char* error             = nullptr;
        const uint32_t scratch_before = memory::g_max_scratch_used;
        memory::g_max_scratch_used    = 0;
        const uint32_t pattern_before = patterns::g_max_pattern_bytes;
        patterns::g_max_pattern_bytes = 0;
        const bool ok                 = load_by_format(src, file_length, *mem, song, fmt, &error);
        const uint32_t scratch_now    = memory::g_max_scratch_used;
        if (scratch_now < scratch_before) memory::g_max_scratch_used = scratch_before;

        // Пик сценариев этого файла: блок паттерна против постоянных по
        // размеру сценариев загрузки (перепаковка PCM, проход секвенсора).
        const uint32_t pattern_now = patterns::g_max_pattern_bytes;
        if (pattern_now < pattern_before) patterns::g_max_pattern_bytes = pattern_before;
        // Сценарии, живущие при уже полной арене, зависят от формата: у .mid
        // сэмплы идут из банка мимо перепаковки, зато ревербератор занимает
        // буфер всю игру. У трекерных наоборот - перепаковка есть,
        // ревербератора нет.
        uint32_t scratch_peak            = pattern_now;
        const uint32_t resident_scenario = fmt == Format::Midi ? memory::kPlayScratchBytes : memory::kSampleRepackBufferBytes;
        if (resident_scenario > scratch_peak) scratch_peak = resident_scenario;
        if (memory::kDurationPassBytes > scratch_peak) scratch_peak = memory::kDurationPassBytes;
        const uint32_t arena_now = static_cast<uint32_t>(memory::arena_used(mem->resident));
        const uint32_t sum_now   = arena_now + scratch_peak;

        const int fi = static_cast<int>(fmt);
        ++stats->attempted;
        ++stats->per_format_attempted[fi];
        auto record = [&](uint32_t now, uint32_t& best, std::string& file) {
            if (now > best) {
                best = now;
                file = display_path;
            }
        };
        record(patterns::g_max_pattern_bytes, stats->pattern_bytes, stats->pattern_file);
        record(patterns::g_max_dict_bytes, stats->dict_bytes, stats->dict_file);
        record(patterns::g_max_rows_bytes, stats->rows_bytes, stats->rows_file);
        record(memory::g_max_scratch_used, stats->scratch_bytes, stats->scratch_file);
        record(arena_now, stats->arena_bytes, stats->arena_file);
        record(sum_now, stats->sum_bytes, stats->sum_file);
        if (stats->per_file != nullptr) {
            // Паттерны - чтобы посчитать арену после ужатия их записи:
            // сейчас Pattern 8 байт плюс 2 байта порядка на каждый.
            // Длительность - проход секвенсора, тот же, что на плате; он
            // портит буфер сценариев, поэтому идёт последним.
            uint32_t seconds = 0;
            if (ok) {
                const uint32_t frames = engine::compute_song_total_frames(
                    song, mem->psram, memory::scratch_take(mem->scratch, memory::Scratch::DurationPass, memory::kDurationPassBytes),
                    memory::kDurationPassBytes);
                memory::scratch_leave(mem->scratch, memory::Scratch::DurationPass);
                seconds = frames / engine::kSampleRateHz;
            }
            std::fprintf(stats->per_file, "%s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%s\t%u\n", format_name(fmt), arena_now, sum_now,
                         static_cast<unsigned>(song.pattern_count), static_cast<unsigned>(song.instrument_count), seconds, mem->psram.pattern_bump_offset,
                         file_length, static_cast<int>(ok), display_path.c_str(), fmt == Format::Midi ? formats::midi::last_load_stats().events_bytes : 0u);
        }
        if (ok) {
            stats->arena_hist.push_back(arena_now);
            stats->sum_hist.push_back(sum_now);
        }
        auto& top = stats->scratch_top[fi];
        if (top.size() < Stats::kScratchTop || scratch_now > top.back().first) {
            top.emplace_back(scratch_now, display_path);
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (top.size() > Stats::kScratchTop) top.pop_back();
        }
        if (ok) {
            ++stats->per_format_ok[fi];
        } else if (failures->size() < kMaxFailuresListed) {
            failures->push_back(std::string(format_name(fmt)) + " " + display_path + ": " + (error ? error : "(no message)"));
        }

        if (stats->attempted % 2000 == 0) {
            std::printf("  ...%llu files processed\n", static_cast<unsigned long long>(stats->attempted));
        }
        if (limit != 0 && stats->attempted >= limit) stop = true;
    }
};

void scan_directory(const std::string& root, ScanState& state) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && !state.stop; it.increment(ec)) {
        if (ec) continue;
        if (!it->is_regular_file(ec)) continue;

        // .u8string(), а не .string(): на Windows string() конвертирует через
        // текущую ANSI-кодовую страницу и бросает исключение на именах вне её
        // диапазона (было падение на E:\ModArchive\XM\A\adj*.xm, баг сканера, не
        // загрузчика). u8string() для валидного UTF-16 из ОС не бросает.
        const std::string display_path = it->path().u8string();
        const Format fmt               = format_from_extension(display_path);
        if (fmt == Format::Unknown) continue;

        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (file.empty()) continue;

        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        state.process(display_path, mbs.as_byte_source(), static_cast<uint32_t>(file.size()), fmt);
    }
}

// Рекурсивно обходит смонтированный FatFs-том и вызывает state.process
// для каждого файла известного расширения - тот же обход, что
// walk_dir в backend/ports/pc/app/main.cpp, но с обработкой вместо печати.
void walk_fat_dir(const std::string& path, ScanState& state) {
    if (state.stop) return;
    DIR dir;
    if (f_opendir(&dir, path.c_str()) != FR_OK) return;
    for (;;) {
        if (state.stop) break;
        FILINFO fno;
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') break;
        const std::string full = path + "/" + fno.fname;
        if (fno.fattrib & AM_DIR) {
            walk_fat_dir(full, state);
        } else {
            const Format fmt = format_from_extension(full);
            if (fmt == Format::Unknown) continue;

            formats::FatFsByteSource fbs;
            if (!fbs.open(full.c_str())) continue;
            const formats::ByteSource fbsrc = fbs.as_byte_source();
            state.process(full, fbsrc, fbsrc.size(fbsrc.self), fmt);
        }
    }
    f_closedir(&dir);
}

bool scan_image(const std::string& image_path, ScanState& state) {
    if (!platform_pc::mount_disk_image(image_path.c_str())) {
        std::fprintf(stderr, "could not open the image: %s\n", image_path.c_str());
        return false;
    }
    static FATFS fs; // должен жить, пока том смонтирован; переиспользуется между образами (монтируется по одному)
    if (f_mount(&fs, "", 1) != FR_OK) {
        std::fprintf(stderr, "f_mount failed: %s\n", image_path.c_str());
        return false;
    }
    std::printf("scanning image %s...\n", image_path.c_str());
    walk_fat_dir("", state);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> dirs;
    std::vector<std::string> images;
    uint64_t limit            = 0; // 0 = без ограничения
    const char* per_file_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            dirs.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            images.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--per-file") == 0 && i + 1 < argc) {
            per_file_path = argv[++i];
        } else if (std::strcmp(argv[i], "--bank") == 0 && i + 1 < argc) {
            // Без банка .mid пропускаются: их разбор без него невозможен.
            std::ifstream bf(argv[++i], std::ios::binary);
            if (!bf) {
                std::fprintf(stderr, "bank does not open: %s\n", argv[i]);
                return 1;
            }
            g_bank_bytes.assign(std::istreambuf_iterator<char>(bf), std::istreambuf_iterator<char>());
            const char* berr = nullptr;
            if (!bank::bank_open(g_bank_bytes.data(), static_cast<uint32_t>(g_bank_bytes.size()), g_bank, &berr)) {
                std::fprintf(stderr, "bank rejected: %s\n", berr ? berr : "(no reason)");
                return 1;
            }
            std::printf("bank: %s, samples %u\n", g_bank.header->name, unsigned(g_bank.header->sample_count));
        } else {
            dirs.push_back(argv[i]); // обратная совместимость: голый путь = каталог
        }
    }
    if (dirs.empty() && images.empty()) {
        dirs.push_back("E:\\ModArchive");
    }

    memory::TrackMemory mem;
    memory::track_memory_create(mem);

    Stats stats;
    if (per_file_path != nullptr) {
        stats.per_file = std::fopen(per_file_path, "wb");
        if (stats.per_file == nullptr) {
            std::fprintf(stderr, "cannot open the per-file output: %s\n", per_file_path);
            return 1;
        }
    }
    std::vector<std::string> failures;
    ScanState state{&mem, &stats, &failures, true, false, limit};

    const auto start_time = std::chrono::steady_clock::now();

    for (const auto& d : dirs) {
        if (state.stop) break;
        if (!fs::exists(d)) {
            std::fprintf(stderr, "directory not found: %s\n", d.c_str());
            continue;
        }
        std::printf("scanning directory %s%s...\n", d.c_str(), limit ? " (with a limit)" : "");
        scan_directory(d, state);
    }
    for (const auto& img : images) {
        if (state.stop) break;
        if (!fs::exists(img)) {
            std::fprintf(stderr, "image not found: %s\n", img.c_str());
            continue;
        }
        scan_image(img, state);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();

    // Раскладка: хранилище трека - весь чип (8 МБ); до 2026-09-13 умолчание
    // было 7 МБ, итоги прежних прогонов с этими не сравниваются.
    std::printf("\n=== Total (%lld s), track storage 8 MB ===\n", static_cast<long long>(elapsed));
    for (int fi = 0; fi < 5; ++fi) {
        const Format fmt = static_cast<Format>(fi);
        if (stats.per_format_attempted[fi] == 0) continue;
        std::printf("%s: %llu/%llu ok\n", format_name(fmt), static_cast<unsigned long long>(stats.per_format_ok[fi]),
                    static_cast<unsigned long long>(stats.per_format_attempted[fi]));
    }
    std::printf("Total: %llu\n", static_cast<unsigned long long>(stats.attempted));
    std::printf("pattern buffer: block %u B (%s), dictionary %u B (%s), rows %u B (%s); scratch %u B (%s)\n", stats.pattern_bytes, stats.pattern_file.c_str(),
                stats.dict_bytes, stats.dict_file.c_str(), stats.rows_bytes, stats.rows_file.c_str(), stats.scratch_bytes, stats.scratch_file.c_str());

    // Арена и пик суммы: по ним выбирается размер общего пула, если арену и
    // буфер сценариев объединять. Печатаются процентили, а не только
    // рекорд: по рекорду буфер не выбирают.
    auto percentiles = [](const char* name, std::vector<uint32_t>& v, uint32_t worst, const char* worst_file) {
        if (v.empty()) return;
        std::sort(v.begin(), v.end());
        auto at = [&](double q) { return v[static_cast<size_t>(q * (v.size() - 1))]; };
        std::printf("%s: median %u, 99%% %u, 99.9%% %u, 99.99%% %u, maximum %u (%s)\n", name, at(0.50), at(0.99), at(0.999), at(0.9999), worst, worst_file);
    };
    percentiles("arena", stats.arena_hist, stats.arena_bytes, stats.arena_file.c_str());
    percentiles("arena+scratch", stats.sum_hist, stats.sum_bytes, stats.sum_file.c_str());

    for (int fi = 0; fi < 5; ++fi) {
        if (stats.scratch_top[fi].empty()) continue;
        std::printf("scratch %s:", format_name(static_cast<Format>(fi)));
        for (const auto& t : stats.scratch_top[fi])
            std::printf(" %u (%s)", t.first, t.second.c_str());
        std::printf("\n");
    }

    if (!failures.empty()) {
        std::printf("\nFirst %zu failures:\n", failures.size());
        for (const auto& f : failures)
            std::printf("  %s\n", f.c_str());
    }

    if (stats.per_file != nullptr) std::fclose(stats.per_file);
    memory::track_memory_destroy(mem);
    return 0;
}
