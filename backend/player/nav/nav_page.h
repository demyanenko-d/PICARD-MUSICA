// SPDX-License-Identifier: MIT
#pragma once

// Страница обмена навигатора образов с платой: начало банка ОЗУ навигатора,
// у Z80 - 0x2000. Запрос (req, cmd, arg) пишет навигатор, ответ - плата:
// запрос новый, пока ack не равен req; ответ готов, когда плата приравняла
// ack к req. Раскладка повторена в ПЗУ навигатора.
//
// Строка списка: байт вида (nav::Kind в kRowKindMask, kRowLockedBit,
// kRowMarkedBit), затем имя в CP866 с нулём в конце, длинное обрезано.
//
// Окно 0x2000 переключает запись в kBankPort: 0 - N0 (эта страница,
// переменные и стек ПЗУ), 1 - N1 (сохранённый экран с kSaveScreen). Байт
// kBank каждого банка - его номер: плата держит его в обоих, по нему ПЗУ
// видит, что окно переключилось.

#include <cstdint>

namespace player::nav_page {

inline constexpr uint32_t kMagic        = 0; // подпись kMagicBytes, плата
inline constexpr uint8_t kMagicBytes[4] = {'N', 'A', 'V', '1'};
inline constexpr uint32_t kReq          = 4; // навигатор: счётчик запросов
inline constexpr uint32_t kCmd          = 5;
inline constexpr uint32_t kArgLo        = 6; // номер пункта, младший байт
inline constexpr uint32_t kArgHi        = 7;
inline constexpr uint32_t kArgDrive     = 8; // Mount: kArgDriveMask, kArgReadOnlyBit
inline constexpr uint32_t kAck          = 9; // плата: = req по готовности
inline constexpr uint32_t kResult       = 10;
inline constexpr uint32_t kLevel        = 11; // Level: где стоит навигатор
inline constexpr uint32_t kCount        = 12; // пунктов в списке, 2 байта
inline constexpr uint32_t kFirst        = 14; // номер пункта первой строки окна, 2 байта
inline constexpr uint32_t kPort7ffd     = 16; // плата: последняя запись в 7FFD
inline constexpr uint32_t kBank         = 17; // номер банка в окне
inline constexpr uint32_t kCursor       = 18; // плата: пункт под курсор при Moved, 2 байта
inline constexpr uint32_t kMarked       = 20; // плата: отмечено файлов, 2 байта
inline constexpr uint32_t kMarkedKb     = 22; // плата: их место на диске, КБ, 2 байта
inline constexpr uint32_t kTitle        = 32; // том и путь
inline constexpr uint32_t kDrives       = 96; // имена вставленных образов A..D
inline constexpr uint32_t kRows         = 160;

// Байт kLevel. Значения - раскладка страницы, ПЗУ навигатора читает их
// числами.
enum class Level : uint8_t { Top = 0, Dir = 1, Recent = 2 };

inline constexpr uint32_t kTitleBytes     = 64;
inline constexpr uint32_t kDriveNameBytes = 16;
static_assert(kTitle + kTitleBytes <= kDrives && kDrives + 4 * kDriveNameBytes <= kRows, "the title and four drive names fit their fields");
inline constexpr uint32_t kRowCount = 32;
inline constexpr uint32_t kRowBytes = 32;
inline constexpr uint32_t kEnd      = kRows + kRowCount * kRowBytes;
static_assert(kEnd <= 0x800, "navigator variables and stack live above the page");
inline constexpr uint32_t kKindImage  = 3;     // вид строки ниже него - вход, с него - образ или файл
inline constexpr uint32_t kKindFile   = 5;     // файл: отмечается в набор
inline constexpr uint32_t kSaveScreen = 0x100; // N1: 6912 Б экрана 5
inline constexpr uint32_t kBankPort   = 0xe3;

inline constexpr uint8_t kArgDriveMask   = 0x03; // байт kArgDrive: дисковод 0..3
inline constexpr uint8_t kArgReadOnlyBit = 0x80;
inline constexpr uint8_t kRowLockedBit   = 0x80; // байт вида строки: войти нельзя
inline constexpr uint8_t kRowMarkedBit   = 0x40; // файл отмечен
inline constexpr uint8_t kRowKindMask    = 0x3f;

enum Cmd : uint8_t {
    Open   = 1, // вход по NMI: последний каталог или верхний уровень
    Window = 2, // окно строк с пункта arg
    Enter  = 3, // пункт arg: вход или выбор образа
    Back   = 4,
    Mount  = 5, // выбранный образ - в дисковод
    Eject  = 6, // дисковод arg_drive пуст
    Leave  = 7, // выход из навигатора
    Mark   = 8, // отметка файла arg: поставить или снять
    Unmark = 9, // снять все отметки
    // Диск TR-DOS из отмеченных файлов - в дисковод; отметок нет - из файла arg.
    MountSet = 10,
};

enum Result : uint8_t {
    Ok    = 0,
    Moved = 1, // список сменился: окно с начала
    Image = 2, // выбран образ: спросить дисковод
    Fail  = 0x80,
    // Отказы отметки (Mark, MountSet).
    TooLong = 0x81, // файл пуст или длиннее 255 секторов
    TooMany = 0x82, // отмечено 128 файлов
    NoRoom  = 0x83, // на диске в 1 МБ нет места
    // Отказы сборки диска (MountSet); kCursor - файл, на котором она встала.
    NoFile    = 0x84, // файл не открылся
    Fragments = 0x85, // файл или весь набор лежит на носителе слишком многими кусками
};

} // namespace player::nav_page
