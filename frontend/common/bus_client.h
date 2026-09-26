#pragma once

/*
 * Протокол обмена с платой. Ни файлов, ни экрана, ни памяти под данные.
 *
 *   bus_ping()                  один раз при запуске
 *   bus_start(размер, порядок)  открыть сессию
 *   bus_poll()                  крутить в главном цикле, возвращает событие
 *
 * Данные файла библиотека не читает: на BUS_EV_READ приложение смотрит
 * bus_req_*, читает откуда хочет и отдаёт через bus_send().
 *
 * Порт статуса плата разбирает прерыванием, и байты сверх его темпа
 * теряет молча - всё, кроме bus_send, звать не быстрее этого. Порт данных
 * на DMA, принимает поток как угодно быстро.
 */

#include "types.h"

/*
 * Порты платы. Обязаны совпадать с firmware_config.h платы. Номер порта данных
 * продублирован в z80_fw/sd.s: сектор с карты идёт в него минуя этот код.
 */
#define BUS_PORT_CMD 0x63
#define BUS_PORT_DAT   0x67

/* -- События bus_poll() -- */

#define BUS_EV_NONE      0   /* команд нет */
#define BUS_EV_TELEMETRY 1   /* переменные ниже обновились */
#define BUS_EV_READ      2   /* плата просит bus_req_length байт */
#define BUS_EV_READY     3   /* сессия открыта */
#define BUS_EV_READ_FAST 4   /* плата просит целое окно BUS_WINDOW_BYTES */

/* -- Телеметрия -- */
/*
 * Пишет библиотека, приложение только читает; до первого кадра нули.
 * Плоскими переменными, а не структурой: на Z80 абсолютный адрес короче
 * и быстрее указателя на поле.
 */
extern char bus_board_name[17]; /* 16 символов и ноль */
extern bool_t bus_board_found;

extern u8  bus_dur_min, bus_dur_sec;
/* Длительность известна. У крупного .mid она приходит не сразу: файл
   дочитывается фоном, до этого плата шлёт признак "неизвестна", а потом
   кадр со временем. Рисовать прочерки, пока FALSE. */
extern bool_t bus_dur_valid;
extern u8  bus_pos_min, bus_pos_sec;
/* Воспроизведение остановлено; перемотка доступна. */
extern bool_t bus_paused, bus_seek_ok;


extern bool_t bus_file_info_valid;
extern u16 bus_samples, bus_patterns, bus_instruments;

extern u8  bus_voices;          /* среднее число активных голосов */
extern u8  bus_peak_voices;     /* максимум за тот же период */
extern u8  bus_cpu;             /* процент ядра под звуком */
/* Сброс голосов по перегрузке: 0 нет, 1..3 - насколько просела
   полифония. Рисуется столькими же восклицательными знаками. */
extern u8  bus_cull;
extern bool_t bus_load_valid;

extern u16 bus_psram_total, bus_psram_queued, bus_psram_loaded;
extern bool_t bus_psram_valid;

extern u8  bus_state;           /* 0 грузится, 1 играет, 2 доиграл */

/* Была ли хоть одна сессия. Отдельным признаком, потому что ноль в
   bus_state - это "грузится", и до первого трека он неотличим. */
extern bool_t bus_session_started;
#define BUS_STATE_LOADING 0
#define BUS_STATE_PLAYING 1
#define BUS_STATE_ENDED   2


/* -- Запрос данных (действителен при BUS_EV_READ*) -- */

extern u32 bus_req_offset;      /* смещение в файле */
extern u16 bus_req_length;      /* сколько байт нужно; при BUS_EV_READ_FAST
                                   всегда BUS_WINDOW_BYTES */

#define BUS_WINDOW_BYTES 4096u  /* размер окна быстрого пути */

/*
 * Окно отдано в порт данных мимо клиента - записать только длину, чтобы
 * bus_done отчитался верно. Для прямого проброса с карты: сектора идут с
 * 0x57 сразу в DAT, минуя память. Поток тот же, что у bus_send_fast.
 */
void bus_sent_external(u16 length);

/* -- Действия -- */

/** Сброс платы и чтение её имени в bus_board_name. TRUE - плата ответила. */
bool_t bus_ping(void);

/**
 * Сброс сессии, допустим в любом состоянии. Дожидается ответа платы и
 * подтверждает его: без подтверждения она ничего больше не вооружит.
 */
void bus_reset(void);

/*
 * Маршрут платы по файлу за сэмплами. Выбирает приложение: цена перемотки
 * зависит от того, как оно читает файл. Меняется порядок, не состав.
 */
#define BUS_LOAD_BY_FILE     0  /* по возрастанию смещения: прыжок назад дорог */
#define BUS_LOAD_BY_PLAYBACK 1  /* в порядке воспроизведения: перемотка даром */

/**
 * Открыть сессию под файл длиной file_length. Готовность придёт событием.
 * load_order - BUS_LOAD_* выше.
 */
void bus_start(u32 file_length, u8 load_order);

/** Один проход: опросить плату и разобрать её команду. Возвращает событие. */
u8 bus_poll(void);

/** Ответ на BUS_EV_READ: отдать len байт. */
void bus_send(const u8 *data, u16 len);

/**
 * Ответ на BUS_EV_READ_FAST: ровно BUS_WINDOW_BYTES байт одним потоком.
 * Плата принимает аппаратно, короче нельзя - буфер обязан быть такого
 * размера.
 */
void bus_send_fast(const u8 *data);

/**
 * Подтвердить отданное окно. Отдельно от bus_send, чтобы между ними
 * приложение успело сбавить темп (см. шапку файла).
 */
void bus_done(void);

/** Не смогли прочитать - пусть плата запросит то же окно заново. */
void bus_nak(void);

/* -- Трассировка -- */
/*
 * Номер точки и два числа уходят плате, она печатает их в свой UART.
 * -DBUS_TRACE=0 убирает и вызовы, и тело. Аргументы при этом не
 * вычисляются - побочных действий в них быть не должно.
 */
#define TRACE_ENTER        1
#define TRACE_PINGED       2
#define TRACE_SESSION      3
#define TRACE_LOADED       4
#define TRACE_WAIT_KEYS    5
#define TRACE_KEYS_FREE    6
#define TRACE_LOOP         7
#define TRACE_PULSE        8
#define TRACE_HALT         9
#define TRACE_LOOP_END    10
#define TRACE_EXIT        11

/* Управление воспроизведением. Обмен не меняет: плата копит запрос и
   разбирает его в своём цикле, неподтверждённая команда возвращается на
   шину. Перемотка у трекеров недоступна, у крупного .mid - пока файл не
   дочитан; смотреть bus_seek_ok. */
#define BUS_TRANS_PAUSE    1   /* пауза / продолжить */
#define BUS_TRANS_FORWARD  2
#define BUS_TRANS_BACKWARD 3
void bus_transport(u8 op);

#ifndef BUS_TRACE
#define BUS_TRACE 1
#endif

#if BUS_TRACE
void bus_trace(u8 code, u16 a, u16 b);
#else
#define bus_trace(code, a, b) ((void)0)
#endif

