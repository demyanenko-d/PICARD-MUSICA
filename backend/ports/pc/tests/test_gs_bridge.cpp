// SPDX-License-Identifier: MIT
// Мост между автоматом General Sound и платформой: кто берёт память
// трека, кто её отдаёт и что делается по сбросу карты.
//
// Порты зовутся тем же путём, что и с шины: через хуки, которые мост
// ставит сам в bridge_init.

#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/memory/track_memory.h"
#include "devices/hal/z80_ports.h"
#include "player/gs/bridge.h"
#include "devices/gs/gs_device.h"
#include "player/gs/fx_player.h"
#include "player/gs/midi_sched.h"
#include "player/gs/rx_layout.h"
#include "player/shared_state.h"

// Хуки записи в порты держит подставная таблица ответов: она же их
// регистрирует, когда мост зовёт z80_port_on_write.
void test_z80_port_write(uint8_t port, uint8_t value);
bool test_z80_port_has_writer(uint8_t port);
uint32_t test_z80_port_set_count(uint8_t port);
uint8_t test_z80_port_read_value(uint8_t port);
uint8_t test_z80_port_read(uint8_t port);

extern char g_test_log[8192];

namespace {

constexpr uint8_t kPortCom = 0xbb;
constexpr uint8_t kPortDat = 0xb3;

// Команда и ожидание её разбора, как у хоста: SC, WC. Разбирает опрос.
void cmd(uint8_t v) {
    test_z80_port_write(kPortCom, v);
    player::gs::bridge_poll();
}
void dat(uint8_t v) {
    test_z80_port_write(kPortDat, v);
}

// Проход рендера тем же путём, каким его зовёт микшер.
void render(uint32_t frames = 64) {
    std::vector<int32_t> l(frames, 0), r(frames, 0);
    soundsinth::mixbus::SoundSource* src = player::gs::fx_player().as_sound_source();
    src->render_add(src->self, l.data(), r.data(), frames);
}

// Память трека и мост заводятся один раз на весь файл: psram_create берёт
// мегабайты.
bool setup() {
    static bool ready = false;
    if (!ready) {
        soundsinth::memory::track_memory_create(shared::g_track_memory);
        player::gs::bridge_init();
        ready = true;
    }
    if (!test_z80_port_has_writer(kPortCom)) {
        std::printf("  SKIP: port write hooks are not wired\n");
        return false;
    }
    return true;
}

// Привести мост в исходное: сброс карты и возврат области треку.
void quiesce() {
    cmd(0xF3);
    player::gs::bridge_poll();
    player::gs::bridge_release_buffer();
    player::gs::bridge_poll();
    render();
}

// Загрузить сэмпл эффекта: #38 открывает поток, байты идут в регистр
// данных, #D2 закрывает. Длину кладёт основной поток, то есть опрос.
void load_fx_sample(uint32_t bytes) {
    cmd(0x38);
    for (uint32_t i = 0; i < bytes; ++i)
        dat(static_cast<uint8_t>(0x40 + (i & 0x0f)));
    cmd(0xD2);
    player::gs::bridge_poll();
}

// Поток сэмпла эффекта забирает область приёма без разбора. Запрет сборки
// движка при этом обязан сняться: живой MIDI по тому же признаку
// закрывает сеанс, и записи в порт AY не включают режим.
void fx_stream_releases_the_load_hold() {
    if (!setup()) return;
    quiesce();
    CHECK(!shared::g_load_in_progress.load(std::memory_order_acquire));

    load_fx_sample(64);
    // Опрос мог уйти на ожидание сноса движка - дать ему дойти.
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    CHECK(!shared::g_load_in_progress.load(std::memory_order_acquire));
}

// Модуль после сэмпла эффекта: область приёма уже отрезана, и запрет,
// взведённый разбором, обязан сняться тем же опросом. Проходит ли разбор,
// не проверяется.
void module_after_fx_releases_the_load_hold() {
    if (!setup()) return;
    quiesce();
    load_fx_sample(64);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    cmd(0x30);
    for (uint32_t i = 0; i < 100; ++i)
        dat(static_cast<uint8_t>(i));
    cmd(0xD2);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    CHECK(!shared::g_load_in_progress.load(std::memory_order_acquire));
}

// Сброс карты (#F3) посреди потока: конца потока не приходит вовсе, и
// запрет всё равно обязан сняться.
void reset_mid_stream_releases_the_load_hold() {
    if (!setup()) return;
    quiesce();

    cmd(0x38);
    dat(0x11);
    dat(0x22);
    cmd(0xF3); // сброс, #D2 не придёт никогда
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    CHECK(!shared::g_load_in_progress.load(std::memory_order_acquire));
}

// Сброс карты гасит играющие голоса эффектов. Иначе следующий сэмпл
// ляжет под играющим голосом, а канал модуля останется немым.
void reset_silences_effect_voices() {
    if (!setup()) return;
    quiesce();

    load_fx_sample(256);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    cmd(0x39); // играть текущий сэмпл
    player::gs::bridge_poll();
    render();
    const uint8_t sounding = player::gs::fx_player().active_mask();
    std::printf("  channels sounding after the launch: %u\n", sounding);
    if (sounding == 0) {
        std::printf("  SKIP: the launch did not take a channel\n");
        return;
    }

    // Эффект отнимает канал только у модуля карты, не у чужого трека.
    const uint64_t quirks  = shared::g_song.quirks;
    shared::g_song.quirks  = 0;
    const uint32_t foreign = player::gs::bridge_fx_mute_mask();
    shared::g_song.quirks  = soundsinth::model::kQuirkGsModule;
    const uint32_t own     = player::gs::bridge_fx_mute_mask();
    shared::g_song.quirks  = quirks;
    std::printf("  effect mutes: foreign track %u, GS module %u\n", foreign, own);
    CHECK_EQ(foreign, 0u);
    CHECK_EQ(own, static_cast<uint32_t>(sounding));

    cmd(0xF3);
    player::gs::bridge_poll();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0);
}

// Проход рендера, итог - сумма модулей отсчётов: звучит или нет.
uint64_t render_level(uint32_t frames = 64) {
    std::vector<int32_t> l(frames, 0), r(frames, 0);
    soundsinth::mixbus::SoundSource* src = player::gs::fx_player().as_sound_source();
    src->render_add(src->self, l.data(), r.data(), frames);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < frames; ++i)
        sum += static_cast<uint64_t>(l[i] < 0 ? -l[i] : l[i]) + static_cast<uint64_t>(r[i] < 0 ? -r[i] : r[i]);
    return sum;
}

// #39 текущего сэмпла: ноль в регистре данных - "текущий".
void play_current() {
    dat(0x00);
    cmd(0x39);
}

// Два #39 в одном кадре игры, без рендера и опроса между ними: второй
// обязан взять другой канал, а не переписать первый.
void two_launches_take_two_channels() {
    if (!setup()) return;
    quiesce();
    load_fx_sample(256);
    play_current();
    play_current();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0x03);
}

// #3A #FF при четырёх занятых и сразу #39: каналы свободны тем же
// кадром, запуск обязан пройти.
void stop_all_then_launch() {
    if (!setup()) return;
    quiesce();
    load_fx_sample(256);
    for (int i = 0; i < 4; ++i)
        play_current();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0x0F);
    dat(0xFF);
    cmd(0x3A);
    play_current();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0x01);
}

// Сэмпл без петли доиграл - канал свободен без опроса моста.
void finished_sample_frees_its_channel() {
    if (!setup()) return;
    quiesce();
    load_fx_sample(64);
    play_current();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0x01);
    render(4096); // доиграл
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0);
    play_current();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0x01);
}

// Сброс, новый сэмпл и запуск без опроса между ними: байты нового ещё в
// кольце моста, в области - прежний сэмпл. Голос ждёт, а не играет чужое.
void launch_waits_for_sample_bytes() {
    if (!setup()) return;
    quiesce();
    load_fx_sample(256);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    cmd(0xF3);
    cmd(0x38);
    for (uint32_t i = 0; i < 64; ++i)
        dat(0x80); // тишина беззнакового сэмпла
    cmd(0xD2);
    play_current();
    CHECK_EQ(render_level(), 0u);
    player::gs::bridge_poll();
    CHECK_EQ(render_level(), 0u); // байты легли, и это тишина
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0x01);
}

// Область вернули треку и отрезали снова: старый номер сэмпла не играет
// ничего - байтов его в области больше нет.
void returned_area_forgets_samples() {
    if (!setup()) return;
    quiesce();
    load_fx_sample(256);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    player::gs::bridge_release_buffer();
    // Следующий поток модуля отрезает область заново.
    cmd(0x30);
    cmd(0xD2);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    dat(0x01);
    cmd(0x39);
    render();
    // Новый сэмпл ложится на то же место: старая запись сыграла бы его.
    load_fx_sample(300);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    render();
    CHECK_EQ(static_cast<int>(player::gs::fx_player().active_mask()), 0);
}

// GS=0: bridge_init не звался, порты чужие (настоящий GS, NeoGS, divIDE).
// Память трека живому MIDI и виток Core1 не должны включать ответ на них.
// Идёт первым: после setup мост подключён до конца прогона.
void detached_bridge_leaves_ports_alone() {
    std::printf("  detached bridge leaves 0xbb and 0xb3 alone\n");
    const uint32_t com = test_z80_port_set_count(kPortCom);
    const uint32_t dat = test_z80_port_set_count(kPortDat);
    player::gs::bridge_track_memory_taken();
    player::gs::bridge_poll();
    player::gs::bridge_release_buffer();
    CHECK_EQ(test_z80_port_set_count(kPortCom), com);
    CHECK_EQ(test_z80_port_set_count(kPortDat), dat);
}

// MOD, дописанный нулями до 300 001 байта: три куска по 128 КБ, хвост
// последнего не кратен шагу суммы 4 КБ. Пусто - файла нет.
std::vector<uint8_t> long_module() {
    std::vector<uint8_t> m;
    if (std::FILE* f = std::fopen("SD/test_music/mod/00.mod", "rb")) {
        int c = 0;
        while ((c = std::fgetc(f)) != EOF)
            m.push_back(static_cast<uint8_t>(c));
        std::fclose(f);
    }
    if (!m.empty()) m.resize(300001, 0);
    return m;
}

// Поток модуля: #30, байты с опросом через каждые 1024, #D2, опрос до итога.
std::string send_module(const std::vector<uint8_t>& m) {
    cmd(0x30);
    for (size_t i = 0; i < m.size(); ++i) {
        dat(m[i]);
        if ((i & 1023u) == 1023u) player::gs::bridge_poll();
    }
    g_test_log[0] = 0; // журнал - только итог приёма
    cmd(0xD2);
    for (int i = 0; i < 8; ++i)
        player::gs::bridge_poll();
    return g_test_log;
}

// Память GS при первом резерве: эффекты 128 КБ и приём 896 КБ с добивкой
// до страниц 32 КБ.
constexpr uint32_t kGsFirstReserve = 128u * 1024u + 896u * 1024u + 32u * 1024u;

// Память GS - отмеченные блоки распределителя: смена трека их не отбирает,
// возврат отдаёт всё.
void receive_buffer_is_a_kept_block() {
    if (!setup()) return;
    quiesce();
    soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    const uint32_t unkept                 = soundsinth::memory::psram_unkept_bytes(psram);
    load_fx_sample(16);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    CHECK_EQ(soundsinth::memory::psram_unkept_bytes(psram), unkept - kGsFirstReserve);
    soundsinth::memory::track_memory_reset_for_new_track(shared::g_track_memory);
    CHECK_EQ(soundsinth::memory::psram_unkept_bytes(psram), unkept - kGsFirstReserve);
    player::gs::bridge_release_buffer();
    CHECK_EQ(soundsinth::memory::psram_unkept_bytes(psram), unkept);
}

// Памяти под приём нет: плеер не висит на бите занятости, запрет сборки
// движка снят, модуль потока отвергнут на его конце.
void refused_reserve_lets_the_player_go() {
    if (!setup()) return;
    quiesce();
    soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    const uint32_t unkept                 = soundsinth::memory::psram_unkept_bytes(psram);
    soundsinth::memory::PsramBlock fill   = soundsinth::memory::psram_take_kept(psram, unkept - kGsFirstReserve + 64u * 1024u);
    CHECK(fill.valid());
    cmd(0x30);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    const uint8_t held = test_z80_port_read_value(kPortCom);
    for (uint32_t i = 0; i < 64; ++i)
        dat(static_cast<uint8_t>(i));
    cmd(0xD2);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    const uint8_t after = test_z80_port_read_value(kPortCom);
    std::printf("  no PSRAM for the receive buffer: status after #30 %02X, after #D2 %02X, load held %d\n", held, after,
                shared::g_load_in_progress.load(std::memory_order_acquire) ? 1 : 0);
    CHECK_EQ(held & 1, 0);
    CHECK_EQ(after & 1, 0);
    CHECK(!shared::g_load_in_progress.load(std::memory_order_acquire));
    soundsinth::memory::psram_drop_kept(psram, fill);
    quiesce();
}

// Сдача буфера посреди открытого потока эффекта снимает только своё:
// запрет сборки движка держит загрузка трека, которая её позвала.
void release_buffer_keeps_the_host_load_ban() {
    if (!setup()) return;
    quiesce();
    cmd(0x38);
    dat(1);
    dat(2);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    shared::track_load_begin(nullptr, nullptr);
    player::gs::bridge_release_buffer();
    const bool held = shared::g_load_in_progress.load(std::memory_order_acquire);
    shared::track_load_end();
    std::printf("  release_buffer inside a host load: ban kept %d, after the load %d\n", held ? 1 : 0,
                shared::g_load_in_progress.load(std::memory_order_acquire) ? 1 : 0);
    CHECK(held);
    CHECK(!shared::g_load_in_progress.load(std::memory_order_acquire));
    quiesce();
}

// Сброс карты (#F3) снимает её модуль: #31 после него не поднимает прежний,
// движок просят снести. Пауза трека платы чужим #F3 и #31 не снимается.
void card_reset_forgets_the_module() {
    if (!setup()) return;
    const std::vector<uint8_t> m = long_module();
    if (m.empty()) return;
    quiesce();
    send_module(m);
    CHECK((shared::g_song.quirks & soundsinth::model::kQuirkGsModule) != 0); // звучит как у карты
    // Громкость модуля #2A - усилением трека: половина шкалы - половина.
    CHECK_EQ(player::gs::bridge_track_gain(), 4096u);
    dat(0x20);
    cmd(0x2A);
    CHECK_EQ(player::gs::bridge_track_gain(), 2048u);
    dat(0x40);
    cmd(0x2A);
    const uint32_t g0 = shared::g_song_generation.load();
    cmd(0x31);
    const uint32_t g1 = shared::g_song_generation.load();
    shared::g_teardown_requested.store(false);
    cmd(0xF3);
    const bool teardown = shared::g_teardown_requested.load();
    cmd(0x31);
    const uint32_t g2 = shared::g_song_generation.load();
    std::printf("  module: #31 generation +%u, after #F3 teardown %d, #31 +%u\n", g1 - g0, teardown ? 1 : 0, g2 - g1);
    CHECK_EQ(g1 - g0, 1u);
    CHECK(teardown);
    CHECK_EQ(g2, g1);
    shared::g_teardown_requested.store(false);

    // Трек платы на паузе, модуля нет: опрос карты паузу не трогает.
    shared::g_playback_paused.store(true);
    cmd(0xF3);
    cmd(0x31);
    const bool still = shared::g_playback_paused.load();
    std::printf("  board track paused, #F3 and #31 from a program: paused %d\n", still ? 1 : 0);
    CHECK(still);
    shared::g_playback_paused.store(false);
    quiesce();
}

// Разобранный модуль держит свою длину с добивкой до страниц, не больше:
// приём ужимается до принятого, под следующий поток берётся новый, а
// прежний модуль отдаётся, когда разобран следующий.
void parsed_module_keeps_its_length() {
    if (!setup()) return;
    const std::vector<uint8_t> m = long_module();
    if (m.empty()) return;
    quiesce();
    soundsinth::memory::PsramStore& psram = shared::g_track_memory.psram;
    const uint32_t unkept                 = soundsinth::memory::psram_unkept_bytes(psram);
    const uint32_t fx                     = 128u * 1024u;
    const auto len                        = static_cast<uint32_t>(m.size());
    for (int round = 0; round < 2; ++round) {
        send_module(m);
        const uint32_t held = unkept - soundsinth::memory::psram_unkept_bytes(psram);
        CHECK(held >= fx + len);
        CHECK(held <= fx + len + 32u * 1024u);
    }
    player::gs::bridge_release_buffer();
    CHECK_EQ(soundsinth::memory::psram_unkept_bytes(psram), unkept);
}

// Сэмплы принятого модуля играют из приёма: цепочка - страницы отмеченного
// блока подряд, байты - как в файле.
void module_samples_play_from_the_receive_block() {
    if (!setup()) return;
    const std::vector<uint8_t> m = long_module();
    if (m.empty()) return;
    quiesce();
    send_module(m);
    soundsinth::memory::TrackMemory& mem = shared::g_track_memory;
    uint32_t resident = 0, in_place = 0, mismatched = 0;
    for (uint16_t i = 0; i < shared::g_song.sample_count; ++i) {
        const soundsinth::model::SampleDescriptor& sd = shared::g_song.samples[i];
        const soundsinth::memory::SampleCacheEntry* e = soundsinth::memory::sample_cache_find(mem.sample_cache, i);
        if (e == nullptr) continue;
        ++resident;
        bool kept = false;
        for (const soundsinth::memory::PsramBlock& b : mem.psram.alloc.keep) {
            if (b.valid() && e->first_page >= b.first && e->first_page < b.first + b.pages) kept = true;
        }
        if (kept) ++in_place;
        uint16_t page = e->first_page;
        for (uint32_t k = 0; k < sd.length_samples; ++k) {
            if (k != 0 && k % soundsinth::memory::kPsramPageBytes == 0) page = soundsinth::memory::psram_page_next(mem.psram, page);
            const uint8_t got = soundsinth::memory::psram_page_ptr(mem.psram, page)[k % soundsinth::memory::kPsramPageBytes];
            if (got != m[sd.file_offset + k]) {
                ++mismatched;
                break;
            }
        }
    }
    std::printf("  samples resident %u, in the receive block %u, mismatched %u\n", resident, in_place, mismatched);
    CHECK(resident != 0);
    CHECK_EQ(in_place, resident);
    CHECK_EQ(mismatched, 0u);
    player::gs::bridge_release_buffer();
}

// Раскладка приёма: файл .mid и не-MOD - смещение как в файле; MOD -
// заголовок и паттерны подряд, каждый непустой сэмпл с начала страницы.
void rx_layout_places_samples_on_pages() {
    using player::gs::RxLayout;
    std::printf("  rx layout places samples on pages\n");
    std::vector<uint8_t> h(RxLayout::kHeaderBytes, 0);
    std::memcpy(h.data() + RxLayout::kHeaderBytes - 4, "M.K.", 4);
    h[952]     = 0;
    h[953]     = 2;
    h[954]     = 1;
    h[955]     = 0xFF; // конец таблицы порядка
    auto words = [&](int i, uint16_t w) {
        h[20 + 30 * i + 22] = static_cast<uint8_t>(w >> 8);
        h[20 + 30 * i + 23] = static_cast<uint8_t>(w);
    };
    words(0, 100);
    words(1, 1); // пустой сэмпл ProTracker
    words(2, 600);

    RxLayout lay;
    uint32_t room = 0, sample = 0;
    CHECK_EQ(lay.place(10, &room), 10u);
    CHECK_EQ(room, RxLayout::kHeaderBytes - 10u);
    lay.decide(h.data(), false); // .mid: раскладки нет
    CHECK_EQ(lay.place(5000, &room, &sample), 5000u);
    CHECK_EQ(room, UINT32_MAX);
    CHECK_EQ(sample, RxLayout::kSamples);

    lay.reset();
    lay.decide(h.data(), true);
    const uint32_t head = RxLayout::kHeaderBytes + 3u * 1024u; // паттерны 0..2
    CHECK_EQ(lay.image_len(100000), head);
    CHECK_EQ(lay.image_len(100), 100u);
    CHECK_EQ(lay.place(head - 1u, &room), head - 1u);
    CHECK_EQ(room, 1u);
    CHECK_EQ(lay.place(head, &room, &sample), 5120u);
    CHECK_EQ(room, 200u);
    CHECK_EQ(sample, 0u);
    CHECK_EQ(lay.place(head + 200u, &room, &sample), 5320u);
    CHECK_EQ(room, 2u);
    CHECK_EQ(sample, 1u);
    CHECK_EQ(lay.place(head + 203u, &room, &sample), 6145u);
    CHECK_EQ(room, 1199u);
    CHECK_EQ(sample, 2u);
    CHECK_EQ(lay.sample_bytes(2), 1200u);
}

// Запись в порт до опроса только взводит разряд состояния: данных - D7,
// команды - D0. Опрос разбирает обе по порядку прихода и снимает разряд
// команды; D7 после него - ответ #20.
void writes_wait_for_the_poll() {
    if (!setup()) return;
    quiesce();
    CHECK_EQ(static_cast<int>(test_z80_port_read_value(kPortCom)), 0x7E);
    test_z80_port_write(kPortDat, 0x55);
    CHECK_EQ(static_cast<int>(test_z80_port_read_value(kPortCom)), 0xFE);
    test_z80_port_write(kPortCom, 0x20);
    CHECK_EQ(static_cast<int>(test_z80_port_read_value(kPortCom)), 0xFF);
    player::gs::bridge_poll();
    CHECK_EQ(static_cast<int>(test_z80_port_read_value(kPortCom)), 0xFE);
}

// Ответ команды уходит машине чтениями порта данных: каждое чтение
// продвигает ответ. Пока следующая запись ждёт разбора, чтение ответ не
// двигает.
void answers_reach_the_data_port() {
    if (!setup()) return;
    quiesce();
    cmd(0x20); // объём памяти, три байта младшим вперёд
    uint32_t total = 0;
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(test_z80_port_read_value(kPortCom)), 0xFE);
        total |= static_cast<uint32_t>(test_z80_port_read(kPortDat)) << (8 * i);
    }
    CHECK_EQ(total, devices::gs::kDeclaredRamBytes);
    CHECK_EQ(static_cast<int>(test_z80_port_read_value(kPortCom)), 0x7E);
    cmd(0x23);
    test_z80_port_write(kPortCom, 0x20); // запись в очереди, разбора нет
    CHECK_EQ(static_cast<int>(test_z80_port_read(kPortDat)), static_cast<int>(devices::gs::kDeclaredPages));
    CHECK_EQ(static_cast<int>(test_z80_port_read(kPortDat)), static_cast<int>(devices::gs::kDeclaredPages));
    player::gs::bridge_poll();
    CHECK_EQ(static_cast<int>(test_z80_port_read(kPortDat)), static_cast<int>(devices::gs::kDeclaredRamBytes & 0xffu));
}

// Строка журнала с числами под маской '#': десятичное число, слово из
// знаков [0-9A-F] длиной от двух, число после 'x' (счётчик). Текст строк
// журнала читают по логам платы, и он обязан остаться прежним; числа
// меняются от прогона к прогону.
std::string mask_numbers(const std::string& s) {
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    auto word  = [&](char c) { return digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (!word(s[i])) {
            out += s[i++];
            continue;
        }
        size_t j = i;
        while (j < s.size() && word(s[j]))
            ++j;
        const std::string w = s.substr(i, j - i);
        bool upper_hex = true, digits = true;
        for (char c : w) {
            upper_hex = upper_hex && (digit(c) || (c >= 'A' && c <= 'F'));
            digits    = digits && digit(c);
        }
        bool counter = w.size() > 1 && w[0] == 'x';
        for (size_t k = 1; k < w.size(); ++k)
            counter = counter && digit(w[k]);
        if (digits || (upper_hex && w.size() >= 2)) {
            out += '#';
        } else if (counter) {
            out += "x#";
        } else {
            out += w;
        }
        i = j;
    }
    return out;
}

bool log_has(const std::string& log, const char* masked_line) {
    return mask_numbers(log).find(masked_line) != std::string::npos;
}

// Строки приёма модуля: принят, отвергнут по потере в кольце.
void module_log_lines_keep_their_text() {
    if (!setup()) return;
    const std::vector<uint8_t> m = long_module();
    if (m.empty()) {
        CHECK(false);
        return;
    }
    quiesce();
    std::string log = send_module(m);
    CHECK(log_has(log, "gs: stream tally: port writes #, device accepted #, stored #, ring lost #\n"));
    CHECK(log_has(log, "gs: module accepted, # bytes, channels #, duration # s, engine teardown # ms\n"));
    CHECK(log_has(log,
                  "gs: module: samples #, not stored #, dropped #, nonresident #, parse # ms | ring peak # of #, drained once per # ms max, bus losses #\n"));

    // Больше кольца без опроса: хвост теряется, модуль отвергнут.
    cmd(0x30);
    for (uint32_t i = 0; i < 5000; ++i)
        dat(static_cast<uint8_t>(i));
    g_test_log[0] = 0;
    cmd(0xD2);
    for (int i = 0; i < 8; ++i)
        player::gs::bridge_poll();
    log = g_test_log;
    CHECK(log_has(log, "gs: module NOT accepted: # bytes, overflow=# (buffer #), lost in ring #, no engine until the next load\n"));
    CHECK(log_has(log,
                  "gs: module: samples #, not stored #, dropped #, nonresident #, parse # ms | ring peak # of #, drained once per # ms max, bus losses #\n"));

    load_fx_sample(16);
    g_test_log[0] = 0;
    player::gs::bridge_log_stats();
    log = g_test_log;
    CHECK(log_has(log, "gs: fx # ok # lost # refused # waited #, # B | cur ## note # vol # prio # seek F/F loop #\n"));
    CHECK(log_has(log, "gs: cmd=# dat=# rd=# st=- | stream # B, modules #, failures #, ring full #, queue lost # | last=# status=# unknown=# x#\n"));
    CHECK(log_has(log, "gs: resident gone | ## x# ## x# close ## x# | pattern asks # served #, last pos # -> pattern # | image # B\n"));
    CHECK(log_has(log, "gs: transport play ## x# stop ## x# cont ## x# | paused #\n"));
    CHECK(log_has(log, "gs: telemetry order # row # speed # | sent notes # # # # levels # # # # | engine # # # #\n"));
    CHECK(log_has(log, "gs: failures - hole #, cut #, short #\n"));
    CHECK(log_has(log, "gs: most frequent: # x#"));
    quiesce();
}

// Файл .mid через мост: заливка проигрывателя, поток файла, разбор, и
// порции кадров, прочитанные из порта, - байт в байт расписание того же
// файла.
void midi_file_over_the_bridge() {
    if (!setup()) return;
    std::FILE* f = std::fopen("ZPlayer/v51/27_gs_midi.bin", "rb");
    if (f == nullptr) {
        CHECK(false);
        return;
    }
    std::vector<uint8_t> body;
    for (int c = 0; (c = std::fgetc(f)) != EOF;)
        body.push_back(static_cast<uint8_t>(c));
    std::fclose(f);
    CHECK_EQ(body.size(), 2356u);
    if (body.size() != 2356u) return;

    // Две дорожки с именами, ноты каждые 24 тика при 96 на четверть.
    std::vector<uint8_t> mid = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96};
    for (int t = 0; t < 2; ++t) {
        std::vector<uint8_t> tr = {0x00, 0xFF, 0x03, 5, 'T', 'r', 'k', ' ', static_cast<uint8_t>('A' + t)};
        for (uint8_t n = 0; n < 40; ++n) {
            tr.insert(tr.end(), {24, static_cast<uint8_t>(0x90 | t), static_cast<uint8_t>(48 + n), 100});
            tr.insert(tr.end(), {12, static_cast<uint8_t>(0x80 | t), static_cast<uint8_t>(48 + n), 0});
        }
        tr.insert(tr.end(), {0x00, 0xFF, 0x2F, 0x00});
        mid.insert(mid.end(), {'M', 'T', 'r', 'k', 0, 0, static_cast<uint8_t>(tr.size() >> 8), static_cast<uint8_t>(tr.size())});
        mid.insert(mid.end(), tr.begin(), tr.end());
    }
    body[0x62d] = 96;
    body[0x62e] = 0;
    body[0x63c] = 0xFF;
    body[0x76b] = 14;
    body[0x7bc] = 2;
    body[0x853] = static_cast<uint8_t>(mid.size() & 0xFF);
    body[0x854] = static_cast<uint8_t>(mid.size() >> 8);

    quiesce();
    dat(0x00);
    cmd(0x18);
    dat(0x50);
    for (uint8_t byte : body) {
        dat(byte);
        cmd(0x19);
        cmd(0x1B);
    }
    dat(0x00);
    cmd(0x13);
    dat(0x50);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    for (uint8_t byte : mid)
        dat(byte);
    g_test_log[0] = 0;
    cmd(0xD2);
    for (int i = 0; i < 4; ++i)
        player::gs::bridge_poll();
    const std::string log = g_test_log;
    CHECK(log_has(log, "gs: midi accepted - # B, ppqn #, tracks # of #, first at # | len field #, skipped #, stop at #, frame # B\n"));
    CHECK(log_has(log, "gs: midi not offered - no bank in flash, nothing would sound\n"));

    // То же расписание здесь: порции обязаны совпасть байт в байт.
    player::gs::MidiSched ref;
    std::vector<uint8_t> work(player::gs::kMidiWorkSize);
    uint8_t state[devices::gs::kMidiStateBytes];
    devices::gs::MidiFileFacts facts;
    facts.division    = 96;
    facts.first_track = 14;
    facts.track_count = 2;
    facts.file_length = static_cast<uint16_t>(mid.size());
    CHECK(ref.begin(mid.data(), static_cast<uint32_t>(mid.size()), facts, work.data(), state));

    cmd(0x03); // имена дорожек
    std::vector<uint8_t> names;
    for (uint32_t i = 0; i < devices::gs::kMidiNamesBytes; ++i)
        names.push_back(test_z80_port_read(kPortDat));
    CHECK(std::equal(names.begin(), names.end(), ref.names()));
    CHECK(names[0] == 'T' && names[4] == 'A' && names[24 + 4] == 'B');

    uint32_t mismatched = 0, bytes = 0;
    for (int frame = 0; frame < 120; ++frame) {
        const uint16_t n   = ref.next_frame();
        const uint8_t* exp = ref.frame();
        cmd(0x00);
        for (uint16_t i = 0; i < n; ++i) {
            if (test_z80_port_read(kPortDat) != exp[i]) ++mismatched;
        }
        bytes += n;
        player::gs::bridge_poll();
    }
    std::printf("  midi frames over the bridge: %u bytes, mismatched %u\n", bytes, mismatched);
    CHECK(bytes > 240u); // ноты взяты и сняты, не одни пустые порции
    CHECK_EQ(mismatched, 0u);
    g_test_log[0] = 0;
    player::gs::bridge_log_stats();
    CHECK(log_has(g_test_log, "gs: blob ## crc # | midi active # ready #, tracks #, frames # (empty #), bytes #\n"));
    cmd(0xF3);
    quiesce();
}

} // namespace

void run_gs_bridge_tests() {
    std::printf("gs: bridge to the platform\n");
    detached_bridge_leaves_ports_alone();
    rx_layout_places_samples_on_pages();
    writes_wait_for_the_poll();
    fx_stream_releases_the_load_hold();
    module_after_fx_releases_the_load_hold();
    reset_mid_stream_releases_the_load_hold();
    reset_silences_effect_voices();
    two_launches_take_two_channels();
    stop_all_then_launch();
    finished_sample_frees_its_channel();
    launch_waits_for_sample_bytes();
    returned_area_forgets_samples();
    receive_buffer_is_a_kept_block();
    refused_reserve_lets_the_player_go();
    release_buffer_keeps_the_host_load_ban();
    card_reset_forgets_the_module();
    parsed_module_keeps_its_length();
    module_samples_play_from_the_receive_block();
    answers_reach_the_data_port();
    module_log_lines_keep_their_text();
    midi_file_over_the_bridge();
}
