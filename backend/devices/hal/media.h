#pragma once

// Носитель под арбитром: всё платформенное, что ему нужно. Определяет порт.
//
// Единица - сектор в 512 байт. Не из ISR: обмен блокирующий, сектор на
// карте по SPI - около 200 мкс. Пересечений границы одно на сектор, поэтому
// это объявления, а не шаблон: вызов теряется на фоне самого обмена.

#include <cstdint>

namespace devices::hal {

inline constexpr uint32_t kSectorBytes = 512;

// Поднять носитель. Повторный вызов - переинициализация.
bool media_init();

bool media_present();
uint32_t media_sector_count();

bool media_read(uint32_t lba, uint8_t* dst);
bool media_write(uint32_t lba, const uint8_t* src);

// Строка здоровья носителя в журнал. Звать по расписанию, не из ISR.
void media_log_health();

} // namespace devices::hal
