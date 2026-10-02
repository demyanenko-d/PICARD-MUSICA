// SPDX-License-Identifier: MIT
// Прогрессивная загрузка: распаковка одного сэмпла, вызываемая извне.
//
// Основной тест: содержимое PSRAM должно совпадать побайтово между
//   (а) обычной загрузкой - load() пакует все сэмплы сам, по порядку;
//   (б) загрузкой, где сэмплы распакованы по одному через публичную
//       load_sample_pcm(), причём в обратном порядке.
//
// Обратный порядок доказывает, что вынесенная функция самодостаточна и не
// зависит ни от порядка вызовов, ни от scratch-состояния, оставшегося от
// предыдущего сэмпла. Это нужно прогрессивной загрузке, где сэмплы
// тянутся с хоста в порядке воспроизведения, а не в порядке следования в
// файле.

#include "testing.h"
#include "song_compare.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/codec/loop_unroll.h"
#include "core/codec/pack_file_pcm.h"
#include "core/formats/it.h"
#include "core/formats/mod.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "player/load/progressive_loader.h"
#include "player/load/sample_prefetch.h"
#include "player/load/session_loader.h"
#include "core/memory/track_memory.h"

namespace {

using namespace soundsinth;

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

using song_compare::collect_sample_bytes;

// Загрузчики - четыре независимые пары функций с одинаковой сигнатурой,
// поэтому тест параметризуется указателями, а не шаблоном.
struct FormatOps {
    bool (*load)(formats::ByteSource, memory::TrackMemory&, soundsinth::model::Song&, const char**, bool);
    bool (*load_sample_pcm)(formats::ByteSource, memory::TrackMemory&, const soundsinth::model::Song&, uint16_t, const char**);
};

const FormatOps kIt{&formats::it::load, &formats::it::load_sample_pcm};
const FormatOps kS3m{&formats::s3m::load, &formats::s3m::load_sample_pcm};
const FormatOps kXm{&formats::xm::load, &formats::xm::load_sample_pcm};
const FormatOps kMod{&formats::mod::load, &formats::mod::load_sample_pcm};

void check_reverse_order_matches_eager(const FormatOps& ops, const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    const uint32_t size = static_cast<uint32_t>(bytes.size());

    // --- (а) эталон: обычная загрузка ---
    memory::TrackMemory mem_ref;
    memory::track_memory_create(mem_ref);
    soundsinth::model::Song song_ref;
    {
        formats::MemoryByteSource src(bytes.data(), size);
        const char* err = nullptr;
        const bool ok   = ops.load(src.as_byte_source(), mem_ref, song_ref, &err, false);
        CHECK(ok);
        if (!ok) {
            std::printf("  the reference load failed %s: %s\n", path, err ? err : "?");
            memory::track_memory_destroy(mem_ref);
            return;
        }
    }

    // --- (б) та же полная загрузка (metadata_only=false), затем каждый
    // сэмпл перепаковывается ещё раз, по одному, в обратном порядке: так
    // проверяется вынесенная функция. ---
    memory::TrackMemory mem_rev;
    memory::track_memory_create(mem_rev);
    soundsinth::model::Song song_rev;
    formats::MemoryByteSource src_rev(bytes.data(), size);
    {
        const char* err = nullptr;
        CHECK(ops.load(src_rev.as_byte_source(), mem_rev, song_rev, &err, false));
    }
    // Сбросить результат упаковки, оставив метаданные и паттерны, нельзя
    // (паттерны тоже в PSRAM), поэтому каждый сэмпл пакуется ещё раз, в
    // обратном порядке, в ту же PSRAM, и содержимое новых цепочек сверяется
    // с эталонными.
    for (int i = static_cast<int>(song_rev.sample_count) - 1; i >= 0; --i) {
        const uint16_t idx                            = static_cast<uint16_t>(i);
        const soundsinth::model::SampleDescriptor& sd = song_rev.samples[idx];
        if (sd.length_samples == 0) continue;

        // Эталонные байты этого сэмпла (из первой, обычной загрузки).
        const std::vector<uint8_t> expect = collect_sample_bytes(mem_ref.psram, mem_ref.sample_cache, idx, sd);

        // Старая цепочка освобождается до перепаковки, а её страницы
        // забиваются мусором: новая упаковка может взять те же страницы, и
        // совпадение с эталоном без мусора было бы тавтологией. Держать
        // старую цепочку на время перепаковки нельзя: у трека с развёрнутыми
        // ping-pong петлями и у пограничного Raw16 запаса на копию сэмпла
        // нет, и тест падал бы из-за собственной методики.
        memory::SampleCacheEntry* before = memory::sample_cache_find(mem_rev.sample_cache, idx);
        if (before != nullptr) {
            const uint16_t old_first_page = before->first_page;
            memory::sample_cache_free_slot(mem_rev.sample_cache, before);
            for (uint16_t p = old_first_page; p != memory::kPageChainEnd; p = memory::psram_page_next(mem_rev.psram, p)) {
                std::memset(memory::psram_page_ptr(mem_rev.psram, p), 0xA5, memory::kPsramPageBytes);
            }
            memory::psram_free_chain(mem_rev.psram, old_first_page);
        }

        const bool packed = ops.load_sample_pcm(src_rev.as_byte_source(), mem_rev, song_rev, idx, nullptr);
        CHECK_EQ(packed, !expect.empty()); // упаковалось тогда же, когда и в эталоне
        if (!packed) continue;

        const std::vector<uint8_t> got = collect_sample_bytes(mem_rev.psram, mem_rev.sample_cache, idx, sd);
        CHECK_EQ(got.size(), expect.size());
        CHECK(got == expect); // побайтово - главное утверждение теста
    }

    std::printf("  %s: %u samples repacked in reverse order, PSRAM matched byte for byte\n", path, song_rev.sample_count);

    memory::track_memory_destroy(mem_ref);
    memory::track_memory_destroy(mem_rev);
}

void test_deferred_sample_load_matches_eager_bit_exact() {
    std::printf("test_deferred_sample_load_matches_eager_bit_exact\n");
    check_reverse_order_matches_eager(kIt, "SD/test_music/it/00009.it");
    check_reverse_order_matches_eager(kIt, "SD/test_music/it/ivi-lite__v61.it");
    check_reverse_order_matches_eager(kS3m, "SD/test_music/s3m/2nd_reality.s3m");
    check_reverse_order_matches_eager(kS3m, "SD/test_music/s3m/starwars.s3m");
    check_reverse_order_matches_eager(kXm, "SD/test_music/xm/final_fantasy.xm");
    check_reverse_order_matches_eager(kXm, "SD/test_music/xm/000h_cara_mia.xm");
    check_reverse_order_matches_eager(kMod, "SD/test_music/mod/star_wars.mod");
    check_reverse_order_matches_eager(kMod, "SD/test_music/mod/legend_of_zelda.mod");
}

// Полный путь прогрессивной загрузки на PC: run_session_load в режиме
// "только метаданные" -> plan_playback_order -> load_track_sample по
// одному, в порядке воспроизведения. PSRAM должен совпасть побайтово с
// обычной полной загрузкой того же файла.
void check_metadata_only_then_planned_order(const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    const uint32_t size = static_cast<uint32_t>(bytes.size());

    // Эталон - обычная полная загрузка через ту же точку входа.
    memory::TrackMemory mem_ref;
    memory::track_memory_create(mem_ref);
    soundsinth::model::Song song_ref;
    {
        formats::MemoryByteSource src(bytes.data(), size);
        player::load::SessionLoadResult load;
        CHECK(player::load::run_session_load(src.as_byte_source(), mem_ref, song_ref, load));
    }

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource src(bytes.data(), size);
    player::load::SessionLoadResult load;
    CHECK(player::load::run_session_load(src.as_byte_source(), mem, song, load, /*metadata_only=*/true));
    const player::load::TrackFormat format = load.format;
    CHECK(format != player::load::TrackFormat::None);

    // Метаданные совпали, а PCM в PSRAM нет ни у одного сэмпла.
    CHECK_EQ(song.sample_count, song_ref.sample_count);
    CHECK_EQ(song.pattern_count, song_ref.pattern_count);
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        CHECK_EQ(song.samples[i].length_samples, song_ref.samples[i].length_samples);
        CHECK_EQ(song.samples[i].resident_encoding, song_ref.samples[i].resident_encoding);
        CHECK(memory::sample_cache_find(mem.sample_cache, i) == nullptr);
    }

    std::vector<uint16_t> plan(song.sample_count == 0 ? 1 : song.sample_count);
    std::vector<uint16_t> last_use(plan.size());
    const player::load::PlaybackPlan planned =
        player::load::plan_playback_order(song, mem.psram, plan.data(), static_cast<uint16_t>(plan.size()), last_use.data());
    const uint16_t count    = planned.count;
    const uint16_t prefetch = planned.prefetch_count;

    uint32_t resident_bytes = 0;
    for (uint16_t k = 0; k < count; ++k) {
        const uint16_t idx = plan[k];
        CHECK(player::load::load_track_sample(format, src.as_byte_source(), mem, song, idx));
        const std::vector<uint8_t> expect = collect_sample_bytes(mem_ref.psram, mem_ref.sample_cache, idx, song_ref.samples[idx]);
        const std::vector<uint8_t> got    = collect_sample_bytes(mem.psram, mem.sample_cache, idx, song.samples[idx]);
        CHECK_EQ(got.size(), expect.size());
        CHECK(got == expect);
        resident_bytes += static_cast<uint32_t>(got.size());
    }

    // Резидентный объём отвечает на вопрос "влезает ли трек в PSRAM"
    // (RP2350: 8 МБ) и показывает, насколько план сокращает работу на
    // файлах, где объявлено больше сэмплов, чем звучит.
    std::printf("  %s: metadata without PCM, then %u of %u samples in playback order (prefetch %u), %u KB in "
                "PSRAM - matched\n",
                path, count, song.sample_count, prefetch, resident_bytes / 1024u);
    memory::track_memory_destroy(mem);
    memory::track_memory_destroy(mem_ref);
}

void test_metadata_only_then_planned_order_matches_eager() {
    std::printf("test_metadata_only_then_planned_order_matches_eager\n");
    check_metadata_only_then_planned_order("SD/test_music/it/00009.it");
    check_metadata_only_then_planned_order("SD/test_music/it/ivi-lite__v61.it");
    check_metadata_only_then_planned_order("SD/test_music/s3m/2nd_reality.s3m");
    check_metadata_only_then_planned_order("SD/test_music/xm/final_fantasy.xm");
    check_metadata_only_then_planned_order("SD/test_music/mod/star_wars.mod");
    // 535 сэмплов, 9.4 МБ - файл, который не пролезал в план при потолке
    // 512 и грузился целиком до старта звука.
    check_metadata_only_then_planned_order("SD/test_music/it/bz_ult9.it");
}
// Каждый сэмпл каждого файла должен загрузиться, причём по одному, через
// публичную load_sample_pcm, а не общим проходом load().
//
// Заведён по логу железа (2026-08-23): на нескольких .it стабильно
// падали отдельные сэмплы с "IT-распаковка вернула 0", и провалы шли на
// тех же индексах при каждом прогоне. Повторяемость означала, что шина ни
// при чём (её к тому моменту вычистили: 0 овер-ранов, пик FIFO 1/8), а
// виновата распаковка. Здесь источник - обычная память, обмена с хостом
// нет, поэтому если тест краснеет - вопрос закрыт в пользу декодера.
//
// Существующие тесты этого не ловили: check_reverse_order_matches_eager
// сверяет два пути между собой, и одинаково провалившийся в обоих сэмпл
// проходит как совпадение (CHECK_EQ(packed, !expect.empty()) - обе
// стороны пусты).
void check_all_samples_load(const FormatOps& ops, const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    const uint32_t size = static_cast<uint32_t>(bytes.size());

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource src(bytes.data(), size);
    {
        const char* err = nullptr;
        const bool ok   = ops.load(src.as_byte_source(), mem, song, &err, /*metadata_only=*/true);
        CHECK(ok);
        if (!ok) {
            std::printf("  metadata parsing failed %s: %s\n", path, err ? err : "?");
            memory::track_memory_destroy(mem);
            return;
        }
    }

    uint16_t failed    = 0;
    uint16_t attempted = 0;
    for (uint16_t idx = 0; idx < song.sample_count; ++idx) {
        if (song.samples[idx].length_samples == 0) continue; // пустой слот - не сэмпл
        ++attempted;
        const char* reason = nullptr;
        if (!ops.load_sample_pcm(src.as_byte_source(), mem, song, idx, &reason)) {
            ++failed;
            std::printf("  %s: sample %u NOT loaded: %s\n", path, idx, reason ? reason : "?");
        }
    }
    CHECK_EQ(failed, 0);
    std::printf("  %s: %u samples, failures %u\n", path, attempted, failed);

    memory::track_memory_destroy(mem);
}

void test_every_sample_loads_standalone() {
    std::printf("test_every_sample_loads_standalone\n");
    // Файлы, на которых железо стабильно теряло сэмплы (по размеру из
    // лога: 723526, 820189, 467564, 490208, 328983).
    check_all_samples_load(kIt, "SD/test_music/it/00012 ladda upp denna.it");
    check_all_samples_load(kIt, "SD/test_music/it/038djzjack_littlerock.it");
    check_all_samples_load(kIt, "SD/test_music/it/deeper__v54.it");
    check_all_samples_load(kIt, "SD/test_music/it/dg_pcorn__v50.it");
    check_all_samples_load(kIt, "SD/test_music/it/dg_reald__v55.it");
    // Контроль: файлы, где провалов не было ни разу.
    check_all_samples_load(kIt, "SD/test_music/it/00009.it");
    check_all_samples_load(kIt, "SD/test_music/it/ivi-lite__v61.it");
    // MOD и S3M: сверка двух путей между собой не ловит сэмпл, провалившийся в обоих.
    check_all_samples_load(kMod, "SD/test_music/mod/star_wars.mod");
    check_all_samples_load(kMod, "SD/test_music/mod/legend_of_zelda.mod");
    check_all_samples_load(kMod, "SD/test_music/mod/12ako.mod"); // 6CHN, петли длиной 0
    check_all_samples_load(kS3m, "SD/test_music/s3m/2nd_reality.s3m");
    check_all_samples_load(kS3m, "SD/test_music/s3m/starwars.s3m");
    check_all_samples_load(kS3m, "SD/test_music/s3m/2nd_pm.s3m");
}
} // namespace

// Song между загрузками переиспользуется (на плате - shared::g_song):
// поля прошлого трека, которые новый загрузчик не пишет, обязаны
// сброситься, иначе MOD после .mid играл бы с лимитером, а после IT - с
// ChnVol, выключенными каналами и сведением ModPlug.
void test_session_load_resets_song() {
    std::printf("test_session_load_resets_song\n");
    const std::vector<uint8_t> bytes = read_whole_file("SD/test_music/mod/star_wars.mod");
    if (bytes.empty()) {
        std::printf("  SKIP (file not found)\n");
        return;
    }
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    song.limiter_enabled     = true;
    song.volume_ramp_samples = 220;
    song.mix_levels          = soundsinth::model::Song::MixLevels::Original;
    song.channel_volume[0]   = 5;
    song.channel_pan[1]      = 3;
    song.channel_muted       = ~uint64_t(0);
    song.channel_surround    = ~uint64_t(0);
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    player::load::SessionLoadResult load;
    CHECK(player::load::run_session_load(src.as_byte_source(), mem, song, load));
    CHECK(!song.limiter_enabled);
    CHECK_EQ(song.volume_ramp_samples, static_cast<uint16_t>(0));
    CHECK(song.mix_levels == soundsinth::model::Song::MixLevels::Compatible);
    CHECK_EQ(song.channel_volume[0], static_cast<uint8_t>(64));
    CHECK_EQ(song.channel_pan[1], static_cast<uint8_t>(32));
    CHECK(song.channel_muted == 0);
    CHECK(song.channel_surround == 0);
    memory::track_memory_destroy(mem);

    // S3M пишет channel_muted через |=: грязный Song обязан дать то же, что
    // чистый.
    const std::vector<uint8_t> s3m = read_whole_file("SD/test_music/s3m/2nd_pm.s3m");
    if (s3m.empty()) {
        std::printf("  SKIP (file not found): 2nd_pm.s3m\n");
        return;
    }
    soundsinth::model::Song clean;
    soundsinth::model::Song dirty;
    dirty.limiter_enabled     = true;
    dirty.volume_ramp_samples = 220;
    dirty.channel_volume[0]   = 5;
    dirty.channel_pan[1]      = 3;
    dirty.channel_muted       = ~uint64_t(0);
    dirty.channel_surround    = ~uint64_t(0);
    for (soundsinth::model::Song* s : {&clean, &dirty}) {
        memory::TrackMemory m;
        memory::track_memory_create(m);
        formats::MemoryByteSource ss(s3m.data(), static_cast<uint32_t>(s3m.size()));
        player::load::SessionLoadResult r;
        CHECK(player::load::run_session_load(ss.as_byte_source(), m, *s, r));
        memory::track_memory_destroy(m);
    }
    CHECK(dirty.channel_muted == clean.channel_muted);
    CHECK(dirty.channel_surround == clean.channel_surround);
    CHECK_EQ(dirty.limiter_enabled, clean.limiter_enabled);
    CHECK_EQ(dirty.volume_ramp_samples, clean.volume_ramp_samples);
    CHECK_EQ(dirty.channel_volume[0], clean.channel_volume[0]);
    CHECK_EQ(dirty.channel_pan[1], clean.channel_pan[1]);
}

// Источник, считающий прочитанные байты: отказ по месту обязан обойтись
// без чтения сэмпла (у WC чтение и повтор - перемотка файла).
struct CountingSource {
    formats::ByteSource inner;
    uint32_t read_bytes = 0;
    static uint32_t read(void* self, void* dst, uint32_t n) {
        auto* c             = static_cast<CountingSource*>(self);
        const uint32_t got  = c->inner.read(c->inner.self, dst, n);
        c->read_bytes      += got;
        return got;
    }
    static bool seek(void* self, uint32_t offset) {
        auto* c = static_cast<CountingSource*>(self);
        return c->inner.seek(c->inner.self, offset);
    }
    static uint32_t size(void* self) {
        auto* c = static_cast<CountingSource*>(self);
        return c->inner.size(c->inner.self);
    }
    formats::ByteSource as_byte_source() { return {this, &read, &seek, &size}; }
};

// Оценка места до чтения (resident_pages) равна взятым страницам у каждого
// сэмпла; не влезающий сэмпл отказывает без чтения и без взятых страниц.
void check_pages_estimate(const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource msrc(bytes.data(), static_cast<uint32_t>(bytes.size()));
    CountingSource src{msrc.as_byte_source()};
    player::load::SessionLoadResult load;
    CHECK(player::load::run_session_load(src.as_byte_source(), mem, song, load, /*metadata_only=*/true));
    uint32_t checked = 0;
    int32_t big      = -1;
    for (uint16_t i = 0; i < song.sample_count; ++i) {
        const soundsinth::model::SampleDescriptor& sd = song.samples[i];
        if (!soundsinth::model::sample_is_resident(sd) || sd.length_samples == 0) continue;
        const uint32_t free_before = memory::psram_free_page_count(mem.psram);
        if (!player::load::load_track_sample(load.format, src.as_byte_source(), mem, song, i)) continue;
        const uint32_t taken = free_before - memory::psram_free_page_count(mem.psram);
        CHECK_EQ(taken, soundsinth::model::resident_pages(sd.resident_encoding, sd.length_samples));
        ++checked;
        if (big < 0 || sd.length_samples > song.samples[big].length_samples) big = i;
    }
    // Самый длинный сэмпл - снова, при памяти на страницу меньше нужного.
    if (big >= 0) {
        const soundsinth::model::SampleDescriptor& sd = song.samples[big];
        memory::SampleCacheEntry* e                   = memory::sample_cache_find(mem.sample_cache, static_cast<uint16_t>(big));
        if (e != nullptr) memory::sample_cache_evict(mem.sample_cache, mem.psram, e);
        const uint32_t need = soundsinth::model::resident_pages(sd.resident_encoding, sd.length_samples);
        while (memory::psram_free_page_count(mem.psram) > need - 1u) {
            if (memory::psram_alloc_page(mem.psram) == memory::kPageChainEnd) break;
        }
        const uint32_t free_before = memory::psram_free_page_count(mem.psram);
        const uint32_t read_before = src.read_bytes;
        const char* why            = nullptr;
        CHECK(!player::load::load_track_sample(load.format, src.as_byte_source(), mem, song, static_cast<uint16_t>(big), &why));
        CHECK(why != nullptr && std::strcmp(why, soundsinth::model::kPsramFull) == 0);
        CHECK_EQ(src.read_bytes, read_before);
        CHECK_EQ(memory::psram_free_page_count(mem.psram), free_before);
    }
    std::printf("  %s: the estimate equals the pages taken for %u samples\n", path, checked);
    memory::track_memory_destroy(mem);
}

void test_pages_estimate_matches_and_refuses_without_reading() {
    std::printf("test_pages_estimate_matches_and_refuses_without_reading\n");
    check_pages_estimate("SD/test_music/it/00009.it");
    check_pages_estimate("SD/test_music/it/ivi-lite__v61.it");
    check_pages_estimate("SD/test_music/s3m/2nd_reality.s3m");
    check_pages_estimate("SD/test_music/xm/final_fantasy.xm");
    check_pages_estimate("SD/test_music/xm/000h_cara_mia.xm");
    check_pages_estimate("SD/test_music/mod/star_wars.mod");
}

// Состав плана, собранного проходом длительности, обязан совпасть с тем,
// что даёт линейный обход. Проверяется на настоящих файлах: массивы плана
// лежат в том же loader_scratch_buffer, который разбор занимает под свои
// буферы, и привязка плана раньше конца разбора затёрла бы их.
void check_plan_from_duration_pass(const char* path) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    CHECK(!bytes.empty());
    const uint32_t size = static_cast<uint32_t>(bytes.size());

    // Линейный обход: план строится после разбора, как было раньше.
    static memory::TrackMemory mem_ref;
    memory::track_memory_create(mem_ref);
    static soundsinth::model::Song song_ref;
    std::vector<uint16_t> ref_plan(player::load::kProgressiveMaxSamples);
    std::vector<uint16_t> ref_last(player::load::kProgressiveMaxSamples);
    std::vector<uint16_t> ref_first(player::load::kProgressiveMaxSamples);
    uint16_t ref_count = 0;
    {
        formats::MemoryByteSource src(bytes.data(), size);
        player::load::SessionLoadResult load;
        CHECK(player::load::run_session_load(src.as_byte_source(), mem_ref, song_ref, load, true));
        const player::load::PlaybackPlan p =
            player::load::plan_playback_order(song_ref, mem_ref.psram, ref_plan.data(), player::load::kProgressiveMaxSamples, ref_last.data(),
                                              player::load::LoadOrder::ByFile, player::load::kPrefetchOrderPositions, ref_first.data());
        ref_count = p.count;
    }
    memory::track_memory_destroy(mem_ref);

    // Сбор проходом длительности: массивы отданы загрузке.
    static memory::TrackMemory mem;
    memory::track_memory_create(mem);
    static soundsinth::model::Song song;
    std::vector<uint16_t> got(3u * player::load::kProgressiveMaxSamples);
    {
        formats::MemoryByteSource src(bytes.data(), size);
        player::load::SessionLoadResult load;
        CHECK(player::load::run_session_load(src.as_byte_source(), mem, song, load, true, got.data(), player::load::kProgressiveMaxSamples));
        CHECK(load.plan_usable);
        CHECK_EQ(load.plan_count, ref_count);
        // Состав: тот же набор сэмплов и те же крайние позиции.
        for (uint16_t i = 0; i < load.plan_count; ++i) {
            const uint16_t idx = got[i];
            CHECK(idx < song.sample_count);
            CHECK_EQ(got[player::load::kProgressiveMaxSamples + idx], ref_last[idx]);
            CHECK_EQ(got[2u * player::load::kProgressiveMaxSamples + idx], ref_first[idx]);
        }
    }
    memory::track_memory_destroy(mem);
}

void test_plan_from_duration_pass_matches_linear() {
    std::printf("test_plan_from_duration_pass_matches_linear\n");
    check_plan_from_duration_pass("SD/test_music/it/00009.it");
    check_plan_from_duration_pass("SD/test_music/xm/000h_cara_mia.xm");
    check_plan_from_duration_pass("SD/test_music/mod/star_wars.mod");
    check_plan_from_duration_pass("SD/test_music/s3m/2nd_reality.s3m");
}

void run_deferred_sample_load_tests() {
    test_session_load_resets_song();
    test_deferred_sample_load_matches_eager_bit_exact();
    test_metadata_only_then_planned_order_matches_eager();
    test_every_sample_loads_standalone();
    test_pages_estimate_matches_and_refuses_without_reading();
    test_plan_from_duration_pass_matches_linear();
}
