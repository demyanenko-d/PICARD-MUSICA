// Достаёт из банка полсотни разных сэмплов в том виде, в каком они лежат
// в PSRAM на плате, то есть резидентным потоком Dpcm8 (или Raw8).
//
//   bank_export <банк.ssb> <каталог> [--count 50] [--dpcm8-only]
//
// Почему отдельный инструмент, а не скрипт: прогоны PCM в банке сжаты
// моделью (bank_codec.h), без распаковщика из них ничего не достать, а
// JS-скрипты замеров читают только Raw8. Здесь используется тот же путь,
// что на плате - bank_make_resident(): выделение цепочки страниц и
// потоковая распаковка в неё.
//
// Что пишется:
//   <каталог>/NNN_prog<P>_<имя GM>_n<нота>.dpcm8  - резидентный прогон
//       целиком (для ударных drum<P>_kit вместо prog<P>_<имя GM>)
//   <каталог>/manifest.csv  - параметры каждого
//
// Прогон целиком - это тело + добивка до страницы + чекпоинты Dpcm8.
// Где кончается тело, сказано в манифесте (length_samples): чекпоинты -
// служебные точки для перемотки, к самой волне они не относятся.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/bank/bank_reader.h"
#include "core/model/instrument.h"
#include "core/memory/track_memory.h"

using namespace soundsinth;

namespace {

const char* kGm[128] = {
    "AcGrand", "BrGrand", "ElGrand", "HonkyTonk", "ElPiano1", "ElPiano2", "Harpsichord", "Clavi",
    "Celesta", "Glockenspiel", "MusicBox", "Vibraphone", "Marimba", "Xylophone", "TubBells", "Dulcimer",
    "DrawbarOrg", "PercOrgan", "RockOrgan", "ChurchOrg", "ReedOrgan", "Accordion", "Harmonica", "TangoAcc",
    "AcGuitarNy", "AcGuitarSt", "ElGuitarJz", "ElGuitarCl", "ElGuitarMt", "OverdriveGt", "DistortionGt", "GtHarmonics",
    "AcBass", "FingeredBs", "PickedBass", "FretlessBs", "SlapBass1", "SlapBass2", "SynthBass1", "SynthBass2",
    "Violin", "Viola", "Cello", "Contrabass", "TremStrings", "PizzStrings", "OrchHarp", "Timpani",
    "Strings1", "Strings2", "SynStrings1", "SynStrings2", "ChoirAahs", "VoiceOohs", "SynthVoice", "OrchHit",
    "Trumpet", "Trombone", "Tuba", "MutedTrumpet", "FrenchHorn", "BrassSection", "SynthBrass1", "SynthBrass2",
    "SopranoSax", "AltoSax", "TenorSax", "BaritoneSax", "Oboe", "EnglishHorn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "PanFlute", "BlownBottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead1Square", "Lead2Saw", "Lead3Calli", "Lead4Chiff", "Lead5Charang", "Lead6Voice", "Lead7Fifths", "Lead8Bass",
    "Pad1NewAge", "Pad2Warm", "Pad3Poly", "Pad4Choir", "Pad5Bowed", "Pad6Metal", "Pad7Halo", "Pad8Sweep",
    "FX1Rain", "FX2Sound", "FX3Crystal", "FX4Atmos", "FX5Bright", "FX6Goblins", "FX7Echoes", "FX8SciFi",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "TinkleBell", "Agogo", "SteelDrums", "Woodblock", "TaikoDrum", "MelodicTom", "SynthDrum", "ReverseCym",
    "GtFretNoise", "BreathNoise", "Seashore", "BirdTweet", "Telephone", "Helicopter", "Applause", "Gunshot",
};

struct Pick {
    uint16_t sample;      // индекс записи в банке
    uint8_t bank, prog, note;
};

// Тот же выбор, что делает загрузчик: слой, чья полоса накрывает силу
// удара, и нота по keymap его инструмента.
uint16_t resolve(const bank::Bank& b, uint8_t bank_no, uint8_t prog, uint8_t note, uint8_t vel) {
    const bank::BankPreset& p = bank::bank_preset(b, bank_no, prog);
    for (uint16_t l = 0; l < bank::preset_layer_count(p); ++l) {
        const bank::BankLayer& la = b.layers[p.first_layer + l];
        if (vel < la.vel_lo || vel > la.vel_hi) continue;
        int8_t off = 0;
        const uint16_t s = bank::bank_lookup_note(b, b.instruments[la.instrument], note, &off);
        if (s != bank::kNoSample) return s;
    }
    return bank::kNoSample;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "нужно: bank_export <банк.ssb> <каталог> [--count 50] [--dpcm8-only]\n");
        return 1;
    }
    const std::string bank_path = argv[1];
    const std::string out_dir = argv[2];
    uint32_t want = 50;
    bool dpcm8_only = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) want = std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--dpcm8-only") == 0) dpcm8_only = true;
    }

    std::ifstream bf(bank_path, std::ios::binary);
    if (!bf) { std::fprintf(stderr, "банк не открывается: %s\n", bank_path.c_str()); return 1; }
    std::vector<char> blob((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
    bank::Bank b;
    const char* err = nullptr;
    if (!bank::bank_open(reinterpret_cast<const uint8_t*>(blob.data()),
                         static_cast<uint32_t>(blob.size()), b, &err)) {
        std::fprintf(stderr, "банк не читается: %s\n", err ? err : "?");
        return 1;
    }
    std::printf("банк %s: сэмплов %u\n", b.header->name, b.header->sample_count);

    // Разнообразие: идём по программам с шагом, чтобы задеть все семейства
    // GM (они идут блоками по восемь), берём по три ноты (низ, середина,
    // верх) и три силы удара. Плюс ударные - без них набор был бы однобоким.
    // Прогоны дедуплицируются по pcm_offset: одна запись служит многим
    // программам, и повторы сузили бы разнообразие при том же числе файлов.
    std::vector<Pick> picks;
    std::vector<uint32_t> seen_offsets;
    auto try_add = [&](uint8_t bank_no, uint8_t prog, uint8_t note, uint8_t vel) {
        if (picks.size() >= want) return;
        const uint16_t s = resolve(b, bank_no, prog, note, vel);
        if (s == bank::kNoSample) return;
        if (dpcm8_only && b.samples[s].resident_encoding !=
                          static_cast<uint8_t>(soundsinth::model::ResidentEncoding::Dpcm8)) return;
        const uint32_t off = b.samples[s].pcm_offset;
        for (uint32_t o : seen_offsets) if (o == off) return;
        seen_offsets.push_back(off);
        picks.push_back(Pick{ s, bank_no, prog, note });
    };

    const uint8_t notes[3] = { 40, 60, 79 };
    const uint8_t vels[3] = { 39, 87, 115 };
    // Первый проход - по одной записи на программу, чтобы охватить все
    // семейства раньше, чем набор кончится.
    for (uint32_t pass = 0; pass < 9 && picks.size() < want; ++pass) {
        for (uint32_t prog = 0; prog < 128 && picks.size() < want; prog += 3) {
            try_add(0, static_cast<uint8_t>(prog), notes[pass % 3], vels[(pass / 3) % 3]);
        }
        // Ударные: бочка, малый, хэт, тарелка, том, коровий колокольчик.
        const uint8_t drums[6] = { 36, 38, 42, 49, 45, 56 };
        for (uint8_t d : drums) try_add(128, 0, d, vels[(pass / 3) % 3]);
    }
    std::printf("выбрано %u разных прогонов\n", (unsigned)picks.size());

    auto* mem = new memory::TrackMemory();
    memory::track_memory_create(*mem);
    // Таблица распаковки - как у загрузчика .mid, одна на весь прогон.
    auto table = std::make_unique<bank::BankDecodeTable>();
    bank::bank_build_decode_table(*b.model, *table);

    std::string man_path = out_dir + "/manifest.csv";
    std::FILE* man = std::fopen(man_path.c_str(), "w");
    if (!man) { std::fprintf(stderr, "не пишется %s (каталог есть?)\n", man_path.c_str()); return 1; }
    std::fprintf(man, "file;bank;program;name;note;encoding;c5_speed_hz;length_samples;"
                      "loop_start;loop_end;looped;run_bytes;checkpoints;volume;panning\n");

    uint32_t written = 0;
    for (uint32_t i = 0; i < picks.size(); ++i) {
        const Pick& p = picks[i];
        const bank::BankSample& s = b.samples[p.sample];
        // Каждый сэмпл в чистую память: цепочки страниц иначе кончатся.
        memory::track_memory_reset_for_new_track(*mem);
        uint16_t cp_page = memory::kPageChainEnd;
        const uint16_t first = bank::bank_make_resident(b, table.get(), p.sample, mem->psram, &cp_page);
        if (first == memory::kPageChainEnd) {
            std::fprintf(stderr, "не влез в PSRAM: запись %u\n", p.sample);
            continue;
        }
        // Собираем прогон обратно в один кусок: страницы не подряд.
        std::vector<uint8_t> run;
        run.reserve(s.pcm_bytes);
        uint16_t page = first;
        while (page != memory::kPageChainEnd && run.size() < s.pcm_bytes) {
            const uint8_t* src = memory::psram_page_ptr(mem->psram, page);
            const uint32_t take = s.pcm_bytes - static_cast<uint32_t>(run.size()) < memory::kPsramPageBytes
                                      ? s.pcm_bytes - static_cast<uint32_t>(run.size())
                                      : memory::kPsramPageBytes;
            run.insert(run.end(), src, src + take);
            page = mem->psram.page_next[page];
        }

        const bool drums = p.bank == 128;
        char name[160];
        std::snprintf(name, sizeof(name), "%s/%03u_%s%u_%s_n%u.dpcm8", out_dir.c_str(), (unsigned)i,
                      drums ? "drum" : "prog", (unsigned)p.prog,
                      drums ? "kit" : kGm[p.prog], (unsigned)p.note);
        std::ofstream of(name, std::ios::binary);
        if (!of) { std::fprintf(stderr, "не пишется %s\n", name); continue; }
        of.write(reinterpret_cast<const char*>(run.data()), static_cast<std::streamsize>(run.size()));
        of.close();
        ++written;

        const char* base = std::strrchr(name, '/');
        std::fprintf(man, "%s;%u;%u;%s;%u;%s;%u;%u;%u;%u;%u;%u;%u;%u;%d\n",
                     base ? base + 1 : name, (unsigned)p.bank, (unsigned)p.prog,
                     drums ? "drum kit" : kGm[p.prog], (unsigned)p.note,
                     s.resident_encoding == static_cast<uint8_t>(soundsinth::model::ResidentEncoding::Dpcm8)
                         ? "Dpcm8" : (s.resident_encoding ==
                                      static_cast<uint8_t>(soundsinth::model::ResidentEncoding::Raw8) ? "Raw8" : "Raw16"),
                     (unsigned)s.c5_speed, (unsigned)s.length_samples,
                     (unsigned)s.loop_start, (unsigned)s.loop_end,
                     (unsigned)((s.flags & bank::kSampleLoopBit) != 0),
                     (unsigned)s.pcm_bytes, (unsigned)s.checkpoint_count,
                     (unsigned)s.global_volume, (int)s.default_panning);
    }
    std::fclose(man);
    std::printf("записано %u файлов в %s, манифест %s\n", written, out_dir.c_str(), man_path.c_str());
    return 0;
}
