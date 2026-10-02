// SPDX-License-Identifier: MIT
#pragma once

// Буферы SRAM трека.
//
// Пул один: снизу арена метаданных, сверху вниз - временные сценарии
// (TrackScratch), у которых своей памяти нет. Жилец в каждый момент ровно
// один, заезд - scratch_take(), съезд - scratch_leave(); наложение на
// играющий ревербератор ловит проверка в отладочной сборке. Арена и
// сценарий встретились - отказ с названной причиной, а не порча.
//
// loader_scratch_buffer отдельно. Внутри load() - временные массивы разбора
// заголовков; от конца загрузки до закрытия источника - план фоновой
// догрузки (player::load::ProgressiveLoader), новый трек или модуль GS поверх живого
// плана не грузится.

#include <cassert>
#include <cstdint>

#include "core/memory/arena.h"
#include "core/memory/psram_store.h"
#include "core/memory/sample_cache_catalog.h"

namespace soundsinth::bank {
struct BankDecodeTable;
}

namespace soundsinth::memory {

// Пул один на метаданные и на временные сценарии трека: они растут
// навстречу, и нужен пик суммы, а не сумма максимумов. Метаданные под
// MIDI: медиана 4.6 КБ, 99.9% - 19.1, плотные файлы просят до 53. Сверху -
// самый дорогой сценарий: у трекеров упаковщик паттернов, у живого MIDI
// ревербератор, который стоит при полной арене.
//
// 80 КБ, а не 72: самый тяжёлый .mid библиотеки просит 71828 байт
// метаданных, и на 72 КБ проходу длительности не остаётся его 9216 -
// длительность выходит нулевая, трек не играет вовсе. Нижняя граница по
// этому файлу - 81044 байта.
inline constexpr uint32_t kResidentMetadataBytes = 81920;
// Паттерн целиком: заголовок, строки и словарь. По 121750 файлам архива
// нынешним упаковщиком худший блок 24258 байт (IT/O/orb_shy.it), словарь до
// 13746 (XM/noahsboutique_rmx.xm), строки до 19890. Тем же буфером пакуются
// паттерны .mid: по библиотеке худший - DREAMING.MID с Timbres of Heaven,
// словарь до 4956, строки до 12588.
inline constexpr uint32_t kPatternPackBufferBytes = 30u * 1024u;
static_assert(kPatternPackBufferBytes <= 65536u, "offsets inside a pattern block are 16-bit");

// Временные буферы загрузчика: состояние конвертера .mid, заголовок
// инструмента и строка паттерна IT, после загрузки на плате - план догрузки
// сэмплов. Все постоянного размера, от шапки файла не зависят (таблицы
// указателей IT и S3M читаются прямо в резидентные записи), каждое место
// проверяет static_assert. Размер задаёт план догрузки - три массива по
// 810 сэмплов, 4860 байт; IT - около 2 КБ, .mid буфера не просит вовсе
// (всё его состояние живёт в PSRAM и переживает загрузку).
inline constexpr uint32_t kLoaderScratchBytes = 4864;

// Перепаковка сэмплов: сырой кусок 8192 + кусок PCM 8192 у xm/s3m, окно
// 8192 + кусок PCM 8192 у it, 12288 у mod. Контрольные точки Dpcm8 идут
// сразу в PSRAM, длину резидентного сэмпла буфер не ограничивает.
//
// Сжатый блок IT читается скользящим окном на 8192, а не целиком (41152 байта).
inline constexpr uint32_t kSampleRepackBufferBytes = 16u * 1024u;

// Сектор карты при проверке носителя на старте.
inline constexpr uint32_t kBootSectorBytes = 512;

// Настройки при старте: блок для записи, текст файла и состояние тома
// FatFs. Постоянного места им не надо - живут они одну загрузку, а на
// стеке Core0 до планировщика всего 4 КБ.
inline constexpr uint32_t kConfigBytes = 4096 + 6144 + 1024;

// Проход секвенсора: отметки посещённых (order_pos, row) 256x256 бит и
// PlayState прохода. Размер держит static_assert у прохода.
inline constexpr uint32_t kDurationPassBytes = 8192 + 1024;

// Ревербератор движка (12612 Б при половинной частоте) и за ним его шина
// (кадр буфера на int32). Размер держит static_assert у движка.
inline constexpr uint32_t kPlayScratchBytes = 12612 + 1024;

// Кто въехал в TrackScratch. Порядок объявления - порядок по времени.
enum class Scratch : uint8_t {
    None,
    BootSector,   // сектор 0 карты, до первой загрузки
    Config,       // блок настроек и текст файла, при старте
    PatternPack,  // упаковщик паттернов, внутри load()
    XmSampleHdrs, // заголовки сэмплов XM: после паттернов, до первого PCM
    DurationPass, // длительность и упреждение: после разбора, до перепаковки
    SampleRepack, // перепаковка PCM в резидентный кодек
    Play,         // ревербератор и его шина: от сборки движка до сноса
};

// Сценарии живут на вершине резидентного пула, вниз от неё, навстречу
// арене. Своей памяти у них нет: своя стоила бы 30 КБ SRAM постоянно, а
// нужны они по очереди и ненадолго.
//
// Отсюда же и отказ: когда арена трека доросла до сценария, место кончилось
// по-настоящему, и сказать об этом надо, а не молча писать поверх.
struct TrackScratch {
    Arena* arena   = nullptr;
    Scratch tenant = Scratch::None;
};

// Занять место под сценарий. nullptr - арена доросла, места нет; вызывающий
// обязан это проверить и отказать с причиной.
//
// Сценарии загрузки идут чередой и сменяют друг друга молча: они
// последовательны по построению. Заезд под играющий ревербератор - ошибка,
// её ловит проверка.
inline uint8_t* scratch_take(TrackScratch& s, Scratch who, uint32_t bytes) {
    assert(s.tenant != Scratch::Play && "the pool is held by the reverb of the playing track");
    // Проверки до арифметики: разность беззнаковая, и при незаданном пуле
    // (capacity 0) она завернулась бы в огромное смещение.
    if (s.arena == nullptr || s.arena->base == nullptr || s.arena->capacity < bytes) return nullptr;
    Arena& a         = *s.arena;
    const size_t top = (a.capacity - bytes) & ~static_cast<size_t>(7); // сценарии выровнены на 8
    if (top < a.offset) return nullptr;
    a.floor  = top;
    s.tenant = who;
    arena_note_peak(a);
    return a.base + top;
}

// Занять всё свободное между ареной и вершиной, но не больше want; got -
// сколько выдано. Для сценариев, которым точный размер не нужен:
// упаковщик паттернов отказывает сам, если содержимое не влезло, а на
// файле с большой ареной лишние килобайты ему и не нужны. Арена при
// занятом сценарии не растёт - иначе места не осталось бы ей.
//
// nullptr - свободного нет вовсе.
inline uint8_t* scratch_take_upto(TrackScratch& s, Scratch who, uint32_t want, uint32_t& got) {
    got = 0;
    if (s.arena == nullptr || s.arena->base == nullptr) return nullptr;
    const size_t free_bytes = (s.arena->capacity - s.arena->offset) & ~static_cast<size_t>(7);
    if (s.arena->capacity <= s.arena->offset || free_bytes == 0) return nullptr;
    got = static_cast<uint32_t>(free_bytes < want ? free_bytes : want);
    return scratch_take(s, who, got);
}

// Съезд: место возвращается арене. Зовут конец загрузки и снос движка.
inline void scratch_leave(TrackScratch& s, Scratch who) {
    assert(s.tenant == who && "the wrong tenant is moving out");
    (void)who;
    s.arena->floor = s.arena->capacity;
    s.tenant       = Scratch::None;
}

// Выселить кого бы то ни было: зовёт отказ загрузки. Путей отказа у
// загрузчика десятки, и каждый обязан вернуть место арене - иначе
// граница осталась бы заниженной до смены трека.
inline void scratch_release_all(TrackScratch& s) {
    if (s.arena != nullptr) s.arena->floor = s.arena->capacity;
    s.tenant = Scratch::None;
}

struct TrackMemory {
    // Пул один на всё: снизу вверх bump метаданных (порядок, Pattern,
    // Instrument, SampleDescriptor, KeymapRange, Envelope), сверху вниз -
    // временные сценарии трека.
    Arena resident;

    // Вид на вершину того же пула; своей памяти нет. Порядок сценариев по
    // времени - в Scratch.
    TrackScratch scratch;

    // Временные массивы переменного размера на время разбора заголовков
    // (ScratchArena). В объединение не входит.
    alignas(4) uint8_t loader_scratch_buffer[kLoaderScratchBytes];

    PsramStore psram;
    SampleCacheCatalog sample_cache;

    // Таблица распаковки банка (8 КБ) - в арене resident, строит загрузчик
    // .mid; живёт до смены трека. Трекерным файлам не нужна.
    const bank::BankDecodeTable* bank_table = nullptr;
};

void track_memory_create(TrackMemory& mem);
void track_memory_destroy(TrackMemory& mem);

// Смена трека: сброс арены метаданных, каталога сэмплов и распределителя
// PSRAM; буферы загрузчика не трогаются. Остановка движка, слив
// BufferPool, загрузка и старт - снаружи, функция о движке не знает.
// Фоновая догрузка прошлого трека к вызову закрыта: иначе она пишет в сброшенное.
void track_memory_reset_for_new_track(TrackMemory& mem);

// Отладочная заливка перед сбросом: арена словами 0xDEADBEEF, PSRAM - как
// psram_poison_track.
void track_memory_poison(TrackMemory& mem);

} // namespace soundsinth::memory
