// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdint>

#include "core/memory/psram_store.h"
#include "core/memory/scratch_arena.h"

// Выравнивание выдачи зоны паттернов PSRAM.
//
// Найденный отказ - HardFault на плате при загрузке .mid: cfsr=0x01000000,
// то есть UFSR.UNALIGNED. Аллокатор был побайтовым, и это сходило с рук,
// пока его единственным потребителем был упаковщик паттернов: тот
// работает байтами, выравнивание ему безразлично. Загрузчик MIDI первым
// положил в эту зону массив структур с uint32_t; копирование такой
// структуры компилятор делает через LDRD, а для LDRD невыровненный адрес
// на Cortex-M33 запрещён всегда, в отличие от обычного LDR.
//
// Почему отдельный тест: на x86 невыровненный доступ проходит молча и
// правильно, поэтому весь набор тестов на ПК был зелёным, пока плата
// падала. Здесь проверяется само свойство выравнивания, а не следствие,
// иначе оно снова окажется незаметным.

namespace {

using namespace soundsinth;

void run_alignment_tests() {
    std::printf("test_psram_pattern_alloc_is_4_aligned\n");

    memory::PsramStore psram{};
    memory::psram_create(psram);

    // База зоны должна быть выровнена сама: смещения считаются от неё.
    CHECK((reinterpret_cast<uintptr_t>(memory::psram_pattern_ptr(psram, 0)) & 3u) == 0u);

    // Нечётные размеры - то, что даёт загрузчик MIDI: число слоёв банка,
    // число дорожек, длина файла .mid.
    const uint32_t sizes[] = {1, 2, 3, 5, 7, 13, 49382, 6708, 1, 1, 255};
    uint32_t prev_end      = 0;
    for (uint32_t s : sizes) {
        const uint32_t off = memory::psram_pattern_alloc(psram, s);
        CHECK(off != memory::kPatternAllocFailed);
        CHECK((off & 3u) == 0u);
        CHECK((reinterpret_cast<uintptr_t>(memory::psram_pattern_ptr(psram, off)) & 3u) == 0u);
        // Выравнивание не должно приводить к наложению выдач: следующая всегда
        // не раньше конца предыдущей.
        CHECK(off >= prev_end);
        prev_end = off + s;
    }

    std::printf("  handouts checked %u, all on a 4 boundary\n", (unsigned)(sizeof(sizes) / sizeof(sizes[0])));

    // Переполнение блока по-прежнему отказ, а не молчаливая выдача за его
    // пределами; выравнивание не должно это сломать.
    const uint32_t huge = psram.track.bytes();
    CHECK(memory::psram_pattern_alloc(psram, huge) == memory::kPatternAllocFailed);

    memory::psram_destroy(psram);
}

// Малое хранилище: блок трека занимает его целиком, поэтому до заморозки
// страниц сэмплам нет вовсе - ни на плате, ни за концом page_next.
void test_small_store_has_no_free_pages() {
    std::printf("test_psram_small_store_has_no_free_pages\n");
    static memory::PsramStore store;
    memory::psram_create(store);
    memory::psram_set_track_bytes(store, 4u * memory::kPsramPageBytes);
    CHECK_EQ(store.track.pages, 4u);
    CHECK_EQ(memory::psram_free_page_count(store), 0u);
    // Временного нет - первая же просьба о странице усекает блок сама.
    CHECK(memory::psram_alloc_page(store) != memory::kPageChainEnd);
    CHECK_EQ(store.track.pages, 0u);
    CHECK_EQ(memory::psram_free_page_count(store), 3u);
    // Временное живо - усечения не будет, страниц нет.
    memory::psram_reset_track(store);
    CHECK(memory::psram_temp_alloc(store, 8u) != memory::kPatternAllocFailed);
    CHECK_EQ(memory::psram_alloc_page(store), memory::kPageChainEnd);
    // Паттернов нет - заморозка отдаёт сэмплам весь блок.
    CHECK_EQ(memory::psram_freeze_pattern_zone(store), 4u);
    memory::psram_set_track_bytes(store, memory::kPsramChipBytes);
    CHECK_EQ(store.track.pages, memory::kMaxSamplePageCount);
    memory::psram_destroy(store);
}

// Верхний край: весь чип, паттернов нет - страниц ровно kMaxSamplePageCount,
// все выдаются, счётчик сходится с проходом, следующая - конец цепочки.
void test_whole_chip_all_pages() {
    std::printf("test_psram_whole_chip_all_pages\n");
    static memory::PsramStore store;
    memory::psram_create(store);
    memory::psram_set_track_bytes(store, memory::kPsramChipBytes);
    CHECK_EQ(memory::psram_freeze_pattern_zone(store), memory::kMaxSamplePageCount);
    CHECK_EQ(memory::psram_free_list_length(store), memory::kMaxSamplePageCount);
    uint32_t got    = 0;
    bool counter_ok = true;
    while (memory::psram_alloc_page(store) != memory::kPageChainEnd) {
        ++got;
        if (got % 1024u == 0 && memory::psram_free_page_count(store) != memory::psram_free_list_length(store)) {
            counter_ok = false;
        }
    }
    CHECK_EQ(got, memory::kMaxSamplePageCount);
    CHECK(counter_ok);
    CHECK_EQ(memory::psram_free_page_count(store), 0u);
    memory::psram_destroy(store);
}

// Двойное освобождение цепочки: счётчик уходит выше числа страниц, флаг
// взведён и держится, пока список не пересобран.
void test_double_free_is_flagged() {
    std::printf("test_psram_double_free_is_flagged\n");
    static memory::PsramStore store;
    memory::psram_create(store);
    (void)memory::psram_freeze_pattern_zone(store); // паттернов нет: блок трека - сэмплам
    uint16_t first = memory::kPageChainEnd, prev = memory::kPageChainEnd;
    for (int i = 0; i < 10; ++i) {
        const uint16_t p = memory::psram_alloc_page(store);
        if (prev == memory::kPageChainEnd)
            first = p;
        else
            memory::psram_set_next(store, prev, p);
        memory::psram_set_next(store, p, memory::kPageChainEnd);
        prev = p;
    }
    memory::psram_free_chain(store, first);
    CHECK(!store.free_list_broken);
    memory::psram_free_chain(store, first);
    CHECK(store.free_list_broken);
    (void)memory::psram_alloc_page(store); // счётчик ниже - флаг держится
    CHECK(store.free_list_broken);
    memory::psram_reset_track(store);
    CHECK(!store.free_list_broken);
    memory::psram_destroy(store);
}

// ScratchArena: выравнивание после нечётного смещения, переполнение -
// nullptr без сдвига, count * sizeof больше 4 ГБ - отказ, а не заворот;
// замер растёт только на успехе; массив обнулён.
void test_scratch_arena() {
    std::printf("test_scratch_arena\n");
    alignas(8) uint8_t buf[64];
    for (uint8_t& b : buf)
        b = 0xAA;
    memory::ScratchArena a(buf, sizeof(buf));
    memory::g_max_scratch_used = 0;
    CHECK(a.alloc<uint8_t>(3) != nullptr);
    uint64_t* q = a.alloc<uint64_t>(2);
    CHECK(q != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(q) % alignof(uint64_t) == 0);
    CHECK(reinterpret_cast<uint8_t*>(q) == buf + 8);
    CHECK(q[0] == 0 && q[1] == 0);
    CHECK_EQ(a.offset, 24u);
    CHECK_EQ(memory::g_max_scratch_used, 24u);
    CHECK(a.alloc<uint32_t>(11) == nullptr);          // 24 + 44 > 64
    CHECK(a.alloc<uint32_t>(0x40000001u) == nullptr); // 4 ГБ + 4 байта
    CHECK_EQ(a.offset, 24u);
    CHECK_EQ(memory::g_max_scratch_used, 24u);
    CHECK(a.alloc<uint32_t>(10) != nullptr); // ровно до конца
    CHECK_EQ(a.offset, 64u);
    CHECK_EQ(memory::g_max_scratch_used, 64u);
}

// Данные трека растут снизу блока вверх, временное загрузчика - сверху
// вниз: на тесном блоке они встречаются, и это отказ, а не наложение. До
// заморозки страниц сэмплам нет вовсе - блок занимает хранилище целиком;
// заморозка отдаёт им и временное, и остаток.
void test_temp_allocator_meets_patterns() {
    std::printf("test_psram_temp_allocator_meets_patterns\n");
    static memory::PsramStore store;
    memory::psram_create(store);
    const uint32_t track = 8u * memory::kPsramPageBytes;
    memory::psram_set_track_bytes(store, track);
    CHECK_EQ(store.track.pages, 8u);

    // Временное на 3 страницы: сверху блока, выровнено на 8.
    const uint32_t t = memory::psram_temp_alloc(store, 3u * memory::kPsramPageBytes - 5u);
    CHECK(t != memory::kPatternAllocFailed);
    CHECK_EQ(t % 8u, 0u);
    CHECK(t >= track - 3u * memory::kPsramPageBytes);
    CHECK_EQ(memory::psram_alloc_page(store), memory::kPageChainEnd);

    // Навстречу до встречи: лишний байт у любой стороны - отказ.
    memory::psram_reset_track(store);
    const uint32_t big = memory::psram_temp_alloc(store, track - 4096u);
    CHECK(big != memory::kPatternAllocFailed);
    CHECK(memory::psram_pattern_alloc(store, big) != memory::kPatternAllocFailed); // ровно до границы
    CHECK_EQ(memory::psram_pattern_alloc(store, 4u), memory::kPatternAllocFailed);
    CHECK_EQ(memory::psram_temp_alloc(store, 8u), memory::kPatternAllocFailed);

    // Заморозка: занятое данными остаётся за блоком, остальное - сэмплам.
    memory::psram_reset_track(store);
    const uint32_t pat = memory::psram_pattern_alloc(store, 1000u);
    CHECK_EQ(pat, 0u);
    CHECK(memory::psram_temp_alloc(store, 2u * memory::kPsramPageBytes) != memory::kPatternAllocFailed);
    const uint32_t pages = memory::psram_freeze_pattern_zone(store);
    CHECK_EQ(store.track.pages, 1u);
    CHECK_EQ(pages, track / memory::kPsramPageBytes - 1u);
    uint32_t got = 0;
    while (memory::psram_alloc_page(store) != memory::kPageChainEnd)
        ++got;
    CHECK_EQ(got, pages);
    // Смещения внутри блока пережили усечение: данные на месте.
    CHECK_EQ(memory::psram_pattern_ptr(store, pat), store.base + memory::psram_track_byte_base(store));
    memory::psram_destroy(store);
}

} // namespace

void run_psram_pattern_alloc_tests() {
    test_temp_allocator_meets_patterns();
    test_scratch_arena();
    test_double_free_is_flagged();
    run_alignment_tests();
    test_small_store_has_no_free_pages();
    test_whole_chip_all_pages();
}
