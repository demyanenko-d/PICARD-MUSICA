// SPDX-License-Identifier: MIT
#include "core/formats/midi.h"
#include "core/formats/midi_convert.h"
#include "core/formats/midi_sysex.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

#include "platform/compiler.h"
#include "platform/memory.h"
#include "core/engine/engine_defs.h" // kSampleRateHz: длительность считается при разборе
#include "core/memory/sample_cache_catalog.h"
#include "core/codec/pattern_cell_codec.h"
#include "core/codec/pattern_packer.h"

namespace soundsinth::formats::midi {
namespace {

// Остальные имена common - из midi_convert.h.
using soundsinth::model::Pattern;

constexpr uint16_t kRowsPerPattern = 128; // как у OpenMPT при импорте
// Сколько миллисекунд доигрывать после последнего события файла: ровно
// столько рендерит эталонный синтезатор, длительности сходятся.
constexpr uint32_t kTailMs = 2000;
// Долей после последней ноты, дольше которых трек не тянется. Шестьдесят
// четыре доли - полминуты на 120 BPM: отпускание самой долгой ноты в них
// укладывается, а трёхчасовой хвост битого файла отсекается.
constexpr uint32_t kTailBeats = 64;
constexpr uint32_t kMaxTracks = 64;
// Потолок карты темпа. Держит сортировку вставками при разборе: по
// библиотеке медиана одна смена, 99-й процентиль 400, максимум 963
// (SWARS.MID). Смены сверх потолка теряются: файл до конца играет на темпе
// последней взятой.
constexpr uint32_t kMaxTempoChanges = 1024;

// Сетка: темп трекера = BPM * строк_на_долю * тиков_на_строку / 24, в
// пределах 32..255. Порядок - от мелкой к грубой: при равной ошибке выбора
// остаётся более мелкая.
// Сетки 32 и 16 строк на долю - для медленных файлов: при 25 BPM строка
// сетки 8/6 длится 0.3 с, и короткая нота держится втрое дольше.
// clang-format off
constexpr Grid kGrids[] = {
    {32, 6}, {32, 5}, {32, 4}, {32, 3},
    {16, 6}, {16, 5}, {16, 4}, {16, 3},
    {8, 6},  {8, 5},  {8, 4},  {8, 3},
    {4, 6},  {4, 5},  {4, 4},  {4, 3}, {4, 2},
};
// clang-format on

// Переменная длина SMF: до четырёх байт, старший бит - "есть продолжение".
uint32_t read_varint(const uint8_t* buf, uint32_t& pos, uint32_t end) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4 && pos < end; ++i) {
        const uint8_t b = buf[pos++];
        v               = (v << 7) | (b & 0x7fu);
        if (!(b & 0x80u)) break;
    }
    return v;
}

// Курсор по дорожке в сыром файле: событие разбирается в момент выборки, а
// не складывается массивом заранее. Так у загрузки и воспроизведения один
// источник событий, а не два, обязанных совпадать; и держать резидентно
// приходится файл (медиана 18.8 КБ), а не Event на каждое сообщение
// (42.5 КБ, у четверти файлов дороже самих паттернов).
struct TrackCursor {
    uint32_t start  = 0; // начало чанка дорожки, для возврата к началу трека
    uint32_t end    = 0;
    uint32_t pos    = 0;
    uint32_t tick   = 0; // абсолютный тик MIDI
    uint8_t running = 0; // running status: его заводят только сообщения канала
    bool has_head   = false;
    Event head{}; // ближайшее событие дорожки, тик уже со сдвигом lead
};

// Наблюдатель разбора. Загрузке нужно то, чего потоку не нужно вовсе:
// карта темпа, назначения ударных каналов по SysEx и края трека. Без него
// пришлось бы держать второй разборщик того же формата, а два разборщика
// обязаны совпадать в правилах пропуска - и когда-нибудь разойдутся.
struct ScanHooks {
    void* user                                                   = nullptr;
    void (*tempo)(void* user, uint32_t tick, uint32_t us)        = nullptr;
    void (*sysex)(void* user, const uint8_t* data, uint32_t len) = nullptr;
    // Все сообщения канала, и отобранные, и нет: длина трека считается по
    // любому из них.
    void (*channel)(void* user, uint32_t tick, uint8_t kind, uint8_t d2, bool wanted) = nullptr;
};

// Слияние дорожек по тику поверх курсоров. В поток идёт только то, что
// влияет на звук; остальное видит наблюдатель.
struct EventSource {
    const uint8_t* file    = nullptr;
    TrackCursor* cur       = nullptr;
    uint16_t track_count   = 0;
    uint32_t lead          = 0;           // срезанная пауза до первой ноты
    uint32_t tail_cap      = 0xffffffffu; // за хвостом последней ноты дорожки нет
    const ScanHooks* hooks = nullptr;     // только на разборе заголовка

    // Разобрать дорожку до ближайшего события, которое влияет на звук.
    void advance(TrackCursor& c) {
        c.has_head = false;
        while (c.pos < c.end) {
            // Тик дорожки не убывает: перенос uint32 (битый файл, дельты под
            // 0x0fffffff) обрывает дорожку - на неубывании стоят слияние
            // дорожек и выбор сетки.
            const uint32_t delta = read_varint(file, c.pos, c.end);
            if (c.tick + delta < c.tick) return;
            c.tick += delta;
            if (c.tick > tail_cap) return;
            if (c.pos >= c.end) return;
            uint8_t status = file[c.pos];
            if (status & 0x80u) {
                ++c.pos;
                if (status < 0xf0) c.running = status;
            } else {
                status = c.running;
            }
            if (status == 0xff) {
                if (c.pos >= c.end) return;
                const uint8_t meta  = file[c.pos++];
                const uint32_t mlen = read_varint(file, c.pos, c.end);
                if (meta == 0x51 && mlen == 3 && c.pos + 2 < c.end && hooks != nullptr && hooks->tempo != nullptr) {
                    const uint32_t us = (static_cast<uint32_t>(file[c.pos]) << 16) | (static_cast<uint32_t>(file[c.pos + 1]) << 8) | file[c.pos + 2];
                    hooks->tempo(hooks->user, c.tick, us);
                }
                c.pos += mlen;
                continue;
            }
            if (status == 0xf0 || status == 0xf7) {
                const uint32_t slen  = read_varint(file, c.pos, c.end);
                const uint8_t* body  = file + c.pos;
                const bool whole     = c.pos + slen <= c.end;
                c.pos               += slen;
                if (!whole) continue;
                if (hooks != nullptr && hooks->sysex != nullptr) hooks->sysex(hooks->user, body, slen);
                uint8_t master = 0;
                if (sysex_master_volume(body, slen, master)) {
                    c.head     = Event{c.tick > lead ? c.tick - lead : 0, kMasterVolumeStatus, master, 0, 0};
                    c.has_head = true;
                    return;
                }
                continue;
            }
            const uint8_t kind = status >> 4;
            if (c.pos >= c.end) return;
            const uint8_t d1 = file[c.pos++] & 0x7fu;
            uint8_t d2       = 0;
            if (kind != 0xc && kind != 0xd) {
                if (c.pos >= c.end) return;
                d2 = file[c.pos++] & 0x7fu;
            }
            // Список обязан совпадать с разбором у конвертера: событие,
            // которого здесь нет, до него не доедет, и обработчик станет
            // мёртвым кодом молча.
            const bool wanted = kind == 0x9 || kind == 0x8 || kind == 0xc || kind == 0xd || kind == 0xe ||
                                (kind == 0xb && (d1 == 0 || d1 == 1 || d1 == 6 || d1 == 7 || d1 == 10 || d1 == 11 || d1 == 64 || d1 == 100 || d1 == 101 ||
                                                 d1 == 120 || d1 == 121 || d1 == 123 || d1 == 5 || d1 == 65 || d1 == 91 || d1 == 98 || d1 == 99));
            if (hooks != nullptr && hooks->channel != nullptr) {
                hooks->channel(hooks->user, c.tick, kind, d2, wanted);
            }
            if (wanted) {
                c.head     = Event{c.tick > lead ? c.tick - lead : 0, status, d1, d2, 0};
                c.has_head = true;
                return;
            }
        }
    }

    // К началу трека: каждая дорожка - на своё начало.
    void rewind() {
        for (uint32_t t = 0; t < track_count; ++t) {
            TrackCursor& c = cur[t];
            c.pos          = c.start;
            c.tick         = 0;
            c.running      = 0;
            advance(c);
        }
    }

    // Ближайшее событие по тику; при равенстве - по номеру дорожки, как у
    // OpenMPT.
    bool pull(Event& out_ev) {
        uint32_t best = 0xffffffffu, best_tick = 0xffffffffu;
        for (uint32_t t = 0; t < track_count; ++t) {
            if (cur[t].has_head && cur[t].head.tick < best_tick) {
                best_tick = cur[t].head.tick;
                best      = t;
            }
        }
        if (best == 0xffffffffu) return false;
        out_ev = cur[best].head;
        advance(cur[best]);
        return true;
    }
};

// Состояние потокового чтения последнего загруженного .mid: файл, курсоры,
// карта темпа и конвертер живут в PSRAM и переживают загрузку, здесь только
// указатель - как у s_stats. Трек в памяти один, второго не бывает.
struct Replay;
Replay* s_replay = nullptr;

bool s_trim_lead_silence = true;
bool s_trim_tail_silence = true;
Grid s_forced_grid       = {0, 0};
// Пишет только load (Core1 на плате), читают после неё.
LoadStats s_stats;
// Трасса для проверок на ПК; на плате nullptr.
ConvertTrace* s_trace = nullptr;

// Шаг "события -> строка": слияние дорожек по тику, карта темпа и общая
// громкость. Отдельно от load(), потому что тем же шагом идёт потоковый
// путь, где паттернов нет и строка отдаётся движку по одной. Состояние всё
// здесь: у потокового пути оно переживает загрузку.
struct RowStepper {
    Converter* cv    = nullptr;
    EventSource* src = nullptr;

    const uint32_t* tempo_ticks = nullptr;
    const uint32_t* tempo_us    = nullptr;
    uint32_t tempo_count        = 0;
    uint32_t tempo_cursor       = 0;
    uint64_t tempo_row          = 0; // строка следующей смены темпа, в 64 битах

    uint16_t division = 96;
    Grid grid{};
    uint32_t kmul = 0; // строк_на_долю * тиков_на_строку

    Event e{};
    bool have_event    = false;
    uint32_t ev_mt     = 0; // тик модуля события
    uint32_t ev_row    = 0; // его строка
    uint8_t cur_global = 0;

    // Темп трекера из BPM - та же формула, что при выборе сетки.
    uint8_t tempo_from_bpm(double bpm) const {
        int32_t t = static_cast<int32_t>(bpm * grid.rows_per_beat * grid.ticks_per_row / 24.0 + 0.5);
        if (t < 32) t = 32;
        if (t > 255) t = 255;
        return static_cast<uint8_t>(t);
    }

    void fetch_event() {
        have_event = src->pull(e);
        if (!have_event) return;
        // Тик модуля и строка - раз при выборке, в 32 битах:
        // (tick / division) * K + (tick % division) * K / division - то же, что
        // tick * K / division, без 64-битного деления: у M33 его нет, это вызов
        // библиотеки на каждом событии.
        ev_mt  = (e.tick / division) * kmul + (e.tick % division) * kmul / division;
        ev_row = ev_mt / grid.ticks_per_row;
    }

    void aim_tempo() {
        if (tempo_cursor < tempo_count) {
            tempo_row = static_cast<uint64_t>(tempo_ticks[tempo_cursor]) * kmul / division / grid.ticks_per_row;
        }
    }

    // К началу трека: курсоры дорожек, первое событие и первая смена темпа.
    void rewind(uint8_t global_volume) {
        src->rewind();
        tempo_cursor = 0;
        cur_global   = global_volume;
        fetch_event();
        aim_tempo();
    }

    // Собрать строку row_abs в cv->cells.
    void step(uint32_t row_abs) {
        cv->begin_row();

        while (have_event) {
            if (ev_row > row_abs) break;
            const uint8_t delay = static_cast<uint8_t>(ev_mt - ev_row * grid.ticks_per_row);
            if (s_trace && s_trace->event) s_trace->event(s_trace->user, e.status, e.d1, e.d2, delay);
            cv->handle(e, ev_mt, delay);
            fetch_event();
        }

        // Смена темпа - в свободную колонку эффекта этой строки; с 64 каналами
        // свободная почти всегда есть, а смены темпа редки.
        while (tempo_cursor < tempo_count && tempo_row <= row_abs) {
            const uint8_t want = tempo_from_bpm(6e7 / tempo_us[tempo_cursor]);
            if (want != cv->cur_tempo) {
                if (!cv->write_row_effect(Effect::SetTempo, want)) { // все заняты - на следующей строке
                    ++s_stats.tempo_deferred;
                    break;
                }
                cv->set_tempo(want);
                if (s_trace) ++s_trace->row_effects;
            }
            ++tempo_cursor;
            aim_tempo();
        }

        // Общая громкость - после темпа и по тому же правилу: первая свободная
        // колонка эффекта, иначе на следующей строке.
        if (cv->want_global != cur_global && cv->write_row_effect(Effect::SetGlobalVolume, cv->want_global)) {
            cur_global = cv->want_global;
            if (s_trace) ++s_trace->row_effects;
        }

        cv->finish_row(static_cast<uint64_t>(row_abs) * grid.ticks_per_row);
        if (s_trace && s_trace->row_done) s_trace->row_done(s_trace->user, cv->cells);
    }
};

// Всё, что нужно, чтобы выдать строки трека заново, не перечитывая файл с
// карты. Лежит в PSRAM рядом с файлом.
struct Replay {
    EventSource events;
    RowStepper step;
    Converter* cv          = nullptr;
    uint32_t row           = 0;
    uint32_t total_rows    = 0;
    uint32_t held_bytes    = 0;
    uint32_t pending_bytes = 0;
    uint8_t start_tempo    = 0;
    uint8_t global_volume  = 0;
    // Длительность трека и кадр начала каждого паттерна: посчитаны при
    // разборе, проходом секвенсора их больше не взять.
    uint32_t total_frames             = 0;
    const uint32_t* frames_at_pattern = nullptr;
    uint32_t pattern_count            = 0;
};

// Счётчики повтора: конвертер пишет потери раскладки на каждом проходе, а
// повторов за трек сколько угодно - без отдельного места числа загрузки
// удваивались бы с каждой перемоткой.
LoadStats s_replay_stats;

// К началу трека. Номера инструментов и записей сэмплов сохраняются: они
// уже запечены в песню, сбросить их значило бы получить другую.
void rewind_replay(Replay& r) {
    s_replay_stats = LoadStats{};
    r.cv->stats    = &s_replay_stats;
    r.cv->reset_for_replay(r.held_bytes, r.pending_bytes, r.start_tempo, r.global_volume);
    r.step.rewind(r.global_volume);
    r.row = 0;
}

// Строка для секвенсора. Он ведёт позицию сам и просит строки по порядку;
// назад - только перемоткой, и тогда трек прогоняется с начала: состояние
// каналов иначе не восстановить, прыжок дал бы чужой тембр и высоту.
void fetch_row(void* /*user*/, uint16_t pattern_idx, uint16_t row, soundsinth::model::PatternCell* out, uint8_t channels) {
    if (s_replay == nullptr) return;
    Replay& r           = *s_replay;
    const uint32_t want = static_cast<uint32_t>(pattern_idx) * kRowsPerPattern + row;
    if (want < r.row) rewind_replay(r);
    while (r.row <= want && r.row < r.total_rows) {
        r.step.step(r.row++);
    }
    if (want >= r.total_rows) return; // за концом трека строк нет
    const uint8_t n = channels < kMaxChannels ? channels : kMaxChannels;
    for (uint8_t c = 0; c < n; ++c) {
        out[c] = r.cv->cells[c];
    }
}

uint32_t rd32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t rd16be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]);
}

// Сетка под темп - по времени, а не по крайним темпам: крайний темп может
// держаться мгновение. Ошибка сетки - сумма по отрезкам карты: вес отрезка
// на (d - 1), d - отношение требуемого темпа к достижимому, большего к
// меньшему. При равной ошибке остаётся первая, более мелкая сетка.
Grid choose_grid(EventSource& src, const uint32_t* tempo_ticks, const uint32_t* tempo_us, uint32_t tempo_count, uint32_t max_tick) {
    Grid grid = kGrids[0];
    // Снаружи отрезки темпа, внутри - вес курсорами дорожек и все сетки разом:
    // карта упорядочена, тики дорожек не убывают, поэтому хватает одного
    // прохода по событиям.
    constexpr uint32_t kGridCount = sizeof(kGrids) / sizeof(kGrids[0]);
    double errs[kGridCount]       = {};
    // Файл без смены темпа - один отрезок в 120 BPM, умолчание SMF. Без него
    // ошибки всех сеток нулевые и выигрывает первая (32/6): темп 960 упирается
    // в 255, трек идёт вчетверо медленнее.
    const uint32_t seg_count = tempo_count ? tempo_count : 1u;
    auto seg_tick            = [&](uint32_t i) -> uint32_t { return tempo_count ? tempo_ticks[i] : 0u; };
    auto seg_us              = [&](uint32_t i) -> uint32_t { return tempo_count ? tempo_us[i] : 500000u; };
    // Один проход по слитому потоку: тики не убывают, поэтому отрезки идут
    // подряд и хватает одного просмотренного вперёд события.
    src.rewind();
    Event ne{};
    bool have = src.pull(ne);
    for (uint32_t i = 0; i < seg_count; ++i) {
        const uint64_t until = (i + 1 < seg_count) ? seg_tick(i + 1) : max_tick;
        uint32_t weight      = 0;
        while (have && ne.tick < until) {
            if ((ne.status >> 4) == 0x9 && ne.d2 != 0 && ne.tick >= seg_tick(i)) ++weight;
            have = src.pull(ne);
        }
        for (uint32_t gi = 0; gi < kGridCount; ++gi) {
            const Grid& g = kGrids[gi];
            // Вес отрезка - число нот в нём плюс длительность в тиках MIDI / 64:
            // одни ноты дают нулевой вес пустому отрезку, одно время - перевес
            // короткому вступлению.
            // Последний отрезок бывает отрицательным: темп стоит после последнего
            // события канала (мета в max_tick не входят). Такой отрезок пуст, иначе
            // беззнаковая разность заворачивается, и он один выбирает сетку.
            const uint64_t length = until > seg_tick(i) ? until - seg_tick(i) : 0;
            const double span     = static_cast<double>(weight) + static_cast<double>(length) / 64.0;
            const double bpm      = 6e7 / seg_us(i);
            const double want     = bpm * g.rows_per_beat * g.ticks_per_row / 24.0;
            // Округление до целого - часть ошибки: темп движка целый, и сетка, дающая
            // 66.67, врёт на полпроцента весь трек, а сетка с ровными 50 не врёт.
            double got = static_cast<double>(static_cast<int32_t>(want + 0.5));
            if (got < 32.0) got = 32.0;
            if (got > 255.0) got = 255.0;
            if (want > 0.0) {
                const double ratio  = want / got;
                const double d      = ratio > 1.0 ? ratio : 1.0 / ratio;
                errs[gi]           += span * (d - 1.0);
            }
        }
    }
    double best_err = 1e300;
    for (uint32_t gi = 0; gi < kGridCount; ++gi) {
        if (errs[gi] < best_err - 1e-9) {
            best_err = errs[gi];
            grid     = kGrids[gi];
        }
    }
    if (s_forced_grid.rows_per_beat && s_forced_grid.ticks_per_row) grid = s_forced_grid;
    return grid;
}

} // namespace

void set_trim_lead_silence(bool on) {
    s_trim_lead_silence = on;
}

void set_trim_tail_silence(bool on) {
    s_trim_tail_silence = on;
}

void set_forced_grid(uint8_t rows_per_beat, uint8_t ticks_per_row) {
    s_forced_grid = Grid{rows_per_beat, ticks_per_row};
}

void set_convert_trace(ConvertTrace* trace) {
    s_trace = trace;
}

const LoadStats& last_load_stats() {
    return s_stats;
}

uint32_t replay_row_count() {
    return s_replay != nullptr ? s_replay->total_rows : 0u;
}

void replay_rewind() {
    if (s_replay != nullptr) rewind_replay(*s_replay);
}

void replay_row_at(uint32_t abs_row, soundsinth::model::PatternCell* out, uint8_t channels) {
    if (s_replay == nullptr) return;
    Replay& r = *s_replay;
    if (abs_row < r.row) rewind_replay(r);
    while (r.row <= abs_row && r.row < r.total_rows) {
        r.step.step(r.row++);
    }
    if (abs_row >= r.total_rows) return;
    const uint8_t n = channels < kMaxChannels ? channels : kMaxChannels;
    for (uint8_t c = 0; c < n; ++c) {
        out[c] = r.cv->cells[c];
    }
}

uint32_t last_total_frames() {
    return s_replay != nullptr ? s_replay->total_frames : 0u;
}

uint16_t order_positions_in_frames(uint32_t frames) {
    if (s_replay == nullptr || s_replay->frames_at_pattern == nullptr) return 1;
    const Replay& r = *s_replay;
    uint16_t n      = 1;
    while (n < r.pattern_count && r.frames_at_pattern[n] < frames) {
        ++n;
    }
    return n;
}

const soundsinth::model::PatternCell* replay_next_row() {
    if (s_replay == nullptr) return nullptr;
    Replay& r = *s_replay;
    if (r.row >= r.total_rows) return nullptr;
    r.step.step(r.row++);
    return r.cv->cells;
}

bool sniff(const uint8_t* head, uint32_t bytes) {
    return bytes >= 4 && std::memcmp(head, "MThd", 4) == 0;
}

bool load(formats::ByteSource src, uint32_t file_length, memory::TrackMemory& mem, const bank::Bank& bank, Song& out, const char** error_out,
          bool metadata_only) {
    auto fail = [&](const char* why) {
        if (error_out) *error_out = why;
        return false;
    };
    if (error_out) *error_out = nullptr;
    s_stats = LoadStats{};
    // Прошлый трек снесён вместе с памятью: указатель в неё держать нельзя.
    s_replay = nullptr;
    if (!bank.valid()) return fail("instrument bank not loaded");
    if (file_length < 14) return fail("file shorter than the MThd header");

    // Файл целиком кладётся в PSRAM: разбор снуёт по дорожкам, а через шину
    // это были бы сотни мелких перемоток. Резидентно, а не во временное:
    // события читаются курсором в момент выборки, строки делаются по ходу
    // игры.
    uint8_t* file = memory::psram_resident_new<uint8_t>(mem.psram, file_length);
    if (!file) return fail("the .mid file does not fit PSRAM");
    if (!src.seek(src.self, 0)) return fail("seek of .mid to the start failed");
    if (src.read(src.self, file, file_length) != file_length) return fail("the .mid file was not read to the end");

    if (!sniff(file, file_length)) return fail("not a Standard MIDI File");
    const uint32_t header_len  = rd32be(file + 4);
    const uint16_t format      = rd16be(file + 8);
    const uint16_t track_count = rd16be(file + 10);
    uint16_t division          = rd16be(file + 12);
    if (format > 1) return fail("SMF format 2 is not supported");
    if (division & 0x8000u) {
        const uint32_t frames = 256u - (division >> 8), sub = division & 0xffu;
        division = static_cast<uint16_t>(frames * sub / 2);
    }
    if (division == 0) division = 96;
    if (track_count == 0 || track_count > kMaxTracks) return fail("no tracks or too many of them");

    // --- Разбор дорожек ---

    // Буфер загрузчика .mid не нужен вовсе: всё, что ему было нужно, живёт в
    // PSRAM и переживает загрузку. Размер kLoaderScratchBytes задаёт теперь
    // только план фоновой догрузки сэмплов.
    uint32_t* tempo_ticks = nullptr;
    uint32_t* tempo_us    = nullptr;
    // Курсоры дорожек: границы чанка и позиция разбора. Событий массивом нет
    // вовсе - они читаются из файла в момент выборки. Резидентно, вместе с
    // файлом: по ним трек читается и во время игры.
    TrackCursor* tracks = memory::psram_resident_new<TrackCursor>(mem.psram, kMaxTracks);
    if (!tracks) return fail(".mid track cursors do not fit PSRAM");
    for (uint32_t t = 0; t < kMaxTracks; ++t) {
        tracks[t] = TrackCursor{};
    }
    uint32_t tempo_count = 0;
    uint32_t max_tick    = 0;

    // Ударные каналы: по умолчанию десятый (индекс 9), как в GM; GS и XG
    // объявляют ударным любой - без этого канал сыграет мелодическую программу.
    // Назначение статическое на весь трек: сообщение назначения есть у 1.14%
    // файлов архива, и лишь 2.3% из них меняют его после первой ноты.
    bool drum_channel[16] = {};
    drum_channel[9]       = true;

    // Один проход по всем дорожкам: границы чанков, карта темпа, ударные
    // каналы по SysEx и края трека. События в нём только считаются для
    // отчёта - читает их потом курсор, из того же файла.
    // Ведущая пауза срезается: трек начинается с первой ноты. Она ищется в
    // проходе 0, события кладутся со сдвигом в проходе 1. Настройки и смены
    // темпа до первой ноты не теряются: их тик прижимается к нулю, события
    // разных дорожек на нём идут по номеру дорожки. Карта темпа сдвигается
    // после сортировки по сырым тикам: при общем нулевом тике побеждать
    // должен последний темп перед нотой, а не порядок дорожек.
    uint32_t lead_tick = 0xffffffffu;
    uint32_t lead      = 0;
    // Хвост после последней ноты ограничен kTailBeats долями: у битого файла
    // за песней лежат выключения нот с испорченными дельтами, и трек тянулся
    // бы часами. Последняя нота ищется в проходе 0, дорожки обрываются по ней
    // в проходе 1: тики внутри дорожки не убывают, хватает выхода из цикла.
    uint32_t last_note_tick = 0;
    bool has_note           = false;
    uint32_t tail_cap       = 0xffffffffu;
    // Карта темпа под потолок сразу: он мал (1024 смены, 8 КБ), а второй
    // проход только ради точного размера стоил бы полного разбора файла.
    // Резидентно: смены темпа выписываются в строки по ходу игры.
    tempo_ticks = memory::psram_resident_new<uint32_t>(mem.psram, kMaxTempoChanges);
    tempo_us    = memory::psram_resident_new<uint32_t>(mem.psram, kMaxTempoChanges);
    if (!tempo_ticks || !tempo_us) return fail(".mid tempo map does not fit PSRAM");

    // Источник событий поверх курсоров: один и на разбор заголовка, и на
    // выбор сетки, и на сборку строк.
    EventSource events;
    events.file          = file;
    events.cur           = tracks;
    uint16_t chunk_count = 0;
    {
        uint32_t pos = 8 + header_len;
        for (uint16_t t = 0; t < track_count; ++t) {
            // Файл кончился раньше заявленных дорожек - играем те, что есть:
            // таких в архиве 0.3%, и они звучат. Чужой чанк среди дорожек -
            // отказ, как у OpenMPT: дальше по файлу дорожки уже не найти, а
            // молча потерять половину трека хуже, чем не играть его вовсе.
            if (pos + 8 > file_length) break;
            if (std::memcmp(file + pos, "MTrk", 4) != 0) return fail("foreign chunk among the .mid tracks");
            const uint32_t len   = rd32be(file + pos + 4);
            const uint32_t start = pos + 8;
            const uint32_t end   = start + len > file_length ? file_length : start + len;
            pos                  = start + len;

            // Границы чанка - курсору: разбирает дорожку он, здесь только
            // находятся чанки.
            tracks[t].start = start;
            tracks[t].end   = end;
            ++chunk_count;
        }
    }
    // Дорожек в заголовке бывает больше, чем чанков в файле: играем те, что
    // нашлись, чужие курсоры к разбору не допускаются.
    events.track_count = chunk_count;

    // Разбор - тем же курсором, что потом читает воспроизведение: второй
    // разборщик того же формата рано или поздно разошёлся бы с первым в
    // правилах пропуска, и заметить это было бы нечем.
    struct Scan {
        uint32_t* tempo_ticks;
        uint32_t* tempo_us;
        uint32_t tempo_count = 0;
        bool* drum_channel;
        uint32_t lead_tick      = 0xffffffffu;
        uint32_t last_note_tick = 0;
        bool has_note           = false;
        uint32_t max_tick       = 0;
        uint32_t events         = 0;
    } scan{tempo_ticks, tempo_us, 0, drum_channel};

    {
        ScanHooks hooks;
        hooks.user  = &scan;
        hooks.tempo = [](void* u, uint32_t tick, uint32_t us) {
            Scan& s = *static_cast<Scan*>(u);
            if (s.tempo_count >= kMaxTempoChanges) {
                ++s_stats.tempo_dropped;
                return;
            }
            // Нулевой темп битого файла - 120 BPM: дальше темп только делитель.
            s.tempo_ticks[s.tempo_count] = tick;
            s.tempo_us[s.tempo_count]    = us ? us : 500000u;
            ++s.tempo_count;
        };
        hooks.sysex   = [](void* u, const uint8_t* data, uint32_t len) { sysex_drum_channels(data, len, static_cast<Scan*>(u)->drum_channel); };
        hooks.channel = [](void* u, uint32_t tick, uint8_t kind, uint8_t d2, bool wanted) {
            Scan& s = *static_cast<Scan*>(u);
            if (wanted) {
                if (kind == 0x9 && d2 > 0) {
                    if (tick < s.lead_tick) s.lead_tick = tick;
                    if (tick > s.last_note_tick) s.last_note_tick = tick;
                    s.has_note = true;
                }
                ++s.events;
            }
            if (tick > s.max_tick) s.max_tick = tick;
        };

        events.hooks = &hooks;
        events.rewind();
        Event ev;
        while (events.pull(ev)) {
        }
        events.hooks = nullptr;
    }
    tempo_count          = scan.tempo_count;
    lead_tick            = scan.lead_tick;
    last_note_tick       = scan.last_note_tick;
    has_note             = scan.has_note;
    max_tick             = scan.max_tick;
    s_stats.events       = scan.events;
    s_stats.events_bytes = scan.events * static_cast<uint32_t>(sizeof(Event));

    if (s_trim_lead_silence && lead_tick != 0xffffffffu) lead = lead_tick;
    if (s_trim_tail_silence && has_note) {
        const uint32_t room = 0xffffffffu - last_note_tick;
        const uint32_t tail = kTailBeats * division;
        tail_cap            = tail < room ? last_note_tick + tail : 0xffffffffu;
    }
    // Проход 0 считал максимум по всем событиям, включая те, что проход 1
    // отсёк хвостом. Сетка выбирается по длине трека, поэтому граница нужна
    // и здесь.
    if (max_tick > tail_cap) max_tick = tail_cap;
    if (max_tick == 0) return fail("no events in the file");

    // Карта собрана по дорожкам подряд - упорядочить по тику, устойчиво (равный
    // тик - по номеру дорожки, как у OpenMPT). Иначе записи следующей дорожки
    // срабатывают пачкой SetTempo на одной строке. Вставками: дорожка даёт
    // упорядоченный кусок, у файла с одной дорожкой темпа сдвигов нет.
    for (uint32_t i = 1; i < tempo_count; ++i) {
        const uint32_t tick = tempo_ticks[i];
        const uint32_t us   = tempo_us[i];
        uint32_t j          = i;
        while (j > 0 && tempo_ticks[j - 1] > tick) {
            tempo_ticks[j] = tempo_ticks[j - 1];
            tempo_us[j]    = tempo_us[j - 1];
            --j;
        }
        tempo_ticks[j] = tick;
        tempo_us[j]    = us;
    }

    if (lead > 0) {
        for (uint32_t i = 0; i < tempo_count; ++i) {
            tempo_ticks[i] = tempo_ticks[i] > lead ? tempo_ticks[i] - lead : 0;
        }
        max_tick = max_tick > lead ? max_tick - lead : 0;
        if (max_tick == 0) return fail("no events in the file after the first note");
    }

    // --- Сетка под темп ---
    // Края трека найдены - теперь тот же источник отдаёт события уже со
    // сдвигом начала и обрывом хвоста.
    events.lead     = lead;
    events.tail_cap = tail_cap;

    const Grid grid               = choose_grid(events, tempo_ticks, tempo_us, tempo_count, max_tick);
    const uint32_t start_tempo_us = tempo_count ? tempo_us[0] : 500000u;
    const double start_bpm        = 6e7 / start_tempo_us;
    uint32_t tracker_tempo        = static_cast<uint32_t>(start_bpm * grid.rows_per_beat * grid.ticks_per_row / 24.0 + 0.5);
    if (tracker_tempo < soundsinth::model::kMinTempo) tracker_tempo = soundsinth::model::kMinTempo;
    if (tracker_tempo > 255) tracker_tempo = 255;

    // Тик модуля = тик MIDI * строк_на_долю * тиков_на_строку / PPQN.
    const uint64_t mod_ticks = (static_cast<uint64_t>(max_tick) * grid.rows_per_beat * grid.ticks_per_row + division - 1) / division;
    // Хвост после последнего события (kTailMs). Без него релиз огибающей и
    // хвост ревербератора срезаются на полном уровне, и в конце слышен щелчок.
    // Строки хвоста - по темпу в конце трека: на нём хвост и играется.
    const uint32_t end_tempo_us = tempo_count ? tempo_us[tempo_count - 1] : 500000u;
    const double end_bpm        = 6e7 / static_cast<double>(end_tempo_us);
    const uint32_t tail_rows    = static_cast<uint32_t>(kTailMs * end_bpm * grid.rows_per_beat / 60000.0 + 0.5);
    // Строки - в 64 битах и проверка до сужения: у битого файла (division
    // единицы, тики в сотни миллионов) частное за 2^32, усечённое оно
    // проходило проверку, и события за сужением ложились в начало трека.
    // После проверки строки, тики модуля и строки событий - в 32 битах.
    const uint64_t rows64 = mod_ticks / grid.ticks_per_row + 1 + tail_rows;
    if (rows64 > static_cast<uint64_t>(4096) * kRowsPerPattern) return fail("track too long for the grid");
    const uint32_t total_rows    = static_cast<uint32_t>(rows64);
    const uint32_t pattern_count = (total_rows + kRowsPerPattern - 1) / kRowsPerPattern;
    // Последний паттерн - ровно столько строк, сколько занято, а не 128:
    // иначе в конце трека до 127 строк тишины, и плата позже переходит к
    // следующему треку.
    auto rows_in = [&](uint32_t pat) -> uint16_t {
        const uint32_t rest = total_rows - pat * kRowsPerPattern;
        return static_cast<uint16_t>(rest < kRowsPerPattern ? rest : kRowsPerPattern);
    };
    if (pattern_count == 0 || pattern_count > 4096) return fail("track too long for the grid");

    init_song_header(out, grid, tracker_tempo);
    uint8_t vibrato_speed = vibrato_speed_at(tracker_tempo);
    // Тиков в секунду у движка 2*tempo/5, с округлением (у скорости вибрато и
    // скольжения - с отбрасыванием): для перевода огибающих и затухания из
    // миллисекунд банка.
    const uint32_t ticks_per_second_rounded = (2u * tracker_tempo + 2u) / 5u;

    // --- Служебные карты в PSRAM ---
    // Во временном PSRAM, не в SRAM (у GeneralUser GS около 26 КБ): инструмент
    // банка -> инструмент песни, сэмпл банка -> сэмпл песни, held [канал
    // MIDI][нота][слой] -> канал трекера (0xff - нет), pending. Заморозка зоны
    // паттернов отдаёт это место сэмплам.
    //
    // Инструмент песни - на инструмент банка, а не на слой: слои одного
    // инструмента банка различаются только полосой velocity для выбора, а
    // Instrument, огибающие и keymap у них одни.
    constexpr uint32_t kHeldBytes    = 16u * 128u * kMaxLayers;
    constexpr uint32_t kPendingBytes = 16u * 128u;
    uint16_t* song_inst_of           = memory::psram_temp_new<uint16_t>(mem.psram, bank.header->instrument_count);
    uint16_t* bank_to_song_sample    = memory::psram_temp_new<uint16_t>(mem.psram, bank.header->sample_count);
    uint8_t* held                    = memory::psram_temp_new<uint8_t>(mem.psram, kHeldBytes);
    // "Нота отпущена, но держится педалью" - отдельным флагом, а не меткой
    // внутри held: там номера каналов, и метка путалась бы с номером.
    uint8_t* pending = memory::psram_temp_new<uint8_t>(mem.psram, kPendingBytes);
    // Кэш выбора слоёв: ключ - пресет, клавиша и полоса силы удара. Прямое
    // отображение, 24 КБ временного PSRAM; ключ 0 невозможен (полоса силы
    // удара с нуля не начинается), поэтому нулевая память - пустой кэш.
    LayerCacheEntry* layer_cache = memory::psram_temp_new<LayerCacheEntry>(mem.psram, kLayerCacheSlots);
    if (!song_inst_of || !bank_to_song_sample || !held || !pending || !layer_cache) {
        return fail("helper maps do not fit PSRAM");
    }
    std::memset(layer_cache, 0, sizeof(LayerCacheEntry) * kLayerCacheSlots);
    std::memset(song_inst_of, 0xff, bank.header->instrument_count * sizeof(uint16_t));
    std::memset(bank_to_song_sample, 0xff, bank.header->sample_count * sizeof(uint16_t));
    std::memset(held, 0xff, kHeldBytes);
    std::memset(pending, 0, kPendingBytes);

    // Слияние дорожек, карта темпа и сборка строки - у RowStepper: тем же
    // шагом обязан идти потоковый путь.
    //
    // Файл уже в PSRAM, источник больше не читается: долгие проходы разбора
    // сами обслуживают шину и карту хоста, иначе тот на секунды остаётся без
    // ответа. На ПК крюка нет.
    auto serve = [&]() {
        if (bank.serve) bank.serve(bank.serve_user);
    };

    // Номера слоям (инструмент песни на инструмент банка) и сэмплам банка
    // даёт конвертер в момент постановки ноты: отдельного прохода для выбора
    // слоёв нет, инструменты песни строятся после паттернов. Резидентно,
    // вместе с файлом и курсорами: строки делаются по ходу игры.
    auto* cv_mem = memory::psram_resident_new<Converter>(mem.psram, 1);
    if (!cv_mem) return fail(".mid converter state does not fit PSRAM");
    *cv_mem       = Converter{}; // умолчания полей - значение-инициализацией
    Converter& cv = *cv_mem;
    cv.bank       = bank;
    cv.stats      = &s_stats;
    cv.grid       = grid;
    std::memcpy(cv.drum_channel, drum_channel, sizeof(cv.drum_channel));
    if (s_trace) {
        s_trace->drum_mask = 0;
        for (uint32_t ch = 0; ch < 16; ++ch) {
            if (drum_channel[ch]) s_trace->drum_mask = static_cast<uint16_t>(s_trace->drum_mask | (1u << ch));
        }
    }
    cv.held                         = held;
    cv.pending                      = pending;
    cv.layer_cache                  = layer_cache;
    cv.song_inst_of                 = song_inst_of;
    cv.bank_to_song_sample          = bank_to_song_sample;
    cv.cur_tempo                    = static_cast<uint8_t>(tracker_tempo);
    cv.vibrato_speed                = vibrato_speed;
    cv.want_global                  = out.default_global_volume;
    auto& used_instruments          = cv.used_instruments;
    uint16_t& used_instrument_count = cv.used_instrument_count;
    uint16_t& used_sample_count     = cv.used_sample_count;
    // Числа слоёв в LoadStats - и на успехе, и на отказе упаковщика: они нужны
    // и для невлезшего файла.
    auto note_layer_stats = [&]() {
        uint32_t km = 0, envs = 0;
        for (uint16_t i = 0; i < used_instrument_count; ++i) {
            const bank::BankInstrument& bi  = bank.instruments[used_instruments[i]];
            km                             += bi.keymap_count;
            if (bi.env_volume != bank::kNoIndex) ++envs;
            if (bi.env_filter != bank::kNoIndex) ++envs;
        }
        s_stats.layers_known  = true;
        s_stats.instruments   = used_instrument_count;
        s_stats.samples       = used_sample_count;
        s_stats.keymap_ranges = km;
        s_stats.envelopes     = envs;
        // Верхняя оценка на случай отказа (огибающие по ссылкам, арена их делит);
        // при успешной загрузке ниже - настоящие числа арены.
        s_stats.arena_bytes = static_cast<uint32_t>(used_instrument_count * sizeof(Instrument) + used_sample_count * sizeof(SampleDescriptor) +
                                                    km * sizeof(KeymapRange) + envs * sizeof(Envelope));
    };

    // --- Паттерны ---
    uint16_t* order   = memory::arena_new<uint16_t>(mem.resident, pattern_count);
    Pattern* patterns = memory::arena_new<Pattern>(mem.resident, pattern_count);
    if (!order || !patterns) return fail("resident memory overflowed (patterns)");
    for (uint32_t i = 0; i < pattern_count; ++i) {
        order[i] = static_cast<uint16_t>(i);
    }
    out.order         = order;
    out.order_count   = static_cast<uint16_t>(pattern_count);
    out.patterns      = patterns;
    out.pattern_count = static_cast<uint16_t>(pattern_count);

    // Кадр начала каждого паттерна: по нему отвечает упреждение загрузки
    // ("сколько позиций order играется за первые N секунд"). Проходом
    // секвенсора это больше не взять - строк в PSRAM нет.
    uint32_t* frames_at_pattern = memory::psram_resident_new<uint32_t>(mem.psram, pattern_count);
    if (!frames_at_pattern) return fail(".mid frame map does not fit PSRAM");

    // Шаг объявлен снаружи блока: его слепок уходит в Replay, чтобы строки
    // можно было выдать заново, уже без паттернов.
    RowStepper step;
    uint32_t total_frames_out = 0;
    {
        auto& cells = cv.cells;

        // Общая громкость: выписанная - у шага, запрошенная (SysEx Master
        // Volume) - у конвертера. Пока файл её не шлёт, обе равны умолчанию и
        // колонку эффекта не занимают.
        step.cv          = &cv;
        step.src         = &events;
        step.tempo_ticks = tempo_ticks;
        step.tempo_us    = tempo_us;
        step.tempo_count = tempo_count;
        step.division    = division;
        step.grid        = grid;
        step.kmul        = static_cast<uint32_t>(grid.rows_per_beat) * grid.ticks_per_row;
        // Границы 32 бит у тика модуля: r * K < 65535 * 65025 < 2^32; тик
        // модуля события не больше тика модуля трека, а тот меньше
        // 4096 x 128 x 255 (проверка числа строк выше).
        step.rewind(out.default_global_volume);

        // Проход по всем строкам. Упаковывать их незачем - играть трек будет
        // тот же шаг, из файла; проход нужен ради другого:
        //   - конвертер раздаёт номера инструментов и записей сэмплов песни
        //     в момент постановки ноты, отдельного прохода для этого нет;
        //   - длительность считается здесь же, по темпу каждой строки:
        //     проходом секвенсора её больше не взять, строк в PSRAM нет.
        // Кадры на строку - тиков на строку по формуле тика движка
        // (compute_tick_samples), с тем же округлением.
        (void)cells;
        uint64_t frames = 0;
        for (uint32_t p = 0; p < pattern_count; ++p) {
            serve();
            frames_at_pattern[p] = frames > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(frames);
            for (uint32_t r = 0; r < rows_in(p); ++r) {
                step.step(p * kRowsPerPattern + r);
                const uint32_t denom = static_cast<uint32_t>(cv.cur_tempo) * 2u;
                using engine::kSampleRateHz;
                const uint32_t tick_samples  = denom != 0 ? (kSampleRateHz * 5u + denom / 2u) / denom : 0u;
                frames                      += static_cast<uint64_t>(tick_samples) * grid.ticks_per_row;
            }
            patterns[p].row_count     = rows_in(p);
            patterns[p].channel_count = kMaxChannels;
            // Данных в зоне паттернов нет: строку даёт row_fetch.
            patterns[p].psram_offset = Pattern::kInvalidOffset;
        }
        total_frames_out = frames > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(frames);
    }

    // Ревербератор включает конвертер: посыл выписывается на строке, а флаг
    // песни читает движок при создании.
    if (cv.reverb_used) out.reverb_enabled = true;

    // Состояние для повторной выдачи строк. Само оно уже в PSRAM, здесь
    // только слепок указателей и курсоров - чтобы играть трек из файла, а не
    // из упакованных паттернов.
    {
        Replay* r = memory::psram_resident_new<Replay>(mem.psram, 1);
        if (!r) return fail(".mid repeat state does not fit PSRAM");
        *r                   = Replay{};
        r->events            = events;
        r->step              = step;
        r->step.src          = &r->events; // на свою копию источника, не на стековую
        r->cv                = &cv;
        r->total_rows        = total_rows;
        r->held_bytes        = kHeldBytes;
        r->pending_bytes     = kPendingBytes;
        r->start_tempo       = static_cast<uint8_t>(tracker_tempo);
        r->global_volume     = out.default_global_volume;
        r->total_frames      = total_frames_out;
        r->frames_at_pattern = frames_at_pattern;
        r->pattern_count     = pattern_count;
        s_replay             = r;
        // Строки трек берёт из файла: упакованных паттернов у .mid нет.
        out.row_fetch      = &fetch_row;
        out.row_fetch_user = nullptr;
        replay_rewind();
    }

    if (used_instrument_count == 0) return fail("no bank instrument matched");
    note_layer_stats();

    // --- Инструменты и сэмплы песни ---
    uint32_t env_copies = 0; // огибающих в арене - для LoadStats
    if (const char* why = build_instruments(out, mem, bank, used_instruments, used_instrument_count, used_sample_count, bank_to_song_sample,
                                            ticks_per_second_rounded, env_copies)) {
        return fail(why);
    }

    // Сколько PCM нужно песне: по её сэмплам, общий прогон банка - один раз
    // (то же правило, что при распаковке). До заморозки зоны паттернов.
    uint32_t need = 0;
    for (uint32_t i = 0; i < out.sample_count; ++i) {
        const uint32_t pcm = bank.samples[out.samples[i].file_offset].pcm_offset;
        bool dup           = false;
        for (uint32_t j = 0; j < i && !dup; ++j) {
            dup = bank.samples[out.samples[j].file_offset].pcm_offset == pcm;
        }
        if (!dup) need += bank.samples[out.samples[i].file_offset].pcm_bytes;
    }
    // Таблица распаковки сэмплов банка - в арене до смены трека: в SRAM
    // постоянно её не держать, трекерным файлам она не нужна.
    if (bank.packed && mem.bank_table == nullptr) {
        bank::BankDecodeTable* table = memory::arena_new<bank::BankDecodeTable>(mem.resident);
        if (!table) return fail("resident memory overflowed (bank decode table)");
        bank::bank_build_decode_table(*bank.model, *table);
        mem.bank_table = table;
    }

    // Паттерны упакованы - весь остаток PSRAM сэмплам, временное загрузчика
    // тоже: дальше оно не читается. Строго после последней записи в зону
    // паттернов и до первой страницы.
    const uint32_t free_pages = memory::psram_freeze_pattern_zone(mem.psram);
    {
        s_stats.psram_known     = true;
        s_stats.arena_bytes     = static_cast<uint32_t>(memory::arena_used(mem.resident));
        s_stats.envelopes       = env_copies;
        s_stats.free_pages      = free_pages;
        s_stats.samples_need_kb = need / 1024u;
        s_stats.patterns_kb     = mem.psram.pattern_bump_offset / 1024u;
    }

    if (metadata_only) return true;

    // --- Сэмплы: распаковка блоков банка в страницы PSRAM ---
    for (uint32_t i = 0; i < out.sample_count; ++i) {
        if (!load_sample_from_bank(mem, bank, out, static_cast<uint16_t>(i), error_out)) return false;
    }

    return true;
}

bool load_sample_from_bank(memory::TrackMemory& mem, const bank::Bank& bank, const Song& song, uint16_t index, const char** error_out) {
    if (error_out) *error_out = nullptr;
    if (index >= song.sample_count) return true;
    if (memory::sample_cache_find(mem.sample_cache, index) != nullptr) return true; // уже резидентен

    const uint16_t bs = static_cast<uint16_t>(song.samples[index].file_offset);
    if (!bank.valid() || bs >= bank.header->sample_count) {
        if (error_out) *error_out = ".mid sample points outside the bank";
        return false;
    }

    // Записи сэмпла с общим блоком PCM (разные затухание, панорама, частота)
    // берут одну цепочку страниц: при игре она только читается. Без этого на
    // Bohemian Rhapsody 9 МБ вместо полутора.
    uint16_t first = memory::kPageChainEnd, cp_page = memory::kPageChainEnd;
    bool shared_chain = false;
    for (uint32_t other = 0; other < song.sample_count; ++other) {
        if (other == index) continue;
        if (song.samples[other].file_offset >= bank.header->sample_count) continue;
        if (bank.samples[song.samples[other].file_offset].pcm_offset != bank.samples[bs].pcm_offset) continue;
        const memory::SampleCacheEntry* e = memory::sample_cache_find(mem.sample_cache, static_cast<uint16_t>(other));
        if (e) {
            first        = e->first_page;
            cp_page      = e->checkpoint_first_page;
            shared_chain = true;
            break;
        }
    }
    if (first == memory::kPageChainEnd) {
        bool read_failed = false;
        first            = bank::bank_make_resident(bank, mem.bank_table, bs, mem.psram, &cp_page, &read_failed);
        if (first == memory::kPageChainEnd) {
            if (error_out) *error_out = read_failed ? bank::kBankReadError : "bank samples do not fit PSRAM";
            return false;
        }
    }
    if (!memory::sample_cache_alloc_slot(mem.sample_cache, index, first, cp_page)) {
        // Цепочка, распакованная этим вызовом, иначе жила бы до смены трека,
        // а повтор после вытеснения распаковал бы её заново. Общую с другой
        // записью не трогать - она той записи.
        if (!shared_chain) memory::psram_free_chain(mem.psram, first);
        if (error_out) *error_out = memory::kSampleCatalogFull;
        return false;
    }
    return true;
}

} // namespace soundsinth::formats::midi
