// SPDX-License-Identifier: MIT
// Эмуляция General Sound: порты GS на шине, приём модуля в
// PSRAM, разбор общим загрузчиком трека.

#include "player/gs/bridge.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/memory.h"
#include "platform/mono_time.h"

#include "core/engine/engine_defs.h"
#include "core/formats/load_stats.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/psram_store.h"
#include "core/util/crc32.h"

#include "devices/config/config_service.h"
#include "devices/gs/gs_device.h"
#include "devices/hal/z80_ports.h"

#include "player/gs/fx_player.h"
#include "player/gs/midi_sched.h"
#include "player/hal/host_link.h"
#include "player/load/session_loader.h"
#include "player/session.h"
#include "player/shared_state.h"

namespace player::gs {

namespace {

// Родные порты GS; протокол платы на 0x63/0x67, эти свободны.
constexpr uint8_t kPortGsCom = 0xbbu; // запись - команда, чтение - состояние
constexpr uint8_t kPortGsDat = 0xb3u; // данные в обе стороны

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
constexpr uint32_t kRingMask  = kRingBytes - 1;
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
// База - у владельца PSRAM, хранилища трека: psram_base_acquire - выделение
// по контракту, на ПК каждый вызов даёт новый блок.
uint8_t* gs_buffer() {
    return shared::g_track_memory.psram.base + s_gs_offset;
}
// Отрезанная под GS область делится надвое: хвост под сэмплы эффектов,
// остальное под модуль. У исходной карты под эффекты отведено 112 КБ, игры
// заливают единицы сэмплов; 128 КБ с запасом, а модулю остаётся 896 КБ -
// четырёхканальному MOD этого много больше, чем нужно.
constexpr uint32_t kFxAreaBytes = 128u * 1024u;
constexpr uint32_t kBufferBytes = soundsinth::memory::kGsReceiveBytes - kFxAreaBytes;
constexpr uint32_t kFxAreaAt    = kBufferBytes;

// Своё кольцо под сэмплы, а не общее с модулем. Общее потребовало бы метить
// байты принадлежностью: пока основной поток доберётся до кольца, поток
// успевает смениться, и хвост сэмпла ушёл бы в модуль. Цена разделения -
// 2 КБ SRAM, приём модуля не меняется.
constexpr uint32_t kFxRingBytes = 2048;

// Рабочая область проигрывателя MIDI - в хвосте той части, куда ложится
// файл: имена дорожек, справка, блок состояния и порция кадра. Файл .mid у
// плеера не длиннее 64 КБ, так что до неё он не достаёт.
constexpr uint32_t kMidiWorkBytes = 4096;
constexpr uint32_t kMidiWorkAt    = kBufferBytes - kMidiWorkBytes;
static_assert(kMidiWorkBytes >= player::gs::kMidiWorkSize, "рабочая область MIDI меньше раскладки");

MidiSched s_midi;
volatile bool s_midi_stream    = false; // идёт приём .mid, а не модуля
volatile bool s_midi_pending   = false; // принят, разобрать в основном потоке
uint32_t s_midi_len            = 0;
constexpr uint32_t kFxRingMask = kFxRingBytes - 1;
uint8_t s_fx_ring[kFxRingBytes];
volatile uint32_t s_fx_ring_head = 0; // пишет ISR
volatile uint32_t s_fx_ring_tail = 0; // читает основной поток

uint32_t s_recv_len          = 0;     // сколько уже переложено в PSRAM
bool s_overflow              = false; // модуль не влез в буфер
volatile bool s_load_pending = false; // #D2 пришёл, разбирать в основном потоке
// Последний модуль разобран: #31 есть что запускать. После отказа песня в
// памяти недогружена, собирать из неё движок нельзя. Снимает #D2 (от него до
// итога разбора #31 ничего не запускает), ставит finish_module.
volatile bool s_module_ready = false;
// Сколько потоков модуля начато (#30), пишет обработчик. Поток, начатый во
// время разбора, делает итог разбора устаревшим: s_module_ready не ставится.
volatile uint32_t s_streams_begun = 0;
// Сбросы карты (#F3/#F4). Разбор идёт сотни миллисекунд, и сброс посреди
// него отменяет итог: машина после сброса не должна услышать модуль,
// принятый до него. Пишет только обработчик, читает основной поток.
volatile uint32_t s_resets = 0;

uint32_t s_writes_cmd = 0;
uint32_t s_writes_dat = 0;
// Записей в GSDAT на начало потока. Разница с концом - сколько байт машина
// подала в порт за модуль, и это единственное число, снятое до автомата:
// по нему недобор делится на "прислали мало" и "потеряли у себя".
volatile uint32_t s_writes_dat_at_begin = 0;
uint32_t s_reads_dat                    = 0;
uint32_t s_reads_stat                   = 0;
uint32_t s_module_loads                 = 0;
uint32_t s_module_fails                 = 0;
// Отказы по причинам: следствия у них разные, а лечатся они в разных
// местах. Потеря в кольце - дыра посреди модуля, переполнение - обрезанный
// модуль, недобор по заголовкам - байт, пропавший в очереди записи шины, и
// ни один счётчик приёма его не видит. Сумма меньше s_module_fails на
// пустой приём и на неразобранный модуль.
uint32_t s_fails_lost     = 0;
uint32_t s_fails_overflow = 0;
uint32_t s_fails_short    = 0;

// Приём модуля от первого байта до #D2 - только основной поток: пик
// заполнения кольца, наибольший промежуток между переливами при идущем
// потоке и снимки счётчиков потерь шины (разность - в строку модуля).
bool s_module_started        = false;
uint32_t s_ring_peak         = 0;
uint32_t s_drain_gap_max_us  = 0;
uint32_t s_last_drain_us     = 0;
uint32_t s_pio_lost_at_start = 0;

// --- Первые обращения к портам ---
//
// Нужен порядок обращений, а не их число. Детект карты
// укладывается в первые полсотни обращений, поэтому записывается начало,
// а не хвост: кольцо последних событий показало бы что угодно, кроме
// нужного.
//
// Запись из прерывания: два байта и инкремент, одно сравнение.
constexpr uint32_t kPrologueLen = 192;

// Вид обращения. Буквы читаются без расшифровки: большая -
// команда/состояние, малая - данные.
enum class GsEv : uint8_t {
    WrCmd,  // C - запись команды
    RdStat, // S - чтение состояния
    WrDat,  // d - запись данных
    RdDat,  // r - чтение данных
};

struct GsEvent {
    GsEv what;
    uint8_t value;
};
GsEvent s_prologue[kPrologueLen];
volatile uint32_t s_prologue_n = 0;
uint32_t s_prologue_seen       = 0; // сколько было на прошлой проверке
// Сколько всплесков напечатано. Печатается каждый, а не только первый: на
// этих портах говорит не только плеер, всплеск при загрузке израсходовал
// бы единственную попытку до запуска плеера. Предел - чтобы шум не залил
// лог.
uint32_t s_dumps             = 0;
constexpr uint32_t kMaxDumps = 6;

// Запись заряжается первой командой, а не первым обращением: порты GS
// внутри диапазона ATA у divIDE (0xa3..0xbf), к порту данных стучится не
// только плеер. Аргументы идут до команды (SD Port / SC #10), поэтому
// короткое предисловие при заряде переливается в начало записи.
constexpr uint32_t kRecentLen = 4;
GsEvent s_recent[kRecentLen];
volatile uint32_t s_recent_n = 0;
volatile bool s_recording    = false;

inline void SOUNDSINTH_HOT_PATH(record_event)(GsEv what, uint8_t value) {
    if (!s_recording) {
        const uint32_t p               = s_recent_n;
        s_recent[p % kRecentLen].what  = what;
        s_recent[p % kRecentLen].value = value;
        s_recent_n                     = p + 1u;
        return;
    }
    const uint32_t n = s_prologue_n;
    if (n >= kPrologueLen) return;
    s_prologue[n].what  = what;
    s_prologue[n].value = value;
    s_prologue_n        = n + 1u;
}

// Перелить предисловие и открыть запись. Из прерывания записи команды:
// четыре копирования.
inline void SOUNDSINTH_HOT_PATH(start_recording)() {
    if (s_recording) return;
    const uint32_t have  = s_recent_n < kRecentLen ? s_recent_n : kRecentLen;
    const uint32_t first = s_recent_n - have;
    for (uint32_t i = 0; i < have; ++i) {
        s_prologue[i] = s_recent[(first + i) % kRecentLen];
    }
    s_prologue_n = have;
    s_recording  = true;
}

// Бросить принятое. Хвост кольца, s_recv_len и s_overflow пишет только
// основной поток: обработчик оставляет снимок головы и просьбу, основной
// поток применяет её в drain_ring. Байты после снимка - уже новый поток.
// s_load_pending гасит обработчик: он же его взводит, и порядок #D2 и
// #F3 сохраняется.
volatile uint32_t s_drop_head = 0;
volatile bool s_drop_req      = false;

SOUNDSINTH_ALWAYS_INLINE void request_drop() {
    s_drop_head = s_ring_head;
    s_drop_req  = true;
    // Разбор брошен вместе с принятым: снять его удержание здесь, иначе не
    // снимет никто и плеер останется придержанным навсегда.
    if (s_load_pending) s_dev.hold_parse_done();
    s_load_pending = false;
    // Приёмный буфер вот-вот перестанет быть модулем: снять его как
    // источник паттернов, иначе резидент отдал бы мусор.
    s_dev.set_module_image(nullptr, 0);
    // Пауза - состояние прежнего трека. Останется взведённой - новый
    // зазвучит в тишину, и причины на экране видно не будет.
    shared::g_playback_paused.store(false, std::memory_order_relaxed);
}

// Основной поток. Флаг снимается раньше чтения снимка: сброс, пришедший
// между ними, не теряется.
void apply_drop() {
    if (!s_drop_req) return;
    s_drop_req       = false;
    s_ring_tail      = s_drop_head;
    s_recv_len       = 0;
    s_overflow       = false;
    s_module_started = false; // поток начат заново: счёт приёма - с его первого байта
}

// --- События автомата ---

volatile bool s_gs_wants_reserve = false; // ставит начало потока
bool s_gs_reserved               = false; // пишет основной поток, читает и on_stream_begin
// Взят ли запрет сборки движка: берётся вместе с областью и разбором модуля,
// снимается, когда принимать и разбирать больше нечего.
bool s_load_held = false;
// Отказ в резерве печатается один раз: опрос идёт на каждом витке Core1.
bool s_reserve_refused       = false;
volatile bool s_reserve_hold = false; // плеер придержан битом занятости

// Поток сэмпла эффекта (#38/#3E). Байты идут в хвост области GS, приёмный
// буфер модуля не трогается, разбор не заводится.
volatile uint8_t s_fx_handle     = 0;     // чей поток открыт, 0 - не сэмпл
volatile uint32_t s_fx_bytes     = 0;     // байт в текущем сэмпле
volatile bool s_fx_torn          = false; // байт потерян: сэмпл негодный целиком
uint32_t s_fx_at                 = 0;     // смещение текущего сэмпла в области
uint32_t s_fx_alloc              = 0;     // занято под сэмплы
uint32_t s_fx_write              = 0;     // куда пишет основной поток; догоняет s_fx_alloc
volatile bool s_fx_drop_req      = false; // сброс карты: выбросить принятое
volatile uint32_t s_fx_drop_head = 0;
uint32_t s_fx_loaded             = 0; // сэмплов принято за сеанс
uint32_t s_fx_lost               = 0; // сэмплов потеряно: не влезли или рвались
uint32_t s_fx_written            = 0; // байт переложено в PSRAM

// Сколько байт области уже в PSRAM - для FxPlayer: запуск ждёт байтов
// сэмпла. Старшие восемь разрядов - поколение сброса: его двигает
// обработчик на #F3 и тут же обнуляет счёт, а основной поток публикует
// новый счёт сравнением с обменом, только если поколение не сменилось.
// Иначе поток, вытесненный сбросом посреди переноса, вернул бы прежний
// большой счёт, и голос нового сэмпла сыграл бы байты прежнего.
std::atomic<uint32_t> s_fx_ready{0};
volatile uint32_t s_fx_epoch     = 0;
constexpr uint32_t kFxEpochShift = 24;
static_assert(kFxAreaBytes <= player::gs::kFxReadyBytesMask, "fx area must fit the ready counter");

// Попросить область GS. Здесь, а не на сбросе и не на любой записи в порт:
// плеер щупает порты, чтобы понять, есть ли GS, и это не должно ничего
// выделять и снимать играющий трек. Память просится на команде загрузки -
// модуля или сэмпла, - после которой пойдут байты.
void SOUNDSINTH_HOT_PATH(want_receive_buffer)() {
    s_gs_wants_reserve = true;
    if (!s_gs_reserved) {
        // Плеер придерживается битом занятости: снять движок и пересобрать
        // список страниц - не мгновенно, а кольцо приёма 4 КБ, начало модуля
        // терять нельзя.
        s_dev.hold_reserve_begin();
        s_reserve_hold = true;
    }
}

// Начат приём модуля (#30). Поток сэмпла эффекта сюда не идёт.
void SOUNDSINTH_HOT_PATH(on_stream_begin)() {
    ++s_streams_begun;
    s_ring_full_at_begin  = s_ring_full;
    s_writes_dat_at_begin = s_writes_dat;
    want_receive_buffer();
    request_drop();
}

// Эффекты: событие приходит и с кода команды, и с её хвоста (прямой запуск
// #88-#9B досылает ноту и громкость уже в регистр данных).
void SOUNDSINTH_HOT_PATH(apply_fx_event)(devices::gs::Event ev) {
    switch (ev) {
        case devices::gs::Event::FxPlay:
            fx_player().request_start(s_dev.fx_start());
            break;
        case devices::gs::Event::FxStop:
            fx_player().request_stop(s_dev.fx_stop_mask());
            break;
        case devices::gs::Event::FxSetNote:
            fx_player().request_note(s_dev.fx_start().channel, s_dev.fx_start().note);
            break;
        case devices::gs::Event::FxSetVolume:
            fx_player().request_volume(s_dev.fx_start().channel, s_dev.fx_start().volume);
            break;
        default:
            break;
    }
}

void SOUNDSINTH_HOT_PATH(on_stream_byte)(uint8_t b) {
    if (s_fx_handle != 0) {
        const uint32_t fx_head = s_fx_ring_head;
        const uint32_t fx_next = (fx_head + 1u) & kFxRingMask;
        if (fx_next == s_fx_ring_tail) {
            // Потерянный байт сдвигает весь хвост сэмпла: тише он не станет,
            // он станет другим звуком. Такой сэмпл не заводится вовсе.
            s_fx_torn = true;
            return;
        }
        s_fx_ring[fx_head] = b;
        s_fx_ring_head     = fx_next;
        ++s_fx_bytes;
        return;
    }
    const uint32_t head = s_ring_head;
    const uint32_t next = (head + 1u) & kRingMask;
    if (next == s_ring_tail) { // кольцо полно, байт теряется
        ++s_ring_full;
        return;
    }
    s_ring[head] = b;
    s_ring_head  = next;
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
    // Пока движок жив - просьба о сносе и следующий опрос; байты копятся в
    // кольце в SRAM, в PSRAM не пишется ни одного.
    // Хранилище меньше буфера: вычитание беззнаковое, и разность дала бы
    // около четырёх гигабайт. Отказ здесь внятный и один раз: карта
    // скажет, что модуль не влез, а плата продолжит играть то, что играла.
    if (shared::g_track_memory.psram.track_bytes < soundsinth::memory::kGsReceiveBytes) {
        if (!s_reserve_refused) {
            s_reserve_refused = true;
            debug_logf("gs: track storage %" PRIu32 " B is smaller than the receive buffer %" PRIu32 " B, no module can be received\n",
                       shared::g_track_memory.psram.track_bytes, static_cast<uint32_t>(soundsinth::memory::kGsReceiveBytes));
        }
        return false;
    }
    if (!shared::try_stop_engine_for_load()) return false;
    s_load_held = true;
    // Буфер - верхний мегабайт хранилища, под таблицами банка с карты,
    // если они есть.
    const uint32_t bytes = shared::g_track_memory.psram.track_bytes - soundsinth::memory::kGsReceiveBytes;
    soundsinth::memory::psram_set_track_bytes(shared::g_track_memory.psram, bytes);
    s_gs_offset   = bytes;
    s_gs_reserved = true;
    // Ужатие хранилища теряет отметки блоков, оказавшихся выше новой
    // границы. Молчать об этом нельзя: страницы потерянного блока потом
    // раздадут треку, а владелец продолжит в них писать - отказ будет
    // выглядеть как повреждённые данные паттерна, а не как нехватка места.
    debug_logf("gs: receive buffer carved out, track storage shrunk%s\n", shared::g_track_memory.psram.alloc.kept_lost != 0 ? ", PERMANENT BLOCK LOST" : "");
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
    const uint8_t data   = s_dev.peek_data();
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
    const uint8_t order = static_cast<uint8_t>(shared::g_order_pos.load(std::memory_order_relaxed) & 0xffu);
    const uint8_t row   = static_cast<uint8_t>(shared::g_row_pos.load(std::memory_order_relaxed) & 0x3fu);
    s_dev.set_position(order, row);
    switch (const devices::gs::Event ev = s_dev.write_command(data); ev) {
        case devices::gs::Event::StreamBegin:
            // Модуль и сэмпл эффекта приходят одним механизмом, и различить
            // их можно только по виду потока. Сэмпл приёмный буфер не
            // занимает: отрезать его под сэмпл значило бы снять играющий
            // модуль ради звука выстрела.
            if (s_dev.stream() == devices::gs::Stream::Module || s_dev.stream() == devices::gs::Stream::Midi) {
                // Файл .mid принимается тем же путём, что и модуль: это
                // файл, и ложится он в тот же буфер. Разница - в разборе.
                s_fx_handle   = 0;
                s_midi_stream = s_dev.stream() == devices::gs::Stream::Midi;
                on_stream_begin();
            } else {
                // Сэмплы ложатся в хвост области подряд, поэтому начало
                // нового - это конец предыдущего, и основному потоку
                // отдельно его сообщать не нужно: он пишет туда же, подряд.
                s_fx_handle = s_dev.fx_loading();
                s_fx_at     = s_fx_alloc;
                s_fx_bytes  = 0;
                s_fx_torn   = false;
                want_receive_buffer();
            }
            break;
        case devices::gs::Event::StreamEnd:
            if (s_fx_handle != 0) {
                // Длину знает только платформа: автомат байтов не считает.
                // Удержания у сэмпла нет - разбирать нечего, плеер идёт
                // дальше сразу.
                //
                // Место занимается ровно на принятые байты, даже у негодного
                // сэмпла: основной поток пишет их подряд и о негодности не
                // знает. Иначе его курсор разошёлся бы с нашим счётом, и
                // следующий сэмпл лёг бы со сдвигом.
                const uint32_t len = s_fx_bytes;
                const bool ok      = !s_fx_torn && len != 0 && s_fx_at + len <= kFxAreaBytes;
                s_dev.set_fx_length(s_fx_handle, s_fx_at, ok ? len : 0u);
                s_fx_alloc = s_fx_at + len;
                if (ok) {
                    ++s_fx_loaded;
                } else {
                    ++s_fx_lost;
                }
                s_fx_handle = 0;
                break;
            }
            if (s_midi_stream) {
                s_midi_stream  = false;
                s_midi_pending = true; // разбирать в основном потоке
                break;
            }
            s_module_ready = false;
            s_load_pending = true; // разбирать в основном потоке
            break;
        case devices::gs::Event::FxPlay:
        case devices::gs::Event::FxStop:
        case devices::gs::Event::FxSetNote:
        case devices::gs::Event::FxSetVolume:
            apply_fx_event(ev);
            break;
        case devices::gs::Event::Reset:
            // Голоса эффектов - первым делом: таблица сэмплов уже пуста, и
            // место под них отдаётся с нуля. Играющий голос читал бы то,
            // что вот-вот перепишет следующий сэмпл, а его канал у модуля
            // так и остался бы немым.
            fx_player().request_stop(devices::gs::kFxChannelMask);
            // Таблицу сэмплов автомат уже очистил - выбросить и байты.
            s_fx_handle    = 0;
            s_fx_alloc     = 0;
            s_fx_drop_head = s_fx_ring_head;
            s_fx_drop_req  = true;
            s_fx_epoch     = s_fx_epoch + 1u;
            s_resets       = s_resets + 1u;
            s_fx_ready.store(s_fx_epoch << kFxEpochShift, std::memory_order_release);
            on_reset();
            break;
        case devices::gs::Event::Play:
            // Через ту же точку, что трек с карты: смена поколения песни.
            // Пауза снимается здесь же: иначе остановленная карта осталась
            // бы немой и после новой команды играть.
            shared::g_playback_paused.store(false, std::memory_order_relaxed);
            if (s_module_ready) {
                shared::g_track_end_frame.store(0, std::memory_order_relaxed); // конец задаёт плеер GS
                shared::g_song_generation.fetch_add(1, std::memory_order_release);
            }
            break;
        case devices::gs::Event::Stop:
            // #32 Stop Module. Конца трека для GS нет, модуль играет до
            // команды, и остановить его может только она.
            //
            // Только при своём модуле. Эту же команду плеер шлёт при
            // опознании карты, до всякой загрузки - у настоящей карты
            // останавливать там нечего, а у нас флаг паузы общий с
            // проигрывателем платы, и взведённый он заглушил бы трек,
            // пущенный мимо GS.
            if (s_module_ready) shared::g_playback_paused.store(true, std::memory_order_relaxed);
            break;
        case devices::gs::Event::Resume:
            if (s_module_ready) shared::g_playback_paused.store(false, std::memory_order_relaxed);
            break;
        default:
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
    // Хвост прямого запуска созревает здесь: у записи в регистр данных
    // своего возврата под событие нет, автомат его откладывает.
    apply_fx_event(s_dev.take_event());
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

// Байты сэмплов из кольца в область эффектов. Границы сэмплов здесь не
// нужны: они ложатся подряд, и смещение каждого обработчик посчитал тем же
// сложением. Курсор виртуальный - он растёт и на байтах, которым места уже
// нет, иначе после переполнения области следующий сэмпл лёг бы со сдвигом.
void SOUNDSINTH_HOT_PATH(drain_fx_ring)() {
    // Поколение - до просьбы сброса: сброс между ними опознается при
    // публикации.
    const uint32_t epoch = s_fx_epoch;
    if (s_fx_drop_req) {
        s_fx_drop_req  = false;
        s_fx_ring_tail = s_fx_drop_head;
        s_fx_write     = 0;
    }
    uint32_t tail       = s_fx_ring_tail;
    const uint32_t head = s_fx_ring_head;
    uint32_t fill       = (head - tail) & kFxRingMask;
    if (fill == 0) return;
    uint8_t* const dst = gs_buffer() + kFxAreaAt;
    while (fill != 0) {
        const uint32_t to_end = kFxRingBytes - tail;
        const uint32_t run    = to_end < fill ? to_end : fill;
        if (s_fx_write < kFxAreaBytes) {
            const uint32_t room = kFxAreaBytes - s_fx_write;
            const uint32_t n    = room < run ? room : run;
            std::memcpy(dst + s_fx_write, s_fx_ring + tail, n);
            s_fx_written += n;
        }
        s_fx_write += run;
        tail        = (tail + run) & kFxRingMask;
        fill       -= run;
    }
    s_fx_ring_tail = tail;

    uint32_t seen = s_fx_ready.load(std::memory_order_relaxed);
    if ((seen >> kFxEpochShift) != (epoch & 0xffu) || s_fx_drop_req) return;
    const uint32_t bytes = s_fx_write < kFxAreaBytes ? s_fx_write : kFxAreaBytes;
    s_fx_ready.compare_exchange_strong(seen, ((epoch & 0xffu) << kFxEpochShift) | bytes, std::memory_order_release, std::memory_order_relaxed);
}

void SOUNDSINTH_HOT_PATH(drain_ring)() {
    apply_drop();
    uint32_t tail       = s_ring_tail;
    const uint32_t head = s_ring_head;
    const uint32_t now  = platform::mono_us();
    const uint32_t fill = (head - tail) & kRingMask;
    if (fill != 0) {
        if (!s_module_started) {
            s_module_started    = true;
            s_ring_peak         = 0;
            s_drain_gap_max_us  = 0;
            s_pio_lost_at_start = hal::bus_lost_port_writes();
        } else if (now - s_last_drain_us > s_drain_gap_max_us) {
            s_drain_gap_max_us = now - s_last_drain_us;
        }
        if (fill > s_ring_peak) s_ring_peak = fill;
        // Перелив под звук - как фоновая догрузка: заминки в его окне видны.
        shared::g_background_loading.store(true, std::memory_order_relaxed);
    }
    s_last_drain_us = now;
    // Кусками, а не побайтно. gs_buffer() живёт в другой единице трансляции,
    // LTO выключен - в теле побайтового цикла стоял живой вызов, и из-за него
    // компилятор перечитывал s_recv_len и смещение на каждом байте: около 18
    // команд там, где нужно копирование. На полном кольце это 270 мкс, на
    // модуле в мегабайт - десятки миллисекунд Core1.
    uint8_t* const dst = gs_buffer();
    uint32_t left      = s_recv_len < kBufferBytes ? kBufferBytes - s_recv_len : 0u;
    if (left > fill) left = fill;
    while (left != 0) {
        // Кольцо до конца буфера, потом остаток с начала: двух кусков хватает.
        const uint32_t to_end = kRingBytes - tail;
        const uint32_t run    = to_end < left ? to_end : left;
        std::memcpy(dst + s_recv_len, &s_ring[tail], run);
        s_recv_len += run;
        tail        = (tail + run) & kRingMask;
        left       -= run;
    }
    // Буфер полон, а байты ещё есть: модуль не влез, остаток не нужен.
    if (tail != head) {
        s_overflow = true;
        tail       = head;
    }
    s_ring_tail = tail;
    if (fill != 0) shared::g_background_loading.store(false, std::memory_order_relaxed);
    apply_drop(); // сброс во время копирования: скопированное - старый поток
}

// Вторая строка модуля: сэмплы, время разбора и приём. Печатается и при
// отказе - по ней видно, лёг ли модуль целиком.
SOUNDSINTH_NOINLINE void log_module_details(uint32_t parse_ms, bool parsed) {
    const auto& ls          = soundsinth::model::g_tracker_load_stats;
    const uint32_t bus_lost = hal::bus_lost_port_writes() - s_pio_lost_at_start;
    char m[320]; // внутри строки - причина отказа сэмпла, до 81 байта
    std::snprintf(m, sizeof(m),
                  "gs: module: samples %u, not stored %u%s%s%s, dropped %u, nonresident %u, parse %" PRIu32 " ms | ring peak %" PRIu32 " of %" PRIu32
                  ", drained once per %" PRIu32 " ms max, bus losses %" PRIu32 "\n",
                  parsed ? static_cast<unsigned>(shared::g_song.sample_count) : 0u, static_cast<unsigned>(ls.samples_failed),
                  ls.samples_failed ? " (first: " : "", ls.samples_failed ? (ls.first_failure ? ls.first_failure : "?") : "", ls.samples_failed ? ")" : "",
                  static_cast<unsigned>(ls.samples_dropped), static_cast<unsigned>(ls.samples_nonresident), parse_ms, s_ring_peak, kRingBytes,
                  s_drain_gap_max_us / 1000u, bus_lost);
    debug_log(m);
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
    debug_logf("gs: module NOT accepted: received %" PRIu32 " bytes, header says %" PRIu32 " - short by %" PRIu32 "\n", s_recv_len, source_end,
               source_end - s_recv_len);
}

// Сверка приёма по трём точкам одного пути: запись в порт, приём автоматом,
// укладка в буфер. Равные числа при недоборе по заголовкам означают, что
// байты не дошли до платы вовсе, а не потерялись внутри неё.
SOUNDSINTH_NOINLINE void log_stream_tally(uint32_t port_writes, uint32_t lost) {
    char m[192];
    std::snprintf(m, sizeof(m), "gs: stream tally: port writes %" PRIu32 ", device accepted %" PRIu32 ", stored %" PRIu32 ", ring lost %" PRIu32 "\n",
                  port_writes, s_dev.stream_bytes(), s_recv_len, lost);
    debug_log(m);
}

// Сумма принятого и суммы по кускам 128 КБ. Сверяется с подлинным файлом на
// ПК: совпали - дорога от карты до буфера отдала верные байты и недобор
// сделан источником; разошлись - виден кусок, в котором данные поехали.
// Куски, а не одна сумма: иначе расхождение известно, а место - нет.
SOUNDSINTH_NOINLINE void log_received_crc() {
    constexpr uint32_t kChunk = 128u * 1024u;
    const uint8_t* const p    = gs_buffer();
    char m[192];
    int off        = std::snprintf(m, sizeof(m), "gs: received crc32 by 128K chunk:");
    uint32_t total = soundsinth::util::kCrc32Init;
    for (uint32_t at = 0; at < s_recv_len; at += kChunk) {
        const uint32_t n = (s_recv_len - at) < kChunk ? (s_recv_len - at) : kChunk;
        total            = soundsinth::util::crc32_update(total, p + at, n);
        const uint32_t c = soundsinth::util::crc32(p + at, n);
        if (off > 0 && static_cast<size_t>(off) < sizeof(m)) {
            off += std::snprintf(m + off, sizeof(m) - static_cast<size_t>(off), " %08" PRIX32, c);
        }
    }
    if (off < 0 || static_cast<size_t>(off) >= sizeof(m) - 2u) off = static_cast<int>(sizeof(m)) - 2;
    m[off]     = '\n';
    m[off + 1] = '\0';
    debug_log(m);
    debug_logf("gs: received crc32 %08" PRIX32 " over %" PRIu32 " bytes\n", total, s_recv_len);
}

SOUNDSINTH_NOINLINE void log_module_rejected(uint32_t lost) {
    char m[192];
    std::snprintf(m, sizeof(m),
                  "gs: module NOT accepted: %" PRIu32 " bytes, overflow=%d (buffer %" PRIu32 "), lost in ring %" PRIu32 ", no engine until the next load\n",
                  s_recv_len, static_cast<int>(s_overflow), kBufferBytes, lost);
    debug_log(m);
}

SOUNDSINTH_NOINLINE void log_module_result(bool ok, const player::load::SessionLoadResult& load, uint32_t teardown_ms) {
    char m[192];
    if (ok) {
        std::snprintf(m, sizeof(m), "gs: module accepted, %" PRIu32 " bytes, channels %u, duration %" PRIu32 " s, engine teardown %" PRIu32 " ms\n", s_recv_len,
                      static_cast<unsigned>(shared::g_song.channel_count), load.total_frames / soundsinth::engine::kSampleRateHz, teardown_ms);
    } else {
        std::snprintf(m, sizeof(m), "gs: module NOT parsed (%" PRIu32 " bytes): %s\n", s_recv_len, load.error ? load.error : "(no reason)");
    }
    debug_log(m);
}

// Ожидание сноса движка: шина и карта обслуживаются, а затянувшееся
// ожидание видно в логе до того, как кончится.
constexpr uint32_t kSlowTeardownUs = 200000;

struct TeardownWait {
    uint32_t started_us;
    bool reported;
};

void serve_teardown_wait(void* user) {
    auto* w = static_cast<TeardownWait*>(user);
    player::serve_without_wait(nullptr);
    if (!w->reported && platform::mono_us() - w->started_us > kSlowTeardownUs) {
        w->reported = true;
        debug_log("gs: waiting for engine teardown over 200 ms\n");
    }
}

// Удержание разбора у автомата одно на все потоки. Новый #D2, пришедший
// во время разбора, взвёл его для себя - снимет его разбор того потока.
void release_parse_hold() {
    if (!s_load_pending && !s_midi_pending) s_dev.hold_parse_done();
}

SOUNDSINTH_NOINLINE void finish_module() {
    const uint32_t streams = s_streams_begun;
    const uint32_t resets  = s_resets;
    drain_ring();

    // Байты, не влезшие в кольцо, - дыра посреди модуля: MOD с нехваткой PCM
    // разбирается и играет мусором, а причина видна только по редкой строке
    // gs:. Отказ вместо мусора; обратного давления у потока нет.
    const uint32_t lost = s_ring_full - s_ring_full_at_begin;
    log_stream_tally(s_writes_dat - s_writes_dat_at_begin, lost);
    if (s_recv_len != 0) log_received_crc();
    if (s_overflow || s_recv_len == 0 || lost != 0) {
        log_module_rejected(lost);
        ++s_module_fails;
        if (lost != 0) ++s_fails_lost;
        if (s_overflow) ++s_fails_overflow;
        log_module_details(0, false);
        release_parse_hold();
        return;
    }

    // Разбор пишет Song и память трека, которые читает движок: сначала снять
    // его. Плеер всё это время придержан (#D2). Буфер разбор читает тем же
    // кэшируемым алиасом, которым его заполнил перелив: сброс кэша не нужен.
    shared::g_core1_phase.store(shared::Core1Phase::GsModule, std::memory_order_relaxed);
    TeardownWait wait{platform::mono_us(), false};
    shared::track_load_begin(&serve_teardown_wait, &wait);
    // Захват - парой со снятием в bridge_poll: резерв области уже есть с
    // прошлого потока, и try_reserve_receive_buffer s_load_held не ставит.
    // Без этого второй модуль подряд или модуль после сэмпла эффекта
    // оставляет запрет до загрузки трека с хоста.
    s_load_held                = true;
    const uint32_t teardown_ms = (platform::mono_us() - wait.started_us) / 1000u;
    soundsinth::formats::MemoryByteSource src(gs_buffer(), s_recv_len);
    player::load::SessionLoadResult load;
    // Тот же вызов, что грузит трек с карты. Своего разбора у эмуляции GS
    // нет: приходит обычный MOD, загрузчик выбирает формат по содержимому
    // (имени у потока нет).
    const uint32_t parse_started_us = platform::mono_us();
    const bool ok                   = player::load::run_session_load(src.as_byte_source(), shared::g_track_memory, shared::g_song, load,
                                                                     /*metadata_only=*/false);
    const uint32_t parse_ms         = (platform::mono_us() - parse_started_us) / 1000u;
    // Недобор: конец файла по его же заголовкам дальше принятого. У MOD
    // длины сэмплов точные, и это ловит третье место потери байта - очередь
    // записи шины, откуда байт до автомата не доходит и ни в одном счётчике
    // приёма не виден. Песня при этом разобрана, но части PCM в ней нет.
    const uint32_t source_end = soundsinth::model::g_tracker_load_stats.source_end;
    const bool short_module   = ok && source_end > s_recv_len;
    const bool good           = ok && !short_module;
    if (short_module) log_module_short(source_end);
    // Новый поток или сброс за время разбора - итог не нужен: образ и
    // поколение песни не ставятся, как при брошенном приёме.
    const bool current = s_streams_begun == streams && s_resets == resets;
    if (!current) debug_log("gs: module result dropped - reset or new stream during the parse\n");
    if (good && current) player::publish_file_info(load.total_frames, shared::g_song);
    s_module_ready = good && current;
    // Принятый модуль - источник паттернов для залитого резидента: плеер
    // просит их по номеру позиции, и отдаются они прямо из буфера.
    if (current) s_dev.set_module_image(good ? gs_buffer() : nullptr, good ? s_recv_len : 0u);
    if (good && current) {
        ++s_module_loads;
        shared::publish_loaded_track(0); // конец задаёт плеер GS
    } else {
        ++s_module_fails;
        if (short_module) ++s_fails_short;
    }
    log_module_result(good, load, teardown_ms);
    log_module_details(parse_ms, good);
    // Удержание разбора снимается только здесь: плеер всё это время ждал,
    // отпустить раньше - сказать, что готово, когда не готово.
    release_parse_hold();
}

// Принят файл .mid: разобрать и отдать автомату таблицы. Плеер сразу
// после этого шлёт пробу #02, поэтому до конца разбора он придержан битом
// команды - его отпускает hold_parse_done.
SOUNDSINTH_NOINLINE void finish_midi() {
    const uint32_t streams = s_streams_begun;
    const uint32_t resets  = s_resets;
    drain_ring();
    s_midi_len = s_recv_len;
    // Приём обязан быть целым, как у модуля: переполнение, потеря в кольце
    // или пустой поток - отказ. Файл длиннее kMidiWorkAt лёг бы под рабочую
    // область расписания, и её очистка стёрла бы его хвост. Нулевая длина
    // в begin() гасит прежний файл.
    const uint32_t lost   = s_ring_full - s_ring_full_at_begin;
    const bool intact     = !s_overflow && lost == 0 && s_midi_len != 0 && s_midi_len <= kMidiWorkAt;
    uint8_t* const buffer = gs_buffer();
    if (!intact) {
        debug_logf("gs: midi stream damaged - %" PRIu32 " B, overflow %u, lost %" PRIu32 "\n", s_midi_len, s_overflow ? 1u : 0u, lost);
    }
    const bool parsed = s_midi.begin(buffer, intact ? s_midi_len : 0u, s_dev.midi_facts(), buffer + kMidiWorkAt, s_dev.midi_state_buffer());
    // Сброс или новый поток за время разбора - таблицы не ставятся.
    const bool ok = parsed && s_streams_begun == streams && s_resets == resets;
    if (ok) {
        s_dev.set_midi_tables(s_midi.names(), s_midi.info());
        s_dev.set_midi_tracks(s_midi.track_flags(), s_midi.track_count());
        // Порядок важен: half собирается в next_frame(), и только после
        // неё frame() показывает на собранное. Порядок вычисления
        // аргументов одного вызова языком не задан, поэтому врозь.
        const uint16_t n = s_midi.next_frame();
        s_dev.set_midi_frame(s_midi.frame(), n);
    }
    // Звучит проигрыватель только живым путём: байты возвращаются на отвод
    // AY. Живой MIDI выключен настройками - на пробу #02 отвечаем "не
    // готов", и плеер отказывается от MIDI, не рисуя проигрывание без звука.
    const bool live_on = devices::config::settings().live_midi != static_cast<uint8_t>(soundsinth::config::LiveMidi::None);
    if (ok && !live_on) debug_log("gs: midi not offered - LIVE_MIDI=NONE, nothing would sound\n");
    // Буфер держит файл .mid, а не модуль: резиденту паттернов
    // отсюда не отдавать.
    s_dev.set_module_image(nullptr, 0);
    s_dev.set_midi_ready(ok && live_on);
    const devices::gs::MidiFileFacts& f = s_dev.midi_facts();
    debug_logf("gs: midi %s - %" PRIu32 " B, ppqn %u, tracks %u of %u, first at %u | len field %u, skipped %" PRIu32 ", stop at %" PRIu32 ", frame %" PRIu32
               " B\n",
               ok ? "accepted" : (parsed ? "dropped by reset" : "REJECTED"), s_midi_len, static_cast<unsigned>(f.division),
               static_cast<unsigned>(s_midi.track_count()), static_cast<unsigned>(f.track_count), static_cast<unsigned>(f.first_track),
               static_cast<unsigned>(f.file_length), s_midi.skipped(), s_midi.stopped_at(), s_midi.bytes_out());
    release_parse_hold();
}

} // namespace

void bridge_init() {
    s_dev.reset();
    s_dev.set_fx_capacity(kFxAreaBytes);
    s_dev.set_fx_end_source(fx_player().ended_seqs());

    devices::hal::z80_port_on_write(kPortGsCom, &gs_com_write);
    devices::hal::z80_port_on_write(kPortGsDat, &gs_dat_write);
    devices::hal::z80_port_on_read_done(kPortGsDat, &gs_dat_read_done);
    // Хук на чтение 0xbb не регистрируется: он нужен только счётчику чтений
    // состояния (st=- в строке gs:), на плате с ним не проверялось.
    publish();

    debug_log("gs: emulation attached at 0xbb (commands/status) and 0xb3 (data)\n");
}

void bridge_release_buffer() {
    // Запрос снимается в любом состоянии, и тот, что ещё ждёт сноса движка:
    // иначе следующий poll снёс бы движок, собранный из трека
    // платы, и отрезал его зоны. Принятое и незаконченное бросается: память
    // уходит треку. Модуль в памяти трека будет переписан загрузкой, #31
    // запускать нечего.
    request_drop();
    apply_drop();
    s_module_ready     = false;
    s_gs_wants_reserve = false;
    // Плеер ждал резерва, а его не будет: отпустить, иначе он встанет на
    // ожидании навсегда. Его модуль не примется (кольцо не разбирается без
    // резерва).
    if (s_reserve_hold) release_player_hold();
    // Область возвращается треку - приёма больше нет, и держать запрет
    // сборки движка незачем.
    if (s_load_held) {
        s_load_held = false;
        shared::track_load_end();
    }
    if (!s_gs_reserved) return;
    soundsinth::memory::psram_set_track_bytes(shared::g_track_memory.psram, shared::g_track_memory.psram.track_bytes + soundsinth::memory::kGsReceiveBytes);
    s_gs_reserved = false;
    // Сэмплы лежали в той же области: их больше нет - ни байтов, ни
    // записей автомата, ни открытого потока.
    fx_player().reset();
    fx_player().attach(nullptr, nullptr);
    s_dev.fx_forget_all();
    s_fx_handle    = 0;
    s_fx_alloc     = 0;
    s_fx_write     = 0;
    s_fx_ring_tail = s_fx_ring_head;
    s_fx_ready.store((s_fx_epoch & 0xffu) << kFxEpochShift, std::memory_order_release);
    debug_log("gs: receive buffer returned to the track\n");
}

void bridge_track_memory_taken() {
    s_module_ready = false;
    s_dev.set_module_image(nullptr, 0);
    publish();
}

uint32_t bridge_fx_mute_mask() {
    // У карты каналов четыре на всё: эффект отнимает канал у модуля, и то,
    // что там играло, замолкает. Настройка это отменяет - тогда эффект
    // звучит своими голосами, а музыка остаётся целой.
    if (devices::config::settings().gs_fx_separate != 0) return 0;
    return fx_player().active_mask();
}

bool bridge_wants_track_memory() {
    return s_gs_wants_reserve;
}

namespace {

// Телеметрия залитому резиденту: позиция, ноты и громкости каналов от
// наблюдателя тиков движка. Собирается потоком, а не обработчиком команды:
// блок состояния нужен только на его запросе, а сборка - вызов во флеш.
SOUNDSINTH_NOINLINE void publish_telemetry() {
    const uint8_t order = static_cast<uint8_t>(shared::g_order_pos.load(std::memory_order_relaxed) & 0xffu);
    const uint8_t row   = static_cast<uint8_t>(shared::g_row_pos.load(std::memory_order_relaxed) & 0x3fu);
    uint8_t notes[shared::kTelemetryChannels];
    uint8_t levels[shared::kTelemetryChannels];
    for (uint8_t c = 0; c < shared::kTelemetryChannels; ++c) {
        notes[c]  = shared::g_channel_note[c].load(std::memory_order_relaxed);
        levels[c] = shared::g_channel_volume[c].load(std::memory_order_relaxed);
    }
    s_dev.set_telemetry(order, row, notes, levels, shared::g_speed.load(std::memory_order_relaxed), s_module_ready);
}

} // namespace

void SOUNDSINTH_HOT_PATH(bridge_poll)() {
    publish_telemetry();
    if (!try_reserve_receive_buffer()) return;
    if (s_reserve_hold) release_player_hold();
    drain_ring();
    drain_fx_ring();
    // Звук и автомат сверяются здесь: громкости #2B и #3D едут с шины, а
    // занятость каналов решает, куда ляжет следующий сэмпл.
    fx_player().attach(&s_dev, gs_buffer() + kFxAreaAt, &s_fx_ready);
    fx_player().set_fx_volume(s_dev.fx_volume_effective());
    if (s_load_pending) {
        s_load_pending = false;
        finish_module();
        publish();
    }
    if (s_midi_pending) {
        s_midi_pending = false;
        finish_midi();
        publish();
    }
    // Порция очередного кадра готовится заранее: плеер спрашивает её раз в
    // кадр и читает сразу, считать в прерывании нечего.
    //
    // Только когда прошлая забрана. Иначе #00, пришедший посреди сборки,
    // отдал бы пустую порцию и взвёл просьбу снова, а следующий проход
    // собрал бы порцию поверх готовой, не отданной ни разу: до 51 байта
    // событий кадра, в том числе note off. Просьба при этом остаётся.
    if (s_dev.midi_frame_wanted() && !s_dev.midi_frame_pending() && s_midi.ready()) {
        s_dev.take_midi_frame_request();
        const uint16_t n = s_midi.next_frame();
        s_dev.set_midi_frame(s_midi.frame(), n);
    }
    // Запрет снимается по концу приёма, а не в разборе: у сэмпла эффекта
    // разбора нет. Взведённый запрет не пускает живой MIDI: тот видит "идёт
    // загрузка" и закрывает сеанс.
    if (s_load_held && !s_load_pending && !s_midi_pending && s_fx_handle == 0 && s_dev.stream() == devices::gs::Stream::None) {
        s_load_held = false;
        shared::track_load_end();
    }
}

namespace {

// Первые две сотни обращений к портам по порядку. Порядок обращений по
// гистограмме не виден.
// Печатает каждый всплеск, не больше kMaxDumps раз, когда обращения
// прекратились: запись заполнилась или с прошлого вызова не прибавилось
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
    static_assert(std::size(kLetter) == 4, "a letter for every kind of GsEv");
    int off = std::snprintf(line, size, "gs: prologue (%" PRIu32 "): ", n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t w  = static_cast<uint8_t>(s_prologue[i].what) & 3u;
        off             += std::snprintf(line + off, size - off, "%c%02X ", kLetter[w], static_cast<unsigned>(s_prologue[i].value));
        if (off > 100 || i + 1u == n) {
            std::snprintf(line + off, size - off, "\n");
            debug_log(line);
            off = std::snprintf(line, size, "gs:            ");
        }
    }
    debug_log("gs: C command, S status, d data from ZX, r data to ZX\n");

    // Запись не разоружается: после последней команды плеер читает порт
    // данных сотнями (259 раз подряд), а новый всплеск заряжается только
    // командой.
    //
    // Предисловие сбрасывается: оно нужно только для аргументов перед первой
    // командой.
    s_prologue_n    = 0;
    s_prologue_seen = 0;
    s_recent_n      = 0;
}

} // namespace

void bridge_log_stats() {
    if (!(s_writes_cmd || s_writes_dat || s_reads_dat)) return;
    // Одна строка за раз: debug_log копирует её в кольцо.
    char gm[208];
    std::snprintf(gm, sizeof(gm),
                  "gs: cmd=%" PRIu32 " dat=%" PRIu32 " rd=%" PRIu32 " st=- | stream %" PRIu32 " B, modules %" PRIu32 ", failures %" PRIu32
                  ", ring full %" PRIu32 " | last=%02X status=%02X unknown=%02X x%" PRIu32 "\n",
                  s_writes_cmd, s_writes_dat, s_reads_dat, s_dev.stream_bytes(), s_module_loads, s_module_fails, static_cast<uint32_t>(s_ring_full),
                  static_cast<unsigned>(s_dev.last_command()), static_cast<unsigned>(s_dev.read_status()), static_cast<unsigned>(s_dev.unknown_command()),
                  s_dev.unknown_count());
    debug_log(gm);
    // Экран плеера: резидент отдаёт блок состояния по #EE/#EF, а следом
    // ведёт поддиалог "позиция -> килобайт паттерна", который закрывается
    // #FF. Пустые паттерны на экране - это asks больше served.
    debug_logf("gs: resident %s | #EE x%u #EF x%u close #FF x%u | pattern asks %" PRIu32 " served %" PRIu32 ", last pos %02X -> pattern %02X | image %" PRIu32
               " B\n",
               s_dev.resident_active() ? "live" : "gone", static_cast<unsigned>(s_dev.command_count(devices::gs::kResidentState)),
               static_cast<unsigned>(s_dev.command_count(devices::gs::kResidentStateVolume)),
               static_cast<unsigned>(s_dev.command_count(devices::gs::kResidentNoPattern)), s_dev.pattern_asks(), s_dev.pattern_served(),
               static_cast<unsigned>(s_dev.last_pattern_pos()), static_cast<unsigned>(s_dev.last_pattern_num()), s_dev.module_len());

    // Управление проигрыванием. Конца трека для GS нет, поэтому
    // кольцо до первой #32 - норма; по счётчикам видно, шлёт ли плеер
    // останов вообще и когда именно.
    debug_logf("gs: transport play #31 x%u stop #32 x%u cont #33 x%u | paused %u\n", static_cast<unsigned>(s_dev.command_count(devices::gs::kCmdPlayModule)),
               static_cast<unsigned>(s_dev.command_count(devices::gs::kCmdStopModule)), static_cast<unsigned>(s_dev.command_count(devices::gs::kCmdContModule)),
               shared::g_playback_paused.load(std::memory_order_relaxed) ? 1u : 0u);

    // Проигрыватель MIDI. Печатается, только когда блоб запускали: по
    // длине и сумме видно, какой блоб прошёл мимо опознания, а по числу
    // порций - идёт ли расписание вообще.
    if (s_dev.last_blob_len() != 0) {
        debug_logf("gs: blob #%04X crc %08" PRIX32 " | midi active %u ready %u, tracks %u, frames %" PRIu32 " (empty %" PRIu32 "), bytes %" PRIu32 "\n",
                   static_cast<unsigned>(s_dev.last_blob_len()), s_dev.last_blob_crc(), s_dev.midi_active() ? 1u : 0u, s_midi.ready() ? 1u : 0u,
                   static_cast<unsigned>(s_midi.track_count()), s_dev.midi_frames_served(), s_dev.midi_frames_empty(), s_midi.bytes_out());
    }

    // Эффекты. Строка печатается, только когда сэмплы были - в обычной
    // работе ей неоткуда взяться, а буфер журнала лишнего не вмещает.
    // refused - #38 без места в таблице, waited - запуск ждал байтов.
    if (s_dev.fx_count() != 0) {
        const devices::gs::FxSample& fx = s_dev.fx_sample(s_dev.current_fx());
        debug_logf("gs: fx %u ok %" PRIu32 " lost %" PRIu32 " refused %" PRIu32 " waited %" PRIu32 ", %" PRIu32
                   " B | cur #%02X note %u vol %02X prio %02X seek %X/%X loop %u\n",
                   static_cast<unsigned>(s_dev.fx_count()), s_fx_loaded, s_fx_lost, s_dev.fx_refused(), fx_player().waited(), s_fx_written,
                   static_cast<unsigned>(s_dev.current_fx()), static_cast<unsigned>(fx.note), static_cast<unsigned>(fx.volume),
                   static_cast<unsigned>(fx.priority), static_cast<unsigned>(fx.seek_first), static_cast<unsigned>(fx.seek_last), fx.loop ? 1u : 0u);
    }

    // Данные блока состояния. Позиция обязана идти вперёд: стоячая
    // позиция значит, что плеер паттерн перезапрашивать не будет вовсе.
    // sent - ноты и уровни, как они уходят в карту; engine - сырые от движка.
    debug_logf("gs: telemetry order %02X row %02X speed %02X | sent notes %02X %02X %02X %02X levels %02X %02X %02X %02X | engine %02X %02X %02X %02X\n",
               static_cast<unsigned>(shared::g_order_pos.load(std::memory_order_relaxed) & 0xffu),
               static_cast<unsigned>(shared::g_row_pos.load(std::memory_order_relaxed) & 0xffu),
               static_cast<unsigned>(shared::g_speed.load(std::memory_order_relaxed)), static_cast<unsigned>(s_dev.telemetry_byte(2)),
               static_cast<unsigned>(s_dev.telemetry_byte(3)), static_cast<unsigned>(s_dev.telemetry_byte(4)), static_cast<unsigned>(s_dev.telemetry_byte(5)),
               static_cast<unsigned>(s_dev.telemetry_byte(6)), static_cast<unsigned>(s_dev.telemetry_byte(7)), static_cast<unsigned>(s_dev.telemetry_byte(8)),
               static_cast<unsigned>(s_dev.telemetry_byte(9)), static_cast<unsigned>(shared::g_channel_note[0].load(std::memory_order_relaxed)),
               static_cast<unsigned>(shared::g_channel_note[1].load(std::memory_order_relaxed)),
               static_cast<unsigned>(shared::g_channel_note[2].load(std::memory_order_relaxed)),
               static_cast<unsigned>(shared::g_channel_note[3].load(std::memory_order_relaxed)));

    // Разбивка отказов - своей строкой и только когда они есть: буфер выше
    // её не вмещает, а в исправной работе печатать нечего.
    if (s_module_fails != 0) {
        debug_logf("gs: failures - hole %" PRIu32 ", cut %" PRIu32 ", short %" PRIu32 "\n", s_fails_lost, s_fails_overflow, s_fails_short);
    }

    // Четыре самые частые команды за один проход: каждый счётчик читается
    // один раз (растут в обработчике записи на Core1, пока здесь идёт
    // обход). По убыванию числа, при равенстве меньший код раньше.
    constexpr uint32_t kTop = 4;
    uint16_t top_n[kTop]    = {};
    uint8_t top_c[kTop]     = {};
    uint32_t count          = 0;
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
    int off = std::snprintf(gm, sizeof(gm), "gs: most frequent:");
    for (uint32_t i = 0; i < count && off < static_cast<int>(sizeof(gm)); ++i) {
        off += std::snprintf(gm + off, sizeof(gm) - off, " %02X x%u", static_cast<unsigned>(top_c[i]), static_cast<unsigned>(top_n[i]));
    }
    if (off < static_cast<int>(sizeof(gm))) std::snprintf(gm + off, sizeof(gm) - off, "\n");
    debug_log(gm);

    dump_prologue_if_settled(gm, sizeof(gm));
}

} // namespace player::gs
