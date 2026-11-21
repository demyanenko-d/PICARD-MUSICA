// SPDX-License-Identifier: MIT
// Шина: авторегулировка громкости, длинное затухание перед концом трека,
// измеритель выхода (пики для VU и спектр в 16 полос).

#include "testing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/audio/meter.h"
#include "core/audio/mixbus.h"

using namespace soundsinth;

namespace {

constexpr uint32_t kRate  = 44100;
constexpr uint32_t kBlock = SOUNDSINTH_AUDIO_BUFFER_FRAMES;

// Синус заданной амплитуды (единицы int16) в оба канала; 0 - тишина.
struct Tone {
    double amplitude = 0, hz = 440, phase = 0;
    static void render_add(void* self, int32_t* ml, int32_t* mr, uint32_t n) {
        Tone& t = *static_cast<Tone*>(self);
        for (uint32_t i = 0; i < n; ++i) {
            const int32_t v  = static_cast<int32_t>(t.amplitude * std::sin(t.phase) * (1 << mixbus::kMixFracBits));
            t.phase         += 6.283185307179586 * t.hz / kRate;
            ml[i]           += v;
            mr[i]           += v;
        }
    }
};

// seconds секунд выхода шины; пик левого канала за последнюю долю секунды.
int32_t run(mixbus::MixBus& bus, double seconds, std::vector<int16_t>* keep = nullptr) {
    int16_t buf[kBlock * 2];
    const uint32_t blocks = static_cast<uint32_t>(seconds * kRate / kBlock);
    int32_t peak          = 0;
    for (uint32_t b = 0; b < blocks; ++b) {
        bus.render(buf, kBlock);
        if (keep != nullptr) keep->insert(keep->end(), buf, buf + kBlock * 2);
        if (b + 20 >= blocks) {
            for (uint32_t i = 0; i < kBlock; ++i) {
                peak = std::max<int32_t>(peak, std::abs(buf[2 * i]));
            }
        }
    }
    return peak;
}

void test_auto_volume() {
    std::printf("test_auto_volume\n");
    struct Case {
        double in, out_lo, out_hi;
    };
    // Цель - средний квадрат -20 дБ от шкалы: синус с пиком около 4500 (мера
    // уровня на 440 Гц чуть выше самого сигнала). Совсем тихий вход
    // упирается в +18 дБ, громкий - в -12 дБ.
    for (const Case c : {Case{8000, 4200, 4800}, Case{1000, 4200, 4800}, Case{300, 2300, 2500}, Case{30000, 7350, 7650}}) {
        mixbus::MixBus bus;
        Tone tone{c.in};
        mixbus::SoundSource src{&tone, &Tone::render_add};
        bus.add_source(&src);
        bus.start_track();
        bus.set_auto_volume(true);
        bus.set_limiter(true);
        const int32_t peak = run(bus, 20);
        CHECK(peak >= c.out_lo && peak <= c.out_hi);
        CHECK_EQ(bus.clipped_frames(), 0u);
    }
    // Тишина усиление не двигает; без настройки шина сигнал не трогает.
    mixbus::MixBus bus;
    Tone tone{0};
    mixbus::SoundSource src{&tone, &Tone::render_add};
    bus.add_source(&src);
    bus.start_track();
    bus.set_auto_volume(true);
    run(bus, 5);
    CHECK_EQ(bus.auto_gain_log(), 0);
    tone.amplitude = 1000;
    run(bus, 1);
    CHECK(bus.auto_gain_log() > 0);
    bus.start_track();
    bus.set_auto_volume(false);
    const int32_t plain = run(bus, 2);
    CHECK(plain >= 998 && plain <= 1000);
}

void test_end_fade() {
    std::printf("test_end_fade\n");
    mixbus::MixBus bus;
    Tone tone{10000};
    mixbus::SoundSource src{&tone, &Tone::render_add};
    bus.add_source(&src);
    bus.start_track();
    bus.set_end_frame(5 * kRate);
    bus.set_end_fade(2 * kRate);
    // До затухания - полный уровень; за секунду до конца остаток половина,
    // громкость - четверть (пик меряется за 0.12 с до этой точки, там
    // остаток до 0.56 и громкость до 0.31); к концу - тишина.
    const int32_t before = run(bus, 2.9);
    CHECK(before >= 9990 && before <= 10000);
    const int32_t half = run(bus, 1.1);
    CHECK(half >= 2500 && half <= 3200);
    const int32_t tail = run(bus, 1.0);
    CHECK(tail < 40);
    CHECK_EQ(run(bus, 0.2), 0);
}

void test_meter() {
    std::printf("test_meter\n");
    CHECK_EQ(mixbus::level_of(32768), uint8_t{255});
    CHECK_EQ(mixbus::level_of(16384), uint8_t{223}); // октава - 32 единицы
    CHECK_EQ(mixbus::level_of(0), uint8_t{0});
    CHECK_EQ(mixbus::level_of(100), uint8_t{0}); // ниже -48 дБ

    mixbus::MixBus bus;
    mixbus::Meter meter;
    Tone tone{32000, 1000};
    mixbus::SoundSource src{&tone, &Tone::render_add};
    bus.add_source(&src);
    bus.start_track();
    bus.set_meter(&meter);
    // Выключенный измеритель рендер не кормит.
    run(bus, 0.1);
    uint8_t off[mixbus::kSpectrumBands];
    CHECK(!meter.take_spectrum(off));
    meter.enable(true);
    uint8_t bands[mixbus::kSpectrumBands];
    CHECK(!meter.take_spectrum(bands));
    run(bus, 0.1);
    uint8_t l = 0, r = 0;
    meter.take_vu(l, r);
    CHECK(l >= 253 && r >= 253);
    // 1 кГц - полоса 7 (861-1033 Гц): она у полной шкалы, дальние - ниже на
    // десятки дБ.
    CHECK(meter.take_spectrum(bands));
    CHECK(bands[7] >= 245);
    CHECK(bands[0] + 100 < bands[7] && bands[3] + 100 < bands[7] && bands[11] + 100 < bands[7] && bands[15] + 100 < bands[7]);
    CHECK(!meter.take_spectrum(bands)); // снимок отдан, новый ещё не набран

    // Высокая и низкая частота - свои полосы; вдвое тише - на 32 единицы ниже.
    tone = Tone{16000, 5500};
    run(bus, 0.1);
    CHECK(meter.take_spectrum(bands) && bands[13] >= 210 && bands[13] <= 228 && bands[7] + 100 < bands[13]);
    tone = Tone{32000, 172};
    run(bus, 0.1);
    CHECK(meter.take_spectrum(bands) && bands[1] >= 240 && bands[8] + 100 < bands[1]);
    // Снимок - по последним отсчётам: тон, сменившийся после прошлого
    // снимка, виден сразу, а прежний в полосы не попадает.
    tone = Tone{16000, 5500};
    run(bus, 0.1);
    tone = Tone{32000, 172};
    run(bus, 0.1);
    CHECK(meter.take_spectrum(bands) && bands[1] >= 240 && bands[13] + 100 < bands[1]);
    meter.take_vu(l, r);
    tone = Tone{0};
    run(bus, 0.1);
    meter.take_vu(l, r);
    CHECK(l == 0 && r == 0);
    CHECK(meter.take_spectrum(bands));
    for (const uint8_t b : bands) {
        CHECK_EQ(b, uint8_t{0});
    }

    // Удар между отправками: счёт посреди периода держит его до забора, хотя
    // последние 512 отсчётов - тишина.
    mixbus::Meter hit;
    hit.enable(true);
    std::vector<int16_t> burst(2 * mixbus::Meter::kFft), quiet(2 * mixbus::Meter::kFft, 0);
    for (uint32_t n = 0; n < mixbus::Meter::kFft; ++n) {
        burst[2 * n] = burst[2 * n + 1] = static_cast<int16_t>(20000 * std::sin(6.283185307179586 * 1000 * n / kRate));
    }
    hit.feed(burst.data(), mixbus::Meter::kFft);
    CHECK(hit.analyze());
    hit.feed(quiet.data(), mixbus::Meter::kFft);
    CHECK(hit.take_spectrum(bands) && bands[7] >= 200);
    hit.feed(quiet.data(), mixbus::Meter::kFft);
    CHECK(hit.take_spectrum(bands) && bands[7] == 0); // максимум сброшен забором
}

// Полосы против прямого ДПФ в double по тем же 512 отсчётам с окном Ханна:
// шум, смесь тонов, тон в полную шкалу. Перед снимком - 300 отсчётов
// другого сигнала: круг начинается не с нуля.
void test_meter_vs_dft() {
    std::printf("test_meter_vs_dft\n");
    constexpr uint32_t kN                               = mixbus::Meter::kFft;
    constexpr uint8_t kEdge[mixbus::kSpectrumBands + 1] = {1, 2, 3, 4, 5, 6, 8, 10, 13, 17, 23, 31, 43, 59, 82, 114, 186};
    uint32_t seed                                       = 12345;
    auto noise                                          = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return static_cast<int32_t>((seed >> 16) & 0x7fff) - 16384;
    };
    int worst = 0;
    for (int sig = 0; sig < 3; ++sig) {
        mixbus::Meter meter;
        meter.enable(true);
        // Ниже порога атаки: только сдвигает круг, окна с атакой не даёт.
        std::vector<int16_t> pre(300 * 2, 500), buf(kN * 2);
        meter.feed(pre.data(), 300);
        std::vector<double> x(kN);
        for (uint32_t n = 0; n < kN; ++n) {
            const double t = n / kRate;
            int32_t l = 0, r = 0;
            if (sig == 0) {
                l = noise();
                r = noise();
            } else if (sig == 1) {
                l = r =
                    static_cast<int32_t>(12000 * std::sin(6.2831853 * 100 * t) + 8000 * std::sin(6.2831853 * 1500 * t) + 4000 * std::sin(6.2831853 * 9000 * t));
            } else {
                l = r = static_cast<int32_t>(32767 * std::sin(6.2831853 * 3000 * t));
            }
            buf[2 * n]     = static_cast<int16_t>(l);
            buf[2 * n + 1] = static_cast<int16_t>(r);
            x[n]           = (l + r) / 2;
        }
        meter.feed(buf.data(), kN);
        uint8_t bands[mixbus::kSpectrumBands];
        CHECK(meter.take_spectrum(bands));
        for (uint32_t b = 0; b < mixbus::kSpectrumBands; ++b) {
            double power = 0;
            for (uint32_t k = kEdge[b]; k < kEdge[b + 1]; ++k) {
                double re = 0, im = 0;
                for (uint32_t n = 0; n < kN; ++n) {
                    const double w  = (0.5 - 0.5 * std::cos(6.283185307179586 * n / kN)) * x[n] / kN;
                    re             += w * std::cos(6.283185307179586 * k * n / kN);
                    im             -= w * std::sin(6.283185307179586 * k * n / kN);
                }
                power += re * re + im * im;
            }
            const int32_t v   = 255 + mixbus::log2_fixed(static_cast<uint32_t>(std::min(power, 4.0e9)), 4) - 425;
            const int32_t ref = power < 1 || v < 0 ? 0 : std::min(v, 255);
            if (ref >= 16) worst = std::max(worst, std::abs(ref - bands[b]));
        }
    }
    std::printf("  worst band deviation from DFT: %d\n", worst);
    CHECK(worst <= 2);
}

// Уровень полосы b прямым ДПФ в double по окну Ханна из 512 отсчётов x,
// кончая отсчётом end - 1, - шкалой измерителя.
int32_t dft_band(const std::vector<double>& x, uint32_t end, uint32_t b) {
    constexpr uint32_t kN                               = mixbus::Meter::kFft;
    constexpr uint8_t kEdge[mixbus::kSpectrumBands + 1] = {1, 2, 3, 4, 5, 6, 8, 10, 13, 17, 23, 31, 43, 59, 82, 114, 186};
    double power                                        = 0;
    for (uint32_t k = kEdge[b]; k < kEdge[b + 1]; ++k) {
        double re = 0, im = 0;
        for (uint32_t n = 0; n < kN; ++n) {
            const double w  = (0.5 - 0.5 * std::cos(6.283185307179586 * n / kN)) * x[end - kN + n] / kN;
            re             += w * std::cos(6.283185307179586 * k * n / kN);
            im             -= w * std::sin(6.283185307179586 * k * n / kN);
        }
        power += re * re + im * im;
    }
    const int32_t v = 255 + mixbus::log2_fixed(static_cast<uint32_t>(std::min(power, 4.0e9)), 4) - 425;
    return power < 1 || v < 0 ? 0 : std::min(v, 255);
}

// Короткий удар у края последнего окна: счёт берёт ещё окно с атакой в
// центре, и полоса - его уровень, а не ослабленный окном Ханна.
void test_meter_onset() {
    std::printf("test_meter_onset\n");
    mixbus::Meter meter;
    meter.enable(true);
    std::vector<double> x(1536, 0.0);
    for (uint32_t n = 1024; n < 1056; ++n) {
        x[n] = std::round(20000 * std::sin(6.283185307179586 * 1000 * n / kRate));
    }
    std::vector<int16_t> buf(2 * 256);
    for (uint32_t blk = 0; blk < 6; ++blk) {
        for (uint32_t n = 0; n < 256; ++n) {
            buf[2 * n] = buf[2 * n + 1] = static_cast<int16_t>(x[blk * 256 + n]);
        }
        meter.feed(buf.data(), 256);
    }
    uint8_t bands[mixbus::kSpectrumBands];
    CHECK(meter.take_spectrum(bands));
    const int32_t centered = dft_band(x, 1152 + 256, 7), last = dft_band(x, 1536, 7);
    std::printf("  band 7: meter %d, centered window %d, last window %d\n", bands[7], centered, last);
    CHECK(centered > last + 20);
    CHECK(std::abs(bands[7] - centered) <= 2);
}

// Источник машины звучит после конца трека и на его паузе, авторегулировка
// трека его не трогает; set_muted гасит всё. Пик машины 4000 - тон 1 кГц,
// ФВЧ 5 Гц его не меняет.
void test_machine_source() {
    std::printf("test_machine_source\n");
    mixbus::MixBus bus;
    Tone track{10000}, machine{4000, 1000};
    mixbus::SoundSource tsrc{&track, &Tone::render_add}, msrc{&machine, &Tone::render_add};
    bus.add_source(&tsrc);
    CHECK(bus.add_machine_source(&msrc));
    bus.start_track();
    bus.set_highpass_hz(mixbus::MixBus::kHighpassHz, kRate);
    bus.set_auto_volume(true);
    bus.set_limiter(true);
    bus.set_end_frame(2 * kRate);
    const int32_t both = run(bus, 1.0);
    CHECK(both > 4000 && both <= 32767);
    // Конец трека: остаётся машина, ровно её уровень.
    const int32_t after_end = run(bus, 1.5);
    std::printf("  both %d, after the end %d\n", both, after_end);
    CHECK(after_end >= 3990 && after_end <= 4000);
    // Пауза нового трека - тоже.
    bus.start_track();
    bus.set_paused(true);
    CHECK(run(bus, 0.3) >= 3990);
    // Меню NMI: тишина, источники не зовутся.
    bus.set_muted(true);
    const double phase = (run(bus, 0.05), machine.phase);
    CHECK_EQ(run(bus, 0.3), 0);
    CHECK(machine.phase == phase);
    bus.set_muted(false);
    CHECK(run(bus, 0.3) >= 3990);
}

// Пауза и снос под меню NMI: затухание трека считается исполненным. После
// снятия глушения трек молчит и не зовётся, звучит только машина; снос
// видит faded() сразу, не дожидаясь потолка.
void test_mute_then_pause() {
    std::printf("test_mute_then_pause\n");
    mixbus::MixBus bus;
    Tone track{10000}, machine{4000, 1000};
    mixbus::SoundSource tsrc{&track, &Tone::render_add}, msrc{&machine, &Tone::render_add};
    bus.add_source(&tsrc);
    CHECK(bus.add_machine_source(&msrc));
    bus.start_track();
    CHECK(run(bus, 0.2) > 4000);
    bus.set_muted(true);
    run(bus, 0.05);
    CHECK_EQ(run(bus, 0.1), 0);
    bus.set_paused(true);
    CHECK_EQ(run(bus, 0.1), 0);
    const double phase = track.phase;
    bus.set_muted(false);
    const int32_t after = run(bus, 0.3);
    std::printf("  after unmute on pause: peak %d, track called %d\n", after, track.phase != phase ? 1 : 0);
    CHECK(after >= 3990 && after <= 4000);
    CHECK(track.phase == phase);
    // Снос под глушением: faded() после одного прохода.
    bus.set_paused(false);
    CHECK(run(bus, 0.1) > 4000);
    bus.set_muted(true);
    run(bus, 0.05);
    bus.fade_out();
    run(bus, 0.01);
    CHECK(bus.faded());
}

// Громкость трека (#2A карты GS): трек тише ровно во столько раз, машина
// как была; новый трек - снова полная.
void test_track_gain() {
    std::printf("test_track_gain\n");
    mixbus::MixBus bus;
    Tone track{10000}, machine{4000, 1000};
    mixbus::SoundSource tsrc{&track, &Tone::render_add}, msrc{&machine, &Tone::render_add};
    bus.add_source(&tsrc);
    bus.start_track();
    const int32_t full = run(bus, 0.2);
    bus.set_track_gain(2048);
    const int32_t half = run(bus, 0.2);
    bus.set_track_gain(0);
    const int32_t none = run(bus, 0.2);
    CHECK(bus.add_machine_source(&msrc));
    const int32_t only_machine = run(bus, 0.2);
    bus.start_track();
    const int32_t again = run(bus, 0.2);
    std::printf("  track gain: full %d, half %d, zero %d, machine under zero %d, next track %d\n", full, half, none, only_machine, again);
    CHECK(std::abs(half * 2 - full) <= 2);
    CHECK_EQ(none, 0);
    CHECK(only_machine >= 3990 && only_machine <= 4000);
    CHECK(again > full);
}

} // namespace

void run_mixbus_level_tests() {
    test_track_gain();
    test_auto_volume();
    test_end_fade();
    test_machine_source();
    test_mute_then_pause();
    test_meter();
    test_meter_vs_dft();
    test_meter_onset();
}
