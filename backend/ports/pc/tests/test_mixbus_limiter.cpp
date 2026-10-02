// SPDX-License-Identifier: MIT
#include "testing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/audio/mixbus.h"

using namespace soundsinth;

namespace {

// Источник, отдающий заранее подготовленный кусок. Проверяем шину, а не
// движок, поэтому вход задаётся руками.
struct Canned {
    const int32_t* l;
    const int32_t* r;
    uint32_t pos = 0;
    uint32_t len = 0;

    static void render_add(void* self, int32_t* ml, int32_t* mr, uint32_t n) {
        Canned* c = static_cast<Canned*>(self);
        for (uint32_t i = 0; i < n; ++i, ++c->pos) {
            const uint32_t p = c->pos < c->len ? c->pos : c->len - 1;
            // Вход задан в единицах int16, шина - в Q24.8.
            ml[i] += c->l[p] * (1 << mixbus::kMixFracBits);
            mr[i] += c->r[p] * (1 << mixbus::kMixFracBits);
        }
    }
    mixbus::SoundSource as_source() { return mixbus::SoundSource{this, &render_add}; }
};

// Компрессор здесь отключён нарочно, порогом выше шкалы.
//
// Он живёт в том же проходе, что и лимитер (apply_limiter), и по
// умолчанию включён на 90% шкалы. Но эти тесты про лимитер: что он не
// трогает материал, который не клипует, и что потолок соблюдается точно.
// С работающим компрессором оба утверждения проверялись бы уже не о
// лимитере: законный -32768 компрессор поджимает, так и задумано.
std::vector<int16_t> run(const std::vector<int32_t>& l, const std::vector<int32_t>& r, bool limiter) {
    Canned src{l.data(), r.data(), 0, static_cast<uint32_t>(l.size())};
    mixbus::SoundSource s = src.as_source();
    mixbus::MixBus bus;
    bus.add_source(&s);
    bus.set_limiter(limiter);
    bus.set_compressor(0x7FFFFFFF, 0);

    std::vector<int16_t> out(l.size() * 2);
    uint32_t done = 0;
    while (done < l.size()) {
        const uint32_t n =
            static_cast<uint32_t>(l.size() - done) < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? static_cast<uint32_t>(l.size() - done) : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        bus.render(out.data() + done * 2, n);
        done += n;
    }
    return out;
}

// Трекерные форматы должны идти через шину как раньше: у них уровень
// задан самим файлом и сверен с эталонными плеерами. Проверяем
// побайтово, а не "примерно так же".
void test_limiter_off_changes_nothing() {
    std::printf("test_mixbus_limiter_off_changes_nothing\n");
    std::vector<int32_t> l(2000), r(2000);
    for (uint32_t i = 0; i < l.size(); ++i) {
        // Заведомо за шкалой - там, где лимитер вмешался бы.
        l[i] = static_cast<int32_t>(70000.0 * std::sin(i * 0.05));
        r[i] = -l[i];
    }
    const std::vector<int16_t> off = run(l, r, false);
    for (uint32_t i = 0; i < off.size(); ++i) {
        CHECK(off[i] <= 32767 && off[i] >= -32768);
    }
    // Обрыв по шкале, как раньше: вершина синуса стоит на ней.
    bool saw_rail = false;
    for (int16_t v : off)
        if (v == 32767 || v == -32768) saw_rail = true;
    CHECK(saw_rail);
}

// Лимитер не должен оставить ни одного отсчёта за шкалой - в этом смысл
// прямого хода: ужатие ложится на тот же кадр, который его вызвал,
// поэтому перелёта не бывает.
void test_limiter_never_exceeds_scale() {
    std::printf("test_mixbus_limiter_never_exceeds_scale\n");
    std::vector<int32_t> l(20000), r(20000);
    for (uint32_t i = 0; i < l.size(); ++i) {
        // Тихо, потом резкий громкий кусок, потом снова тихо - так
        // проверяются и атака, и восстановление.
        const double amp = (i > 5000 && i < 9000) ? 200000.0 : 8000.0;
        l[i]             = static_cast<int32_t>(amp * std::sin(i * 0.07));
        r[i]             = static_cast<int32_t>(amp * std::sin(i * 0.11));
    }
    const std::vector<int16_t> on = run(l, r, true);

    uint32_t rail = 0;
    for (int16_t v : on)
        if (v == 32767 || v == -32768) ++rail;
    // Стоять на шкале лимитер может (он туда и целится), уходить за неё -
    // нет; int16 сам за неё не пустит, поэтому проверяем другое: громкий
    // кусок должен быть ужат, а не срезан полкой.
    CHECK(rail * 100 < on.size()); // меньше процента на самой шкале

    // Восстановление: к концу тихого хвоста усиление должно вернуться, иначе
    // после каждого удара музыка проседала бы навсегда.
    int32_t tail_peak = 0;
    for (uint32_t i = 19000; i < 20000; ++i) {
        const int32_t v = on[i * 2] < 0 ? -on[i * 2] : on[i * 2];
        if (v > tail_peak) tail_peak = v;
    }
    CHECK(tail_peak > 7800); // исходный пик хвоста 8000
}

// Шкала несимметрична: -32768 законен, +32768 уже нет. Лимитер не должен
// принимать законный минимум за перегруз - иначе он трогает материал,
// который не клипует. На GeneralUser это дало шесть изменившихся рендеров
// из тридцати двух.
void test_limiter_ignores_legal_full_scale_minimum() {
    std::printf("test_mixbus_limiter_ignores_legal_full_scale_minimum\n");
    std::vector<int32_t> l(64, -32768), r(64, -32768);
    const std::vector<int16_t> on  = run(l, r, true);
    const std::vector<int16_t> off = run(l, r, false);
    CHECK(on == off);
    for (int16_t v : on)
        CHECK_EQ(v, -32768);
}

// Мягкое насыщение: вход-выход по всей кривой. До порога (0.75 шкалы) -
// тождество побитово, выше - монотонно, без скачков, шкалу не переходит, на
// большом перегрузе (в 200 раз) - у самой шкалы. Кривая симметрична.
// u/(1+u) - точно по формуле, до единицы.
void test_soft_clip_curve() {
    std::printf("test_mixbus_soft_clip_curve\n");
    std::vector<int32_t> in;
    for (int32_t v = 0; v <= 70000; v += 7)
        in.push_back(v);
    in.push_back(6553600);
    std::vector<int32_t> neg(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        neg[i] = -in[i];

    Canned src{in.data(), neg.data(), 0, static_cast<uint32_t>(in.size())};
    mixbus::SoundSource s = src.as_source();
    mixbus::MixBus bus;
    bus.add_source(&s);
    bus.set_soft_clip(true);
    std::vector<int16_t> out(in.size() * 2);
    for (uint32_t done = 0; done < in.size();) {
        const uint32_t n =
            static_cast<uint32_t>(in.size() - done) < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? static_cast<uint32_t>(in.size() - done) : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        bus.render(out.data() + done * 2, n);
        done += n;
    }
    int32_t prev = -1, max_step = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const int32_t y = out[i * 2], yn = out[i * 2 + 1];
        CHECK_EQ(yn, -y);                       // симметрия
        if (in[i] <= 24576) CHECK_EQ(y, in[i]); // до порога не тронуто
        if (in[i] > 24576) {
            const double exact = 24576.0 + 8191.0 * (in[i] - 24576) / (8191.0 + (in[i] - 24576));
            CHECK(std::fabs(y - exact) <= 1.0);
        }
        CHECK(y <= 32767);
        CHECK(y >= prev); // монотонно
        if (prev >= 0 && i + 1 < in.size()) max_step = std::max(max_step, y - prev);
        prev = y;
    }
    CHECK(max_step <= 7);                     // шаг входа 7 - наклон не больше единицы
    CHECK(out[(in.size() - 1) * 2] >= 32700); // большой перегруз - у шкалы
    CHECK(bus.soft_clipped_frames() > 0);
}

// Затухание: огибающая монотонна, после kFadeFrames - нули; кадр конца
// трека гасит выход к нему; start_track() возвращает звук следующему треку.
void test_fade_out_and_end_frame() {
    std::printf("test_mixbus_fade_out_and_end_frame\n");
    const uint32_t n = 4096;
    std::vector<int32_t> l(n, 20000), r(n, -20000);
    Canned src{l.data(), r.data(), 0, n};
    mixbus::SoundSource s = src.as_source();
    mixbus::MixBus bus;
    bus.add_source(&s);
    const uint32_t kF = mixbus::MixBus::kFadeFrames;
    std::vector<int16_t> buf(kF * 2);

    bus.render(buf.data(), kF);
    CHECK_EQ(buf[0], 20000);
    bus.fade_out();
    bus.render(buf.data(), kF);
    bool monotone = true;
    for (uint32_t i = 1; i < kF; ++i) {
        if (buf[i * 2] > buf[(i - 1) * 2]) monotone = false;
    }
    CHECK(monotone);
    CHECK(buf[0] > 19000);
    CHECK(bus.faded());
    bus.render(buf.data(), kF);
    bool zeros = true;
    for (int16_t v : buf) {
        if (v != 0) zeros = false;
    }
    CHECK(zeros);

    // Следующий трек: звук вернулся; кадр конца через три буфера.
    bus.start_track();
    bus.set_end_frame(3 * kF);
    bus.render(buf.data(), kF);
    CHECK_EQ(buf[0], 20000);
    bus.render(buf.data(), kF); // затухание - к концу этого буфера плюс kF
    bus.render(buf.data(), kF);
    CHECK(bus.faded());
    CHECK_EQ(buf[(kF - 1) * 2], 0);
}

} // namespace

// Источник, пишущий сырые Q24.8 - для проверки округления на выходе.
struct RawQ {
    const int32_t* v;
    uint32_t len;
    uint32_t pos = 0;
    static void render_add(void* self, int32_t* ml, int32_t* mr, uint32_t n) {
        RawQ* c = static_cast<RawQ*>(self);
        for (uint32_t i = 0; i < n; ++i, ++c->pos) {
            const int32_t x  = c->pos < c->len ? c->v[c->pos] : 0;
            ml[i]           += x;
            mr[i]           += x;
        }
    }
};

// Выключенный лимитер - ровно полка clamp_s16 входа; включённый (без
// компрессора) на материале в пределах шкалы, включая 32767 и -32768, -
// побайтово как выключенный.
void test_limiter_off_is_clamp_and_on_is_transparent() {
    std::printf("test_mixbus_limiter_off_is_clamp_and_on_is_transparent\n");
    std::vector<int32_t> l(2000), r(2000);
    for (uint32_t i = 0; i < l.size(); ++i) {
        l[i] = static_cast<int32_t>(70000.0 * std::sin(i * 0.05));
        r[i] = -l[i];
    }
    const std::vector<int16_t> off = run(l, r, false);
    uint32_t bad                   = 0;
    for (uint32_t i = 0; i < l.size(); ++i) {
        if (off[i * 2] != std::clamp(l[i], -32768, 32767) || off[i * 2 + 1] != std::clamp(r[i], -32768, 32767)) ++bad;
    }
    CHECK_EQ(bad, 0u);

    std::vector<int32_t> a(20000), b(20000);
    uint32_t x = 7;
    for (uint32_t i = 0; i < a.size(); ++i) {
        x    = x * 1664525u + 1013904223u;
        a[i] = static_cast<int32_t>(x >> 16) - 32768; // -32768..32767
        b[i] = -a[i] > 32767 ? 32767 : -a[i];
    }
    a[100] = 32767;
    a[101] = -32768;
    CHECK(run(a, b, true) == run(a, b, false));
}

// Округление Q24.8 на выходе - к ближайшему, половина вверх.
void test_bus_rounding() {
    std::printf("test_mixbus_bus_rounding\n");
    const int32_t in[7]   = {127, 128, 129, -127, -128, -129, -384};
    const int16_t want[7] = {0, 1, 1, 0, 0, -1, -1};
    RawQ src{in, 7};
    mixbus::SoundSource s{&src, &RawQ::render_add};
    mixbus::MixBus bus;
    bus.add_source(&s);
    bus.set_limiter(false);
    bus.set_compressor(0x7FFFFFFF, 0);
    int16_t out[14];
    bus.render(out, 7);
    for (uint32_t i = 0; i < 7; ++i)
        CHECK_EQ(out[i * 2], want[i]);
}

// Счётчики шины: скачок больше kJumpThreshold на границе буфера - разрыв,
// ровно kJumpThreshold - нет; полка через границу буфера - одна серия.
void test_bus_counters_across_buffers() {
    std::printf("test_mixbus_bus_counters_across_buffers\n");
    {
        std::vector<int32_t> l(600, 0), r(600, 0);
        for (uint32_t i = 300; i < 600; ++i)
            l[i] = 30000;
        Canned src{l.data(), r.data(), 0, 600};
        mixbus::SoundSource s = src.as_source();
        mixbus::MixBus bus;
        bus.add_source(&s);
        bus.set_compressor(0x7FFFFFFF, 0);
        std::vector<int16_t> out(1200);
        bus.render(out.data(), 256);
        bus.render(out.data() + 512, 256);
        bus.render(out.data() + 1024, 88);
        CHECK_EQ(bus.jumps(), 1u);
        CHECK_EQ(bus.max_jump(), 30000u);
    }
    {
        std::vector<int32_t> l(600, 0), r(600, 0);
        for (uint32_t i = 300; i < 600; ++i)
            l[i] = mixbus::MixBus::kJumpThreshold;
        Canned src{l.data(), r.data(), 0, 600};
        mixbus::SoundSource s = src.as_source();
        mixbus::MixBus bus;
        bus.add_source(&s);
        bus.set_compressor(0x7FFFFFFF, 0);
        std::vector<int16_t> out(1200);
        bus.render(out.data(), 256);
        bus.render(out.data() + 512, 256);
        bus.render(out.data() + 1024, 88);
        CHECK_EQ(bus.jumps(), 0u);
    }
    {
        // 300 кадров по 40000 с кадра 200 - через границу 256; пауза; ещё 10.
        std::vector<int32_t> l(800, 0), r(800, 0);
        for (uint32_t i = 200; i < 500; ++i)
            l[i] = 40000;
        for (uint32_t i = 600; i < 610; ++i)
            l[i] = 40000;
        Canned src{l.data(), r.data(), 0, 800};
        mixbus::SoundSource s = src.as_source();
        mixbus::MixBus bus;
        bus.add_source(&s);
        bus.set_limiter(false);
        bus.set_compressor(0x7FFFFFFF, 0);
        std::vector<int16_t> out(1600);
        for (uint32_t done = 0; done < 800; done += 256) {
            const uint32_t n = 800 - done < 256 ? 800 - done : 256;
            bus.render(out.data() + done * 2, n);
        }
        CHECK_EQ(bus.clipped_frames(), 310u);
        CHECK_EQ(bus.longest_clip_run(), 300u);
    }
}

// Оракул шины: округление, лимитер с компрессором, полка или мягкое
// насыщение, счёт полки и разрывов - прямо по определению, отдельными
// проходами по кадру.
struct RefBus {
    bool limiter           = false;
    bool soft              = false;
    int32_t knee           = mixbus::MixBus::kSoftKneeDefault;
    int32_t comp_threshold = SOUNDSINTH_MIDI_COMPRESSOR_THRESHOLD;
    int32_t comp_amount    = SOUNDSINTH_MIDI_COMPRESSOR_AMOUNT;
    int32_t gain           = 32768;
    uint32_t limited = 0, min_gain = 32768;
    uint32_t clipped = 0, longest = 0, run = 0, jumps = 0, max_jump = 0, soft_clipped = 0;
    int32_t pl = 0, pr = 0;

    int32_t soft_clip(int32_t x) const {
        const int32_t a = x < 0 ? -x : x;
        if (a <= knee) return x;
        const uint32_t span = static_cast<uint32_t>(32767 - knee);
        const int32_t y     = 32767 - static_cast<int32_t>(span * span / (span + static_cast<uint32_t>(a - knee)));
        return x < 0 ? -y : y;
    }
    void frame(int32_t ql, int32_t qr, int16_t* out) {
        int32_t l = (ql + 128) >> 8;
        int32_t r = (qr + 128) >> 8;
        if (limiter) {
            const int32_t al = l < 0 ? ~l : l, ar = r < 0 ? ~r : r;
            const int32_t peak = al > ar ? al : ar;
            if (peak > comp_threshold) {
                const int32_t q      = static_cast<int32_t>((static_cast<uint32_t>(comp_threshold) << 15) / uint32_t(peak));
                const int32_t target = 32768 - (((32768 - q) * comp_amount) >> 15);
                if (target < gain) gain = target;
            }
            if (peak > SOUNDSINTH_MIDI_LIMITER_CEILING && static_cast<int32_t>((static_cast<int64_t>(peak) * gain) >> 15) > SOUNDSINTH_MIDI_LIMITER_CEILING) {
                gain = static_cast<int32_t>((static_cast<uint32_t>(SOUNDSINTH_MIDI_LIMITER_CEILING) << 15) / uint32_t(peak));
            }
            if (gain < 32768) {
                l = static_cast<int32_t>((static_cast<int64_t>(l) * gain) >> 15);
                r = static_cast<int32_t>((static_cast<int64_t>(r) * gain) >> 15);
                ++limited;
                if (static_cast<uint32_t>(gain) < min_gain) min_gain = static_cast<uint32_t>(gain);
                const int32_t step  = (32768 - gain) >> SOUNDSINTH_MIDI_LIMITER_RELEASE_SHIFT;
                gain               += step > 0 ? step : 1;
            }
        }
        if (l < -32768 || l > 32767 || r < -32768 || r > 32767) {
            ++clipped;
            if (++run > longest) longest = run;
        } else {
            run = 0;
        }
        int32_t ol  = soft ? soft_clip(l) : std::clamp(l, -32768, 32767);
        int32_t orr = soft ? soft_clip(r) : std::clamp(r, -32768, 32767);
        if (soft && (ol != l || orr != r)) ++soft_clipped;
        const int32_t d = std::max(std::abs(ol - pl), std::abs(orr - pr));
        if (d > mixbus::MixBus::kJumpThreshold) {
            ++jumps;
            if (static_cast<uint32_t>(d) > max_jump) max_jump = static_cast<uint32_t>(d);
        }
        pl     = ol;
        pr     = orr;
        out[0] = static_cast<int16_t>(ol);
        out[1] = static_cast<int16_t>(orr);
    }
};

// Шина против оракула на дробном входе Q24.8: тишина, полная шкала, перегруз
// в разы, крайние значения, случайные куски; буферы разной длины; все
// четыре сочетания лимитера и мягкого насыщения; выход и все счётчики.
void test_bus_matches_oracle() {
    std::printf("test_mixbus_bus_matches_oracle\n");
    const uint32_t n = 40000;
    std::vector<int32_t> ql(n), qr(n);
    uint32_t x = 12345;
    auto rnd   = [&x]() {
        x = x * 1664525u + 1013904223u;
        return x;
    };
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t seg  = (i / 1500) % 6;
        const int32_t scale = seg == 0   ? 0
                              : seg == 1 ? 32768 * 256
                              : seg == 2 ? 4 * 32768 * 256
                              : seg == 3 ? 256 * 256
                              : seg == 4 ? 40 * 32768 * 256
                                         : 2 * 32768 * 256;
        const auto v        = [&]() {
            const int64_t s = static_cast<int64_t>(static_cast<int32_t>(rnd())) * scale / 2147483648LL;
            return static_cast<int32_t>(s);
        };
        ql[i] = v();
        qr[i] = (rnd() & 3) == 0 ? ql[i] : v();
    }
    const int32_t edges[] = {32767 * 256 + 127, 32767 * 256 + 128, -32768 * 256 - 128, -32768 * 256 - 129, 1340000000, -1340000000, 128, -128, -129, 127};
    for (uint32_t k = 0; k < sizeof(edges) / sizeof(edges[0]); ++k) {
        ql[100 + k] = edges[k];
        qr[200 + k] = edges[k];
    }
    for (int mode = 0; mode < 4; ++mode) {
        const bool limiter = (mode & 1) != 0, soft = (mode & 2) != 0;
        struct Two {
            const int32_t* l;
            const int32_t* r;
            uint32_t pos = 0;
            static void render_add(void* self, int32_t* ml, int32_t* mr, uint32_t m) {
                Two* t = static_cast<Two*>(self);
                for (uint32_t i = 0; i < m; ++i, ++t->pos) {
                    ml[i] += t->l[t->pos];
                    mr[i] += t->r[t->pos];
                }
            }
        } src{ql.data(), qr.data()};
        mixbus::SoundSource s{&src, &Two::render_add};
        mixbus::MixBus bus;
        bus.add_source(&s);
        bus.set_limiter(limiter);
        bus.set_soft_clip(soft);
        RefBus ref;
        ref.limiter = limiter;
        ref.soft    = soft;
        std::vector<int16_t> out(n * 2), want(n * 2);
        uint32_t done = 0, k = 0;
        while (done < n) {
            const uint32_t sizes[] = {256, 1, 37, 256, 128, 2, 255};
            uint32_t m             = sizes[k++ % 7];
            if (m > n - done) m = n - done;
            bus.render(out.data() + done * 2, m);
            for (uint32_t i = 0; i < m; ++i)
                ref.frame(ql[done + i], qr[done + i], &want[(done + i) * 2]);
            done += m;
        }
        uint32_t bad = 0;
        for (uint32_t i = 0; i < n * 2; ++i)
            bad += out[i] != want[i];
        std::printf("  limiter %d, soft %d: differences %u, plateau %u/%u, jumps %u (max %u), softened %u, compressed %u\n", limiter, soft, bad,
                    bus.clipped_frames(), bus.longest_clip_run(), bus.jumps(), bus.max_jump(), bus.soft_clipped_frames(), bus.limited_frames());
        CHECK_EQ(bad, 0u);
        CHECK_EQ(bus.clipped_frames(), ref.clipped);
        CHECK_EQ(bus.longest_clip_run(), ref.longest);
        CHECK_EQ(bus.jumps(), ref.jumps);
        CHECK_EQ(bus.max_jump(), ref.max_jump);
        CHECK_EQ(bus.soft_clipped_frames(), ref.soft_clipped);
        CHECK_EQ(bus.limited_frames(), ref.limited);
        CHECK_EQ(bus.min_gain_q15(), ref.min_gain);
        CHECK(bus.jumps() > 0);
        CHECK(limiter ? bus.limited_frames() > 0 : bus.clipped_frames() > 0); // ветки не пустые
    }
}

void run_mixbus_limiter_tests() {
    test_bus_matches_oracle();
    test_limiter_off_is_clamp_and_on_is_transparent();
    test_bus_rounding();
    test_bus_counters_across_buffers();
    test_fade_out_and_end_frame();
    test_soft_clip_curve();
    test_limiter_off_changes_nothing();
    test_limiter_never_exceeds_scale();
    test_limiter_ignores_legal_full_scale_minimum();
}
