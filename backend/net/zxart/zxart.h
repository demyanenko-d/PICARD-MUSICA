// SPDX-License-Identifier: MIT
#pragma once

// Поставщик виртуального диска: архив zxart.ee каталогами.
//
//   authors/<деление>/<автор>/<раздел>/<программа>/<образы>
//   <раздел>/names/<деление>/<программа>/<образы>
//   <раздел>/authors/<деление>/<автор>/<программа>
//   <раздел>/years/<год|unknown>/names|authors/...
//   <раздел>/genres/<жанр>/names|authors|years/...
//   <раздел>/best/<программа>          100 лучших по оценке
//
// <деление>: список больше 200 - буквы, большая буква - её префиксы (пары
// и тройки одним списком); каталог префикса - один фильтр сайта. Какие
// префиксы есть, знает словарь, собранный по выгрузке сайта;
// список вне словаря - сразу записи, страницами.
//
// Работа - по запросам диска (vdisk_want): листинг или файл. Сеть - через
// транспорт: поставщик даёт путь запроса GET, ответ транспорт приносит
// вызовами zxart_http_*. Запросы идут не чаще kGapMs и не больше
// kRequestsPerMinute; ключ, на котором запрос не удался, kRetryMs не
// запрашивается; ответ 403 или 429 останавливает запросы на kBlockMs.
//
// Все вызовы - из одного потока с диском.

#include <cstddef>
#include <cstdint>

#include "platform/memory.h"

namespace net::zxart {

inline constexpr uint32_t kGapMs             = 300;
inline constexpr uint32_t kRequestsPerMinute = 120;
inline constexpr uint32_t kRetryMs           = 30000;
inline constexpr uint32_t kBlockMs           = 300000;
inline constexpr uint32_t kQuietMs           = 20000; // без байтов ответа - отказ
inline constexpr uint32_t kQueueMs           = 5000;  // ждать очереди дольше - отказ
inline constexpr uint32_t kRetries           = 2;     // повторов файла или страницы листинга после обрыва

// Начать GET пути на zxart.ee. false - транспорт занят, запрос будет
// повторён. Принятый запрос транспорт завершает ровно одним вызовом
// zxart_http_end. Каждый запрос несёт непустой User-Agent: без него сайт
// банит адрес сразу.
// stream - тело забирать не быстрее zxart_http_room (файл в окно диска).
using HttpGet = bool (*)(const char* path, bool stream);
// Бросить начатый запрос без вызова zxart_http_end.
using HttpCancel = void (*)();

struct Stats {
    uint32_t requests;
    uint32_t failed;
    uint32_t listings;
    uint32_t files;
    uint32_t json_errors;
    uint32_t format_errors; // ответ разобран, но не тот, что ожидался
    uint32_t throttled;     // отказ без запроса: пауза после отказа или предел в минуту
    uint32_t busy;          // отказ без запроса: транспорт не брал запрос дольше kQueueMs
    uint32_t timeouts;
    uint32_t ids_dropped; // id не влез в ключ
    uint32_t retries;     // файл запрошен заново после обрыва или молчания
    uint32_t prefetches;  // пачек релизов запрошено заранее
    uint32_t prefetch_failed;
    uint32_t cache_hits; // каталог программы собран из кэша, без сети
    uint32_t queued;     // программ в очереди ждут предзагрузки
};

// Кэш релизов: списки файлов программ последнего списка, взятые заранее
// пачками по kPrefetchBatch в паузах; каталог программы из кэша
// собирается сразу, без заглушки LOADING. Память - от порта (PSRAM).
inline constexpr uint32_t kCacheBytes    = 160u * 1024u;
inline constexpr uint32_t kPrefetchBatch = 10;

// Состояние работы - память от порта, kStateBytes, кратна 8. cache -
// kCacheBytes или nullptr: без кэша нет и предзагрузки. to_write - адрес
// записи в cache от порта.
inline constexpr uint32_t kStateBytes = 896;
void zxart_init(HttpGet get, HttpCancel cancel, uint8_t* state, uint8_t* cache = nullptr, platform::WriteAliasFn to_write = nullptr);

// Текущий год - последний каталог в years. Транспорт может взять его из
// заголовка Date ответа.
void zxart_set_year(uint32_t year);

// Взять новую работу у диска, послать отложенный запрос, проверить срок.
// Звать с цикла.
void zxart_poll();

// Ответ на последний GET: код, тело кусками, конец (ok - соединение
// закрылось штатно).
void zxart_http_status(int code);
void zxart_http_body(const uint8_t* p, uint32_t n);
// Сколько тела можно отдать сейчас; транспорт не отдаёт больше.
uint32_t zxart_http_room();
void zxart_http_end(bool ok);

Stats zxart_stats();

} // namespace net::zxart
