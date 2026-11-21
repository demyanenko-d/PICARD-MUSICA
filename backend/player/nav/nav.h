// SPDX-License-Identifier: MIT
#pragma once

// Навигатор образов Beta Disk, часть платы: где стоит пользователь и что
// видно. Носителей три (SD, USB, WIFI), четвёртый пункт верхнего уровня -
// последние открытые образы (RECENT). В каталоге - каталоги, образы TRD,
// SCL и прочие файлы, сначала каталоги, по алфавиту без учёта регистра
// (имена в CP866). Первый пункт каталога и RECENT - ".." (kUp), выход на
// уровень выше.
//
// Файлы отмечаются в набор - диск TR-DOS только для чтения. Отметки живут
// до смены списка. Отметить можно то, что помещается на диск: файл в
// 1..255 секторов, пока набрано меньше 128 файлов и 4080 секторов.
//
// Носитель доступен - есть и монтируется; WIFI пока заготовка: виден, но не
// входится. Если доступен один пункт верхнего уровня - он и открывается.
//
// Память (последний каталог, последние образы) лежит у владельца в PSRAM и
// переживает сброс платы; проверяется подписью и суммой.

#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "devices/bdi/bdi.h" // PsramTakeFn
#include "player/nav/nav_page.h"

namespace player::nav {

inline constexpr int kVolSd     = 0;
inline constexpr int kVolUsb    = 1;
inline constexpr int kVolWifi   = 2;
inline constexpr int kVolumes   = 3;
inline constexpr int kRecent    = 10;
inline constexpr int kPathBytes = 192;
inline constexpr int kNameBytes = 96; // длинное имя носителя целиком: по обрезанному файл не открыть
inline constexpr char kUp[]     = "..";

enum class Kind : uint8_t { Volume, Recent, Dir, Image, RecentImage, File };

// Части подряд в dst ёмкостью cap с нулём; длинное обрезается.
inline void cut(char* dst, size_t cap, std::initializer_list<const char*> parts) {
    size_t n = 0;
    for (const char* s : parts) {
        while (*s != 0 && n + 1 < cap) {
            dst[n++] = *s++;
        }
    }
    dst[n] = 0;
}

// Последний элемент пути: "/A/B.TRD" -> "B.TRD".
inline const char* base_name(const char* path) {
    const char* s = std::strrchr(path, '/');
    return s != nullptr ? s + 1 : path;
}

struct Item {
    Kind kind;
    uint8_t vol;  // том: у Volume, RecentImage
    bool enabled; // Volume и Recent: можно войти
    char name[kNameBytes];
    bool marked;   // File: отмечен в набор
    uint32_t size; // File: длина, байт
};

struct Place {
    uint8_t vol;
    char path[kPathBytes]; // "/" или "/GAMES/DIZZY"; у образа - путь к файлу
};

struct Memory {
    uint32_t magic;
    Place last;
    uint8_t recent_count;
    Place recent[kRecent];
    uint32_t sum;
};

// Носитель: present - доступен; list - каталог path тома vol: каталоги и
// файлы в out (не больше cap), возврат - число или -1 при отказе.
struct Media {
    bool (*present)(int vol);
    int (*list)(int vol, const char* path, Item* out, int cap);
};

enum class Pick : uint8_t { None, Moved, Image };

class Navigator;

// Действия платы по запросам страницы обмена: вставка, извлечение, имя
// образа в дисководе (пустая строка - пусто).
struct Board {
    bool (*mount)(int drive, const Place& image, bool read_only);
    bool (*eject)(int drive); // false - дисковод ещё держит прежний образ
    const char* (*drive_name)(int drive);
    // Диск из отмеченных файлов каталога nv.where() - в дисковод. Ответ -
    // nav_page::Result; при NoFile и Fragments bad - пункт файла.
    uint8_t (*mount_set)(int drive, const Navigator& nv, int& bad);
};

// Подъём на плате: место под навигатор в PSRAM (при загрузке, после
// bdi_reserve). false - PSRAM не хватило.
bool nav_reserve(devices::bdi::PsramTakeFn take);
// Цикл Core1: запрос со страницы обмена, пока Beta Disk поднят.
void nav_poll();

// Запрос со страницы обмена, если он новый: исполнить и
// перерисовать страницу; picked - выбранный образ между Enter и Mount.
void serve(Navigator& nv, Place& picked, uint8_t* page, uint8_t port_7ffd, const Board& board);

class Navigator {
public:
    // items - место под список (PSRAM), mem - память последних (PSRAM).
    void init(Item* items, int cap, Memory* mem, const Media& media);

    // Вход по NMI: последний каталог, если он ещё открывается, иначе верхний
    // уровень (или единственный доступный пункт).
    void open();
    // Пункт i: каталог, том, RECENT - вход (Moved); образ - Image, где он - в
    // picked. None - нечего делать (WIFI, отказ носителя).
    Pick enter(int i, Place& picked);
    // Назад: из корня тома - на верхний уровень. Курсор - на пункте, из
    // которого вышли.
    void back();
    // Образ вставлен: в начало последних.
    void remember(const Place& image);
    // Отметка файла i: поставить или снять. Ответ - nav_page::Result: Ok,
    // отказ TooLong, TooMany, NoRoom; не файл - Fail.
    uint8_t mark(int i);
    void unmark();
    int marked() const { return marked_; }
    // Место отмеченных на диске, секторов.
    uint32_t marked_sectors() const { return marked_secs_; }

    int count() const { return count_; }
    // Пункт под курсор после смены списка.
    int cursor() const { return cursor_; }
    void point(int i) { cursor_ = i; }
    const Item& item(int i) const { return items_[i]; }
    nav_page::Level level() const { return level_; }
    const Place& where() const { return at_; }

private:
    void clear();
    void show_top();
    bool show(const Place& p);
    void up_item();
    void cursor_to(Kind kind, const char* name);
    void save();
    bool single_top(int& idx) const;

    Item* items_           = nullptr;
    int cap_               = 0;
    int count_             = 0;
    int cursor_            = 0;
    int marked_            = 0;
    uint32_t marked_secs_  = 0;
    nav_page::Level level_ = nav_page::Level::Top;
    Place at_{};
    Memory* mem_ = nullptr;
    Media media_{};
};

} // namespace player::nav
