// SPDX-License-Identifier: MIT
// Голоса эффектов General Sound: заявки с шины и их применение рендером.
//
// Заявку ставит обработчик порта на Core1, принимает рендер на Core0.
// Гонка здесь не проверяется; проверяется, что заявки не теряются, останов
// сильнее запуска, а сброс гасит всё.

#include "testing.h"

#include <cstdio>
#include <vector>

#include "player/gs/fx_player.h"

using player::gs::FxPlayer;

namespace {

constexpr uint8_t kChannels = devices::gs::kFxChannels;

// Сэмпл-пустышка: постоянная величина, чтобы по сумме было видно, звучит
// канал или нет.
struct Area {
    std::vector<uint8_t> pcm = std::vector<uint8_t>(4096, 0x40);
    devices::gs::GsDevice dev;

    // Запись в таблице заводит сама карта, командой #38: до неё номера
    // нет, и длину положить некуда.
    uint8_t add_sample(uint32_t len) {
        dev.write_command(0x38);
        const uint8_t h = dev.fx_loading();
        dev.write_command(0xD2);
        dev.set_fx_length(h, 0, len);
        return h;
    }
};

// Заявка с заголовком сэмпла на этот момент - как её собирает автомат на #39.
devices::gs::FxStart start_of(const Area& a, uint8_t channel, uint8_t handle, uint8_t volume = 0x40) {
    devices::gs::FxStart s;
    s.channel = channel;
    s.handle  = handle;
    s.note    = 60;
    s.volume  = volume;
    devices::gs::fx_snapshot(s, a.dev.fx_sample(handle));
    return s;
}

// Один проход рендера, и ровно тем же путём, каким его зовёт микшер:
// через контракт источника звука, а не внутренним методом.
void render_into(FxPlayer& fx, int32_t* l, int32_t* r, uint32_t frames) {
    soundsinth::mixbus::SoundSource* src = fx.as_sound_source();
    src->render_add(src->self, l, r, frames);
}

void render(FxPlayer& fx, uint32_t frames = 64) {
    std::vector<int32_t> l(frames, 0), r(frames, 0);
    render_into(fx, l.data(), r.data(), frames);
}

// Заявки на разные каналы, поставленные до одного прохода рендера, обязаны
// примениться все, без потери разряда маски.
void requests_on_all_channels_survive() {
    Area a;
    for (uint8_t h = 0; h < kChannels; ++h)
        a.add_sample(1024);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);

    for (uint8_t c = 0; c < kChannels; ++c)
        fx.request_start(start_of(a, c, static_cast<uint8_t>(c + 1)));
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0); // до прохода ещё ничего
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), static_cast<int>(devices::gs::kFxChannelMask));
    CHECK_EQ(fx.started(), 4u);
}

// Останов сильнее запуска в одном проходе: заявки принимаются в том же
// порядке, в каком их обрабатывает карта.
void stop_and_start_in_one_pass() {
    Area a;
    a.add_sample(1024);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);

    fx.request_start(start_of(a, 0, 1));
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 1);

    // Останов и новый запуск того же канала: канал остаётся звучащим -
    // запуск пришёл после останова.
    fx.request_stop(1);
    fx.request_start(start_of(a, 0, 1));
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 1);

    // Один останов - канал молчит.
    fx.request_stop(1);
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0);
}

// Маской останова можно погасить все каналы разом - так делает сброс карты.
void stop_mask_silences_every_channel() {
    Area a;
    for (uint8_t h = 0; h < kChannels; ++h)
        a.add_sample(1024);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);

    for (uint8_t c = 0; c < kChannels; ++c)
        fx.request_start(start_of(a, c, static_cast<uint8_t>(c + 1)));
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), static_cast<int>(devices::gs::kFxChannelMask));

    fx.request_stop(devices::gs::kFxChannelMask);
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0);
}

// Звук идёт в накопитель, и громкость эффектов на него влияет. Проверяется
// не величина, а то, что голос вообще складывается и что ноль громкости
// его гасит.
void voice_adds_to_the_mix() {
    Area a;
    a.add_sample(2048);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);
    fx.request_start(start_of(a, 0, 1));

    std::vector<int32_t> l(64, 0), r(64, 0);
    render_into(fx, l.data(), r.data(), 64);
    int64_t sum = 0;
    for (uint32_t i = 0; i < 64; ++i)
        sum += l[i] + r[i];
    CHECK(sum != 0);

    // Нулевая громкость эффектов - тишина, голос при этом жив.
    fx.set_fx_volume(0);
    std::vector<int32_t> l2(64, 0), r2(64, 0);
    render_into(fx, l2.data(), r2.data(), 64);
    int64_t sum2 = 0;
    for (uint32_t i = 0; i < 64; ++i)
        sum2 += l2[i] + r2[i];
    CHECK_EQ(sum2, 0);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 1);
}

// Сэмпл нулевой длины канал не занимает: длину кладёт основной поток уже
// после команды, и в это окно запуск приходить может.
void start_without_length_does_not_sound() {
    Area a;
    a.add_sample(0);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);
    fx.request_start(start_of(a, 0, 1));
    render(fx);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0);
    CHECK_EQ(fx.started(), 0u);
}

// #49 без петли укорачивает звук: конец звука - конец петли, а не длина.
// SD LEN.L / SC #49 / SD LEN.M / SD LEN.H над текущим сэмплом.
void loop_end_cuts_the_sound() {
    Area a;
    const uint8_t h = a.add_sample(1000);
    a.dev.write_data(0x90); // 400 = 0x000190
    a.dev.write_command(0x49);
    a.dev.write_data(0x01);
    a.dev.write_data(0x00);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);
    fx.request_start(start_of(a, 0, h));
    // Нота 60: около 0.376 байта на кадр - 400 байт за 1063 кадра, 1000
    // за 2658.
    render(fx, 64);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0x01);
    render(fx, 1500);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0);
}

// Заявка взята, а до буфера рендера шина меняет заголовок того же сэмпла
// (#49 для следующего выстрела): звук играет заголовок на момент запуска,
// а не половину старого и половину нового.
void start_keeps_the_header_snapshot() {
    Area a;
    const uint8_t h = a.add_sample(1000);
    FxPlayer fx;
    fx.reset();
    fx.attach(&a.dev, a.pcm.data());
    fx.set_fx_volume(0x40);
    fx.request_start(start_of(a, 0, h));
    a.dev.set_fx_length(h, 0, 10); // десять байт - около 27 кадров на ноте 60
    render(fx, 64);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0x01);
    render(fx, 2600);
    CHECK_EQ(static_cast<int>(fx.active_mask()), 0);
}

// Таблица полна: #38 отвергнут, и CurFX не указывает на последний
// принятый - команды параметров и #39 0 его не трогают.
void refused_load_clears_current() {
    Area a;
    for (uint8_t i = 0; i < devices::gs::kFxSamplesMax; ++i)
        a.add_sample(64);
    const uint8_t last = a.dev.current_fx();
    const uint8_t vol  = a.dev.fx_sample(last).volume;
    a.dev.write_command(0x38); // отказ
    CHECK_EQ(a.dev.fx_refused(), 1u);
    a.dev.write_data(0x10);
    a.dev.write_command(0x41); // громкость текущего
    CHECK_EQ(static_cast<int>(a.dev.fx_sample(last).volume), static_cast<int>(vol));
    a.dev.write_data(0x00);
    CHECK(a.dev.write_command(0x39) == devices::gs::Event::None);
}

} // namespace

void run_gs_fx_player_tests() {
    std::printf("gs: effect voices\n");
    requests_on_all_channels_survive();
    stop_and_start_in_one_pass();
    stop_mask_silences_every_channel();
    voice_adds_to_the_mix();
    start_without_length_does_not_sound();
    loop_end_cuts_the_sound();
    start_keeps_the_header_snapshot();
    refused_load_clears_current();
}
