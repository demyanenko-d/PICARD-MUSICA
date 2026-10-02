// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <fstream>
#include <vector>

#ifdef _WIN32
// См. backend/ports/pc/app/main.cpp и backend/ports/pc/fatfs_byte_source.cpp:
// ff.h на MSVC подключает <windows.h> внутри extern "C" { ... }.
#include <windows.h>
#endif
#include "ff.h"

#include "pc/fatfs_disk.h"
#include "core/formats/mod.h"
#include "pc/fatfs_byte_source.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "song_compare.h"

using namespace soundsinth;

namespace {

// Результат загрузки через FatFsByteSource (образ build/sd.img, тот же
// путь, что на SD-карте, см. platform_pc::mount_disk_image) должен
// побитно совпасть с загрузкой того же файла напрямую через
// MemoryByteSource - это доказывает, что formats::ByteSource взаимозаменяем
// на практике.
void test_fatfs_matches_memory_bytesource() {
    std::printf("test_fatfs_mod_matches_memory_bytesource\n");

    if (!platform_pc::mount_disk_image("build/sd.img")) {
        std::printf("  build/sd.img not found - SKIP (build it: scripts\\build_sd.bat)\n");
        return;
    }
    static FATFS fs;
    if (f_mount(&fs, "", 1) != FR_OK) {
        std::printf("  f_mount(build/sd.img) failed - SKIP\n");
        return;
    }

    formats::FatFsByteSource fatfs_src;
    CHECK(fatfs_src.open("/test_music/mod/ptiswap.mod"));

    std::ifstream in("SD/test_music/mod/ptiswap.mod", std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    formats::MemoryByteSource mem_src(file.data(), static_cast<uint32_t>(file.size()));

    memory::TrackMemory mem_a;
    memory::track_memory_create(mem_a);
    memory::TrackMemory mem_b;
    memory::track_memory_create(mem_b);

    soundsinth::model::Song song_a;
    const char* error_a = nullptr;
    const bool ok_a     = formats::mod::load(fatfs_src.as_byte_source(), mem_a, song_a, &error_a);
    if (!ok_a) std::printf("  FatFsByteSource load() failed: %s\n", error_a ? error_a : "(no message)");
    CHECK(ok_a);

    soundsinth::model::Song song_b;
    const char* error_b = nullptr;
    const bool ok_b     = formats::mod::load(mem_src.as_byte_source(), mem_b, song_b, &error_b);
    if (!ok_b) std::printf("  MemoryByteSource load() failed: %s\n", error_b ? error_b : "(no message)");
    CHECK(ok_b);

    if (ok_a && ok_b) {
        song_compare::check_songs_equal(song_a, mem_a.psram, song_b, mem_b.psram);
    }

    memory::track_memory_destroy(mem_a);
    memory::track_memory_destroy(mem_b);
}

} // namespace

void run_fatfs_mod_tests() {
    test_fatfs_matches_memory_bytesource();
}
