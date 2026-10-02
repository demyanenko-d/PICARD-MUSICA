// SPDX-License-Identifier: MIT
// sf2bake - пекарь банка инструментов: .sf2 -> .ssb
//
//   sf2bake <банк.sf2> <выход.ssb> [--rate-cap 32000] [--report файл]
//
// Превращает SoundFont в готовые трекерные инструменты: на плате банк
// только читается, ни разбора, ни перекодирования там нет.
//
// --- Почему C++, а не скрипт ---
//
// Пекарь должен кодировать PCM тем же dpcm8::encode_block, что играет
// плата. Кодек на скриптовом языке был бы вторым кодеком, который молча
// разойдётся с первым. По той же причине раскладка записей берётся из
// общего заголовка bank/bank_format.h, а не описывается здесь заново.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/bank/bank_codec.h"
#include "core/bank/bank_reader.h"
#include "core/bank/bank_format.h"
#include "core/codec/dpcm8.h"
#include "core/model/instrument.h"

namespace {

using namespace soundsinth::bank;
namespace dpcm8 = soundsinth::dpcm8;

// --- Генераторы SF2, которые нас интересуют ---
enum Gen {
    GEN_START_OFFSET         = 0,
    GEN_END_OFFSET           = 1,
    GEN_LOOP_START_OFFSET    = 2,
    GEN_LOOP_END_OFFSET      = 3,
    GEN_START_COARSE         = 4,
    GEN_INITIAL_FILTER_FC    = 8,
    GEN_INITIAL_FILTER_Q     = 9,
    GEN_MOD_ENV_TO_FILTER_FC = 11,
    GEN_END_COARSE           = 12,
    GEN_PAN                  = 17,
    GEN_DELAY_MOD_ENV        = 25,
    GEN_ATTACK_MOD_ENV       = 26,
    GEN_HOLD_MOD_ENV         = 27,
    GEN_DECAY_MOD_ENV        = 28,
    GEN_SUSTAIN_MOD_ENV      = 29,
    GEN_RELEASE_MOD_ENV      = 30,
    GEN_DELAY_VOL_ENV        = 33,
    GEN_ATTACK_VOL_ENV       = 34,
    GEN_HOLD_VOL_ENV         = 35,
    GEN_DECAY_VOL_ENV        = 36,
    GEN_SUSTAIN_VOL_ENV      = 37,
    GEN_RELEASE_VOL_ENV      = 38,
    GEN_INSTRUMENT           = 41,
    GEN_KEY_RANGE            = 43,
    GEN_VEL_RANGE            = 44,
    GEN_LOOP_START_COARSE    = 45,
    GEN_INITIAL_ATTENUATION  = 48,
    GEN_LOOP_END_COARSE      = 50,
    GEN_COARSE_TUNE          = 51,
    GEN_FINE_TUNE            = 52,
    GEN_SAMPLE_ID            = 53,
    GEN_SAMPLE_MODES         = 54,
    GEN_SCALE_TUNING         = 56,
    GEN_EXCLUSIVE_CLASS      = 57,
    GEN_OVERRIDING_ROOT      = 58,
};

// Значение генератора "не задан" - отличаем от заданного нуля.
constexpr int32_t kGenUnset = INT32_MIN;

// Умолчания генераторов по SF2.04, раздел 8.1.3. Нужны в одном месте - при
// сложении пресетного смещения с уровнем инструмента (см. combine). У
// большинства генераторов умолчание ноль, но у среза фильтра 13500 центов,
// а у всех времён огибающих -12000. Если считать их нулём, смещение
// подменяется абсолютом: пресет, сдвигающий срез на -1000 центов,
// превращался в срез 1000 центов, то есть 14 Гц вместо 11 кГц. Так банк
// получил фильтр у 2403 инструментов из 3575, хотя в самом .sf2 он стоит
// у 39 зон из 2486.
inline int32_t sf2_gen_default(int id) {
    switch (id) {
        case 8:
            return 13500; // initialFilterFc
        case 21:
        case 23: // delayModLFO/delayVibLFO
        case 25:
        case 26:
        case 27:
        case 28:
        case 30: // *ModEnv
        case 33:
        case 34:
        case 35:
        case 36:
        case 38:
            return -12000; // *VolEnv
        case 46:
        case 47:
        case 58:
            return -1; // keynum/velocity/overridingRootKey
        case 56:
            return 100; // scaleTuning
        default:
            return 0;
    }
}

struct GenSet {
    int32_t g[64];
    GenSet() {
        for (auto& v : g)
            v = kGenUnset;
    }
    int32_t get(int id, int32_t dflt) const { return g[id] == kGenUnset ? dflt : g[id]; }
    bool has(int id) const { return g[id] != kGenUnset; }
};

// --- Разбор файла ---
struct Sf2Sample {
    std::string name;
    uint32_t start = 0, end = 0, loop_start = 0, loop_end = 0, rate = 0;
    uint8_t root      = 60;
    int8_t correction = 0;
    uint16_t link = 0, type = 0;
};

// Модулятор от силы удара: источник (с направлением, полярностью и кривой) и
// величина в единицах назначения.
struct VelMod {
    uint16_t src;
    int16_t amount;
};

struct Sf2Zone { // зона инструмента, уже со слитыми генераторами
    GenSet gen;
    // Модулятор `initialFilterFc <- velocity`, сведённый к размаху в центах
    // от velocity 0 до 127. Генератором эта связь не выражается, а банки на
    // неё опираются: у GeneralUser GS 248 зон, у SGM 134.
    int32_t vel_to_fc_cents = 0;
    // Модуляторы `modEnvToFilterFc <- velocity`: глубина огибающей фильтра
    // от силы удара. У GeneralUser их 123 (36 пресетов: медь, басы,
    // электропиано), у валторны +6400 центов поверх генератора 400.
    std::vector<VelMod> vel_to_menv;
};

struct Sf2Inst {
    std::string name;
    std::vector<Sf2Zone> zones;
};

struct Sf2PresetZone {
    GenSet gen;                  // генераторы уровня пресета (складываются к зонам инструмента)
    int32_t vel_to_fc_cents = 0; // модулятор уровня пресета - тоже слагаемое
    std::vector<VelMod> vel_to_menv;
    int instrument = -1;
};

struct Sf2Preset {
    std::string name;
    uint16_t bank = 0, program = 0;
    std::vector<Sf2PresetZone> zones;
};

struct Sf2 {
    std::string name;
    std::vector<int16_t> pcm; // весь smpl целиком, 16 бит
    std::vector<Sf2Sample> samples;
    std::vector<Sf2Inst> insts;
    std::vector<Sf2Preset> presets;
};

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

bool parse_sf2(const std::string& path, Sf2& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        err = "the read was cut short";
        return false;
    }
    std::fclose(f);

    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "sfbk", 4) != 0) {
        err = "not a SoundFont (no RIFF/sfbk)";
        return false;
    }

    // Верхний уровень: списки INFO / sdta / pdta.
    struct Chunk {
        size_t off, size;
    };
    std::map<std::string, Chunk> lists;
    size_t pos       = 12;
    const size_t end = 8 + rd32(buf.data() + 4);
    while (pos + 12 <= std::min(end, buf.size())) {
        const uint32_t sz = rd32(buf.data() + pos + 4);
        if (std::memcmp(buf.data() + pos, "LIST", 4) == 0) {
            lists[std::string(reinterpret_cast<const char*>(buf.data()) + pos + 8, 4)] = {pos + 12, sz - 4};
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!lists.count("pdta") || !lists.count("sdta")) {
        err = "the required pdta/sdta lists are missing";
        return false;
    }

    // Имя банка из INFO/INAM - только для строки в логе.
    if (lists.count("INFO")) {
        size_t p       = lists["INFO"].off;
        const size_t e = p + lists["INFO"].size;
        while (p + 8 <= e) {
            const uint32_t sz = rd32(buf.data() + p + 4);
            if (std::memcmp(buf.data() + p, "INAM", 4) == 0) {
                out.name.assign(reinterpret_cast<const char*>(buf.data()) + p + 8, std::min<uint32_t>(sz, 31));
                out.name = out.name.c_str(); // обрезать по первому нулю
            }
            p += 8 + sz + (sz & 1);
        }
    }

    // PCM: smpl - 16 бит; sm24 (младшие 8 бит 24-битного) не читаем:
    // резидентно у нас всё равно 8 бит на отсчёт.
    {
        size_t p       = lists["sdta"].off;
        const size_t e = p + lists["sdta"].size;
        while (p + 8 <= e) {
            const uint32_t sz = rd32(buf.data() + p + 4);
            if (std::memcmp(buf.data() + p, "smpl", 4) == 0) {
                out.pcm.resize(sz / 2);
                std::memcpy(out.pcm.data(), buf.data() + p + 8, sz);
            }
            p += 8 + sz + (sz & 1);
        }
    }
    if (out.pcm.empty()) {
        err = "empty smpl";
        return false;
    }

    // pdta: девять подчанков фиксированной раскладки.
    std::map<std::string, Chunk> sub;
    {
        size_t p       = lists["pdta"].off;
        const size_t e = p + lists["pdta"].size;
        while (p + 8 <= e) {
            const uint32_t sz                                                   = rd32(buf.data() + p + 4);
            sub[std::string(reinterpret_cast<const char*>(buf.data()) + p, 4)]  = {p + 8, sz};
            p                                                                  += 8 + sz + (sz & 1);
        }
    }
    for (const char* need : {"phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr"}) {
        if (!sub.count(need)) {
            err = std::string("pdta has no ") + need;
            return false;
        }
    }

    // shdr
    {
        const size_t n = sub["shdr"].size / 46;
        for (size_t i = 0; i + 1 < n; ++i) { // последняя запись - терминатор EOS
            const uint8_t* r = buf.data() + sub["shdr"].off + i * 46;
            Sf2Sample s;
            s.name.assign(reinterpret_cast<const char*>(r), 20);
            s.name       = s.name.c_str();
            s.start      = rd32(r + 20);
            s.end        = rd32(r + 24);
            s.loop_start = rd32(r + 28);
            s.loop_end   = rd32(r + 32);
            s.rate       = rd32(r + 36);
            s.root       = r[40];
            s.correction = static_cast<int8_t>(r[41]);
            s.link       = rd16(r + 42);
            s.type       = rd16(r + 44);
            out.samples.push_back(std::move(s));
        }
    }

    // Слияние генераторов зоны: глобальная зона (та, у которой нет
    // терминирующего генератора) применяется ко всем последующим.
    auto read_gens = [&](const char* genChunk, size_t from, size_t to) {
        GenSet g;
        const uint8_t* base = buf.data() + sub[genChunk].off;
        for (size_t i = from; i < to; ++i) {
            const uint16_t op = rd16(base + i * 4);
            const uint16_t am = rd16(base + i * 4 + 2);
            if (op < 64) g.g[op] = static_cast<int16_t>(am); // знаковая трактовка; диапазоны разбираются отдельно
            if (op == GEN_KEY_RANGE || op == GEN_VEL_RANGE || op == GEN_SAMPLE_ID || op == GEN_INSTRUMENT || op == GEN_OVERRIDING_ROOT ||
                op == GEN_SAMPLE_MODES || op == GEN_EXCLUSIVE_CLASS || op == GEN_SCALE_TUNING) {
                g.g[op] = am; // эти беззнаковые
            }
        }
        return g;
    };
    // Модуляторы: интересует один - `initialFilterFc <- velocity`.
    //
    // Запись модулятора - десять байт: источник (u16), назначение (u16),
    // величина (s16), источник-множитель (u16), преобразование (u16).
    // В источнике младшие семь бит - индекс, бит 7 - "это CC", бит 8 -
    // направление (0: min->max, 1: max->min), бит 9 - полярность
    // (0: униполярный, 1: биполярный).
    //
    // Сводим к размаху в центах между velocity 0 и 127. Форма кривой
    // (линейная, вогнутая, выпуклая) на размах не влияет: у всех типов она
    // даёт 0 на одном конце и 1 на другом. Поэтому размах зависит только от
    // направления и полярности и берётся точно, без таблиц кривых из
    // спецификации.
    auto read_vel_to_fc = [&](const char* modChunk, size_t from, size_t to) {
        int32_t total = 0;
        if (!sub.count(modChunk)) return total;
        const uint8_t* base = buf.data() + sub[modChunk].off;
        for (size_t i = from; i < to; ++i) {
            const uint16_t src  = rd16(base + i * 10);
            const uint16_t dest = rd16(base + i * 10 + 2);
            const int16_t amt   = static_cast<int16_t>(rd16(base + i * 10 + 4));
            if (dest != GEN_INITIAL_FILTER_FC || amt == 0) continue;
            if (src & 0x80) continue;        // источник - контроллер, не velocity
            if ((src & 0x7F) != 2) continue; // 2 = сила удара
            const bool descending = (src & 0x0100) != 0;
            const bool bipolar    = (src & 0x0200) != 0;
            int32_t swing         = bipolar ? 2 * int32_t(amt) : int32_t(amt);
            if (descending) swing = -swing; // при max->min размах меняет знак
            total += swing;
        }
        return total;
    };
    // Модуляторы `modEnvToFilterFc <- velocity` - списком: их вклад зависит от
    // силы удара нелинейно (кривая, полярность), и считается он при выпечке
    // слоя (vel_mod_cents).
    auto read_vel_to_menv = [&](const char* modChunk, size_t from, size_t to) {
        std::vector<VelMod> mods;
        if (!sub.count(modChunk)) return mods;
        const uint8_t* base = buf.data() + sub[modChunk].off;
        for (size_t i = from; i < to; ++i) {
            const uint16_t src     = rd16(base + i * 10);
            const uint16_t dest    = rd16(base + i * 10 + 2);
            const int16_t amt      = static_cast<int16_t>(rd16(base + i * 10 + 4));
            const uint16_t amt_src = rd16(base + i * 10 + 6);
            if (dest != GEN_MOD_ENV_TO_FILTER_FC || amt == 0) continue;
            if (src & 0x80) continue;        // источник - контроллер, не velocity
            if ((src & 0x7F) != 2) continue; // 2 = сила удара
            if (amt_src != 0) continue;      // множитель от второго источника не переносится
            mods.push_back(VelMod{src, amt});
        }
        return mods;
    };

    auto merge = [](const GenSet& global, const GenSet& local) {
        GenSet r = global;
        for (int i = 0; i < 64; ++i)
            if (local.g[i] != kGenUnset) r.g[i] = local.g[i];
        return r;
    };

    // inst + ibag + igen
    {
        const size_t nInst  = sub["inst"].size / 22;
        const uint8_t* ibag = buf.data() + sub["ibag"].off;
        for (size_t j = 0; j + 1 < nInst; ++j) {
            const uint8_t* r = buf.data() + sub["inst"].off + j * 22;
            Sf2Inst inst;
            inst.name.assign(reinterpret_cast<const char*>(r), 20);
            inst.name           = inst.name.c_str();
            const uint16_t bag0 = rd16(r + 20);
            const uint16_t bag1 = rd16(r + 22 + 20);
            GenSet global;
            bool have_global      = false;
            int32_t global_vel_fc = 0; // модуляторы глобальной зоны - ко всем последующим
            std::vector<VelMod> global_vel_menv;
            for (uint16_t b = bag0; b < bag1; ++b) {
                const uint16_t g0         = rd16(ibag + b * 4);
                const uint16_t g1         = rd16(ibag + (b + 1) * 4);
                const uint16_t m0         = rd16(ibag + b * 4 + 2);
                const uint16_t m1         = rd16(ibag + (b + 1) * 4 + 2);
                GenSet g                  = read_gens("igen", g0, g1);
                const int32_t vfc         = read_vel_to_fc("imod", m0, m1);
                std::vector<VelMod> vmenv = read_vel_to_menv("imod", m0, m1);
                if (!g.has(GEN_SAMPLE_ID)) {
                    // Зона без sampleID - глобальная, но только первая.
                    if (!have_global && b == bag0) {
                        global          = g;
                        global_vel_fc   = vfc;
                        global_vel_menv = vmenv;
                        have_global     = true;
                    }
                    continue;
                }
                Sf2Zone z;
                z.gen = have_global ? merge(global, g) : g;
                // Модулятор зоны перебивает одноимённый глобальный (как и генератор), а
                // не складывается с ним.
                z.vel_to_fc_cents = vfc != 0 ? vfc : global_vel_fc;
                z.vel_to_menv     = !vmenv.empty() ? vmenv : global_vel_menv;
                inst.zones.push_back(std::move(z));
            }
            out.insts.push_back(std::move(inst));
        }
    }

    // phdr + pbag + pgen
    {
        const size_t nPre   = sub["phdr"].size / 38;
        const uint8_t* pbag = buf.data() + sub["pbag"].off;
        for (size_t i = 0; i + 1 < nPre; ++i) {
            const uint8_t* r = buf.data() + sub["phdr"].off + i * 38;
            Sf2Preset p;
            p.name.assign(reinterpret_cast<const char*>(r), 20);
            p.name              = p.name.c_str();
            p.program           = rd16(r + 20);
            p.bank              = rd16(r + 22);
            const uint16_t bag0 = rd16(r + 24);
            const uint16_t bag1 = rd16(r + 38 + 24);
            GenSet global;
            bool have_global      = false;
            int32_t global_vel_fc = 0;
            std::vector<VelMod> global_vel_menv;
            for (uint16_t b = bag0; b < bag1; ++b) {
                const uint16_t g0         = rd16(pbag + b * 4);
                const uint16_t g1         = rd16(pbag + (b + 1) * 4);
                const uint16_t m0         = rd16(pbag + b * 4 + 2);
                const uint16_t m1         = rd16(pbag + (b + 1) * 4 + 2);
                GenSet g                  = read_gens("pgen", g0, g1);
                const int32_t vfc         = read_vel_to_fc("pmod", m0, m1);
                std::vector<VelMod> vmenv = read_vel_to_menv("pmod", m0, m1);
                if (!g.has(GEN_INSTRUMENT)) {
                    if (!have_global && b == bag0) {
                        global          = g;
                        global_vel_fc   = vfc;
                        global_vel_menv = vmenv;
                        have_global     = true;
                    }
                    continue;
                }
                Sf2PresetZone z;
                z.gen             = have_global ? merge(global, g) : g;
                z.vel_to_fc_cents = vfc != 0 ? vfc : global_vel_fc;
                z.vel_to_menv     = !vmenv.empty() ? vmenv : global_vel_menv;
                z.instrument      = g.g[GEN_INSTRUMENT];
                p.zones.push_back(std::move(z));
            }
            out.presets.push_back(std::move(p));
        }
    }
    return true;
}

// --- Пересчёты SF2 -> наши шкалы ---

// Таймценты в миллисекунды. -12000 (умолчание) = 1 мс; SF2 считает
// "мгновенно" всё, что меньше -11000.
uint16_t timecents_to_ms(int32_t tc) {
    if (tc <= -11000) return 0;
    const double sec = std::pow(2.0, tc / 1200.0);
    const double ms  = sec * 1000.0;
    return static_cast<uint16_t>(std::min(65535.0, std::max(0.0, ms)));
}

// Сантибелы огибающей в множитель 0..1 по спецификации: единица - 0.1 дБ.
double centibels_to_linear(int32_t cb) {
    if (cb <= 0) return 1.0;
    if (cb >= 1440) return 0.0;
    return std::pow(10.0, -cb / 200.0);
}

// initialAttenuation читается иначе, и на это есть причина.
//
// Спецификация называет единицу сантибелом, но банки делаются под
// реализации с множителем 0.4 (так вело себя железо EMU, так считает
// FluidSynth). Разница заметная: в GeneralUser GS затухание задано и на
// уровне инструмента (679 зон), и на уровне пресета (1938 зон), они
// складываются, медиана около сотни сантибел каждая. При прямом чтении
// это -20 дБ на типовой ноте и -50 дБ на хвосте, и весь микс уезжал на
// -43 дБ вместо -15.
double g_atten_db_per_cb = 0.04; // см. ключ --atten-db-per-cb

double attenuation_to_linear(int32_t cb) {
    const double db = cb * g_atten_db_per_cb;
    if (db >= 96.0) return 0.0;
    // Отрицательное затухание (усиление) в банке встречается: у зон пресета
    // минимум -150 сантибел. Если обрезать его до единицы, то, что автор
    // поднял, заиграет тише.
    return std::pow(10.0, -db / 20.0);
}

// Делений шкалы среза на октаву. У банка с версии 4 - 16 (Song::
// filter_units_per_octave у .mid), а не 24, как у IT: срез в герцах
// 110 * 2^(0.25 + cutoff/16) (engine/resonant_filter.cpp) покрывает 131 Гц..
// 20 кГц. При 24 верх шкалы был 5.1 кГц, и всё, что в SF2 выше, выходило
// "открыт": у SGM Halo Pad огибающая раскрывает срез до 8.3 кГц, и атака
// теряла 10..13 дБ в полосе 6..10 кГц.
constexpr double kCutoffUnitsPerOctave = 16.0;

// Абсолютные центы SF2 в шкалу среза 0..127.
uint8_t filter_cents_to_it(int32_t cents) {
    const double hz = 8.176 * std::pow(2.0, cents / 1200.0);
    const double c  = kCutoffUnitsPerOctave * (std::log2(hz / 110.0) - 0.25);
    if (c >= 127.0) return 127;
    if (c <= 0.0) return 0;
    return static_cast<uint8_t>(c + 0.5);
}

// --- Ресемплинг: оконный sinc ---
//
// Рантаймовая децимация 2:1 усреднением пары отсчётов давит на новой
// частоте Найквиста всего 3 дБ и заворачивает верх обратно; у пекаря нет
// причин так делать. Срез чуть ниже Найквиста (0.90 от него), потому что
// воспроизведение интерполирует линейно, а у линейной интерполяции
// отклик sinc^2 - содержимое у самой границы было бы испорчено ещё раз
// уже на плате.
constexpr int kTaps  = 24;
constexpr double kPi = 3.14159265358979323846;

double sinc(double x) {
    return std::abs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
}

// Доля энергии сэмпла выше заданной частоты - то, что пропадёт, если
// опустить его до потолка вдвое ниже.
//
// Первая версия мерила иначе: опустить и поднять обратно тем же
// ресемплером и вычесть. Не годится: ресемплер сдвигает сигнал по фазе,
// вычитание сравнивает несовмещённое, и "потеря" выходила положительной
// (+2.9 дБ, то есть ошибка вдвое больше сигнала). Прямой спектр этого
// недостатка не имеет.
void fft_inplace(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / double(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const double ur = re[i + k], ui = im[i + k];
                const double vr     = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const double vi     = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k]           = ur + vr;
                im[i + k]           = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const double nc     = cr * wr - ci * wi;
                ci                  = cr * wi + ci * wr;
                cr                  = nc;
            }
        }
    }
}

// Окон несколько, берётся худшее. Одним окном из середины мерить нельзя:
// у щипковых и у тарелок весь верх в атаке, а середина уже тусклая -
// такой сэмпл был бы опущен зря. Восемь окон скользят по всей длине, и
// берётся максимум доли верха: решение по самому яркому месту, а не по
// среднему.
double energy_above_db(const int16_t* data, size_t count, uint32_t rate, double cutoff_hz) {
    const size_t N = 2048;
    if (count < N) return 0.0; // короткий - не рискуем, оставляем как есть
    const size_t windows = 8;
    const size_t step    = count > N ? (count - N) / windows : 0;
    double worst         = -999.0;
    std::vector<double> re(N), im(N);
    for (size_t w = 0; w <= windows; ++w) {
        const size_t at = step ? w * step : 0;
        if (at + N > count) break;
        for (size_t k = 0; k < N; ++k) {
            const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * double(k) / double(N - 1));
            re[k]            = double(data[at + k]) * win;
            im[k]            = 0.0;
        }
        fft_inplace(re, im);
        double total = 0.0, high = 0.0;
        for (size_t k = 1; k < N / 2; ++k) {
            const double f  = double(k) * rate / double(N);
            const double p  = re[k] * re[k] + im[k] * im[k];
            total          += p;
            if (f > cutoff_hz) high += p;
        }
        if (total <= 0.0) continue;
        const double db = 10.0 * std::log10(high / total + 1e-18);
        if (db > worst) worst = db;
        if (step == 0) break;
    }
    return worst <= -998.0 ? 0.0 : worst;
}

// tilt_db - наклон спектра перед порогом, дБ на октаву. Ноль даёт прежнее
// поведение: порог по чистой доле энергии.
//
// Зачем он нужен: энергия и слышимость - разные вещи. У низкой ноты выше
// 3 кГц меньше десятой доли процента энергии, и порог по доле энергии
// разрешает выбросить весь её верхний регистр, а эти слабые гармоники
// дают удару определённость. Ошибка тем больше, чем ниже нота: замер
// хроматическим прогоном показал потерю 6-8 дБ в полосе 4 кГц на нотах
// 24-48 против 1-3 дБ выше.
//
// Наклон делает верх весомее низа, и граница спектра у басовых записей
// сама уезжает вверх, без нижнего ограничителя.
//
// Опорная частота не нужна: порог берётся отношением взвешенных энергий,
// общий множитель сокращается. Опорой взята частота Найквиста, чтобы веса
// не превышали единицу и не переполняли double на длинных прогонах.
double spectral_top_hz(const int16_t* data, size_t count, uint32_t rate, double loss_db, double tilt_db) {
    const size_t N = 2048;
    if (count < N) return double(rate) / 2.0; // короткий - не рискуем
    const size_t windows = 8;
    const size_t step    = count > N ? (count - N) / windows : 0;
    const double frac    = std::pow(10.0, loss_db / 10.0);
    const double nyq     = double(rate) / 2.0;
    // Вес бина считается один раз на весь сэмпл: он зависит только от номера
    // бина и частоты, а окон восемь.
    std::vector<double> wt(N / 2, 1.0);
    if (tilt_db != 0.0) {
        for (size_t k = 1; k < N / 2; ++k) {
            const double f = double(k) * double(rate) / double(N);
            wt[k]          = std::pow(10.0, tilt_db * std::log2(f / nyq) / 10.0);
        }
    }
    double top = 0.0;
    std::vector<double> re(N), im(N), p(N / 2);
    for (size_t w = 0; w <= windows; ++w) {
        const size_t at = step ? w * step : 0;
        if (at + N > count) break;
        for (size_t k = 0; k < N; ++k) {
            const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * double(k) / double(N - 1));
            re[k]            = double(data[at + k]) * win;
            im[k]            = 0.0;
        }
        fft_inplace(re, im);
        double total = 0.0;
        for (size_t k = 1; k < N / 2; ++k) {
            p[k]   = (re[k] * re[k] + im[k] * im[k]) * wt[k];
            total += p[k];
        }
        if (total <= 0.0) continue;
        // Сверху вниз, накапливая энергию: как только накопленное превысило
        // бюджет, ниже резать нельзя - это и есть граница.
        size_t kt  = 1;
        double acc = 0.0;
        for (size_t k = N / 2; k-- > 1;) {
            acc += p[k];
            if (acc / total >= frac) {
                kt = k;
                break;
            }
        }
        const double f = double(kt) * double(rate) / double(N);
        if (f > top) top = f;
        if (step == 0) break;
    }
    return top;
}

std::vector<int16_t> resample(const std::vector<int16_t>& in, double ratio) {
    if (ratio >= 0.999999) return in;
    const size_t n_out = static_cast<size_t>(in.size() * ratio);
    std::vector<int16_t> out(n_out);
    const double fc = 0.90 * ratio; // срез в долях частоты Найквиста входа
    for (size_t n = 0; n < n_out; ++n) {
        const double p = n / ratio;
        const long c   = static_cast<long>(std::floor(p));
        double acc = 0.0, wsum = 0.0;
        for (long k = c - kTaps + 1; k <= c + kTaps; ++k) {
            const double d  = k - p;
            const double w  = sinc(fc * d) * (0.42 - 0.5 * std::cos(kPi * (d / kTaps + 1.0)) + 0.08 * std::cos(2.0 * kPi * (d / kTaps + 1.0)));
            const long idx  = std::min<long>(std::max<long>(k, 0), static_cast<long>(in.size()) - 1);
            acc            += in[static_cast<size_t>(idx)] * w;
            wsum           += w;
        }
        const double v = wsum > 1e-9 ? acc / wsum : 0.0;
        out[n]         = static_cast<int16_t>(std::min(32767.0, std::max(-32768.0, v)));
    }
    return out;
}

// Хеш содержимого прогона PCM - только для дедупликации внутри пекаря,
// в файл не попадает, поэтому годится быстрый FNV-1a.
uint64_t content_hash(const int8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; ++i) {
        h ^= static_cast<uint8_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h ^ (static_cast<uint64_t>(n) * 1099511628211ull);
}

// --- Огибающая громкости SF2 -> ломаная трекера ---
//
// У SF2 шесть параметров (delay/attack/hold/decay/sustain/release), у
// нас - ломаная по точкам с sustain-петлёй. Два места, где нельзя
// сделать наивно:
//
// 1. Спад и релиз у SF2 линейны в децибелах. Значения точек нашей
//    ломаной - тоже затухание в децибелах (kQuirkEnvelopeDecibel, см.
//    db_to_val ниже), поэтому спад - прямая из двух точек. Релиза в
//    огибающей нет, его ведёт затухание (fadeout), см. ниже.
// 2. Спад и релиз в SF2 - время полного спада на 96 дБ (960 сантибел), а
//    не от текущего уровня. До сустейна на S сантибел проходит S/960
//    этого времени, от сустейна до тишины - (960-S)/960.
struct EnvOut {
    BankEnvelope env;
    uint16_t fadeout_ms;
    bool used;
};

EnvOut build_volume_envelope(const GenSet& g) {
    EnvOut out{};
    out.env.flags            = 0;
    const int32_t delay_tc   = g.get(GEN_DELAY_VOL_ENV, -12000);
    const int32_t attack_tc  = g.get(GEN_ATTACK_VOL_ENV, -12000);
    const int32_t hold_tc    = g.get(GEN_HOLD_VOL_ENV, -12000);
    const int32_t decay_tc   = g.get(GEN_DECAY_VOL_ENV, -12000);
    const int32_t release_tc = g.get(GEN_RELEASE_VOL_ENV, -12000);
    const int32_t sustain_cb = std::max(0, std::min(1440, g.get(GEN_SUSTAIN_VOL_ENV, 0)));

    const uint16_t delay_ms  = timecents_to_ms(delay_tc);
    const uint16_t attack_ms = timecents_to_ms(attack_tc);
    const uint16_t hold_ms   = timecents_to_ms(hold_tc);

    // decayVolEnv и releaseVolEnv в SF2 задают время полного спада на весь
    // диапазон затухания (96 дБ), а не время достижения уровня сустейна.
    // Спад идёт с постоянной скоростью в децибелах, поэтому до сустейна на S
    // сантибел проходит S/960 этого времени, а релиз от сустейна до тишины -
    // оставшиеся (960-S)/960.
    //
    // Раньше здесь бралось полное время спада, и всё, что должно было быстро
    // уходить в фон, оставалось громким: на слух "инструмент вылезает
    // невпопад".
    constexpr double kFullRangeCb  = 960.0;
    const uint32_t decay_full_ms   = timecents_to_ms(decay_tc);
    const uint32_t decay_ms        = static_cast<uint32_t>(decay_full_ms * std::min(1.0, sustain_cb / kFullRangeCb));
    const uint32_t release_full_ms = timecents_to_ms(release_tc);
    const uint32_t release_ms      = static_cast<uint32_t>(release_full_ms * std::max(0.0, (kFullRangeCb - sustain_cb) / kFullRangeCb));

    const double sustain_lin  = centibels_to_linear(sustain_cb);
    const int16_t sustain_val = static_cast<int16_t>(std::lround(64.0 * sustain_lin));

    // Тривиальная огибающая (мгновенная атака, полный сустейн, мгновенный
    // релиз) - одиночный удар вроде перкуссии: огибающая ему не нужна, и её
    // отсутствие экономит 108 байт и работу на тик.
    if (delay_ms == 0 && attack_ms == 0 && hold_ms == 0 && decay_ms == 0 && sustain_val >= 64 && release_ms < 20) {
        out.used       = false;
        out.fadeout_ms = 0;
        return out;
    }

    std::vector<BankEnvelopePoint> pts;
    auto push = [&](uint32_t ms, int32_t val) {
        if (pts.size() >= kMaxEnvelopePoints) return;
        const uint16_t m = static_cast<uint16_t>(std::min<uint32_t>(ms, 65535));
        if (!pts.empty() && m <= pts.back().ms) {
            if (val != pts.back().value && pts.size() < kMaxEnvelopePoints) {
                pts.push_back({static_cast<uint16_t>(std::min<uint32_t>(pts.back().ms + 1u, 65535u)), static_cast<int16_t>(val)});
            }
            return;
        }
        pts.push_back({m, static_cast<int16_t>(val)});
    };

    // Значение точки - затухание в децибелах по 1.5 дБ на ступень
    // (kQuirkEnvelopeDecibel): 64 - единица, 0 - -96 дБ, то есть тишина по
    // мерке SF2. Шкала совпадает с SF2 точно: 96/1.5 = 64.
    //
    // Раньше значение было амплитудой 0..64, и спад приходилось рисовать
    // ступенями по 6 дБ (32, 16, 8, 4, 2, 1), ниже -36 дБ огибающая ничего не
    // выражала. Теперь спад - прямая из двух точек и совпадает с SF2 точно
    // (релиз из огибающей убран, см. ниже).
    auto db_to_val = [](double db) {
        const int32_t v = static_cast<int32_t>(std::lround(64.0 - db / 1.5));
        return v < 0 ? 0 : (v > 64 ? 64 : v);
    };
    const double sustain_db = (sustain_cb < 960 ? double(sustain_cb) : 960.0) / 10.0;

    uint32_t t = 0;
    push(0, 0);
    if (delay_ms) {
        t += delay_ms;
        push(t, 0);
    }

    // Атака в SF2 линейна по амплитуде, а шкала точки в децибелах, поэтому
    // прямой здесь она не выходит: половина времени - это -6 дБ, четверть -
    // -12. Четыре точки покрывают её с ошибкой меньше децибела.
    if (attack_ms) {
        push(t + attack_ms / 8, db_to_val(18.0));
        push(t + attack_ms / 4, db_to_val(12.0));
        push(t + attack_ms / 2, db_to_val(6.0));
        t += attack_ms;
        push(t, 64);
    } else {
        push(t, 64);
    }
    if (hold_ms) {
        t += hold_ms;
        push(t, 64);
    }

    // Спад - прямая в децибелах от нуля до сустейна.
    if (decay_ms && sustain_db > 0.0) {
        t += decay_ms;
        push(t, db_to_val(sustain_db));
    } else if (sustain_db > 0.0) {
        push(t, db_to_val(sustain_db));
    }

    const uint8_t sustain_idx = static_cast<uint8_t>(pts.size() - 1);

    // Релиза в огибающей нет намеренно: под MIDI движок замораживает
    // огибающую на снятии ноты, а спуск ведёт затухание (см.
    // effect_dispatch.cpp). Причина: в SF2 релиз заменяет остаток спада, а
    // трекерная огибающая умеет только продолжать его - рояль, снятый
    // посреди своего спада, доигрывал спад целиком.
    //
    // Последняя точка всё равно нужна: без неё огибающая кончалась бы на
    // сустейне, а у движка это признак конца.
    push(t + 1, pts.empty() ? 0 : pts.back().value);

    out.env.point_count = static_cast<uint8_t>(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        out.env.points[i] = pts[i];
    out.env.flags         = kEnvEnabledBit | kEnvSustainBit;
    out.env.sustain_point = sustain_idx;
    out.env.sustain_end   = sustain_idx;
    out.env.loop_start    = 0;
    out.env.loop_end      = 0;
    out.used              = true;

    // Текущая схема: затухание (fadeout) и есть релиз SF2 - от текущего
    // уровня вниз со скоростью 96 дБ за release_full_ms. Затухание движка
    // экспоненциальное, по децибелам (kQuirkFadeoutExponential, ставится
    // загрузчиком MIDI), за fade_ms оно проходит путь до порога остановки
    // голоса -48 дБ, поэтому время берётся вдвое меньше полного.
    //
    // Берётся полное время релиза, а не остаток от сустейна: релиз в SF2 -
    // это скорость (96 дБ за release_full_ms), а не длительность. Сколько он
    // продлится, зависит от уровня, на котором сняли ноту, а при выпечке это
    // неизвестно.
    //
    // Прежние варианты и почему от них отказались (в порядке, в каком они
    // здесь сменялись):
    //
    // - Fadeout был нужен как предохранитель: голос движка останавливался не
    //   по нулю огибающей, а по нулю fadeout_level, и зацикленный сэмпл с
    //   fadeout_rate==0 звенел бы вечно на нулевой громкости, занимая канал.
    //   Поэтому fadeout ставился примерно во время релиза.
    // - Найденный отказ: у Overdrive Guitar сустейн стоит на 960 сантибел
    //   (тишина), а спад до него длится 22 секунды. Формула "от сустейна до
    //   тишины" давала release_ms = 0, и затухание выставлялось в 20 мс, хотя
    //   в .sf2 написано 0.343 с. Ноты обрывались почти сразу, в DoomE1M1
    //   между ними были дыры по 85 мс: на слух заикание, на осциллограмме
    //   разрыв там, где у эталона сплошной звук.
    // - Полное время при линейном затухании тоже не годится: затухание было
    //   линейным по амплитуде, а релиз SF2 линеен в децибелах. Линейный спад
    //   за 343 мс держит звук на -6 дБ ещё половину времени, а экспонента к
    //   этому моменту уже на -48. Ноты наслаивались бы - на слух гулкий зал.
    // - Бралась доля 30/96 полного времени (релиз проходит слышимые 30 дБ,
    //   дальше неразличим на фоне музыки); если у огибающей свой релиз,
    //   затухание не короче него. Замер одиночной ноты против эталонного
    //   SF2-синтезатора показал две противоположные ошибки: перкуссию
    //   обрывало раньше эталона (треугольник уходил в цифровой ноль на
    //   1.05 с, где эталон идёт -31 дБ/с ещё две секунды), а сустейновые
    //   инструменты линейное затухание держало дольше (струнные звучали на
    //   -8.7 дБ там, где эталон уже молчал). Кроме того, затухание
    //   складывалось со спадом огибающей и удваивало скорость: у треугольника
    //   55 дБ/с против 31 у эталона.
    // - С переходом на экспоненциальное затухание (порог -48 дБ) время
    //   бралось за прохождение релизом тех же 48 дБ, чтобы скорость спада
    //   совпадала с эталонной.
    // - По концу огибающей равняться нельзя: её точки отсчитываются от взятия
    //   ноты и включают спад (у рояля бывает двадцатисекундный), а затухание
    //   начинается со снятия. Проверено - рояль переставал затухать вовсе.
    // - Одно время хвост формировала огибающая: движок под MIDI останавливал
    //   голос по нулю самой огибающей (см. effect_dispatch.cpp), а затухание
    //   было предохранителем для зацикленных сэмплов, у которых огибающая
    //   нуля не достигает. Сначала бралось полное время релиза, затем втрое
    //   больше: замер показал, что при затухании в длину релиза оно всё ещё
    //   складывалось со спадом огибающей и добавляло около 7 дБ/с (у
    //   треугольника 40 дБ/с против 31 у эталона).
    uint32_t fade_ms = release_full_ms / 2;
    if (fade_ms < 10u) fade_ms = 10u;
    if (fade_ms > 60000u) fade_ms = 60000u;
    out.fadeout_ms = static_cast<uint16_t>(std::min<uint32_t>(fade_ms ? fade_ms : 20u, 65535u));
    return out;
}

// --- Профиль инструмента: что должно совпадать у зон одного keymap ---
//
// У трекерного инструмента одна огибающая на все ноты, один фильтр, одна
// панорама. Поэтому зоны, различающиеся хоть чем-то из этого, должны
// разойтись по разным инструментам, даже если по нотам не пересекаются.
// Затухания и панорамы в профиле нет намеренно: обе величины лежат в
// записи сэмпла (BankSample::global_volume/default_panning), а в профиле
// только дробили бы инструменты. На Bohemian Rhapsody это давало 234
// инструмента и 25 КБ огибающих при арене в 48 КБ.
struct Profile {
    int32_t env_delay, env_attack, env_hold, env_decay, env_sustain, env_release;
    int32_t filter_fc, filter_q, exclusive, vel_lo, vel_hi;
    int32_t vel_to_fc;
    // Огибающая модуляции на срез (build_filter_envelope): её глубина
    // modEnvToFilterFc, статический initialFilterFc и шесть параметров. У
    // инструментов без неё всё нулевое - иначе дробились бы инструменты.
    int32_t menv_to_fc, menv_fc0;
    int32_t menv_delay, menv_attack, menv_hold, menv_decay, menv_sustain, menv_release;
    bool operator<(const Profile& o) const { return std::memcmp(this, &o, sizeof(Profile)) < 0; }
};

// Смещение ноты, выражающее scaleTuning: нота n должна звучать как
// root + (n-root)*scale/100, движок же играет queried_note + note_offset.
inline int scale_offset(int note, int root, int32_t scale) {
    const int d   = note - root;
    const int off = static_cast<int>(std::lround(d * (scale - 100) / 100.0));
    return off < -128 ? -128 : (off > 127 ? 127 : off);
}

// Значение источника модулятора SF2 от силы удара, как у spessasynth_core
// (modulator_curves.ts): x = v/128, при направлении max->min 1 - x; кривая
// 0 линейная, 1 вогнутая, 2 выпуклая, 3 ступенька; биполярный источник
// отображается на -1..1.
double vel_mod_source(uint16_t src, int velocity) {
    double x = velocity / 128.0;
    if (src & 0x0100) x = 1.0 - x;
    const bool bipolar = (src & 0x0200) != 0;
    const int curve    = src >> 10;
    auto shape         = [curve](double t) {
        if (t <= 0.0) return 0.0;
        if (t >= 1.0) return 1.0;
        switch (curve) {
            case 1:
                return std::min(1.0, -400.0 / 960.0 * std::log10(1.0 - t));
            case 2:
                return std::max(0.0, 1.0 + 400.0 / 960.0 * std::log10(t));
            case 3:
                return t > 0.5 ? 1.0 : 0.0;
            default:
                return t;
        }
    };
    if (!bipolar) return shape(x);
    if (curve == 0 || curve == 3) return 2.0 * shape(x) - 1.0;
    const double y = 2.0 * x - 1.0;
    return y < 0.0 ? -shape(-y) : shape(y);
}

int32_t vel_mod_cents(const std::vector<VelMod>& mods, int velocity) {
    double total = 0.0;
    for (const VelMod& m : mods)
        total += m.amount * vel_mod_source(m.src, velocity);
    return static_cast<int32_t>(std::lround(total));
}

// Сила удара, при которой печётся глубина огибающей фильтра слоя: среднее
// точек выбора слоя (kVelBands - квантили velocity нот библиотеки, у каждой
// равная доля нот), попавших в диапазон слоя. Без таких точек слой выбирается
// по точной velocity - берётся середина диапазона.
int layer_typical_velocity(int vel_lo, int vel_hi) {
    int sum = 0, n = 0;
    for (uint8_t b : kVelBands) {
        if (b >= vel_lo && b <= vel_hi) {
            sum += b;
            ++n;
        }
    }
    return n ? (sum + n / 2) / n : (vel_lo + vel_hi + 1) / 2;
}

Profile profile_of(const GenSet& g, int32_t vel_to_fc_cents = 0, int32_t vel_to_menv_cents = 0) {
    Profile p{};
    p.env_delay   = g.get(GEN_DELAY_VOL_ENV, -12000);
    p.env_attack  = g.get(GEN_ATTACK_VOL_ENV, -12000);
    p.env_hold    = g.get(GEN_HOLD_VOL_ENV, -12000);
    p.env_decay   = g.get(GEN_DECAY_VOL_ENV, -12000);
    p.env_sustain = g.get(GEN_SUSTAIN_VOL_ENV, 0);
    p.env_release = g.get(GEN_RELEASE_VOL_ENV, -12000);
    // Срез берётся там, куда его открывает огибающая модуляции, а не там,
    // где он стоит статически.
    //
    // modEnvToFilterFc в банке не редкость, медиана у него +2200 центов:
    // автор ставит низкий статический срез и поднимает его огибающей на
    // атаке. Огибающую модуляции мы не переносим, и один статический срез
    // глушит инструмент: у Grand Piano из GeneralUser он 798 Гц, и рояль в
    // Unchain.mid выходил на 39 дБ темнее эталонного выше 8 кГц.
    //
    // Пик огибающей - приближение в правильную сторону: атака звучит верно,
    // а то, что огибающая потом прикрывает, теряется. Это гораздо меньшая
    // ошибка, чем оба крайних варианта: замер по набору из 31 файла даёт
    // 35.2 дБ при статическом срезе и 30.1 при выключенном фильтре.
    //
    // Знак огибающей учитывать обязательно. modEnvToFilterFc бывает
    // отрицательным, и тогда огибающая срез не открывает, а закрывает - так
    // инструмент темнеет по мере затухания ноты. Если складывать его вслепую,
    // берётся самая тёмная точка огибающей и применяется с самого начала,
    // то есть наоборот.
    //
    // Случай: у SGM рояль (STEINWAY PIANO p/f) не задаёт initialFilterFc
    // (умолчание 13500 центов, фильтр открыт), а modEnvToFilterFc у него
    // -2000 и -2700. Сумма давала срез около 1.5 кГц на весь диапазон, и
    // рояль звучал глухо; на слух вариант "фильтр выключен совсем" уверенно
    // выигрывал.
    //
    // Пик огибающей при отрицательном ходе - её начало, то есть сам
    // initialFilterFc. Поэтому прибавляется только положительная часть.
    //
    // Статический срез - по-прежнему пик: он же базовый срез огибающей фильтра
    // (build_filter_envelope), которая дальше только опускает его по центам.
    //
    // Глубина огибающей - генератор плюс модуляторы от силы удара при типичной
    // velocity слоя (vel_to_menv_cents). У валторны GeneralUser генератор даёт
    // 400 центов, модулятор при velocity 100 ещё около 5000: срез 250 Гц на
    // атаке уходит к 6 кГц. Без модулятора фильтр держался у 300-500 Гц, и
    // гармоники с пятой по двенадцатую выходили на 15-20 дБ ниже эталона.
    const int32_t mod_env_fc = g.get(GEN_MOD_ENV_TO_FILTER_FC, 0) + vel_to_menv_cents;
    p.filter_fc              = g.get(GEN_INITIAL_FILTER_FC, 13500) + (mod_env_fc > 0 ? mod_env_fc : 0);
    if (mod_env_fc != 0) {
        p.menv_to_fc   = mod_env_fc;
        p.menv_fc0     = g.get(GEN_INITIAL_FILTER_FC, 13500);
        p.menv_delay   = g.get(GEN_DELAY_MOD_ENV, -12000);
        p.menv_attack  = g.get(GEN_ATTACK_MOD_ENV, -12000);
        p.menv_hold    = g.get(GEN_HOLD_MOD_ENV, -12000);
        p.menv_decay   = g.get(GEN_DECAY_MOD_ENV, -12000);
        p.menv_sustain = g.get(GEN_SUSTAIN_MOD_ENV, 0);
        p.menv_release = g.get(GEN_RELEASE_MOD_ENV, -12000);
    }
    p.filter_q       = g.get(GEN_INITIAL_FILTER_Q, 0);
    p.vel_to_fc      = vel_to_fc_cents;
    p.exclusive      = g.get(GEN_EXCLUSIVE_CLASS, 0);
    const int32_t vr = g.get(GEN_VEL_RANGE, 0x7F00);
    p.vel_lo         = vr & 0xFF;
    p.vel_hi         = (vr >> 8) & 0xFF;
    if (!g.has(GEN_VEL_RANGE)) {
        p.vel_lo = 0;
        p.vel_hi = 127;
    }
    return p;
}

// --- Огибающая модуляции на срез -> огибающая фильтра трекера ---
//
// Без неё срез стоял статически в пике огибающей, и то, что огибающая
// потом прикрывает, пропадало. У SGM Halo Pad срез 580 Гц с резонансом
// 11.7 дБ, огибающая открывает его на +4612 центов (8.3 кГц) за 8 мс и за
// 1.4 с опускает до 1.3 кГц: пэд вздыхает и темнеет. У нас он играл весь
// открытым, выше 6 кГц на 4..17 дБ ярче эталона, на слух - не тот тембр.
//
// Шкала движка (resonant_filter.cpp): f = 110 * 2^(0.25 + co*m/(U*512)), где
// U - делений на октаву (kCutoffUnitsPerOctave), co - срез инструмента
// 0..127, m = значение огибающей * 8 (0..512). При
// фиксированном co показатель линеен по m, то есть огибающая линейна в
// центах - как modEnvToFilterFc в SF2. Поэтому co - пик хода (он же
// Profile::filter_fc), а точки - доли пика в показателе.
//
// Огибающая модуляции SF2 линейна по значению 0..1: атака 0 -> 1, удержание,
// спад 1 -> сустейн (1 - sustainModEnv/1000) за долю полного времени спада,
// релиз от сустейна до 0 за долю полного времени релиза - та же схема
// "время полного хода", что у огибающей громкости. Релиз - после точки
// сустейна: огибающая фильтра у движка на снятии ноты не замораживается.
// Всё, что выше потолка шкалы, выражается как "открыт":
// точка упирается в 64.
EnvOut build_filter_envelope(const Profile& p) {
    EnvOut out{};
    if (p.menv_to_fc == 0) return out;
    const int32_t peak_c = p.menv_fc0 + (p.menv_to_fc > 0 ? p.menv_to_fc : 0);
    const uint8_t co     = filter_cents_to_it(peak_c);
    if (co == 0) return out; // весь ход ниже шкалы - фильтр глухой и так
    const int32_t low_c = p.menv_fc0 + (p.menv_to_fc < 0 ? p.menv_to_fc : 0);
    if (filter_cents_to_it(low_c) >= 127) return out; // весь ход выше шкалы - открыт всегда
    // Значение точки для доли огибающей e (0..1).
    auto raw_of = [&](double e) -> int16_t {
        const double cents = p.menv_fc0 + p.menv_to_fc * e;
        const double hz    = 8.176 * std::pow(2.0, cents / 1200.0);
        const double expo  = std::log2(hz / 110.0) - 0.25;              // co*m/(U*512)
        const double m     = expo * kCutoffUnitsPerOctave * 512.0 / co; // 0..512
        const long v       = std::lround(m / 8.0);
        return static_cast<int16_t>(v < 0 ? 0 : (v > 64 ? 64 : v));
    };

    const uint32_t delay_ms   = timecents_to_ms(p.menv_delay);
    const uint32_t attack_ms  = timecents_to_ms(p.menv_attack);
    const uint32_t hold_ms    = timecents_to_ms(p.menv_hold);
    const double sus_e        = 1.0 - std::max(0, std::min(1000, p.menv_sustain)) / 1000.0;
    const uint32_t decay_ms   = static_cast<uint32_t>(timecents_to_ms(p.menv_decay) * (1.0 - sus_e));
    const uint32_t release_ms = static_cast<uint32_t>(timecents_to_ms(p.menv_release) * sus_e);

    std::vector<BankEnvelopePoint> pts;
    auto push = [&](uint32_t ms, int16_t val) {
        if (pts.size() >= kMaxEnvelopePoints) return;
        uint16_t m = static_cast<uint16_t>(std::min<uint32_t>(ms, 65535));
        if (!pts.empty() && m <= pts.back().ms) {
            if (val == pts.back().value) return;
            m = static_cast<uint16_t>(std::min<uint32_t>(pts.back().ms + 1u, 65535u));
        }
        pts.push_back({m, val});
    };
    // Прямой в e кусок от ea до eb за [ta, tb]. Если по дороге значение
    // упирается в 0 или 64, точка ставится в месте упора: иначе ломаная
    // между обрезанными концами растянула бы ход на весь кусок.
    auto segment = [&](uint32_t ta, double ea, uint32_t tb, double eb) {
        const double ca = p.menv_fc0 + p.menv_to_fc * ea, cb = p.menv_fc0 + p.menv_to_fc * eb;
        std::vector<std::pair<double, int16_t>> cross; // (доля пути, значение упора)
        for (int lim : {0, 64}) {
            const double hz_lim = 110.0 * std::pow(2.0, 0.25 + co * lim * 8.0 / (kCutoffUnitsPerOctave * 512.0));
            const double c_lim  = 1200.0 * std::log2(hz_lim / 8.176);
            if ((ca - c_lim) * (cb - c_lim) < 0.0) cross.push_back({(c_lim - ca) / (cb - ca), static_cast<int16_t>(lim)});
        }
        std::sort(cross.begin(), cross.end());
        for (const auto& c : cross)
            push(ta + static_cast<uint32_t>((tb - ta) * c.first), c.second);
        push(tb, raw_of(eb));
    };

    uint32_t t = 0;
    push(0, raw_of(0.0));
    if (delay_ms) {
        t += delay_ms;
        push(t, raw_of(0.0));
    }
    segment(t, 0.0, t + attack_ms, 1.0);
    t += attack_ms;
    if (hold_ms) {
        t += hold_ms;
        push(t, raw_of(1.0));
    }
    segment(t, 1.0, t + decay_ms, sus_e);
    t                         += decay_ms;
    const uint8_t sustain_idx  = static_cast<uint8_t>(pts.size() - 1);
    segment(t, sus_e, t + std::max<uint32_t>(release_ms, 1u), 0.0);

    // Огибающая, у которой все точки одинаковы, ничего не двигает.
    bool moves = false;
    for (const auto& q : pts)
        if (q.value != pts[0].value) moves = true;
    if (!moves) return out;

    out.env.point_count = static_cast<uint8_t>(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        out.env.points[i] = pts[i];
    out.env.flags         = kEnvEnabledBit | kEnvSustainBit;
    out.env.sustain_point = sustain_idx;
    out.env.sustain_end   = sustain_idx;
    out.env.loop_start    = 0;
    out.env.loop_end      = 0;
    out.used              = true;
    return out;
}

} // namespace

// --- Точка входа ---
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "sf2bake <bank.sf2> <out.ssb> [--rate-cap 32000] [--report file]\n");
        return 2;
    }
    const std::string in_path = argv[1], out_path = argv[2];
    double q_comp_db_per_cb = 0.1;   // сантибел = 0.1 дБ, как в спецификации и в spessasynth; 0 - выключить
    double rate_keep_db     = -40.0; // энергия выше потолка, при которой его не применять
    double atten_db_per_cb  = 0.04;  // шкала initialAttenuation: 0.04 (fluidsynth) или 0.1 (спека, spessasynth)
    uint32_t rate_cap       = 32000;
    // Нижний потолок для сэмплов, которые ничего не теряют. 0 выключает
    // посэмпльный выбор и возвращает прежнее поведение.
    uint32_t rate_cap_low = 22050;
    double rate_loss_db   = -30.0;
    // Непрерывная частота вместо ступеней: каждому сэмплу назначается та,
    // что нужна его верхней границе спектра (spectral_top_hz), с запасом на
    // линейную интерполяцию воспроизведения. Интерполяция сама роняет около
    // 4 дБ у частоты Найквиста и заворачивает зеркала, поэтому резать надо
    // не до половины частоты, а примерно до 45% - отсюда множитель.
    bool rate_auto          = false;
    uint32_t rate_auto_min  = 11025; // ниже не опускаем ни при каком спектре
    double rate_auto_margin = 2.2;   // частота = граница спектра x этого
    double rate_tilt_db     = 0.0;   // наклон спектра перед порогом, дБ/октаву
    // Прореживание зон keymap. По умолчанию выключено: банку, который
    // влезает во флеш и чьи треки влезают в память, оно только вредит.
    // Нужно большим банкам под карту, где память кончается раньше звука.
    int merge_zones = 1; // оставлять каждую N-ю зону, 1 = не трогать
    // Насколько далеко нота может уехать от своей записи вниз. Октава -
    // предел, за которым тембр уже не тот же инструмент.
    int merge_max_stretch = 12;
    // Подъём верха у сэмплов, которые после прореживания играют ниже своей
    // записи. Растяжение вниз сдвигает весь спектр вниз, и верх уходит:
    // замер на гитаре Doom дал -1.5 дБ в полосе 4 кГц и -2.6 в 8 кГц при
    // среднем растяжении 6 полутонов. Полка компенсирует это в самом сэмпле,
    // другого места нет: срез в движке низкочастотный, поднять им нельзя, и
    // он один на инструмент, а не на зону.
    //
    // Ударным и всему с фиксированной высотой полка не ставится: их зоны не
    // сливаются, компенсировать нечего.
    double merge_bright_db = 0.0;
    // Выравнивать ли уровень поглощённой зоны. По умолчанию да: без этого
    // слияние приносит на чужие ноты чужую громкость (замер ниже, у самого
    // слияния). Ключ нужен только для перемера.
    bool merge_level = true;
    // Не выбрасывать дубли слоёв. Ключ для замеров: у некоторых банков
    // одинаковые зоны стоят намеренно, ради утолщения звука.
    bool keep_dup_layers = false;
    // Общий уровень банка в децибелах. Банки расходятся по громкости на
    // десяток децибел: Timbres выдаёт -8.3 дБ RMS там, где SGM даёт -16.6,
    // и на одном и том же файле первый клипует (до 0.197% отсчётов на
    // полке), а второй нет. Это не динамика, компрессор тут не нужен - банк
    // просто громче, и приводить его надо здесь, множителем громкости
    // записей, а не на выходе.
    double bank_gain_db = 0.0;
    // Отдельный уровень для наборов ударных (банк 128). Баланс "ударные
    // против мелодии" - свойство банка, у некоторых авторов ударные забивают
    // всё остальное. Задаётся абсолютом, а не добавкой к --bank-gain-db: так
    // проще сказать "инструменты на -6, ударные на -12". Не задан - ударные
    // идут вместе со всеми.
    double drum_gain_db = 0.0;
    bool drum_gain_set  = false;
    // Выравнивание уровня соседних зон мультисэмпла. Ухо ловит не общий
    // уровень инструмента, а перепад между соседними нотами: у SGM
    // дисторшн-гитара скачет на 3.6 дБ на границе F-2/F#2, и проход через
    // эту границу слышен как провал громкости. У GeneralUser тот же
    // инструмент ровный в пределах 0.7 дБ.
    //
    // Медленный ход уровня по клавиатуре не трогаем - он у инструмента
    // естественный. Ограничивается только шаг между соседями.
    // 0 = выключено.
    double even_zones_db = 0.0;
    // Укорочение затухания у названных программ: "35=1.2,48=2".
    //
    // Нужно там, где инструмент банка тянется, а соседние по аранжировке
    // умолкают: в DreamOn.mid на 72-й секунде смолкает дисторшн-гитара, и
    // зацикленный бас SGM выходит вперёд на 9.7 дБ - режет ухо. У GeneralUser
    // тот же бас не зациклен и просто затухает, поэтому там ничего не
    // вылезает. Это свойство банка: эталонный синтезатор и другие плееры
    // играют его так же.
    //
    // Правится огибающей, а не громкостью: во время гитары бас SGM даже
    // тише, чем у GeneralUser (-12.3 против -7.8 дБ относительно самого
    // громкого канала), то есть уровень ни при чём.
    std::map<int, double> decay_override;
    // Множитель длины релиза по программам: "30=2,48=1.5".
    //
    // Длину хвоста задаёт банк (releaseVolEnv зоны), и банки расходятся
    // вдвое: у Timbres спад на 40 дБ идёт 260 мс, у Arachno 100. Там, где в
    // аранжировке после аккорда остаётся одна нота, короткий хвост даёт
    // слышимую дыру, длинный её затягивает. Это правка на слух, отход от
    // банка, поэтому по умолчанию единица и включать надо явно.
    std::map<int, double> release_scale;
    // Сведение стереопар в моно. Голос у нас моно, поэтому пара "левый +
    // правый" стоит двух голосов и двух прогонов в памяти, а звучит как один
    // источник. Пары ищутся по употреблению, а не по sampleLink: в реальных
    // банках эта ссылка врёт (у Arachno из 180 половин взаимная только одна,
    // левый рояль ссылается на тромбон), и флаг sampleType врёт тоже.
    bool fuse_stereo = false;
    // Ниже этой корреляции половин складывать нельзя - сложение
    // расфазированного вычитает середину. Тогда берётся громкая половина.
    double fuse_min_corr = 0.2;
    bool gm_only         = false; // только банк 0 и ударные: расширения GS/XG отбрасываются
    // Печь всё Raw8, даже там, где Dpcm8 точнее. Ключ для замеров: им
    // отделяется вклад дельта-кодека от всего остального. Raw8 не имеет
    // состояния, поэтому стык петли у него заведомо чист.
    bool force_raw8 = false;
    // Прогоны PCM без сжатия (--codec none): подкачка - копирование, таблица
    // распаковки не нужна; банк больше.
    bool pack_pcm = true;
    // Длина сшивки петли в отсчётах, 0 - не сшивать.
    int loop_xfade = 0;
    // Сколько отсчётов после конца петли подменить началом петли.
    int loop_seam = 0;
    std::string report_path;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--atten-db-per-cb") == 0 && i + 1 < argc)
            atten_db_per_cb = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--rate-keep-db") == 0 && i + 1 < argc)
            rate_keep_db = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--q-comp") == 0 && i + 1 < argc)
            q_comp_db_per_cb = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--force-raw8") == 0)
            force_raw8 = true;
        else if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            const char* c = argv[++i];
            if (std::strcmp(c, "tans") == 0)
                pack_pcm = true;
            else if (std::strcmp(c, "none") == 0)
                pack_pcm = false;
            else {
                std::fprintf(stderr, "--codec: tans or none\n");
                return 2;
            }
        } else if (std::strcmp(argv[i], "--loop-xfade") == 0 && i + 1 < argc)
            loop_xfade = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--loop-seam") == 0 && i + 1 < argc)
            loop_seam = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--rate-cap") == 0 && i + 1 < argc)
            rate_cap = std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--rate-cap-low") == 0 && i + 1 < argc)
            rate_cap_low = std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--rate-loss-db") == 0 && i + 1 < argc)
            rate_loss_db = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--rate-auto") == 0)
            rate_auto = true;
        else if (std::strcmp(argv[i], "--rate-auto-min") == 0 && i + 1 < argc)
            rate_auto_min = std::strtoul(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--rate-auto-margin") == 0 && i + 1 < argc)
            rate_auto_margin = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--rate-tilt-db") == 0 && i + 1 < argc)
            rate_tilt_db = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--merge-zones") == 0 && i + 1 < argc)
            merge_zones = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--merge-max-stretch") == 0 && i + 1 < argc)
            merge_max_stretch = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--merge-bright-db") == 0 && i + 1 < argc)
            merge_bright_db = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--no-merge-level") == 0)
            merge_level = false;
        else if (std::strcmp(argv[i], "--keep-dup-layers") == 0)
            keep_dup_layers = true;
        else if (std::strcmp(argv[i], "--bank-gain-db") == 0 && i + 1 < argc)
            bank_gain_db = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--drum-gain-db") == 0 && i + 1 < argc) {
            drum_gain_db  = std::strtod(argv[++i], nullptr);
            drum_gain_set = true;
        } else if (std::strcmp(argv[i], "--even-zones-db") == 0 && i + 1 < argc)
            even_zones_db = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--release") == 0 && i + 1 < argc) {
            // "30=2" - одна программа; "2" без знака равенства - все.
            //
            // Все сразу нужны потому, что пустоту в аранжировке заполняют хвосты
            // всех звучащих каналов разом, а не того инструмента, у которого пауза.
            // Растянув одну программу, разницы не услышишь (замерено на DreamOn:
            // дыра 5.8 против 5.7 дБ).
            const char* p2 = argv[++i];
            if (!std::strchr(p2, '=')) {
                const double k = std::strtod(p2, nullptr);
                if (k > 0.0)
                    for (int pr = 0; pr < 128; ++pr)
                        release_scale[pr] = k;
                continue;
            }
            while (*p2) {
                char* end       = nullptr;
                const long prog = std::strtol(p2, &end, 10);
                if (end == p2 || *end != '=') break;
                const double k = std::strtod(end + 1, &end);
                if (prog >= 0 && prog < 128 && k > 0.0) release_scale[static_cast<int>(prog)] = k;
                if (*end != ',') break;
                p2 = end + 1;
            }
        } else if (std::strcmp(argv[i], "--decay") == 0 && i + 1 < argc) {
            // "35=1.2,48=2" - программа и время спада до тишины в секундах.
            const char* p2 = argv[++i];
            while (*p2) {
                char* end       = nullptr;
                const long prog = std::strtol(p2, &end, 10);
                if (end == p2 || *end != '=') break;
                const double sec = std::strtod(end + 1, &end);
                if (prog >= 0 && prog < 128 && sec > 0.0) decay_override[static_cast<int>(prog)] = sec;
                if (*end != ',') break;
                p2 = end + 1;
            }
        } else if (std::strcmp(argv[i], "--fuse-stereo") == 0)
            fuse_stereo = true;
        else if (std::strcmp(argv[i], "--fuse-min-corr") == 0 && i + 1 < argc)
            fuse_min_corr = std::strtod(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--gm-only") == 0)
            gm_only = true;
        else if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            report_path = argv[++i];
    }

    Sf2 sf2;
    std::string err;
    if (!parse_sf2(in_path, sf2, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("bank: %s\n", sf2.name.c_str());
    std::printf("  presets %zu, instruments %zu, samples %zu, PCM %.1f MB\n", sf2.presets.size(), sf2.insts.size(), sf2.samples.size(),
                sf2.pcm.size() * 2.0 / 1048576.0);
    std::printf("  rate ceiling: %u Hz, %u Hz for the \"quiet on top\" ones when the loss is below %.0f dB\n", rate_cap, rate_cap_low, rate_loss_db);

    // --- Выходные таблицы ---
    std::vector<BankPreset> presets(kPresetSlots);
    for (auto& p : presets) {
        p.first_layer           = 0;
        p.layer_count_and_flags = 0;
    }
    std::vector<BankLayer> layers;
    uint32_t stat_layer_dup = 0;
    std::vector<BankInstrument> instruments;
    std::vector<BankEnvelope> envelopes;
    std::vector<BankKeymapRange> keymap;
    std::vector<BankSample> samples;
    std::vector<uint8_t> pcm;

    // Прогон PCM переиспользуется, если совпали сэмпл, обрезка и частота:
    // один и тот же звук у разных зон не должен лежать в банке дважды.
    struct PcmKey {
        int sid;
        uint32_t s, e, rate;
        int bright_q; // яркость в десятых долях дБ
        int fuse;     // партнёр по стереопаре или -1
        bool operator<(const PcmKey& o) const { return std::memcmp(this, &o, sizeof(PcmKey)) < 0; }
    };
    // ratio нужен, чтобы пересчитать точки петли зоны в координаты
    // испечённого прогона: сам прогон о петле ничего не знает.
    // Прогон копится сырым: модель сжатия строится по всему банку, то есть
    // только после того, как испечён последний прогон.
    // rate - частота, на которой прогон испечён. Заново из потолка не
    // выводится: потолок посэмпльный (см. bake_pcm), и повторный вывод дал
    // бы 32000 там, где прогон испечён на 22050. Такой промах и был:
    // c5_speed считался по невыбранному потолку, и опущенные сэмплы играли
    // на 6.5 полутона выше (найдено на слух: трубы в Bond.mid с 10-й
    // секунды).
    struct PcmRun {
        uint32_t raw_index, bytes, length, checkpoints, start, rate;
        double ratio;
        bool raw8;
        double rms;
    }; // rms исходных отсчётов - для выравнивания зон
    std::vector<std::vector<uint8_t>> raw_runs;
    std::map<PcmKey, PcmRun> pcm_runs;
    std::map<uint64_t, PcmRun> pcm_by_hash; // тот же звук из разных зон - один прогон

    // Записи сэмплов различаются ещё и частотой c5_speed (root и
    // подстройка), но PCM при этом делят.
    struct SmpKey {
        uint32_t run_offset, c5, loop_flag;
        int32_t pan;
        bool operator<(const SmpKey& o) const { return std::memcmp(this, &o, sizeof(SmpKey)) < 0; }
    };
    std::map<SmpKey, uint16_t> sample_index;
    std::vector<uint32_t> sample_raw_index; // сэмпл -> индекс сырого прогона, для сквозной проверки
    // Слышимый уровень записи: rms прогона, умноженный на её громкость.
    // Нужен для выравнивания соседних зон мультисэмпла - см. --even-zones-db.
    std::vector<double> sample_level;
    std::map<std::string, uint16_t> inst_by_sig;
    // Одинаковые прогоны keymap у разных инструментов пишутся один раз:
    // читатели берут только [keymap_first, keymap_first + keymap_count).
    std::map<std::string, uint16_t> keymap_by_run;
    std::map<std::string, uint16_t> env_by_sig; // одинаковые инструменты разных пресетов - одна запись

    // Запись хозяина, приведённая к уровню поглощённой зоны.
    //
    // Прогон PCM тот же (он и есть дорогая часть), меняется только громкость
    // записи - 36 байт на пару (хозяин, нужный уровень). Одинаковые пары
    // переиспользуются, поэтому записей добавляется столько, сколько разных
    // уровней понадобилось.
    //
    // Поправка ограничена шестью децибелами в обе стороны: если зоны
    // расходятся сильнее, дело не в сведении, а в том, что это разные звуки,
    // и тянуть их друг к другу нельзя.
    uint32_t stat_level_fixed = 0;
    double stat_level_db_sum = 0.0, stat_level_db_max = 0.0;
    std::map<std::pair<int, int>, uint16_t> level_cache;
    auto level_matched = [&](int host, int want) -> int {
        if (!merge_level || host < 0 || want < 0 || host == want) return host;
        const double have = sample_level[host], target = sample_level[want];
        if (!(have > 0.0) || !(target > 0.0)) return host;
        double ratio = target / have;
        if (ratio > 2.0)
            ratio = 2.0;
        else if (ratio < 0.5)
            ratio = 0.5;
        const double db = 20.0 * std::log10(ratio);
        if (std::fabs(db) < 0.3) return host; // не слышно, не плодим запись
        int gv = static_cast<int>(std::lround(samples[host].global_volume * ratio));
        if (gv < 1)
            gv = 1;
        else if (gv > 255)
            gv = 255;
        if (gv == samples[host].global_volume) return host;
        const auto key = std::make_pair(host, gv);
        auto it        = level_cache.find(key);
        if (it != level_cache.end()) return it->second;
        BankSample bs      = samples[host];
        bs.global_volume   = static_cast<uint8_t>(gv);
        const uint16_t idx = static_cast<uint16_t>(samples.size());
        samples.push_back(bs);
        sample_level.push_back(sample_level[host] * (double(gv) / double(samples[host].global_volume)));
        sample_raw_index.push_back(sample_raw_index[host]);
        level_cache[key] = idx;
        ++stat_level_fixed;
        stat_level_db_sum += std::fabs(db);
        if (std::fabs(db) > stat_level_db_max) stat_level_db_max = std::fabs(db);
        return idx;
    };

    // --- Стереопары: поиск по употреблению и решение по каждой ---
    //
    // Пара - две зоны одного пресета (возможно, из разных инструментов, то
    // есть разных слоёв) с одинаковым диапазоном клавиш, панорамой в разные
    // края и одинаковыми длиной и корневой нотой сэмпла. Так стерео и
    // записано в этих банках; sampleLink и sampleType врут и здесь
    // бесполезны.
    //
    // fuse_into[правый] = левый - правая зона при выпечке пропускается,
    // левая печётся сведённой. fuse_pan[левый] - куда поставить моно.
    std::map<int, int> fuse_into;     // поглощённый -> выживший
    std::map<int, int> fuse_sum_with; // выживший -> чей PCM подмешать
    std::map<int, int8_t> fuse_pan;   // выживший -> панорама из баланса
    // Выживший -> множитель громкости. Две жёстко разведённые половины
    // отдают в сумме мощность rl^2 + rr^2, а один голос в центре - rf^2.
    // Без поправки сведение тише: на ударных SGM это замерено как -2.3 дБ.
    std::map<int, double> fuse_gain;
    uint32_t stat_pairs = 0, stat_fused_sum = 0, stat_fused_pick = 0, stat_pairs_phase = 0;
    uint32_t stat_max_layers     = 0;
    uint32_t stat_decay_forced   = 0;
    uint32_t stat_release_scaled = 0;
    uint32_t stat_drum_gain_hits = 0, stat_gain_records = 0;
    uint32_t stat_even_fixed = 0, stat_even_new = 0, stat_even_seen = 0;
    double stat_even_maxdelta = 0.0;
    uint32_t stat_even_groups = 0, stat_even_biggest = 0;
    std::map<std::pair<uint16_t, uint8_t>, uint16_t> even_variant;
    std::printf("  layers in the heaviest preset: %u (format ceiling 32767)\n", stat_max_layers);
    if (fuse_stereo) {
        auto rms_of = [&](const Sf2Sample& x) {
            double acc          = 0.0;
            uint32_t n          = 0;
            const uint32_t step = std::max<uint32_t>(1, (x.end - x.start) / 40000);
            for (uint32_t i = x.start; i < x.end; i += step) {
                const double v  = sf2.pcm[i];
                acc            += v * v;
                ++n;
            }
            return n ? std::sqrt(acc / n) : 0.0;
        };
        auto corr_of = [&](const Sf2Sample& a, const Sf2Sample& b) {
            const uint32_t n2   = std::min(a.end - a.start, b.end - b.start);
            const uint32_t step = std::max<uint32_t>(1, n2 / 40000);
            double sa = 0, sb = 0, sab = 0, saa = 0, sbb = 0;
            uint32_t k = 0;
            for (uint32_t i = 0; i < n2; i += step) {
                const double x = sf2.pcm[a.start + i], y = sf2.pcm[b.start + i];
                sa  += x;
                sb  += y;
                sab += x * y;
                saa += x * x;
                sbb += y * y;
                ++k;
            }
            if (!k) return 0.0;
            const double cov = sab / k - (sa / k) * (sb / k);
            const double va = saa / k - (sa / k) * (sa / k), vb = sbb / k - (sb / k) * (sb / k);
            return (va > 0 && vb > 0) ? cov / std::sqrt(va * vb) : 0.0;
        };
        struct ZoneRef {
            int sid;
            int pan, lo, hi;
        };
        std::set<std::pair<int, int>> seen;
        for (const Sf2Preset& pre : sf2.presets) {
            std::vector<ZoneRef> zs;
            for (const Sf2PresetZone& pz : pre.zones) {
                if (pz.instrument < 0 || pz.instrument >= static_cast<int>(sf2.insts.size())) continue;
                for (const Sf2Zone& z : sf2.insts[pz.instrument].zones) {
                    const int sid = z.gen.get(GEN_SAMPLE_ID, -1);
                    if (sid < 0 || sid >= static_cast<int>(sf2.samples.size())) continue;
                    const int32_t kr = z.gen.get(GEN_KEY_RANGE, -1);
                    const int lo = kr >= 0 ? (kr & 255) : 0, hi = kr >= 0 ? ((kr >> 8) & 255) : 127;
                    zs.push_back({sid, z.gen.get(GEN_PAN, 0) + pz.gen.get(GEN_PAN, 0), lo, hi});
                }
            }
            for (size_t a = 0; a < zs.size(); ++a) {
                for (size_t b = a + 1; b < zs.size(); ++b) {
                    const ZoneRef& x = zs[a];
                    const ZoneRef& y = zs[b];
                    if (x.lo != y.lo || x.hi != y.hi) continue;
                    if (!((x.pan <= -300 && y.pan >= 300) || (y.pan <= -300 && x.pan >= 300))) continue;
                    const Sf2Sample& sx = sf2.samples[x.sid];
                    const Sf2Sample& sy = sf2.samples[y.sid];
                    if ((sx.end - sx.start) != (sy.end - sy.start) || sx.root != sy.root) continue;
                    const int L = (x.pan < 0) ? x.sid : y.sid, R = (x.pan < 0) ? y.sid : x.sid;
                    if (L == R) continue;
                    const auto key = std::make_pair(std::min(L, R), std::max(L, R));
                    if (seen.count(key)) continue;
                    seen.insert(key);
                    ++stat_pairs;
                    if (fuse_into.count(R) || fuse_into.count(L)) continue; // уже в другой паре
                    const double c = corr_of(sf2.samples[L], sf2.samples[R]);
                    if (c < -0.1) {
                        ++stat_pairs_phase;
                        continue;
                    } // противофаза - не трогаем
                    const double rl = rms_of(sf2.samples[L]), rr = rms_of(sf2.samples[R]);
                    // RMS того, что получится, считается здесь же, чтобы поправка была
                    // замером, а не выкладкой.
                    auto rms_mix = [&](const Sf2Sample& a, const Sf2Sample& b) {
                        const uint32_t n2   = std::min(a.end - a.start, b.end - b.start);
                        const uint32_t step = std::max<uint32_t>(1, n2 / 40000);
                        double acc          = 0.0;
                        uint32_t k          = 0;
                        for (uint32_t i = 0; i < n2; i += step) {
                            const double v  = (double(sf2.pcm[a.start + i]) + double(sf2.pcm[b.start + i])) / 2.0;
                            acc            += v * v;
                            ++k;
                        }
                        return k ? std::sqrt(acc / k) : 0.0;
                    };
                    // Панорама из баланса половин: позиция сохраняется,
                    // ширина - нет, её одним числом не выразить.
                    const double bal  = (rl + rr) > 0 ? (rr - rl) / (rr + rl) : 0.0;
                    fuse_pan[L]       = static_cast<int8_t>(std::max(0.0, std::min(64.0, 32.0 + bal * 32.0)));
                    const double want = std::sqrt(rl * rl + rr * rr);
                    if (c >= fuse_min_corr) {
                        const double rf  = rms_mix(sf2.samples[L], sf2.samples[R]);
                        fuse_into[R]     = L;
                        fuse_sum_with[L] = R;
                        if (rf > 1e-6) fuse_gain[L] = std::min(4.0, want / rf);
                        ++stat_fused_sum;
                    } else {
                        // Связи нет: складывать нечего, берём громкую
                        // половину - тембр сохраняется точно.
                        const int keep  = (rr > rl) ? R : L;
                        const double rk = (rr > rl) ? rr : rl;
                        if (keep == R) {
                            fuse_into[L] = R;
                            fuse_pan[R]  = fuse_pan[L];
                            fuse_pan.erase(L);
                        } else
                            fuse_into[R] = L;
                        if (rk > 1e-6) fuse_gain[keep] = std::min(4.0, want / rk);
                        ++stat_fused_pick;
                    }
                }
            }
        }
    }
    g_atten_db_per_cb       = atten_db_per_cb;
    uint32_t stat_resampled = 0, stat_filter_lost = 0, stat_scale_tuning = 0, stat_notes_over = 0, stat_vel_empty = 0;
    uint32_t stat_vel_fc       = 0;
    uint32_t stat_menv_vel     = 0;
    uint32_t stat_filter_env   = 0;
    uint32_t stat_rate_low     = 0;
    uint32_t stat_zones_merged = 0, stat_zones_total = 0, stat_inst_unmergeable = 0;
    double stat_stretch_sum = 0.0;
    uint32_t stat_stretch_n = 0;
    int stat_stretch_max    = 0;
    std::map<int, bool> rate_choice;          // сэмпл -> опускать ли до нижнего потолка
    std::map<int, uint32_t> rate_auto_choice; // сэмпл -> непрерывная частота
    uint32_t stat_rate_auto = 0;
    uint64_t rate_auto_sum  = 0;
    uint32_t rate_auto_n    = 0;
    std::map<int, bool> rate_keep; // сэмпл -> оставить ли ему свою частоту вопреки потолку
    uint32_t stat_rate_keep  = 0;
    uint32_t stat_raw8       = 0;
    uint32_t stat_xfade      = 0;
    uint32_t stat_seam       = 0;
    uint32_t stat_pcm_shared = 0, stat_inst_shared = 0, stat_gain_clipped = 0, stat_keymap_shared = 0;
    uint64_t stat_body = 0, stat_pad = 0, stat_cp = 0;
    std::map<int, int> runs_per_sample;

    auto bake_pcm = [&](int sid, const GenSet& g, double bright_db, int fuse_with) -> PcmRun* {
        const Sf2Sample& s   = sf2.samples[sid];
        const uint32_t start = s.start + g.get(GEN_START_OFFSET, 0) + g.get(GEN_START_COARSE, 0) * 32768;
        const uint32_t end   = s.end + g.get(GEN_END_OFFSET, 0) + g.get(GEN_END_COARSE, 0) * 32768;
        if (end <= start || end > sf2.pcm.size()) return nullptr;

        // Потолок частоты выбирается по содержимому, а не один на банк. Меряется
        // спектром (energy_above_db): доля энергии выше частоты Найквиста
        // нижнего потолка. Если она ниже rate_loss_db, берётся нижний потолок -
        // для этого сэмпла он бесплатен.
        //
        // Выигрыш по процессору, а не по памяти: распаковка Dpcm8 стоит 281 нс
        // на вызов, а вызовов столько, сколько родных отсчётов (config.h, модель
        // цены голоса). Вдвое ниже частота - вдвое меньше распаковок на тот же
        // звук.
        //
        // Замер по GeneralUser GS: у 77% сэмплов потеря ниже -30 дБ, значимый
        // верх только у 3.5% (тарелки, хэты).
        uint32_t target_rate = (rate_cap && s.rate > rate_cap) ? rate_cap : s.rate;
        // Верхний потолок тоже по содержимому, а не для всех подряд. У сэмпла с
        // настоящим верхом за потолком плоская обрезка слышна: у треугольника из
        // Standard 1 (44100 Гц, звон на 11.2 кГц) полоса 12..16 кГц падала с 102
        // до 76 дБ, то есть исчезала, и вместо звонкого "дзинь" оставался глухой
        // призвук - по замеру против эталонного стема -21 дБ в этой полосе.
        //
        // Порог по смыслу тот же, что у нижнего: если энергии выше будущей
        // частоты Найквиста меньше rate_keep_db, обрезка не слышна и потолок
        // применяется. Иначе сэмпл остаётся на своей частоте - объёмом и
        // распаковкой платим только за те сэмплы, где это слышно.
        if (rate_cap && s.rate > rate_cap && rate_keep_db > -200.0) {
            auto kit = rate_keep.find(sid);
            if (kit == rate_keep.end()) {
                const double hi = energy_above_db(sf2.pcm.data() + start, end - start, s.rate, double(rate_cap) / 2.0);
                const bool keep = hi >= rate_keep_db;
                kit             = rate_keep.emplace(sid, keep).first;
                if (keep) ++stat_rate_keep;
            }
            if (kit->second) target_rate = s.rate;
        }
        if (rate_auto) {
            // Непрерывный выбор. Идёт после решения "оставить свою частоту вопреки
            // потолку" и только понижает: у сэмпла со слышимым верхом граница
            // спектра всё равно выйдет высокой, и минимум оставит его как есть.
            //
            // Решение кэшируется по сэмплу, а не по зоне: прогоны PCM
            // переиспользуются по ключу, куда входит целевая частота. Разные частоты
            // у зон одного сэмпла сломали бы переиспользование и съели весь выигрыш.
            auto ait = rate_auto_choice.find(sid);
            if (ait == rate_auto_choice.end()) {
                const double f = spectral_top_hz(sf2.pcm.data() + start, end - start, s.rate, rate_loss_db, rate_tilt_db);
                double want    = f * rate_auto_margin;
                if (want < double(rate_auto_min)) want = double(rate_auto_min);
                // Округление до сотни герц: точнее незачем, а одинаковые
                // частоты лучше делят прогоны.
                uint32_t w = static_cast<uint32_t>(std::lround(want / 100.0) * 100);
                if (w > s.rate) w = s.rate;
                ait            = rate_auto_choice.emplace(sid, w).first;
                rate_auto_sum += w;
                ++rate_auto_n;
            }
            if (ait->second < target_rate) {
                target_rate = ait->second;
                ++stat_rate_auto;
            }
        } else if (rate_cap_low && target_rate > rate_cap_low) {

            // Решение кэшируется по сэмплу: зон у одного сэмпла бывает
            // десятки, а спектр у него один.
            auto dit = rate_choice.find(sid);
            if (dit == rate_choice.end()) {
                const double hi = energy_above_db(sf2.pcm.data() + start, end - start, s.rate, double(rate_cap_low) / 2.0);
                const bool low  = hi < rate_loss_db;
                dit             = rate_choice.emplace(sid, low).first;
                if (low) ++stat_rate_low;
            }
            if (dit->second) target_rate = rate_cap_low;
        }

        const int bright_q = static_cast<int>(std::lround(bright_db * 10.0));
        const PcmKey key{sid, start, end, target_rate, bright_q, fuse_with};
        auto it = pcm_runs.find(key);
        if (it != pcm_runs.end()) return &it->second;

        // Обрезка по концу петли здесь не делается, и это решение по замеру: она
        // экономит 0.02 МБ на весь банк, но делает прогон зависимым от
        // sampleModes зоны. Тогда звук, у которого часть зон зациклена, а часть
        // нет, печётся дважды - на этом банк раздувался на 3.7 МБ. Точки петли
        // живут в записи сэмпла, а прогон зависит только от сэмпла, обрезки и
        // частоты.
        std::vector<int16_t> body(sf2.pcm.begin() + start, sf2.pcm.begin() + end);
        // Сведение стереопары: половины складываются пополам, а не с
        // сохранением суммы - иначе коррелированная пара даёт +6 дБ и вылетает
        // за шкалу; уровень возвращают панорама и общий множитель зоны.
        if (fuse_with >= 0 && fuse_with < static_cast<int>(sf2.samples.size())) {
            const Sf2Sample& o = sf2.samples[fuse_with];
            const uint32_t off = start - sf2.samples[sid].start; // тот же сдвиг у партнёра
            for (size_t i = 0; i < body.size(); ++i) {
                const uint32_t j = o.start + off + static_cast<uint32_t>(i);
                if (j >= o.end || j >= sf2.pcm.size()) break;
                body[i] = static_cast<int16_t>((int32_t(body[i]) + int32_t(sf2.pcm[j])) / 2);
            }
        }
        double ratio = 1.0;
        if (target_rate != s.rate) {
            ratio = static_cast<double>(target_rate) / s.rate;
            body  = resample(body, ratio);
            ++stat_resampled;
        }
        // Полка верхних частот первого порядка: y = x + k*(x - lp), где
        // lp - однополюсный низкочастотный на 2 кГц. Выше 2 кГц выходит
        // подъём на (1+k), ниже - единица. Считается в double и
        // ограничивается по шкале: сэмплы у нас нормализованы к полной,
        // и без ограничения полка выбивала бы вершины в клип.
        if (bright_q != 0) {
            const double gain = std::pow(10.0, (bright_q / 10.0) / 20.0) - 1.0;
            const double a    = 1.0 - std::exp(-2.0 * kPi * 2000.0 / double(target_rate));
            double lp         = body.empty() ? 0.0 : double(body[0]);
            for (size_t n = 0; n < body.size(); ++n) {
                const double x  = double(body[n]);
                lp             += a * (x - lp);
                const double y  = x + gain * (x - lp);
                body[n]         = static_cast<int16_t>(y > 32767.0 ? 32767.0 : (y < -32768.0 ? -32768.0 : y));
            }
        }
        // Продолжение за петлёй = начало петли.
        //
        // Интерполяция на границе петли берёт пару соседних отсчётов, и если
        // вторым оказывается то, что лежит за loop_end (обычная незацикленная
        // хвостовая часть записи), на стыке выходит излом. Слышно его, только
        // когда шаг воспроизведения не единица, то есть на слитых зонах, где нота
        // играет чужую запись: при своей записи шаг равен единице, интерполяции
        // нет, и стыка не видно.
        //
        // Лечится офлайн: несколько отсчётов после loop_end заменяются на те, что
        // стоят в начале петли. Данные за петлёй при зацикленном воспроизведении
        // не звучат, терять нечего.
        if (loop_seam > 0 && s.loop_end > s.loop_start && s.loop_end < s.end) {
            const uint32_t ls = static_cast<uint32_t>((s.loop_start - start) * ratio);
            const uint32_t le = static_cast<uint32_t>((s.loop_end - start) * ratio);
            const uint32_t n  = static_cast<uint32_t>(loop_seam);
            if (le + n <= body.size() && ls + n <= body.size() && le > ls) {
                for (uint32_t i = 0; i < n; ++i)
                    body[le + i] = body[ls + i];
                ++stat_seam;
            }
        }

        // Сшивка петли. Зацикленный сэмпл идёт по кругу loop_start..loop_end, и
        // если на стыке форма не сходится, каждый оборот даёт излом. Слышно как
        // тихий щелчок с периодом ноты: на рояле SGM петля длиной в один период,
        // и излом повторяется 116 раз в секунду.
        //
        // Кодек ни при чём - проверено выпечкой всего банка в Raw8, изломы те же
        // (28 против 27). Дело в самих данных.
        //
        // Лечится офлайн и бесплатно для платы: хвост петли плавно сводится с
        // тем, что лежит перед её началом. Тогда отсчёт после loop_end-1
        // продолжает то, с чего начинается loop_start. Вес - та же кривая, что у
        // антиклика: производная на концах нулевая, поэтому шва нет и в первой
        // производной.
        if (loop_xfade > 0 && s.loop_end > s.loop_start && s.loop_start > s.start) {
            const uint32_t ls           = static_cast<uint32_t>((s.loop_start - start) * ratio);
            const uint32_t le           = static_cast<uint32_t>((s.loop_end - start) * ratio);
            const uint32_t avail_before = ls; // сколько есть перед петлёй
            const uint32_t loop_len     = le > ls ? le - ls : 0;
            uint32_t n                  = static_cast<uint32_t>(loop_xfade);
            if (n > avail_before) n = avail_before;
            if (n > loop_len / 2) n = loop_len / 2;
            if (le <= body.size() && n >= 4) {
                for (uint32_t i = 0; i < n; ++i) {
                    const double x   = double(i + 1) / double(n);
                    const double w   = x * x * (3.0 - 2.0 * x); // 0 -> 1, гладко
                    const size_t dst = le - n + i;
                    const size_t src = ls - n + i;
                    if (dst >= body.size() || src >= body.size()) break;
                    const double v = double(body[dst]) * (1.0 - w) + double(body[src]) * w;
                    body[dst]      = static_cast<int16_t>(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
                }
                ++stat_xfade;
            }
        }

        const uint32_t len = static_cast<uint32_t>(body.size());
        if (len == 0) return nullptr;
        // Громкость прогона - по первой половине секунды, а не по всему телу.
        //
        // Ухо судит ноту по атаке и началу спада; у соседних зон мультисэмпла
        // длина и хвост разные, и средний по всему сэмплу уровень переставляет
        // их местами. Первая версия мерила целиком, и поправка выходила мимо:
        // ступень F-2/F#2 у SGM оставалась теми же 3.4 дБ после "выравнивания".
        //
        // Пик тоже не годится: у щипковых он стоит на одном отсчёте.
        const size_t rms_window = std::min(body.size(), static_cast<size_t>(target_rate / 2));
        double rms_acc          = 0.0;
        uint32_t rms_n          = 0;
        for (size_t i = 0; i < rms_window; i += std::max<size_t>(1, rms_window / 40000)) {
            rms_acc += double(body[i]) * double(body[i]);
            ++rms_n;
        }
        const double run_rms = rms_n ? std::sqrt(rms_acc / rms_n) : 0.0;

        // Кодирование тем же кодеком, что играет плата.
        std::vector<int8_t> enc(len);
        const uint32_t cp_cap = len / dpcm8::kCheckpointIntervalSamples + 2;
        std::vector<dpcm8::Dpcm8Checkpoint> cps(cp_cap);
        dpcm8::Dpcm8State st{};
        uint32_t cp_count = 0;
        cp_count          = dpcm8::encode_block(body.data(), len, 0, st, enc.data(), cps.data(), cp_cap);

        // Кодек выбирается замером ошибки, а не назначается всем подряд.
        //
        // У Dpcm8 шум продифференцирован (сидит наверху), и на крутом громком
        // фронте кодек не успевает за сигналом - перегрузка по крутизне. У Raw8
        // шум ровный и перегрузки нет, размер тот же (байт на отсчёт), а
        // распаковка дешевле (прыжок вместо последовательного декодирования).
        //
        // Пользователь услышал это как "хрюкает на сильном ударнике". Замер: на
        // сильных ударах Breakout полоса 16..22 кГц у нас была на 13..17 дБ выше
        // эталонной, причём интерполяция ни при чём - эталон, принудительно
        // переведённый на линейную, ультразвука не набрал.
        bool use_raw8 = false;
        {
            dpcm8::Dpcm8State ds{};
            double err_dpcm = 0.0, err_raw = 0.0;
            for (uint32_t i = 0; i < len; ++i) {
                const int32_t dec  = dpcm8::decode_delta(static_cast<uint8_t>(enc[i]), ds);
                const double d     = double(dec) - double(body[i]);
                err_dpcm          += d * d;
                // Raw8: старший байт со знаком, восстановление сдвигом.
                const int32_t r8  = int32_t(int8_t(body[i] >> 8)) << 8;
                const double e    = double(r8) - double(body[i]);
                err_raw          += e * e;
            }
            use_raw8 = force_raw8 || err_dpcm > err_raw;
            if (use_raw8) ++stat_raw8;
        }
        if (use_raw8) {
            for (uint32_t i = 0; i < len; ++i)
                enc[i] = static_cast<int8_t>(body[i] >> 8);
            cp_count = 0;
        }

        // Раскладка прогона: тело, добивка до границы страницы, чекпоинты.
        // Так их кладёт SamplePacker::finish (чекпоинты строго с новой страницы,
        // чтобы K-й лежал по offset = K * sizeof от её начала), и подкачка на
        // плате сводится к копированию прогона подряд.
        constexpr uint32_t kPage   = 1024;
        const uint32_t body_padded = ((len + kPage - 1) / kPage) * kPage;
        const uint32_t cp_bytes    = cp_count * static_cast<uint32_t>(sizeof(dpcm8::Dpcm8Checkpoint));
        PcmRun run{};
        run.bytes       = body_padded + cp_bytes;
        run.length      = len;
        run.checkpoints = cp_count;
        run.start       = start;
        run.ratio       = ratio;
        run.rate        = target_rate;
        run.raw8        = use_raw8;
        run.rms         = run_rms;

        // Один и тот же звук приходит из разных зон под разными ключами (свои
        // смещения обрезки), но байты выходят те же. Сверяем по содержимому и
        // переиспользуем, иначе банк раздувается на ровном месте.
        const uint64_t hash = content_hash(enc.data(), len);
        auto hit            = pcm_by_hash.find(hash);
        if (hit != pcm_by_hash.end() && hit->second.length == len && hit->second.checkpoints == cp_count) {
            run.start     = start;
            run.ratio     = ratio;
            run.rate      = target_rate;
            run.raw8      = use_raw8;
            run.raw_index = hit->second.raw_index;
            ++stat_pcm_shared;
        } else {
            run.raw_index  = static_cast<uint32_t>(raw_runs.size());
            stat_body     += len;
            stat_pad      += body_padded - len;
            stat_cp       += cp_bytes;
            ++runs_per_sample[sid];
            std::vector<uint8_t> raw(run.bytes, 0);
            std::memcpy(raw.data(), enc.data(), len);
            if (cp_bytes) std::memcpy(raw.data() + body_padded, cps.data(), cp_bytes);
            raw_runs.push_back(std::move(raw));
            pcm_by_hash[hash] = run;
        }
        return &pcm_runs.emplace(key, run).first->second;
    };

    // Складывает генераторы пресета к генераторам зоны инструмента.
    // Диапазоны пересекаются, остальное складывается - так требует
    // спецификация; сложение диапазонов дало бы чушь.
    auto combine = [](const GenSet& pre, const GenSet& ins) {
        GenSet r = ins;
        for (int i = 0; i < 64; ++i) {
            if (!pre.has(i)) continue;
            if (i == GEN_KEY_RANGE || i == GEN_VEL_RANGE) {
                if (!ins.has(i)) {
                    r.g[i] = pre.g[i];
                    continue;
                }
                const int32_t lo = std::max(pre.g[i] & 0xFF, ins.g[i] & 0xFF);
                const int32_t hi = std::min((pre.g[i] >> 8) & 0xFF, (ins.g[i] >> 8) & 0xFF);
                r.g[i]           = (hi << 8) | lo;
            } else if (i == GEN_SAMPLE_ID || i == GEN_INSTRUMENT || i == GEN_SAMPLE_MODES || i == GEN_EXCLUSIVE_CLASS || i == GEN_OVERRIDING_ROOT ||
                       i == GEN_SCALE_TUNING || i == GEN_START_OFFSET || i == GEN_END_OFFSET || i == GEN_LOOP_START_OFFSET || i == GEN_LOOP_END_OFFSET) {
                // Уровня инструмента; на уровне пресета игнорируются.
            } else {
                // Генератор пресета - всегда смещение, и складывается он с уровнем
                // инструмента, а если там ничего нет - с умолчанием SF2, а не с нулём
                // (см. sf2_gen_default выше).
                r.g[i] = (ins.has(i) ? ins.g[i] : sf2_gen_default(i)) + pre.g[i];
            }
        }
        return r;
    };

    // --- Сборка по пресетам ---
    struct ZoneOut {
        int key_lo, key_hi, sample;
        int8_t note_offset;
    };
    // mergeable - можно ли прореживать зоны этого инструмента. Нельзя у
    // всего, что не следует за клавиатурой: ударные (scaleTuning 0) и
    // растянутые строи. Там сосед по клавише - другой звук, а не тот же
    // на другой высоте, и слияние подменило бы малый барабан хай-хэтом.
    struct InstBuild {
        Profile prof;
        std::vector<ZoneOut> zones;
        bool mergeable = true;
    };

    for (const Sf2Preset& pre : sf2.presets) {
        if (pre.bank > 128 || pre.program > 127) continue;
        if (gm_only && pre.bank != 0 && pre.bank != 128) continue;
        std::map<Profile, std::vector<InstBuild>> groups;

        for (const Sf2PresetZone& pz : pre.zones) {
            if (pz.instrument < 0 || pz.instrument >= static_cast<int>(sf2.insts.size())) continue;
            for (const Sf2Zone& iz : sf2.insts[pz.instrument].zones) {
                GenSet g          = combine(pz.gen, iz.gen);
                const int32_t sid = g.get(GEN_SAMPLE_ID, -1);
                if (sid < 0 || sid >= static_cast<int>(sf2.samples.size())) continue;

                const int32_t kr = g.get(GEN_KEY_RANGE, 0x7F00);
                int key_lo = kr & 0xFF, key_hi = (kr >> 8) & 0xFF;
                if (!g.has(GEN_KEY_RANGE)) {
                    key_lo = 0;
                    key_hi = 127;
                }
                if (key_lo > key_hi) continue;
                // Диапазоны velocity пресета и зоны не пересекаются - зона к пресету не
                // относится, как и при пустом диапазоне нот: слой не выбирался бы никогда.
                const int32_t vr = g.get(GEN_VEL_RANGE, 0x7F00);
                if (g.has(GEN_VEL_RANGE) && (vr & 0xFF) > ((vr >> 8) & 0xFF)) {
                    ++stat_vel_empty;
                    continue;
                }
                // Шкала движка - 0..127, как у MIDI: зоны выше 119 не режутся.
                if (key_lo > 127) {
                    ++stat_notes_over;
                    continue;
                }
                if (key_hi > 127) key_hi = 127;

                // Полка только там, где зоны сливаются: у ударных и растянутых строёв
                // слияние выключено.
                const bool stretched = merge_zones > 1 && pre.bank != 128 && g.get(GEN_SCALE_TUNING, 100) == 100;
                // Правая половина сведённой пары не печётся: её зона поглощена левой.
                if (fuse_into.count(sid)) continue;
                const auto fs_it    = fuse_sum_with.find(sid);
                const int fuse_with = fs_it != fuse_sum_with.end() ? fs_it->second : -1;
                PcmRun* run         = bake_pcm(sid, g, stretched ? merge_bright_db : 0.0, fuse_with);
                if (!run) continue;

                const Sf2Sample& s         = sf2.samples[sid];
                const uint32_t target_rate = run->rate; // та, на которой прогон испечён
                const int32_t root_gen     = g.get(GEN_OVERRIDING_ROOT, -1);
                const int32_t root         = (root_gen >= 0 && root_gen <= 127) ? root_gen : s.root;
                const double cents         = g.get(GEN_COARSE_TUNE, 0) * 100.0 + g.get(GEN_FINE_TUNE, 0) + s.correction;
                // Вся подстройка и root складываются в c5_speed: у движка
                // finetune всего плюс-минус полутон, а coarseTune бывает 120.
                const uint32_t c5 = static_cast<uint32_t>(target_rate * std::pow(2.0, (60.0 - root) / 12.0 + cents / 1200.0) + 0.5);

                const uint32_t zls = s.loop_start + g.get(GEN_LOOP_START_OFFSET, 0) + g.get(GEN_LOOP_START_COARSE, 0) * 32768;
                const uint32_t zle = s.loop_end + g.get(GEN_LOOP_END_OFFSET, 0) + g.get(GEN_LOOP_END_COARSE, 0) * 32768;
                uint32_t rls       = zls > run->start ? static_cast<uint32_t>((zls - run->start) * run->ratio) : 0;
                uint32_t rle       = zle > run->start ? static_cast<uint32_t>((zle - run->start) * run->ratio) : 0;
                if (rle > run->length) rle = run->length;
                if (rls >= rle) {
                    rls = 0;
                    rle = 0;
                }
                const int32_t modes   = g.get(GEN_SAMPLE_MODES, 0);
                const bool loop       = (modes & 1) != 0 && rle > rls;
                const int32_t pan_raw = g.get(GEN_PAN, 0); // -500..500
                int8_t pan            = static_cast<int8_t>(std::max(0, std::min(64, 32 + pan_raw * 32 / 500)));
                // У сведённой пары панорама берётся из баланса половин, а не из
                // генератора: генератор говорит "жёстко влево", а моно должно встать
                // туда, где пара звучала вместе.
                const auto fp_it = fuse_pan.find(sid);
                if (fp_it != fuse_pan.end()) pan = fp_it->second;

                const int32_t atten_cb = g.get(GEN_INITIAL_ATTENUATION, 0);
                double gain            = attenuation_to_linear(atten_cb);
                // Поправка за сведение стереопары - см. fuse_gain.
                const auto fg_it = fuse_gain.find(sid);
                if (fg_it != fuse_gain.end()) gain *= fg_it->second;
                // Компенсация резонанса фильтра.
                //
                // initialFilterQ задаёт высоту пика на срезе, и синтезатор должен на неё
                // же приглушить весь голос, иначе резонанс поднимал бы и всё остальное:
                // fluidsynth так и делает - filter_gain = 1/sqrt(q_lin). Когда писалось,
                // фильтр банка на плеере не применялся (ради полифонии), приглушения не
                // было, и зоны с резонансом играли громче всех прочих. Сейчас загрузчик
                // .mid включает срез (бит 0x80 в filter_cutoff), а приглушение за Q
                // по-прежнему печётся здесь.
                //
                // Q ненулевой у 3438 зон из 21650 (15.9%).
                //
                // Сантибел здесь читается как 0.1 дБ - так в спецификации и в
                // spessasynth_core (lowpass_filter.ts: qGain =
                // 1/sqrt(cbAttenuationToGain(-qCb)), а cbAttenuationToGain - это
                // 10^(-cb/200)). Раньше тут стояло 0.04 по аналогии с
                // initialAttenuation - оно подгонялось под замер, снятый до починки
                // потолка частоты, то есть по треугольнику с отрезанным верхом. С
                // восстановленным верхом 0.1 лучше и по замеру: среднее отклонение по
                // девяти каналам Bohemian 1.96 -> 1.89 дБ. Услышано на треугольнике в
                // Bohemian Rhapsody: у него Q = 190 сантибел, он единственный такой в
                // наборе и поэтому лез вперёд бочки и малого.
                //
                // Когда фильтр включим, эту компенсацию надо убрать отсюда и отдать
                // самому фильтру, иначе она учтётся дважды.
                if (q_comp_db_per_cb > 0.0) {
                    const int32_t q_cb = g.get(GEN_INITIAL_FILTER_Q, 0);
                    if (q_cb > 0) {
                        const double q_db  = q_cb * q_comp_db_per_cb;
                        gain              /= std::sqrt(std::pow(10.0, q_db / 20.0));
                    }
                }
                // Усиление выше единицы не обрезаем. 64 в этом поле - единица (движок
                // множит на 1024 в Q16, tracker_engine.cpp), поле однобайтовое, значит
                // выражается до 3.98, и +7 дБ, самое большое усиление в банке, ложится
                // в 143.
                //
                // Обрезка до единицы была слышна: усиление просят 98 зон из 21650, почти
                // все - тарелки и хэты ударных наборов (-50 сантибел, то есть +5 дБ, у
                // Crash, Ride, Splash, Hi-Hats). Треугольник усиления не просит, и при
                // обрезке весь остальной набор оказывался тише него на 5 дБ. Замер на
                // Bohemian Rhapsody, 3:22: полоса 4.5..6 кГц (у нас это на 99%
                // треугольник) была на 4.3 дБ громче эталонной относительно всего
                // набора. Пользователь слышал это как "треугольник лезет на первый
                // план".
                if (gain > 1.0) ++stat_gain_clipped;
                const long gv         = std::lround(64.0 * gain);
                const uint8_t smp_gv0 = static_cast<uint8_t>(gv < 0 ? 0 : (gv > 255 ? 255 : gv));
                // Общий уровень банка (и отдельный уровень ударных) применяется здесь,
                // до построения ключа записи.
                //
                // Иначе он теряется на переиспользовании: ключ совпал бы с уже созданной
                // записью того же прогона, и вернулась бы чужая громкость. Так и вышло с
                // ударными: часть их сэмплов делит прогон с мелодическими пресетами, те
                // обрабатываются раньше, и --drum-gain-db на этих нотах не срабатывал
                // (на пробе -20 против +9 разница выходила 0.4 дБ вместо 29).
                const double gain_db = (drum_gain_set && pre.bank == 128) ? drum_gain_db : bank_gain_db;
                uint8_t smp_gv       = smp_gv0;
                if (gain_db != 0.0) {
                    double g2 = std::lround(double(smp_gv0) * std::pow(10.0, gain_db / 20.0));
                    if (g2 < 1.0) g2 = 1.0;
                    if (g2 > 255.0) g2 = 255.0;
                    smp_gv = static_cast<uint8_t>(g2);
                }
                if (drum_gain_set && pre.bank == 128) ++stat_drum_gain_hits;
                ++stat_gain_records;
                const SmpKey sk{run->raw_index, c5, loop ? (rls * 65536u + rle) | 0x80000000u : 0u, (pan << 8) | smp_gv};
                auto sit = sample_index.find(sk);
                uint16_t smp_idx;
                if (sit == sample_index.end()) {
                    BankSample bs{};
                    bs.pcm_offset       = run->raw_index; // пока индекс прогона, чинится после сжатия
                    bs.pcm_bytes        = run->bytes;
                    bs.pcm_packed_bytes = 0;
                    bs.length_samples   = run->length;
                    bs.loop_start       = loop ? rls : 0;
                    bs.loop_end         = loop ? rle : 0;
                    bs.c5_speed         = c5;
                    bs.checkpoint_count = static_cast<uint16_t>(run->checkpoints);
                    bs.resident_encoding =
                        static_cast<uint8_t>(run->raw8 ? soundsinth::model::ResidentEncoding::Raw8 : soundsinth::model::ResidentEncoding::Dpcm8);
                    bs.flags          = loop ? kSampleLoopBit : 0;
                    bs.default_volume = 64;
                    // Общий уровень банка ложится сюда же, к
                    // initialAttenuation зоны: одно поле, одна цена.
                    bs.global_volume   = smp_gv; // уровень уже учтён в ключе выше
                    bs.default_panning = pan;
                    smp_idx            = static_cast<uint16_t>(samples.size());
                    samples.push_back(bs);
                    sample_level.push_back(run->rms * (smp_gv / 64.0));
                    sample_raw_index.push_back(run->raw_index);
                    sample_index[sk] = smp_idx;
                } else {
                    smp_idx = sit->second;
                }

                // scaleTuning - центов на клавишу; 100 - обычное следование за
                // клавиатурой, 0 - фиксированная высота (ударные), промежуточные значения
                // растягивают строй. Выражается точно через note_offset диапазонов
                // keymap: нота n должна звучать как root + (n-root)*scale/100, значит
                // смещение (n-root)*(scale-100)/100.
                //
                // Раньше бралось 100 у всего, кроме нуля. Это задевало 109 зон из 2486:
                // шумы, птицы, машины (программы 120..127), а из мелодических - Bottle
                // Blow, Shakuhachi, Whistle и часть наборов ударных со scaleTuning 50.
                // Ошибка на краю зоны доходила до полутора октав, то есть эти звуки
                // играли не ту высоту.
                const int32_t scale = g.get(GEN_SCALE_TUNING, 100);

                // Удлинение релиза: таймценты логарифмические, значит
                // множитель времени - это слагаемое 1200*log2(k).
                {
                    auto rs = release_scale.find(static_cast<int>(pre.program));
                    if (rs != release_scale.end() && pre.bank != 128) {
                        g.g[GEN_RELEASE_VOL_ENV] += static_cast<int32_t>(std::lround(1200.0 * std::log2(rs->second)));
                        ++stat_release_scaled;
                    }
                }
                // Укорочение спада: время в таймцентах, сустейн в полную тишину, иначе
                // нота дойдёт до сустейна и там повиснет.
                {
                    auto ov = decay_override.find(static_cast<int>(pre.program));
                    if (ov != decay_override.end() && pre.bank != 128) {
                        g.g[GEN_DECAY_VOL_ENV]   = static_cast<int32_t>(std::lround(1200.0 * std::log2(ov->second)));
                        g.g[GEN_SUSTAIN_VOL_ENV] = 960; // -96 дБ, то есть тишина
                        ++stat_decay_forced;
                    }
                }
                // Модулятор velocity->срез: зона инструмента и зона
                // пресета складываются, как и генераторы-смещения. Так же и
                // модуляторы velocity->глубина огибающей фильтра.
                const int32_t vr_all   = g.get(GEN_VEL_RANGE, 0x7F00);
                const int typical_vel  = g.has(GEN_VEL_RANGE) ? layer_typical_velocity(vr_all & 0xFF, (vr_all >> 8) & 0xFF) : layer_typical_velocity(0, 127);
                const int32_t menv_mod = vel_mod_cents(iz.vel_to_menv, typical_vel) + vel_mod_cents(pz.vel_to_menv, typical_vel);
                if (menv_mod != 0) ++stat_menv_vel;
                const Profile prof = profile_of(g, iz.vel_to_fc_cents + pz.vel_to_fc_cents, menv_mod);
                auto& bucket       = groups[prof];
                // Зона идёт в первый инструмент того же облика, где её ноты
                // ни с кем не пересекаются; иначе заводится новый - это и
                // есть слой.
                InstBuild* target = nullptr;
                for (auto& ib : bucket) {
                    bool overlap = false;
                    for (const auto& z : ib.zones) {
                        if (!(key_hi < z.key_lo || key_lo > z.key_hi)) {
                            overlap = true;
                            break;
                        }
                    }
                    if (!overlap) {
                        target = &ib;
                        break;
                    }
                }
                if (!target) {
                    bucket.push_back(InstBuild{prof, {}});
                    target = &bucket.back();
                }

                if (scale != 100) target->mergeable = false;
                if (scale != 100) {
                    // Соседние ноты с одинаковым смещением сводятся в один диапазон: при
                    // scale 50 смещение меняется через ноту, и диапазон на каждую ноту зря
                    // раздул бы keymap вдвое.
                    int run_lo  = key_lo;
                    int run_off = scale_offset(key_lo, root, scale);
                    for (int n = key_lo + 1; n <= key_hi + 1; ++n) {
                        const int off = n <= key_hi ? scale_offset(n, root, scale) : (run_off + 1);
                        if (off == run_off) continue;
                        target->zones.push_back(ZoneOut{run_lo, n - 1, smp_idx, static_cast<int8_t>(run_off)});
                        run_lo  = n;
                        run_off = off;
                    }
                    ++stat_scale_tuning;
                } else {
                    target->zones.push_back(ZoneOut{key_lo, key_hi, smp_idx, 0});
                }
            }
        }

        // --- Выравнивание по суммарной громкости ноты ---
        //
        // Ухо ловит перепад между соседними нотами. Считать его по зонам
        // поодиночке нельзя: на ноте звучит несколько слоёв, и на границе зон
        // меняется вся их пара разом. У SGM на F-2 звучат сэмплы 969+999, на F#2
        // уже 1000+1001, и слышна сумма - она и прыгает на 3.4 дБ, хотя каждая
        // зона внутри своей полосы velocity ровная. Первая версия мерила по зонам
        // и ступень не увидела.
        //
        // Поэтому считается суммарная энергия каждой ноты при типичной силе
        // нажатия, сглаживается по клавиатуре, и поправка разносится на все зоны,
        // которые эту ноту обслуживают. Медленный ход уровня по регистру
        // сохраняется - ограничивается только шаг между соседними нотами.
        if (even_zones_db > 0.0) {
            constexpr int kProbeVel = 100;
            // Зоны, звучащие на ноте, и их суммарная энергия.
            std::vector<std::vector<ZoneOut*>> at_note(128);
            for (auto& kv : groups) {
                for (auto& ib : kv.second) {
                    if (kProbeVel < ib.prof.vel_lo || kProbeVel > ib.prof.vel_hi) continue;
                    for (auto& z : ib.zones) {
                        for (int n = std::max(0, z.key_lo); n <= std::min(127, z.key_hi); ++n) {
                            at_note[n].push_back(&z);
                        }
                    }
                }
            }
            auto note_db = [&](int n) {
                double energy = 0.0;
                for (const ZoneOut* z : at_note[n]) {
                    if (z->sample < 0 || z->sample >= static_cast<int>(sample_level.size())) continue;
                    const double l  = sample_level[z->sample];
                    energy         += l * l;
                }
                return energy > 1e-12 ? 10.0 * std::log10(energy) : -999.0;
            };
            // Целевая кривая: та же, но без шагов больше порога.
            std::vector<double> cur(128, -999.0), target(128, -999.0);
            for (int n = 0; n < 128; ++n)
                cur[n] = note_db(n);
            double prev = -999.0;
            for (int n = 0; n < 128; ++n) {
                if (cur[n] < -900.0) {
                    target[n] = cur[n];
                    continue;
                }
                if (prev < -900.0) {
                    target[n] = cur[n];
                    prev      = cur[n];
                    continue;
                }
                const double d = cur[n] - prev;
                target[n]      = prev + (std::fabs(d) <= even_zones_db ? d : (d > 0 ? even_zones_db : -even_zones_db));
                prev           = target[n];
            }
            // Поправка зоны - среднее по нотам, которые она обслуживает.
            std::map<ZoneOut*, std::pair<double, int>> corr;
            for (int n = 0; n < 128; ++n) {
                if (cur[n] < -900.0) continue;
                ++stat_even_seen;
                const double d = target[n] - cur[n];
                if (std::fabs(d) > stat_even_maxdelta) stat_even_maxdelta = std::fabs(d);
                if (std::fabs(d) < 0.05) continue;
                for (ZoneOut* z : at_note[n]) {
                    corr[z].first  += d;
                    corr[z].second += 1;
                }
            }
            for (auto& kv2 : corr) {
                ZoneOut* z      = kv2.first;
                const double db = kv2.second.second ? kv2.second.first / kv2.second.second : 0.0;
                if (std::fabs(db) < 0.05) continue;
                if (z->sample < 0 || z->sample >= static_cast<int>(samples.size())) continue;
                const BankSample& src = samples[z->sample];
                const long ngv        = std::lround(src.global_volume * std::pow(10.0, db / 20.0));
                const uint8_t gv      = static_cast<uint8_t>(ngv < 1 ? 1 : (ngv > 255 ? 255 : ngv));
                if (gv == src.global_volume) continue;
                const auto key = std::make_pair(static_cast<uint16_t>(z->sample), gv);
                auto it        = even_variant.find(key);
                if (it == even_variant.end()) {
                    BankSample copy    = src;
                    copy.global_volume = gv;
                    const uint16_t idx = static_cast<uint16_t>(samples.size());
                    samples.push_back(copy);
                    sample_level.push_back(sample_level[z->sample] * (double(gv) / double(src.global_volume)));
                    sample_raw_index.push_back(sample_raw_index[z->sample]);
                    it = even_variant.emplace(key, idx).first;
                    ++stat_even_new;
                }
                z->sample = it->second;
                ++stat_even_fixed;
            }
        }

        // Инструменты пресета в записи банка.
        const uint16_t first_layer = static_cast<uint16_t>(layers.size());
        uint32_t layer_count       = 0;
        for (auto& kv : groups) {
            for (auto& ib : kv.second) {
                // Потолок - пятнадцать бит поля, а не 255: на 255 молча терялись верхние
                // полосы velocity целых пресетов (bank_format.h, BankPreset).
                if (ib.zones.empty() || layer_count >= 0x7FFF) continue;
                std::sort(ib.zones.begin(), ib.zones.end(), [](const ZoneOut& a, const ZoneOut& b) { return a.key_lo < b.key_lo; });

                // --- Прореживание зон, строго вниз ---
                //
                // Оставляем каждую N-ю зону, считая сверху, и каждая оставшаяся
                // забирает диапазон соседей снизу. То есть нота играет сэмплом,
                // записанным выше неё и растянутым вниз.
                //
                // Почему вниз, а не вверх: растяжение вверх поднимает шаг
                // воспроизведения, а у Dpcm8 распаковка последовательная, и число
                // распаковок пропорционально шагу (config.h, модель цены голоса). Три
                // полутона вверх - это +19% к цене голоса и подъём зеркал линейной
                // интерполяции. Вниз шаг меньше единицы: голос дешевеет, зеркал нет.
                // Платим тусклостью, её компенсируем отдельно.
                stat_zones_total += static_cast<uint32_t>(ib.zones.size());
                if (!ib.mergeable) ++stat_inst_unmergeable;
                if (merge_zones > 1 && ib.mergeable && ib.zones.size() > 1) {
                    // Жадно сверху вниз: зона-хозяин забирает соседей снизу, пока их не
                    // станет merge_zones и пока нота не уедет от своей записи дальше, чем
                    // разрешено.
                    //
                    // Ограничитель обязателен. Без него у инструментов с редкими широкими
                    // зонами слияние уносило ноту на девять октав от записи - это уже другой
                    // звук. Мерить надо расстояние от исходной нижней границы хозяина, а не
                    // от текущей: она сдвигается при каждом поглощении.
                    //
                    // Поглощённая зона не растворяется в хозяине, а остаётся своим
                    // диапазоном keymap - только играет теперь запись хозяина, а её
                    // собственный уровень сохраняется отдельной записью сэмпла на том же
                    // прогоне PCM. Экономится то, ради чего прореживание и делается: прогоны
                    // PCM. Запись сэмпла стоит 36 байт, диапазон keymap - четыре.
                    //
                    // Без этого слияние приносило на чужие ноты чужую громкость: у SGM
                    // дисторшн-гитара проваливалась в нижнем регистре на 1.2 дБ, чего у
                    // эталонного синтезатора на том же .sf2 нет (+0.02). Слышно это было в
                    // DreamOn.mid на 1:12.
                    std::vector<ZoneOut> kept;
                    int i = static_cast<int>(ib.zones.size()) - 1;
                    while (i >= 0) {
                        ZoneOut host      = ib.zones[i];
                        const int host_lo = host.key_lo;
                        int taken         = 0;
                        int j             = i - 1;
                        while (j >= 0 && taken + 1 < merge_zones && host_lo - ib.zones[j].key_lo <= merge_max_stretch) {
                            const int stretch  = host_lo - ib.zones[j].key_lo;
                            stat_stretch_sum  += stretch;
                            if (stretch > stat_stretch_max) stat_stretch_max = stretch;
                            ++stat_stretch_n;
                            ZoneOut moved     = ib.zones[j];
                            moved.sample      = merge_level ? level_matched(host.sample, moved.sample) : host.sample;
                            moved.note_offset = host.note_offset;
                            kept.push_back(moved);
                            ++stat_zones_merged;
                            ++taken;
                            --j;
                        }
                        kept.push_back(host);
                        i = j;
                    }
                    std::sort(kept.begin(), kept.end(), [](const ZoneOut& a, const ZoneOut& b) { return a.key_lo < b.key_lo; });
                    ib.zones.swap(kept);
                }

                BankInstrument bi{};
                bi.default_volume = 64;
                bi.global_volume  = 128; // нейтрально: затухание в записи сэмпла
                bi.filter_cutoff  = filter_cents_to_it(ib.prof.filter_fc);
                if (bi.filter_cutoff == 127 && ib.prof.filter_fc < 13500) ++stat_filter_lost;
                // Резонанс: initialFilterQ в сантибелах - высота пика на срезе. Шкала
                // резонанса движка 0..127 - это 0..24 дБ (resonant_filter.cpp:
                // dmpfac = 10^(-r*24/128/20), пик около r*24/128 дБ), то есть 128/240
                // единицы на сантибел. Деление на 960 (как если бы вся шкала была
                // 96 дБ) давало пик вчетверо ниже: у SGM Halo Pad 11.7 дБ выходили в
                // 2.8, и гармоники у среза, на которых держится тембр пэда, пропадали.
                bi.filter_resonance = static_cast<uint8_t>(std::max(0, std::min(127, (ib.prof.filter_q * 128 + 120) / 240)));
                // Яркость от силы удара - из модулятора, а не из генератора.
                //
                // prof.vel_to_fc - размах в центах между velocity 0 и 127. Шкала среза -
                // kCutoffUnitsPerOctave делений на октаву (при 16 - одно на 75 центов); поле
                // же задаёт сдвиг в каждую сторону от velocity 64, значит половину
                // размаха:
                //   единиц = центы * U / 1200 / 2 (при 16 - центы / 150).
                //
                // Обрезаем до int8: у GeneralUser встречается 13500 центов - это больше
                // всей шкалы среза, и 127 там уже означает "открыт полностью".
                {
                    const int32_t units   = static_cast<int32_t>(std::lround(ib.prof.vel_to_fc * kCutoffUnitsPerOctave / 2400.0));
                    bi.velocity_to_cutoff = static_cast<int8_t>(std::max(-128, std::min(127, units)));
                    if (bi.velocity_to_cutoff != 0) ++stat_vel_fc;
                }
                bi.exclusive_class    = static_cast<uint8_t>(std::max(0, std::min(255, ib.prof.exclusive)));
                bi.nna                = static_cast<uint8_t>(soundsinth::model::NewNoteAction::Cut);
                bi.dct                = static_cast<uint8_t>(soundsinth::model::DuplicateCheckType::Off);
                bi.dca                = static_cast<uint8_t>(soundsinth::model::DuplicateCheckAction::Cut);
                bi.instrument_panning = -1; // нет своей: панорама в записи сэмпла

                GenSet eg;
                eg.g[GEN_DELAY_VOL_ENV]   = ib.prof.env_delay;
                eg.g[GEN_ATTACK_VOL_ENV]  = ib.prof.env_attack;
                eg.g[GEN_HOLD_VOL_ENV]    = ib.prof.env_hold;
                eg.g[GEN_DECAY_VOL_ENV]   = ib.prof.env_decay;
                eg.g[GEN_SUSTAIN_VOL_ENV] = ib.prof.env_sustain;
                eg.g[GEN_RELEASE_VOL_ENV] = ib.prof.env_release;
                const EnvOut e            = build_volume_envelope(eg);
                bi.env_volume             = kNoIndex;
                if (e.used) {
                    // Одинаковые огибающие у разных инструментов - обычное дело; каждая
                    // весит 108 байт, и в резидентной арене платы это заметно.
                    const std::string esig(reinterpret_cast<const char*>(&e.env), sizeof(BankEnvelope));
                    auto ed = env_by_sig.find(esig);
                    if (ed != env_by_sig.end()) {
                        bi.env_volume = ed->second;
                    } else {
                        bi.env_volume    = static_cast<uint16_t>(envelopes.size());
                        env_by_sig[esig] = bi.env_volume;
                        envelopes.push_back(e.env);
                    }
                }
                bi.env_panning = kNoIndex;
                bi.env_pitch   = kNoIndex;
                bi.env_filter  = kNoIndex;
                {
                    const EnvOut fe = build_filter_envelope(ib.prof);
                    if (fe.used) {
                        const std::string fsig(reinterpret_cast<const char*>(&fe.env), sizeof(BankEnvelope));
                        auto fd = env_by_sig.find(fsig);
                        if (fd != env_by_sig.end()) {
                            bi.env_filter = fd->second;
                        } else {
                            bi.env_filter    = static_cast<uint16_t>(envelopes.size());
                            env_by_sig[fsig] = bi.env_filter;
                            envelopes.push_back(fe.env);
                        }
                        ++stat_filter_env;
                    }
                }
                bi.fadeout_ms = e.fadeout_ms;

                // keymap: сплошное покрытие 0..127, дырки - kNoSample.
                const size_t keymap_mark = keymap.size(); // откат - к этому размеру
                bi.keymap_first          = static_cast<uint16_t>(keymap_mark);
                int next_note            = 0;
                for (const auto& z : ib.zones) {
                    if (z.key_lo > next_note) {
                        keymap.push_back({static_cast<uint8_t>(next_note), 0, kNoSample});
                    }
                    keymap.push_back({static_cast<uint8_t>(z.key_lo), static_cast<uint8_t>(z.note_offset), static_cast<uint16_t>(z.sample)});
                    next_note = z.key_hi + 1;
                }
                if (next_note < 128) keymap.push_back({static_cast<uint8_t>(next_note), 0, kNoSample});
                bi.keymap_count   = static_cast<uint16_t>(keymap.size() - bi.keymap_first);
                bi.default_sample = static_cast<uint16_t>(ib.zones[0].sample);

                // Один и тот же SF2-инструмент используют несколько пресетов; строить
                // копию каждому - раздувать банк на ровном месте. Сверяем по содержимому
                // (поля записи плюс её keymap) и переиспользуем.
                std::string sig(reinterpret_cast<const char*>(&bi), sizeof(bi));
                sig.append(reinterpret_cast<const char*>(keymap.data() + bi.keymap_first), bi.keymap_count * sizeof(BankKeymapRange));
                // keymap_first у копии другой, поэтому в подписи он обнуляется. Вместо
                // индекса огибающей к подписи добавляется её содержимое.
                {
                    BankInstrument probe = bi;
                    probe.keymap_first   = 0;
                    sig.assign(reinterpret_cast<const char*>(&probe), sizeof(probe));
                    sig.append(reinterpret_cast<const char*>(keymap.data() + bi.keymap_first), bi.keymap_count * sizeof(BankKeymapRange));
                    if (bi.env_volume != kNoIndex) {
                        sig.append(reinterpret_cast<const char*>(&envelopes[bi.env_volume]), sizeof(BankEnvelope));
                    }
                    if (bi.env_filter != kNoIndex) {
                        sig.append("F", 1);
                        sig.append(reinterpret_cast<const char*>(&envelopes[bi.env_filter]), sizeof(BankEnvelope));
                    }
                }
                uint16_t inst_idx;
                auto dup = inst_by_sig.find(sig);
                if (dup != inst_by_sig.end()) {
                    inst_idx = dup->second;
                    keymap.resize(keymap_mark); // откатываем добавленный keymap
                    ++stat_inst_shared;
                } else {
                    // Новый инструмент: такой же прогон keymap уже есть - берём его, а
                    // только что дописанный откатываем. Откат - к размеру до дописывания,
                    // не к keymap_first: тот может указывать на более ранний чужой прогон.
                    const std::string run(reinterpret_cast<const char*>(keymap.data() + keymap_mark), bi.keymap_count * sizeof(BankKeymapRange));
                    auto shared = keymap_by_run.find(run);
                    if (shared != keymap_by_run.end()) {
                        keymap.resize(keymap_mark);
                        bi.keymap_first = shared->second;
                        ++stat_keymap_shared;
                    } else {
                        keymap_by_run[run] = bi.keymap_first;
                    }
                    inst_idx = static_cast<uint16_t>(instruments.size());
                    instruments.push_back(bi);
                    inst_by_sig[sig] = inst_idx;
                }

                BankLayer bl{};
                bl.vel_lo     = static_cast<uint8_t>(ib.prof.vel_lo);
                bl.vel_hi     = static_cast<uint8_t>(ib.prof.vel_hi);
                bl.instrument = inst_idx;

                // Дубль слоя - тот же инструмент в том же диапазоне velocity - значит,
                // что сэмпл прозвучит дважды, то есть на 6 дБ громче остальных. Взяться
                // ему есть откуда: профили группируются по огибающей и фильтру, а
                // дедупликация выше схлопывает группы, ставшие побайтово одинаковыми, -
                // список слоёв про это не знал.
                //
                // Задело один пресет из 287, но это Tenor Sax (программа 66): 12 лишних
                // слоёв из 25. В Bond.mid он стоит в унисоне с валторной и тромбоном и
                // забивал обоих, так что трио звучало одним инструментом.
                bool already = false;
                for (size_t q = first_layer; q < layers.size(); ++q) {
                    if (layers[q].instrument == bl.instrument && layers[q].vel_lo == bl.vel_lo && layers[q].vel_hi == bl.vel_hi) {
                        already = true;
                        break;
                    }
                }
                if (already && !keep_dup_layers) {
                    ++stat_layer_dup;
                    continue;
                }

                layers.push_back(bl);
                ++layer_count;
            }
        }
        if (layer_count) {
            BankPreset& slot           = presets[pre.bank * kProgramCount + pre.program];
            slot.first_layer           = first_layer;
            slot.layer_count_and_flags = static_cast<uint16_t>(layer_count & 0x7FFFu);
        }
        if (layer_count > stat_max_layers) stat_max_layers = static_cast<uint32_t>(layer_count);
    }

    // --- Выброс осиротевших сэмплов и прогонов ---
    //
    // Записи сэмплов заводятся, когда зона впервые их просит, а зоны потом
    // сливаются (--merge-zones): поглощённая зона уходит, а её запись и PCM
    // остаются в банке, хотя на них уже никто не ссылается. На Arachno при
    // 1:3 это 8.5 МБ мёртвого груза.
    //
    // Проход простой: собрать живые индексы из keymap и default_sample,
    // сжать таблицы, переписать ссылки. Прогоны - тем же приёмом следом, уже
    // по сжатой таблице сэмплов.
    {
        std::vector<uint8_t> live_sample(samples.size(), 0);
        for (const auto& k : keymap) {
            if (k.sample_index != kNoSample && k.sample_index < samples.size()) live_sample[k.sample_index] = 1;
        }
        for (const auto& bi : instruments) {
            if (bi.default_sample != kNoIndex && bi.default_sample < samples.size()) live_sample[bi.default_sample] = 1;
        }
        std::vector<uint16_t> smap(samples.size(), kNoSample);
        std::vector<BankSample> kept_samples;
        std::vector<uint32_t> kept_raw_index;
        for (size_t i = 0; i < samples.size(); ++i) {
            if (!live_sample[i]) continue;
            smap[i] = static_cast<uint16_t>(kept_samples.size());
            kept_samples.push_back(samples[i]);
            kept_raw_index.push_back(sample_raw_index[i]);
        }
        const size_t dropped_samples = samples.size() - kept_samples.size();
        for (auto& k : keymap) {
            if (k.sample_index != kNoSample && k.sample_index < smap.size()) k.sample_index = smap[k.sample_index];
        }
        for (auto& bi : instruments) {
            if (bi.default_sample != kNoIndex && bi.default_sample < smap.size()) bi.default_sample = smap[bi.default_sample];
        }
        samples.swap(kept_samples);
        sample_raw_index.swap(kept_raw_index);

        std::vector<uint8_t> live_run(raw_runs.size(), 0);
        for (const auto& bs : samples) {
            if (bs.pcm_offset < raw_runs.size()) live_run[bs.pcm_offset] = 1; // тут ещё индекс прогона
        }
        std::vector<uint32_t> rmap(raw_runs.size(), 0);
        std::vector<std::vector<uint8_t>> kept_runs;
        uint64_t dropped_bytes = 0;
        for (size_t i = 0; i < raw_runs.size(); ++i) {
            if (!live_run[i]) {
                dropped_bytes += raw_runs[i].size();
                continue;
            }
            rmap[i] = static_cast<uint32_t>(kept_runs.size());
            kept_runs.push_back(std::move(raw_runs[i]));
        }
        const size_t dropped_runs = raw_runs.size() - kept_runs.size();
        for (auto& bs : samples) {
            if (bs.pcm_offset < rmap.size()) bs.pcm_offset = rmap[bs.pcm_offset];
        }
        for (auto& ri : sample_raw_index) {
            if (ri < rmap.size()) ri = rmap[ri];
        }
        raw_runs.swap(kept_runs);
        if (dropped_samples || dropped_runs) {
            std::printf("  orphans dropped: samples %u, runs %u, %.2f MB of PCM\n", (unsigned)dropped_samples, (unsigned)dropped_runs,
                        dropped_bytes / 1048576.0);
        }
    }

    // --- Сжатие прогонов ---
    //
    // Модель строится по всему банку и одна на всех: своя модель на каждый
    // прогон означала бы хранить таблицу при каждом сэмпле. Сами прогоны
    // кодируются независимо: подкачка на плате идёт по одному сэмплу, и
    // распаковывать соседей ради одного незачем. Замер показал, что
    // независимость почти ничего не стоит.
    BankModel model{};
    {
        std::vector<std::vector<uint64_t>> counts(kModelContexts, std::vector<uint64_t>(kModelSymbols, 0));
        for (const auto& run : raw_runs) {
            uint8_t prev = 0;
            for (uint8_t byte : run) {
                ++counts[model_context(prev)][byte];
                prev = byte;
            }
        }
        // Нормировка к kModelStates: встретившийся символ - не меньше
        // единицы, невстретившийся - ноль (его и кодировать не придётся).
        // Лишнее снимается с самых частых, недостача отдаётся самому частому.
        for (uint32_t c = 0; c < kModelContexts; ++c) {
            uint64_t total = 0;
            for (uint32_t s = 0; s < kModelSymbols; ++s)
                total += counts[c][s];
            if (total == 0) {
                model.freq[c][0] = kModelStates;
                continue;
            }
            uint32_t sum = 0;
            for (uint32_t s = 0; s < kModelSymbols; ++s) {
                uint32_t f = 0;
                if (counts[c][s]) f = std::max<uint32_t>(1, static_cast<uint32_t>((counts[c][s] * kModelStates + total / 2) / total));
                model.freq[c][s]  = static_cast<uint16_t>(f);
                sum              += f;
            }
            auto most = [&]() {
                uint32_t best = 0;
                for (uint32_t s = 1; s < kModelSymbols; ++s)
                    if (model.freq[c][s] > model.freq[c][best]) best = s;
                return best;
            };
            while (sum > kModelStates) {
                --model.freq[c][most()];
                --sum;
            }
            model.freq[c][most()] = static_cast<uint16_t>(model.freq[c][most()] + (kModelStates - sum));
        }
        if (!model_valid(model)) {
            std::fprintf(stderr, "ERROR: the compression model did not match\n");
            return 1;
        }
    }
    auto enc_table = std::make_unique<BankEncodeTable>();
    auto dec_table = std::make_unique<BankDecodeTable>();
    bank_build_encode_table(model, *enc_table);
    bank_build_decode_table(model, *dec_table);

    // Сжимаем и тут же проверяем распаковкой. Кодировщик и декодер лежат в
    // одном заголовке, чтобы не разойтись, но проверка всё равно
    // обязательна: разойдясь, они молча превратят банк в шум.
    std::vector<uint32_t> run_offset(raw_runs.size(), 0), run_packed(raw_runs.size(), 0);
    {
        std::vector<uint8_t> packed, check;
        std::vector<uint32_t> scratch;
        for (size_t i = 0; i < raw_runs.size(); ++i) {
            const auto& raw = raw_runs[i];
            run_offset[i]   = static_cast<uint32_t>(pcm.size());
            if (!pack_pcm) {
                run_packed[i] = static_cast<uint32_t>(raw.size());
                pcm.insert(pcm.end(), raw.begin(), raw.end());
                continue;
            }
            packed.assign(raw.size() * 2 + 8, 0);
            scratch.assign(raw.size(), 0);
            const uint32_t n = bank_compress(*enc_table, raw.data(), static_cast<uint32_t>(raw.size()), packed.data(), scratch.data());
            if (n == 0) {
                std::fprintf(stderr, "ERROR: run %zu: the symbol is missing from the model\n", i);
                return 1;
            }
            check.assign(raw.size(), 0);
            bank_decompress(*dec_table, packed.data(), n, check.data(), static_cast<uint32_t>(raw.size()));
            if (std::memcmp(check.data(), raw.data(), raw.size()) != 0) {
                std::fprintf(stderr, "ERROR: run %zu did not match when checked by decoding\n", i);
                return 1;
            }
            run_packed[i] = n;
            pcm.insert(pcm.end(), packed.begin(), packed.begin() + n);
        }
        // Декодер читает биты с запасом за конец прогона.
        pcm.insert(pcm.end(), kPcmTailBytes, 0);
    }

    // Записи сэмплов держали индекс прогона; ставим настоящие смещения.
    for (auto& bs : samples) {
        const uint32_t idx  = bs.pcm_offset;
        bs.pcm_offset       = run_offset[idx];
        bs.pcm_packed_bytes = run_packed[idx];
        if (pack_pcm) bs.flags |= kSamplePackedBit;
    }

    // Откаты: несуществующая вариация GS - в ту же программу банка 0, а если
    // нет и её - в рояль. Разрешаются здесь, чтобы на плате не было ни одной
    // развилки.
    //
    // Ударные (банк 128) откатываются только на стандартный набор 128:0 и
    // никогда на мелодическую программу.
    //
    // Иначе выходит бессмыслица: в Nirvana - All Apologies.mid десятый канал
    // просит набор 117, которого в SGM нет, и прежний откат подставлял
    // мелодическую программу 117 - Melodic Tom. Один и тот же том играл все
    // ноты набора, транспонируясь по клавишам: вместо хай-хэтов и тарелок
    // глухие удары, на слух "пропал инструмент с высокими частотами". Замер:
    // у нас все ноты набора выходили в -21..-22 дБ ровным рядом, у
    // эталонного синтезатора разброс -30..-48, а на 16 кГц мы отставали на
    // 26 дБ.
    uint32_t filled = 0, fallback = 0;
    for (uint32_t bank = 0; bank < kBankCount; ++bank) {
        for (uint32_t prog = 0; prog < kProgramCount; ++prog) {
            BankPreset& slot = presets[bank * kProgramCount + prog];
            if (preset_layer_count(slot)) {
                ++filled;
                continue;
            }
            const bool drums         = bank == 128;
            const BankPreset melodic = drums ? presets[128 * kProgramCount] : presets[prog];
            const BankPreset piano   = drums ? presets[128 * kProgramCount] : presets[0];
            const BankPreset src     = preset_layer_count(melodic) ? melodic : piano;
            if (!preset_layer_count(src)) continue; // у ударных нет даже набора 0 - оставляем пусто
            slot.first_layer           = src.first_layer;
            slot.layer_count_and_flags = static_cast<uint16_t>(preset_layer_count(src) | kPresetFallbackBit);
            ++fallback;
        }
    }

    // --- Запись ---
    BankHeader h{};
    h.magic       = kMagic;
    h.version     = kVersion;
    h.rate_cap_hz = static_cast<uint16_t>(std::min<uint32_t>(rate_cap, 65535));
    std::snprintf(h.name, sizeof(h.name), "%s", sf2.name.c_str());
    h.layer_count      = static_cast<uint16_t>(layers.size());
    h.instrument_count = static_cast<uint16_t>(instruments.size());
    h.envelope_count   = static_cast<uint16_t>(envelopes.size());
    h.sample_count     = static_cast<uint16_t>(samples.size());
    h.keymap_count     = static_cast<uint32_t>(keymap.size());

    uint32_t off = sizeof(BankHeader);
    auto place   = [&](uint32_t& field, size_t bytes) {
        field  = off;
        off   += static_cast<uint32_t>(bytes);
    };
    place(h.presets_offset, presets.size() * sizeof(BankPreset));
    place(h.layers_offset, layers.size() * sizeof(BankLayer));
    place(h.instruments_offset, instruments.size() * sizeof(BankInstrument));
    place(h.envelopes_offset, envelopes.size() * sizeof(BankEnvelope));
    place(h.keymap_offset, keymap.size() * sizeof(BankKeymapRange));
    place(h.samples_offset, samples.size() * sizeof(BankSample));
    place(h.model_offset, sizeof(BankModel));
    h.pcm_offset  = off;
    h.pcm_bytes   = static_cast<uint32_t>(pcm.size());
    h.total_bytes = off + h.pcm_bytes;

    std::vector<uint8_t> blob(h.total_bytes, 0);
    auto put = [&](uint32_t at, const void* src, size_t bytes) {
        if (bytes) std::memcpy(blob.data() + at, src, bytes);
    };
    put(h.presets_offset, presets.data(), presets.size() * sizeof(BankPreset));
    put(h.layers_offset, layers.data(), layers.size() * sizeof(BankLayer));
    put(h.instruments_offset, instruments.data(), instruments.size() * sizeof(BankInstrument));
    put(h.envelopes_offset, envelopes.data(), envelopes.size() * sizeof(BankEnvelope));
    put(h.keymap_offset, keymap.data(), keymap.size() * sizeof(BankKeymapRange));
    put(h.samples_offset, samples.data(), samples.size() * sizeof(BankSample));
    put(h.model_offset, &model, sizeof(BankModel));
    put(h.pcm_offset, pcm.data(), pcm.size());

    h.table_crc32 = bank_crc(blob.data() + sizeof(BankHeader), h.pcm_offset - static_cast<uint32_t>(sizeof(BankHeader)));
    h.pcm_crc32   = bank_crc(blob.data() + h.pcm_offset, h.pcm_bytes);
    std::memcpy(blob.data(), &h, sizeof(BankHeader));

    FILE* fo = std::fopen(out_path.c_str(), "wb");
    if (!fo) {
        std::fprintf(stderr, "cannot create %s\n", out_path.c_str());
        return 1;
    }
    std::fwrite(blob.data(), 1, blob.size(), fo);
    std::fclose(fo);

    // --- Сквозная проверка настоящей читалкой ---
    //
    // Проверка внутри сжатия сверяет кодировщик с декодером, но ничего не
    // говорит о заголовке, смещениях таблиц и раскладке по страницам PSRAM.
    // Ошибка там дала бы не отказ, а шум, и искать её пришлось бы на слух.
    // Поэтому только что записанный файл открывается настоящим bank_open, и
    // сэмплы прогоняются через настоящий bank_make_resident со сверкой байтов
    // в цепочке страниц с исходными.
    {
        Bank rb;
        const char* rerr = nullptr;
        if (!bank_open(blob.data(), static_cast<uint32_t>(blob.size()), rb, &rerr)) {
            std::fprintf(stderr, "ERROR: the written bank does not open: %s\n", rerr ? rerr : "?");
            return 1;
        }
        soundsinth::memory::PsramStore store;
        soundsinth::memory::psram_create(store);
        soundsinth::memory::psram_reset_track(store);

        uint32_t checked = 0, bad = 0;
        // Каждый сэмпл гонять незачем - их тысячи, а проверяется механизм.
        // Берётся каждый семнадцатый: попадают и разные размеры, и разные
        // кодеки, и с чекпоинтами, и без.
        for (uint32_t i = 0; i < samples.size(); i += 17) {
            const BankSample& bs = samples[i];
            uint16_t cp_page     = soundsinth::memory::kPageChainEnd;
            soundsinth::memory::psram_reset_track(store);
            const uint16_t first = bank_make_resident(rb, dec_table.get(), static_cast<uint16_t>(i), store, &cp_page);
            if (first == soundsinth::memory::kPageChainEnd) {
                ++bad;
                continue;
            }

            // Собираем цепочку обратно в плоский буфер и сверяем с сырьём.
            std::vector<uint8_t> got(bs.pcm_bytes);
            uint32_t done = 0;
            uint16_t page = first;
            while (done < bs.pcm_bytes && page != soundsinth::memory::kPageChainEnd) {
                const uint32_t chunk = std::min<uint32_t>(bs.pcm_bytes - done, soundsinth::memory::kPsramPageBytes);
                std::memcpy(got.data() + done, soundsinth::memory::psram_page_ptr(store, page), chunk);
                done += chunk;
                page  = soundsinth::memory::psram_page_next(store, page);
            }
            const std::vector<uint8_t>& want = raw_runs[sample_raw_index[i]];
            if (done != bs.pcm_bytes || got != want) ++bad;
            ++checked;
        }
        std::printf("  end-to-end check with the reader: samples %u, mismatches %u\n", checked, bad);
        if (bad) {
            std::fprintf(stderr, "ERROR: the bank reads back different from what was written\n");
            return 1;
        }
    }

    std::printf("\nbaked:\n");
    std::printf("  layers %zu, instruments %zu, envelopes %zu, keymap ranges %zu, samples %zu\n", layers.size(), instruments.size(), envelopes.size(),
                keymap.size(), samples.size());
    int multi = 0, multi_max = 0;
    for (const auto& kv : runs_per_sample) {
        if (kv.second > 1) ++multi;
        if (kv.second > multi_max) multi_max = kv.second;
    }
    std::printf("  PCM runs: unique %zu, keys %zu, resampled %u, reused %u\n", pcm_by_hash.size(), pcm_runs.size(), stat_resampled, stat_pcm_shared);
    std::printf("  PCM bytes: body %.2f MB, padding %.2f MB, checkpoints %.2f MB\n", stat_body / 1048576.0, stat_pad / 1048576.0, stat_cp / 1048576.0);
    std::printf("  samples baked more than once: %d (maximum %d)\n", multi, multi_max);
    std::printf("  instruments reused: %u, shared keymap runs: %u\n", stat_inst_shared, stat_keymap_shared);
    if (!decay_override.empty()) {
        std::printf("  release shortened: %u zones in %u programs\n", stat_decay_forced, (unsigned)decay_override.size());
    }
    if (fuse_stereo) {
        std::printf("  stereo pairs found %u: summed %u, loud half taken %u, left out of phase %u\n", stat_pairs, stat_fused_sum, stat_fused_pick,
                    stat_pairs_phase);
    }
    if (even_zones_db > 0.0) {
        std::printf("  zones levelled: %u at a %.1f dB threshold (adjacent pairs examined %u, largest step %.1f dB), new entries %u\n", stat_even_fixed,
                    even_zones_db, stat_even_seen, stat_even_maxdelta, stat_even_new);
    }
    if (merge_zones > 1) {
        std::printf("  zone decimation 1:%d - merged %u of %u (%.0f%%), instruments not allowed to merge %u\n", merge_zones, stat_zones_merged,
                    stat_zones_total, stat_zones_total ? 100.0 * stat_zones_merged / stat_zones_total : 0.0, stat_inst_unmergeable);
        std::printf("  stretch DOWN: average %.1f, maximum %d semitones\n", stat_stretch_n ? stat_stretch_sum / stat_stretch_n : 0.0, stat_stretch_max);
    }
    if (stat_seam) {
        std::printf("  loop seam fixed: %u runs\n", stat_seam);
    }
    if (stat_xfade) {
        std::printf("  loop seams cross-faded: %u\n", stat_xfade);
    }
    if (stat_gain_records) {
        std::printf("  sample entries: %u, of them drums with their own level %u\n", stat_gain_records, stat_drum_gain_hits);
    }
    if (stat_level_fixed) {
        std::printf("  level matched on merge: %u entries, %.2f dB on average, worst %.2f\n", stat_level_fixed, stat_level_db_sum / stat_level_fixed,
                    stat_level_db_max);
    }
    std::printf("  tables %.0f KB, PCM %.2f MB, total %.2f MB\n", h.pcm_offset / 1024.0, h.pcm_bytes / 1048576.0, h.total_bytes / 1048576.0);
    std::printf("  preset slots: own %u, by fallback %u\n", filled, fallback);
    if (stat_rate_auto) {
        std::printf("  rate from the spectrum: %u zones lowered, average assigned %.0f Hz (samples %u)\n", stat_rate_auto,
                    rate_auto_n ? double(rate_auto_sum) / rate_auto_n : 0.0, rate_auto_n);
    }
    if (stat_rate_low) {
        std::printf("  lowered to the bottom ceiling: %u samples (their top is below the threshold)\n", stat_rate_low);
    }
    if (stat_raw8) {
        std::printf("  encoded as Raw8 instead of Dpcm8 (by mistake): %u runs\n", stat_raw8);
    }
    if (stat_rate_keep) {
        std::printf("  kept at the native rate despite the ceiling: %u samples (their top is audible)\n", stat_rate_keep);
    }
    if (stat_layer_dup) {
        std::printf("  duplicate layers dropped: %u (the sample would sound twice)\n", stat_layer_dup);
    }
    if (stat_vel_fc) {
        std::printf("  brightness from velocity carried over in %u instruments (modulator initialFilterFc <- velocity)\n", stat_vel_fc);
    }
    if (stat_menv_vel) {
        std::printf("  filter envelope depth from velocity in %u zones (modulator modEnvToFilterFc <- velocity)\n", stat_menv_vel);
    }
    if (stat_filter_env) {
        std::printf("  filter envelope (modEnvToFilterFc) in %u instruments\n", stat_filter_env);
    }
    if (stat_filter_lost) {
        std::printf("  WARNING: cutoff above the engine scale in %u instruments, filter left open\n", stat_filter_lost);
    }
    if (stat_scale_tuning) {
        std::printf("  scaleTuning is not 100 in %u zones, expressed through note_offset\n", stat_scale_tuning);
    }
    if (stat_notes_over) {
        std::printf("  WARNING: notes above 127 (engine scale) in %u zones, clamped\n", stat_notes_over);
    }
    if (stat_vel_empty) {
        std::printf("  layers dropped for no velocity overlap between preset and zone: %u (they would never be picked)\n", stat_vel_empty);
    }
    return 0;
}
