// SPDX-License-Identifier: MIT
// Эмуляция General Sound: порты GS на шине, приём модуля в
// PSRAM, разбор общим загрузчиком трека.

#include "player/gs/bridge.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/memory.h"
#include "platform/mono_time.h"

#include "core/engine/engine_defs.h"
#include "core/formats/load_stats.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/psram_store.h"

#include "devices/gs/gs_device.h"
#include "devices/hal/z80_ports.h"

#include "player/config/config_service.h"
#include "player/gs/fx_player.h"
#include "player/gs/midi_sched.h"
#include "player/gs/rx_layout.h"
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
struct RxRing {
    uint8_t ring[kRingBytes] = {};
    volatile uint32_t head   = 0; // пишет приём байта
    volatile uint32_t tail   = 0; // читает основной поток
    volatile uint32_t full   = 0; // сколько байт потеряно на переполнении
    // Снимок счётчика на начале модуля: разность говорит, что в этом модуле
    // дыра.
    volatile uint32_t full_at_begin = 0;
    uint32_t len                    = 0;     // сколько уже переложено в PSRAM
    bool overflow                   = false; // модуль не влез в буфер
    // Просьба бросить принятое: снимок головы, его применяет drain_ring
    // (request_drop).
    volatile uint32_t drop_head = 0;
    volatile bool drop_req      = false;
};
RxRing s_rx;

// Память GS - отмеченные блоки распределителя, смена трека их не отбирает:
//  - область эффектов, пока говорит плеер GS;
//  - приём: куда ложится поток модуля или файла .mid, 896 КБ;
//  - принятый модуль: приём после разбора, ужатый до длины модуля. Из него
//    резидент получает паттерны; живёт, пока не разобран следующий.
// Первый резерв берёт эффекты и приём разом; приём под следующий модуль -
// из свободных страниц, под играющим прежним.
struct Memory {
    soundsinth::memory::PsramBlock fx;
    soundsinth::memory::PsramBlock rx;
    soundsinth::memory::PsramBlock live;
    volatile bool wanted    = false; // ставит начало потока
    volatile bool rx_wanted = false; // поток модуля или .mid ждёт приёма
    // Взят ли запрет сборки движка: берётся вместе с областью и разбором
    // модуля, снимается, когда принимать и разбирать больше нечего.
    bool load_held = false;
    // Отказ в резерве печатается один раз: опрос идёт на каждом витке Core1.
    bool refused       = false;
    volatile bool hold = false; // плеер придержан битом занятости
};
Memory s_mem;
uint8_t* gs_buffer() {
    return soundsinth::memory::psram_block_ptr(shared::g_track_memory.psram, s_mem.rx);
}
uint8_t* fx_area() {
    return soundsinth::memory::psram_block_ptr(shared::g_track_memory.psram, s_mem.fx);
}
// У исходной карты под эффекты отведено 112 КБ, игры заливают единицы
// сэмплов; 128 КБ с запасом. Приёму 896 КБ - четырёхканальному MOD этого
// много больше, чем нужно.
constexpr uint32_t kFxAreaBytes = 128u * 1024u;
constexpr uint32_t kBufferBytes = soundsinth::memory::kGsReceiveBytes - kFxAreaBytes; // потолок файла

// Своё кольцо под сэмплы, а не общее с модулем. Общее потребовало бы метить
// байты принадлежностью: пока основной поток доберётся до кольца, поток
// успевает смениться, и хвост сэмпла ушёл бы в модуль. Цена разделения -
// 2 КБ SRAM, приём модуля не меняется.
constexpr uint32_t kFxRingBytes = 2048;

// Рабочая область проигрывателя MIDI - в хвосте той части, куда ложится
// файл: имена дорожек, справка и две порции кадра. Файл длиннее kMidiWorkAt
// не принимается.
constexpr uint32_t kMidiWorkBytes = 4096;
constexpr uint32_t kMidiWorkAt    = kBufferBytes - kMidiWorkBytes;
static_assert(kMidiWorkBytes >= gs::kMidiWorkSize, "the MIDI work area is smaller than its layout");

MidiSched s_midi;

// Принятое ждёт разбора в основном потоке.
struct Parse {
    volatile bool midi_stream  = false; // идёт приём .mid, а не модуля
    volatile bool midi_pending = false; // .mid принят
    uint32_t midi_len          = 0;
    volatile bool load_pending = false; // модуль принят, #D2 пришёл
    // Последний модуль разобран: #31 есть что запускать. После отказа песня
    // в памяти недогружена, собирать из неё движок нельзя. Снимает #D2 (от
    // него до итога разбора #31 ничего не запускает), ставит finish_module.
    volatile bool module_ready = false;
    // Сколько потоков модуля начато (#30). Поток, начатый во время
    // разбора, делает итог разбора устаревшим: module_ready не ставится.
    volatile uint32_t streams_begun = 0;
    // Сбросы карты (#F3/#F4). Разбор идёт сотни миллисекунд, и сброс
    // посреди него отменяет итог: машина после сброса не должна услышать
    // модуль, принятый до него.
    volatile uint32_t resets = 0;
};
Parse s_parse;

constexpr uint32_t kFxRingMask = kFxRingBytes - 1;

// Приём - файл и добивка раскладки: потолок файла раскладка не уменьшает.
constexpr uint32_t kRxBytes = kBufferBytes + RxLayout::kPadBytes;
RxLayout s_lay;

// Загрузчику - файл через раскладку.
struct RxSource {
    const uint8_t* base;
    uint16_t first_page;
    uint32_t pos;

    static uint32_t read(void* self, void* dst, uint32_t n) {
        auto* s       = static_cast<RxSource*>(self);
        auto* out     = static_cast<uint8_t*>(dst);
        uint32_t done = 0;
        while (done < n && s->pos < s_rx.len) {
            uint32_t room       = 0;
            const uint32_t at   = s_lay.place(s->pos, &room);
            const uint32_t left = s_rx.len - s->pos;
            uint32_t k          = n - done;
            if (k > room) k = room;
            if (k > left) k = left;
            std::memcpy(out + done, s->base + at, k);
            done   += k;
            s->pos += k;
        }
        return done;
    }
    static bool seek(void* self, uint32_t offset) {
        if (offset > s_rx.len) return false;
        static_cast<RxSource*>(self)->pos = offset;
        return true;
    }
    static uint32_t size(void*) { return s_rx.len; }
    // Сэмпл целиком внутри своего отрезка и с его начала - страницы приёма.
    static uint16_t pages_at(void* self, uint32_t offset, uint32_t n) {
        uint32_t room = 0, sample = RxLayout::kSamples;
        const uint32_t at     = s_lay.place(offset, &room, &sample);
        const uint32_t len    = sample < RxLayout::kSamples ? s_lay.sample_bytes(sample) : 0u;
        const bool page_start = (at & (soundsinth::memory::kPsramPageBytes - 1u)) == 0u;
        if (sample == RxLayout::kSamples || room != len || !page_start || n > room || offset + n > s_rx.len) return soundsinth::memory::kPsramNoPage;
        return static_cast<uint16_t>(static_cast<RxSource*>(self)->first_page + at / soundsinth::memory::kPsramPageBytes);
    }
    soundsinth::formats::ByteSource as_byte_source() { return {this, &read, &seek, &size, &pages_at}; }
};
// bridge_init был: порты GS наши. Без него (GS=0) мост на шину не
// отвечает - порты бывают у настоящего GS, NeoGS или divIDE.
bool s_attached = false;

// Счётчики журнала.
struct Stats {
    uint32_t writes_cmd = 0;
    uint32_t writes_dat = 0;
    // Записей в GSDAT на начало потока. Разница с концом - сколько байт
    // машина подала в порт за модуль, и это единственное число, снятое до
    // автомата: по нему недобор делится на "прислали мало" и "потеряли у
    // себя".
    uint32_t writes_dat_at_begin = 0;
    uint32_t reads_dat           = 0;
    uint32_t module_loads        = 0;
    uint32_t module_fails        = 0;
    // Отказы по причинам: следствия у них разные, а лечатся они в разных
    // местах. Потеря в кольце - дыра посреди модуля, переполнение -
    // обрезанный модуль, недобор по заголовкам - байт, пропавший в очереди
    // записи шины, и ни один счётчик приёма его не видит. Сумма меньше
    // module_fails на пустой приём и на неразобранный модуль.
    uint32_t fails_lost     = 0;
    uint32_t fails_overflow = 0;
    uint32_t fails_short    = 0;
    // Приём модуля от первого байта до #D2: пик заполнения кольца,
    // наибольший промежуток между переливами при идущем потоке и снимок
    // счётчика потерь шины (разность - в строку модуля).
    bool module_started        = false;
    uint32_t ring_peak         = 0;
    uint32_t drain_gap_max_us  = 0;
    uint32_t last_drain_us     = 0;
    uint32_t pio_lost_at_start = 0;
};
Stats s_stats;

// Бросить принятое. Просьбой, а не сразу: #F3 приходит и из serve_bus
// посреди разбора модуля, и немедленный сброс обнулил бы s_rx.len под
// читающим RxSource. Просьбу - снимок головы кольца - применяет
// drain_ring; байты после снимка - уже новый поток. s_parse.load_pending
// гасится здесь же: его взводит разбор команд, и порядок #D2 и #F3
// сохраняется.

SOUNDSINTH_ALWAYS_INLINE void request_drop() {
    s_rx.drop_head = s_rx.head;
    s_rx.drop_req  = true;
    // Разбор брошен вместе с принятым: снять его удержание здесь, иначе не
    // снимет никто и плеер останется придержанным навсегда.
    if (s_parse.load_pending) s_dev.hold_parse_done();
    s_parse.load_pending = false;
    // Приёмный буфер вот-вот перестанет быть модулем: снять его как
    // источник паттернов, иначе резидент отдал бы мусор.
    s_dev.set_module_image(nullptr, 0);
}

// Основной поток. Флаг снимается раньше чтения снимка: сброс, пришедший
// между ними, не теряется.
void apply_drop() {
    if (!s_rx.drop_req) return;
    s_rx.drop_req = false;
    s_rx.tail     = s_rx.drop_head;
    s_rx.len      = 0;
    s_rx.overflow = false;
    s_lay.reset();
    s_stats.module_started = false; // поток начат заново: счёт приёма - с его первого байта
}

// --- События автомата ---

// Поток сэмпла эффекта (#38/#3E). Байты идут в хвост области GS, приёмный
// буфер модуля не трогается, разбор не заводится.
struct FxRx {
    uint8_t ring[kFxRingBytes] = {};
    volatile uint32_t head     = 0;     // пишет приём байта
    volatile uint32_t tail     = 0;     // читает основной поток
    volatile uint8_t handle    = 0;     // чей поток открыт, 0 - не сэмпл
    volatile uint32_t received = 0;     // байт в текущем сэмпле
    volatile bool torn         = false; // байт потерян: сэмпл негодный целиком
    uint32_t at                = 0;     // смещение текущего сэмпла в области
    uint32_t alloc             = 0;     // занято под сэмплы
    uint32_t write             = 0;     // куда пишет основной поток; догоняет alloc
    uint32_t loaded            = 0;     // сэмплов принято за сеанс
    uint32_t lost              = 0;     // сэмплов потеряно: не влезли или рвались
    uint32_t written           = 0;     // байт переложено в PSRAM
    // Сколько байт области уже в PSRAM: запуск эффекта ждёт байтов сэмпла.
    // Пишет только этот поток - перенос и сброс; сброс посреди переноса
    // не случается.
    std::atomic<uint32_t> ready{0};
};
FxRx s_fxrx;

// Попросить область GS. Здесь, а не на сбросе и не на любой записи в порт:
// плеер щупает порты, чтобы понять, есть ли GS, и это не должно ничего
// выделять и снимать играющий трек. Память просится на команде загрузки -
// модуля или сэмпла, - после которой пойдут байты. stream - поток модуля
// или .mid: ему нужен ещё и приём.
void want_receive_buffer(bool stream) {
    s_mem.wanted = true;
    if (stream) s_mem.rx_wanted = true;
    if (!s_mem.fx.valid() || (stream && !s_mem.rx.valid())) {
        // Плеер придерживается битом занятости: снять движок и пересобрать
        // список страниц - не мгновенно, а кольцо приёма 4 КБ, начало модуля
        // терять нельзя.
        s_dev.hold_reserve_begin();
        s_mem.hold = true;
    }
}

// Начат приём модуля (#30). Поток сэмпла эффекта сюда не идёт.
void on_stream_begin() {
    ++s_parse.streams_begun;
    s_rx.full_at_begin          = s_rx.full;
    s_stats.writes_dat_at_begin = s_stats.writes_dat;
    want_receive_buffer(true);
    request_drop();
    // Пауза - состояние прежнего трека. Останется взведённой - новый
    // зазвучит в тишину, и причины на экране видно не будет.
    shared::g_playback_paused.store(false, std::memory_order_relaxed);
}

// Эффекты: событие приходит и с кода команды, и с её хвоста (прямой запуск
// #88-#9B досылает ноту и громкость уже в регистр данных).
void apply_fx_event(devices::gs::Event ev) {
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
    if (s_fxrx.handle != 0) {
        const uint32_t fx_head = s_fxrx.head;
        const uint32_t fx_next = (fx_head + 1u) & kFxRingMask;
        if (fx_next == s_fxrx.tail) {
            // Потерянный байт сдвигает весь хвост сэмпла: тише он не станет,
            // он станет другим звуком. Такой сэмпл не заводится вовсе.
            s_fxrx.torn = true;
            return;
        }
        s_fxrx.ring[fx_head] = b;
        s_fxrx.head          = fx_next;
        ++s_fxrx.received;
        return;
    }
    const uint32_t head = s_rx.head;
    const uint32_t next = (head + 1u) & kRingMask;
    if (next == s_rx.tail) { // кольцо полно, байт теряется
        ++s_rx.full;
        return;
    }
    s_rx.ring[head] = b;
    s_rx.head       = next;
}

// --- Память GS по требованию ---
//
// Нужна только пока с картой GS работает плеер. Одновременно GS и .mid не
// играют, держать её всегда - отнимать у каждого трека.
//
// Первый резерв - по команде загрузки модуля или сэмпла (#30, #38):
// детектирование не должно выделять память и снимать трек. Между модулями
// эффекты и приём не возвращаются - плеер шлёт их пачками, - а
// возвращаются, когда трек запускают через протокол платы
// (release_buffer): тогда GS больше не говорит.
//
// Первый резерв пересобирает блок трека, а трек играет из страниц, которых
// после пересборки нет. Поэтому сначала движок снимается тем же путём, что
// при смене трека (g_teardown_requested), и только когда Core0 подтвердил
// (g_engine_alive == false), блоки берутся. Плеер всё это время придержан
// битом занятости. Приём под следующий модуль берётся из свободных страниц
// под играющим прежним, и снос нужен, только если подряд их нет.

// Принятый модуль больше не нужен: прежний трек снят или память ушла.
void drop_live() {
    if (!s_mem.live.valid()) return;
    s_dev.set_module_image(nullptr, 0);
    soundsinth::memory::psram_drop_kept(shared::g_track_memory.psram, s_mem.live);
}

void release_player_hold();
void release_parse_hold();

// Резерва не будет: просьба памяти снимается, плеер отпускается, запрет
// сборки движка - тоже. Иначе плеер висит на бите занятости, а движок и
// живой MIDI стоят до загрузки трека с хоста; модуль потока отвергается
// на его конце.
SOUNDSINTH_NOINLINE void give_up_reserve() {
    s_mem.wanted    = false;
    s_mem.rx_wanted = false;
    if (s_mem.hold) release_player_hold();
    if (s_mem.load_held) {
        s_mem.load_held = false;
        shared::track_load_end();
    }
}

// Отказ печатается один раз: опрос идёт на каждом витке Core1.
SOUNDSINTH_NOINLINE void refuse(const char* why) {
    give_up_reserve();
    if (s_mem.refused) return;
    s_mem.refused = true;
    debug_logf("gs: %s NOT taken - no run of pages or the keep list is full\n", why);
}

// Во флеше: нужен раз на сеанс GS, а опрос в SRAM проверяет флаги сам.
SOUNDSINTH_NOINLINE bool try_reserve_receive_buffer() {
    // Распределять нечего даже без трека: отказ один раз и внятный, трек
    // играет дальше.
    soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    if (soundsinth::memory::psram_unkept_bytes(psram) < kFxAreaBytes + kRxBytes) {
        if (!s_mem.refused) {
            s_mem.refused = true;
            debug_logf("gs: %" PRIu32 " B of PSRAM left for tracks, less than the receive buffer %" PRIu32 " B, no module can be received\n",
                       soundsinth::memory::psram_unkept_bytes(psram), kFxAreaBytes + kRxBytes);
        }
        give_up_reserve();
        return false;
    }
    // Пока движок жив - просьба о сносе и следующий опрос; байты копятся в
    // кольце в SRAM, в PSRAM не пишется ни одного.
    if (!shared::try_stop_engine_for_load()) return false;
    s_mem.load_held = true;
    drop_live();
    s_mem.fx = soundsinth::memory::psram_take_kept(psram, kFxAreaBytes);
    if (!s_mem.rx.valid()) s_mem.rx = soundsinth::memory::psram_take_kept(psram, kRxBytes);
    if (!s_mem.fx.valid() || !s_mem.rx.valid()) {
        soundsinth::memory::psram_drop_kept(psram, s_mem.fx);
        soundsinth::memory::psram_drop_kept(psram, s_mem.rx);
        refuse("receive buffer");
        return false;
    }
    debug_logf("gs: memory taken, effects at page %u, receive at page %u\n", static_cast<unsigned>(s_mem.fx.first), static_cast<unsigned>(s_mem.rx.first));
    return true;
}

// Приём под следующий поток: свободные страницы под играющим модулем, а
// подряд их нет - снос трека, как у первого резерва.
SOUNDSINTH_NOINLINE bool take_rx_block() {
    soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    s_mem.rx                              = soundsinth::memory::psram_alloc_kept(psram, kRxBytes);
    if (s_mem.rx.valid()) return true;
    if (!shared::try_stop_engine_for_load()) return false;
    s_mem.load_held = true;
    drop_live();
    s_mem.rx = soundsinth::memory::psram_take_kept(psram, kRxBytes);
    if (s_mem.rx.valid()) return true;
    refuse("receive block");
    return false;
}

// Автомат сброшен (команда сброса или сброс машины).
void card_reset() {
    // Голоса эффектов - первым делом: таблица сэмплов уже пуста, и место
    // под них отдаётся с нуля. Играющий голос читал бы то, что вот-вот
    // перепишет следующий сэмпл, а его канал у модуля так и остался бы
    // немым.
    fx_player().request_stop(devices::gs::kFxChannelMask);
    // Таблицу сэмплов автомат уже очистил - выбросить и байты.
    s_fxrx.handle  = 0;
    s_fxrx.alloc   = 0;
    s_fxrx.write   = 0;
    s_fxrx.tail    = s_fxrx.head;
    s_parse.resets = s_parse.resets + 1u;
    s_fxrx.ready.store(0, std::memory_order_release);
    request_drop(); // принятое не нужно
    // Модуль сброшенной карты не играет и #31 его не поднимет: карта
    // сбрасывается, как настоящая. Пауза - только своему треку: у трека
    // платы она остаётся.
    if (s_parse.module_ready) {
        s_parse.module_ready = false;
        shared::g_playback_paused.store(false, std::memory_order_relaxed);
        shared::forget_built_track();
    }
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

// Очередь записей в порты. Обработчик порта кладёт байт и взводит разряд
// состояния: команды - D0, данных - D7. Разбирает основной поток, по
// порядку прихода; запись уходит из очереди после своего разбора, и пока
// очередь не пуста, обработчик автомат не трогает. Байт открытого потока
// идёт мимо очереди, в кольцо приёма.
constexpr uint32_t kBusQueueLen = 32;     // степень двойки
constexpr uint16_t kBusCommand  = 0x100u; // запись в регистр команд, иначе - данных
struct BusQueue {
    uint16_t entries[kBusQueueLen] = {};
    volatile uint32_t head         = 0; // пишет обработчик
    volatile uint32_t tail         = 0; // пишет основной поток
    volatile uint32_t cmd_in       = 0; // счёт по регистрам: пришло - обработчик,
    volatile uint32_t dat_in       = 0; // разобрано - основной поток
    volatile uint32_t cmd_out      = 0;
    volatile uint32_t dat_out      = 0;
    volatile uint32_t pub_seq      = 0; // обработчик менял клетки портов
    volatile uint8_t status_out    = 0; // состояние автомата на последней публикации
    uint32_t lost                  = 0; // записей мимо полной очереди
};
BusQueue s_busq;

SOUNDSINTH_ALWAYS_INLINE uint8_t busq_bits() {
    return static_cast<uint8_t>((s_busq.cmd_in != s_busq.cmd_out ? devices::gs::kCommandBit : 0u) |
                                (s_busq.dat_in != s_busq.dat_out ? devices::gs::kDataBit : 0u));
}

// Обработчик порта вытесняет основной поток посреди публикации и кладёт
// клетки свежее: основной поток тогда публикует заново.
inline void SOUNDSINTH_HOT_PATH(publish)() {
    uint32_t seq = 0;
    do {
        seq                  = s_busq.pub_seq;
        const uint8_t status = s_dev.read_status();
        const uint8_t data   = s_dev.peek_data();
        s_busq.status_out    = status;
        devices::hal::z80_port_set_read(kPortGsDat, data);
        devices::hal::z80_port_set_read(kPortGsCom, static_cast<uint8_t>(status | busq_bits()));
    } while (seq != s_busq.pub_seq);
}

// Из обработчика записи порта. Разряд в клетке состояния нужен к первому
// чтению состояния после OUT.
SOUNDSINTH_ALWAYS_INLINE void busq_push(uint16_t entry) {
    const uint32_t head = s_busq.head;
    if (head - s_busq.tail >= kBusQueueLen) {
        ++s_busq.lost;
        return;
    }
    s_busq.entries[head & (kBusQueueLen - 1u)] = entry;
    s_busq.head                                = head + 1u;
    if ((entry & kBusCommand) != 0) {
        s_busq.cmd_in = s_busq.cmd_in + 1u;
    } else {
        s_busq.dat_in = s_busq.dat_in + 1u;
    }
    s_busq.pub_seq = s_busq.pub_seq + 1u;
    devices::hal::z80_port_set_read(kPortGsCom, static_cast<uint8_t>(s_busq.status_out | busq_bits()));
}

void SOUNDSINTH_HOT_PATH(gs_com_write)(uint8_t, uint8_t data) {
    busq_push(static_cast<uint16_t>(kBusCommand | data));
    ++s_stats.writes_cmd;
}

// Команда из очереди, основной поток.
void on_bus_command(uint8_t data) {
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
                s_fxrx.handle       = 0;
                s_parse.midi_stream = s_dev.stream() == devices::gs::Stream::Midi;
                on_stream_begin();
            } else {
                // Сэмплы ложатся в хвост области подряд, поэтому начало
                // нового - это конец предыдущего, и основному потоку
                // отдельно его сообщать не нужно: он пишет туда же, подряд.
                s_fxrx.handle   = s_dev.fx_loading();
                s_fxrx.at       = s_fxrx.alloc;
                s_fxrx.received = 0;
                s_fxrx.torn     = false;
                want_receive_buffer(false);
            }
            break;
        case devices::gs::Event::StreamEnd:
            if (s_fxrx.handle != 0) {
                // Длину знает только платформа: автомат байтов не считает.
                // Удержания у сэмпла нет - разбирать нечего, плеер идёт
                // дальше сразу.
                //
                // Место занимается ровно на принятые байты, даже у негодного
                // сэмпла: основной поток пишет их подряд и о негодности не
                // знает. Иначе его курсор разошёлся бы с нашим счётом, и
                // следующий сэмпл лёг бы со сдвигом.
                const uint32_t len = s_fxrx.received;
                const bool ok      = !s_fxrx.torn && len != 0 && s_fxrx.at + len <= kFxAreaBytes;
                s_dev.set_fx_length(s_fxrx.handle, s_fxrx.at, ok ? len : 0u);
                s_fxrx.alloc = s_fxrx.at + len;
                if (ok) {
                    ++s_fxrx.loaded;
                } else {
                    ++s_fxrx.lost;
                }
                s_fxrx.handle = 0;
                break;
            }
            if (!s_mem.rx.valid() && !s_mem.rx_wanted) {
                // Резерва не было: разбирать нечего, плеер получает отказ.
                s_parse.midi_stream  = false;
                s_parse.module_ready = false;
                ++s_stats.module_fails;
                release_parse_hold();
                break;
            }
            if (s_parse.midi_stream) {
                s_parse.midi_stream  = false;
                s_parse.midi_pending = true; // разбирать в основном потоке
                break;
            }
            s_parse.module_ready = false;
            s_parse.load_pending = true; // разбирать в основном потоке
            break;
        case devices::gs::Event::FxPlay:
        case devices::gs::Event::FxStop:
        case devices::gs::Event::FxSetNote:
        case devices::gs::Event::FxSetVolume:
            apply_fx_event(ev);
            break;
        case devices::gs::Event::Reset:
            card_reset();
            break;
        case devices::gs::Event::Play:
            // Через ту же точку, что трек с карты: смена поколения песни.
            // Пауза снимается здесь же, но только у своего модуля: флаг
            // общий с треком платы, и #31 при опознании карты снял бы его
            // паузу.
            if (s_parse.module_ready) {
                shared::g_playback_paused.store(false, std::memory_order_relaxed);
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
            if (s_parse.module_ready) shared::g_playback_paused.store(true, std::memory_order_relaxed);
            break;
        case devices::gs::Event::Resume:
            if (s_parse.module_ready) shared::g_playback_paused.store(false, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

// Запись в регистр данных из очереди, основной поток.
void on_bus_data(uint8_t data) {
    if (s_dev.write_data(data)) {
        on_stream_byte(data);
    }
    // Хвост прямого запуска созревает здесь: у записи в регистр данных
    // своего возврата под событие нет, автомат его откладывает.
    apply_fx_event(s_dev.take_event());
}

// Разбор очереди, основной поток. Хвост двигается после разбора записи.
// Запись разобрана или отменена: снять её разряд и убрать из очереди.
void busq_retire(uint32_t tail, uint16_t entry) {
    if ((entry & kBusCommand) != 0) {
        s_busq.cmd_out = s_busq.cmd_out + 1u;
    } else {
        s_busq.dat_out = s_busq.dat_out + 1u;
    }
    s_busq.tail = tail + 1u;
}

SOUNDSINTH_NOINLINE void drain_bus_queue() {
    while (s_busq.tail != s_busq.head) {
        const uint32_t tail  = s_busq.tail;
        const uint16_t entry = s_busq.entries[tail & (kBusQueueLen - 1u)];
        if ((entry & kBusCommand) != 0) {
            on_bus_command(static_cast<uint8_t>(entry));
        } else {
            on_bus_data(static_cast<uint8_t>(entry));
        }
        busq_retire(tail, entry);
    }
    publish();
}

// Сброс машины: записи, не дошедшие до разбора, отменяются.
void drop_bus_queue() {
    while (s_busq.tail != s_busq.head) {
        const uint32_t tail = s_busq.tail;
        busq_retire(tail, s_busq.entries[tail & (kBusQueueLen - 1u)]);
    }
}

// Обслуживание посреди долгой работы основного потока. Записи в порты
// разбираются здесь же: сброс карты посреди разбора модуля отменяет его
// итог.
void serve_bus() {
    serve_without_wait(nullptr);
    if (s_busq.tail != s_busq.head) drain_bus_queue();
}

void SOUNDSINTH_HOT_PATH(gs_dat_write)(uint8_t, uint8_t data) {
    ++s_stats.writes_dat;
    // Поток - мимо очереди, только пока она пуста: иначе байт обогнал бы
    // запись, пришедшую раньше него.
    if (s_busq.head == s_busq.tail && s_dev.stream_byte(data)) {
        on_stream_byte(data);
        return;
    }
    busq_push(data);
}

// Чтение GSDAT состоялось: продвинуть указатель и зарядить следующий
// байт.
//
// Запись следующей команды вытесняет этот обработчик. Пока она в очереди,
// ответ прежней команды не двигается: его сменит разбор.
void SOUNDSINTH_HOT_PATH(gs_dat_read_done)(uint8_t) {
    ++s_stats.reads_dat;
    if (s_busq.head != s_busq.tail) return;
    s_busq.pub_seq = s_busq.pub_seq + 1u;
    s_dev.read_data(); // продвигает указатель ответа
    publish();
}

// --- Разбор накопленного, основной поток ---

// Байты сэмплов из кольца в область эффектов. Границы сэмплов здесь не
// нужны: они ложатся подряд, и смещение каждого обработчик посчитал тем же
// сложением. Курсор виртуальный - он растёт и на байтах, которым места уже
// нет, иначе после переполнения области следующий сэмпл лёг бы со сдвигом.
void SOUNDSINTH_HOT_PATH(drain_fx_ring)() {
    uint32_t tail       = s_fxrx.tail;
    const uint32_t head = s_fxrx.head;
    uint32_t fill       = (head - tail) & kFxRingMask;
    if (fill == 0) return;
    uint8_t* const dst = fx_area();
    while (fill != 0) {
        const uint32_t to_end = kFxRingBytes - tail;
        const uint32_t run    = to_end < fill ? to_end : fill;
        if (s_fxrx.write < kFxAreaBytes) {
            const uint32_t room = kFxAreaBytes - s_fxrx.write;
            const uint32_t n    = room < run ? room : run;
            std::memcpy(dst + s_fxrx.write, s_fxrx.ring + tail, n);
            s_fxrx.written += n;
        }
        s_fxrx.write += run;
        tail          = (tail + run) & kFxRingMask;
        fill         -= run;
    }
    s_fxrx.tail = tail;

    s_fxrx.ready.store(s_fxrx.write < kFxAreaBytes ? s_fxrx.write : kFxAreaBytes, std::memory_order_release);
}

// Во флеше: работа есть только при приёме, опрос в SRAM зовёт его по
// байтам в кольце, по сбросу и пока идёт приём.
SOUNDSINTH_NOINLINE void drain_ring() {
    apply_drop();
    // Приёма ещё нет - байты ждут в кольце, плеер придержан.
    if (!s_mem.rx.valid()) return;
    uint32_t tail       = s_rx.tail;
    const uint32_t head = s_rx.head;
    const uint32_t now  = platform::mono_us();
    const uint32_t fill = (head - tail) & kRingMask;
    if (fill != 0) {
        if (!s_stats.module_started) {
            s_stats.module_started    = true;
            s_stats.ring_peak         = 0;
            s_stats.drain_gap_max_us  = 0;
            s_stats.pio_lost_at_start = hal::bus_lost_port_writes();
        } else if (now - s_stats.last_drain_us > s_stats.drain_gap_max_us) {
            s_stats.drain_gap_max_us = now - s_stats.last_drain_us;
        }
        if (fill > s_stats.ring_peak) s_stats.ring_peak = fill;
        // Перелив под звук - как фоновая догрузка: заминки в его окне видны.
        shared::g_background_loading.store(true, std::memory_order_relaxed);
    }
    s_stats.last_drain_us = now;
    // Кусками memcpy, а не побайтно: без LTO побайтовый цикл перечитывает
    // s_rx.len и смещение на каждом байте (около 18 команд), это 270 мкс на
    // полное кольцо. Раскладка - во флеше: зовётся только при байтах в кольце.
    uint8_t* const dst = gs_buffer();
    uint32_t left      = s_rx.len < kBufferBytes ? kBufferBytes - s_rx.len : 0u;
    if (left > fill) left = fill;
    while (left != 0) {
        if (!s_lay.decided() && s_rx.len >= RxLayout::kHeaderBytes) s_lay.decide(dst, !s_parse.midi_stream);
        // Кусок - до конца кольца и до конца отрезка раскладки.
        uint32_t room         = 0;
        const uint32_t at     = s_lay.place(s_rx.len, &room);
        const uint32_t to_end = kRingBytes - tail;
        uint32_t run          = to_end < left ? to_end : left;
        if (run > room) run = room;
        std::memcpy(dst + at, &s_rx.ring[tail], run);
        s_rx.len += run;
        tail      = (tail + run) & kRingMask;
        left     -= run;
    }
    // Буфер полон, а байты ещё есть: модуль не влез, остаток не нужен.
    if (tail != head) {
        s_rx.overflow = true;
        tail          = head;
    }
    s_rx.tail = tail;
    if (fill != 0) shared::g_background_loading.store(false, std::memory_order_relaxed);
    apply_drop(); // сброс во время копирования: скопированное - старый поток
}

// Вторая строка модуля: сэмплы, время разбора и приём. Печатается и при
// отказе - по ней видно, лёг ли модуль целиком.
SOUNDSINTH_NOINLINE void log_module_details(uint32_t parse_ms, bool parsed) {
    const auto& ls          = soundsinth::model::g_tracker_load_stats;
    const uint32_t bus_lost = hal::bus_lost_port_writes() - s_stats.pio_lost_at_start;
    char m[320]; // внутри строки - причина отказа сэмпла, до 81 байта
    std::snprintf(m, sizeof(m),
                  "gs: module: samples %u, not stored %u%s%s%s, dropped %u, nonresident %u, parse %" PRIu32 " ms | ring peak %" PRIu32 " of %" PRIu32
                  ", drained once per %" PRIu32 " ms max, bus losses %" PRIu32 "\n",
                  parsed ? static_cast<unsigned>(shared::g_song.sample_count) : 0u, static_cast<unsigned>(ls.samples_failed),
                  ls.samples_failed ? " (first: " : "", ls.samples_failed ? (ls.first_failure ? ls.first_failure : "?") : "", ls.samples_failed ? ")" : "",
                  static_cast<unsigned>(ls.samples_dropped), static_cast<unsigned>(ls.samples_nonresident), parse_ms, s_stats.ring_peak, kRingBytes,
                  s_stats.drain_gap_max_us / 1000u, bus_lost);
    debug_log(m);
    s_stats.module_started = false;
}

// Плеер придержан битом занятости до резерва (on_stream_begin): отпустить.
SOUNDSINTH_ALWAYS_INLINE void release_player_hold() {
    s_mem.hold = false;
    s_dev.hold_reserve_done();
    publish();
}

// Строки итога модуля - своим кадром (SOUNDSINTH_NOINLINE): буфер строки не
// лежит на стеке Core1 под разбором.
// Недобор виден только по заголовкам самого модуля: ни кольцо, ни буфер о
// нём не знают.
SOUNDSINTH_NOINLINE void log_module_short(uint32_t source_end) {
    debug_logf("gs: module NOT accepted: received %" PRIu32 " bytes, header says %" PRIu32 " - short by %" PRIu32 "\n", s_rx.len, source_end,
               source_end - s_rx.len);
}

// Сверка приёма по трём точкам одного пути: запись в порт, приём автоматом,
// укладка в буфер. Равные числа при недоборе по заголовкам означают, что
// байты не дошли до платы вовсе, а не потерялись внутри неё.
SOUNDSINTH_NOINLINE void log_stream_tally(uint32_t port_writes, uint32_t lost) {
    debug_logf("gs: stream tally: port writes %" PRIu32 ", device accepted %" PRIu32 ", stored %" PRIu32 ", ring lost %" PRIu32 "\n", port_writes,
               s_dev.stream_bytes(), s_rx.len, lost);
}

SOUNDSINTH_NOINLINE void log_module_rejected(uint32_t lost) {
    debug_logf("gs: module NOT accepted: %" PRIu32 " bytes, overflow=%d (buffer %" PRIu32 "), lost in ring %" PRIu32 ", no engine until the next load\n",
               s_rx.len, static_cast<int>(s_rx.overflow), kBufferBytes, lost);
}

SOUNDSINTH_NOINLINE void log_module_result(bool ok, const load::SessionLoadResult& load, uint32_t teardown_ms) {
    if (ok) {
        debug_logf("gs: module accepted, %" PRIu32 " bytes, channels %u, duration %" PRIu32 " s, engine teardown %" PRIu32 " ms\n", s_rx.len,
                   static_cast<unsigned>(shared::g_song.channel_count), load.total_frames / soundsinth::engine::kSampleRateHz, teardown_ms);
    } else {
        debug_logf("gs: module NOT parsed (%" PRIu32 " bytes): %s\n", s_rx.len, load.error ? load.error : "(no reason)");
    }
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
    serve_bus();
    if (!w->reported && platform::mono_us() - w->started_us > kSlowTeardownUs) {
        w->reported = true;
        debug_log("gs: waiting for engine teardown over 200 ms\n");
    }
}

// Итог потока: счётчики потоков и сбросов снимаются до перелива остатка
// кольца - поток или сброс, пришедшие за разбор, делают итог устаревшим.
struct Received {
    uint32_t streams;
    uint32_t resets;
    uint32_t lost; // байт потеряно в кольце за поток
    bool current() const { return s_parse.streams_begun == streams && s_parse.resets == resets; }
};

Received drain_received() {
    Received rx{s_parse.streams_begun, s_parse.resets, 0};
    drain_ring();
    rx.lost = s_rx.full - s_rx.full_at_begin;
    return rx;
}

// Удержание разбора у автомата одно на все потоки. Новый #D2, пришедший
// во время разбора, взвёл его для себя - снимет его разбор того потока.
void release_parse_hold() {
    if (!s_parse.load_pending && !s_parse.midi_pending) s_dev.hold_parse_done();
}

SOUNDSINTH_NOINLINE void finish_module() {
    const Received rx = drain_received();
    // Байты, не влезшие в кольцо, - дыра посреди модуля: MOD с нехваткой PCM
    // разбирается и играет мусором, а причина видна только по редкой строке
    // gs:. Отказ вместо мусора; обратного давления у потока нет.
    // Поток, начатый во время разбора, взведёт просьбу заново.
    s_mem.rx_wanted = false;
    log_stream_tally(s_stats.writes_dat - s_stats.writes_dat_at_begin, rx.lost);
    shared::g_core1_phase.store(shared::Core1Phase::GsModule, std::memory_order_relaxed);
    if (s_rx.overflow || s_rx.len == 0 || rx.lost != 0) {
        log_module_rejected(rx.lost);
        ++s_stats.module_fails;
        if (rx.lost != 0) ++s_stats.fails_lost;
        if (s_rx.overflow) ++s_stats.fails_overflow;
        log_module_details(0, false);
        release_parse_hold();
        return;
    }

    // Разбор пишет Song и память трека, которые читает движок: сначала снять
    // его. Плеер всё это время придержан (#D2). Буфер разбор читает тем же
    // кэшируемым алиасом, которым его заполнил перелив: сброс кэша не нужен.
    TeardownWait wait{platform::mono_us(), false};
    shared::track_load_begin(&serve_teardown_wait, &wait);
    // Захват - парой со снятием в bridge_poll: резерв области уже есть с
    // прошлого потока, и try_reserve_receive_buffer s_mem.load_held не ставит.
    // Без этого второй модуль подряд или модуль после сэмпла эффекта
    // оставляет запрет до загрузки трека с хоста.
    s_mem.load_held            = true;
    const uint32_t teardown_ms = (platform::mono_us() - wait.started_us) / 1000u;
    // Прежний трек снят - его модуль больше не нужен.
    drop_live();
    uint8_t* const image = gs_buffer();
    RxSource src{image, s_mem.rx.first, 0};
    load::SessionLoadResult load;
    // Тот же вызов, что грузит трек с карты. Своего разбора у эмуляции GS
    // нет: приходит обычный MOD, загрузчик выбирает формат по содержимому
    // (имени у потока нет).
    const uint32_t parse_started_us = platform::mono_us();
    const bool ok                   = load::run_session_load(src.as_byte_source(), shared::g_track_memory, shared::g_song, load,
                                                             /*metadata_only=*/false);
    const uint32_t parse_ms         = (platform::mono_us() - parse_started_us) / 1000u;
    // Недобор: конец файла по его же заголовкам дальше принятого. У MOD
    // длины сэмплов точные, и это ловит третье место потери байта - очередь
    // записи шины, откуда байт до автомата не доходит и ни в одном счётчике
    // приёма не виден. Песня при этом разобрана, но части PCM в ней нет.
    const uint32_t source_end = soundsinth::model::g_tracker_load_stats.source_end;
    const bool short_module   = ok && source_end > s_rx.len;
    const bool good           = ok && !short_module;
    // Модуль карты звучит как у карты: раскладка и лесенка (kQuirkGsModule).
    if (ok) shared::g_song.quirks |= soundsinth::model::kQuirkGsModule;
    if (short_module) log_module_short(source_end);
    // Новый поток или сброс за время разбора - итог не нужен: образ и
    // поколение песни не ставятся, как при брошенном приёме.
    const bool current = rx.current();
    if (!current) debug_log("gs: module result dropped - reset or new stream during the parse\n");
    if (good && current) publish_file_info(load.total_frames, shared::g_song);
    s_parse.module_ready = good && current;
    // Приём становится модулем трека, ужатым до принятого с добивкой: из него
    // же играют сэмплы. Следующему потоку - новый приём.
    uint32_t room = 0;
    soundsinth::memory::psram_shrink_kept(shared::g_track_memory.psram, s_mem.rx, s_lay.place(s_rx.len, &room));
    s_mem.live = s_mem.rx;
    s_mem.rx   = soundsinth::memory::PsramBlock{};
    // Принятый модуль - источник паттернов для залитого резидента: плеер
    // просит их по номеру позиции, и отдаются они прямо из буфера. При
    // раскладке подряд как в файле лежат только заголовок и паттерны.
    const uint32_t image_len = s_lay.image_len(s_rx.len);
    if (current) s_dev.set_module_image(good ? image : nullptr, good ? image_len : 0u);
    if (good && current) {
        ++s_stats.module_loads;
        shared::publish_loaded_track(0); // конец задаёт плеер GS
    } else {
        ++s_stats.module_fails;
        if (short_module) ++s_stats.fails_short;
    }
    // Брошенному итогу хватает строки "dropped": "accepted" следом ей бы
    // противоречила.
    if (current) log_module_result(good, load, teardown_ms);
    log_module_details(parse_ms, good);
    // Удержание разбора снимается только здесь: плеер всё это время ждал,
    // отпустить раньше - сказать, что готово, когда не готово.
    release_parse_hold();
}

// Принят файл .mid: разобрать и отдать автомату таблицы. Плеер сразу
// после этого шлёт пробу #02, поэтому до конца разбора он придержан битом
// команды - его отпускает hold_parse_done.
SOUNDSINTH_NOINLINE void finish_midi() {
    const Received rx = drain_received();
    s_parse.midi_len  = s_rx.len;
    // Приём обязан быть целым, как у модуля: переполнение, потеря в кольце
    // или пустой поток - отказ. Файл длиннее kMidiWorkAt лёг бы под рабочую
    // область расписания, и её очистка стёрла бы его хвост. Нулевая длина
    // в begin() гасит прежний файл.
    const bool intact     = !s_rx.overflow && rx.lost == 0 && s_parse.midi_len != 0 && s_parse.midi_len <= kMidiWorkAt;
    uint8_t* const buffer = gs_buffer();
    if (!intact) {
        debug_logf("gs: midi stream damaged - %" PRIu32 " B, overflow %u, lost %" PRIu32 "\n", s_parse.midi_len, s_rx.overflow ? 1u : 0u, rx.lost);
    }
    const bool parsed = s_midi.begin(buffer, intact ? s_parse.midi_len : 0u, s_dev.midi_facts(), buffer + kMidiWorkAt, s_dev.midi_state_buffer());
    // Сброс или новый поток за время разбора - таблицы не ставятся.
    const bool ok = parsed && rx.current();
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
    // Сэмплы живой путь берёт только из флеш-банка: банк с карты ему не
    // годится.
    const bool live_set  = player::config::settings().live_midi != static_cast<uint8_t>(soundsinth::config::LiveMidi::None);
    const bool bank_here = shared::g_flash_bank.valid();
    const bool live_on   = live_set && bank_here;
    if (ok && !live_set) debug_log("gs: midi not offered - LIVE_MIDI=NONE, nothing would sound\n");
    if (ok && live_set && !bank_here) debug_log("gs: midi not offered - no bank in flash, nothing would sound\n");
    // Буфер держит файл .mid, а не модуль: резиденту паттернов
    // отсюда не отдавать.
    s_dev.set_module_image(nullptr, 0);
    s_dev.set_midi_ready(ok && live_on);
    const devices::gs::MidiFileFacts& f = s_dev.midi_facts();
    debug_logf("gs: midi %s - %" PRIu32 " B, ppqn %u, tracks %u of %u, first at %u | len field %u, skipped %" PRIu32 ", stop at %" PRIu32 ", frame %" PRIu32
               " B\n",
               ok ? "accepted" : (parsed ? "dropped by reset" : "REJECTED"), s_parse.midi_len, static_cast<unsigned>(f.division),
               static_cast<unsigned>(s_midi.track_count()), static_cast<unsigned>(f.track_count), static_cast<unsigned>(f.first_track),
               static_cast<unsigned>(f.file_length), s_midi.skipped(), s_midi.stopped_at(), s_midi.bytes_out());
    release_parse_hold();
}

} // namespace

void bridge_init() {
    s_dev.reset();
    s_dev.set_fx_end_source(fx_player().ended_seqs());

    devices::hal::z80_port_on_write(kPortGsCom, &gs_com_write);
    devices::hal::z80_port_on_write(kPortGsDat, &gs_dat_write);
    devices::hal::z80_port_on_read_done(kPortGsDat, &gs_dat_read_done);
    s_attached = true;
    publish();

    debug_log("gs: emulation attached at 0xbb (commands/status) and 0xb3 (data)\n");
}

void bridge_stop_effects() {
    fx_player().request_stop(devices::gs::kFxChannelMask);
}

// Из основного потока: обработчик порта мог бы вытеснить, но после сброса
// машины ПЗУ к портам GS не обращается.
void bridge_machine_reset() {
    if (!s_attached) return;
    drop_bus_queue();
    s_dev.reset();
    card_reset();
    publish();
}

void bridge_machine_reset_light() {
    if (!s_attached) return;
    drop_bus_queue();
    s_dev.abort_exchange();
    // Поток модуля или сэмпла оборван: принятое бросается, плеера больше нет.
    s_fxrx.handle       = 0;
    s_parse.midi_stream = false;
    request_drop();
    if (s_mem.hold) release_player_hold();
    publish();
}

void bridge_release_buffer() {
    // Запрос снимается в любом состоянии, и тот, что ещё ждёт сноса движка:
    // иначе следующий poll снёс бы движок, собранный из трека
    // платы, и отрезал его зоны. Принятое и незаконченное бросается: память
    // уходит треку. Модуль в памяти трека будет переписан загрузкой, #31
    // запускать нечего.
    request_drop();
    apply_drop();
    s_parse.module_ready = false;
    s_mem.wanted         = false;
    // Плеер ждал резерва, а его не будет: отпустить, иначе он встанет на
    // ожидании навсегда. Его модуль не примется (кольцо не разбирается без
    // резерва).
    if (s_mem.hold) release_player_hold();
    // Область возвращается треку - приёма больше нет. Запрет сборки движка
    // теперь держит загрузка трека (зовёт отсюда), она его и снимет: снять
    // его здесь - открыть сборку посреди её разбора.
    s_mem.load_held = false;
    s_mem.rx_wanted = false;
    if (!s_mem.fx.valid()) return;
    drop_live();
    soundsinth::memory::psram_drop_kept(shared::g_track_memory.psram, s_mem.rx);
    soundsinth::memory::psram_drop_kept(shared::g_track_memory.psram, s_mem.fx);
    // Сэмплов эффектов больше нет - ни байтов, ни записей автомата, ни
    // открытого потока. Стоп, а не reset(): рендер идёт на другом ядре, и
    // заявки, ждущие байтов, снимает счётчик стопов.
    fx_player().request_stop(devices::gs::kFxChannelMask);
    fx_player().attach(nullptr, nullptr);
    // Расписание MIDI жило в отданных страницах: порция по старой просьбе
    // #00 легла бы в чужую память.
    s_midi.begin(nullptr, 0, s_dev.midi_facts(), nullptr, nullptr);
    s_dev.fx_forget_all();
    s_fxrx.handle = 0;
    s_fxrx.alloc  = 0;
    s_fxrx.write  = 0;
    s_fxrx.tail   = s_fxrx.head;
    s_fxrx.ready.store(0, std::memory_order_release);
    debug_log("gs: receive buffer returned to the track\n");
}

void bridge_track_memory_taken() {
    s_parse.module_ready = false;
    // Трек модуля уходит вместе с памятью: его образ - тоже.
    drop_live();
    if (s_attached) publish();
}

uint32_t bridge_fx_mute_mask() {
    // У карты каналов четыре на всё: эффект отнимает канал у модуля, и то,
    // что там играло, замолкает. Настройка это отменяет - тогда эффект
    // звучит своими голосами, а музыка остаётся целой.
    if (player::config::settings().gs_fx_separate != 0) return 0;
    // Только у модуля самой карты: трек с хоста, .mid и живой MIDI - чужая
    // музыка, их каналы 0..3 эффект не отнимает.
    if ((shared::g_song.quirks & soundsinth::model::kQuirkGsModule) == 0) return 0;
    return fx_player().active_mask();
}

uint16_t bridge_track_gain() {
    if ((shared::g_song.quirks & soundsinth::model::kQuirkGsModule) == 0) return 4096u;
    return static_cast<uint16_t>(s_dev.module_volume() * 4096u / devices::gs::kModuleVolumeMax);
}

bool bridge_wants_track_memory() {
    return s_mem.wanted;
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
    s_dev.set_telemetry(order, row, notes, levels, shared::g_speed.load(std::memory_order_relaxed), s_parse.module_ready);
}

} // namespace

void SOUNDSINTH_HOT_PATH(bridge_poll)() {
    if (!s_attached) return;
    if (s_busq.tail != s_busq.head) drain_bus_queue();
    publish_telemetry();
    if (!s_mem.fx.valid() && (!s_mem.wanted || !try_reserve_receive_buffer())) return;
    if (s_mem.rx_wanted && !s_mem.rx.valid() && !take_rx_block()) return;
    if (s_mem.hold) release_player_hold();
    // Пока идёт приём - на каждом опросе: по промежуткам меряется витком Core1.
    if (s_rx.head != s_rx.tail || s_rx.drop_req || s_stats.module_started) drain_ring();
    drain_fx_ring();
    // Звук и автомат сверяются здесь: громкости #2B и #3D едут с шины, а
    // занятость каналов решает, куда ляжет следующий сэмпл.
    fx_player().attach(&s_dev, fx_area(), &s_fxrx.ready);
    fx_player().set_fx_volume(s_dev.fx_volume_effective());
    if (s_parse.load_pending) {
        s_parse.load_pending = false;
        finish_module();
        publish();
    }
    if (s_parse.midi_pending) {
        s_parse.midi_pending = false;
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
    if (s_mem.load_held && !s_parse.load_pending && !s_parse.midi_pending && s_fxrx.handle == 0 && s_dev.stream() == devices::gs::Stream::None) {
        s_mem.load_held = false;
        shared::track_load_end();
    }
}

namespace {

// Счётчики портов, потока и модулей - первая строка журнала GS. line -
// буфер строки вызывающего, не меньше 208 байт.
void log_gs_ports(char* line, size_t size) {
    std::snprintf(line, size,
                  "gs: cmd=%" PRIu32 " dat=%" PRIu32 " rd=%" PRIu32 " st=- | stream %" PRIu32 " B, modules %" PRIu32 ", failures %" PRIu32
                  ", ring full %" PRIu32 ", queue lost %" PRIu32 " | last=%02X status=%02X unknown=%02X x%" PRIu32 "\n",
                  s_stats.writes_cmd, s_stats.writes_dat, s_stats.reads_dat, s_dev.stream_bytes(), s_stats.module_loads, s_stats.module_fails,
                  static_cast<uint32_t>(s_rx.full), s_busq.lost, static_cast<unsigned>(s_dev.last_command()), static_cast<unsigned>(s_dev.read_status()),
                  static_cast<unsigned>(s_dev.unknown_command()), s_dev.unknown_count());
    debug_log(line);
}

void log_gs_resident_and_transport() {
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
}

void log_gs_midi_and_fx() {
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
                   static_cast<unsigned>(s_dev.fx_count()), s_fxrx.loaded, s_fxrx.lost, s_dev.fx_refused(), fx_player().waited(), s_fxrx.written,
                   static_cast<unsigned>(s_dev.current_fx()), static_cast<unsigned>(fx.note), static_cast<unsigned>(fx.volume),
                   static_cast<unsigned>(fx.priority), static_cast<unsigned>(fx.seek_first), static_cast<unsigned>(fx.seek_last), fx.loop ? 1u : 0u);
    }
}

void log_gs_telemetry() {
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

    // Разбивка отказов - своей строкой и только когда они есть: буфер
    // журнала лишнего не вмещает, а в исправной работе печатать нечего.
    if (s_stats.module_fails != 0) {
        debug_logf("gs: failures - hole %" PRIu32 ", cut %" PRIu32 ", short %" PRIu32 "\n", s_stats.fails_lost, s_stats.fails_overflow, s_stats.fails_short);
    }
}

constexpr uint32_t kTopCommands = 4;

// Самые частые команды за один проход: каждый счётчик читается один раз
// (растут в обработчике записи на Core1, пока здесь идёт обход). По
// убыванию числа, при равенстве меньший код раньше. Возврат - сколько
// ячеек заполнено.
uint32_t top_commands(uint8_t codes[kTopCommands], uint16_t counts[kTopCommands]) {
    uint32_t count = 0;
    for (uint32_t c = 0; c < 256; ++c) {
        const uint16_t n = s_dev.command_count(static_cast<uint8_t>(c));
        if (n == 0) continue;
        if (count == kTopCommands && n <= counts[kTopCommands - 1]) continue;
        uint32_t i = count < kTopCommands ? count++ : kTopCommands - 1;
        while (i > 0 && n > counts[i - 1]) {
            counts[i] = counts[i - 1];
            codes[i]  = codes[i - 1];
            --i;
        }
        counts[i] = n;
        codes[i]  = static_cast<uint8_t>(c);
    }
    return count;
}

// line - буфер строки вызывающего, не меньше 128 байт.
void log_gs_top_commands(char* line, size_t size) {
    uint8_t codes[kTopCommands]   = {};
    uint16_t counts[kTopCommands] = {};
    const uint32_t count          = top_commands(codes, counts);
    int off                       = std::snprintf(line, size, "gs: most frequent:");
    for (uint32_t i = 0; i < count && off < static_cast<int>(size); ++i) {
        off += std::snprintf(line + off, size - off, " %02X x%u", static_cast<unsigned>(codes[i]), static_cast<unsigned>(counts[i]));
    }
    if (off < static_cast<int>(size)) std::snprintf(line + off, size - off, "\n");
    debug_log(line);
}

} // namespace

void bridge_log_stats() {
    if (!(s_stats.writes_cmd || s_stats.writes_dat || s_stats.reads_dat)) return;
    // Одна строка за раз: debug_log копирует её в кольцо. Порядок строк
    // не менять - по нему читают логи платы.
    char gm[208];
    log_gs_ports(gm, sizeof(gm));
    log_gs_resident_and_transport();
    log_gs_midi_and_fx();
    log_gs_telemetry();
    log_gs_top_commands(gm, sizeof(gm));
}

} // namespace player::gs
