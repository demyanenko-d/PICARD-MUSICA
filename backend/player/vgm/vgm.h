// SPDX-License-Identifier: MIT
#pragma once

// Плеер VGM: журнал записей в регистры чипов с паузами в отсчётах 44100 Гц -
// это же частота выхода, запись ложится ровно в свой отсчёт.
//
// Чипы - ядра платы: SN76489, AY-3-8910 / YM2149, SAA1099, YM2203, YM2612,
// YM2413, SCC (K051649, K052539), OPL (YM3526, YM3812), Y8950 и OPL3
// (YMF262); пара одинаковых чипов - два экземпляра. OPL и OPL2 играет движок
// OPL3 в режиме совместимости: частота отсчётов у них одна (вход / 72 против
// входа / 288).
//
// ЦАП канала 6 YM2612 берёт отсчёты прямой записью в регистр 0x2A или из
// блоков данных журнала: по команде на отсчёт (0x8n, 0xE0) либо потоком с
// заданной частотой (0x90-0x95). Блоки не копируются.
//
// Чип отдаёт готовое стерео: каналы складываются в левую и правую сумму
// сразу за ядром по регистрам самого чипа (разряды выхода канала OPL3,
// стерео Game Gear, две стороны SAA1099), как на выводах микросхемы.
//
// Батч - до 64 отсчётов. Чипы считаются по очереди и делят одни буферы;
// каждый проходит команды батча заново и берёт свои. Очереди записей нет:
// серия любой длины в одном отсчёте проходит целиком.

#include <cstddef>
#include <cstdint>

#include "core/chips/sn_chip.h"

namespace player::vgm {

enum class Chip : uint8_t { Sn76489, Ay8910, Ym2203, Opl2, Opl3, Saa1099, Ym2612, Ym2413, Scc, Y8950, Ym2151, Ym2610, Ym2608 };

// Кусок образа памяти чипа из блока данных журнала: адрес в образе, длина,
// смещение данных в файле.
struct RomBlock {
    uint32_t start, size, at;
    bool second;  // образ второго чипа пары
    uint8_t type; // вид блока данных: чей это образ
};

// Блоки данных журнала, читаются на месте. Банк отсчётов ЦАП YM2612 - блоки
// вида 0 подряд, в порядке встречи; блоки сверх kPcm в банк не входят.
// Образы памяти сэмплов (ADPCM чипов Y8950, YM2610 и YM2608) - блоки видов
// 0x81-0x83 и 0x88; блоки сверх kRom в образ не входят, в коллекции их до 80
// на файл.
struct Blocks {
    static constexpr uint32_t kPcm = 16;
    static constexpr uint32_t kRom = 96;
    struct Pcm {
        uint32_t at, size;
    };
    const uint8_t* data = nullptr; // файл
    uint32_t pcm_count  = 0;
    uint32_t pcm_bytes  = 0; // длина банка ЦАП
    Pcm pcm[kPcm]{};
    uint32_t rom_count = 0;
    RomBlock rom[kRom]{};
    void pcm_add(uint32_t at, uint32_t size);
    void rom_add(const RomBlock& block);
    // Отсчёт банка; за концом банка - середина шкалы.
    uint8_t pcm_byte(uint32_t pos) const;
};

inline constexpr uint32_t kMaxChips = 8;
inline constexpr uint32_t kRate     = 44100;

struct ChipSpec {
    Chip kind;
    bool second;    // второй чип пары
    uint8_t cmd;    // команда записи в первый порт чипа
    bool ym;        // AY8910 и SSG: уровни YM2149; YM2612: это YM3438; YM2413: VRC7; YM2610: YM2610B
    uint32_t clock; // вход чипа, Гц
    soundsinth::chips::SnVariant sn;
    uint8_t id = 0; // номер чипа в формате VGM
    // Громкость чипа в файле, 0x100 - единица: основная по виду чипа,
    // делённая на число одинаковых, поправка из дополнительного заголовка,
    // общая нормировка набора и усиление заголовка - как считает VGMPlay.
    // ssg_volume - часть SSG у YM2203.
    uint16_t volume     = 0x100;
    uint16_t ssg_volume = 0;
};

struct Info {
    uint32_t version       = 0; // 0x151 - версия 1.51
    uint32_t data_start    = 0;
    uint32_t loop_start    = 0; // 0 - петли нет
    uint32_t total_samples = 0;
    uint32_t loop_samples  = 0;
    // Поправки числа повторов петли: множитель в 1/16 и вычитаемая база.
    uint8_t loop_modifier = 0x10;
    int8_t loop_base      = 0;
    uint32_t count        = 0;
    ChipSpec chips[kMaxChips];
    // Первый чип заголовка, которого плеер не играет; nullptr - играет все.
    const char* unsupported = nullptr;
};

const char* chip_name(Chip kind);
// Обозначение микросхемы с разновидностью: YM3812 у OPL2, YM2149 у AY с
// уровнями YM, YM3438, VRC7, YM2610B по признаку ym.
const char* part_name(const ChipSpec& chip);

// Образ ПЗУ ритм-сэмплов YM2608, 8192 байта, живёт дольше плееров; nullptr
// (так с запуска) - образа нет, ритм YM2608 молчит.
inline constexpr uint32_t kRhythmRomBytes = 0x2000;
void set_rhythm_rom(const uint8_t* rom);

// Заголовок файла. false - не VGM или обрезан.
bool parse(const uint8_t* data, uint32_t size, Info& out);

// Строка тега GD3 в out, не длиннее cap - 1: index 0 - трек, 2 - игра, 4 -
// система, 6 - автор (нечётные - то же по-японски). Символ вне ASCII - '?'.
// false - тега или строки нет.
bool gd3_text(const uint8_t* data, uint32_t size, uint32_t index, char* out, uint32_t cap);
// Строка тега GD3 как в файле: начало (UTF-16LE) и длина в символах без
// нуля. false - тега или строки нет.
bool gd3_field(const uint8_t* data, uint32_t size, uint32_t index, const uint8_t** text, uint32_t* units);

struct Unit; // чип плеера: общая часть, за ней ядро вида

class Player {
public:
    // Память чипов и общих буферов для набора info.
    static size_t memory_need(const Info& info);
    // data - файл целиком, живёт дольше плеера. mem - память чипов, не
    // меньше memory_need(). loops - сколько раз играть петлю, 0 - без конца.
    // false - не VGM, в файле нет играемых чипов или памяти мало. Чип
    // заголовка, которому журнал ни разу не пишет, не заводится; service -
    // обслуживание шины на время прохода по журналу.
    bool open(const uint8_t* data, uint32_t size, void* mem, size_t mem_bytes, uint32_t loops, void (*service)(void*) = nullptr, void* user = nullptr);
    // Прибавить n отсчётов к сумме шины (Q24.8). false - журнал кончился:
    // дальше чипы доигрывают то, что в них записано.
    bool render_add(int32_t* mix_l, int32_t* mix_r, uint32_t n);
    // Перемотка вперёд на n отсчётов: записи журнала доходят до чипов без
    // счёта звука. Время чипов стоит - огибающие и фазы за пропущенное не
    // идут, взятые ноты начинают звучать с места остановки.
    void skip(uint32_t n);

    const Info& info() const { return info_; }
    bool tag(uint32_t index, char* out, uint32_t cap) const { return gd3_text(data_, size_, index, out, cap); }
    bool tag_field(uint32_t index, const uint8_t** text, uint32_t* units) const { return gd3_field(data_, size_, index, text, units); }
    // Длина с повторами петли в отсчётах; 0 - без конца.
    uint32_t total_frames() const;
    // Голосов слышно сейчас, сумма по чипам: канал FM с несущим громче
    // -48 дБ, канал PSG с уровнем не ноль, канал ADPCM, играющий сэмпл.
    uint32_t heard() const;
    // Журнал оборван на команде, длины которой плеер не знает.
    bool broken() const { return cursor_.broken; }
    // Кусков FM посчитано с открытия: FM считается кусками между отсчётами,
    // в которых есть записи. Один кусок на батч из 64 отсчётов - минимум.
    uint32_t fm_pieces() const { return fm_pieces_; }

private:
    // Место в журнале: смещение команды, остаток паузы, сыгранные петли,
    // место чтения в банке отсчётов ЦАП.
    struct Cursor {
        uint32_t pos   = 0;
        uint32_t wait  = 0;
        uint32_t loops = 0;
        uint32_t pcm   = 0;
        bool ended     = false;
        bool broken    = false;
    };
    void batch(int32_t* mix_l, int32_t* mix_r, uint32_t n, bool skip);
    void scan(Cursor& c, uint32_t n, Unit& u, bool skip);

    const uint8_t* data_ = nullptr;
    uint32_t size_       = 0;
    Info info_{};
    Cursor cursor_{};
    uint32_t loops_     = 0;
    uint32_t fm_pieces_ = 0;
    Blocks blocks_{};
    Unit* units_[kMaxChips]{};
};

} // namespace player::vgm
