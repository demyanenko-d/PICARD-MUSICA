#pragma once

// PSRAM как постраничный пул. На ПК - буфер в куче размером с чип, на
// МК - адресное пространство PSRAM; код аллокатора один, меняется только
// то, что стоит за PsramStore::base.
//
// Зоны паттернов и сэмплов адресуются по-разному. Паттернам хватает
// байтового bump-указателя: они не вытесняются по одному и сбрасываются
// целиком при смене трека. Зона сэмплов - страницы со связным списком
// через page_next: там вытесняются отдельные сэмплы произвольного размера.

#include <cstddef>
#include <cstdint>

#include "core/config.h"

namespace soundsinth::memory {

inline constexpr uint32_t kPsramPageBytes = 1024;
// Физический размер чипа. Хранилище трека - до всего чипа; хвосты под
// буфер GS и таблицы банка с карты отрезаются по требованию.
inline constexpr uint32_t kPsramChipBytes = 8u * 1024u * 1024u;

// Приёмный буфер модуля от плеера GS: модуль идёт потоком один раз, а
// разбирается с произвольным доступом. Отрезается сверху от хранилища трека,
// пока говорит плеер GS. Мегабайта хватает MOD на 4 канала и с 256 паттернами;
// у 99% файлов архива сырого PCM не больше 3.7 МБ.
inline constexpr uint32_t kGsReceiveBytes = 1024u * 1024u;

// Таблицы банка с карты: нужны с произвольным доступом, PCM читается с карты.
// В конце чипа; хранилище ужимается на них, только когда банк с карты взят.
// Буфер GS, если отрезан, лежит под ними.
inline constexpr uint32_t kBankTableBytes = SOUNDSINTH_BANK_SD_TABLE_BYTES;
inline constexpr uint32_t kBankTableOffset = kPsramChipBytes - kBankTableBytes;
inline constexpr uint32_t kPatternZoneBytes = 1536u * 1024u; // потолок зоны сжатых паттернов

// page_next - на весь чип: граница зон подвижная, без паттернов и буфера GS
// страниц сэмплов почти 8192. Выход за массив на плате молча портит память.
inline constexpr uint32_t kMaxSamplePageCount = kPsramChipBytes / kPsramPageBytes; // 8192

inline constexpr uint16_t kPageChainEnd = 0xffffu; // конец цепочки сэмпла или free-list
static_assert(kMaxSamplePageCount < kPageChainEnd, "номер страницы не совпадает с концом цепочки");
inline constexpr uint32_t kPatternAllocFailed = 0xffffffffu;

struct PsramStore {
    uint8_t* base = nullptr; // весь чип; с начала зона паттернов, за ней зона сэмплов (граница подвижная)
    // Дескрипторы страниц только зоны сэмплов: индекс 0 - первая страница
    // зоны (sample_zone_base). next - следующая страница того же сэмпла или
    // следующая свободная.
    uint16_t page_next[kMaxSamplePageCount] = {};
    uint16_t free_list_head = kPageChainEnd;
    // Длина free-list без прохода, меняется вместе со списком.
    uint16_t free_page_count = 0;
    // Освобождение подняло счётчик выше числа страниц: цепочку вернули
    // дважды, список испорчен. Снимает только пересборка списка.
    bool free_list_broken = false;
    uint32_t pattern_bump_offset = 0; // байтовое смещение в зоне паттернов, растёт монотонно между сбросами

    // Начало зоны сэмплов и число страниц. Без заморозки зоны граница на
    // kPatternZoneBytes: так пакуют сэмплы тесты.
    uint32_t sample_zone_base = 0;
    uint32_t sample_page_count = 0;

    // Сколько байт PSRAM достаётся треку: весь чип, пока не отрезан буфер
    // GS или хвост под таблицы банка с карты.
    uint32_t track_bytes = kPsramChipBytes;

    // Нижняя граница временного загрузчика (растёт вниз от track_bytes).
    // Равна track_bytes, когда временного нет.
    uint32_t temp_floor = kPsramChipBytes;
};

// Получает PsramStore::base - разово при старте, не при каждой смене
// трека.
void psram_create(PsramStore& store);
void psram_destroy(PsramStore& store);

// Смена трека: bump-указатель паттернов в 0, free-list страниц сэмплов
// пересобирается одним проходом (старое содержимое целиком
// недействительно, обходить цепочки по одной незачем).
void psram_reset_track(PsramStore& store);

// Отладочная заливка перед сбросом: зона паттернов словами 0xDEADBEEF, зона
// сэмплов байтом 0xEF - чтение освобождённого видно сразу.
void psram_poison_track(PsramStore& store);
// Словами 0xDEADBEEF, хвост короче слова не трогается.
void poison_words(uint8_t* base, size_t bytes);

// Отрезать хвост хранилища под чужие данные (буфер GS, таблицы банка с
// карты). Пересобирает список свободных страниц: всё выделенное раньше
// становится недействительным. Звать только при снятом движке, который не
// соберётся, пока идёт вызов (между track_load_begin и track_load_end).
void psram_set_track_bytes(PsramStore& store, uint32_t bytes);

// Паттерны упакованы: зона сэмплов - сразу за занятым (обычно сотни КБ из
// 1.5 МБ; для Raw16 это решает, влезет ли трек). Временное загрузчика
// освобождается. Звать один раз на трек, после последнего
// psram_pattern_alloc() и последнего чтения временного, до первого
// psram_alloc_page(). Возвращает число страниц у сэмплов.
uint32_t psram_freeze_pattern_zone(PsramStore& store);

// Свободные страницы, O(1). По нему выбирается Raw16 и порог упреждения
// догрузки; в строке отказа отличает нехватку PSRAM от другой причины.
inline uint32_t psram_free_page_count(const PsramStore& store) { return store.free_page_count; }

// Та же величина проходом по free-list, O(N), с потолком от зацикленного
// списка. Для сверки счётчика в тестах.
uint32_t psram_free_list_length(const PsramStore& store);

// --- Зона паттернов: байтовый bump ---
// Не выше границы временного: встреча двух распределителей - отказ.
uint32_t psram_pattern_alloc(PsramStore& store, uint32_t size); // kPatternAllocFailed при переполнении

// --- Временное загрузчика: сверху хранилища вниз ---
// Нужное только внутри загрузки (копия файла, события, служебные карты)
// лежит сверху хранилища, а не в зоне паттернов: заморозка зоны паттернов
// отдаёт его сэмплам целиком. До заморозки psram_alloc_page страниц выше
// границы временного не выдаёт. Смещение - от base, как у паттернов,
// выровнено на 8.
uint32_t psram_temp_alloc(PsramStore& store, uint32_t size); // kPatternAllocFailed при встрече с паттернами
// В заголовке: зовётся из тика секвенсора в SRAM, вызов во флеш отнимал бы QMI у сэмплов.
inline uint8_t* psram_pattern_ptr(PsramStore& store, uint32_t offset) { return store.base + offset; }

// count объектов T во временном загрузчика; nullptr - встреча с паттернами.
// Без инициализации: вызывающий заполняет сам (чтением, memset, циклом).
template <typename T>
T* psram_temp_new(PsramStore& store, uint32_t count) {
    static_assert(alignof(T) <= 8, "временное выровнено на 8");
    const uint32_t off = psram_temp_alloc(store, count * static_cast<uint32_t>(sizeof(T)));
    if (off == kPatternAllocFailed) return nullptr;
    return reinterpret_cast<T*>(psram_pattern_ptr(store, off));
}

// count объектов T в зоне паттернов, то есть на всё время трека: заморозка
// прибивает границу к тому, что уже выделено, и это переживает её. Для
// того, что нужно при игре, а не только при загрузке: у .mid это сам файл,
// карта темпа и состояние разбора - строки делаются из них по ходу.
// nullptr - зона переполнена. Без инициализации, как у временного.
template <typename T>
T* psram_resident_new(PsramStore& store, uint32_t count) {
    static_assert(alignof(T) <= 8, "зона паттернов выровнена на 8");
    const uint32_t off = psram_pattern_alloc(store, count * static_cast<uint32_t>(sizeof(T)));
    if (off == kPatternAllocFailed) return nullptr;
    return reinterpret_cast<T*>(psram_pattern_ptr(store, off));
}

// --- Зона сэмплов: страницы по kPsramPageBytes, связный список ---
uint16_t psram_alloc_page(PsramStore& store); // kPageChainEnd - свободных страниц нет
void psram_free_chain(PsramStore& store, uint16_t first_page);
// Страница зоны сэмплов. Аксессоры ниже - в заголовке: их зовёт цикл голоса
// из другой единицы трансляции (LTO нет), вызов не дал бы держать поля голоса
// в регистрах.
inline uint8_t* psram_page_ptr(PsramStore& store, uint16_t page_index) {
    return store.base + store.sample_zone_base + static_cast<uint32_t>(page_index) * kPsramPageBytes;
}

#if SOUNDSINTH_TEST_COUNTERS
// Обходы page_next - только сборка ПК: тест видит цену поиска позиции.
inline uint32_t g_page_next_walks = 0;
#endif
inline uint16_t psram_page_next(const PsramStore& store, uint16_t page_index) {
#if SOUNDSINTH_TEST_COUNTERS
    ++g_page_next_walks;
#endif
    return store.page_next[page_index];
}
// page_count шагов по page_next от first_page, PSRAM не читается; kPageChainEnd, если цепочка короче.
inline uint16_t psram_page_advance(const PsramStore& store, uint16_t first_page, uint32_t page_count) {
    uint16_t page = first_page;
    for (uint32_t i = 0; i < page_count && page != kPageChainEnd; ++i) {
        page = psram_page_next(store, page);
    }
    return page;
}
// Выданную страницу в цепочку связывает вызывающий: выдача ничего не связывает.
inline void psram_set_next(PsramStore& store, uint16_t page_index, uint16_t next) {
    store.page_next[page_index] = next;
}

} // namespace soundsinth::memory
