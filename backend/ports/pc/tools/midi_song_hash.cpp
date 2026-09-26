// Хэш всего, что строит конвертер .mid (metadata_only): поля Song,
// инструменты (указатели - смещениями в арене), огибающие, keymap, сэмплы,
// паттерны и зона паттернов PSRAM целиком. Сторож рефакторинга загрузчика:
// перестройка кода обязана давать тот же хэш, правка поведения - менять его
// только у файлов, которых она касается.
//
// midi_song_hash <bank.ssb> <file.mid>...  -> строки "хэш файл"
// --check первым ключом: ещё и число ячеек, где у настоящей ноты в колонке
// эффекта NoteCut (такая нота звучит один тик), - строка "хэш файл cut=N".
// --content первым ключом: то же содержимое без смещений в арене и её
// заполнения - для перестройки, меняющей порядок выделений; заполнение арены
// печатается отдельно, строка "хэш файл arena=N".
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/bank/bank_reader.h"
#include "core/formats/midi.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

using namespace soundsinth;

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    v.resize(n > 0 ? static_cast<size_t>(n) : 0);
    if (n > 0 && std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    std::fclose(f);
    return v;
}

// FNV-1a, 64 бита.
struct Hash {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* q = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= q[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T> void add(T x) { bytes(&x, sizeof(x)); }
};

void hash_envelope(Hash& h, const soundsinth::model::Envelope* e) {
    if (e == nullptr) return;
    h.add(static_cast<bool>(e->enabled));
    h.add(static_cast<bool>(e->sustain_enabled));
    h.add(static_cast<bool>(e->loop_enabled));
    h.add(static_cast<bool>(e->carry));
    h.add(e->point_count);
    h.add(e->sustain_point);
    h.add(e->sustain_end);
    h.add(e->loop_start);
    h.add(e->loop_end);
    for (uint32_t k = 0; k < e->point_count; ++k) {
        h.add(e->points[k].tick);
        h.add(e->points[k].value);
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool check = argc > 1 && std::strcmp(argv[1], "--check") == 0;
    if (check) {
        --argc;
        ++argv;
    }
    const bool content = argc > 1 && std::strcmp(argv[1], "--content") == 0;
    if (content) {
        --argc;
        ++argv;
    }
    if (argc < 3) {
        std::fprintf(stderr, "midi_song_hash <bank.ssb> <file.mid>...\n");
        return 2;
    }
    std::vector<uint8_t> blob = read_file(argv[1]);
    bank::Bank bnk;
    const char* why = nullptr;
    if (!bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), bnk, &why)) {
        std::printf("bank: %s\n", why ? why : "?");
        return 1;
    }
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);
    for (int a = 2; a < argc; ++a) {
        const char* path = argv[a];
        std::vector<uint8_t> file = read_file(path);
        memory::track_memory_reset_for_new_track(mem);
        // Нули под зоной паттернов и в арене: хэш не должен зависеть от
        // того, что лежало там от прошлого файла.
        std::memset(mem.psram.base, 0, memory::kPsramChipBytes);
        std::memset(mem.resident.base, 0, mem.resident.capacity);
        formats::MemoryByteSource mbs(file.data(), static_cast<uint32_t>(file.size()));
        soundsinth::model::Song song;
        const char* err = nullptr;
        const bool ok = formats::midi::load(mbs.as_byte_source(), static_cast<uint32_t>(file.size()), mem, bnk, song,
                                            &err, true);
        Hash h;
        h.add(ok);
        if (err) h.bytes(err, std::strlen(err));
        const uint8_t* arena = mem.resident.base;
        auto off = [&](const void* p) -> uint64_t {
            if (content) return p ? 1u : 0u; // есть ли, без места
            return p ? static_cast<uint64_t>(static_cast<const uint8_t*>(p) - arena) : ~0ull;
        };
        if (ok) {
            h.add(song.channel_count);
            h.add(song.instrument_count);
            h.add(song.sample_count);
            h.add(song.order_count);
            h.add(song.pattern_count);
            h.add(song.default_speed);
            h.add(song.default_tempo);
            h.add(song.default_global_volume);
            h.add(song.reverb_enabled);
            h.add(song.quirks);
            h.add(song.sample_preamp);
            h.add(song.volume_ramp_samples);
            h.add(song.envelopes_in_real_time);
            h.add(off(song.instruments));
            h.add(off(song.samples));
            h.add(off(song.patterns));
            h.add(off(song.order));
            for (uint32_t i = 0; i < song.order_count; ++i) h.add(song.order[i]);
            for (uint32_t i = 0; i < song.pattern_count; ++i) {
                h.add(song.patterns[i].row_count);
                h.add(song.patterns[i].channel_count);
                h.add(song.patterns[i].psram_offset);
            }
            for (uint32_t i = 0; i < song.instrument_count; ++i) {
                const auto& in = song.instruments[i];
                h.add(off(in.note_to_sample_ranges));
                h.add(off(in.volume_envelope));
                h.add(off(in.filter_envelope));
                h.add(off(in.panning_envelope));
                h.add(off(in.pitch_envelope));
                h.add(in.fadeout_rate);
                h.add(in.default_sample_index);
                h.add(in.note_to_sample_range_count);
                h.add(in.global_volume);
                h.add(in.velocity_to_cutoff);
                h.add(in.filter_cutoff);
                h.add(in.filter_resonance);
                h.add(in.nna);
                h.add(in.dct);
                h.add(in.dca);
                h.add(in.instrument_panning);
                for (uint32_t k = 0; k < in.note_to_sample_range_count; ++k) {
                    const auto& r = in.note_to_sample_ranges[k];
                    h.add(r.start_note);
                    h.add(r.sample_index);
                    h.add(r.note_offset);
                }
                hash_envelope(h, in.volume_envelope);
                hash_envelope(h, in.filter_envelope);
            }
            for (uint32_t i = 0; i < song.sample_count; ++i) {
                const auto& d = song.samples[i];
                h.add(d.resident_encoding);
                h.add(d.length_samples);
                h.add(d.loop_enabled);
                h.add(d.loop_start);
                h.add(d.loop_end);
                h.add(d.c5_speed);
                h.add(d.default_volume);
                h.add(d.global_volume);
                h.add(d.default_panning);
                h.add(d.file_offset);
            }
            if (!content) h.add(static_cast<uint64_t>(mem.resident.offset));
        }
        h.add(mem.psram.pattern_bump_offset);
        h.bytes(mem.psram.base, mem.psram.pattern_bump_offset);
        if (content) {
            std::printf("%016llx %s arena=%u\n", static_cast<unsigned long long>(h.h), path,
                        static_cast<unsigned>(ok ? mem.resident.offset : 0));
            continue;
        }
        if (!check) {
            std::printf("%016llx %s\n", static_cast<unsigned long long>(h.h), path);
            continue;
        }
        uint32_t cut_on_note = 0;
        if (ok) {
            soundsinth::model::PatternCell cells[64];
            for (uint32_t pi = 0; pi < song.pattern_count; ++pi) {
                const soundsinth::model::Pattern& pat = song.patterns[pi];
                patterns::PatternReader reader(memory::psram_pattern_ptr(mem.psram, pat.psram_offset), pat.row_count,
                                               pat.channel_count);
                for (uint16_t r = 0; r < pat.row_count; ++r) {
                    reader.read_row(r, cells);
                    for (uint32_t c = 0; c < pat.channel_count; ++c) {
                        if (soundsinth::model::is_real_note(cells[c].note) &&
                            cells[c].effect.type == soundsinth::model::Effect::NoteCut) {
                            ++cut_on_note;
                        }
                    }
                }
            }
        }
        std::printf("%016llx %s cut=%u\n", static_cast<unsigned long long>(h.h), path, cut_on_note);
    }
    memory::track_memory_destroy(mem);
    return 0;
}
