// SPDX-License-Identifier: MIT
#pragma once

// Настройки платы: откуда берутся и куда сохраняются.
//
// Порядок при старте:
//
//   1. читаются оба блока флеша, берётся годный со свежим счётчиком;
//      годных нет - остаются умолчания;
//   2. на карте ищется set_config.txt длиннее десяти байт: он читается,
//      применяется поверх и переименовывается в set_config.done;
//   3. если настройки изменились или во флеше не было годного блока,
//      новый блок пишется в соседний слот;
//   4. если на карте лежит get_config.txt, в него сохраняются текущие
//      настройки, и он переименовывается в get_config.done.
//
// Зовётся один раз, до подъёма шины и второго ядра: запись во флеш на
// время стирания отнимает XIP, а запись на карту идёт через FatFs.

#include <cstdint>

#include "core/config/config.h"
#include "core/memory/track_memory.h"

namespace devices::config {

struct BootReport {
    bool flash_valid   = false; // во флеше был годный блок
    bool file_applied  = false; // применён set_config.txt
    bool file_saved    = false; // сохранён get_config.txt
    bool flash_written = false; // блок переписан
    // Настройки пришли запросом от конфигуратора прошлой загрузки.
    bool from_configurator = false;
    // Запрос велит поднять конфигуратор.
    bool configurator = false;
    // На карте лежит приглашение open.gui.
    bool gui_invite  = false;
    uint8_t slot     = 0; // куда писали
    uint32_t applied = 0; // ключей из файла
    uint32_t unknown = 0;
    uint32_t bad     = 0;
    // Время по шагам, мс: том, чтение set_config, запись флеша, запись
    // get_config, переименование, размонтирование.
    uint32_t mount_ms       = 0;
    uint32_t set_ms         = 0;
    uint32_t flash_ms       = 0;
    uint32_t save_write_ms  = 0;
    uint32_t save_rename_ms = 0;
    uint32_t unmount_ms     = 0;
};

// Текущие настройки. До config_boot() - умолчания.
const soundsinth::config::Settings& settings();

// Поставить настройки на ходу. Зовёт конфигуратор: правка с экрана должна
// быть видна тем, кто спрашивает настройки, ещё до перезагрузки. Во флеш
// отсюда ничего не пишется - это делает следующая загрузка по запросу.
void settings_set(const soundsinth::config::Settings& s);

// Поднять настройки. Возвращает то, что случилось, - для одной строки в
// журнал; сама ничего не печатает.
BootReport config_boot(soundsinth::memory::TrackScratch& scratch);

} // namespace devices::config
