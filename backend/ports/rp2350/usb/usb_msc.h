#pragma once

// Флешка на USB: подключение, ёмкость и чтение сектора.
//
// Стек MSC асинхронный, а носитель у платы читается вызовом, который
// возвращается с данными. Поэтому чтение здесь ждёт в цикле, крутя виток
// стека, - как эмулятор карты ждёт SPI. Кому отдавать время в это время,
// решает yield_to_bus() хоста: без неё ожидание крутит только USB, и шина
// на это время замирает.
//
// Всё зовётся с Core1: стек собран без операционной системы.

#include <cstdint>

namespace rp2350::usb {

inline constexpr uint32_t kMscSectorBytes = 512;

void msc_task();

bool msc_present();
uint32_t msc_sector_count();
uint8_t msc_drive_count();

// Один сектор. false - диска нет, обмен не начался или устройство
// ответило отказом.
bool msc_read(uint32_t lba, uint8_t* dst);
bool msc_write(uint32_t lba, const uint8_t* src);


} // namespace rp2350::usb
