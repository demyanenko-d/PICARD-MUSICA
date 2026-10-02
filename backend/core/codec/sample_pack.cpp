// SPDX-License-Identifier: MIT
#include "core/codec/sample_pack.h"

#include <cstring>

#include "core/codec/block_locate.h"
#include "core/audio/saturate.h"

namespace soundsinth::sample_pack {

namespace {
// Кусок Dpcm8 не выходит за страницу данных, а страница начинается с
// отсчёта, кратного её размеру: точек на кусок не больше этого числа.
constexpr uint32_t kCheckpointsPerChunk = memory::kPsramPageBytes / dpcm8::kCheckpointIntervalSamples;
static_assert(memory::kPsramPageBytes % dpcm8::kCheckpointIntervalSamples == 0, "points are multiples of a page");
static_assert(memory::kPsramPageBytes % sizeof(dpcm8::Dpcm8Checkpoint) == 0, "a point does not cross a page");
} // namespace

SamplePacker::SamplePacker(memory::PsramStore& psram, ResidentEncoding mode, bool decimate) : psram_(psram), mode_(mode), decimate_(decimate) {}

bool SamplePacker::add_samples(const int16_t* samples, uint32_t count) {
    if (!decimate_) return pack(samples, count);

    // pack() берёт массив: средние пар копятся по 64.
    constexpr uint32_t kOutChunk = 64;
    int16_t out[kOutChunk];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!has_pending_) {
            pending_     = samples[i];
            has_pending_ = true;
            continue;
        }
        out[n++]     = static_cast<int16_t>((static_cast<int32_t>(pending_) + static_cast<int32_t>(samples[i])) / 2);
        has_pending_ = false;
        if (n == kOutChunk) {
            if (!pack(out, n)) return false;
            n = 0;
        }
    }
    return n == 0 || pack(out, n);
}

void SamplePacker::set_loop_unroll(soundsinth::model::LoopUnroll mode, uint32_t loop_start, uint32_t loop_end) {
    // Петля в два отсчёта и короче: обратного хода нет, разворот ничего не
    // меняет.
    const bool useful = loop_end > loop_start + 2u;
    unroll_           = useful ? mode : soundsinth::model::LoopUnroll::None;
    unroll_start_     = loop_start;
    unroll_end_       = loop_end;
}

bool SamplePacker::pack(const int16_t* samples, uint32_t count) {
    using soundsinth::model::LoopUnroll;
    while (count > 0) {
        uint32_t n = count;
        if (unroll_ != LoopUnroll::None) {
            if (total_samples_ == unroll_end_ && !write_unroll()) return false;
            if (unroll_ != LoopUnroll::None && total_samples_ + n > unroll_end_) n = unroll_end_ - total_samples_;
        }
        if (!write(samples, n)) return false;
        samples += n;
        count   -= n;
    }
    // Петля кончается вместе с куском: обратный проход сразу, хвоста может
    // и не быть.
    if (unroll_ != LoopUnroll::None && total_samples_ == unroll_end_) return write_unroll();
    return true;
}

void SamplePacker::read_back(uint32_t first, uint32_t count, int16_t* out) {
    const uint32_t bytes_per_sample = mode_ == ResidentEncoding::Raw16 ? 2u : 1u;
    // Dpcm8 распаковывается от контрольной точки блока, куда попал first:
    // она уже записана, first меньше total_samples_.
    uint32_t from = first;
    dpcm8::Dpcm8State st;
    if (mode_ == ResidentEncoding::Dpcm8) {
        from         = first - first % dpcm8::kCheckpointIntervalSamples;
        st.predictor = dpcm8::read_checkpoint(psram_, cp_first_page_, first / dpcm8::kCheckpointIntervalSamples).predictor;
    }
    uint32_t byte = from * bytes_per_sample;
    uint16_t page = memory::psram_page_advance(psram_, first_page_, byte / memory::kPsramPageBytes);
    uint32_t pos  = byte % memory::kPsramPageBytes;
    for (uint32_t i = from; i < first + count; ++i) {
        if (pos >= memory::kPsramPageBytes) {
            page = memory::psram_page_next(psram_, page);
            pos  = 0;
        }
        const uint8_t* p = memory::psram_page_ptr(psram_, page) + pos;
        int16_t v;
        switch (mode_) {
            case ResidentEncoding::Raw8:
                v = static_cast<int8_t>(p[0]);
                break;
            case ResidentEncoding::Raw16:
                v = static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
                break;
            default:
                v = dpcm8::decode_delta(p[0], st);
                break;
        }
        pos += bytes_per_sample;
        if (i >= first) out[i - first] = v;
    }
}

// Буфер ниже заполняет read_back чтением из PSRAM целиком, но анализатор
// туда не проходит и считает его неинициализированным - и здесь, и дальше,
// куда буфер уходит параметром.
// NOLINTBEGIN(clang-analyzer-core.uninitialized.Assign,clang-analyzer-core.CallAndMessage)
bool SamplePacker::write_unroll() {
    using soundsinth::model::LoopUnroll;
    const LoopUnroll mode = unroll_;
    unroll_               = LoopUnroll::None; // дальше пишется обычным путём
    // Кусками по 64 с конца петли: буфер на стеке Core1.
    constexpr uint32_t kChunk = 64;
    int16_t buf[kChunk];
    if (mode == LoopUnroll::Xm) {
        // Отражение на le: s[le-1] звучит лишний раз.
        read_back(unroll_end_ - 1u, 1u, buf);
        if (!write(buf, 1u)) return false;
    }
    // s[le-1] .. s[ls+1] в обратном порядке.
    uint32_t hi           = unroll_end_;
    const uint32_t lo_end = unroll_start_ + 1u;
    while (hi > lo_end) {
        const uint32_t n  = (hi - lo_end) < kChunk ? (hi - lo_end) : kChunk;
        const uint32_t lo = hi - n;
        read_back(lo, n, buf);
        for (uint32_t i = 0; i < n / 2u; ++i) {
            const int16_t t = buf[i];
            buf[i]          = buf[n - 1u - i];
            buf[n - 1u - i] = t;
        }
        if (!write(buf, n)) return false;
        hi = lo;
    }
    return true;
}

bool SamplePacker::write(const int16_t* samples, uint32_t count) {
    if (!ok_) return false;

    // Отсчёт кодируется прямо в страницу PSRAM, куском до её конца. У Raw16
    // страница и каждая запись - чётное число байт, поэтому значение не
    // разрывается границей страницы.
    static_assert(memory::kPsramPageBytes % 2 == 0, "a Raw16 sample does not cross a page");
    const uint32_t bytes_per_sample = mode_ == ResidentEncoding::Raw16 ? 2u : 1u;
    while (count > 0) {
        if (!ensure_page()) return false;
        uint8_t* dst         = memory::psram_page_ptr(psram_, current_page_) + page_pos_;
        const uint32_t room  = (memory::kPsramPageBytes - page_pos_) / bytes_per_sample;
        const uint32_t chunk = count < room ? count : room;

        switch (mode_) {
            case ResidentEncoding::Raw8:
                for (uint32_t i = 0; i < chunk; ++i) {
                    dst[i] = static_cast<uint8_t>(static_cast<int8_t>(clamp_i32(samples[i], -128, 127)));
                }
                break;
            case ResidentEncoding::Raw16:
                for (uint32_t i = 0; i < chunk; ++i) {
                    const uint16_t v = static_cast<uint16_t>(samples[i]);
                    dst[i * 2 + 0]   = static_cast<uint8_t>(v & 0xffu);
                    dst[i * 2 + 1]   = static_cast<uint8_t>(v >> 8);
                }
                break;
            case ResidentEncoding::Dpcm8: {
                dpcm8::Dpcm8Checkpoint points[kCheckpointsPerChunk];
                const uint32_t n = dpcm8::encode_block(samples, chunk, total_samples_, state_, reinterpret_cast<int8_t*>(dst), points, kCheckpointsPerChunk);
                if (!append_checkpoints(points, n)) return false;
                break;
            }
        }

        page_pos_      += chunk * bytes_per_sample;
        total_samples_ += chunk;
        samples        += chunk;
        count          -= chunk;
    }
    return true;
}
// NOLINTEND(clang-analyzer-core.uninitialized.Assign,clang-analyzer-core.CallAndMessage)

bool SamplePacker::ensure_page() {
    if (current_page_ != memory::kPageChainEnd && page_pos_ < memory::kPsramPageBytes) return true;
    if (!link_new_page(first_page_, current_page_)) return false;
    page_pos_ = 0;
    return true;
}

bool SamplePacker::link_new_page(uint16_t& first, uint16_t& current) {
    const uint16_t new_page = memory::psram_alloc_page(psram_);
    if (new_page == memory::kPageChainEnd) {
        ok_    = false;
        error_ = "PSRAM exhausted";
        return false;
    }
    if (current == memory::kPageChainEnd) {
        first = new_page;
    } else {
        memory::psram_set_next(psram_, current, new_page);
    }
    memory::psram_set_next(psram_, new_page, memory::kPageChainEnd);
    current = new_page;
    return true;
}

bool SamplePacker::append_checkpoints(const dpcm8::Dpcm8Checkpoint* points, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (cp_page_ == memory::kPageChainEnd || cp_pos_ >= memory::kPsramPageBytes) {
            if (!link_new_page(cp_first_page_, cp_page_)) return false;
            cp_pos_ = 0;
        }
        std::memcpy(memory::psram_page_ptr(psram_, cp_page_) + cp_pos_, &points[i], sizeof(points[i]));
        cp_pos_ += sizeof(points[i]);
        ++checkpoint_count_;
    }
    return true;
}

PackResult SamplePacker::finish() {
    // Непарный последний отсчёт при прореживании идёт без усреднения.
    if (decimate_ && has_pending_) {
        has_pending_ = false;
        pack(&pending_, 1);
    }
    // Поток не дошёл до конца петли: развёрнутого сэмпла не вышло.
    if (unroll_ != soundsinth::model::LoopUnroll::None && ok_) {
        ok_    = false;
        error_ = "the stream did not reach the end of the unrolled loop";
    }
    // Точки пришиваются за последней страницей данных и при отказе:
    // вызывающий освобождает одну цепочку. Точка есть только после
    // записанной страницы данных.
    if (cp_first_page_ != memory::kPageChainEnd) memory::psram_set_next(psram_, current_page_, cp_first_page_);

    PackResult r;
    r.first_page       = first_page_;
    r.checkpoint_count = checkpoint_count_;
    r.total_samples    = total_samples_;
    if (ok_) r.checkpoint_first_page = cp_first_page_;
    r.ok    = ok_;
    r.error = error_;
    return r;
}

bool finish_and_publish(SamplePacker& packer, bool data_ok, memory::PsramStore& psram, memory::SampleCacheCatalog& catalog, uint16_t sample_index,
                        const char** reason_out) {
    const PackResult result = packer.finish();
    // Ключ в каталоге пишется после finish(), последним: читатель видит
    // запись только с готовой цепочкой и таблицей точек.
    if (data_ok && result.ok && memory::sample_cache_alloc_slot(catalog, sample_index, result.first_page, result.checkpoint_first_page) != nullptr) {
        return true;
    }
    if (data_ok && reason_out) {
        *reason_out = result.ok ? memory::kSampleCatalogFull : result.error;
    }
    // Не опубликована - освобождается ровно один раз: точки в той же
    // цепочке (checkpoint_first_page - узел внутри first_page). Второе
    // освобождение замыкает список свободных в кольцо: страница уходит двум
    // сэмплам.
    memory::psram_free_chain(psram, result.first_page);
    return false;
}

} // namespace soundsinth::sample_pack
