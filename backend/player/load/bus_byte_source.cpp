#include "player/load/bus_byte_source.h"

#include <algorithm>
#include <cstring>

#include "platform/hot_path.h"

// Весь файл в SRAM: read_fn и request_window крутят pump() всю загрузку трека.

namespace player::load {

SOUNDSINTH_HOT_PATH_ATTR("bbs_request_window")
void BusByteSource::request_window(uint32_t offset, uint32_t n) {
    // Прежнее окно лежит в том же буфере протокола, и новый запрос его
    // затрёт: с этой строки оно недействительно.
    window_len_ = 0;
    window_base_ = offset;
    protocol_.request_file_chunk(offset, static_cast<uint16_t>(n));
    uint16_t len = 0;
    while (!protocol_.take_received(len)) {
        if (aborted_) return;
        pump_(pump_user_);
    }
    window_ = protocol_.data_buffer();
    window_len_ = std::min<uint32_t>(len, player::protocol::HostProtocol::kDataBufferBytes);
}

SOUNDSINTH_HOT_PATH_ATTR("bbs_read_fn")
uint32_t BusByteSource::read_fn(void* self, void* dst, uint32_t n) {
    auto* src = static_cast<BusByteSource*>(self);
    if (src->aborted_) return 0;

    auto* out = static_cast<uint8_t*>(dst);
    uint32_t total = 0;

    while (total < n) {
        if (src->pos_ >= src->file_length_) break; // EOF: короткое чтение, контракт soundsinth::formats::ByteSource это допускает

        const bool in_window = src->pos_ >= src->window_base_ && src->pos_ < src->window_base_ + src->window_len_;
        if (in_window) {
            const uint32_t avail = (src->window_base_ + src->window_len_) - src->pos_;
            const uint32_t to_copy = std::min(n - total, avail);
            std::memcpy(out + total, src->window_ + (src->pos_ - src->window_base_), to_copy);
            src->pos_ += to_copy;
            total += to_copy;
            continue;
        }

        // Окно - от границы сектора вниз: невыровненный запрос идёт медленным путём,
        // хост сдвигается на 9 секторов, следующий запрос оказывается на 512 байт
        // назад, и хост перематывает файл с начала - O(N^2) на линейном чтении.
        // pos_ внутри запрошенного окна: base отстоит от него меньше чем на сектор.
        const uint32_t sector = src->protocol_.sector_size_bytes();
        const uint32_t base = sector ? (src->pos_ & ~(sector - 1u)) : src->pos_; // сектор - степень двойки
        const uint32_t want = std::min<uint32_t>(player::protocol::HostProtocol::kDataBufferBytes, src->file_length_ - base);
        src->request_window(base, want);

        // Окно не накрыло pos_: сброс, протокол сдался (длина 0) или хост
        // подтвердил меньше, чем pos_ - base. Отдаём что есть, а не просим
        // то же окно снова без конца; распаковка провалится, в логе видно
        // место.
        if (src->pos_ >= src->window_base_ + src->window_len_) break;
    }

    return total;
}

SOUNDSINTH_HOT_PATH_ATTR("bbs_seek_fn")
bool BusByteSource::seek_fn(void* self, uint32_t offset) {
    auto* src = static_cast<BusByteSource*>(self);
    if (offset > src->file_length_) return false;
    src->pos_ = offset;
    return true;
}

SOUNDSINTH_HOT_PATH_ATTR("bbs_size_fn")
uint32_t BusByteSource::size_fn(void* self) {
    return static_cast<BusByteSource*>(self)->file_length_;
}

} // namespace player::load
