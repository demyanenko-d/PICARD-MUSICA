// SPDX-License-Identifier: MIT
#include "player/vgm/vgm.h"

#include <cstring>
#include <new>

#include "ymfm_adpcm.h"
#include "ymfm_opl.h"
#include "ymfm_opm.h"
#include "ymfm_opn.h"

#include "platform/compiler.h"

#include "core/audio/saturate.h"
#include "core/chips/ay_chip.h"
#include "core/chips/saa_chip.h"
#include "core/chips/scc_chip.h"
#include "core/chips/sn_chip.h"

#include "player/fm_tables.h"

namespace player::vgm {

namespace {

using soundsinth::chips::AyChip;
using soundsinth::chips::SaaChip;
using soundsinth::chips::SccChip;
using soundsinth::chips::SnChip;
using soundsinth::chips::SnVariant;

constexpr uint32_t kBatch = 64;

// Вид чипа, по порядку Chip.
// gain - вес ядра в сумме шины Q24.8 при громкости чипа 0x100
// (ChipSpec::volume), 256 - отсчёт int16 как есть. Выверен по ядрам VGMPlay
// тоном в полный уровень на каждом чипе - с ним уровень чипа равен его
// уровню в VGMPlay. Полный уровень канала ядер с перепадами - 16384; OPL и
// OPL2 отдают сумму каналов сдвинутой на разряд; канал YM2612 и YM2413 - 9
// разрядов со знаком; канал SCC - волна на громкость, до 128 * 15. Y8950 -
// как OPL.
// volume и weight2 - основная громкость чипа и его вес в сумме громкостей
// набора, таблицы VGMPlay (GetChipVolume и сумма AbsVol). Вес - в половинах:
// у YM2413 он половина. У YM2203 в сумму идут обе части по единице.
struct KindInfo {
    const char* name;
    int32_t gain;
    uint16_t volume;
    uint8_t weight2;
};
// clang-format off
constexpr KindInfo kKinds[] = {
    {"SN76489", 128,  0x80,  2},
    {"AY8910",  128,  0x100, 4},
    {"YM2203",  256,  0x100, 2},
    {"OPL2",    256,  0x100, 4},
    {"OPL3",    128,  0x100, 4},
    {"SAA1099", 80,   0x100, 2},
    {"YM2612",  8192, 0x100, 2},
    {"YM2413",  2048, 0x200, 1},
    {"SCC",     819,  0xa0,  2},
    {"Y8950",   256,  0x100, 4},
    {"YM2151",  256,  0x100, 2},
    {"YM2610",  256,  0x100, 2},
    {"YM2608",  256,  0x100, 2},
};
// clang-format on
static_assert(sizeof(kKinds) / sizeof(kKinds[0]) == static_cast<size_t>(Chip::Ym2608) + 1u, "one KindInfo per Chip");
const KindInfo& kind_info(Chip kind) {
    return kKinds[static_cast<uint8_t>(kind)];
}
constexpr int32_t kSsgGain    = 117; // SSG YM2203
constexpr uint16_t kSsgVolume = 0x80;
// Громкость чипа не выше восьми единиц: сумма каналов, ограниченная шкалой
// ЦАП чипа (sat_s16), с усилением остаётся в int32.
constexpr uint32_t kMaxVolume = 0x800;
// 2^(i/32) в Q15 - шаг усиления заголовка.
constexpr uint16_t kPow[32] = {32768, 33486, 34219, 34968, 35734, 36516, 37316, 38133, 38968, 39821, 40693, 41584, 42495, 43425, 44376, 45348,
                               46341, 47356, 48393, 49452, 50535, 51642, 52773, 53928, 55109, 56316, 57549, 58809, 60097, 61413, 62757, 64132};

// Команды с одним и с двумя байтами данных, которые пишут в чипы.
constexpr uint8_t kCmdGgStereo  = 0x4f;
constexpr uint8_t kCmdSn        = 0x50;
constexpr uint8_t kCmdSn2       = 0x30;
constexpr uint8_t kCmdGgStereo2 = 0x3f;
constexpr uint8_t kCmdAy        = 0xa0;
constexpr uint8_t kCmdSaa       = 0xbd;
constexpr uint8_t kCmdScc       = 0xd2;

uint32_t le16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8));
}
uint32_t le32(const uint8_t* p) {
    return le16(p) | (le16(p + 2) << 16);
}

// Чипы заголовка, которые плеер играет, в порядке полей: вид, номер чипа в
// формате, смещение поля частоты, команды записи первого и второго чипа пары.
struct Slot {
    Chip kind;
    uint8_t id, at, cmd, cmd2;
};
// Номер YM3526 в формате VGM: OPL2 без форм волны, тот же вид Chip::Opl2.
constexpr uint8_t kYm3526 = 0x0a;
// clang-format off
constexpr Slot kSlots[] = {
    {Chip::Sn76489, 0x00, 0x0c, kCmdSn, kCmdSn2},
    {Chip::Ym2413,  0x01, 0x10, 0x51,   0xa1},
    {Chip::Ym2612,  0x02, 0x2c, 0x52,   0xa2},
    {Chip::Ym2151,  0x03, 0x30, 0x54,   0xa4},
    {Chip::Ym2203,  0x06, 0x44, 0x55,   0xa5},
    {Chip::Ym2608,  0x07, 0x48, 0x56,   0xa6},
    {Chip::Ym2610,  0x08, 0x4c, 0x58,   0xa8},
    {Chip::Opl2,    0x09, 0x50, 0x5a,   0xaa}, // YM3812
    {Chip::Opl2,    0x0a, 0x54, 0x5b,   0xab}, // YM3526
    {Chip::Y8950,   0x0b, 0x58, 0x5c,   0xac},
    {Chip::Opl3,    0x0c, 0x5c, 0x5e,   0xae},
    {Chip::Ay8910,  0x12, 0x74, kCmdAy, kCmdAy},
    {Chip::Scc,     0x19, 0x9c, kCmdScc, kCmdScc},
    {Chip::Saa1099, 0x23, 0xc8, kCmdSaa, kCmdSaa},
};
// clang-format on

// Чипы заголовка, которых плеер не играет: смещение поля частоты и имя.
struct Other {
    uint8_t at;
    const char* name;
};
// clang-format off
constexpr Other kOthers[] = {
    {0x38, "Sega PCM"}, {0x40, "RF5C68"},
    {0x60, "YMF278B"},  {0x64, "YMF271"},   {0x68, "YMZ280B"}, {0x6c, "RF5C164"},
    {0x70, "PWM"},      {0x80, "DMG"},     {0x84, "NES APU"},  {0x88, "MultiPCM"}, {0x8c, "uPD7759"}, {0x90, "OKIM6258"},
    {0x98, "OKIM6295"}, {0xa0, "K054539"}, {0xa4, "HuC6280"},  {0xa8, "C140"},     {0xac, "K053260"},
    {0xb0, "Pokey"},    {0xb4, "QSound"},  {0xb8, "SCSP"},     {0xc0, "WonderSwan"}, {0xc4, "VSU"},   {0xcc, "ES5503"},
    {0xd0, "ES5506"},   {0xd8, "X1-010"},  {0xdc, "C352"},     {0xe0, "GA20"},
};
// clang-format on

using Opl3 = ymfm::fm_engine_base<ymfm::opl3_registers>;
using Opna = ymfm::fm_engine_base<ymfm::opna_registers>;
using Opm  = ymfm::fm_engine_base<ymfm::opm_registers>;
struct Intf : ymfm::ymfm_interface {};
template <class Engine>
struct Fm {
    Fm() {
        engine.set_tables(fm_tables());
        engine.reset();
    }
    Intf intf;
    Engine engine{intf};
    // Снятие клавиши действует сразу, а не с такта чипа: журнал ставит
    // снятие и взятие ноты одного канала в один отсчёт, и до такта взятие
    // скрыло бы снятие - нота не перезапустилась бы.
    void write(uint32_t reg, uint8_t value) {
        engine.write(static_cast<uint16_t>(reg), value);
        engine.latch_keyoff();
    }
};

// Счёт на частоте чипа: ядро даёт отсчёты чипа, отсчёт выхода - их среднее
// за своё время (так же сводит частоту VGMPlay). Частоты выше половины
// частоты чипа заворачиваются как в микросхеме, а не вокруг 44100 Гц, и
// отсчётов чипа на отсчёт выхода может быть больше двух. N потоков разом.
template <uint32_t N>
struct Box {
    int32_t cur[N] = {};       // текущий отсчёт чипа
    uint32_t used  = 1u << 16; // сколько его ушло в выход, Q16
    // Новых отсчётов чипа на m отсчётов выхода длиной span (Q16).
    uint32_t fresh(uint32_t m, uint32_t span) const { return (used + m * span - 1u) >> 16; }
    // Сумма отсчётов чипа за отсчёт выхода с весом по времени; next(cur)
    // кладёт следующий отсчёт чипа.
    template <class Next>
    void take(uint32_t span, int64_t* acc, Next&& next) {
        for (uint32_t i = 0; i < N; i++) {
            acc[i] = 0;
        }
        for (uint32_t need = span; need != 0;) {
            if (used == 1u << 16) {
                next(cur);
                used = 0;
            }
            const uint32_t part = (1u << 16) - used < need ? (1u << 16) - used : need;
            for (uint32_t i = 0; i < N; i++) {
                acc[i] += static_cast<int64_t>(cur[i]) * static_cast<int32_t>(part);
            }
            used += part;
            need -= part;
        }
    }
};
// Отсчётов выхода, на которые хватит kBatch - 1 новых отсчётов чипа.
uint32_t box_room(uint32_t span) {
    return ((kBatch - 1u) << 16) / span;
}
// Среднее из суммы Box с тремя разрядами после запятой; recip - 2^43 / span.
// Постоянный сигнал выходит точно.
int32_t box_mean8(int64_t acc, uint64_t recip) {
    return static_cast<int32_t>((acc * static_cast<int64_t>(recip) + (int64_t{1} << 39)) >> 40);
}

// Тень регистров FM семейства OPN (YM2203, YM2612). Драйверы приставок
// переписывают тембры и частоты целиком на каждом такте, а таймер - на
// каждом отсчёте ЦАП: до 98% записей повторяют прежнее значение. Такая
// запись звука не меняет: ни дробить ею батч, ни отдавать её ядру незачем.
template <uint32_t N>
struct OpnShadow {
    uint8_t value[N];
    uint8_t known[N / 8] = {};
    uint8_t latch[2]     = {0xff, 0xff}; // старший байт частоты, ждёт записи младшего; 0xff - не писался
    bool has(uint32_t reg) const { return ((known[reg >> 3] >> (reg & 7u)) & 1u) != 0; }
    bool same(uint32_t reg, uint8_t v) const { return has(reg) && value[reg] == v; }
    void set(uint32_t reg, uint8_t v) {
        value[reg]       = v;
        known[reg >> 3] |= static_cast<uint8_t>(1u << (reg & 7u));
    }
};
// Запись в регистр: Skip - ядру не нужна; Latch - нужна, но звука не меняет;
// Sound - меняет звук с этого отсчёта.
enum class OpnWrite { Skip, Latch, Sound };
template <uint32_t N>
OpnWrite opn_write(OpnShadow<N>& s, uint32_t reg, uint8_t v) {
    const uint32_t r = reg & 0xffu;
    // Таймеры и адреса, где регистров FM нет.
    if (r < 0x22 || (r >= 0x23 && r <= 0x26) || r == 0x29 || (r >= 0x2d && r <= 0x2f)) return OpnWrite::Skip;
    if (r == 0x27) {
        // Старшие два разряда - режим канала 3, остальные - таймеры.
        const bool same = s.has(reg) && ((s.value[reg] ^ v) & 0xc0u) == 0;
        s.set(reg, v);
        return same ? OpnWrite::Skip : OpnWrite::Sound;
    }
    if ((r & 0xf0u) == 0xa0) {
        if ((r & 3u) == 3) return OpnWrite::Skip;
        // Старший байт частоты ложится в общую защёлку (своя у 0xA4-0xA6 и
        // у 0xAC-0xAE) и действует с записью младшего байта канала.
        uint8_t& latch = s.latch[(r >> 3) & 1u];
        if ((r & 4u) != 0) {
            if (latch == (v & 0x3fu)) return OpnWrite::Skip;
            latch = v & 0x3fu;
            return OpnWrite::Latch;
        }
        if (s.same(reg, v) && s.same(reg | 4u, latch)) return OpnWrite::Skip;
        s.set(reg, v);
        s.set(reg | 4u, latch);
        return OpnWrite::Sound;
    }
    if (s.same(reg, v)) return OpnWrite::Skip;
    s.set(reg, v);
    return OpnWrite::Sound;
}

// Образ ПЗУ ритм-сэмплов YM2608 (set_rhythm_rom).
const uint8_t* g_rhythm_rom = nullptr;

// Память сэмплов ADPCM чипа: куски образа из блоков данных журнала, читаются
// на месте по байту на запрос движка; поздний кусок перекрывает ранний, вне
// образа - ноль. У чтения ADPCM-A и ADPCM-B - свой вид блока (0 - образа нет)
// и свой последний найденный кусок: чтение идёт подряд.
struct Rom : ymfm::ymfm_interface {
    // second - образ второго чипа пары; pcm_a и pcm_b - виды блоков.
    Rom(const Blocks& b, bool second_, uint8_t pcm_a, uint8_t pcm_b) : blocks(b), second(second_), type{pcm_a, pcm_b} {}
    const Blocks& blocks;
    const uint8_t* rhythm = nullptr; // ADPCM-A читает ПЗУ внутри чипа, а не блоки
    const bool second;
    const uint8_t type[2];
    uint32_t last[2] = {};
    uint8_t ymfm_external_read(ymfm::access_class access, uint32_t address) override {
        if (access != ymfm::ACCESS_ADPCM_A && access != ymfm::ACCESS_ADPCM_B) return 0;
        const uint32_t k = access == ymfm::ACCESS_ADPCM_B ? 1u : 0u;
        if (k == 0 && rhythm != nullptr) return address < kRhythmRomBytes ? rhythm[address] : uint8_t{0};
        auto in = [&](const RomBlock& b) { return b.type == type[k] && b.second == second && address - b.start < b.size; };
        if (last[k] >= blocks.rom_count || !in(blocks.rom[last[k]])) {
            uint32_t i = blocks.rom_count;
            while (i != 0 && !in(blocks.rom[i - 1u])) {
                i--;
            }
            if (i == 0) return 0;
            last[k] = i - 1u;
        }
        const RomBlock& b = blocks.rom[last[k]];
        return blocks.data[b.at + (address - b.start)];
    }
};

// Запись, которая звука не меняет и ядру не нужна: повтор прежнего значения
// и таймеры (0x10-0x14, плеер их не ведёт). Регистр 0x01 (сброс LFO)
// действует каждой записью. Клавиши (0x08) помнятся по каналу; 0x19 - два
// регистра, AMD и PMD, по разряду 7.
bool opm_skip(OpnShadow<272>& s, uint8_t reg, uint8_t v) {
    if (reg >= 0x10 && reg <= 0x14) return true;
    if (reg == 0x01) return false;
    const uint32_t slot = reg == 0x08 ? 0x100u + (v & 7u) : reg == 0x19 ? 0x108u + (v >> 7) : reg;
    if (s.same(slot, v)) return true;
    s.set(slot, v);
    return false;
}

constexpr int16_t kNoDac = INT16_MIN; // в строке ЦАП батча: канал 6 играет FM
// Поток ЦАП: отсчёты банка идут в регистр 0x2A с частотой freq. Счёт - как у
// VGMPlay: место в данных к отсчёту выхода t - round(t * step * freq / 44100),
// отсчёт данных пишется в чип, когда место до него дошло.
struct DacStream {
    uint8_t id      = 0xff; // номер потока журнала; 0xff - потока нет
    bool running    = false;
    bool loop       = false;
    bool reverse    = false;
    bool sent       = false; // отсчёт текущего места уже записан в чип
    uint8_t step    = 1;     // шаг по данным
    uint8_t base    = 0;     // сдвиг начала
    uint32_t freq   = 0;     // отсчётов данных в секунду
    uint32_t start  = 0;     // начало в банке
    uint32_t count  = 0;     // отсчётов на проход
    uint32_t remain = 0;
    uint32_t pos    = 0; // пройдено по данным
    uint32_t target = 0; // место, до которого дошло время
    uint32_t acc    = 0; // дробная часть места, в 1/44100
};
constexpr uint32_t kStreamMaxHz = 1000000; // частота потока из журнала - не выше

// Отсчётов FM на отсчёт выхода, Q24; движок OPL берёт не больше двух.
uint32_t fm_ratio(uint32_t clock, uint32_t divider) {
    const uint64_t r = (static_cast<uint64_t>(clock) << 24) / (static_cast<uint64_t>(divider) * kRate);
    return r > (2u << 24) ? (2u << 24) : static_cast<uint32_t>(r);
}

size_t up8(size_t n) {
    return (n + 7u) & ~size_t{7};
}

// Чип с одним выходом на частоте чипа: make(k) кладёт k отсчётов чипа в
// строки rows, по каналу на строку, и возвращает каналы, где есть звук; их
// сумма - через усреднитель в обе стороны выхода. kClamp - сумма цифровая и
// ограничена шкалой ЦАП чипа, как в микросхеме (и в ymfm); без него -
// сложение аналоговое (YM2413).
template <bool kClamp = true, class Make>
void mono_run(Make&& make, Box<1>& box, int16_t* const* rows, uint32_t channels, uint32_t span, uint64_t recip, int32_t gain, int32_t* l, int32_t* r,
              uint32_t n) {
    for (uint32_t i = 0; i < n;) {
        const uint32_t m     = n - i < box_room(span) ? n - i : box_room(span);
        const uint32_t fresh = box.fresh(m, span);
        const uint32_t live  = fresh != 0 ? make(fresh) : 0u;
        // Сводятся только каналы со звуком.
        const int16_t* on[32];
        uint32_t count = 0;
        for (uint32_t c = 0; c < channels; c++) {
            if (((live >> c) & 1u) != 0) on[count++] = rows[c];
        }
        uint32_t k = 0;
        for (uint32_t j = 0; j < m; j++, i++) {
            int64_t acc[1];
            box.take(span, acc, [&](int32_t* cur) {
                int32_t sum = 0;
                for (uint32_t c = 0; c < count; c++) {
                    sum += on[c][k];
                }
                cur[0] = kClamp ? soundsinth::sat_s16(sum) : sum;
                k++;
            });
            const int32_t v  = box_mean8(acc[0], recip) * gain / 8;
            l[i]            += v;
            r[i]            += v;
        }
    }
}

// Чип с двумя сторонами на частоте чипа: как mono_run, а side(c) - стороны
// канала c (разряд 0 - левая, разряд 1 - правая). Сумма стороны - в шкале
// ЦАП чипа.
template <class Make, class Side>
void stereo_run(Make&& make, Side&& side, Box<2>& box, int16_t* const* rows, uint32_t channels, uint32_t span, uint64_t recip, int32_t gain, int32_t* l,
                int32_t* r, uint32_t n) {
    for (uint32_t i = 0; i < n;) {
        const uint32_t m     = n - i < box_room(span) ? n - i : box_room(span);
        const uint32_t fresh = box.fresh(m, span);
        const uint32_t live  = fresh != 0 ? make(fresh) : 0u;
        const int16_t* left[32];
        const int16_t* right[32];
        uint32_t nl = 0, nr = 0;
        for (uint32_t c = 0; c < channels; c++) {
            if (((live >> c) & 1u) == 0) continue;
            const uint32_t s = side(c);
            if ((s & 1u) != 0) left[nl++] = rows[c];
            if ((s & 2u) != 0) right[nr++] = rows[c];
        }
        uint32_t k = 0;
        for (uint32_t j = 0; j < m; j++, i++) {
            int64_t acc[2];
            box.take(span, acc, [&](int32_t* cur) {
                int32_t sl = 0, sr = 0;
                for (uint32_t q = 0; q < nl; q++) {
                    sl += left[q][k];
                }
                for (uint32_t q = 0; q < nr; q++) {
                    sr += right[q][k];
                }
                cur[0] = soundsinth::sat_s16(sl);
                cur[1] = soundsinth::sat_s16(sr);
                k++;
            });
            l[i] += box_mean8(acc[0], recip) * gain / 8;
            r[i] += box_mean8(acc[1], recip) * gain / 8;
        }
    }
}

// Длина команды журнала вместе с кодом; 0 - команда неизвестна или обрезана.
// Коды 0x00-0x2F не заняты, VGMPlay пропускает их по байту; неизвестные
// 0x60-0x9F обрывают журнал и у него.
uint32_t command_bytes(const uint8_t* p, uint32_t left, uint32_t version) {
    const uint8_t cmd = p[0];
    uint32_t len      = 0;
    if (cmd < 0x30) {
        len = 1;
    } else if (cmd <= 0x3f) {
        len = 2;
    } else if (cmd >= 0x40 && cmd <= 0x4e) {
        len = version >= 0x160 ? 3 : 2;
    } else if (cmd == 0x4f || cmd == 0x50) {
        len = 2;
    } else if (cmd >= 0x51 && cmd <= 0x5f) {
        len = 3;
    } else if (cmd == 0x61) {
        len = 3;
    } else if (cmd == 0x62 || cmd == 0x63 || (cmd >= 0x70 && cmd <= 0x8f)) {
        len = 1;
    } else if (cmd == 0x64) {
        len = 4;
    } else if (cmd == 0x67) {
        len = left >= 7 ? 7 + (le32(p + 3) & 0x7fffffffu) : 0;
    } else if (cmd == 0x68) {
        len = 12;
    } else if (cmd == 0x90 || cmd == 0x91 || cmd == 0x95) {
        len = 5;
    } else if (cmd == 0x92) {
        len = 6;
    } else if (cmd == 0x93) {
        len = 11;
    } else if (cmd == 0x94) {
        len = 2;
    } else if (cmd >= 0xa0 && cmd <= 0xbf) {
        len = 3;
    } else if (cmd >= 0xc0 && cmd <= 0xdf) {
        len = 4;
    } else if (cmd >= 0xe0) {
        len = 5;
    }
    return len <= left ? len : 0;
}

// До версии 1.10 частота в поле 0x10 - общая для YM2413, YM2612 и YM2151.
// Чип файла - тот, чья команда в журнале первая; её код и возвращается
// (0x53 - как 0x52), 0 - команд FM нет.
uint8_t old_fm(const uint8_t* data, uint32_t size, uint32_t at, uint32_t version) {
    while (at < size && data[at] != 0x66) {
        const uint8_t cmd = data[at];
        if (cmd >= 0x51 && cmd <= 0x54) return cmd == 0x53 ? uint8_t{0x52} : cmd;
        const uint32_t len = command_bytes(data + at, size - at, version);
        if (len == 0) break;
        at += len;
    }
    return 0;
}

// Команда журнала адресована этому чипу: cmd - её код, a - первый байт.
bool addressed(const ChipSpec& s, uint8_t cmd, uint8_t a) {
    switch (s.kind) {
        case Chip::Sn76489:
            return cmd == s.cmd || cmd == (s.second ? kCmdGgStereo2 : kCmdGgStereo);
        case Chip::Ay8910:
        case Chip::Saa1099:
        case Chip::Scc:
            // Разряд 7 первого байта - второй чип.
            return cmd == s.cmd && ((a & 0x80u) != 0) == s.second;
        case Chip::Ym2612:
        case Chip::Ym2610:
        case Chip::Ym2608:
        case Chip::Opl3:
            // Команда чипа - первый банк регистров, следующая - второй.
            return cmd == s.cmd || cmd == s.cmd + 1u;
        default:
            return cmd == s.cmd;
    }
}

// Чип заголовка, которому журнал ни разу не пишет, не заводится: молчащий
// чип считался бы весь трек. Громкости остальных посчитаны вместе с ним.
// Если не пишут никому, остаётся первый: журнал из одних пауз - тишина.
void drop_unused(const uint8_t* data, uint32_t size, Info& info, void (*service)(void*), void* user) {
    const uint32_t all = (1u << info.count) - 1u;
    uint32_t seen = 0, steps = 0;
    for (uint32_t at = info.data_start; seen != all && at < size && data[at] != 0x66;) {
        const uint32_t len = command_bytes(data + at, size - at, info.version);
        if (len == 0) break;
        // Отсчёт ЦАП из банка - запись в регистр 0x2A первого YM2612.
        const bool dac    = data[at] >= 0x80 && data[at] <= 0x8f;
        const uint8_t cmd = dac ? uint8_t{0x52} : data[at];
        const uint8_t a   = dac ? uint8_t{0x2a} : len > 1 ? data[at + 1] : uint8_t{0};
        for (uint32_t k = 0; k < info.count; k++) {
            if (addressed(info.chips[k], cmd, a)) seen |= 1u << k;
        }
        at += len;
        if (service != nullptr && (++steps & 0xfffu) == 0) service(user);
    }
    if (seen == 0) seen = 1;
    uint32_t n = 0;
    for (uint32_t k = 0; k < info.count; k++) {
        if (((seen >> k) & 1u) != 0) info.chips[n++] = info.chips[k];
    }
    info.count = n;
}

// Поля заголовка до его конца end; за концом - ноль.
struct Header {
    const uint8_t* data;
    uint32_t end;
    uint32_t field(uint32_t off) const { return off + 4 <= end ? le32(data + off) : 0u; }
    uint8_t byte(uint32_t off) const { return off < end ? data[off] : uint8_t{0}; }
};

// Чипы заголовка - в out.chips с основными громкостями; первый чип, которого
// плеер не играет, - в out.unsupported.
void parse_chips(const uint8_t* data, uint32_t size, const Header& h, Info& out) {
    SnVariant sn;
    const SnVariant none{};
    const uint8_t flags = out.version >= 0x151 ? data[0x2b] : 0;
    if (out.version >= 0x110 && le16(data + 0x28) != 0) sn.taps = static_cast<uint16_t>(le16(data + 0x28));
    if (out.version >= 0x110 && data[0x2a] != 0) sn.width = data[0x2a];
    sn.freq0_max = (flags & 0x01u) != 0;
    sn.negate    = (flags & 0x02u) != 0;
    sn.divide8   = (flags & 0x08u) == 0;
    // До версии 1.10 поле 0x10 - частота того чипа FM, которому пишет журнал.
    const uint8_t fm = out.version < 0x110 && h.field(0x10) != 0 ? old_fm(data, size, out.data_start, out.version) : uint8_t{0};
    for (const Slot& s : kSlots) {
        const bool psg = s.kind == Chip::Sn76489, ay = s.kind == Chip::Ay8910, opn2 = s.kind == Chip::Ym2612;
        const bool shared  = out.version < 0x110 && (opn2 || s.kind == Chip::Ym2413 || s.kind == Chip::Ym2151);
        const uint32_t raw = shared ? (fm == s.cmd ? h.field(0x10) : 0u) : h.field(s.at);
        if (psg && (raw & 0x80000000u) != 0) {
            out.unsupported = "T6W28";
            continue;
        }
        // Разряд 30 частоты - два чипа; громкость каждого - половина. У AY
        // тип 0x1x - YM2149 и родня; разряд 4 признаков - вывод 26 в нуле,
        // вход делится на два.
        const uint32_t n     = (raw & 0x40000000u) != 0 ? 2u : 1u;
        const uint32_t clock = (raw & 0x3fffffffu) >> (ay && (h.byte(0x79) & 0x10u) != 0 ? 1 : 0);
        // Разряд 31 частоты YM2612 - YM3438, YM2413 - VRC7, YM2610 - YM2610B.
        const bool flag = opn2 || s.kind == Chip::Ym2413 || s.kind == Chip::Ym2610;
        const bool ym   = s.kind == Chip::Ym2203 || (ay && (h.byte(0x78) & 0x10u) != 0) || (flag && (raw & 0x80000000u) != 0);
        for (uint32_t i = 0; clock != 0 && i < n && out.count < kMaxChips; i++) {
            ChipSpec& c    = out.chips[out.count++];
            c              = ChipSpec{s.kind, i != 0, i != 0 ? s.cmd2 : s.cmd, ym, clock, psg ? sn : none};
            c.id           = s.id;
            c.volume       = static_cast<uint16_t>(kind_info(s.kind).volume / n);
            const bool ssg = s.kind == Chip::Ym2203 || s.kind == Chip::Ym2610 || s.kind == Chip::Ym2608;
            c.ssg_volume   = ssg ? static_cast<uint16_t>(kSsgVolume / n) : uint16_t{0};
        }
    }
    for (const Other& o : kOthers) {
        if (out.unsupported == nullptr && (h.field(o.at) & 0x3fffffffu) != 0) out.unsupported = o.name;
    }
}

// Дополнительный заголовок ext: список громкостей чипов. Запись - номер чипа
// (разряд 7 - часть SSG), признак второго чипа и громкость: своя или, с
// разрядом 15, доля основной в 1/256. Действует первая запись чипа.
void parse_volumes(const uint8_t* data, uint32_t size, uint32_t ext, Info& out) {
    const uint32_t list_at = ext != 0 && le32(data + ext) >= 12 ? le32(data + ext + 8) : 0u;
    const uint32_t list    = list_at != 0 && list_at < size - ext - 8 ? ext + 8 + list_at : 0u;
    auto listed            = [&](uint16_t& to, uint32_t id, bool second) {
        for (uint32_t i = 0; list != 0 && i < data[list] && list + 5 + 4 * i <= size; i++) {
            const uint8_t* e = data + list + 1 + 4 * i;
            if (e[0] != id || ((e[1] & 1u) != 0) != second) continue;
            const uint32_t volume = le16(e + 2);
            to                    = static_cast<uint16_t>((volume & 0x8000u) != 0 ? (to * (volume & 0x7fffu) + 0x80u) >> 8 : volume);
            return;
        }
    };
    for (uint32_t k = 0; k < out.count; k++) {
        ChipSpec& c = out.chips[k];
        listed(c.volume, c.id, c.second);
        if (c.ssg_volume != 0) listed(c.ssg_volume, c.id | 0x80u, c.second);
    }
}

// Нормировка набора: все громкости удваиваются или делятся пополам, пока их
// взвешенная сумма не встанет между 0x200 и 0x300; затем усиление заголовка
// gain (байт 0x7C).
void normalize(uint8_t gain, Info& out) {
    uint32_t sum = 0;
    for (uint32_t k = 0; k < out.count; k++) {
        sum += out.chips[k].volume * kind_info(out.chips[k].kind).weight2 + out.chips[k].ssg_volume * 2u;
    }
    int32_t shift = 0;
    for (; sum != 0 && sum < 0x400u; sum *= 2u) {
        shift++;
    }
    for (; sum > 0x600u; sum /= 2u) {
        shift--;
    }
    // Усиление заголовка: 2^(n/32), n от -63 до 192; байт 0xC1 - это -64.
    const int32_t n     = gain <= 0xc0 ? gain : gain == 0xc1 ? -64 : gain - 0x100;
    const int32_t steps = n + 32 * shift + 32 * 16; // сдвиг и усиление одним показателем, от -16 октав
    for (uint32_t k = 0; k < out.count; k++) {
        ChipSpec& c = out.chips[k];
        for (uint16_t* to : {&c.volume, &c.ssg_volume}) {
            const uint64_t scaled = ((static_cast<uint64_t>(*to) * kPow[steps % 32]) << (steps / 32)) >> (15 + 16);
            *to                   = static_cast<uint16_t>(scaled > kMaxVolume ? kMaxVolume : scaled);
        }
    }
}

} // namespace

// Буферы батча, одни на все чипы: у ядер с перепадами - строки разностей и
// отсчёты выходов (SAA1099 - 12 выходов), у OPL3 - отсчёты 18 каналов. FM
// YM2203 пишет в выходы 3-5, рядом со своим SSG. У YM2612 строки 0-5 -
// каналы FM куска, строка 6 - ЦАП на весь батч.
struct Scratch {
    union {
        struct {
            SaaChip::Row rows[SaaChip::kOutputs];
            int16_t out[SaaChip::kOutputs][kBatch];
        } psg;
        int16_t fm[Opl3::CHANNELS][kBatch];
    };
};

// Чип плеера, общая часть. Ядро и код вида - в наследнике (Sn, Opn, ...):
// конструктор, begin, write, end, heard; у вида, который считается кусками, -
// run. По виду их зовёт visit().
struct Unit {
    ChipSpec spec;
    int32_t gain;             // вес чипа в сумме шины
    Scratch* sc;              // буферы батча
    uint32_t* pieces;         // счётчик кусков плеера
    uint32_t span  = 0;       // счёт на частоте чипа: отсчётов чипа на отсчёт выхода, Q16
    uint64_t recip = 0;       // 2^43 / span
    uint32_t done  = 0;       // отсчётов батча посчитано кусками и сведено
    int32_t* mix_l = nullptr; // сумма шины на время батча
    int32_t* mix_r = nullptr;
    // Начало батча на n отсчётов и команда потока ЦАП: виду без своих - пусто.
    void begin(uint32_t) {}
    void stream_cmd(uint32_t, const uint8_t*) {}
};

namespace {

// Канал FM тише этого не слышен: ослабление огибающей плюс общий уровень
// ymfm, 0x200 - 48 дБ.
constexpr uint32_t kHeard = 0x200;
uint32_t playing(const ymfm::adpcm_b_engine& pcm) {
    return (pcm.status() & ymfm::adpcm_b_channel::STATUS_PLAYING) != 0 ? 1u : 0u;
}

// Строки батча from[0..n) - в out.
void rows(int16_t** out, int16_t (*from)[kBatch], uint32_t n) {
    for (uint32_t c = 0; c < n; c++) {
        out[c] = from[c];
    }
}

// Стороны канала c по регистрам OPN и OPM: разряд 0 - левая, разряд 1 -
// правая.
template <class Regs>
uint32_t sides(const Regs& regs, uint32_t c) {
    const uint32_t co = Regs::channel_offset(c);
    return (regs.ch_output_0(co) != 0 ? 1u : 0u) | (regs.ch_output_1(co) != 0 ? 2u : 0u);
}

// Отсчётов FM на отсчёт выхода для Box: Q16 и обратная величина. Больше
// kBatch - 2 отсчётов чипа на отсчёт выхода усреднитель за раз не берёт.
// Не встраивается: зовут конструкторы семи видов.
SOUNDSINTH_NOINLINE void set_span(Unit& u, uint32_t divider) {
    const uint64_t span = (static_cast<uint64_t>(u.spec.clock) << 16) / (static_cast<uint64_t>(divider) * kRate);
    u.span              = span > ((kBatch - 2u) << 16) ? (kBatch - 2u) << 16 : (span != 0 ? static_cast<uint32_t>(span) : 1u);
    u.recip             = (uint64_t{1} << 43) / u.span;
}

// Чип, который считается кусками (FM, SCC), - до отсчёта to батча, сразу в
// сумму шины: запись, которая меняет выходы канала, действует со своего
// отсчёта.
template <class C>
void run_to(C& c, uint32_t to) {
    if (to <= c.done) return;
    ++*c.pieces;
    c.run(c.mix_l + c.done, c.mix_r + c.done, to - c.done);
    c.done = to;
}

// Запись в FM семейства OPN через тень: повтор не доходит до ядра, запись
// без звука не дробит батч.
template <class C>
void shadow_write(C& c, uint32_t at, uint32_t reg, uint8_t v) {
    const OpnWrite kind = opn_write(c.shadow, reg, v);
    if (kind == OpnWrite::Sound) run_to(c, at);
    if (kind != OpnWrite::Skip) c.fm.write(reg, v);
}

// Запись в регистр AY и SSG со своего отсчёта.
void ay_write(AyChip& chip, uint32_t at, uint8_t reg, uint8_t v) {
    chip.run_to(chip.units_at(at));
    chip.write(reg, v);
}

// Конец батча AY и SSG: три канала поровну в обе стороны.
void ay_end(AyChip& chip, Unit& u, int32_t gain, uint32_t n) {
    int16_t* out[AyChip::kChannels];
    rows(out, u.sc->psg.out, AyChip::kChannels);
    if (!chip.end(out)) return;
    for (uint32_t i = 0; i < n; i++) {
        const int32_t v  = (out[0][i] + out[1][i] + out[2][i]) * gain;
        u.mix_l[i]      += v;
        u.mix_r[i]      += v;
    }
}

struct Sn : Unit {
    SnChip chip;

    Sn(const Unit& u, const Blocks&) : Unit(u) { chip.init(spec.clock, spec.sn, kRate); }
    void begin(uint32_t n) { chip.begin(n, sc->psg.rows); }
    void write(uint32_t at, uint8_t cmd, uint8_t a, uint8_t, uint8_t) {
        chip.run_to(chip.units_at(at));
        if (cmd != spec.cmd) {
            chip.write_stereo(a);
        } else {
            chip.write(a);
        }
    }
    void end(uint32_t n) {
        int16_t* out[SnChip::kChannels];
        rows(out, sc->psg.out, SnChip::kChannels);
        if (!chip.end(out)) return;
        // Стерео Game Gear: разряды 4-7 - каналы слева, 0-3 - справа.
        const uint32_t mask = chip.stereo();
        for (uint32_t ch = 0; ch < SnChip::kChannels; ch++) {
            const int32_t gl = (mask & (0x10u << ch)) != 0 ? gain : 0, gr = (mask & (1u << ch)) != 0 ? gain : 0;
            for (uint32_t i = 0; i < n; i++) {
                mix_l[i] += out[ch][i] * gl;
                mix_r[i] += out[ch][i] * gr;
            }
        }
    }
    uint32_t heard() const { return chip.heard(); }
};

struct Ay : Unit {
    AyChip chip;

    Ay(const Unit& u, const Blocks&) : Unit(u) { chip.init(spec.clock, spec.ym, kRate); }
    void begin(uint32_t n) { chip.begin(n, sc->psg.rows); }
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t) {
        if ((a & 0x7fu) < AyChip::kRegs) ay_write(chip, at, a & 0x0fu, b);
    }
    void end(uint32_t n) { ay_end(chip, *this, gain, n); }
    uint32_t heard() const { return chip.heard(); }
};

struct Saa : Unit {
    SaaChip chip;

    Saa(const Unit& u, const Blocks&) : Unit(u) { chip.init(spec.clock, kRate); }
    void begin(uint32_t n) { chip.begin(n, sc->psg.rows); }
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t) {
        chip.run_to(chip.units_at(at));
        chip.write_address(a & 0x7fu);
        chip.write_data(b);
    }
    void end(uint32_t n) {
        int16_t* out[SaaChip::kOutputs];
        rows(out, sc->psg.out, SaaChip::kOutputs);
        if (!chip.end(out)) return;
        for (uint32_t i = 0; i < n; i++) {
            int32_t l = 0, r = 0;
            for (uint32_t ch = 0; ch < SaaChip::kChannels; ch++) {
                l += out[2u * ch][i];
                r += out[2u * ch + 1u][i];
            }
            mix_l[i] += l * gain;
            mix_r[i] += r * gain;
        }
    }
    uint32_t heard() const { return chip.heard(); }
};

struct Scc : Unit {
    SccChip chip;
    Box<1> box;

    Scc(const Unit& u, const Blocks&) : Unit(u) {
        chip.reset();
        set_span(*this, 16);
    }
    // Порт, регистр, данные.
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t d) {
        run_to(*this, at);
        chip.write(a & 0x7fu, b, d);
    }
    void run(int32_t* l, int32_t* r, uint32_t n) {
        int16_t* const out[1] = {sc->fm[0]};
        auto make             = [&](uint32_t k) {
            chip.generate(out[0], k);
            return 1u;
        };
        mono_run(make, box, out, 1, span, recip, gain, l, r, n);
    }
    void end(uint32_t n) { run_to(*this, n); }
    uint32_t heard() const { return chip.heard(); }
};

// YM2203: три канала FM на движке OPNA (каналы 0-2), SSG и выбор делителя
// входа.
struct Opn : Unit {
    static constexpr uint32_t kChannels = 3;
    Fm<Opna> fm;
    OpnShadow<256> shadow;
    Box<1> box;
    uint8_t sel = 2; // регистры 0x2D-0x2F: 2 - делитель 6, 3 - 3, 0 - 2
    AyChip ssg;
    int32_t ssg_gain;

    Opn(const Unit& u, const Blocks&) : Unit(u), ssg_gain((kSsgGain * spec.ssg_volume) >> 8) {
        // SSG считает на сетке двух входов: самый быстрый его такт.
        ssg.init(spec.clock * 2u, true, kRate);
        divider();
    }
    // Делитель входа по выбору sel: отсчёт FM - вход / 72, / 36 или / 24,
    // такт SSG - половина входа, вход или два входа.
    void divider() {
        static constexpr uint8_t kFm[4]    = {24, 24, 72, 36};
        static constexpr uint8_t kShift[4] = {0, 0, 2, 1};
        set_span(*this, kFm[sel]);
        ssg.set_period_shift(kShift[sel]);
    }
    void begin(uint32_t n) { ssg.begin(n, sc->psg.rows); }
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t) {
        if (a < AyChip::kRegs) {
            ay_write(ssg, at, a, b);
        } else if (a >= 0x2d && a <= 0x2f) {
            // Выбор делителя: 0x2D и 0x2E ставят по разряду, 0x2F
            // сбрасывает оба; байт данных не значит ничего.
            const uint8_t next = a == 0x2f ? uint8_t{0} : static_cast<uint8_t>(sel | (a == 0x2d ? 2u : 1u));
            if (next != sel) {
                run_to(*this, at);
                ssg.run_to(ssg.units_at(at));
                sel = next;
                divider();
            }
        } else if (uint8_t v = b; ymfm::ym2203_on_opna(a, v)) {
            shadow_write(*this, at, a, v);
        }
    }
    // Строки FM - рядом с выходами SSG: его строки разностей живут весь батч.
    void run(int32_t* l, int32_t* r, uint32_t n) {
        int16_t* out[Opna::CHANNELS] = {};
        rows(out, sc->psg.out + AyChip::kChannels, kChannels);
        mono_run([&](uint32_t k) { return fm.engine.generate_channels((1u << kChannels) - 1u, out, k, 1u << 24, 0, 32767); }, box, out, kChannels, span, recip,
                 gain, l, r, n);
    }
    void end(uint32_t n) {
        run_to(*this, n);
        ay_end(ssg, *this, ssg_gain, n);
    }
    uint32_t heard() const { return fm.engine.heard_channels((1u << kChannels) - 1u, kHeard) + ssg.heard(); }
};

// YM2610 (OPNB): FM на движке OPNA (каналы 2, 3, 5 и 6; у YM2610B все
// шесть), SSG, шесть каналов ADPCM-A и канал ADPCM-B из образов в блоках
// данных журнала (0x82 и 0x83). ADPCM считается на частоте отсчётов FM,
// ADPCM-A идёт каждый третий отсчёт.
// YM2608 (OPNA) - то же с другой картой регистров: шесть каналов FM (с
// разрядом 7 регистра 0x29, без него три), ADPCM-A - шесть ритм-сэмплов из
// ПЗУ чипа (том и римшот идут через раз), память ADPCM-B - блоки 0x81,
// адреса без сдвига.
struct Opnb : Unit {
    const bool opna; // YM2608
    Fm<Opna> fm;
    Box<2> box;     // FM
    Box<2> pcm_box; // ADPCM
    OpnShadow<512> shadow;
    Rom rom;
    ymfm::adpcm_a_engine pcm_a;
    ymfm::adpcm_b_engine pcm_b;
    uint8_t fm_mask;
    uint8_t third = 0;     // отсчёт FM в тройке: ADPCM-A идёт на нулевом
    bool half     = false; // YM2608: такт ADPCM-A без тома и римшота
    AyChip ssg;
    int32_t ssg_gain;

    Opnb(const Unit& u, const Blocks& blocks)
        : Unit(u)
        , opna(spec.kind == Chip::Ym2608)
        , rom(blocks, spec.second, 0x82, opna ? 0x81 : 0x83)
        , pcm_a(rom, opna ? 0u : 8u)
        , pcm_b(rom, opna ? 0u : 8u)
        , fm_mask(opna      ? 0x07
                  : spec.ym ? 0x3f
                            : 0x36)
        , ssg_gain((kSsgGain * spec.ssg_volume) >> 8) {
        pcm_a.reset();
        pcm_b.reset();
        if (opna) {
            // Шесть ритм-сэмплов в ПЗУ чипа: бочка, малый, тарелка,
            // хай-хэт, том, римшот.
            static constexpr uint16_t kDrum[6][2] = {{0x0000, 0x01bf}, {0x01c0, 0x043f}, {0x0440, 0x1b7f},
                                                     {0x1b80, 0x1cff}, {0x1d00, 0x1f7f}, {0x1f80, 0x1fff}};
            rom.rhythm                            = g_rhythm_rom;
            for (uint8_t i = 0; i < 6; i++) {
                pcm_a.set_start_end(i, kDrum[i][0], kDrum[i][1]);
            }
        }
        // Такт SSG - четверть входа.
        ssg.init(spec.clock / 4u, true, kRate);
        set_span(*this, 144);
    }
    void begin(uint32_t n) { ssg.begin(n, sc->psg.rows); }
    void write(uint32_t at, uint8_t cmd, uint8_t a, uint8_t b, uint8_t) {
        const bool hi = cmd != spec.cmd; // второй банк регистров
        if (!hi && a < 0x10) {
            // SSG; 0x0E и 0x0F - порты ввода-вывода.
            if (a < 0x0e) ay_write(ssg, at, a, b);
        } else if (opna) {
            write_opna(at, hi, a, b);
        } else if (!hi && a < 0x1c) {
            // ADPCM-B: память у чипа всегда внешняя, записи в неё нет.
            run_to(*this, at);
            pcm_b.write(a & 0x0fu, a == 0x10 ? static_cast<uint8_t>((b | 0x20u) & ~0x40u) : b);
        } else if (hi && a < 0x30) {
            run_to(*this, at);
            pcm_a.write(a & 0x3fu, b);
        } else if (hi || a > 0x1c) {
            // 0x1C первого банка - сброс признаков конца сэмпла.
            shadow_write(*this, at, (hi ? 0x100u : 0u) | a, b);
        }
    }
    void write_opna(uint32_t at, bool hi, uint8_t a, uint8_t b) {
        if (!hi && a < 0x20) {
            // Ритм; без образа ПЗУ сэмплов он молчит.
            if (rom.rhythm == nullptr) return;
            run_to(*this, at);
            pcm_a.write(a & 0x0fu, b);
        } else if (!hi && a == 0x29) {
            // Разряд 7 - режим шести каналов FM, остальное - прерывания.
            const uint8_t mask = (b & 0x80u) != 0 ? 0x3f : 0x07;
            if (mask == fm_mask) return;
            run_to(*this, at);
            fm_mask = mask;
        } else if (hi && a < 0x10) {
            run_to(*this, at);
            pcm_b.write(a, b);
        } else if (!hi || a > 0x10) {
            // 0x10 второго банка - признаки прерываний; делитель входа
            // (0x2D-0x2F) тень отбрасывает: журналы пишут только 0x2D,
            // он и так по умолчанию.
            shadow_write(*this, at, (hi ? 0x100u : 0u) | a, b);
        }
    }
    void run(int32_t* l, int32_t* r, uint32_t n) {
        // Строки - рядом с выходами SSG: его строки разностей живут весь батч.
        static_assert(AyChip::kChannels + Opna::CHANNELS + 2u <= SaaChip::kOutputs, "the YM2610 rows fit beside the SSG outputs");
        int16_t* out[Opna::CHANNELS + 2u];
        rows(out, sc->psg.out + AyChip::kChannels, Opna::CHANNELS + 2u);
        // FM - со сдвигом на разряд, как сводит чип: ADPCM идёт без сдвига.
        // У YM2608 идут все шесть каналов, и вне режима шести: каналы 4-6
        // тогда только не слышны.
        const uint32_t made = opna ? 0x3fu : fm_mask;
        stereo_run([&](uint32_t k) { return fm.engine.generate_channels(made, out, k, 1u << 24, 1, 32767); },
                   [&](uint32_t c) { return ((fm_mask >> c) & 1u) != 0 ? sides(fm.engine.regs(), c) : 0u; }, box, out, Opna::CHANNELS, span, recip, gain, l, r,
                   n);
        // ADPCM-A и ADPCM-B: сумма сторон на каждом отсчёте FM.
        int16_t* const pcm[2] = {out[Opna::CHANNELS], out[Opna::CHANNELS + 1u]};
        auto make             = [&](uint32_t k) {
            int32_t any = 0;
            for (uint32_t i = 0; i < k; i++) {
                if (third == 0) {
                    pcm_a.clock(half ? 0x0f : 0x3f);
                    half = opna && !half;
                }
                third = third == 2 ? uint8_t{0} : static_cast<uint8_t>(third + 1u);
                pcm_b.clock();
                ymfm::ymfm_output<2> o;
                pcm_a.output(o.clear(), 0x3f);
                pcm_b.output(o, 1);
                o.clamp16();
                pcm[0][i]  = static_cast<int16_t>(o.data[0]);
                pcm[1][i]  = static_cast<int16_t>(o.data[1]);
                any       |= o.data[0] | o.data[1];
            }
            return any != 0 ? 3u : 0u;
        };
        stereo_run(make, [](uint32_t c) { return c + 1u; }, pcm_box, pcm, 2, span, recip, gain, l, r, n);
    }
    void end(uint32_t n) {
        run_to(*this, n);
        ay_end(ssg, *this, ssg_gain, n);
    }
    uint32_t heard() const { return fm.engine.heard_channels(fm_mask, kHeard) + pcm_a.playing() + playing(pcm_b) + ssg.heard(); }
};

// YM2612: шесть каналов FM на движке OPNA и ЦАП канала 6. Отсчёт канала - 9
// разрядов со знаком; ЦАП подменяет отсчёт канала 6, пока включён.
struct Opn2 : Unit {
    static constexpr uint32_t kChannels = 6;
    Fm<Opna> fm;
    uint16_t dac_data = 0;     // байт регистра 0x2A без знака в разрядах 1-8, разряд 0 - из 0x2C
    bool dac_on       = false; // регистр 0x2B, разряд 7
    uint32_t dac_done = 0;     // отсчётов строки ЦАП батча заполнено
    Box<3> box;                // каналы 1-5 слева, они же справа, канал 6
    DacStream stream;
    OpnShadow<512> shadow;
    const Blocks& blocks; // банк отсчётов ЦАП

    Opn2(const Unit& u, const Blocks& b) : Unit(u), blocks(b) { set_span(*this, 144); }
    void begin(uint32_t) { dac_done = 0; }
    void write(uint32_t at, uint8_t cmd, uint8_t a, uint8_t b, uint8_t) {
        if (cmd != spec.cmd || a < 0x2a || a > 0x2c) {
            shadow_write(*this, at, (cmd != spec.cmd ? 0x100u : 0u) | a, b);
            return;
        }
        // ЦАП: ядра FM запись не касается, батч FM не дробится.
        dac_run(at);
        if (a == 0x2a) {
            dac_write(b);
        } else if (a == 0x2b) {
            dac_on = (b & 0x80u) != 0;
        } else {
            dac_data = static_cast<uint16_t>((dac_data & ~1u) | ((b >> 3) & 1u));
        }
    }
    void dac_write(uint8_t b) { dac_data = static_cast<uint16_t>((dac_data & 1u) | (static_cast<uint32_t>(b ^ 0x80u) << 1)); }
    // Строка ЦАП батча - до отсчёта to: отсчёт ЦАП или отметка "играет FM".
    // Поток пишет в регистр ЦАП перед каждым отсчётом выхода.
    void dac_run(uint32_t to) {
        for (int16_t* row = sc->fm[kChannels]; dac_done < to; dac_done++) {
            if (stream.running) stream_tick();
            row[dac_done] = dac_on ? static_cast<int16_t>(static_cast<int16_t>(dac_data << 7) >> 7) : kNoDac;
        }
    }
    // Один отсчёт выхода потока ЦАП: записать отсчёт данных текущего места,
    // пройти места, до которых дошло время. Проход кончился - заново или стоп.
    // Не встраивается: dac_run зовут из четырёх мест.
    SOUNDSINTH_NOINLINE void stream_tick() {
        DacStream& s = stream;
        auto send    = [&] {
            if (s.sent) return;
            s.sent                = true;
            const uint32_t passed = s.count - s.remain;
            const uint32_t at     = s.start + (s.reverse ? s.count - 1u - passed : passed) * s.step;
            if (s.remain != 0 && at < blocks.pcm_bytes) dac_write(blocks.pcm_byte(at));
        };
        s.acc    += static_cast<uint32_t>(s.step) * s.freq;
        s.target += s.acc / kRate;
        s.acc    %= kRate;
        send();
        while (s.remain != 0 && s.pos < s.target) {
            send();
            s.pos  += s.step;
            s.sent  = false;
            s.remain--;
        }
        if (s.remain == 0 && s.loop && s.count != 0) {
            s.remain = s.count;
            s.pos    = 0;
            s.target = 0;
            s.acc    = kRate / 2u;
        }
        if (s.remain == 0) s.running = false;
    }
    // Команды потока ЦАП. 0x90 - поток берёт чип: вид 2 - YM2612, разряд 7 -
    // второй чип, цель - регистр 0x2A первого банка. Остальные - по номеру
    // потока: данные (банк 0), частота, запуск с места, стоп, запуск блока.
    void stream_cmd(uint32_t at, const uint8_t* p) {
        DacStream& s = stream;
        if (p[0] == 0x90) {
            if (p[1] != 0xff && (p[2] & 0x7fu) == 0x02 && ((p[2] & 0x80u) != 0) == spec.second && p[3] == 0x00 && p[4] == 0x2a) {
                dac_run(at);
                s    = DacStream{};
                s.id = p[1];
            }
            return;
        }
        // Стоп с номером 0xff - всем потокам.
        if (s.id == 0xff || (p[1] != s.id && !(p[0] == 0x94 && p[1] == 0xff))) return;
        dac_run(at);
        // Запуск: место начала (0xffffffff - прежнее), вид длины в младших
        // разрядах mode, разряд 4 - задом наперёд, разряд 7 - по кругу.
        auto start = [&](uint32_t from, uint8_t mode, uint32_t length) {
            if (from != 0xffffffffu) s.start = from + s.base < blocks.pcm_bytes ? from + s.base : blocks.pcm_bytes;
            switch (mode & 0x0fu) {
                case 0x00: // длина прежняя
                    break;
                case 0x01: // число отсчётов данных
                    s.count = length;
                    break;
                case 0x02: // миллисекунды - формула VGMPlay
                    s.count = s.freq != 0 ? 1000u * length / s.freq : 0;
                    break;
                case 0x03: // до конца банка
                    s.count = (blocks.pcm_bytes - (s.start - s.base)) / s.step;
                    break;
                case 0x0f: // байты
                    s.count = length / s.step;
                    break;
                default:
                    s.count = 0;
                    break;
            }
            s.reverse = (mode & 0x10u) != 0;
            s.loop    = (mode & 0x80u) != 0;
            s.remain  = s.count;
            s.pos     = 0;
            s.target  = 0;
            s.acc     = kRate / 2u;
            s.sent    = false;
            s.running = true;
        };
        switch (p[0]) {
            case 0x91:
                // Данные не из банка YM2612 - потоку играть нечего.
                if (p[2] != 0x00) {
                    s.id      = 0xff;
                    s.running = false;
                }
                s.step = p[3] != 0 ? p[3] : uint8_t{1};
                s.base = p[4];
                break;
            case 0x92:
                s.freq = le32(p + 2) < kStreamMaxHz ? le32(p + 2) : kStreamMaxHz;
                break;
            case 0x93:
                if (blocks.pcm_count != 0) start(le32(p + 2), p[6], le32(p + 7));
                break;
            case 0x94:
                s.running = false;
                break;
            case 0x95: {
                if (blocks.pcm_count == 0) break;
                // Блок банка по номеру; номер за концом - блок 0.
                const uint32_t id = le16(p + 2) < blocks.pcm_count ? le16(p + 2) : 0;
                uint32_t from     = 0;
                for (uint32_t i = 0; i < id; i++) {
                    from += blocks.pcm[i].size;
                }
                start(from, static_cast<uint8_t>(0x0fu | (p[4] & 0x10u) | ((p[4] & 0x01u) << 7)), blocks.pcm[id].size);
                break;
            }
        }
    }
    // Счёт - на частоте чипа, отсчёт выхода - среднее отсчётов чипа за своё
    // время: частоты выше половины частоты чипа заворачиваются как в
    // микросхеме, а не вокруг 44100 Гц. Каналы 1-5 сводятся в стороны до
    // усреднения, канал 6 идёт отдельно - его отсчёт подменяет ЦАП (строка
    // kChannels батча, с отсчёта done).
    void run(int32_t* l, int32_t* r, uint32_t n) {
        dac_run(done + n);
        int16_t* out[Opna::CHANNELS] = {};
        rows(out, sc->fm, kChannels);
        // Ступенька ЦАП YM2612: отрицательный отсчёт канала сдвинут на -7
        // (в микросхеме +4 и -3: постоянная +4 на канал - щелчок на старте
        // каждого трека, разрыв у нуля тот же). У YM3438 её нет.
        const auto& regs   = fm.engine.regs();
        const int16_t* dac = sc->fm[kChannels] + done;
        const bool ladder  = !spec.ym;
        auto step_of       = [ladder](int32_t v) { return ladder && v < 0 ? v - 7 : v; };
        bool to_l[kChannels], to_r[kChannels];
        for (uint32_t c = 0; c < kChannels; c++) {
            const uint32_t co = ymfm::opna_registers::channel_offset(c);
            to_l[c]           = regs.ch_output_0(co) != 0;
            to_r[c]           = regs.ch_output_1(co) != 0;
        }
        const int32_t width = static_cast<int32_t>(span);
        for (uint32_t i = 0; i < n;) {
            const uint32_t m     = n - i < box_room(span) ? n - i : box_room(span);
            const uint32_t fresh = box.fresh(m, span);
            const uint32_t live  = fresh != 0 ? fm.engine.generate_channels((1u << kChannels) - 1u, out, fresh, 1u << 24, 5, 256) : 0u;
            // Каналы 1-5 со звуком.
            uint32_t on[kChannels], count = 0;
            for (uint32_t c = 0; c + 1u < kChannels; c++) {
                if (((live >> c) & 1u) != 0) on[count++] = c;
            }
            const bool six = ((live >> (kChannels - 1u)) & 1u) != 0;
            uint32_t k     = 0;
            auto next      = [&](int32_t* cur) {
                int32_t sl = 0, sr = 0;
                for (uint32_t q = 0; q < count; q++) {
                    const uint32_t c  = on[q];
                    const int32_t v   = step_of(out[c][k]);
                    sl               += to_l[c] ? v : 0;
                    sr               += to_r[c] ? v : 0;
                }
                cur[0] = sl;
                cur[1] = sr;
                cur[2] = six ? step_of(out[kChannels - 1u][k]) : 0;
                k++;
            };
            for (uint32_t j = 0; j < m; j++, i++) {
                int64_t acc[3];
                box.take(span, acc, next);
                if (dac[i] != kNoDac) acc[2] = static_cast<int64_t>(step_of(dac[i])) * width;
                l[i] += box_mean8(acc[0] + (to_l[kChannels - 1u] ? acc[2] : 0), recip) * gain / 8;
                r[i] += box_mean8(acc[1] + (to_r[kChannels - 1u] ? acc[2] : 0), recip) * gain / 8;
            }
        }
    }
    void end(uint32_t n) { run_to(*this, n); }
    // ЦАП подменяет канал 6.
    uint32_t heard() const { return dac_on ? fm.engine.heard_channels(0x1fu, kHeard) + 1u : fm.engine.heard_channels(0x3fu, kHeard); }
};

// YM2413: движок OPLL с ПЗУ тембров микросхемы; класс ymfm открывает движок
// наследнику. Девять каналов по 9 разрядов, ритм занимает каналы 7-9.
// VRC7 - тот же движок с другими тембрами, без ритма и каналов 7-9.
struct Opll : Unit {
    struct Core : ymfm::ym2413 {
        Core(ymfm::ymfm_interface& intf, bool vrc7) : ym2413(intf, vrc7 ? ymfm::ds1001::s_default_instruments : nullptr) {}
        fm_engine& engine() { return m_fm; }
        const fm_engine& engine() const { return m_fm; }
    };
    static constexpr uint32_t kChannels     = Core::fm_engine::CHANNELS;
    static constexpr uint32_t kVrc7Channels = 6;
    Intf intf;
    Core core;
    uint32_t channels;
    Box<1> box;

    Opll(const Unit& u, const Blocks&) : Unit(u), core(intf, spec.ym), channels(spec.ym ? kVrc7Channels : kChannels) {
        core.engine().set_tables(fm_tables());
        core.reset();
        set_span(*this, 72);
    }
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t) {
        run_to(*this, at);
        // Регистр ритма и регистры каналов 7-9: у VRC7 их нет.
        if (channels != kChannels && (a == 0x0e || (a >= 0x10 && (a & 0x0fu) >= kVrc7Channels))) return;
        core.engine().write(a, b);
        core.engine().latch_keyoff();
    }
    void run(int32_t* l, int32_t* r, uint32_t n) {
        int16_t* out[kChannels];
        rows(out, sc->fm, kChannels);
        mono_run<false>([&](uint32_t k) { return core.engine().generate_channels((1u << channels) - 1u, out, k, 1u << 24, 5, 256); }, box, out, channels, span,
                        recip, gain, l, r, n);
    }
    void end(uint32_t n) { run_to(*this, n); }
    uint32_t heard() const { return core.engine().heard_channels((1u << channels) - 1u, kHeard); }
};

// YM2151: восемь каналов по четыре оператора, у канала свои левая и правая
// стороны; отсчёт чипа - вход / 64.
struct Opm2151 : Unit {
    Fm<Opm> fm;
    Box<2> box;            // левая и правая стороны
    OpnShadow<272> shadow; // регистры, взятие клавиш по каналу, PMD

    Opm2151(const Unit& u, const Blocks&) : Unit(u) { set_span(*this, 64); }
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t) {
        if (opm_skip(shadow, a, b)) return;
        run_to(*this, at);
        fm.write(a, b);
    }
    void run(int32_t* l, int32_t* r, uint32_t n) {
        int16_t* out[Opm::CHANNELS];
        rows(out, sc->fm, Opm::CHANNELS);
        stereo_run([&](uint32_t k) { return fm.engine.generate_channels(Opm::ALL_CHANNELS, out, k, 1u << 24, 0, 32767); },
                   [&](uint32_t c) { return sides(fm.engine.regs(), c); }, box, out, Opm::CHANNELS, span, recip, gain, l, r, n);
    }
    void end(uint32_t n) { run_to(*this, n); }
    uint32_t heard() const { return fm.engine.heard_channels(Opm::ALL_CHANNELS, kHeard); }
};

// OPL2 (YM3812, YM3526) и OPL3 на движке OPL3.
struct Opl : Unit {
    Fm<Opl3> fm;
    const bool opl3;
    const uint32_t channels;
    const uint32_t ratio; // отсчётов чипа на отсчёт выхода, Q24
    bool wave = false;    // OPL2: формы волны разрешены регистром 1

    Opl(const Unit& u, const Blocks&)
        : Unit(u)
        , opl3(spec.kind == Chip::Opl3)
        , channels(opl3 ? Opl3::CHANNELS : 9u)
        , ratio(fm_ratio(spec.clock, opl3 ? 288 : 72)) {}
    void write(uint32_t at, uint8_t cmd, uint8_t a, uint8_t b, uint8_t) {
        if (!opl3) {
            // YM3812 даёт формы волны только при разряде 5 регистра 1, у
            // YM3526 их нет; движок OPL3 даёт всегда.
            if (a == 0x01) wave = (b & 0x20u) != 0 && spec.id != kYm3526;
            if (a >= 0xe0 && a <= 0xf5 && !wave) b = 0;
        }
        run_to(*this, at);
        // Вторая команда OPL3 - второй банк регистров.
        fm.write((cmd != spec.cmd ? 0x100u : 0u) | a, b);
    }
    // Не встраивается: один на OPL и Y8950.
    SOUNDSINTH_NOINLINE void run(int32_t* l, int32_t* r, uint32_t n) {
        int16_t* out[Opl3::CHANNELS] = {};
        rows(out, sc->fm, channels);
        const uint32_t live = fm.engine.generate_channels((1u << channels) - 1u, out, n, ratio, opl3 ? 0 : 1, 32767);
        // Выходы канала A и C сводятся в левую сторону, B и D - в правую.
        // OPL3 без режима OPL3 (NEW = 0) шлёт канал во все четыре выхода -
        // уровень вдвое; так сводит VGMPlay на ядре MAME. У OPL2 выход
        // один: канал идёт в каждую сторону по разу.
        // Каждый выход - своя цифровая сумма, ограниченная шкалой ЦАП, как в
        // микросхеме и ymfm.
        const auto& regs = fm.engine.regs();
        const bool four  = opl3 && regs.newflag() == 0;
        const int16_t* src[Opl3::CHANNELS];
        uint8_t to[Opl3::CHANNELS];
        uint32_t m = 0;
        for (uint32_t c = 0; c < channels; c++) {
            if (((live >> c) & 1u) == 0) continue; // в канале тишина
            const uint32_t co = ymfm::opl3_registers::channel_offset(c);
            src[m]            = out[c];
            to[m++]           = static_cast<uint8_t>(four ? 0x0fu
                                                          : (regs.ch_output_0(co) != 0 ? 1u : 0u) | (regs.ch_output_1(co) != 0 ? 2u : 0u) |
                                                                (regs.ch_output_2(co) != 0 ? 4u : 0u) | (regs.ch_output_3(co) != 0 ? 8u : 0u));
        }
        for (uint32_t i = 0; m != 0 && i < n; i++) {
            int32_t a = 0, b = 0, c = 0, d = 0;
            for (uint32_t q = 0; q < m; q++) {
                const int32_t v  = src[q][i];
                const uint8_t o  = to[q];
                a               += (o & 1u) != 0 ? v : 0;
                b               += (o & 2u) != 0 ? v : 0;
                c               += (o & 4u) != 0 ? v : 0;
                d               += (o & 8u) != 0 ? v : 0;
            }
            l[i] += (soundsinth::sat_s16(a) + soundsinth::sat_s16(c)) * gain;
            r[i] += (soundsinth::sat_s16(b) + soundsinth::sat_s16(d)) * gain;
        }
    }
    void end(uint32_t n) { run_to(*this, n); }
    uint32_t heard() const { return fm.engine.heard_channels((1u << channels) - 1u, kHeard); }
};

// Y8950: OPL без форм волны и ADPCM-B - движок ymfm с памятью из блоков 0x88.
// run и end - свои: унаследованный end считал бы один FM.
struct Y8950 : Opl {
    Rom memory;
    ymfm::adpcm_b_engine adpcm{memory};
    Box<1> box;

    Y8950(const Unit& u, const Blocks& blocks) : Opl(u, blocks), memory(blocks, spec.second, 0, 0x88) {
        adpcm.reset();
        // ADPCM-B идёт на частоте отсчётов FM.
        set_span(*this, 72);
    }
    void write(uint32_t at, uint8_t, uint8_t a, uint8_t b, uint8_t) {
        run_to(*this, at);
        if (a == 0x08) {
            // Регистр поделён: младшая тетрада - ADPCM-B, старшие разряды - FM.
            adpcm.write(1, static_cast<uint8_t>((b & 0x0fu) | 0x80u));
            fm.write(a, b & 0xc0u);
        } else if ((a >= 0x07 && a <= 0x12) || (a >= 0x15 && a <= 0x17)) {
            adpcm.write(a - 0x07u, b);
        } else if (a < 0x18 || a > 0x1a) {
            // 0x18-0x1A - ввод-вывод; форм волны у чипа нет.
            fm.write(a, a >= 0xe0 && a <= 0xf5 ? uint8_t{0} : b);
        }
    }
    void run(int32_t* l, int32_t* r, uint32_t n) {
        Opl::run(l, r, n);
        // ADPCM-B - поверх FM, на частоте чипа: отсчёт движка сдвинут на три
        // разряда, как в ymfm, и идёт в обе стороны.
        int16_t* const row[1] = {sc->fm[Opl3::CHANNELS - 1u]};
        auto make             = [&](uint32_t k) {
            for (uint32_t i = 0; i < k; i++) {
                ymfm::ymfm_output<1> o;
                adpcm.clock();
                adpcm.output(o.clear(), 3);
                row[0][i] = static_cast<int16_t>(o.clamp16().data[0]);
            }
            return 1u;
        };
        mono_run(make, box, row, 1, span, recip, gain, l, r, n);
    }
    void end(uint32_t n) { run_to(*this, n); }
    uint32_t heard() const { return Opl::heard() + playing(adpcm); }
};

// Чип вида kind по адресу p - в f указателем на свою структуру. Вид чипа
// становится типом только здесь.
template <class P, class F>
auto visit(Chip kind, P* p, F&& f) {
    switch (kind) {
        case Chip::Sn76489:
            break;
        case Chip::Ay8910:
            return f(static_cast<Ay*>(p));
        case Chip::Saa1099:
            return f(static_cast<Saa*>(p));
        case Chip::Scc:
            return f(static_cast<Scc*>(p));
        case Chip::Ym2203:
            return f(static_cast<Opn*>(p));
        case Chip::Ym2610:
        case Chip::Ym2608:
            return f(static_cast<Opnb*>(p));
        case Chip::Ym2612:
            return f(static_cast<Opn2*>(p));
        case Chip::Ym2413:
            return f(static_cast<Opll*>(p));
        case Chip::Ym2151:
            return f(static_cast<Opm2151*>(p));
        case Chip::Opl2:
        case Chip::Opl3:
            return f(static_cast<Opl*>(p));
        case Chip::Y8950:
            return f(static_cast<Y8950*>(p));
    }
    return f(static_cast<Sn*>(p));
}
template <class F>
auto visit(Unit& u, F&& f) {
    return visit(u.spec.kind, &u, f);
}

// Чип вида T на месте p.
template <class T>
T* make(T* p, const Unit& base, const Blocks& blocks) {
    return new (p) T(base, blocks);
}

size_t chip_bytes(Chip kind) {
    return visit(kind, static_cast<void*>(nullptr), [](auto* c) { return up8(sizeof(*c)); });
}

// Команда чипу: a, b, d - её байты после кода, недостающие - ноль.
void unit_write(Unit& u, uint32_t at, uint8_t cmd, uint8_t a, uint8_t b, uint8_t d = 0) {
    if (!addressed(u.spec, cmd, a)) return;
    visit(u, [&](auto* c) { c->write(at, cmd, a, b, d); });
}

} // namespace

void set_rhythm_rom(const uint8_t* rom) {
    g_rhythm_rom = rom;
}

const char* chip_name(Chip kind) {
    return kind_info(kind).name;
}

const char* part_name(const ChipSpec& chip) {
    switch (chip.kind) {
        case Chip::Ay8910:
            return chip.ym ? "YM2149" : "AY8910";
        case Chip::Opl2:
            return chip.id == kYm3526 ? "YM3526" : "YM3812";
        case Chip::Opl3:
            return "YMF262";
        case Chip::Scc:
            return "K051649";
        case Chip::Ym2612:
            return chip.ym ? "YM3438" : "YM2612";
        case Chip::Ym2413:
            return chip.ym ? "VRC7" : "YM2413";
        case Chip::Ym2610:
            return chip.ym ? "YM2610B" : "YM2610";
        default:
            return chip_name(chip.kind);
    }
}

bool parse(const uint8_t* data, uint32_t size, Info& out) {
    out = Info{};
    if (data == nullptr || size < 0x40 || std::memcmp(data, "Vgm ", 4) != 0) return false;
    out.version       = le32(data + 0x08);
    const uint32_t at = le32(data + 0x34);
    out.data_start    = out.version >= 0x150 && at != 0 ? 0x34 + at : 0x40;
    if (out.data_start < 0x40 || out.data_start > size) return false;
    out.total_samples   = le32(data + 0x18);
    out.loop_samples    = le32(data + 0x20);
    const uint32_t loop = le32(data + 0x1c);
    if (loop != 0 && out.loop_samples != 0 && 0x1c + loop >= out.data_start && 0x1c + loop < size) out.loop_start = 0x1c + loop;
    // Заголовок кончается началом данных или дополнительным заголовком, если
    // тот лежит раньше: дальше байты - не поля заголовка.
    const uint32_t ext_at = 0xbc + 4 <= out.data_start ? le32(data + 0xbc) : 0u;
    const uint32_t ext    = ext_at != 0 && ext_at <= size && 0xbc + ext_at + 12 <= size ? 0xbc + ext_at : 0u;
    const Header h{data, ext != 0 && ext < out.data_start ? ext : out.data_start};
    // База повторов - с версии 1.50, множитель - с 1.51; ноль - множителя нет.
    if (out.version >= 0x150) out.loop_base = static_cast<int8_t>(h.byte(0x7e));
    if (out.version >= 0x151 && h.byte(0x7f) != 0) out.loop_modifier = h.byte(0x7f);
    parse_chips(data, size, h, out);
    parse_volumes(data, size, ext, out);
    normalize(h.byte(0x7c), out);
    return true;
}

bool gd3_field(const uint8_t* data, uint32_t size, uint32_t index, const uint8_t** text, uint32_t* units) {
    *units = 0;
    if (data == nullptr || size < 0x40) return false;
    const uint32_t rel = le32(data + 0x14);
    if (rel == 0 || rel >= size - 0x14 || size - 0x14 - rel < 12 || std::memcmp(data + 0x14 + rel, "Gd3 ", 4) != 0) return false;
    uint32_t p         = 0x14 + rel + 12;
    const uint32_t len = le32(data + p - 4), end = len < size - p ? p + len : size;
    // Строки - UTF-16LE с нулём в конце, подряд.
    for (; index != 0 && p + 1 < end; p += 2) {
        if (data[p] == 0 && data[p + 1] == 0) index--;
    }
    if (index != 0) return false;
    uint32_t n = 0;
    while (p + 2 * n + 1 < end && (data[p + 2 * n] != 0 || data[p + 2 * n + 1] != 0)) {
        ++n;
    }
    *text  = data + p;
    *units = n;
    return n != 0;
}

bool gd3_text(const uint8_t* data, uint32_t size, uint32_t index, char* out, uint32_t cap) {
    if (cap == 0) return false;
    out[0]           = 0;
    const uint8_t* t = nullptr;
    uint32_t units   = 0;
    if (!gd3_field(data, size, index, &t, &units)) return false;
    uint32_t n = 0;
    for (; n < units && n + 1 < cap; ++n) {
        out[n] = t[2 * n + 1] == 0 && t[2 * n] >= 0x20 && t[2 * n] < 0x7f ? static_cast<char>(t[2 * n]) : '?';
    }
    out[n] = 0;
    return true;
}

size_t Player::memory_need(const Info& info) {
    size_t n = up8(sizeof(Scratch));
    for (uint32_t i = 0; i < info.count; i++) {
        n += chip_bytes(info.chips[i].kind);
    }
    return n;
}

bool Player::open(const uint8_t* data, uint32_t size, void* mem, size_t mem_bytes, uint32_t loops, void (*service)(void*), void* user) {
    if (!parse(data, size, info_)) return false;
    drop_unused(data, size, info_, service, user);
    if (info_.count == 0 || mem == nullptr || mem_bytes < memory_need(info_)) return false;
    data_           = data;
    blocks_.data    = data;
    size_           = size;
    loops_          = loops;
    cursor_         = Cursor{};
    cursor_.pos     = info_.data_start;
    auto* at        = static_cast<uint8_t*>(mem);
    auto* const sc  = new (at) Scratch();
    at             += up8(sizeof(Scratch));
    for (uint32_t i = 0; i < info_.count; i++) {
        const ChipSpec& s = info_.chips[i];
        const Unit base{s, (kind_info(s.kind).gain * s.volume) >> 8, sc, &fm_pieces_};
        units_[i]  = visit(s.kind, static_cast<void*>(at), [&](auto* c) -> Unit* { return make(c, base, blocks_); });
        at        += chip_bytes(s.kind);
    }
    return true;
}

uint32_t Player::total_frames() const {
    if (info_.loop_start == 0) return info_.total_samples;
    return loops_ == 0 ? 0 : info_.total_samples + info_.loop_samples * (loops_ - 1u);
}

uint32_t Player::heard() const {
    uint32_t n = 0;
    for (uint32_t i = 0; i < info_.count; i++) {
        n += visit(*units_[i], [](auto* c) { return c->heard(); });
    }
    return n;
}

// Кусок образа памяти - в таблицу; повторный проход журнала его не дублирует.
void Blocks::rom_add(const RomBlock& block) {
    for (uint32_t i = 0; i < rom_count; i++) {
        if (rom[i].at == block.at) return;
    }
    if (rom_count < kRom) rom[rom_count++] = block;
}

// Блок отсчётов ЦАП - в банк; блок, который журнал проходит повторно (петля,
// обход батча каждым чипом), уже там.
void Blocks::pcm_add(uint32_t at, uint32_t size) {
    for (uint32_t i = 0; i < pcm_count; i++) {
        if (pcm[i].at == at) return;
    }
    if (pcm_count == kPcm) return;
    pcm[pcm_count++]  = Pcm{at, size};
    pcm_bytes        += size;
}

uint8_t Blocks::pcm_byte(uint32_t pos) const {
    for (uint32_t i = 0; i < pcm_count; i++) {
        if (pos < pcm[i].size) return data[pcm[i].at + pos];
        pos -= pcm[i].size;
    }
    return 0x80;
}

// Команды журнала на n отсчётов с места c - чипу u, с номером отсчёта.
// Отсчёт ЦАП из банка (0x8n) приходит записью в регистр 0x2A первого YM2612.
// Команды чужих чипов и их блоки данных пропускаются по длине. skip -
// перемотка: все записи ложатся в отсчёт 0, времени чипам она не даёт.
void Player::scan(Cursor& c, uint32_t n, Unit& u, bool skip) {
    uint32_t at = 0;
    bool dry    = false; // петля пройдена без единой паузы
    while (at < n) {
        if (c.wait != 0) {
            const uint32_t step  = c.wait < n - at ? c.wait : n - at;
            c.wait              -= step;
            at                  += step;
            dry                  = false;
            continue;
        }
        if (c.ended) return;
        const uint32_t left = c.pos < size_ ? size_ - c.pos : 0;
        const uint8_t cmd   = left != 0 ? data_[c.pos] : uint8_t{0x66};
        const uint8_t* p    = data_ + c.pos;
        if (cmd == 0x66) {
            const bool again = info_.loop_start != 0 && !dry && (loops_ == 0 || c.loops + 1u < loops_);
            if (!again) {
                c.ended = true;
                return;
            }
            c.loops++;
            c.pos = info_.loop_start;
            dry   = true;
            continue;
        }
        // Неизвестная или обрезанная команда: длины нет, дальше не пройти.
        const uint32_t len = command_bytes(p, left, info_.version);
        if (len == 0) {
            c.ended  = true;
            c.broken = true;
            return;
        }
        const uint32_t to = skip ? 0u : at;
        if (cmd == 0x61) {
            c.wait = le16(p + 1);
        } else if (cmd == 0x62) {
            c.wait = 735;
        } else if (cmd == 0x63) {
            c.wait = 882;
        } else if (cmd >= 0x70 && cmd <= 0x7f) {
            c.wait = (cmd & 0x0fu) + 1u;
        } else if (cmd >= 0x80 && cmd <= 0x8f) {
            unit_write(u, to, 0x52, 0x2a, blocks_.pcm_byte(c.pcm++));
            c.wait = cmd & 0x0fu;
        } else if (cmd == 0xe0) {
            c.pcm = le32(p + 1);
        } else if (cmd == 0x67) {
            if (p[2] == 0x00) blocks_.pcm_add(c.pos + 7u, len - 7u);
            // Образ памяти: полный размер, адрес начала куска, данные;
            // разряд 31 длины блока - образ второго чипа.
            if ((p[2] == 0x88 || (p[2] >= 0x81 && p[2] <= 0x83)) && len >= 15u) {
                blocks_.rom_add(RomBlock{le32(p + 11), len - 15u, c.pos + 15u, (le32(p + 3) & 0x80000000u) != 0, p[2]});
            }
        } else if (cmd >= 0x90 && cmd <= 0x95) {
            visit(u, [&](auto* chip) { chip->stream_cmd(to, p); });
        } else if (len == 2) {
            unit_write(u, to, cmd, p[1], 0);
        } else if (len == 3) {
            unit_write(u, to, cmd, p[1], p[2]);
        } else if (len == 4) {
            unit_write(u, to, cmd, p[1], p[2], p[3]);
        }
        c.pos += len;
    }
}

// Батч: каждый чип проходит команды журнала на n отсчётов с места cursor_
// и сводит свой выход в mix. Перемотка (skip) кладёт записи в отсчёт 0 и
// считает один отсчёт.
void Player::batch(int32_t* mix_l, int32_t* mix_r, uint32_t n, bool skip) {
    const uint32_t out = skip ? 1u : n;
    Cursor next        = cursor_;
    for (uint32_t i = 0; i < info_.count; i++) {
        Unit& u = *units_[i];
        u.mix_l = mix_l;
        u.mix_r = mix_r;
        u.done  = 0;
        next    = cursor_;
        visit(u, [&](auto* c) { c->begin(out); });
        scan(next, n, u, skip);
        visit(u, [&](auto* c) { c->end(out); });
    }
    cursor_ = next;
}

void Player::skip(uint32_t n) {
    // Чипы считают один отсчёт, он никуда не идёт.
    int32_t sink[2] = {};
    batch(&sink[0], &sink[1], n, true);
}

bool Player::render_add(int32_t* mix_l, int32_t* mix_r, uint32_t n) {
    for (uint32_t at = 0; at < n;) {
        const uint32_t k = n - at < kBatch ? n - at : kBatch;
        batch(mix_l + at, mix_r + at, k, false);
        at += k;
    }
    return !cursor_.ended;
}

} // namespace player::vgm
