// SPDX-License-Identifier: MIT
// Мост между автоматом General Sound и платформой: кто берёт память
// трека, кто её отдаёт и что делается по сбросу карты.
//
// Порты зовутся тем же путём, что и с шины: через хуки, которые мост
// ставит сам в bridge_init.

#include "testing.h"

#include <cstdio>
#include <vector>

#include "core/memory/track_memory.h"
#include "devices/hal/z80_ports.h"
#include "player/gs/bridge.h"
#include "player/gs/fx_player.h"
#include "player/shared_state.h"

// Хуки записи в порты держит подставная таблица ответов: она же их
// регистрирует, когда мост зовёт z80_port_on_write.
void test_z80_port_write(uint8_t port, uint8_t value);
bool test_z80_port_has_writer(uint8_t port);

namespace {

constexpr uint8_t kPortCom = 0xbb;
constexpr uint8_t kPortDat = 0xb3;

void cmd(uint8_t v) {
    test_z80_port_write(kPortCom, v);
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

} // namespace

void run_gs_bridge_tests() {
    std::printf("gs: bridge to the platform\n");
    fx_stream_releases_the_load_hold();
    module_after_fx_releases_the_load_hold();
    reset_mid_stream_releases_the_load_hold();
    reset_silences_effect_voices();
    two_launches_take_two_channels();
    stop_all_then_launch();
    finished_sample_frees_its_channel();
    launch_waits_for_sample_bytes();
    returned_area_forgets_samples();
}
