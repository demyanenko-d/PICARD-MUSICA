// SPDX-License-Identifier: MIT
#include "platform/memory.h"

#include <cstdint>

#include "hardware/sync.h" // __dsb, __isb
#include "pico.h"          // panic()

#include "core/memory/track_memory.h" // kResidentMetadataBytes, kArenaBaseAlign

#include "psram/psram_pins.h"

// Без кучи. Резидентная арена трека (64 КБ) - статический пул: отказ
// выделения из кучи виден только в работе и плохо (pico_malloc паникует
// "Out of memory" без указания, кто и сколько просил), а переполнение
// статического пула - ошибка линковки с конкретными байтами. Фрагментации
// нет, занятость одинакова с треком и без.
//
// PSRAM на МК - фиксированный адрес QMI XIP; QMI настраивает psram_init()
// один раз при старте (Core0, до multicore_launch_core1), здесь только
// отдаётся указатель.

namespace platform {

namespace {

alignas(soundsinth::memory::kArenaBaseAlign) uint8_t s_resident_pool[soundsinth::memory::kResidentMetadataBytes];
bool s_resident_taken = false;

} // namespace

uint8_t* resident_storage_acquire(size_t bytes) {
    // Пул один и берётся один раз: track_memory_create() вызывается однажды
    // (до multicore_launch_core1). Второй запрос или запрос крупнее
    // пула - ошибка бюджета; nullptr превратился бы в неработающую загрузку
    // трека без внятной причины.
    if (s_resident_taken || bytes > sizeof(s_resident_pool)) {
        panic("resident_storage_acquire: pool %zu B, requested %zu B, taken=%d", sizeof(s_resident_pool), bytes, static_cast<int>(s_resident_taken));
    }
    s_resident_taken = true;
    return s_resident_pool;
}

void resident_storage_release(uint8_t* /*storage*/) {
    // Пул статический. track_memory_destroy() на плате не вызывается;
    // повторный запрос упрётся в panic выше, а не выдаст чужую память.
}

// Размер не проверяется: чип один, его объём - psram::kSizeBytes.
uint8_t* psram_base_acquire(size_t /*total_bytes*/) {
    // QMI/XIP уже настроены psram_init() на Core0 до
    // track_memory_create() и запуска Core1; здесь отдаётся кэшируемый
    // write-back алиас.
    return reinterpret_cast<uint8_t*>(psram::kXipBase);
}

void psram_base_release(uint8_t* /*base*/) {
    // PSRAM не освобождается: QMI настроен на всё время работы.
}

// Строки выбрасываются по адресу (операция 2 окна обслуживания кэша), не
// по set/way. Кэш общий у обоих ядер.
uint8_t* __not_in_flash_func(psram_write_alias)(uint8_t* p, size_t bytes) {
    const uintptr_t offset                   = reinterpret_cast<uintptr_t>(p) - XIP_BASE;
    constexpr uintptr_t kInvalidateByAddress = 2u;
    constexpr uintptr_t kLineBytes           = 8u;
    for (uintptr_t o = offset; o < offset + bytes; o += kLineBytes) {
        *reinterpret_cast<volatile uint8_t*>(XIP_MAINTENANCE_BASE + o + kInvalidateByAddress) = 0;
    }
    __dsb();
    __isb();
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(p) + (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE));
}

} // namespace platform
