#include "testing.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/codec/block_locate.h"
#include "core/codec/dpcm8.h"
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"
#include "core/codec/sample_pack.h"

using namespace soundsinth;

namespace {

// Декодирует блок block_index целиком (kCheckpointIntervalSamples
// отсчётов, меньше только у последнего блока) через контрольную точку:
// locate_block и прямой decode_block. Оракул произвольного доступа -
// голос позиционируется тем же locate_block. Возвращает записанное число
// отсчётов (0, если блок за пределами сэмпла).
uint32_t decode_block_from_checkpoint(memory::PsramStore& psram, uint16_t first_page, uint16_t checkpoint_first_page,
                                      uint32_t block_index, uint32_t sample_length, int16_t* out) {
    const uint32_t block_start = block_index * dpcm8::kCheckpointIntervalSamples;
    if (block_start >= sample_length) return 0;
    const uint32_t block_samples = (sample_length - block_start) < dpcm8::kCheckpointIntervalSamples
                                       ? (sample_length - block_start)
                                       : dpcm8::kCheckpointIntervalSamples;

    const dpcm8::BlockPosition pos = dpcm8::locate_block(psram, first_page, checkpoint_first_page, block_index);
    const uint8_t* block_bytes = memory::psram_page_ptr(psram, pos.page) + pos.byte_offset;
    dpcm8::decode_block(reinterpret_cast<const int8_t*>(block_bytes), block_samples, pos.state, out);
    return block_samples;
}

// Читает n сэмплов DPCM8, идя по цепочке страниц (см. memory::PsramStore),
// как engine::Voice при воспроизведении; здесь - для сверки
// результата упаковки с прямым dpcm8::decode_block.
void decode_from_page_chain(memory::PsramStore& psram, uint16_t first_page, dpcm8::Dpcm8State state, uint32_t n,
                             int16_t* out) {
    uint16_t page = first_page;
    uint32_t page_pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (page_pos >= memory::kPsramPageBytes) {
            page = memory::psram_page_next(psram, page);
            page_pos = 0;
        }
        const uint8_t byte = memory::psram_page_ptr(psram, page)[page_pos++];
        out[i] = dpcm8::decode_delta(byte, state);
    }
}

void test_dpcm8_multi_chunk_pack_matches_single_shot() {
    std::printf("test_sample_pack_dpcm8_multi_chunk_matches_single_shot\n");

    constexpr uint32_t kN = 4999; // несколько страниц PSRAM (1024), намеренно
    std::vector<int16_t> samples(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        samples[i] = static_cast<int16_t>(((i * 4099) % 30000) - 15000);
    }

    // Эталон - прямой однопроходный encode_block/decode_block.
    std::vector<int8_t> ref_dpcm(kN);
    dpcm8::Dpcm8State ref_state;
    uint32_t ref_cp_count = 0;
    std::vector<dpcm8::Dpcm8Checkpoint> ref_checkpoints((kN + dpcm8::kCheckpointIntervalSamples - 1) /
                                                          dpcm8::kCheckpointIntervalSamples);
    ref_cp_count = dpcm8::encode_block(samples.data(), kN, 0, ref_state, ref_dpcm.data(), ref_checkpoints.data(),
                                        static_cast<uint32_t>(ref_checkpoints.size()));
    std::vector<int16_t> ref_decoded(kN);
    dpcm8::decode_block(ref_dpcm.data(), kN, dpcm8::Dpcm8State{}, ref_decoded.data());

    // Прогон через SamplePacker несколькими кусками разного размера,
    // пересекающими границу страницы (1024): проверяется склейка между
    // вызовами add_samples(), а не сам DPCM8.
    memory::PsramStore psram;
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Dpcm8);

    uint32_t pos = 0;
    const uint32_t chunk_sizes[] = {700, 1300, 2999};
    for (uint32_t cs : chunk_sizes) {
        CHECK(packer.add_samples(samples.data() + pos, cs));
        pos += cs;
    }
    CHECK_EQ(pos, kN);

    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK_EQ(result.total_samples, kN);
    CHECK_EQ(result.checkpoint_count, ref_cp_count);
    CHECK(result.first_page != memory::kPageChainEnd);

    std::vector<int16_t> chained_decoded(kN);
    decode_from_page_chain(psram, result.first_page, dpcm8::Dpcm8State{}, kN, chained_decoded.data());

    for (uint32_t i = 0; i < kN; ++i) {
        CHECK_EQ(chained_decoded[i], ref_decoded[i]);
    }

    memory::psram_destroy(psram);
}

// Персистентные чекпоинты (PackResult::checkpoint_first_page) - тот же
// контракт, что был у старого sample_repack (см. историю файла), теперь
// для DPCM8: (1) каждый персистентный чекпоинт (dpcm8::read_checkpoint)
// побайтово совпадает с тем, что encode_block посчитал отдельным проходом
// на том же индексе; (2) decode_block_from_checkpoint (выше) для любого
// блока (не только первого) даёт те же сэмплы, что прямой decode_block с
// нуля, то есть произвольный доступ по чекпоинту точно эквивалентен
// полному линейному декоду.
void test_dpcm8_persisted_checkpoints_match_encoder_and_enable_random_access() {
    std::printf("test_sample_pack_dpcm8_persisted_checkpoints_enable_random_access\n");

    constexpr uint32_t kN = 4999;
    std::vector<int16_t> samples(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        samples[i] = static_cast<int16_t>(((i * 4099) % 30000) - 15000);
    }

    std::vector<int16_t> ref_decoded(kN);
    std::vector<dpcm8::Dpcm8Checkpoint> ref_checkpoints((kN + dpcm8::kCheckpointIntervalSamples - 1) /
                                                          dpcm8::kCheckpointIntervalSamples);
    {
        std::vector<int8_t> ref_dpcm(kN);
        dpcm8::Dpcm8State ref_state;
        uint32_t ref_cp_count = 0;
        ref_cp_count = dpcm8::encode_block(samples.data(), kN, 0, ref_state, ref_dpcm.data(), ref_checkpoints.data(),
                                            static_cast<uint32_t>(ref_checkpoints.size()));
        dpcm8::decode_block(ref_dpcm.data(), kN, dpcm8::Dpcm8State{}, ref_decoded.data());
    }

    memory::PsramStore psram;
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Dpcm8);
    CHECK(packer.add_samples(samples.data(), kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK(result.checkpoint_first_page != memory::kPageChainEnd);
    CHECK(result.checkpoint_count > 1); // 4999/256 -> несколько чекпоинтов, не вырожденный случай

    for (uint32_t k = 0; k < result.checkpoint_count; ++k) {
        const dpcm8::Dpcm8Checkpoint persisted = dpcm8::read_checkpoint(psram, result.checkpoint_first_page, k);
        CHECK_EQ(persisted.predictor, ref_checkpoints[k].predictor);
    }

    // Случайный доступ: декодируем каждый блок независимо через чекпоинт,
    // не по порядку (последний -> первый -> середина).
    const uint32_t block_count = result.checkpoint_count;
    std::vector<uint32_t> order = {block_count - 1, 0};
    if (block_count > 2) order.push_back(block_count / 2);
    for (uint32_t block_index : order) {
        int16_t block_out[dpcm8::kCheckpointIntervalSamples];
        const uint32_t got = decode_block_from_checkpoint(psram, result.first_page, result.checkpoint_first_page,
                                                          block_index, kN, block_out);
        const uint32_t block_start = block_index * dpcm8::kCheckpointIntervalSamples;
        const uint32_t expected = (kN - block_start) < dpcm8::kCheckpointIntervalSamples
                                       ? (kN - block_start)
                                       : dpcm8::kCheckpointIntervalSamples;
        CHECK_EQ(got, expected);
        for (uint32_t i = 0; i < got; ++i) {
            CHECK_EQ(block_out[i], ref_decoded[block_start + i]);
        }
    }

    memory::psram_destroy(psram);
}

// Raw8 - без чекпоинтов и декодера: резидентный байт совпадает с
// исходным 8-битным значением (в диапазоне [-128,127], без x256, см.
// sample_pack.h), доступен по индексу за O(число страниц) переходов.
void test_raw8_direct_index_access() {
    std::printf("test_sample_pack_raw8_direct_index_access\n");

    constexpr uint32_t kN = 3000; // > 2 страниц (1024 байт каждая)
    std::vector<int16_t> samples(kN);
    for (uint32_t i = 0; i < kN; ++i) {
        samples[i] = static_cast<int16_t>(static_cast<int8_t>(i * 37)); // произвольный узнаваемый паттерн в [-128,127]
    }

    memory::PsramStore psram;
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8);
    CHECK(packer.add_samples(samples.data(), kN));
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK_EQ(result.total_samples, kN);
    CHECK_EQ(result.checkpoint_count, static_cast<uint32_t>(0)); // Raw8 не пишет чекпоинты вовсе
    CHECK(result.checkpoint_first_page == memory::kPageChainEnd);

    // Проверяем произвольный (не последовательный) доступ по индексу:
    // страница = индекс/1024, смещение = индекс%1024 - копия того, что
    // делает engine/voice.cpp для Raw8-сэмплов.
    const uint32_t indices[] = {0, 1023, 1024, 2999, 1500};
    for (uint32_t idx : indices) {
        const uint16_t page = memory::psram_page_advance(psram, result.first_page, idx / memory::kPsramPageBytes);
        const uint16_t byte_offset = static_cast<uint16_t>(idx % memory::kPsramPageBytes);
        const int8_t raw = static_cast<int8_t>(memory::psram_page_ptr(psram, page)[byte_offset]);
        CHECK_EQ(static_cast<int16_t>(raw), samples[idx]);
    }

    memory::psram_destroy(psram);
}

// Прореживание в упаковщике: пары соседних отсчётов усредняются, непарный
// "хвост" ждёт следующего add_samples(), finish() отдаёт последний непарный
// отсчёт без усреднения (sample_pack.h).
void test_decimation_averages_pairs_across_calls() {
    std::printf("test_sample_pack_decimation_averages_pairs\n");

    memory::PsramStore psram;
    memory::psram_create(psram);
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8, /*decimate=*/true);

    // 7 исходных сэмплов, поданных неровными кусками (граница вызова
    // посередине пары 2/3 - проверка буферизации "хвоста") -> ожидаем 4
    // прореженных: avg(10,20)=15, avg(30,40)=35, avg(50,60)=55, непарный
    // 70 (finish, без усреднения).
    const int16_t chunk1[] = {10, 20, 30};
    const int16_t chunk2[] = {40, 50, 60, 70};
    CHECK(packer.add_samples(chunk1, 3));
    CHECK(packer.add_samples(chunk2, 4));

    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK_EQ(result.total_samples, static_cast<uint32_t>(4));

    const int16_t expected[4] = {15, 35, 55, 70};
    for (uint32_t i = 0; i < 4; ++i) {
        const uint16_t page = memory::psram_page_advance(psram, result.first_page, i / memory::kPsramPageBytes);
        const uint16_t byte_offset = static_cast<uint16_t>(i % memory::kPsramPageBytes);
        const int8_t raw = static_cast<int8_t>(memory::psram_page_ptr(psram, page)[byte_offset]);
        CHECK_EQ(static_cast<int16_t>(raw), expected[i]);
    }

    memory::psram_destroy(psram);
}

// Таблица чекпоинтов лежит в той же цепочке, что и данные.
//
// Это контракт для всех, кто освобождает результат: одного
// psram_free_chain(first_page) достаточно, второй вызов на
// checkpoint_first_page был бы двойным освобождением.
//
// Цена такой ошибки - не утечка. psram_free_chain() доходит до конца
// цепочки и подшивает её перед головой free-list; на втором проходе
// "хвостом" оказывается конец всего free-list, и он замыкается на
// голову. Список зацикливается, и одна страница выдаётся двум разным
// сэмплам. На железе это выглядело как "страниц свободно 6657/6656" -
// больше, чем их существует (лог 2026-08-25).
void test_checkpoints_share_the_sample_chain() {
    std::printf("test_checkpoints_share_the_sample_chain\n");

    memory::PsramStore psram;
    memory::psram_create(psram);
    const uint32_t all_free = memory::psram_free_page_count(psram);
    CHECK_EQ(all_free, psram.sample_page_count);

    // Достаточно длинный сэмпл, чтобы чекпоинтов было не ноль и они
    // заняли отдельные страницы.
    constexpr uint32_t kSamples = 40000;
    std::vector<int16_t> pcm(kSamples);
    for (uint32_t i = 0; i < kSamples; ++i) pcm[i] = static_cast<int16_t>((i * 37) % 4096 - 2048);

    sample_pack::SamplePacker packer(psram, soundsinth::model::ResidentEncoding::Dpcm8);
    packer.add_samples(pcm.data(), kSamples);
    const sample_pack::PackResult result = packer.finish();
    CHECK(result.ok);
    CHECK(result.checkpoint_count > 0);
    CHECK(result.checkpoint_first_page != memory::kPageChainEnd);
    CHECK(memory::psram_free_page_count(psram) < all_free); // что-то заняли

    // Главное: цепочка одна. Освобождаем только first_page - и свободной
    // памяти должно стать столько же, сколько было.
    memory::psram_free_chain(psram, result.first_page);
    CHECK_EQ(memory::psram_free_page_count(psram), all_free);

    memory::psram_destroy(psram);
}

// Прореживание на всех трёх кодировках: цепочка прореженного сэмпла
// побайтово равна цепочке без прореживания от вручную усреднённого
// массива, включая таблицу контрольных точек Dpcm8. Нечётная длина и
// неровные куски: непарный последний отсчёт обязан лечь в данные до
// таблицы точек, а пары - пережить границы вызовов.
void test_decimation_matches_manual_average_all_encodings() {
    std::printf("test_sample_pack_decimation_matches_manual_average_all_encodings\n");

    constexpr uint32_t kN = 5001; // нечётно; после прореживания 2501 - больше двух страниц
    std::vector<int16_t> src(kN);
    for (uint32_t i = 0; i < kN; ++i) src[i] = static_cast<int16_t>(((i * 7919) % 60000) - 30000);
    std::vector<int16_t> avg;
    for (uint32_t i = 0; i + 1 < kN; i += 2) {
        avg.push_back(static_cast<int16_t>((static_cast<int32_t>(src[i]) + static_cast<int32_t>(src[i + 1])) / 2));
    }
    avg.push_back(src[kN - 1]);

    const sample_pack::ResidentEncoding modes[] = {sample_pack::ResidentEncoding::Raw8,
                                                   sample_pack::ResidentEncoding::Raw16,
                                                   sample_pack::ResidentEncoding::Dpcm8};
    for (const sample_pack::ResidentEncoding mode : modes) {
        std::vector<int16_t> input = src;
        std::vector<int16_t> expect = avg;
        if (mode == sample_pack::ResidentEncoding::Raw8) {
            // Raw8 ждёт 8-битную шкалу источника.
            for (int16_t& v : input) v = static_cast<int16_t>(v / 256);
            expect.clear();
            for (uint32_t i = 0; i + 1 < kN; i += 2) {
                expect.push_back(
                    static_cast<int16_t>((static_cast<int32_t>(input[i]) + static_cast<int32_t>(input[i + 1])) / 2));
            }
            expect.push_back(input[kN - 1]);
        }

        memory::PsramStore psram_a, psram_b;
        memory::psram_create(psram_a);
        memory::psram_create(psram_b);

        sample_pack::SamplePacker dec(psram_a, mode, /*decimate=*/true);
        const uint32_t cuts[] = {1, 2, 3, 777, 1000, 1};
        uint32_t pos = 0;
        for (uint32_t c = 0; pos < kN; ++c) {
            const uint32_t want = cuts[c % 6];
            const uint32_t n = want < kN - pos ? want : kN - pos;
            CHECK(dec.add_samples(input.data() + pos, n));
            pos += n;
        }
        const sample_pack::PackResult ra = dec.finish();

        sample_pack::SamplePacker plain(psram_b, mode);
        CHECK(plain.add_samples(expect.data(), static_cast<uint32_t>(expect.size())));
        const sample_pack::PackResult rb = plain.finish();

        CHECK(ra.ok && rb.ok);
        CHECK_EQ(ra.total_samples, rb.total_samples);
        CHECK_EQ(ra.checkpoint_count, rb.checkpoint_count);
        CHECK_EQ(ra.checkpoint_first_page == memory::kPageChainEnd, rb.checkpoint_first_page == memory::kPageChainEnd);
        uint16_t pa = ra.first_page, pb = rb.first_page;
        uint32_t pages = 0;
        while (pa != memory::kPageChainEnd && pb != memory::kPageChainEnd) {
            CHECK(std::memcmp(memory::psram_page_ptr(psram_a, pa), memory::psram_page_ptr(psram_b, pb),
                              memory::kPsramPageBytes) == 0);
            if (pa == ra.checkpoint_first_page) CHECK(pb == rb.checkpoint_first_page);
            pa = memory::psram_page_next(psram_a, pa);
            pb = memory::psram_page_next(psram_b, pb);
            ++pages;
        }
        CHECK(pa == memory::kPageChainEnd && pb == memory::kPageChainEnd);
        CHECK(pages >= 3);

        memory::psram_destroy(psram_a);
        memory::psram_destroy(psram_b);
    }
}

// Счётчик свободных страниц (O(1)) обязан совпадать с проходом по
// free-list после каждой операции, которая список меняет: выдачи,
// освобождения цепочки, заморозки зоны паттернов, смены размера
// хранилища и сброса трека. Пропущенная точка обновления сменила бы
// кодек (Raw16 решается по этому числу) молча.
void test_free_page_counter_matches_walk() {
    std::printf("test_psram_free_page_counter_matches_walk\n");

    memory::PsramStore psram;
    memory::psram_create(psram);
    auto same = [&]() { return memory::psram_free_page_count(psram) == memory::psram_free_list_length(psram); };
    CHECK(same());

    // Две цепочки разной длины, одна освобождается в середине.
    uint16_t chain[2] = {memory::kPageChainEnd, memory::kPageChainEnd};
    for (int c = 0; c < 2; ++c) {
        uint16_t prev = memory::kPageChainEnd;
        for (int i = 0; i < 5 + c * 7; ++i) {
            const uint16_t p = memory::psram_alloc_page(psram);
            CHECK(p != memory::kPageChainEnd);
            if (prev == memory::kPageChainEnd) chain[c] = p; else memory::psram_set_next(psram, prev, p);
            memory::psram_set_next(psram, p, memory::kPageChainEnd);
            prev = p;
            CHECK(same());
        }
    }
    memory::psram_free_chain(psram, chain[0]);
    CHECK(same());
    memory::psram_free_chain(psram, chain[1]);
    CHECK(same());
    CHECK_EQ(memory::psram_free_page_count(psram), psram.sample_page_count);

    // Паттерны заняли 3 КБ с хвостиком - заморозка даёт другое число страниц.
    CHECK(memory::psram_pattern_alloc(psram, 3000) != memory::kPatternAllocFailed);
    memory::psram_freeze_pattern_zone(psram);
    CHECK(same());
    CHECK_EQ(memory::psram_free_page_count(psram), psram.sample_page_count);

    memory::psram_set_track_bytes(psram, memory::kBankTableOffset);
    CHECK(same());
    (void)memory::psram_alloc_page(psram);
    CHECK(same());
    memory::psram_reset_track(psram);
    CHECK(same());

    // Всё выбрать: счётчик доходит до нуля вместе со списком.
    while (memory::psram_alloc_page(psram) != memory::kPageChainEnd) {
    }
    CHECK_EQ(memory::psram_free_page_count(psram), 0u);
    CHECK(same());

    memory::psram_destroy(psram);
}

} // namespace

// Закрытие упаковки загрузчиком: опубликовать, а при любом отказе - оборванная
// заливка, переполненный каталог - вернуть все страницы ровно один раз.
// Раньше копии в загрузчиках расходились: S3M при полном каталоге терял
// цепочку и отвечал true, IT при неудачном finish() не освобождал её.
void test_finish_and_publish_frees_on_every_failure() {
    std::printf("test_finish_and_publish_frees_on_every_failure\n");

    memory::PsramStore psram;
    memory::psram_create(psram);
    static memory::SampleCacheCatalog catalog;
    memory::sample_cache_reset(catalog);
    const uint32_t all_free = memory::psram_free_page_count(psram);

    constexpr uint32_t kSamples = 40000;
    std::vector<int16_t> pcm(kSamples);
    for (uint32_t i = 0; i < kSamples; ++i) pcm[i] = static_cast<int16_t>((i * 37) % 4096 - 2048);
    auto pack = [&](sample_pack::SamplePacker& packer) { CHECK(packer.add_samples(pcm.data(), kSamples)); };

    // Успех: опубликован, страницы заняты.
    {
        sample_pack::SamplePacker packer(psram, soundsinth::model::ResidentEncoding::Dpcm8);
        pack(packer);
        const char* why = nullptr;
        CHECK(sample_pack::finish_and_publish(packer, true, psram, catalog, 7, &why));
        CHECK(why == nullptr);
        CHECK(memory::sample_cache_find(catalog, 7) != nullptr);
    }
    const uint32_t after_one = memory::psram_free_page_count(psram);
    CHECK(after_one < all_free);

    // Оборванная заливка: не публикуется, всё занятое возвращается.
    {
        sample_pack::SamplePacker packer(psram, soundsinth::model::ResidentEncoding::Dpcm8);
        pack(packer);
        CHECK(!sample_pack::finish_and_publish(packer, false, psram, catalog, 8, nullptr));
        CHECK(memory::sample_cache_find(catalog, 8) == nullptr);
        CHECK_EQ(memory::psram_free_page_count(psram), after_one);
    }

    // Полный каталог: причина названа, всё занятое возвращается.
    for (uint16_t i = 100; memory::sample_cache_alloc_slot(catalog, i, memory::kPageChainEnd) != nullptr; ++i) {
    }
    {
        sample_pack::SamplePacker packer(psram, soundsinth::model::ResidentEncoding::Dpcm8);
        pack(packer);
        const char* why = nullptr;
        CHECK(!sample_pack::finish_and_publish(packer, true, psram, catalog, 9, &why));
        CHECK(why != nullptr);
        CHECK_EQ(memory::psram_free_page_count(psram), after_one);
        CHECK_EQ(memory::psram_free_page_count(psram), memory::psram_free_list_length(psram));
    }

    memory::psram_destroy(psram);
}

// Таблица контрольных точек длиннее страницы: на странице 512 точек, у
// 200000 отсчётов их 782 - три страницы. read_checkpoint и locate_block
// обязаны идти по цепочке таблицы, голос со смещением за второй страницей -
// совпасть с полным проходом.
void test_checkpoint_table_spans_pages() {
    std::printf("test_sample_pack_checkpoint_table_spans_pages\n");
    constexpr uint32_t kN = 200000;
    constexpr uint32_t kPoints = (kN + dpcm8::kCheckpointIntervalSamples - 1) / dpcm8::kCheckpointIntervalSamples;
    std::vector<int16_t> src(kN);
    for (uint32_t i = 0; i < kN; ++i) src[i] = static_cast<int16_t>(((i * 2654435761u) >> 16) % 20000 - 10000);
    memory::PsramStore psram;
    memory::psram_create(psram);
    std::vector<dpcm8::Dpcm8Checkpoint> cp(kPoints);
    {
        std::vector<int8_t> bytes(kN);
        dpcm8::Dpcm8State st;
        CHECK_EQ(dpcm8::encode_block(src.data(), kN, 0, st, bytes.data(), cp.data(), kPoints), kPoints);
    }
    sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Dpcm8);
    CHECK(packer.add_samples(src.data(), kN));
    const sample_pack::PackResult r = packer.finish();
    CHECK(r.ok);
    CHECK_EQ(r.checkpoint_count, kPoints);
    for (const uint32_t k : {0u, 511u, 512u, 513u, 781u}) {
        CHECK_EQ(dpcm8::read_checkpoint(psram, r.checkpoint_first_page, k).predictor, cp[k].predictor);
    }
    // Полная распаковка - оракул для блоков через точки.
    std::vector<int16_t> full(kN);
    decode_from_page_chain(psram, r.first_page, dpcm8::Dpcm8State{}, kN, full.data());
    for (const uint32_t block : {600u, 781u}) {
        int16_t out[dpcm8::kCheckpointIntervalSamples];
        const uint32_t n = decode_block_from_checkpoint(psram, r.first_page, r.checkpoint_first_page, block, kN, out);
        CHECK(n > 0);
        for (uint32_t i = 0; i < n; ++i) CHECK_EQ(out[i], full[block * dpcm8::kCheckpointIntervalSamples + i]);
    }
    memory::psram_destroy(psram);
}

// Отказ PSRAM посреди сэмпла на странице точек: точек 782, вторая страница
// точек берётся на отсчёте 131072, после 128 страниц данных и первой
// страницы точек. Публикации нет, все страницы возвращены.
// Отказ в finish() на непарном отсчёте прореживания: причина названа.
void test_checkpoint_page_failure_and_reason() {
    std::printf("test_sample_pack_checkpoint_page_failure_and_reason\n");
    static memory::SampleCacheCatalog catalog;
    {
        constexpr uint32_t kN = 200000;
        std::vector<int16_t> src(kN);
        for (uint32_t i = 0; i < kN; ++i) src[i] = static_cast<int16_t>((i * 37) % 4096 - 2048);
        memory::PsramStore psram;
        memory::psram_create(psram);
        memory::sample_cache_reset(catalog);
        while (memory::psram_free_page_count(psram) > 130u) (void)memory::psram_alloc_page(psram);
        const uint32_t before = memory::psram_free_page_count(psram);
        sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Dpcm8);
        CHECK(!packer.add_samples(src.data(), 131073));
        CHECK_EQ(memory::psram_free_page_count(psram), 0u);
        CHECK(!sample_pack::finish_and_publish(packer, false, psram, catalog, 3, nullptr));
        CHECK(memory::sample_cache_find(catalog, 3) == nullptr);
        CHECK_EQ(memory::psram_free_page_count(psram), before);
        CHECK_EQ(memory::psram_free_page_count(psram), memory::psram_free_list_length(psram));
        memory::psram_destroy(psram);
    }
    {
        // 2049 отсчётов при прореживании: 1024 средних занимают страницу,
        // непарный последний уходит в finish() и страницы не находит.
        std::vector<int16_t> src(2049, 5);
        memory::PsramStore psram;
        memory::psram_create(psram);
        memory::sample_cache_reset(catalog);
        while (memory::psram_free_page_count(psram) > 1u) (void)memory::psram_alloc_page(psram);
        const uint32_t before = memory::psram_free_page_count(psram);
        sample_pack::SamplePacker packer(psram, sample_pack::ResidentEncoding::Raw8, /*decimate=*/true);
        CHECK(packer.add_samples(src.data(), 2049));
        const char* why = nullptr;
        CHECK(!sample_pack::finish_and_publish(packer, true, psram, catalog, 4, &why));
        CHECK(why != nullptr && std::strcmp(why, "PSRAM кончилась") == 0);
        CHECK_EQ(memory::psram_free_page_count(psram), before);
        memory::psram_destroy(psram);
    }
}

void run_sample_pack_tests() {
    test_checkpoint_table_spans_pages();
    test_checkpoint_page_failure_and_reason();
    test_dpcm8_multi_chunk_pack_matches_single_shot();
    test_dpcm8_persisted_checkpoints_match_encoder_and_enable_random_access();
    test_raw8_direct_index_access();
    test_decimation_averages_pairs_across_calls();
    test_decimation_matches_manual_average_all_encodings();
    test_free_page_counter_matches_walk();
    test_checkpoints_share_the_sample_chain();
    test_finish_and_publish_frees_on_every_failure();
}
