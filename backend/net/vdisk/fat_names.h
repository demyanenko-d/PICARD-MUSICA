// SPDX-License-Identifier: MIT
#pragma once

// Имена записей FAT из UTF-8 поставщика: длинное - UTF-16, короткое - как
// у Windows с кодовой страницей OEM 866.
//
// Длинное имя: кириллица А-я и Ёё как есть, латиница с надстрочными
// знаками - основой ASCII (Wild Commander при знаке вне ASCII и
// кириллицы бросает длинное имя целиком), прочее и запрещённое в FAT -
// '_'. Proteus и Wild Commander переводят длинное имя в cp866 для показа.

#include <cstdint>

#include "net/vdisk/vdisk.h"

namespace net::vdisk {

// Длина расширения длинного имени с точкой: последняя точка среди пяти
// последних знаков ("a.b.c" - ".c"); нет её - 0. Одно правило на обрезку
// имени и на номер повтора.
uint32_t fat_ext_len(const uint16_t* name, uint32_t len);

// Длинное имя: без пробелов по краям и точек в конце; длиннее kNameMax -
// режется основа, расширение (точка и до четырёх знаков) остаётся. Пустое
// - "_". Возвращает число знаков.
uint32_t fat_long_name(const char* utf8, uint16_t* out);

// Имя само годно как 8.3: ASCII, каждая часть в одном регистре. Строчные
// части - флагами регистра (байт 12 записи), как пишет Windows.
bool fat_exact_sfn(const uint16_t* name, uint32_t len, uint8_t sfn[11], uint8_t& case_bits);

// Основа короткого имени по правилам Windows: заглавные cp866, пробелы и
// лишние точки выброшены, недопустимое - '_'; lossy - что-то выброшено,
// заменено или отрезано.
struct SfnBasis {
    uint8_t base[8];
    uint8_t ext[3];
    uint8_t base_len;
    uint8_t ext_len;
    bool lossy;
};
SfnBasis fat_sfn_basis(const uint16_t* name, uint32_t len);

// Кандидат номер n: 0 - основа как есть, 1..4 - шесть знаков и ~n, дальше
// - два знака, четыре шестнадцатеричные цифры hash и ~(n-4).
void fat_sfn_candidate(const SfnBasis& b, uint32_t n, uint16_t hash, uint8_t sfn[11]);

// Знак без регистра: повторы имён в каталоге FAT не различают регистр.
uint16_t fat_fold(uint16_t c);

} // namespace net::vdisk
