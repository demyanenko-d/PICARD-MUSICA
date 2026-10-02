// SPDX-License-Identifier: MIT
#include "core/bank/bank_reader.h"

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/memory.h"

namespace soundsinth::bank {

namespace {

inline uint32_t load_le32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

// Дочитывание без ветвлений: слово кладётся над непрочитанными битами,
// указатель сдвигается на целые байты, что в него вошли, - в буфере
// становится 24..31 бит, хватает на два символа (до 9 бит каждый). Байты
// сверх счёта кладутся повторно на те же места, ИЛИ их не портит. Четыре
// байта собираются в слово и пишутся одной записью.
//
// Дочитывание сдвигает p не больше чем на 3 байта: перед четвёркой нужно 6
// байт до limit, перед одиночным - 3. По одному - только последние меньше
// четырёх байт куска: иначе распаковано кратно 4, и запись в dst идёт
// выровненными словами.
uint32_t SOUNDSINTH_HOT_PATH(bank_decode_block)(const BankDecodeTable& t, const uint8_t*& p_io, const uint8_t* limit, uint8_t* dst, uint32_t n, TansState& st) {
    static_assert(2u * kModelStateBits <= 24u, "two symbols per refill");
    const uint32_t* entry = t.entry;
    const uint8_t* p      = p_io;
    uint32_t x = st.x, bits = st.bits, cnt = st.cnt;
    auto refill = [&]() {
        bits |= load_le32(p) << cnt;
        p    += (31u - cnt) >> 3;
        cnt  |= 24u;
    };
    auto step = [&]() {
        const uint32_t e    = entry[x];
        const uint32_t nb   = (e >> 8) & 0xfu;
        x                   = (e >> 16) + (bits & ((1u << nb) - 1u));
        bits              >>= nb;
        cnt                -= nb;
        return e & 0xffu;
    };
    uint32_t i = 0;
    for (; i + 4 <= n && limit - p >= 6; i += 4) {
        refill();
        const uint32_t s0 = step();
        const uint32_t s1 = step();
        refill();
        const uint32_t s2 = step();
        const uint32_t s3 = step();
        const uint32_t w  = s0 | (s1 << 8) | (s2 << 16) | (s3 << 24);
        std::memcpy(dst + i, &w, sizeof(w));
    }
    for (; i < n && n - i < 4 && limit - p >= 3; ++i) {
        refill();
        dst[i] = static_cast<uint8_t>(step());
    }
    st   = TansState{x, bits, cnt};
    p_io = p;
    return i;
}

namespace {

// Входной буфер распаковщика. Сэмплы банка распаковывает только Core1,
// по одному.
uint8_t s_input[kBankInputBufferBytes];

} // namespace

bool bank_open(const uint8_t* blob, uint32_t bytes, Bank& out, const char** error_out, bool tables_only) {
    auto fail = [&](const char* why) {
        if (error_out) *error_out = why;
        out = Bank{};
        return false;
    };
    if (!blob || bytes < sizeof(BankHeader)) return fail("bank is shorter than its header");

    const BankHeader* h = reinterpret_cast<const BankHeader*>(blob);
    if (h->magic != kMagic) return fail("not a bank: signature mismatch");
    if (h->version != kVersion) return fail("wrong bank format version");
    if ((tables_only ? h->pcm_offset : h->total_bytes) > bytes) return fail("bank is truncated");
    if (h->pcm_offset < sizeof(BankHeader)) return fail("bank tables before the end of the header");
    if (static_cast<uint64_t>(h->pcm_offset) + h->pcm_bytes > h->total_bytes) {
        return fail("PCM runs past the bank boundary");
    }
    // Поля заголовка CRC не защищает: каждая таблица - внутри области
    // таблиц, между заголовком и PCM.
    auto table_fits = [h](uint32_t offset, uint64_t table_bytes) {
        return offset >= sizeof(BankHeader) && offset <= h->pcm_offset && table_bytes <= h->pcm_offset - offset;
    };
    if (!table_fits(h->presets_offset, static_cast<uint64_t>(kPresetSlots) * sizeof(BankPreset)) ||
        !table_fits(h->layers_offset, static_cast<uint64_t>(h->layer_count) * sizeof(BankLayer)) ||
        !table_fits(h->instruments_offset, static_cast<uint64_t>(h->instrument_count) * sizeof(BankInstrument)) ||
        !table_fits(h->envelopes_offset, static_cast<uint64_t>(h->envelope_count) * sizeof(BankEnvelope)) ||
        !table_fits(h->keymap_offset, static_cast<uint64_t>(h->keymap_count) * sizeof(BankKeymapRange)) ||
        !table_fits(h->samples_offset, static_cast<uint64_t>(h->sample_count) * sizeof(BankSample)) || !table_fits(h->model_offset, sizeof(BankModel))) {
        return fail("bank table outside the table area");
    }

    const uint32_t table_bytes = h->pcm_offset - static_cast<uint32_t>(sizeof(BankHeader));
    if (bank_crc(blob + sizeof(BankHeader), table_bytes) != h->table_crc32) return fail("table crc mismatch");
    // Сумма частот контекста - число состояний: на ней держится таблица
    // распаковки.
    const BankModel& model = *reinterpret_cast<const BankModel*>(blob + h->model_offset);
    if (!model_valid(model)) return fail("bank compression model is corrupt");
    const BankSample* samples = reinterpret_cast<const BankSample*>(blob + h->samples_offset);
    bool packed               = false;
    for (uint32_t i = 0; i < h->sample_count && !packed; ++i) {
        packed = (samples[i].flags & kSamplePackedBit) != 0;
    }

    out.base        = blob;
    out.header      = h;
    out.presets     = reinterpret_cast<const BankPreset*>(blob + h->presets_offset);
    out.layers      = reinterpret_cast<const BankLayer*>(blob + h->layers_offset);
    out.instruments = reinterpret_cast<const BankInstrument*>(blob + h->instruments_offset);
    out.envelopes   = reinterpret_cast<const BankEnvelope*>(blob + h->envelopes_offset);
    out.keymap      = reinterpret_cast<const BankKeymapRange*>(blob + h->keymap_offset);
    out.samples     = reinterpret_cast<const BankSample*>(blob + h->samples_offset);
    out.model       = &model;
    out.packed      = packed;
    out.pcm         = tables_only ? nullptr : blob + h->pcm_offset;
    if (error_out) *error_out = nullptr;
    return true;
}

// Общая часть обеих веток: цепочка страниц набивается распаковщиком,
// откуда бы он ни брал байты.
namespace {

// Цепочка сэмпла; kPageChainEnd - не влез или отказ чтения.
struct Pages {
    uint16_t first            = memory::kPageChainEnd;
    uint16_t checkpoint_first = memory::kPageChainEnd; // kPageChainEnd - точек нет
    bool read_failed          = false;
};

// Не встраивается: иначе цикл распаковки встаёт копией в обе ветки.
SOUNDSINTH_NOINLINE Pages fill_pages(const Bank& b, const BankSample& s, BankUnpacker& unpacker, memory::PsramStore& psram) {
    const uint32_t total = s.pcm_bytes;
    // Контрольные точки лежат с новой страницы, их первая страница
    // вычисляется арифметикой. Без точек индекс пришёлся бы на хвост тела.
    const bool has_checkpoints           = s.checkpoint_count != 0;
    const uint32_t body_padded           = total - static_cast<uint32_t>(s.checkpoint_count) * static_cast<uint32_t>(sizeof(dpcm8::Dpcm8Checkpoint));
    const uint32_t checkpoint_page_index = body_padded / memory::kPsramPageBytes;

    Pages out;
    uint16_t first   = memory::kPageChainEnd;
    uint16_t prev    = memory::kPageChainEnd;
    uint32_t done    = 0;
    uint32_t page_no = 0;
    while (done < total) {
        if (b.serve) b.serve(b.serve_user);
        const uint16_t page = memory::psram_alloc_page(psram);
        if (page == memory::kPageChainEnd) {
            // Не влезло - вернуть занятое, половина сэмпла не нужна.
            if (first != memory::kPageChainEnd) memory::psram_free_chain(psram, first);
            return Pages{};
        }
        if (first == memory::kPageChainEnd) {
            first = page;
        } else {
            memory::psram_set_next(psram, prev, page);
        }
        memory::psram_set_next(psram, page, memory::kPageChainEnd);

        const uint32_t chunk = total - done < memory::kPsramPageBytes ? total - done : memory::kPsramPageBytes;
        // Страница новая, её никто не читает: запись мимо кэша.
        unpacker.decode(platform::psram_write_alias(memory::psram_page_ptr(psram, page), memory::kPsramPageBytes), chunk);
        if (unpacker.failed()) {
            // Отказ чтения: вместо сэмпла была бы полка шкалы. Сэмпл молчит.
            memory::psram_free_chain(psram, first);
            out.read_failed = true;
            return out;
        }
        if (has_checkpoints && page_no == checkpoint_page_index) out.checkpoint_first = page;
        prev  = page;
        done += chunk;
        ++page_no;
    }
    out.first = first;
    return out;
}

} // namespace

uint16_t bank_make_resident(const Bank& b, const BankDecodeTable* table, uint16_t sample_index, memory::PsramStore& psram, uint16_t* checkpoint_first_page_out,
                            bool* read_failed_out) {
    if (checkpoint_first_page_out) *checkpoint_first_page_out = memory::kPageChainEnd;
    if (read_failed_out) *read_failed_out = false;
    if (!b.valid() || sample_index >= b.header->sample_count) return memory::kPageChainEnd;

    const BankSample& s = b.samples[sample_index];
    if (s.pcm_bytes == 0) return memory::kPageChainEnd;
    const BankDecodeTable* run_table = (s.flags & kSamplePackedBit) ? table : nullptr;
    if ((s.flags & kSamplePackedBit) && table == nullptr) return memory::kPageChainEnd;
    // Место - до распаковки: fill_pages берёт по странице на каждые
    // kPsramPageBytes прогона, и отказ на середине распаковывал бы зря, а
    // повтор после вытеснения - ещё раз.
    const uint32_t need_pages = (s.pcm_bytes + memory::kPsramPageBytes - 1u) / memory::kPsramPageBytes;
    if (memory::psram_free_page_count(psram) < need_pages) return memory::kPageChainEnd;

    // Предел - конец прогона и не дальше конца зоны PCM.
    const uint32_t zone_left = b.header->pcm_bytes - s.pcm_offset;
    const uint32_t packed    = s.pcm_packed_bytes < zone_left ? s.pcm_packed_bytes : zone_left;
    Pages pages;
    if (b.pcm) {
        BankUnpacker unpacker(run_table, b.pcm + s.pcm_offset, packed, s_input);
        pages = fill_pages(b, s, unpacker, psram);
    } else if (b.pcm_source.read) {
        BankUnpacker unpacker(run_table, &b.pcm_source, s.pcm_offset, packed, s_input);
        pages = fill_pages(b, s, unpacker, psram);
    }
    if (checkpoint_first_page_out) *checkpoint_first_page_out = pages.checkpoint_first;
    if (read_failed_out) *read_failed_out = pages.read_failed;
    return pages.first;
}

} // namespace soundsinth::bank
