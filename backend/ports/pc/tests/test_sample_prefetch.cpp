// SPDX-License-Identifier: MIT
// Планировщик порядка загрузки сэмплов для прогрессивной загрузки -
// io/sample_prefetch.h.
//
// Проверяется:
//   1. last_use_order_pos совпадает с независимым обходом, написанным
//      здесь по-другому (сначала множество сэмплов на каждую позицию
//      order-листа, потом максимум по позициям);
//   2. префетч - сэмплы первых двух воспроизводимых позиций (порядок
//      внутри - по file_offset, см. ниже);
//   3. хвост - по возрастанию file_offset от конца префетча, лежащие
//      раньше - в конце (один прыжок назад);
//   4. режим ByPlayback.
//
// Отдельно - синтетический order-лист, где один и тот же паттерн стоит
// и в начале, и в конце: сэмпл должен быть помечен по последнему
// вхождению. Ради этого случая last_use считается по позициям
// order-листа, а не по номерам паттернов.

#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include "core/formats/it.h"
#include "core/formats/mod.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "player/load/sample_prefetch.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_reader.h"

namespace {

using namespace soundsinth;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::Song;

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool is_real_note_ref(uint8_t note) {
    return note != soundsinth::model::kNoteNone && note != soundsinth::model::kNoteOff && note != soundsinth::model::kNoteCut &&
           note != soundsinth::model::kNoteFade;
}

// Независимый обход: множество сэмплов на каждую воспроизводимую позицию
// order-листа (плюс порядок первого появления отдельным списком). Из него
// выводятся и last_use, и префетч - другой структурой циклов, чем в
// планировщике, чтобы сверка не была тавтологией.
struct Reference {
    std::vector<std::pair<uint16_t, std::set<uint16_t>>> per_position; // (order_pos, сэмплы)
    std::vector<uint16_t> first_appearance_order;
};

Reference build_reference(const Song& song, memory::PsramStore& psram) {
    Reference ref;
    std::set<uint16_t> seen;
    uint8_t last_instrument[64] = {};

    for (uint16_t pos = 0; pos < song.order_count; ++pos) {
        const uint16_t pat_index = song.order[pos];
        if (pat_index == soundsinth::model::kOrderEnd) break;
        if (pat_index == soundsinth::model::kOrderSkip || pat_index >= song.pattern_count) continue;
        const Pattern& pat = song.patterns[pat_index];
        if (pat.psram_offset == Pattern::kInvalidOffset || pat.channel_count == 0) continue;

        std::set<uint16_t> here;
        PatternCell row[64];
        const patterns::PatternReader reader(memory::psram_pattern_ptr(psram, pat.psram_offset), pat.row_count, pat.channel_count);
        const uint8_t channels = std::min<uint8_t>(pat.channel_count, 64);
        for (uint16_t r = 0; r < pat.row_count; ++r) {
            reader.read_row(r, row);
            for (uint8_t ch = 0; ch < channels; ++ch) {
                if (row[ch].instrument != 0) last_instrument[ch] = row[ch].instrument;
                if (!is_real_note_ref(row[ch].note)) continue;
                uint16_t idx = 0;
                uint8_t note = row[ch].note;
                if (!soundsinth::model::resolve_sample_index(song, last_instrument[ch], row[ch].note, &idx, &note)) {
                    continue;
                }
                if (song.samples[idx].length_samples == 0) continue;
                here.insert(idx);
                if (seen.insert(idx).second) ref.first_appearance_order.push_back(idx);
            }
        }
        ref.per_position.emplace_back(pos, here);
    }
    return ref;
}

struct FormatOps {
    bool (*load)(formats::ByteSource, memory::TrackMemory&, Song&, const char**, bool);
};

void check_planner(const FormatOps& ops, const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    if (!ops.load(src.as_byte_source(), mem, song, &err, false)) {
        CHECK(false);
        memory::track_memory_destroy(mem);
        return;
    }

    std::vector<uint16_t> indices(song.sample_count == 0 ? 1 : song.sample_count);
    std::vector<uint16_t> last_use(song.sample_count == 0 ? 1 : song.sample_count);
    const player::load::PlaybackPlan planned =
        player::load::plan_playback_order(song, mem.psram, indices.data(), static_cast<uint16_t>(indices.size()), last_use.data());
    const uint16_t count    = planned.count;
    const uint16_t prefetch = planned.prefetch_count;

    const Reference ref = build_reference(song, mem.psram);

    // (1) last_use - максимум по позициям независимого обхода.
    std::map<uint16_t, uint16_t> expect_last_use;
    for (const auto& entry : ref.per_position) {
        for (const uint16_t idx : entry.second)
            expect_last_use[idx] = entry.first; // pos растёт => максимум
    }
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        const auto it       = expect_last_use.find(i);
        const uint16_t want = (it == expect_last_use.end()) ? player::load::kSampleNeverUsed : it->second;
        CHECK_EQ(last_use[i], want);
    }

    // (2) Состав и порядок: планировщик отдаёт те же сэмплы, что и
    // независимый обход.
    //
    // Порядок внутри префетча больше не "по первому появлению", это
    // сознательная смена (см. sample_prefetch.cpp): на железе битыми раз за
    // разом оказывались сэмплы, перед которыми шёл самый длинный прыжок
    // вперёд по файлу, а порядок первого появления такие прыжки и создавал.
    // Теперь префетч тоже упорядочен по file_offset.
    //
    // Проверяется состав префетча (должен остаться прежним, ниже) и то, что
    // порядок неубывающий по смещению в файле: это и делает маршрут по файлу
    // линейным.
    CHECK_EQ(count, static_cast<uint16_t>(ref.first_appearance_order.size()));
    CHECK(prefetch <= count);
    for (uint16_t i = 1; i < prefetch; ++i) {
        CHECK(song.samples[indices[i - 1]].file_offset <= song.samples[indices[i]].file_offset);
    }

    // Префетч - объединение сэмплов первых двух воспроизводимых позиций.
    std::set<uint16_t> expect_prefetch;
    for (size_t p = 0; p < ref.per_position.size() && p < player::load::kPrefetchOrderPositions; ++p) {
        expect_prefetch.insert(ref.per_position[p].second.begin(), ref.per_position[p].second.end());
    }
    CHECK_EQ(static_cast<size_t>(prefetch), expect_prefetch.size());
    for (uint16_t i = 0; i < prefetch; ++i)
        CHECK(expect_prefetch.count(indices[i]) == 1);

    // Множества (весь план) тоже должны совпасть; порядок хвоста иной,
    // поэтому сверяем как множества.
    std::set<uint16_t> got_all(indices.begin(), indices.begin() + count);
    std::set<uint16_t> want_all(ref.first_appearance_order.begin(), ref.first_appearance_order.end());
    CHECK(got_all == want_all);

    // (3) хвост - от конца префетча вперёд, потом лежащие раньше: часть
    // "впереди" (смещение не меньше последнего сэмпла префетча) неубывает,
    // часть "позади" неубывает, и все "впереди" идут раньше всех "позади".
    {
        const uint32_t origin = prefetch > 0 ? song.samples[indices[prefetch - 1]].file_offset : 0u;
        bool behind           = false;
        uint32_t prev_ahead = 0, prev_behind = 0, bad = 0;
        for (uint16_t i = prefetch; i < count; ++i) {
            const uint32_t off = song.samples[indices[i]].file_offset;
            if (off >= origin) {
                if (behind || off < prev_ahead) ++bad; // впереди после оборота или спуск
                prev_ahead = off;
            } else {
                if (behind && off < prev_behind) ++bad;
                behind      = true;
                prev_behind = off;
            }
        }
        CHECK_EQ(bad, 0u);
    }

    // (4) ByPlayback - тот же состав и та же граница префетча, но порядок
    // тот, в каком сэмплы встретились в order-листе, без пересортировки.
    // Эталон независимого обхода строится так же, поэтому сверяем
    // поэлементно, а не как множества.
    {
        std::vector<uint16_t> pb(indices.size());
        std::vector<uint16_t> pb_last_use(last_use.size());
        // Мусор до вызова: незвучащие планировщик обязан пометить сам.
        std::vector<uint16_t> pb_first_use(last_use.size(), 0x1234);
        const player::load::PlaybackPlan pb_planned =
            player::load::plan_playback_order(song, mem.psram, pb.data(), static_cast<uint16_t>(pb.size()), pb_last_use.data(),
                                              player::load::LoadOrder::ByPlayback, player::load::kPrefetchOrderPositions, pb_first_use.data());
        const uint16_t pb_count    = pb_planned.count;
        const uint16_t pb_prefetch = pb_planned.prefetch_count;
        CHECK_EQ(pb_count, count);
        CHECK_EQ(pb_prefetch, prefetch);
        for (uint16_t i = 0; i < pb_count; ++i)
            CHECK_EQ(pb[i], ref.first_appearance_order[i]);
        for (uint16_t i = 0; i < song.sample_count; ++i)
            CHECK_EQ(pb_last_use[i], last_use[i]);
        // (5) first_use - первая позиция независимого обхода.
        std::map<uint16_t, uint16_t> expect_first_use;
        for (const auto& entry : ref.per_position) {
            for (const uint16_t idx : entry.second)
                expect_first_use.emplace(idx, entry.first); // pos растёт => первое вхождение
        }
        for (uint16_t i = 0; i < song.sample_count; ++i) {
            const auto it = expect_first_use.find(i);
            CHECK_EQ(pb_first_use[i], it == expect_first_use.end() ? player::load::kSampleNeverUsed : it->second);
        }
    }

    // Сколько байт файла нужно прочитать, чтобы префетч состоялся, - это
    // задержка до первой ноты. Считаем как самый дальний конец данных среди
    // префетч-сэмплов: источник читает вперёд, и до этой точки дойти придётся
    // в любом случае.
    uint32_t prefetch_far_end = 0;
    uint32_t prefetch_bytes   = 0;
    for (uint16_t i = 0; i < prefetch; ++i) {
        const soundsinth::model::SampleDescriptor& sd  = song.samples[indices[i]];
        const uint32_t bytes                           = sd.source_length_samples * ((sd.encoding == soundsinth::model::SampleEncoding::Pcm16) ? 2u : 1u);
        prefetch_bytes                                += bytes;
        const uint32_t end                             = sd.file_offset + bytes;
        if (end > prefetch_far_end) prefetch_far_end = end;
    }
    uint16_t tail_behind = 0;
    {
        const uint32_t origin = prefetch > 0 ? song.samples[indices[prefetch - 1]].file_offset : 0u;
        for (uint16_t i = prefetch; i < count; ++i) {
            if (song.samples[indices[i]].file_offset < origin) ++tail_behind;
        }
    }
    std::printf("  %s: plan %u, prefetch %u - before the first note read up to %u KB of the file (%u KB of data itself); "
                "tail: %u ahead of the prefetch end, %u behind\n",
                path, count, prefetch, prefetch_far_end / 1024u, prefetch_bytes / 1024u, static_cast<unsigned>(count - prefetch - tail_behind),
                static_cast<unsigned>(tail_behind));
    memory::track_memory_destroy(mem);
}

// Один и тот же паттерн в order-листе дважды: в начале и в конце.
// Сэмплы этого паттерна должны быть помечены по последнему вхождению,
// иначе вытеснение выбросит их сразу после первого прохода, и во второй
// раз паттерн отыграет тишиной.
void test_repeated_pattern_marks_last_occurrence() {
    std::printf("test_sample_prefetch_repeated_pattern_marks_last_occurrence\n");
    const char* path                 = "SD/test_music/it/00009.it";
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    if (!formats::it::load(src.as_byte_source(), mem, song, &err)) {
        CHECK(false);
        memory::track_memory_destroy(mem);
        return;
    }

    // Подменяем order-лист на [P, Q, Q, Q, P], где P - первый
    // воспроизводимый паттерн исходного файла, Q - второй. Массив
    // bump-аллоцирован внутри mem.resident и принадлежит нам целиком,
    // пока жива mem.
    std::vector<uint16_t> playable;
    for (uint16_t i = 0; i < song.order_count && playable.size() < 2; ++i) {
        const uint16_t p = song.order[i];
        if (p == soundsinth::model::kOrderEnd) break;
        if (p == soundsinth::model::kOrderSkip || p >= song.pattern_count) continue;
        if (song.patterns[p].psram_offset == Pattern::kInvalidOffset) continue;
        if (std::find(playable.begin(), playable.end(), p) == playable.end()) playable.push_back(p);
    }
    if (playable.size() < 2 || song.order_count < 5) {
        std::printf("  SKIP: the file has fewer than two distinct playable patterns\n");
        memory::track_memory_destroy(mem);
        return;
    }
    uint16_t* order                     = const_cast<uint16_t*>(song.order);
    order[0]                            = playable[0];
    order[1]                            = playable[1];
    order[2]                            = playable[1];
    order[3]                            = playable[1];
    order[4]                            = playable[0];
    const_cast<Song&>(song).order_count = 5;

    std::vector<uint16_t> indices(song.sample_count);
    std::vector<uint16_t> last_use(song.sample_count);
    const player::load::PlaybackPlan planned =
        player::load::plan_playback_order(song, mem.psram, indices.data(), static_cast<uint16_t>(indices.size()), last_use.data());
    const uint16_t count    = planned.count;
    const uint16_t prefetch = planned.prefetch_count;
    CHECK(count > 0);

    const Reference ref = build_reference(song, mem.psram);
    CHECK_EQ(ref.per_position.size(), static_cast<size_t>(5));

    // Сэмплы паттерна P звучат на позициях 0 и 4 - метка должна быть 4.
    const std::set<uint16_t>& in_p = ref.per_position[0].second;
    CHECK(!in_p.empty());
    for (const uint16_t idx : in_p)
        CHECK_EQ(last_use[idx], static_cast<uint16_t>(4));

    // Сэмплы, звучащие только в Q (позиции 1..3), - метка 3, а не 1.
    const std::set<uint16_t>& in_q = ref.per_position[1].second;
    size_t q_only                  = 0;
    for (const uint16_t idx : in_q) {
        if (in_p.count(idx) != 0) continue;
        CHECK_EQ(last_use[idx], static_cast<uint16_t>(3));
        ++q_only;
    }
    std::printf("  samples only in a repeated pattern: %u, marked by the last occurrence\n", static_cast<unsigned>(q_only));

    memory::track_memory_destroy(mem);
}

// Ёмкость меньше числа сэмплов: план упирается в неё, метки последнего
// использования - у всех сэмплов, как при полной ёмкости. prefetch_positions
// 0 - префетча нет, больше длины песни - префетч весь план.
void test_capacity_and_prefetch_positions() {
    std::printf("test_sample_prefetch_capacity_and_prefetch_positions\n");
    const char* path                 = "SD/test_music/it/00009.it";
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    CHECK(formats::it::load(src.as_byte_source(), mem, song, &err));
    const uint16_t n = song.sample_count;
    std::vector<uint16_t> full(n), full_last(n), small(n), small_last(n);
    const player::load::PlaybackPlan all = player::load::plan_playback_order(song, mem.psram, full.data(), n, full_last.data());
    CHECK(all.count > 3);
    const player::load::PlaybackPlan cut = player::load::plan_playback_order(song, mem.psram, small.data(), 3, small_last.data());
    CHECK_EQ(cut.count, 3u);
    CHECK(cut.prefetch_count <= 3u);
    CHECK(small_last == full_last);
    const player::load::PlaybackPlan none =
        player::load::plan_playback_order(song, mem.psram, small.data(), n, small_last.data(), player::load::LoadOrder::ByFile, 0);
    CHECK_EQ(none.prefetch_count, 0u);
    CHECK_EQ(none.count, all.count);
    const player::load::PlaybackPlan whole =
        player::load::plan_playback_order(song, mem.psram, small.data(), n, small_last.data(), player::load::LoadOrder::ByFile, 0xffff);
    CHECK_EQ(whole.prefetch_count, whole.count);
    memory::track_memory_destroy(mem);
}

// Сборщик плана из прохода длительности: позиция в проходе убывает на
// переходе назад, поэтому first_use - минимум, last_use - максимум. Нота
// без инструмента играет прежним инструментом канала; сэмпл за ёмкостью
// массивов не пишется никуда.
void test_plan_collect_row_extremes() {
    std::printf("test_sample_prefetch_plan_collect_row_extremes\n");
    soundsinth::model::SampleDescriptor samples[4];
    soundsinth::model::Instrument instruments[4];
    for (uint16_t i = 0; i < 4; ++i) {
        samples[i].length_samples           = 1;
        instruments[i].default_sample_index = i;
    }
    Song song;
    song.samples            = samples;
    song.sample_count       = 4;
    song.instruments        = instruments;
    song.instrument_count   = 4;
    constexpr uint16_t kCap = 3;
    uint16_t indices[kCap], last_use[kCap], first_use[kCap];
    for (uint16_t i = 0; i < kCap; ++i)
        indices[i] = last_use[i] = first_use[i] = 0x1234;
    uint32_t words[4]        = {};
    player::load::UseMap map = player::load::use_map_layout(kCap, 20);
    map.words                = words;
    player::load::PlanCollector pc;
    player::load::plan_collect_begin(pc, song, indices, kCap, last_use, first_use);
    pc.use_map     = &map;
    const auto row = [&](uint16_t pos, uint16_t ins0, uint16_t ins1) {
        PatternCell cells[2];
        cells[0].note       = 60;
        cells[0].instrument = ins0;
        cells[1].note       = ins1 != 0 ? 60 : soundsinth::model::kNoteNone;
        cells[1].instrument = ins1;
        player::load::plan_collect_row(&pc, cells, 2, pos);
    };
    row(5, 3, 0); // сэмпл 2
    row(3, 1, 4); // сэмпл 0; сэмпл 3 за ёмкостью
    row(9, 0, 0); // канал 0 - прежним инструментом, сэмпл 0
    row(1, 3, 0); // сэмпл 2 раньше всех
    CHECK_EQ(pc.count, 2u);
    CHECK_EQ(indices[0], 2u);
    CHECK_EQ(indices[1], 0u);
    CHECK_EQ(first_use[2], 1u);
    CHECK_EQ(last_use[2], 5u);
    CHECK_EQ(first_use[0], 3u);
    CHECK_EQ(last_use[0], 9u);
    CHECK_EQ(first_use[1], player::load::kSampleNeverUsed);
    CHECK_EQ(last_use[1], player::load::kSampleNeverUsed);
    CHECK_EQ(player::load::use_map_distance(map, 0, 4), 5u);
    CHECK_EQ(player::load::use_map_distance(map, 2, 2), 3u);
    CHECK_EQ(player::load::use_map_distance(map, 1, 0), player::load::kUseMapNever);
}

void test_sample_prefetch_matches_independent_walk() {
    std::printf("test_sample_prefetch_matches_independent_walk\n");
    const FormatOps kIt{&formats::it::load};
    const FormatOps kS3m{&formats::s3m::load};
    const FormatOps kXm{&formats::xm::load};
    const FormatOps kMod{&formats::mod::load};
    check_planner(kIt, "SD/test_music/it/00009.it");
    check_planner(kIt, "SD/test_music/it/ivi-lite__v61.it");
    check_planner(kS3m, "SD/test_music/s3m/2nd_reality.s3m");
    check_planner(kXm, "SD/test_music/xm/final_fantasy.xm");
    check_planner(kXm, "SD/test_music/xm/000h_cara_mia.xm"); // 4.5 МБ - тот самый медленный старт
    check_planner(kMod, "SD/test_music/mod/star_wars.mod");
    check_planner(kMod, "SD/test_music/mod/legend_of_zelda.mod");
}

} // namespace

// Граница префетча - в сырых номерах order, как first_use: маркер в начале
// order её не сдвигает. Деление устойчивое: у ByPlayback порядок первого
// появления сохраняется.
void test_split_prefetch_counts_raw_positions() {
    std::vector<soundsinth::model::Pattern> pats(2);
    for (auto& pt : pats) {
        pt.psram_offset  = 0;
        pt.channel_count = 4;
    }
    const uint16_t order[3] = {soundsinth::model::kOrderSkip, 0, 1};
    soundsinth::model::Song song;
    song.patterns      = pats.data();
    song.pattern_count = 2;
    song.order         = order;
    song.order_count   = 3;
    // Сэмплы 0..3: первое появление на сырых позициях 1, 2, 1, никогда.
    const uint16_t first_use[4]           = {1, 2, 1, player::load::kSampleNeverUsed};
    uint16_t idx[4]                       = {3, 2, 1, 0};
    const player::load::PlaybackPlan plan = player::load::plan_split_prefetch(song, idx, 4, first_use, 1, player::load::LoadOrder::ByPlayback);
    std::printf("  prefetch over 1 playable position after a marker: %u samples, order %u %u\n", plan.prefetch_count, idx[0], idx[1]);
    CHECK_EQ(plan.prefetch_count, 2u);
    CHECK(idx[0] == 2 && idx[1] == 0); // в прежнем порядке
}

void run_sample_prefetch_tests() {
    test_split_prefetch_counts_raw_positions();
    test_sample_prefetch_matches_independent_walk();
    test_repeated_pattern_marks_last_occurrence();
    test_capacity_and_prefetch_positions();
    test_plan_collect_row_extremes();
}
