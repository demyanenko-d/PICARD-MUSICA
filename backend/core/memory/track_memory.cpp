// SPDX-License-Identifier: MIT
#include "core/memory/track_memory.h"

#include <cstring>

#include "platform/memory.h"
#include "core/model/song.h"

namespace soundsinth::memory {

// Типы резидентной арены: выравнивание не строже базы.
static_assert(alignof(soundsinth::model::Instrument) <= kArenaBaseAlign && alignof(soundsinth::model::SampleDescriptor) <= kArenaBaseAlign &&
                  alignof(soundsinth::model::Pattern) <= kArenaBaseAlign && alignof(soundsinth::model::Envelope) <= kArenaBaseAlign &&
                  alignof(soundsinth::model::KeymapRange) <= kArenaBaseAlign,
              "the arena type is aligned stricter than the base");

void track_memory_create(TrackMemory& mem) {
    arena_init(mem.resident, platform::resident_storage_acquire(kResidentMetadataBytes), kResidentMetadataBytes);
    // Сценарии живут на вершине того же пула: своей памяти у них нет.
    mem.scratch.arena  = &mem.resident;
    mem.scratch.tenant = Scratch::None;
    psram_create(mem.psram);
    sample_cache_reset(mem.sample_cache);
    mem.bank_table = nullptr;
}

void track_memory_destroy(TrackMemory& mem) {
    platform::resident_storage_release(mem.resident.base);
    mem.resident = Arena{};
    psram_destroy(mem.psram);
}

void track_memory_poison(TrackMemory& mem) {
    poison_words(mem.resident.base, arena_used(mem.resident));
    psram_poison_track(mem.psram);
}

void track_memory_reset_for_new_track(TrackMemory& mem) {
    arena_reset(mem.resident);
    mem.bank_table = nullptr;

    // Буфер сценариев не трогается, только освобождается: движок прошлого
    // трека снесён, следующий загрузчик перезапишет содержимое раньше, чем
    // его кто-то прочитает.
    mem.scratch.tenant = Scratch::None;

    sample_cache_reset(mem.sample_cache);
    psram_reset_track(mem.psram);
}

} // namespace soundsinth::memory
