// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/bank/bank_reader.h"
#include "core/codec/block_locate.h"
#include "core/codec/dpcm8.h"
#include "core/model/instrument.h"
#include "core/memory/psram_store.h"

// Таблица распаковки из модели банка, как у загрузчика .mid. Одна на всех:
// у банков одного теста модель одна.
static const soundsinth::bank::BankDecodeTable* decode_table(const soundsinth::bank::Bank& b) {
    static soundsinth::bank::BankDecodeTable table;
    soundsinth::bank::bank_build_decode_table(*b.model, table);
    return &table;
}

using namespace soundsinth;

namespace {

// Банк на карте памяти в адресное пространство не попадает: PCM читается
// кусками через Bank::pcm_source, а не по указателю. Ветка одна на два
// места (флеш и карта), и разойтись она может молча - шумом в одном
// сэмпле из тысячи. Поэтому проверяем напрямую: тот же банк, те же
// сэмплы, два пути распаковки, побайтовое совпадение цепочек.
//
// Источник здесь читает из того же блоба: проверяется механизм
// дозагрузки, а не диск.

struct Feed {
    const uint8_t* pcm;
    uint32_t bytes;
    uint32_t calls;
    uint32_t max_chunk;
    uint32_t run_end  = 0xffffffffu; // конец прогона текущего сэмпла
    uint32_t past_run = 0;           // запросов за конец прогона
};

uint32_t feed_read(void* user, uint32_t offset, uint8_t* dst, uint32_t bytes) {
    Feed* f = static_cast<Feed*>(user);
    if (offset >= f->bytes) return 0;
    const uint32_t n = bytes < f->bytes - offset ? bytes : f->bytes - offset;
    std::memcpy(dst, f->pcm + offset, n);
    ++f->calls;
    if (n > f->max_chunk) f->max_chunk = n;
    if (offset + n > f->run_end) ++f->past_run;
    return n;
}

// Собирает цепочку страниц обратно в плоский буфер.
std::vector<uint8_t> flatten(memory::PsramStore& store, uint16_t first, uint32_t bytes) {
    std::vector<uint8_t> out(bytes);
    uint32_t done = 0;
    uint16_t page = first;
    while (done < bytes && page != memory::kPageChainEnd) {
        const uint32_t chunk = bytes - done < memory::kPsramPageBytes ? bytes - done : memory::kPsramPageBytes;
        std::memcpy(out.data() + done, memory::psram_page_ptr(store, page), chunk);
        done += chunk;
        page  = memory::psram_page_next(store, page);
    }
    out.resize(done);
    return out;
}

void test_bank_pcm_source_matches_pointer(const char* bank_path, uint32_t step) {
    std::printf("test_bank_pcm_source_matches_pointer: %s\n", bank_path);

    std::ifstream in(bank_path, std::ios::binary);
    if (!in) {
        std::printf("  SKIP: bank not found (bake it: sf2bake)\n");
        return;
    }
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    bank::Bank direct;
    const char* err = nullptr;
    if (!bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), direct, &err)) {
        // Банк в release/ - локальный артефакт, не часть репозитория, и легко
        // оказывается испечённым предыдущей версией формата. Это устаревший
        // файл, а не провал кода: тест должен сообщить об этом, а не падать.
        std::printf("  SKIP: bank did not open (%s), bake it again\n", err ? err : "?");
        return;
    }

    // Тот же банк, но PCM только через источник. Таблицы остаются на
    // месте: на плате они лежат в PSRAM, тоже по указателю.
    Feed feed{direct.pcm, direct.header->pcm_bytes, 0, 0};
    bank::Bank chunked = direct;
    chunked.pcm        = nullptr;
    chunked.pcm_source = bank::BankPcmSource{feed_read, &feed};

    static memory::PsramStore store_a;
    static memory::PsramStore store_b;
    memory::psram_create(store_a);
    (void)memory::psram_freeze_pattern_zone(store_a); // паттернов нет: блок трека - сэмплам
    memory::psram_create(store_b);
    (void)memory::psram_freeze_pattern_zone(store_b); // паттернов нет: блок трека - сэмплам

    uint32_t checked = 0, with_checkpoints = 0;
    const uint32_t count = direct.header->sample_count;
    // step 17 - как в сквозной проверке sf2bake: разные размеры, оба кодека,
    // с чекпоинтами и без; step 1 - все сэмплы.
    for (uint32_t i = 0; i < count; i += step) {
        const bank::BankSample& s = direct.samples[i];
        if (s.pcm_bytes == 0) continue;
        feed.run_end = s.pcm_offset + s.pcm_packed_bytes;

        memory::psram_reset_track(store_a);
        (void)memory::psram_freeze_pattern_zone(store_a); // паттернов нет: блок трека - сэмплам
        memory::psram_reset_track(store_b);
        (void)memory::psram_freeze_pattern_zone(store_b); // паттернов нет: блок трека - сэмплам
        uint16_t cp_a = memory::kPageChainEnd, cp_b = memory::kPageChainEnd;
        const uint16_t fa = bank::bank_make_resident(direct, decode_table(direct), static_cast<uint16_t>(i), store_a, &cp_a);
        const uint16_t fb = bank::bank_make_resident(chunked, decode_table(chunked), static_cast<uint16_t>(i), store_b, &cp_b);
        CHECK(fa != memory::kPageChainEnd);
        CHECK(fb != memory::kPageChainEnd);

        const std::vector<uint8_t> a = flatten(store_a, fa, s.pcm_bytes);
        const std::vector<uint8_t> b = flatten(store_b, fb, s.pcm_bytes);
        CHECK(a.size() == s.pcm_bytes);
        CHECK(a == b);
        // Страница чекпоинтов вычисляется арифметикой от размера, но сверить её
        // стоит: путь один, ошибка была бы общей.
        CHECK((cp_a == memory::kPageChainEnd) == (cp_b == memory::kPageChainEnd));
        if (s.checkpoint_count) ++with_checkpoints;
        ++checked;
    }

    CHECK(checked > 0);
    CHECK(with_checkpoints > 0);                    // иначе проверили только короткие
    CHECK(feed.calls > 0);                          // источник действительно работал
    CHECK(feed.max_chunk <= bank::kBankInputBytes); // кусками, а не прогоном целиком
    CHECK_EQ(feed.past_run, 0u);                    // чтение не выходит за прогон сэмпла
    std::printf("  samples %u (with checkpoints %u), source accesses %u, past the end of the run %u\n", checked, with_checkpoints, feed.calls, feed.past_run);
}

// Второй случай - то, что делает плата с картой: в памяти только
// таблицы, PCM читается из файла по требованию. Здесь stdio-файл вместо
// FatFs, но выше по стеку всё то же: bank_open(tables_only), источник,
// дозагрузка кусками.
struct FileFeed {
    std::FILE* f;
    uint32_t pcm_base;
};

uint32_t file_read(void* user, uint32_t offset, uint8_t* dst, uint32_t bytes) {
    FileFeed* ff = static_cast<FileFeed*>(user);
    if (std::fseek(ff->f, static_cast<long>(ff->pcm_base + offset), SEEK_SET) != 0) return 0;
    return static_cast<uint32_t>(std::fread(dst, 1, bytes, ff->f));
}

void test_bank_tables_only_from_file() {
    std::printf("test_bank_tables_only_from_file\n");

    std::FILE* f = std::fopen("release/banks/GeneralUser-GS.ssb", "rb");
    if (!f) {
        std::printf("  SKIP: no bank\n");
        return;
    }

    // Таблицы читаются целиком, PCM не читается вовсе - как на плате.
    bank::BankHeader head{};
    CHECK(std::fread(&head, 1, sizeof(head), f) == sizeof(head));
    std::vector<uint8_t> tables(head.pcm_offset);
    CHECK(std::fseek(f, 0, SEEK_SET) == 0);
    CHECK(std::fread(tables.data(), 1, tables.size(), f) == tables.size());

    bank::Bank sd;
    const char* err = nullptr;
    if (!bank::bank_open(tables.data(), head.pcm_offset, sd, &err, /*tables_only=*/true)) {
        std::printf("  SKIP: bank did not open (%s), bake it again\n", err ? err : "?");
        std::fclose(f);
        return;
    }
    CHECK(sd.pcm == nullptr);
    FileFeed ff{f, head.pcm_offset};
    sd.pcm_source = bank::BankPcmSource{file_read, &ff};

    // Эталон - тот же банк, загруженный целиком.
    std::ifstream in("release/banks/GeneralUser-GS.ssb", std::ios::binary);
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank whole;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), whole, &err));

    static memory::PsramStore store_a;
    static memory::PsramStore store_b;
    memory::psram_create(store_a);
    (void)memory::psram_freeze_pattern_zone(store_a); // паттернов нет: блок трека - сэмплам
    memory::psram_create(store_b);
    (void)memory::psram_freeze_pattern_zone(store_b); // паттернов нет: блок трека - сэмплам

    uint32_t checked = 0;
    for (uint32_t i = 0; i < whole.header->sample_count; i += 53) {
        const bank::BankSample& s = whole.samples[i];
        if (s.pcm_bytes == 0) continue;
        memory::psram_reset_track(store_a);
        (void)memory::psram_freeze_pattern_zone(store_a); // паттернов нет: блок трека - сэмплам
        memory::psram_reset_track(store_b);
        (void)memory::psram_freeze_pattern_zone(store_b); // паттернов нет: блок трека - сэмплам
        const uint16_t fa = bank::bank_make_resident(whole, decode_table(whole), static_cast<uint16_t>(i), store_a, nullptr);
        const uint16_t fb = bank::bank_make_resident(sd, decode_table(sd), static_cast<uint16_t>(i), store_b, nullptr);
        CHECK(fa != memory::kPageChainEnd);
        CHECK(fb != memory::kPageChainEnd);
        CHECK(flatten(store_a, fa, s.pcm_bytes) == flatten(store_b, fb, s.pcm_bytes));
        ++checked;
    }
    CHECK(checked > 0);
    std::printf("  samples %u, tables %lu KB in memory instead of %lu MB of bank\n", checked, (unsigned long)(head.pcm_offset / 1024u),
                (unsigned long)(head.total_bytes / 1048576u));
    std::fclose(f);
}

// Контрольные точки банка на пути платы (таблицы в памяти, PCM - файлом):
// у каждого сэмпла Dpcm8 точек ceil(длина/256), первая страница точек -
// та, что вернул bank_make_resident, и точка k равна предиктору
// последовательной распаковки перед отсчётом 256*k. Ошибка вычисления
// страницы точек в fill_pages общая для обоих путей и сверкой путей не
// ловится.
void test_bank_checkpoints_match_sequential_decode(const char* bank_path) {
    std::printf("test_bank_checkpoints_match_sequential_decode: %s\n", bank_path);
    std::FILE* f = std::fopen(bank_path, "rb");
    if (!f) {
        std::printf("  SKIP: no bank\n");
        return;
    }
    bank::BankHeader head{};
    CHECK(std::fread(&head, 1, sizeof(head), f) == sizeof(head));
    std::vector<uint8_t> tables(head.pcm_offset);
    CHECK(std::fseek(f, 0, SEEK_SET) == 0);
    CHECK(std::fread(tables.data(), 1, tables.size(), f) == tables.size());
    bank::Bank sd;
    if (!bank::bank_open(tables.data(), head.pcm_offset, sd, nullptr, /*tables_only=*/true)) {
        std::printf("  SKIP: bank did not open\n");
        std::fclose(f);
        return;
    }
    FileFeed ff{f, head.pcm_offset};
    sd.pcm_source = bank::BankPcmSource{file_read, &ff};

    static memory::PsramStore store;
    memory::psram_create(store);
    (void)memory::psram_freeze_pattern_zone(store); // паттернов нет: блок трека - сэмплам
    uint32_t samples = 0, points = 0, bad_count = 0, bad_points = 0, failed = 0;
    for (uint32_t i = 0; i < head.sample_count; ++i) {
        const bank::BankSample& s = sd.samples[i];
        if (s.resident_encoding != static_cast<uint8_t>(soundsinth::model::ResidentEncoding::Dpcm8)) continue;
        memory::psram_reset_track(store);
        (void)memory::psram_freeze_pattern_zone(store); // паттернов нет: блок трека - сэмплам
        uint16_t cp_first    = memory::kPageChainEnd;
        const uint16_t first = bank::bank_make_resident(sd, decode_table(sd), static_cast<uint16_t>(i), store, &cp_first);
        if (first == memory::kPageChainEnd) {
            ++failed;
            continue;
        }
        ++samples;
        const uint32_t want = (s.length_samples + dpcm8::kCheckpointIntervalSamples - 1) / dpcm8::kCheckpointIntervalSamples;
        if (s.checkpoint_count != want || (want > 0 && cp_first == memory::kPageChainEnd)) {
            ++bad_count;
            continue;
        }
        dpcm8::Dpcm8State st{};
        uint16_t page = first;
        uint32_t pos  = 0;
        for (uint32_t n = 0; n < s.length_samples; ++n) {
            if (n % dpcm8::kCheckpointIntervalSamples == 0) {
                ++points;
                if (dpcm8::read_checkpoint(store, cp_first, n / dpcm8::kCheckpointIntervalSamples).predictor != st.predictor) ++bad_points;
            }
            if (pos == memory::kPsramPageBytes) {
                page = memory::psram_page_next(store, page);
                pos  = 0;
            }
            dpcm8::decode_delta(memory::psram_page_ptr(store, page)[pos++], st);
        }
    }
    std::printf("  Dpcm8 samples %u, points %u, wrong count %u, differences %u, failed to decode %u\n", samples, points, bad_count, bad_points, failed);
    CHECK(samples > 0);
    CHECK_EQ(bad_count, 0u);
    CHECK_EQ(bad_points, 0u);
    CHECK_EQ(failed, 0u);
    memory::psram_destroy(store);
    std::fclose(f);
}

// Источник отказывает посреди прогона: сэмпл не публикуется (не полка
// шкалы), цепочка возвращена, причина - чтение. Обслуживание шины
// зовётся между страницами.
struct FailingFeed {
    Feed feed;
    uint32_t fail_at_call;
};

uint32_t failing_read(void* user, uint32_t offset, uint8_t* dst, uint32_t bytes) {
    FailingFeed* f = static_cast<FailingFeed*>(user);
    if (f->feed.calls >= f->fail_at_call) return 0;
    return feed_read(&f->feed, offset, dst, bytes);
}

uint32_t g_serve_calls = 0;
void count_serve(void*) {
    ++g_serve_calls;
}

void test_bank_pcm_source_read_failure(const char* bank_path) {
    std::printf("test_bank_pcm_source_read_failure: %s\n", bank_path);
    std::ifstream in(bank_path, std::ios::binary);
    if (!in) {
        std::printf("  SKIP: bank not found\n");
        return;
    }
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank direct;
    if (!bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), direct, nullptr)) {
        std::printf("  SKIP: bank did not open\n");
        return;
    }
    uint32_t idx = 0;
    while (idx < direct.header->sample_count && direct.samples[idx].pcm_packed_bytes < 8192u)
        ++idx;
    CHECK(idx < direct.header->sample_count);
    if (idx >= direct.header->sample_count) return;

    FailingFeed ff{{direct.pcm, direct.header->pcm_bytes, 0, 0}, 3};
    bank::Bank chunked = direct;
    chunked.pcm        = nullptr;
    chunked.pcm_source = bank::BankPcmSource{failing_read, &ff};
    chunked.serve      = &count_serve;

    static memory::PsramStore store;
    memory::psram_create(store);
    (void)memory::psram_freeze_pattern_zone(store); // паттернов нет: блок трека - сэмплам
    memory::psram_reset_track(store);
    (void)memory::psram_freeze_pattern_zone(store); // паттернов нет: блок трека - сэмплам
    const uint32_t free_before = memory::psram_free_page_count(store);
    g_serve_calls              = 0;
    bool read_failed           = false;
    uint16_t cp                = memory::kPageChainEnd;
    const uint16_t first       = bank::bank_make_resident(chunked, decode_table(chunked), static_cast<uint16_t>(idx), store, &cp, &read_failed);
    CHECK(first == memory::kPageChainEnd);
    CHECK(read_failed);
    CHECK_EQ(memory::psram_free_page_count(store), free_before);
    CHECK_EQ(ff.feed.calls, 3u); // после отказа источник больше не зовётся
    CHECK(g_serve_calls > 0);

    // Исправный источник: причина не взводится.
    memory::psram_reset_track(store);
    (void)memory::psram_freeze_pattern_zone(store); // паттернов нет: блок трека - сэмплам
    Feed ok{direct.pcm, direct.header->pcm_bytes, 0, 0};
    chunked.pcm_source = bank::BankPcmSource{feed_read, &ok};
    CHECK(bank::bank_make_resident(chunked, decode_table(chunked), static_cast<uint16_t>(idx), store, &cp, &read_failed) != memory::kPageChainEnd);
    CHECK(!read_failed);
}

// Поля заголовка вне CRC: испорченное смещение или счётчик - отказ с
// причиной, а не таблица за пределами блоба.
void test_bank_open_rejects_bad_header(const char* bank_path) {
    std::printf("test_bank_open_rejects_bad_header: %s\n", bank_path);
    std::ifstream in(bank_path, std::ios::binary);
    if (!in) {
        std::printf("  SKIP: bank not found\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank b;
    if (!bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), b, nullptr)) {
        std::printf("  SKIP: bank did not open\n");
        return;
    }
    const bank::BankHeader good = *b.header;
    auto rejects                = [&](void (*spoil)(bank::BankHeader&)) {
        std::vector<uint8_t> copy = blob;
        bank::BankHeader h        = good;
        spoil(h);
        std::memcpy(copy.data(), &h, sizeof(h));
        bank::Bank out;
        const char* err = nullptr;
        const bool ok   = bank::bank_open(copy.data(), static_cast<uint32_t>(copy.size()), out, &err);
        return !ok && err != nullptr && !out.valid();
    };
    CHECK(rejects([](bank::BankHeader& h) { h.presets_offset = 0x7FFFFFF0u; }));
    CHECK(rejects([](bank::BankHeader& h) { h.layers_offset = h.pcm_offset; }));
    CHECK(rejects([](bank::BankHeader& h) { h.instruments_offset = h.pcm_offset + 16u; }));
    CHECK(rejects([](bank::BankHeader& h) { h.samples_offset = h.pcm_offset - 4u; }));
    CHECK(rejects([](bank::BankHeader& h) { h.model_offset = h.pcm_offset - 4u; }));
    CHECK(rejects([](bank::BankHeader& h) { h.sample_count = 65535; }));
    CHECK(rejects([](bank::BankHeader& h) { h.keymap_count = 0xFFFFFFFFu; }));
    CHECK(rejects([](bank::BankHeader& h) { h.pcm_offset = 40; }));
    CHECK(rejects([](bank::BankHeader& h) { h.magic ^= 1u; }));
    CHECK(rejects([](bank::BankHeader& h) { ++h.version; }));

    // Обрезанный блоб: целиком - отказ на байт короче; только таблицы -
    // ровно до PCM открывается, на байт короче - нет.
    bank::Bank out;
    const char* err = nullptr;
    CHECK(!bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()) - 1u, out, &err) && err != nullptr);
    CHECK(bank::bank_open(blob.data(), good.pcm_offset, out, &err, true));
    CHECK(out.pcm == nullptr);
    CHECK(!bank::bank_open(blob.data(), good.pcm_offset - 1u, out, &err, true) && err != nullptr);
    CHECK(!bank::bank_open(blob.data(), static_cast<uint32_t>(sizeof(bank::BankHeader)) - 1u, out, &err));

    // Испорченная строка модели при верном CRC: сумма частот контекста не
    // та - банк отвергнут.
    auto rejects_model = [&](void (*spoil)(uint16_t* freq)) {
        std::vector<uint8_t> copy = blob;
        auto* freq                = reinterpret_cast<uint16_t*>(copy.data() + good.model_offset) + 3u * 256u;
        spoil(freq);
        bank::BankHeader h = good;
        h.table_crc32      = bank::bank_crc(copy.data() + sizeof(bank::BankHeader), good.pcm_offset - static_cast<uint32_t>(sizeof(bank::BankHeader)));
        std::memcpy(copy.data(), &h, sizeof(h));
        bank::Bank o;
        const char* e = nullptr;
        return !bank::bank_open(copy.data(), static_cast<uint32_t>(copy.size()), o, &e) && e != nullptr;
    };
    CHECK(rejects_model([](uint16_t* freq) { freq[100] = static_cast<uint16_t>(freq[100] + 1u); }));
    CHECK(rejects_model([](uint16_t* freq) { freq[255] = static_cast<uint16_t>(freq[255] - 1u); }));
}

// Ссылки внутри таблиц банка плата берёт без проверки: слой -> инструмент,
// инструмент -> keymap, сэмпл, огибающие; keymap -> сэмпл и по возрастанию
// (на этом стоит break в bank_lookup_note); прогон внутри зоны PCM; модель
// монотонна, cum[256] - сумма частот. Слоёв с vel_lo > vel_hi нет: такой
// слой не выбирается никогда, пекарь его не пишет.
void test_bank_structure(const char* bank_path) {
    std::printf("test_bank_structure: %s\n", bank_path);
    std::ifstream in(bank_path, std::ios::binary);
    if (!in) {
        std::printf("  SKIP: bank not found\n");
        return;
    }
    const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bank::Bank b;
    CHECK(bank::bank_open(blob.data(), static_cast<uint32_t>(blob.size()), b, nullptr));
    if (!b.valid()) return;
    const bank::BankHeader& h = *b.header;
    uint32_t bad_presets = 0, bad_layers = 0, inverted = 0, bad_inst = 0, unsorted = 0, bad_keymap = 0, bad_samples = 0, bad_env = 0, bad_model = 0;
    for (uint32_t i = 0; i < bank::kPresetSlots; ++i) {
        if (static_cast<uint32_t>(b.presets[i].first_layer) + bank::preset_layer_count(b.presets[i]) > h.layer_count) ++bad_presets;
    }
    for (uint32_t i = 0; i < h.layer_count; ++i) {
        if (b.layers[i].instrument >= h.instrument_count) ++bad_layers;
        if (b.layers[i].vel_lo > b.layers[i].vel_hi) ++inverted;
    }
    auto env_ok = [&](uint16_t e) { return e == bank::kNoIndex || e < h.envelope_count; };
    for (uint32_t i = 0; i < h.instrument_count; ++i) {
        const bank::BankInstrument& inst = b.instruments[i];
        if (static_cast<uint32_t>(inst.keymap_first) + inst.keymap_count > h.keymap_count ||
            (inst.default_sample != bank::kNoIndex && inst.default_sample >= h.sample_count) || !env_ok(inst.env_volume) || !env_ok(inst.env_panning) ||
            !env_ok(inst.env_pitch) || !env_ok(inst.env_filter)) {
            ++bad_inst;
            continue;
        }
        const bank::BankKeymapRange* r = b.keymap + inst.keymap_first;
        for (uint32_t k = 0; k < inst.keymap_count; ++k) {
            if (r[k].sample_index != bank::kNoSample && r[k].sample_index >= h.sample_count) ++bad_keymap;
            if (k > 0 && r[k].start_note <= r[k - 1].start_note) ++unsorted;
        }
    }
    for (uint32_t i = 0; i < h.sample_count; ++i) {
        const bank::BankSample& s = b.samples[i];
        if (static_cast<uint64_t>(s.pcm_offset) + s.pcm_packed_bytes > h.pcm_bytes) ++bad_samples;
    }
    for (uint32_t i = 0; i < h.envelope_count; ++i) {
        if (b.envelopes[i].point_count > bank::kMaxEnvelopePoints) ++bad_env;
    }
    if (!bank::model_valid(*b.model)) ++bad_model;
    std::printf("  presets %u, layers %u (inverted %u), instruments %u, keymap %u (not ascending %u), "
                "samples %u, envelopes %u, model %u\n",
                bad_presets, bad_layers, inverted, bad_inst, bad_keymap, unsorted, bad_samples, bad_env, bad_model);
    CHECK_EQ(bad_presets, 0u);
    CHECK_EQ(bad_layers, 0u);
    CHECK_EQ(bad_inst, 0u);
    CHECK_EQ(bad_keymap, 0u);
    CHECK_EQ(unsorted, 0u);
    CHECK_EQ(bad_samples, 0u);
    CHECK_EQ(bad_env, 0u);
    CHECK_EQ(bad_model, 0u);
    CHECK_EQ(inverted, 0u);
}

} // namespace

void run_bank_pcm_source_tests() {
    test_bank_open_rejects_bad_header("release/banks/GeneralUser-GS.ssb");
    test_bank_structure("release/banks/GeneralUser-GS.ssb");
    test_bank_structure("release/banks/SGM.ssb");
    test_bank_structure("release/banks/Timbres-of-Heaven.ssb");
    test_bank_pcm_source_read_failure("release/banks/GeneralUser-GS.ssb");
    // Оба банка, если оба испечены: у большого другой набор кодеков и
    // прогоны в сотни килобайт - ради этого дозагрузка кусками и заводилась.
    test_bank_pcm_source_matches_pointer("release/banks/GeneralUser-GS.ssb", 1);
    test_bank_pcm_source_matches_pointer("release/banks/Timbres-of-Heaven.ssb", 17);
    test_bank_tables_only_from_file();
    test_bank_checkpoints_match_sequential_decode("release/banks/GeneralUser-GS.ssb");
    test_bank_checkpoints_match_sequential_decode("release/banks/SGM.ssb");
    test_bank_checkpoints_match_sequential_decode("release/banks/Timbres-of-Heaven.ssb");
}
