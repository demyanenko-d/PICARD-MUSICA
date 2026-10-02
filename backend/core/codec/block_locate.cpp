// SPDX-License-Identifier: MIT
#include "core/codec/block_locate.h"

#include <cstring>

namespace soundsinth::dpcm8 {

namespace {
// Байт на отсчёт, блок - ровно kCheckpointIntervalSamples байт; 4 блока
// на страницу (1024/256), блок не пересекает страницу.
constexpr uint32_t kBlockDpcm8Bytes    = kCheckpointIntervalSamples;
constexpr uint32_t kBlocksPerDpcm8Page = memory::kPsramPageBytes / kBlockDpcm8Bytes;
constexpr uint32_t kCheckpointsPerPage = memory::kPsramPageBytes / sizeof(Dpcm8Checkpoint);
static_assert(memory::kPsramPageBytes % kBlockDpcm8Bytes == 0, "a Dpcm8 block does not cross a page");
static_assert(memory::kPsramPageBytes % sizeof(Dpcm8Checkpoint) == 0, "a point does not cross a page");
} // namespace

Dpcm8Checkpoint read_checkpoint(memory::PsramStore& psram, uint16_t checkpoint_first_page, uint32_t block_index) {
    const uint32_t page_offset   = block_index / kCheckpointsPerPage;
    const uint32_t index_in_page = block_index % kCheckpointsPerPage;
    const uint16_t page          = memory::psram_page_advance(psram, checkpoint_first_page, page_offset);
    Dpcm8Checkpoint cp{};
    std::memcpy(&cp, memory::psram_page_ptr(psram, page) + index_in_page * sizeof(Dpcm8Checkpoint), sizeof(Dpcm8Checkpoint));
    return cp;
}

BlockPosition locate_block(memory::PsramStore& psram, uint16_t first_page, uint16_t checkpoint_first_page, uint32_t block_index) {
    BlockPosition pos;
    if (block_index != 0 && checkpoint_first_page != memory::kPageChainEnd) {
        const Dpcm8Checkpoint cp = read_checkpoint(psram, checkpoint_first_page, block_index);
        pos.state.predictor      = cp.predictor;
    } // block_index == 0 или точек нет: старт с Dpcm8State{}, уже в pos.state

    const uint32_t page_offset   = block_index / kBlocksPerDpcm8Page;
    const uint32_t block_in_page = block_index % kBlocksPerDpcm8Page;
    pos.page                     = memory::psram_page_advance(psram, first_page, page_offset);
    pos.byte_offset              = static_cast<uint16_t>(block_in_page * kBlockDpcm8Bytes);
    return pos;
}

} // namespace soundsinth::dpcm8
