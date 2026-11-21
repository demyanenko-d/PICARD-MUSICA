// SPDX-License-Identifier: MIT
#pragma once

// Два блока флеша под настройки: чтение и запись.
//
// Блоки лежат в последних гранулах первого мегабайта - ниже банка
// инструментов и выше прошивки. Туда не пишет ни заливка прошивки, ни
// заливка банка, поэтому настройки переживают оба обновления.
//
// Запись зовётся ОДИН раз при старте, до подъёма шины и второго ядра:
// пока идёт стирание, XIP недоступен, и всё, что исполняется из флеша,
// встало бы. Из работающей машины это звать нельзя.

#include <cstdint>

namespace platform {

// Сколько блоков доступно. Ноль - платформа хранить настройки не умеет,
// остаётся то, что пришло с карты, и умолчания.
uint32_t config_flash_pages();

// Блок на чтение прямо по месту: копировать его незачем, на плате он и
// так виден как память. nullptr - номер вне диапазона.
const uint8_t* config_flash_view(uint32_t slot);

// Стереть гранулу и записать блок. false - не записалось.
bool config_flash_write(uint32_t slot, const uint8_t* page, uint32_t bytes);

// Области своих ПЗУ: 64 КБ каждая, пишутся гранулами по 4 КБ. Только при
// загрузке, до второго ядра, как и блоки настроек.
inline constexpr uint32_t kRomSlots       = 3;
inline constexpr uint32_t kRomSlotBytes   = 65536;
inline constexpr uint32_t kRomSectorBytes = 4096;
// Область на чтение по месту; nullptr - нет такой или платформа не умеет.
const uint8_t* rom_slot_view(uint32_t slot);
// Стереть и записать гранулу sector (0..15) области slot.
bool rom_slot_write(uint32_t slot, uint32_t sector, const uint8_t* data);

} // namespace platform
