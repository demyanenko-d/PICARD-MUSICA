// SPDX-License-Identifier: MIT
#include "player/vgm/vgm_track.h"

#include <cstring>

#include "core/memory/arena.h"
#include "core/memory/psram_store.h"
#include "core/util/inflate.h"

namespace player::vgm {

namespace memory = soundsinth::memory;
namespace util   = soundsinth::util;

namespace {

constexpr uint32_t kGzipTail = 8; // CRC32 и длина распакованного в конце gzip

bool gzip(const uint8_t* head) {
    return head[0] == 0x1f && head[1] == 0x8b;
}

} // namespace

bool sniff(const uint8_t* head, uint32_t bytes) {
    return bytes >= 4 && (std::memcmp(head, "Vgm ", 4) == 0 || gzip(head));
}

Player* load_track(soundsinth::formats::ByteSource src, memory::TrackMemory& mem, const char** error, void (*service)(void*), void* user,
                   RhythmRomFn rhythm_rom) {
    auto fail = [error](const char* why) -> Player* {
        *error = why;
        return nullptr;
    };
    memory::track_memory_reset_for_new_track(mem);
    const uint32_t len = src.size(src.self);
    uint8_t head[4];
    if (!src.seek(src.self, 0) || src.read(src.self, head, sizeof(head)) != sizeof(head)) return fail("the file is shorter than a VGM header");
    const bool packed   = gzip(head);
    uint8_t* const file = packed ? memory::psram_temp_new<uint8_t>(mem.psram, len) : memory::psram_resident_new<uint8_t>(mem.psram, len);
    if (file == nullptr) return fail("the VGM file does not fit PSRAM");
    if (!src.seek(src.self, 0) || src.read(src.self, file, len) != len) return fail("the VGM file was not read to the end");

    const uint8_t* log = file;
    uint32_t size      = len;
    if (packed) {
        size_t at = 0;
        if (!util::gzip_parse(file, len, at, size)) return fail("the gzip header is damaged");
        uint8_t* const out = memory::psram_resident_new<uint8_t>(mem.psram, size);
        if (out == nullptr) return fail("the unpacked VGM log does not fit PSRAM");
        auto* const work = reinterpret_cast<util::InflateWork*>(memory::scratch_take(mem.scratch, memory::Scratch::Unpack, sizeof(util::InflateWork)));
        if (work == nullptr) return fail("no room for the unpacker tables");
        const size_t got = util::inflate(file + at, len - at - kGzipTail, out, size, *work, service, user);
        memory::scratch_leave(mem.scratch, memory::Scratch::Unpack);
        if (got != size) return fail("the gzip stream is damaged");
        log = out;
    }
    // Сжатый файл больше не нужен: временное уходит вместе с хвостом блока.
    memory::psram_freeze_pattern_zone(mem.psram);

    Info info;
    if (!parse(log, size, info)) return fail("not a VGM file");
    if (info.count == 0) return fail("the VGM file has no chips the player knows");
    if (rhythm_rom != nullptr) {
        for (uint32_t i = 0; i < info.count; i++) {
            if (info.chips[i].kind != Chip::Ym2608) continue;
            set_rhythm_rom(rhythm_rom(mem));
            break;
        }
    }
    const size_t need  = Player::memory_need(info);
    void* const chips  = memory::arena_alloc(mem.resident, need, 8);
    Player* const play = memory::arena_new<Player>(mem.resident);
    // Петля журнала идёт без конца: трек кончает кадр конца с затуханием.
    if (chips == nullptr || play == nullptr || !play->open(log, size, chips, need, looped(info) ? 0 : 1, service, user)) {
        return fail("no memory for the chips of the VGM file");
    }
    return play;
}

} // namespace player::vgm
