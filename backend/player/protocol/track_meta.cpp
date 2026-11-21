#include "player/protocol/track_meta.h"

#include <cstring>

#include "core/engine/engine_defs.h"
#include "core/model/song.h"
#include "player/vgm/vgm.h"

namespace player::protocol {

namespace {

constexpr uint32_t kChunkBytes = 16;
constexpr uint32_t kTextMax    = 256; // 16 блоков: 0..E и F
constexpr uint32_t kChipBytes  = 16;
constexpr uint32_t kNameBytes  = 11;
// Теги 0x10-0x16 - номера строк GD3 1.00: английское название трека,
// автор, игра, система; дата, кто снял, примечание.
constexpr uint8_t kGd3Field[] = {0, 6, 2, 4, 8, 9, 10};
// Места строк метаданных по порядку: формат, числа, чипы, теги, программа.
constexpr uint8_t kSlotParams  = 1;
constexpr uint8_t kSlotChips   = 2;
constexpr uint8_t kSlotTags    = kSlotChips + vgm::kMaxChips;
constexpr uint8_t kSlotProgram = kSlotTags + sizeof(kGd3Field);
constexpr uint8_t kSlots       = kSlotProgram + 1;
// Строки до тегов - по блоку, тег и программа - до 16 блоков.
static_assert(kSlotTags + (kSlots - kSlotTags) * (kTextMax / kChunkBytes) <= 256, "the frame number fits a byte");

struct Record {
    uint8_t kind;
    uint32_t len;
    const vgm::ChipSpec* chip; // nullptr у чипа "MUSICA"
    uint8_t count;             // одинаковых чипов
    const uint8_t* text;       // UTF-16LE
    const char* bytes;         // 8 бит: CP437 трекеров, ASCII .mid
};

// Длина строки без '\0' и пробелов в конце.
uint32_t trimmed(const char* s, uint32_t max) {
    uint32_t n = 0;
    while (n < max && s[n] != '\0') {
        ++n;
    }
    while (n > 0 && s[n - 1] == ' ') {
        --n;
    }
    return n;
}

// Строка на месте slot; false - место пусто.
bool record_at(const TrackMeta& t, uint8_t slot, Record& r) {
    r = Record{};
    if (slot == 0) {
        r.kind = kMetaFormat;
        r.len  = 7;
        return true;
    }
    if (slot == kSlotParams) {
        if (t.song == nullptr) return false;
        r.kind = kMetaParams;
        r.len  = t.klass == kClassMidi ? 12 : 16;
        return true;
    }
    if (slot == kSlotProgram) {
        if (t.song == nullptr || (r.len = trimmed(t.song->program, sizeof(t.song->program))) == 0) return false;
        r.kind  = kMetaProgram;
        r.bytes = t.song->program;
        return true;
    }
    if (slot < kSlotTags) {
        const uint32_t i = slot - kSlotChips;
        if (t.vgm == nullptr) {
            if (i != 0) return false;
            r.kind  = kMetaChip;
            r.len   = kChipBytes;
            r.count = 1;
            return true;
        }
        // Строка на модель чипа: первый чип пары, номер - по порядку первых.
        const vgm::Info& info = t.vgm->info();
        if (i >= info.count || info.chips[i].second) return false;
        uint8_t n = 0;
        for (uint32_t k = 0; k < i; ++k) {
            n += info.chips[k].second ? 0 : 1;
        }
        r.kind  = static_cast<uint8_t>(kMetaChip + n);
        r.len   = kChipBytes;
        r.chip  = &info.chips[i];
        r.count = 1;
        for (uint32_t k = i + 1; k < info.count; ++k) {
            r.count += info.chips[k].kind == r.chip->kind && info.chips[k].second ? 1 : 0;
        }
        return true;
    }
    if (t.vgm == nullptr) {
        if (t.song == nullptr || slot != kSlotTags || (r.len = trimmed(t.song->title, sizeof(t.song->title))) == 0) return false;
        r.kind  = kMetaTagFirst;
        r.bytes = t.song->title;
        return true;
    }
    uint32_t units = 0;
    if (!t.vgm->tag_field(kGd3Field[slot - kSlotTags], &r.text, &units)) return false;
    r.kind = static_cast<uint8_t>(kMetaTagFirst + slot - kSlotTags);
    r.len  = units < kTextMax ? units : kTextMax;
    return true;
}

// Следующая непустая строка с места slot; false - строк больше нет.
bool next_record(const TrackMeta& t, uint8_t& slot, Record& r) {
    while (slot < kSlots) {
        if (record_at(t, slot++, r)) return true;
    }
    return false;
}

void put_bcd32(uint8_t* p, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i, v /= 100u) {
        p[i] = static_cast<uint8_t>((v / 10u % 10u) << 4 | v % 10u);
    }
}

// Байты строки с offset по n в out.
void fill(const TrackMeta& t, const Record& r, uint32_t offset, uint32_t n, uint8_t* out) {
    if (r.kind == kMetaFormat) {
        std::memcpy(out, t.format, sizeof(t.format));
        out[4] = t.klass;
        out[5] = static_cast<uint8_t>(t.version);
        out[6] = static_cast<uint8_t>(t.version >> 8);
        return;
    }
    if (r.kind == kMetaParams) {
        const soundsinth::model::Song& s = *t.song;
        const uint32_t tracker[]         = {s.channel_count, s.default_speed, s.default_tempo, s.order_count};
        const uint32_t midi[]            = {s.midi_header[0], s.midi_header[1], s.midi_header[2]};
        for (uint32_t i = 0; i < n / 4; ++i) {
            put_bcd32(out + 4 * i, t.klass == kClassMidi ? midi[i] : tracker[i]);
        }
        return;
    }
    if (r.bytes != nullptr) {
        for (uint32_t i = 0; i < n; ++i) {
            out[i] = cp866_from_cp437(static_cast<uint8_t>(r.bytes[offset + i]));
        }
        return;
    }
    if (r.text == nullptr) {
        const char* name = r.chip != nullptr ? vgm::part_name(*r.chip) : "MUSICA";
        std::strncpy(reinterpret_cast<char*>(out), name, kNameBytes);
        put_bcd32(out + kNameBytes, r.chip != nullptr ? r.chip->clock : soundsinth::engine::kSampleRateHz);
        out[kNameBytes + 4] = r.count;
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* u = r.text + 2u * (offset + i);
        out[i]           = cp866(static_cast<uint16_t>(u[0] | u[1] << 8));
    }
}

} // namespace

uint8_t cp866_from_cp437(uint8_t c) {
    if (c < 0x20) return ' ';
    if (c < 0x7f || (c >= 0xb0 && c < 0xe0)) return c;
    return '?';
}

uint8_t cp866(uint16_t unit) {
    if (unit < 0x20) return ' ';
    if (unit < 0x7f) return static_cast<uint8_t>(unit);
    if (unit >= 0x410 && unit < 0x440) return static_cast<uint8_t>(0x80 + (unit - 0x410));
    if (unit >= 0x440 && unit < 0x450) return static_cast<uint8_t>(0xe0 + (unit - 0x440));
    if (unit == 0x401) return 0xf0;
    if (unit == 0x451) return 0xf1;
    return '?';
}

bool track_meta_frame(void* user, uint8_t frame, uint8_t (&out)[HostProtocol::kTrackMetaBytes]) {
    const TrackMeta& t = *static_cast<const TrackMeta*>(user);
    uint8_t slot       = 0;
    Record r{};
    uint32_t chunk = frame;
    for (;;) {
        if (!next_record(t, slot, r)) return false;
        const uint32_t chunks = (r.len + kChunkBytes - 1) / kChunkBytes;
        if (chunk < chunks) break;
        chunk -= chunks;
    }
    const uint32_t offset = chunk * kChunkBytes;
    const uint32_t n      = r.len - offset < kChunkBytes ? r.len - offset : kChunkBytes;
    const bool last_chunk = offset + n == r.len;
    Record rest{};
    const bool last = last_chunk && !next_record(t, slot, rest);
    std::memset(out, 0, sizeof(out));
    out[0] = static_cast<uint8_t>(r.kind | (last ? HostProtocol::kTrackMetaLast : 0u));
    out[1] = static_cast<uint8_t>((last_chunk ? 0xf0u : chunk << 4) | (n - 1u));
    fill(t, r, offset, n, out + 2);
    return true;
}

} // namespace player::protocol
