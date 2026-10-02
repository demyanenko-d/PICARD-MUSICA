// SPDX-License-Identifier: MIT
#pragma once

// PSRAM как постраничный пул. На ПК - буфер в куче размером с чип, на
// МК - адресное пространство PSRAM; код один, меняется только то, что
// стоит за PsramStore::base.
//
// Страницы раздаёт распределитель блоков. Загрузка трека берёт один
// непрерывный блок на весь свободный прогон и растит внутри него два
// указателя навстречу: данные трека снизу вверх, временное загрузчика
// сверху вниз. Заморозка усекает блок до занятого данными, и освободившийся
// хвост достаётся сэмплам обычными страницами.
//
// Данные трека адресуются сплошным смещением от начала блока, поэтому блок
// обязан быть непрерывным; сэмплы ходят по цепочке page_next и разрывов не
// боятся.

#include <cstddef>
#include <cstdint>

#include "core/config.h"
#include "core/memory/psram_alloc.h"

namespace soundsinth::memory {

// Приёмный буфер модуля от плеера GS: модуль идёт потоком один раз, а
// разбирается с произвольным доступом. Отрезается сверху от хранилища трека,
// пока говорит плеер GS. Мегабайта хватает MOD на 4 канала и с 256 паттернами;
// у 99% файлов архива сырого PCM не больше 3.7 МБ.
inline constexpr uint32_t kGsReceiveBytes = 1024u * 1024u;

// Таблицы банка с карты: нужны с произвольным доступом, PCM читается с карты.
// В конце чипа; хранилище ужимается на них, только когда банк с карты взят.
// Буфер GS, если отрезан, лежит под ними.
inline constexpr uint32_t kBankTableBytes  = SOUNDSINTH_BANK_SD_TABLE_BYTES;
inline constexpr uint32_t kBankTableOffset = kPsramChipBytes - kBankTableBytes;

inline constexpr uint16_t kPageChainEnd = 0xffffu; // конец цепочки сэмпла
static_assert(kMaxSamplePageCount < kPageChainEnd, "the page number does not match the end of the chain");
inline constexpr uint32_t kPatternAllocFailed = 0xffffffffu;

struct PsramStore {
    uint8_t* base = nullptr; // весь чип
    PsramAlloc alloc;        // занятость страниц
    PsramBlock track;        // блок под данные трека и временное загрузчика

    // Следующая страница того же сэмпла. Индекс - абсолютный номер страницы
    // чипа. Выход за массив на плате молча портит память.
    uint16_t page_next[kMaxSamplePageCount] = {};

    // Данные трека: байтовое смещение от начала блока, растёт монотонно.
    uint32_t pattern_bump_offset = 0;
    // Нижняя граница временного загрузчика, там же от начала блока. Равна
    // размеру блока, когда временного нет.
    uint32_t temp_floor = 0;

    // Сколько байт чипа достаётся треку: весь чип, пока не отрезан буфер GS
    // или хвост под таблицы банка с карты.
    uint32_t track_bytes = kPsramChipBytes;

    // Страницу вернули дважды - учёт врёт, и дальше раздавались бы чужие.
    bool free_list_broken = false;
};

// Получает PsramStore::base - разово при старте, не при каждой смене
// трека.
void psram_create(PsramStore& store);
void psram_destroy(PsramStore& store);

// Смена трека: все страницы возвращаются, блок трека берётся заново.
void psram_reset_track(PsramStore& store);

// Отладочная заливка перед сбросом: занятое данными трека словами
// 0xDEADBEEF, страницы сэмплов байтом 0xEF - чтение освобождённого видно
// сразу.
void psram_poison_track(PsramStore& store);
// Словами 0xDEADBEEF, хвост короче слова не трогается.
void poison_words(uint8_t* base, size_t bytes);

// Отрезать хвост хранилища под чужие данные (буфер GS, таблицы банка с
// карты). Всё выделенное раньше становится недействительным. Звать только
// при снятом движке, который не соберётся, пока идёт вызов.
void psram_set_track_bytes(PsramStore& store, uint32_t bytes);

// Данные трека уложены: блок усекается до занятого, хвост достаётся
// сэмплам, временное загрузчика освобождается вместе с ним. Звать один раз
// на трек, после последнего psram_pattern_alloc() и последнего чтения
// временного, до первого psram_alloc_page(). Возвращает число страниц,
// доставшихся сэмплам.
uint32_t psram_freeze_pattern_zone(PsramStore& store);

// Блок, который переживает смену трека: кэш носителей, таблицы банка с
// карты, приёмник модуля GS. Брать при подъёме - тогда он ложится с краю
// чипа и не режет середину. nullptr - места нет или список отметок полон.
uint8_t* psram_take_permanent(PsramStore& store, uint32_t bytes);

// Оставить данным трека столько байт, остальное блока отдать сэмплам сразу.
// Для тех, кто кладёт данные и сэмплы вперемешку и не может дождаться
// заморозки. Загрузчикам не нужно: у них порядок строгий.
void psram_reserve_track_bytes(PsramStore& store, uint32_t bytes);

// Свободные страницы, O(1). По нему выбирается Raw16 и порог упреждения
// догрузки; в строке отказа отличает нехватку PSRAM от другой причины.
inline uint32_t psram_free_page_count(const PsramStore& store) {
    return psram_alloc_free_pages(store.alloc);
}

// Та же величина проходом по битовой карте. Для сверки счётчика в тестах.
uint32_t psram_free_list_length(const PsramStore& store);

// Сколько страниц досталось сэмплам - всё, что не занял блок трека. После
// заморозки постоянно; по нему судят, много ли памяти осталось.
inline uint32_t psram_sample_page_count(const PsramStore& store) {
    return static_cast<uint32_t>(store.alloc.page_count) - store.track.pages;
}

// --- Данные трека: байтовый bump снизу блока вверх ---
// Не выше границы временного: встреча двух указателей - отказ.
uint32_t psram_pattern_alloc(PsramStore& store, uint32_t size); // kPatternAllocFailed при переполнении

// --- Временное загрузчика: сверху блока вниз ---
// Нужное только внутри загрузки (копия файла, события, служебные карты)
// лежит сверху блока, а не вместе с данными трека: заморозка отдаёт его
// сэмплам целиком. Смещение - от начала блока, как у данных, выровнено на 8.
uint32_t psram_temp_alloc(PsramStore& store, uint32_t size); // kPatternAllocFailed при встрече с данными
// Начало блока трека в байтах от base.
inline uint32_t psram_track_byte_base(const PsramStore& store) {
    return static_cast<uint32_t>(store.track.first) * kPsramPageBytes;
}
// В заголовке: зовётся из тика секвенсора в SRAM, вызов во флеш отнимал бы QMI у сэмплов.
inline uint8_t* psram_pattern_ptr(PsramStore& store, uint32_t offset) {
    return store.base + psram_track_byte_base(store) + offset;
}

// count объектов T во временном загрузчика; nullptr - встреча с данными.
// Без инициализации: вызывающий заполняет сам (чтением, memset, циклом).
template <typename T>
T* psram_temp_new(PsramStore& store, uint32_t count) {
    static_assert(alignof(T) <= 8, "the temporary is aligned to 8");
    const uint32_t off = psram_temp_alloc(store, count * static_cast<uint32_t>(sizeof(T)));
    if (off == kPatternAllocFailed) return nullptr;
    return reinterpret_cast<T*>(psram_pattern_ptr(store, off));
}

// count объектов T в данных трека, то есть на всё время трека: заморозка
// прибивает границу к тому, что уже выделено, и это переживает её. Для
// того, что нужно при игре, а не только при загрузке: у .mid это сам файл,
// карта темпа и состояние разбора - строки делаются из них по ходу.
// nullptr - места нет. Без инициализации, как у временного.
template <typename T>
T* psram_resident_new(PsramStore& store, uint32_t count) {
    static_assert(alignof(T) <= 8, "track data is aligned to 8");
    const uint32_t off = psram_pattern_alloc(store, count * static_cast<uint32_t>(sizeof(T)));
    if (off == kPatternAllocFailed) return nullptr;
    return reinterpret_cast<T*>(psram_pattern_ptr(store, off));
}

// --- Сэмплы: страницы по kPsramPageBytes, связный список ---
uint16_t psram_alloc_page(PsramStore& store); // kPageChainEnd - свободных страниц нет
void psram_free_chain(PsramStore& store, uint16_t first_page);
// Страница чипа по абсолютному номеру. Аксессоры ниже - в заголовке: их
// зовёт цикл голоса из другой единицы трансляции (LTO нет), вызов не дал бы
// держать поля голоса в регистрах.
inline uint8_t* psram_page_ptr(PsramStore& store, uint16_t page_index) {
    return store.base + static_cast<uint32_t>(page_index) * kPsramPageBytes;
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
