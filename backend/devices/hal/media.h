#pragma once

// Носитель под арбитром: всё платформенное, что ему нужно. Определяет порт.
//
// Единица - сектор в 512 байт. Не из ISR: обмен блокирующий, сектор на
// карте по SPI - около 200 мкс. Пересечений границы одно на сектор, поэтому
// это объявления, а не шаблон: вызов теряется на фоне самого обмена.

#include <cstdint>

namespace devices::hal {

inline constexpr uint32_t kSectorBytes = 512;

// Носителей два, и они не взаимозаменяемы: карта на своей шине есть
// всегда, флешка появляется и исчезает вместе с разъёмом.
enum class Medium : uint8_t { Card, Usb };

// Поднять карту. Повторный вызов - переинициализация. Флешку поднимать
// нечем: она появляется сама, когда её воткнут.
bool media_init();

bool media_present(Medium m);
uint32_t media_sector_count(Medium m);

bool media_read(Medium m, uint32_t lba, uint8_t* dst);
bool media_write(Medium m, uint32_t lba, const uint8_t* src);

// Строка здоровья носителя в журнал. Звать по расписанию, не из ISR.
void media_log_health();

} // namespace devices::hal
