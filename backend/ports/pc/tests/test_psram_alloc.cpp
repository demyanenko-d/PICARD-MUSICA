// SPDX-License-Identifier: MIT
// Распределитель страниц PSRAM: проси, расширяй, освобождай.
//
// Проверяется то, на чём держится замысел: блок непрерывен, расширение
// идёт на месте, освобождённое возвращается целиком, а дробление видно
// через наибольший свободный прогон.

#include "testing.h"

#include "core/memory/psram_alloc.h"

namespace memory = soundsinth::memory;

namespace {

// Заняты ли ровно эти страницы и никакие другие сверх ожидаемого числа.
bool block_inside(const memory::PsramBlock& b, uint16_t page_count) {
    return b.valid() && static_cast<uint32_t>(b.first) + b.pages <= page_count;
}

} // namespace

void run_psram_alloc_tests() {
    std::printf("test_psram_alloc_basics\n");
    {
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 64);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 64);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_largest_run(a)), 64);

        memory::PsramBlock b1 = memory::psram_alloc(a, 10);
        CHECK(block_inside(b1, 64));
        CHECK_EQ(static_cast<int>(b1.pages), 10);
        CHECK_EQ(static_cast<int>(b1.bytes()), 10 * static_cast<int>(memory::kPsramPageBytes));
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 54);

        // Второй блок не налезает на первый.
        memory::PsramBlock b2 = memory::psram_alloc(a, 10);
        CHECK(block_inside(b2, 64));
        CHECK(b2.first >= b1.first + b1.pages || b1.first >= b2.first + b2.pages);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 44);

        memory::psram_alloc_free(a, b1);
        CHECK(!b1.valid());
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 54);
        // Повторное освобождение безвредно.
        memory::psram_alloc_free(a, b1);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 54);
    }

    std::printf("test_psram_alloc_extend_in_place\n");
    {
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 64);
        // Ровно случай загрузчика: просит десять, не хватило - прирастил.
        memory::PsramBlock b = memory::psram_alloc(a, 10);
        const uint16_t first = b.first;
        CHECK(memory::psram_alloc_extend(a, b, 10));
        CHECK_EQ(static_cast<int>(b.first), static_cast<int>(first)); // на месте
        CHECK_EQ(static_cast<int>(b.pages), 20);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 44);

        // Следом занято - расширить нельзя, блок не тронут.
        memory::PsramBlock other = memory::psram_alloc(a, 5);
        CHECK(other.valid());
        const uint16_t pages_before = b.pages;
        const uint16_t free_before  = memory::psram_alloc_free_pages(a);
        CHECK(!memory::psram_alloc_extend(a, b, 1));
        CHECK_EQ(static_cast<int>(b.pages), static_cast<int>(pages_before));
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), static_cast<int>(free_before));
    }

    std::printf("test_psram_alloc_loader_order\n");
    {
        // Порядок загрузчика: временное берётся до разбора, паттерны
        // растут после него. Обычного выделения для этого хватает -
        // временное ложится снизу, паттерны встают за ним, и над ними
        // пусто. Особых направлений роста не нужно.
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 64);
        memory::PsramBlock temp     = memory::psram_alloc(a, 4);
        memory::PsramBlock patterns = memory::psram_alloc(a, 1);
        CHECK(temp.valid() && patterns.valid());
        CHECK_EQ(static_cast<int>(patterns.first), static_cast<int>(temp.first + temp.pages));
        // Паттерны прирастают сколько угодно: над ними никого.
        CHECK(memory::psram_alloc_extend(a, patterns, 50));
        CHECK_EQ(static_cast<int>(patterns.pages), 51);
        // Разбор кончился - временное отдано, дыра внизу достаётся сэмплам.
        memory::psram_alloc_free(a, temp);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 13);
        memory::PsramBlock sample = memory::psram_alloc(a, 1);
        CHECK_EQ(static_cast<int>(sample.first), 0);
    }

    std::printf("test_psram_alloc_double_free_and_high_water\n");
    {
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 64);
        memory::PsramBlock b = memory::psram_alloc(a, 10);
        CHECK_EQ(static_cast<int>(a.high_water), 10);
        CHECK(memory::psram_alloc_extend(a, b, 5));
        CHECK_EQ(static_cast<int>(a.high_water), 15);

        // Копия блока, возвращённая после настоящего возврата, - учёт
        // соврёт, и дальше раздавались бы занятые страницы.
        memory::PsramBlock copy = b;
        memory::psram_alloc_free(a, b);
        CHECK_EQ(static_cast<int>(a.double_free), 0);
        memory::psram_alloc_free(a, copy);
        CHECK_EQ(static_cast<int>(a.double_free), 1);

        // Высшая точка не опускается возвратом: травить надо всё, что
        // трек успел занять.
        CHECK_EQ(static_cast<int>(a.high_water), 15);
        memory::psram_alloc_reset(a, 64);
        CHECK_EQ(static_cast<int>(a.high_water), 0);
        CHECK_EQ(static_cast<int>(a.double_free), 0);
    }

    std::printf("test_psram_alloc_shrink_keeps_start\n");
    {
        // Ровно случай загрузки трека: блок взят на весь свободный прогон,
        // на заморозке хвост уходит сэмплам, а смещения внутри переживают
        // усечение - начало блока не двигается.
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 64);
        memory::PsramBlock track = memory::psram_alloc(a, memory::psram_alloc_largest_run(a));
        CHECK_EQ(static_cast<int>(track.pages), 64);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 0);

        const uint16_t first = track.first;
        memory::psram_alloc_shrink(a, track, 10);
        CHECK_EQ(static_cast<int>(track.first), static_cast<int>(first)); // начало на месте
        CHECK_EQ(static_cast<int>(track.pages), 10);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 54);
        // Отданное достаётся сэмплам одним прогоном.
        CHECK_EQ(static_cast<int>(memory::psram_alloc_largest_run(a)), 54);
        memory::PsramBlock samples = memory::psram_alloc(a, 54);
        CHECK(samples.valid());
        CHECK_EQ(static_cast<int>(samples.first), static_cast<int>(first + 10));

        // Усечение до нынешнего размера и выше - ничего не меняет.
        memory::psram_alloc_shrink(a, track, 10);
        memory::psram_alloc_shrink(a, track, 99);
        CHECK_EQ(static_cast<int>(track.pages), 10);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 0);
        // До нуля - тот же возврат, что и освобождение.
        memory::psram_alloc_shrink(a, track, 0);
        CHECK(!track.valid());
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 10);
        CHECK_EQ(static_cast<int>(a.double_free), 0);
    }

    std::printf("test_psram_alloc_kept_survives_resize\n");
    {
        // Отказ платы 2026-10-01: кэш носителей взят при подъёме, потом банк
        // с карты ужал хранилище под свои таблицы - и отметка терялась.
        // Страницы кэша раздавались треку заново, кэш писал в паттерны, и
        // загрузчик видел "повреждённые данные паттерна".
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 100);
        memory::PsramBlock cache = memory::psram_alloc(a, 32);
        CHECK(cache.valid());
        CHECK(memory::psram_alloc_keep(a, cache));
        const uint16_t cache_first = cache.first;

        // Трек берёт остаток и возвращает его; кэш не трогается.
        memory::PsramBlock track = memory::psram_alloc(a, memory::psram_alloc_largest_run(a));
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 0);
        memory::psram_alloc_free(a, track);

        // Банк ужал хранилище: страниц стало 80, кэш остался занятым.
        memory::psram_alloc_release_track(a, 80);
        CHECK_EQ(static_cast<int>(a.kept_lost), 0);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 80 - 32);
        CHECK(memory::psram_alloc_page_busy(a, cache_first));
        CHECK(memory::psram_alloc_page_busy(a, static_cast<uint16_t>(cache_first + 31)));
        // Блок трека после ужатия не налезает на кэш.
        track = memory::psram_alloc(a, memory::psram_alloc_largest_run(a));
        CHECK(track.valid());
        CHECK(track.first >= cache_first + 32 || track.first + track.pages <= cache_first);

        // Отметка выше новой границы теряется и считается.
        memory::psram_alloc_free(a, track);
        memory::PsramBlock high = memory::psram_alloc(a, 8);
        CHECK(high.valid() && high.first >= 32);
        CHECK(memory::psram_alloc_keep(a, high));
        memory::psram_alloc_release_track(a, 36);
        CHECK_EQ(static_cast<int>(a.kept_lost), 1);
    }

    std::printf("test_psram_alloc_exhaustion\n");
    {
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 16);
        memory::PsramBlock all = memory::psram_alloc(a, 16);
        CHECK(all.valid());
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 0);
        // Больше нет ничего, даже одной страницы.
        memory::PsramBlock none = memory::psram_alloc(a, 1);
        CHECK(!none.valid());
        CHECK(!memory::psram_alloc_extend(a, all, 1));
        // Просьба больше чипа - пустой блок, а не порча.
        memory::psram_alloc_free(a, all);
        memory::PsramBlock too_big = memory::psram_alloc(a, 17);
        CHECK(!too_big.valid());
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 16);
    }

    std::printf("test_psram_alloc_fragmentation\n");
    {
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 30);
        memory::PsramBlock b1 = memory::psram_alloc(a, 10);
        memory::PsramBlock b2 = memory::psram_alloc(a, 10);
        memory::PsramBlock b3 = memory::psram_alloc(a, 10);
        CHECK(b1.valid() && b2.valid() && b3.valid());
        // Дыра в середине: свободно десять, но подряд только десять.
        memory::psram_alloc_free(a, b2);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 10);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_largest_run(a)), 10);
        // Одиннадцать подряд не найдётся, хотя свободных страниц столько нет.
        CHECK(!memory::psram_alloc(a, 11).valid());
        // Трек кончился - всё вернулось, дробление не копится.
        memory::psram_alloc_free(a, b1);
        memory::psram_alloc_free(a, b3);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 30);
        CHECK_EQ(static_cast<int>(memory::psram_alloc_largest_run(a)), 30);
    }

    std::printf("test_psram_alloc_config_dependent\n");
    {
        // Ради чего всё затевалось: чего не просили, то и не занято.
        // Накопителей может быть ноль, а может быть три.
        memory::PsramAlloc a;
        memory::psram_alloc_reset(a, 100);
        constexpr uint16_t kPagesPerMedium = 16; // 16 КБ при странице в 1 КБ
        for (uint16_t media = 0; media <= 3; ++media) {
            memory::psram_alloc_reset(a, 100);
            memory::PsramBlock caches[3];
            for (uint16_t i = 0; i < media; ++i)
                caches[i] = memory::psram_alloc(a, kPagesPerMedium);
            CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 100 - media * kPagesPerMedium);
            for (uint16_t i = 0; i < media; ++i)
                memory::psram_alloc_free(a, caches[i]);
            CHECK_EQ(static_cast<int>(memory::psram_alloc_free_pages(a)), 100);
        }
    }
}
