// Эмуляция General Sound (bridge.h): порты GS на шине, приём модуля в
// PSRAM, разбор общим загрузчиком трека.


#include "player/gs/bridge.h"

#include <cinttypes>
#include <cstdio>
#include <iterator>

#include "player/session.h"
#include "platform/log.h"
#include "player/shared_state.h"

#include "devices/hal/z80_ports.h"
#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/memory.h"
#include "player/hal/host_link.h"
#include "platform/mono_time.h"
#include "devices/gs/gs_device.h"
#include "core/engine/engine_defs.h"
#include "core/formats/load_stats.h"
#include "core/formats/memory_byte_source.h"
#include "player/load/session_loader.h"
#include "core/memory/psram_store.h"

namespace player::gs {

namespace {

// Родные порты GS; протокол платы на 0x63/0x67, эти свободны.
constexpr uint8_t kPortGsCom = 0xBBu; // запись - команда, чтение - состояние
constexpr uint8_t kPortGsDat = 0xB3u; // данные в обе стороны

devices::gs::GsDevice s_dev;

// --- Приём потока ---
//
// Байты приходят по одному из прерывания записи в порт. В PSRAM оттуда
// класть нельзя: запись через кэшируемый алиас, а прерывание должно быть
// коротким. В ISR - только кольцо в SRAM, в PSRAM перекладывает основной
// поток.
//
// Кольцо 4 КБ при темпе Z80 61 такт на байт - запас около 70 мс на 3.5 МГц
// (17 мкс на байт) и около 18 мс на 14 МГц.
constexpr uint32_t kRingBytes = 4096;
constexpr uint32_t kRingMask = kRingBytes - 1;
uint8_t s_ring[kRingBytes];
volatile uint32_t s_ring_head = 0; // пишет ISR
volatile uint32_t s_ring_tail = 0; // читает основной поток
volatile uint32_t s_ring_full = 0; // сколько байт потеряно на переполнении
// Снимок счётчика на начале модуля: разность говорит, что в этом модуле
// дыра. Пишет только ISR, читает основной поток.
volatile uint32_t s_ring_full_at_begin = 0;

// Куда складывается модуль целиком: сразу за хранилищем трека, каким оно
// было в момент резерва (try_reserve_receive_buffer). С зонами паттернов и сэмплов
// не пересекается, пока резерв не возвращён.
uint32_t s_gs_offset = 0;
uint8_t* gs_buffer() {
    return platform::psram_base_acquire(soundsinth::memory::kPsramChipBytes) + s_gs_offset;
}
constexpr uint32_t kBufferBytes = soundsinth::memory::kGsReceiveBytes;

uint32_t s_recv_len = 0; // сколько уже переложено в PSRAM
bool s_overflow = false; // модуль не влез в буфер
volatile bool s_load_pending = false; // #D2 пришёл, разбирать в основном потоке
// Последний модуль разобран: #31 есть что запускать. После отказа песня в
// памяти недогружена, собирать из неё движок нельзя. Снимает #D2 (от него до
// итога разбора #31 ничего не запускает), ставит finish_module.
volatile bool s_module_ready = false;
// Сколько потоков модуля начато (#30), пишет обработчик. Поток, начатый во
// время разбора, делает итог разбора устаревшим: s_module_ready не ставится.
volatile uint32_t s_streams_begun = 0;

uint32_t s_writes_cmd = 0;
uint32_t s_writes_dat = 0;
uint32_t s_reads_dat = 0;
uint32_t s_reads_stat = 0;
uint32_t s_module_loads = 0;
uint32_t s_module_fails = 0;

// Приём модуля от первого байта до #D2 - только основной поток: пик
// заполнения кольца, наибольший промежуток между переливами при идущем
// потоке и снимки счётчиков потерь шины (разность - в строку модуля).
bool s_module_started = false;
uint32_t s_ring_peak = 0;
uint32_t s_drain_gap_max_us = 0;
uint32_t s_last_drain_us = 0;
uint32_t s_pio_lost_at_start = 0;
uint32_t s_divmmc_lost_at_start = 0;

// --- Пролог разговора ---
//
// Счётчики отвечают "сколько", а нужно "что именно". Детект карты
// укладывается в первые полсотни обращений, поэтому записывается начало,
// а не хвост: кольцо последних событий показало бы что угодно, кроме
// нужного.
//
// Запись из прерывания: два байта и инкремент, одно сравнение.
constexpr uint32_t kPrologueLen = 192;

// Вид обращения. Буквы читаются без расшифровки: большая -
// команда/состояние, малая - данные.
enum class GsEv : uint8_t {
    WrCmd, // C - запись команды
    RdStat, // S - чтение состояния
    WrDat, // d - запись данных
    RdDat, // r - чтение данных
};

struct GsEvent {
    GsEv what;
    uint8_t value;
};
GsEvent s_prologue[kPrologueLen];
volatile uint32_t s_prologue_n = 0;
uint32_t s_prologue_seen = 0; // сколько было на прошлой проверке
// Сколько всплесков напечатано. Печатается каждый, а не только первый: на
// этих портах говорит не только плеер, всплеск при загрузке израсходовал
// бы единственную попытку до запуска плеера. Предел - чтобы шум не залил
// лог.
uint32_t s_dumps = 0;
constexpr uint32_t kMaxDumps = 6;

// Запись заряжается первой командой, а не первым обращением: порты GS
// внутри диапазона ATA у divIDE (0xA3..0xBF), к порту данных стучится не
// только плеер. Аргументы идут до команды (SD Port / SC #10), поэтому
// короткое предисловие при заряде переливается в начало записи.
constexpr uint32_t kRecentLen = 4;
GsEvent s_recent[kRecentLen];
volatile uint32_t s_recent_n = 0;
volatile bool s_recording = false;

inline void SOUNDSINTH_HOT_PATH(record_event)(GsEv what, uint8_t value) {
    if (!s_recording) {
        const uint32_t p = s_recent_n;
        s_recent[p % kRecentLen].what = what;
        s_recent[p % kRecentLen].value = value;
        s_recent_n = p + 1u;
        return;
    }
    const uint32_t n = s_prologue_n;
    if (n >= kPrologueLen) return;
    s_prologue[n].what = what;
    s_prologue[n].value = value;
    s_prologue_n = n + 1u;
}

// Перелить предисловие и открыть запись. Из прерывания записи команды:
// четыре копирования.
inline void SOUNDSINTH_HOT_PATH(start_recording)() {
    if (s_recording) return;
    const uint32_t have = s_recent_n < kRecentLen ? s_recent_n : kRecentLen;
    const uint32_t first = s_recent_n - have;
    for (uint32_t i = 0; i < have; ++i) {
        s_prologue[i] = s_recent[(first + i) % kRecentLen];
    }
    s_prologue_n = have;
    s_recording = true;
}

// Бросить принятое. Хвост кольца, s_recv_len и s_overflow пишет только
// основной поток: обработчик оставляет снимок головы и просьбу, основной
// поток применяет её в drain_ring. Байты после снимка - уже новый поток.
// s_load_pending гасит обработчик: он же его взводит, и порядок #D2 и
// #F3 сохраняется.
volatile uint32_t s_drop_head = 0;
volatile bool s_drop_req = false;

SOUNDSINTH_ALWAYS_INLINE void request_drop() {
    s_drop_head = s_ring_head;
    s_drop_req = true;
    // Разбор брошен вместе с принятым: снять его удержание здесь, иначе не
    // снимет никто и плеер останется придержанным навсегда.
    if (s_load_pending) s_dev.hold_parse_done();
    s_load_pending = false;
}

// Основной поток. Флаг снимается раньше чтения снимка: сброс, пришедший
// между ними, не теряется.
void apply_drop() {
    if (!s_drop_req) return;
    s_drop_req = false;
    s_ring_tail = s_drop_head;
    s_recv_len = 0;
    s_overflow = false;
    s_module_started = false; // поток начат заново: счёт приёма - с его первого байта
}

// --- События автомата ---

volatile bool s_gs_wants_reserve = false; // ставит начало потока
bool s_gs_reserved = false; // пишет основной поток, читает и on_stream_begin
volatile bool s_reserve_hold = false; // плеер придержан битом занятости

// Начат приём модуля (#30). Автомат открывает поток только для модуля.
void SOUNDSINTH_HOT_PATH(on_stream_begin)() {
    // Здесь, а не на сбросе и не на любой записи в порт: плеер щупает
    // порты, чтобы понять, есть ли GS, и это не должно ничего выделять и
    // снимать играющий трек. Память просится на команде загрузки модуля,
    // после которой пойдут байты.
    ++s_streams_begun;
    s_ring_full_at_begin = s_ring_full;
    s_gs_wants_reserve = true;
    if (!s_gs_reserved) {
        // Плеер придерживается битом занятости: снять движок и пересобрать
        // список страниц - не мгновенно, а кольцо приёма 4 КБ, начало модуля
        // терять нельзя.
        s_dev.hold_reserve_begin();
        s_reserve_hold = true;
    }
    request_drop();
}

void SOUNDSINTH_HOT_PATH(on_stream_byte)(uint8_t b) {
    const uint32_t head = s_ring_head;
    const uint32_t next = (head + 1u) & kRingMask;
    if (next == s_ring_tail) { // кольцо полно, байт теряется
        ++s_ring_full;
        return;
    }
    s_ring[head] = b;
    s_ring_head = next;
}

// --- Отрезание приёмного буфера по требованию ---
//
// Мегабайт в хвосте PSRAM нужен только пока идёт разговор с плеером под
// GS. Одновременно GS и .mid не играют, держать его отрезанным всегда -
// отнимать у каждого трека.
//
// Отрезается по команде загрузки модуля (#30): детектирование не должно
// выделять память и снимать трек. Между модулями не возвращается - плеер
// шлёт их пачками, - а возвращается, когда трек запускают через протокол
// платы (release_buffer): тогда GS больше не говорит.
//
// Сужать хранилище под играющим треком нельзя: psram_set_track_bytes
// пересобирает список свободных страниц, а трек играет из страниц,
// которых после пересборки нет. Поэтому сначала движок снимается тем же
// путём, что при смене трека (g_teardown_requested), и только когда Core0
// подтвердил (g_engine_alive == false), хранилище режется. Плеер всё это
// время придержан битом занятости.

bool try_reserve_receive_buffer() {
    if (s_gs_reserved) return true;
    if (!s_gs_wants_reserve) return false;
    // Запрет сборки держится до track_load_end в finish_module или в сессии
    // платы: после нарезки песни в памяти нет. Пока движок жив - просьба о
    // сносе и следующий опрос; байты копятся в кольце в SRAM, в PSRAM не
    // пишется ни одного.
    if (!shared::try_stop_engine_for_load()) return false;
    // Буфер - верхний мегабайт хранилища, под таблицами банка с карты,
    // если они есть.
    const uint32_t bytes = shared::g_track_memory.psram.track_bytes - soundsinth::memory::kGsReceiveBytes;
    soundsinth::memory::psram_set_track_bytes(shared::g_track_memory.psram, bytes);
    s_gs_offset = bytes;
    s_gs_reserved = true;
    platform::debug_log("gs: приёмный буфер отрезан, хранилище трека ужато\n");
    return true;
}

// Карта сброшена (#F3/#F4): принятое не нужно.
void SOUNDSINTH_HOT_PATH(on_reset)() {
    request_drop();
}

// --- Порты ---

// Оба порта отвечают из таблицы, которую разбирает DMA, байт уходит на
// шину задолго до того, как обработчик узнает о чтении. После каждого
// изменения состояния обновляются обе клетки: слово состояния и байт
// следующего чтения регистра данных.
//
// Публикуется ответ автомата GS: через GSDAT Z80 получает дескриптор
// модуля (#30), объём памяти (#20), значение порта (#11).
// Оба значения - до первой записи, данные раньше состояния: драйвер,
// ждущий бит данных (WN), читает GSDAT сразу, как увидит состояние.
inline void SOUNDSINTH_HOT_PATH(publish)() {
    const uint8_t status = s_dev.read_status();
    const uint8_t data = s_dev.peek_data();
    devices::hal::z80_port_set_read(kPortGsDat, data);
    devices::hal::z80_port_set_read(kPortGsCom, status);
}

void SOUNDSINTH_HOT_PATH(gs_com_write)(uint8_t, uint8_t data) {
    ++s_writes_cmd;
    start_recording();
    record_event(GsEv::WrCmd, data);
    // Где сейчас играет, для #60/#61/#62: из общего состояния, которое
    // наполняет наблюдатель тиков движка; своего доступа к трекеру у моста
    // нет. Строка шире шести бит ответу не нужна: у MOD в паттерне 64.
    s_dev.set_position(static_cast<uint8_t>(shared::g_order_pos.load(std::memory_order_relaxed) & 0xffu),
                       static_cast<uint8_t>(shared::g_row_pos.load(std::memory_order_relaxed) & 0x3fu));
    switch (s_dev.write_command(data)) {
        case devices::gs::Event::StreamBegin:
            on_stream_begin();
            break;
        case devices::gs::Event::StreamEnd:
            s_module_ready = false;
            s_load_pending = true; // разбирать в основном потоке
            break;
        case devices::gs::Event::Reset:
            on_reset();
            break;
        case devices::gs::Event::Play:
            // Через ту же точку, что трек с карты: смена поколения песни.
            if (s_module_ready) {
                shared::g_track_end_frame.store(0, std::memory_order_relaxed); // конец задаёт плеер GS
                shared::g_song_generation.fetch_add(1, std::memory_order_release);
            }
            break;
        default:
            // Stop/Resume: своего плеера нет.
            break;
    }
    publish();
}

// Чтение слова состояния. Сейчас не регистрируется, s_reads_stat остаётся
// нулём.
[[maybe_unused]] void SOUNDSINTH_HOT_PATH(gs_com_read_done)(uint8_t) {
    ++s_reads_stat;
    record_event(GsEv::RdStat, s_dev.read_status());
}

void SOUNDSINTH_HOT_PATH(gs_dat_write)(uint8_t, uint8_t data) {
    ++s_writes_dat;
    record_event(GsEv::WrDat, data);
    if (s_dev.write_data(data)) {
        on_stream_byte(data);
    }
    publish();
}

// Чтение GSDAT состоялось: продвинуть указатель и зарядить следующий
// байт.
//
// read_data - первым делом: в режиме DivMMC запись следующей команды
// вытесняет этот обработчик, и read_data после неё съела бы первый байт
// нового ответа - плеер встал бы на ожидании бита данных.
void SOUNDSINTH_HOT_PATH(gs_dat_read_done)(uint8_t) {
    ++s_reads_dat;
    const uint8_t data = s_dev.read_data(); // продвигает указатель ответа
    record_event(GsEv::RdDat, data);
    publish();
}

// --- Разбор накопленного, основной поток ---

void drain_ring() {
    apply_drop();
    uint32_t tail = s_ring_tail;
    const uint32_t head = s_ring_head;
    const uint32_t now = platform::mono_us();
    const uint32_t fill = (head - tail) & kRingMask;
    if (fill != 0) {
        if (!s_module_started) {
            s_module_started = true;
            s_ring_peak = 0;
            s_drain_gap_max_us = 0;
            s_pio_lost_at_start = hal::bus_lost_port_writes();
            s_divmmc_lost_at_start = hal::bus_lost_rom_writes();
        } else if (now - s_last_drain_us > s_drain_gap_max_us) {
            s_drain_gap_max_us = now - s_last_drain_us;
        }
        if (fill > s_ring_peak) s_ring_peak = fill;
        // Перелив под звук - как фоновая догрузка: заминки в его окне видны.
        shared::g_background_loading.store(true, std::memory_order_relaxed);
    }
    s_last_drain_us = now;
    while (tail != head && s_recv_len < kBufferBytes) {
        gs_buffer()[s_recv_len++] = s_ring[tail];
        tail = (tail + 1u) & kRingMask;
    }
    // Буфер полон, а байты ещё есть: модуль не влез, остаток не нужен.
    if (tail != head) {
        s_overflow = true;
        tail = head;
    }
    s_ring_tail = tail;
    if (fill != 0) shared::g_background_loading.store(false, std::memory_order_relaxed);
    apply_drop(); // сброс во время копирования: скопированное - старый поток
}

// Вторая строка модуля: сэмплы, время разбора и приём. Печатается и при
// отказе - по ней видно, лёг ли модуль целиком.
SOUNDSINTH_NOINLINE void log_module_details(uint32_t parse_ms, bool parsed) {
    const auto& ls = soundsinth::model::g_tracker_load_stats;
    const uint32_t pio_lost = hal::bus_lost_port_writes() - s_pio_lost_at_start;
    const uint32_t divmmc_lost = hal::bus_lost_rom_writes() - s_divmmc_lost_at_start;
    char m[320]; // причина отказа сэмпла - до 81 байта, кириллица по два
    snprintf(m, sizeof(m),
             "gs: модуль: сэмплов %u, не легло %u%s%s%s, выброшено %u, нерезидентных %u, разбор %" PRIu32
             " мс | кольцо пик %" PRIu32 " из %" PRIu32 ", перелив раз в %" PRIu32 " мс макс, потери PIO %" PRIu32
             ", DivMMC %" PRIu32 "\n",
             parsed ? static_cast<unsigned>(shared::g_song.sample_count) : 0u, static_cast<unsigned>(ls.samples_failed),
             ls.samples_failed ? " (первый: " : "",
             ls.samples_failed ? (ls.first_failure ? ls.first_failure : "?") : "", ls.samples_failed ? ")" : "",
             static_cast<unsigned>(ls.samples_dropped), static_cast<unsigned>(ls.samples_nonresident), parse_ms,
             s_ring_peak, kRingBytes, s_drain_gap_max_us / 1000u, pio_lost, divmmc_lost);
    platform::debug_log(m);
    s_module_started = false;
}

// Плеер придержан битом занятости до резерва (on_stream_begin): отпустить.
SOUNDSINTH_ALWAYS_INLINE void release_player_hold() {
    s_reserve_hold = false;
    s_dev.hold_reserve_done();
    publish();
}

// Строки итога модуля - своим кадром (SOUNDSINTH_NOINLINE): буфер строки не
// лежит на стеке Core1 под разбором.
// Недобор виден только по заголовкам самого модуля: ни кольцо, ни буфер о
// нём не знают.
SOUNDSINTH_NOINLINE void log_module_short(uint32_t source_end) {
    platform::debug_logf("gs: модуль НЕ принят: принято %" PRIu32 " байт, по заголовку %" PRIu32 " - недобор %" PRIu32 "\n",
               s_recv_len, source_end, source_end - s_recv_len);
}

SOUNDSINTH_NOINLINE void log_module_rejected(uint32_t lost) {
    char m[192];
    snprintf(m, sizeof(m),
             "gs: модуль НЕ принят: %" PRIu32 " байт, переполнение=%d (буфер %" PRIu32
             "), потеряно в кольце %" PRIu32 ", движка до новой загрузки нет\n",
             s_recv_len, static_cast<int>(s_overflow), kBufferBytes, lost);
    platform::debug_log(m);
}

SOUNDSINTH_NOINLINE void log_module_result(bool ok, const player::load::SessionLoadResult& load,
                                           uint32_t teardown_ms) {
    char m[192];
    if (ok) {
        snprintf(m, sizeof(m),
                 "gs: модуль принят, %" PRIu32 " байт, каналов %u, длительность %" PRIu32 " с, снос движка %" PRIu32
                 " мс\n",
                 s_recv_len, static_cast<unsigned>(shared::g_song.channel_count),
                 load.total_frames / soundsinth::engine::kSampleRateHz, teardown_ms);
    } else {
        snprintf(m, sizeof(m), "gs: модуль НЕ разобран (%" PRIu32 " байт): %s\n", s_recv_len,
                 load.error ? load.error : "(без причины)");
    }
    platform::debug_log(m);
}

// Ожидание сноса движка: шина и карта обслуживаются, а затянувшееся
// ожидание видно в логе до того, как кончится.
constexpr uint32_t kSlowTeardownUs = 200000;

struct TeardownWait {
    uint32_t started_us;
    bool reported;
};

void pump_teardown_wait(void* user) {
    auto* w = static_cast<TeardownWait*>(user);
    player::serve_without_wait(nullptr);
    if (!w->reported && platform::mono_us() - w->started_us > kSlowTeardownUs) {
        w->reported = true;
        platform::debug_log("gs: снос движка ждём дольше 200 мс\n");
    }
}

void finish_module() {
    const uint32_t streams = s_streams_begun;
    drain_ring();

    // Байты, не влезшие в кольцо, - дыра посреди модуля: MOD с нехваткой PCM
    // разбирается и играет мусором, а причина видна только по редкой строке
    // gs:. Отказ вместо мусора; обратного давления у потока нет.
    const uint32_t lost = s_ring_full - s_ring_full_at_begin;
    if (s_overflow || s_recv_len == 0 || lost != 0) {
        log_module_rejected(lost);
        ++s_module_fails;
        log_module_details(0, false);
        s_dev.hold_parse_done();
        return;
    }

    // Разбор пишет Song и память трека, которые читает движок: сначала снять
    // его. Плеер всё это время придержан (#D2). Буфер разбор читает тем же
    // кэшируемым алиасом, которым его заполнил перелив: сброс кэша не нужен.
    shared::g_core1_phase.store(shared::Core1Phase::GsModule, std::memory_order_relaxed);
    TeardownWait wait{platform::mono_us(), false};
    shared::track_load_begin(&pump_teardown_wait, &wait);
    const uint32_t teardown_ms = (platform::mono_us() - wait.started_us) / 1000u;
    soundsinth::formats::MemoryByteSource src(gs_buffer(), s_recv_len);
    player::load::SessionLoadResult load;
    // Тот же вызов, что грузит трек с карты. Своего разбора у эмуляции GS
    // нет: приходит обычный MOD, загрузчик выбирает формат по содержимому
    // (имени у потока нет).
    const uint32_t parse_started_us = platform::mono_us();
    const bool ok = player::load::run_session_load(src.as_byte_source(), shared::g_track_memory, shared::g_song, load,
                                                     /*metadata_only=*/false);
    const uint32_t parse_ms = (platform::mono_us() - parse_started_us) / 1000u;
    // Недобор: конец файла по его же заголовкам дальше принятого. У MOD
    // длины сэмплов точные, и это ловит третье место потери байта - очередь
    // записи шины, откуда байт до автомата не доходит и ни в одном счётчике
    // приёма не виден. Песня при этом разобрана, но части PCM в ней нет.
    const uint32_t source_end = soundsinth::model::g_tracker_load_stats.source_end;
    const bool short_module = ok && source_end > s_recv_len;
    const bool good = ok && !short_module;
    if (short_module) log_module_short(source_end);
    if (good) player::publish_file_info(load.total_frames, shared::g_song);
    s_module_ready = good && s_streams_begun == streams;
    shared::track_load_end();
    if (good) {
        ++s_module_loads;
        shared::publish_loaded_track(0); // конец задаёт плеер GS
    } else {
        ++s_module_fails;
    }
    log_module_result(good, load, teardown_ms);
    log_module_details(parse_ms, good);
    // Удержание разбора снимается только здесь: плеер всё это время ждал,
    // отпустить раньше - сказать, что готово, когда не готово.
    s_dev.hold_parse_done();
}

} // namespace

void init() {
    s_dev.reset();

    devices::hal::z80_port_on_write(kPortGsCom, &gs_com_write);
    devices::hal::z80_port_on_write(kPortGsDat, &gs_dat_write);
    devices::hal::z80_port_on_read_done(kPortGsDat, &gs_dat_read_done);
    // Хук на чтение 0xBB не регистрируется: он нужен только счётчику чтений
    // состояния (st=- в строке gs:), на плате с ним не проверялось.
    publish();

    platform::debug_log("gs: эмуляция подключена на 0xBB (команды/состояние) и 0xB3 (данные)\n");
}

void release_buffer() {
    // Запрос снимается в любом состоянии, и тот, что ещё ждёт сноса движка:
    // иначе следующий poll снёс бы движок, собранный из трека
    // платы, и отрезал его зоны. Принятое и незаконченное бросается: память
    // уходит треку. Модуль в памяти трека будет переписан загрузкой, #31
    // запускать нечего.
    request_drop();
    apply_drop();
    s_module_ready = false;
    s_gs_wants_reserve = false;
    // Плеер ждал резерва, а его не будет: отпустить, иначе он встанет на
    // ожидании навсегда. Его модуль не примется (кольцо не разбирается без
    // резерва).
    if (s_reserve_hold) release_player_hold();
    if (!s_gs_reserved) return;
    soundsinth::memory::psram_set_track_bytes(shared::g_track_memory.psram, shared::g_track_memory.psram.track_bytes +
                                                                                soundsinth::memory::kGsReceiveBytes);
    s_gs_reserved = false;
    platform::debug_log("gs: приёмный буфер возвращён треку\n");
}

bool wants_track_memory() {
    return s_gs_wants_reserve;
}

void poll() {
    if (!try_reserve_receive_buffer()) return;
    if (s_reserve_hold) release_player_hold();
    drain_ring();
    if (s_load_pending) {
        s_load_pending = false;
        finish_module();
        publish();
    }
}

namespace {

// Начало разговора с картой - первые две сотни обращений по порядку. Детект
// ломается на "что и в каком порядке", гистограммой это не разобрать.
// Печатает каждый всплеск, не больше kMaxDumps раз, когда разговор
// устоялся: запись заполнилась или с прошлого вызова не прибавилось
// событий. line - буфер строки вызывающего, не меньше 128 байт.
void dump_prologue_if_settled(char* line, size_t size) {
    if (s_dumps >= kMaxDumps) return;
    const uint32_t n = s_prologue_n;
    if (n == 0) return;
    if (n < kPrologueLen && n != s_prologue_seen) {
        s_prologue_seen = n;
        return;
    }
    ++s_dumps;

    // Буква на вид обращения, по порядку GsEv.
    static constexpr char kLetter[] = {'C', 'S', 'd', 'r'};
    static_assert(std::size(kLetter) == 4, "буква на каждый вид GsEv");
    int off = snprintf(line, size, "gs: пролог (%" PRIu32 "): ", n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t w = static_cast<uint8_t>(s_prologue[i].what) & 3u;
        off += snprintf(line + off, size - off, "%c%02X ", kLetter[w], static_cast<unsigned>(s_prologue[i].value));
        if (off > 100 || i + 1u == n) {
            snprintf(line + off, size - off, "\n");
            platform::debug_log(line);
            off = snprintf(line, size, "gs:            ");
        }
    }
    platform::debug_log("gs: C команда, S состояние, d данные от ZX, r данные к ZX\n");

    // Запись не разоружается: после последней команды плеер читает порт
    // данных сотнями (259 раз подряд), а новый всплеск заряжается только
    // командой.
    //
    // Предисловие сбрасывается: оно нужно только для аргументов перед первой
    // командой.
    s_prologue_n = 0;
    s_prologue_seen = 0;
    s_recent_n = 0;
}

} // namespace

void log_stats() {
    if (!(s_writes_cmd || s_writes_dat || s_reads_dat)) return;
    // Одна строка за раз: debug_log копирует её в кольцо.
    char gm[208];
    snprintf(gm, sizeof(gm),
             "gs: cmd=%" PRIu32 " dat=%" PRIu32 " rd=%" PRIu32 " st=- | поток %" PRIu32 " Б, модулей %" PRIu32
             ", отказов %" PRIu32 ", кольцо полно %" PRIu32 " | last=%02X status=%02X unknown=%02X x%" PRIu32 "\n",
             s_writes_cmd, s_writes_dat, s_reads_dat, s_dev.stream_bytes(), s_module_loads, s_module_fails,
             static_cast<uint32_t>(s_ring_full), static_cast<unsigned>(s_dev.last_command()),
             static_cast<unsigned>(s_dev.read_status()), static_cast<unsigned>(s_dev.unknown_command()),
             s_dev.unknown_count());
    platform::debug_log(gm);

    // Четыре самые частые команды за один проход: каждый счётчик читается
    // один раз (растут в обработчике записи на Core1, пока здесь идёт
    // обход). По убыванию числа, при равенстве меньший код раньше.
    constexpr uint32_t kTop = 4;
    uint16_t top_n[kTop] = {};
    uint8_t top_c[kTop] = {};
    uint32_t count = 0;
    for (uint32_t c = 0; c < 256; ++c) {
        const uint16_t n = s_dev.command_count(static_cast<uint8_t>(c));
        if (n == 0) continue;
        if (count == kTop && n <= top_n[kTop - 1]) continue;
        uint32_t i = count < kTop ? count++ : kTop - 1;
        while (i > 0 && n > top_n[i - 1]) {
            top_n[i] = top_n[i - 1];
            top_c[i] = top_c[i - 1];
            --i;
        }
        top_n[i] = n;
        top_c[i] = static_cast<uint8_t>(c);
    }
    int off = snprintf(gm, sizeof(gm), "gs: чаще всего:");
    for (uint32_t i = 0; i < count && off < static_cast<int>(sizeof(gm)); ++i) {
        off += snprintf(gm + off, sizeof(gm) - off, " %02X x%u", static_cast<unsigned>(top_c[i]),
                        static_cast<unsigned>(top_n[i]));
    }
    if (off < static_cast<int>(sizeof(gm))) snprintf(gm + off, sizeof(gm) - off, "\n");
    platform::debug_log(gm);

    dump_prologue_if_settled(gm, sizeof(gm));
}

} // namespace player::gs
