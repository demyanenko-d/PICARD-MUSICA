// Фоновая догрузка ядра (progressive_loader): вытеснение отыгравших
// сэмплов и шаг загрузки с упреждением. Отказ здесь на плате - страницы,
// освобождённые под звучащим голосом, то есть мусор в звуке.

#include "testing.h"

#include <cstdio>
#include <memory>

#include "player/load/progressive_loader.h"
#include "player/load/sample_prefetch.h"
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"
#include "core/memory/track_memory.h"

namespace {

using namespace soundsinth;

// Цепочка из n страниц зоны сэмплов.
uint16_t alloc_chain(memory::PsramStore& psram, uint32_t n) {
    uint16_t first = memory::kPageChainEnd, prev = memory::kPageChainEnd;
    for (uint32_t i = 0; i < n; ++i) {
        const uint16_t p = memory::psram_alloc_page(psram);
        if (p == memory::kPageChainEnd) return first;
        if (prev == memory::kPageChainEnd) first = p; else memory::psram_set_next(psram, prev, p);
        memory::psram_set_next(psram, p, memory::kPageChainEnd);
        prev = p;
    }
    return first;
}

struct Env {
    std::unique_ptr<memory::TrackMemory> mem = std::make_unique<memory::TrackMemory>();
    std::unique_ptr<player::load::ProgressiveLoader> pl = std::make_unique<player::load::ProgressiveLoader>();
    std::unique_ptr<uint16_t[]> plan_mem = std::make_unique<uint16_t[]>(3 * player::load::kProgressiveMaxSamples);
    std::atomic<uint32_t> in_use_bits[2] = {};
    Env() {
        memory::track_memory_create(*mem);
        player::load::progressive_attach_plan(*pl, plan_mem.get());
    }
    ~Env() { memory::track_memory_destroy(*mem); }
    player::load::SamplesInUse in_use(uint16_t bit_count = 64) const { return player::load::SamplesInUse{in_use_bits, bit_count}; }
    // Сэмпл idx резидентен цепочкой из pages страниц, последняя позиция last.
    uint16_t resident(uint16_t idx, uint32_t pages, uint16_t last) {
        const uint16_t first = alloc_chain(mem->psram, pages);
        CHECK(memory::sample_cache_alloc_slot(mem->sample_cache, idx, first) != nullptr);
        pl->plan_last_use[idx] = last;
        if (pl->plan_count <= idx) pl->plan_count = static_cast<uint16_t>(idx + 1);
        return first;
    }
};

void test_evict_one_branches() {
    std::printf("test_progressive_evict_one_branches\n");
    {
        Env e;
        e.resident(0, 3, 1);
        CHECK(!player::load::progressive_evict_one(*e.pl, *e.mem, 0, e.in_use())); // позиция 0 - вытеснять нечего
        e.pl->no_eviction = true;
        CHECK(!player::load::progressive_evict_one(*e.pl, *e.mem, 5, e.in_use())); // проход назад - вытеснения нет
    }
    {
        Env e;
        e.resident(0, 3, 5);                       // ещё прозвучит
        e.resident(1, 3, player::load::kSampleNeverUsed);     // планировщик его не видел
        e.resident(2, 3, 1);                       // держит голос
        e.in_use_bits[0] = 1u << 2;
        CHECK(!player::load::progressive_evict_one(*e.pl, *e.mem, 3, e.in_use()));
        CHECK(!player::load::progressive_evict_one(*e.pl, *e.mem, 3, e.in_use(2))); // индекс за картой - занят
        e.in_use_bits[0] = 0;
        const uint32_t before = memory::psram_free_page_count(e.mem->psram);
        CHECK(player::load::progressive_evict_one(*e.pl, *e.mem, 3, e.in_use()));
        CHECK(memory::sample_cache_find(e.mem->sample_cache, 2) == nullptr);
        CHECK_EQ(memory::psram_free_page_count(e.mem->psram), before + 3u); // цепочка вернулась
        CHECK_EQ(e.pl->evicted, 1u);
        CHECK(memory::sample_cache_find(e.mem->sample_cache, 0) != nullptr);
    }
    {
        // .mid: две записи делят цепочку - слот снимается, страницы нет.
        Env e;
        const uint16_t first = e.resident(0, 4, 1);
        CHECK(memory::sample_cache_alloc_slot(e.mem->sample_cache, 1, first) != nullptr);
        e.pl->plan_last_use[1] = 9;
        e.pl->plan_count = 2;
        const uint32_t before = memory::psram_free_page_count(e.mem->psram);
        CHECK(player::load::progressive_evict_one(*e.pl, *e.mem, 3, e.in_use()));
        CHECK(memory::sample_cache_find(e.mem->sample_cache, 0) == nullptr);
        CHECK_EQ(memory::psram_free_page_count(e.mem->psram), before);
    }
}

// Загрузка: страниц на сэмпл pages; отказ, пока свободных меньше. Считает вызовы.
struct Loader {
    memory::TrackMemory* mem = nullptr;
    uint32_t pages = 1;
    uint32_t calls = 0;
    static bool load(void* user, uint16_t idx, const char** reason_out) {
        auto* l = static_cast<Loader*>(user);
        ++l->calls;
        if (memory::psram_free_page_count(l->mem->psram) < l->pages) {
            *reason_out = "нет места";
            return false;
        }
        const uint16_t first = alloc_chain(l->mem->psram, l->pages);
        return memory::sample_cache_alloc_slot(l->mem->sample_cache, idx, first) != nullptr;
    }
};

// Занять всё, кроме free страниц.
void leave_free(memory::PsramStore& psram, uint32_t free) {
    while (memory::psram_free_page_count(psram) > free) (void)memory::psram_alloc_page(psram);
}

void test_load_next_steps() {
    std::printf("test_progressive_load_next_steps\n");
    {
        // Done, уже в каталоге - без вызова загрузки.
        Env e;
        Loader l{e.mem.get(), 2, 0};
        e.resident(3, 2, 7);
        e.pl->plan_indices[0] = 3;
        e.pl->plan_count = 1;
        uint16_t got = 0;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l, &got) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(got, 3u);
        CHECK_EQ(l.calls, 0u);
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l) == player::load::ProgressiveStep::Done);
    }
    {
        // Не влезло - вытеснить отыгравшие по одному и повторить.
        Env e;
        Loader l{e.mem.get(), 5, 0};
        e.resident(0, 2, 1);
        e.resident(1, 2, 1);
        e.resident(2, 2, 1);
        leave_free(e.mem->psram, 0);
        e.pl->plan_indices[0] = 4;
        e.pl->plan_count = 5;
        e.pl->plan_last_use[4] = 9;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 3, e.in_use(), &Loader::load, &l) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(e.pl->evicted, 3u); // 0 + 2 + 2 + 2 страниц: хватило на третьем
        CHECK_EQ(l.calls, 4u);
        CHECK(memory::sample_cache_find(e.mem->sample_cache, 4) != nullptr);
    }
    {
        // Вытеснять нечего - Failed, счётчик отказов.
        Env e;
        Loader l{e.mem.get(), 5, 0};
        leave_free(e.mem->psram, 1);
        e.pl->plan_indices[0] = 0;
        e.pl->plan_count = 1;
        const char* why = nullptr;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 3, e.in_use(), &Loader::load, &l, nullptr, &why) ==
              player::load::ProgressiveStep::Failed);
        CHECK_EQ(e.pl->plan_failed, 1u);
        CHECK(why != nullptr);
    }
    {
        // Упреждение при тесной памяти: несвоевременные пропускаются,
        // своевременный встаёт в голову очереди; нет своевременных - Waiting.
        Env e;
        Loader l{e.mem.get(), 1, 0};
        leave_free(e.mem->psram, e.mem->psram.sample_page_count / player::load::kAmpleFreeDivisor);
        e.pl->lead_positions = 2;
        e.pl->plan_count = 3;
        e.pl->plan_indices[0] = 10;
        e.pl->plan_indices[1] = 11;
        e.pl->plan_indices[2] = 12;
        e.pl->plan_first_use[10] = 20;
        e.pl->plan_first_use[11] = 5;
        e.pl->plan_first_use[12] = 30;
        uint16_t got = 0;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 4, e.in_use(), &Loader::load, &l, &got) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(got, 11u); // первое появление 5 <= 4 + 2
        CHECK_EQ(e.pl->plan_indices[1], 10u); // несвоевременный уехал на его место
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 4, e.in_use(), &Loader::load, &l) ==
              player::load::ProgressiveStep::Waiting);
        CHECK_EQ(e.pl->plan_next, 1u);
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 18, e.in_use(), &Loader::load, &l, &got) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(got, 10u);
    }
    {
        // Waiting запоминает наименьшую первую позицию: пока план и голова те
        // же, а горизонт ниже неё, очередь не просматривается.
        Env e;
        Loader l{e.mem.get(), 1, 0};
        leave_free(e.mem->psram, e.mem->psram.sample_page_count / player::load::kAmpleFreeDivisor);
        e.pl->lead_positions = 2;
        e.pl->plan_count = 2;
        e.pl->plan_indices[0] = 10;
        e.pl->plan_indices[1] = 11;
        e.pl->plan_first_use[10] = 20;
        e.pl->plan_first_use[11] = 9;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 4, e.in_use(), &Loader::load, &l) ==
              player::load::ProgressiveStep::Waiting);
        CHECK_EQ(e.pl->waiting_min_first_use, 9u);
        e.pl->plan_first_use[10] = 5; // без смены плана не видно
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 2, e.in_use(), &Loader::load, &l) ==
              player::load::ProgressiveStep::Waiting);
        player::load::progressive_plan_changed(*e.pl);
        uint16_t got = 0;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 4, e.in_use(), &Loader::load, &l, &got) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(got, 10u);
        CHECK_EQ(e.pl->waiting_min_first_use, player::load::kSampleNeverUsed);
        // Горизонт дошёл до запомненного - просмотр и загрузка.
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 4, e.in_use(), &Loader::load, &l) ==
              player::load::ProgressiveStep::Waiting);
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 7, e.in_use(), &Loader::load, &l, &got) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(got, 11u);
    }
}

// Загрузка, у которой может кончиться каталог: причина - общая константа.
struct CatalogLoader {
    memory::TrackMemory* mem = nullptr;
    uint32_t calls = 0;
    static bool load(void* user, uint16_t idx, const char** reason_out) {
        auto* l = static_cast<CatalogLoader*>(user);
        ++l->calls;
        if (memory::psram_free_page_count(l->mem->psram) < 1) {
            *reason_out = "нет места";
            return false;
        }
        const uint16_t first = alloc_chain(l->mem->psram, 1);
        if (memory::sample_cache_alloc_slot(l->mem->sample_cache, idx, first) != nullptr) return true;
        memory::psram_free_chain(l->mem->psram, first);
        *reason_out = memory::kSampleCatalogFull;
        return false;
    }
};

// Повтор загрузки - только после вытеснения, которое что-то дало.
void test_retry_only_after_useful_eviction() {
    std::printf("test_progressive_retry_only_after_useful_eviction\n");
    {
        // .mid: единственный отыгравший делит цепочку с живым - вытеснение
        // страниц не вернуло, загрузка второй раз не зовётся.
        Env e;
        Loader l{e.mem.get(), 5, 0};
        const uint16_t first = e.resident(0, 4, 1);
        CHECK(memory::sample_cache_alloc_slot(e.mem->sample_cache, 1, first) != nullptr);
        e.pl->plan_last_use[1] = 9;
        leave_free(e.mem->psram, 0);
        e.pl->plan_indices[0] = 4;
        e.pl->plan_count = 5;
        e.pl->plan_last_use[4] = 9;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 3, e.in_use(), &Loader::load, &l) ==
              player::load::ProgressiveStep::Failed);
        CHECK_EQ(e.pl->evicted, 1u);
        CHECK_EQ(l.calls, 1u);   // было 2: повтор при тех же свободных страницах
    }
    {
        // Каталог полон записями без страниц: вытеснение освобождает слот без
        // страниц, отказ по каталогу - повод к повтору, сэмпл грузится.
        Env e;
        CatalogLoader l{e.mem.get(), 0};
        for (uint16_t i = 0; i < memory::kSampleCacheCatalogCapacity; ++i) {
            CHECK(memory::sample_cache_alloc_slot(e.mem->sample_cache, i, memory::kPageChainEnd) != nullptr);
            e.pl->plan_last_use[i] = i < 8 ? 1 : player::load::kSampleNeverUsed;
        }
        const uint16_t idx = memory::kSampleCacheCatalogCapacity + 10;
        e.pl->plan_indices[0] = idx;
        e.pl->plan_count = static_cast<uint16_t>(idx + 1);
        e.pl->plan_last_use[idx] = 9;
        CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 3, e.in_use(), &CatalogLoader::load, &l) ==
              player::load::ProgressiveStep::Loaded);
        CHECK_EQ(e.pl->evicted, 1u);
        CHECK_EQ(l.calls, 2u);
        CHECK(memory::sample_cache_find(e.mem->sample_cache, idx) != nullptr);
    }
}

// Живой поток: запросы по событиям, жертва - дольше всех не звучавший,
// звучащий не вытесняется.
void test_demand() {
    std::printf("test_progressive_demand\n");
    Env e;
    player::load::progressive_start_demand(*e.pl);
    Loader l{e.mem.get(), 2, 0};
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l) == player::load::ProgressiveStep::Waiting);

    // Повторный запрос не встаёт в очередь второй раз.
    CHECK(player::load::progressive_request(*e.pl, 5));
    CHECK(player::load::progressive_request(*e.pl, 5));
    CHECK(player::load::progressive_request(*e.pl, 7));
    CHECK(!player::load::progressive_request(*e.pl, player::load::kProgressiveMaxSamples));
    uint16_t got = 0;
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l, &got) ==
          player::load::ProgressiveStep::Loaded);
    CHECK_EQ(got, 5u);
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l, &got) ==
          player::load::ProgressiveStep::Loaded);
    CHECK_EQ(got, 7u);
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l) == player::load::ProgressiveStep::Waiting);
    CHECK_EQ(l.calls, 2u);
    // Уже резидентный: запрос проходит без загрузки.
    CHECK(player::load::progressive_request(*e.pl, 5));
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l) == player::load::ProgressiveStep::Loaded);
    CHECK_EQ(l.calls, 2u);

    // Места нет: 5 звучал позже 7 - вытесняется 7.
    leave_free(e.mem->psram, 0);
    e.in_use_bits[0] = 1u << 5;
    player::load::progressive_note_in_use(*e.pl, *e.mem, e.in_use(), 10);
    e.in_use_bits[0] = 0;
    CHECK(player::load::progressive_request(*e.pl, 9));
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l, &got) ==
          player::load::ProgressiveStep::Loaded);
    CHECK_EQ(got, 9u);
    CHECK(memory::sample_cache_find(e.mem->sample_cache, 7) == nullptr);
    CHECK(memory::sample_cache_find(e.mem->sample_cache, 5) != nullptr);
    CHECK_EQ(e.pl->evicted, 1u);

    // Звучащие не вытесняются: оба заняты - отказ.
    e.in_use_bits[0] = (1u << 5) | (1u << 9);
    player::load::progressive_note_in_use(*e.pl, *e.mem, e.in_use(), 20);
    CHECK(player::load::progressive_request(*e.pl, 11));
    CHECK(player::load::progressive_load_next(*e.pl, *e.mem, 0, e.in_use(), &Loader::load, &l) == player::load::ProgressiveStep::Failed);
    CHECK(memory::sample_cache_find(e.mem->sample_cache, 5) != nullptr);
    CHECK(memory::sample_cache_find(e.mem->sample_cache, 9) != nullptr);
    CHECK_EQ(e.pl->plan_failed, 1u);
}

} // namespace

void run_progressive_loader_tests() {
    test_evict_one_branches();
    test_load_next_steps();
    test_retry_only_after_useful_eviction();
    test_demand();
}
