#include "platform/memory.h"

#include <cstdint>
#include <cstdlib>

namespace platform {

// malloc выравнивает на alignof(std::max_align_t) = memory::kArenaBaseAlign.
uint8_t* resident_storage_acquire(size_t bytes) {
    return static_cast<uint8_t*>(std::malloc(bytes));
}

void resident_storage_release(uint8_t* storage) {
    std::free(storage);
}

uint8_t* psram_base_acquire(size_t total_bytes) {
    return static_cast<uint8_t*>(std::malloc(total_bytes));
}

void psram_base_release(uint8_t* base) {
    std::free(base);
}

uint8_t* psram_write_alias(uint8_t* p, size_t /*bytes*/) {
    return p;
}

} // namespace platform
