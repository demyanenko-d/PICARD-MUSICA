// Разворот ping-pong петли в прямую при упаковке (LoopUnroll): что лежит в
// PSRAM, какие петли выбирает планировщик и как голос считает смещение за
// концом петли.

#include "testing.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "core/codec/dpcm8.h"
#include "core/engine/voice.h"
#include "core/codec/loop_unroll.h"
#include "core/memory/psram_store.h"
#include "core/codec/sample_pack.h"

using namespace soundsinth;
using soundsinth::model::LoopUnroll;
using soundsinth::model::ResidentEncoding;

namespace {

// Ожидаемый развёрнутый поток: s[0..le), обратный проход, хвост s[le..).
std::vector<int16_t> unrolled(const std::vector<int16_t>& s, uint32_t ls, uint32_t le, LoopUnroll mode) {
    std::vector<int16_t> u(s.begin(), s.begin() + le);
    if (mode == LoopUnroll::Xm) u.push_back(s[le - 1]);
    for (uint32_t i = le - 1; i > ls; --i) u.push_back(s[i]);
    u.insert(u.end(), s.begin() + le, s.end());
    return u;
}

// Весь резидентный поток сэмпла в int16, как его читает голос.
std::vector<int16_t> read_stream(memory::PsramStore& psram, uint16_t first_page, ResidentEncoding e, uint32_t n) {
    std::vector<int16_t> out(n);
    dpcm8::Dpcm8State st;
    uint16_t page = first_page;
    uint32_t pos = 0;
    const uint32_t bps = soundsinth::model::resident_bytes_per_sample(e);
    for (uint32_t i = 0; i < n; ++i) {
        if (pos >= memory::kPsramPageBytes) {
            page = memory::psram_page_next(psram, page);
            pos = 0;
        }
        const uint8_t* p = memory::psram_page_ptr(psram, page) + pos;
        if (e == ResidentEncoding::Raw8) {
            out[i] = static_cast<int8_t>(p[0]);
        } else if (e == ResidentEncoding::Raw16) {
            out[i] = static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)));
        } else {
            out[i] = dpcm8::decode_delta(p[0], st);
        }
        pos += bps;
    }
    return out;
}

// Пакует s с разворотом кусками cut (через границу петли) и сверяет с
// эталоном. Raw8 и Raw16 - побайтово. Dpcm8: прямой ход совпадает с
// упаковкой эталона точно, обратный - по ошибке против исходника.
// harsh - пила со скачком на всю шкалу (у Dpcm8 срез крутизны), иначе
// синус с шумом, как у настоящего сэмпла.
void check_unroll(ResidentEncoding e, LoopUnroll mode, uint32_t n, uint32_t ls, uint32_t le, uint32_t cut,
                  bool harsh = true) {
    std::vector<int16_t> s(n);
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t noise = static_cast<int32_t>((i * 2654435761u) >> 26);
        const int32_t v = harsh ? static_cast<int32_t>((i * 7u) % 4000u) * 8 - 16000 + noise
                                : static_cast<int32_t>(20000.0 * std::sin(i * 0.0627)) + noise;
        s[i] = static_cast<int16_t>(e == ResidentEncoding::Raw8 ? v / 256 : v);
    }
    const std::vector<int16_t> want = unrolled(s, ls, le, mode);

    memory::PsramStore psram;
    memory::psram_create(psram);

    sample_pack::SamplePacker packer(psram, e);
    packer.set_loop_unroll(mode, ls, le);
    for (uint32_t pos = 0; pos < n;) {
        const uint32_t k = (n - pos) < cut ? (n - pos) : cut;
        CHECK(packer.add_samples(s.data() + pos, k));
        pos += k;
    }
    const sample_pack::PackResult r = packer.finish();
    CHECK(r.ok);
    CHECK_EQ(r.total_samples, static_cast<uint32_t>(want.size()));
    CHECK_EQ(r.total_samples, n + soundsinth::model::loop_unroll_extra(mode, le - ls));

    sample_pack::SamplePacker ref(psram, e);
    CHECK(ref.add_samples(want.data(), static_cast<uint32_t>(want.size())));
    const sample_pack::PackResult rr = ref.finish();
    CHECK(rr.ok);

    const std::vector<int16_t> got = read_stream(psram, r.first_page, e, r.total_samples);
    const std::vector<int16_t> exp = read_stream(psram, rr.first_page, e, rr.total_samples);
    uint32_t diff = 0;
    int32_t worst = 0;
    for (uint32_t i = 0; i < got.size(); ++i) {
        const int32_t d = std::abs(static_cast<int32_t>(got[i]) - static_cast<int32_t>(exp[i]));
        if (d != 0) ++diff;
        if (d > worst) worst = d;
        if (i < le) CHECK(d == 0); // прямой ход кодируется тем же путём
    }
    // Обратный ход против зеркала распакованного прямого: у Raw8 и Raw16
    // совпадает точно, у Dpcm8 печатается.
    uint32_t mirror_diff = 0;
    int32_t mirror_worst = 0;
    {
        const std::vector<int16_t> fwd(got.begin(), got.begin() + le);
        std::vector<int16_t> tail_fwd = fwd;
        tail_fwd.insert(tail_fwd.end(), got.end() - (n - le), got.end());
        const std::vector<int16_t> mirror = unrolled(tail_fwd, ls, le, mode);
        for (uint32_t i = le; i < got.size(); ++i) {
            const int32_t d = std::abs(static_cast<int32_t>(got[i]) - static_cast<int32_t>(mirror[i]));
            if (i >= mirror.size() - (n - le)) break; // хвост кодируется из исходника
            if (d != 0) ++mirror_diff;
            if (d > mirror_worst) mirror_worst = d;
        }
    }
    // Ошибка против исходника: прямой ход (ошибка кодека) и обратный
    // (второе поколение).
    double fwd_sq = 0.0, rev_sq = 0.0;
    const uint32_t extra = r.total_samples - n;
    for (uint32_t i = ls; i < le; ++i) fwd_sq += std::pow(double(got[i]) - double(want[i]), 2.0);
    for (uint32_t i = le; i < le + extra; ++i) rev_sq += std::pow(double(got[i]) - double(want[i]), 2.0);
    const double fwd_rms = std::sqrt(fwd_sq / double(le - ls));
    const double rev_rms = extra ? std::sqrt(rev_sq / double(extra)) : 0.0;
    if (e == ResidentEncoding::Dpcm8) {
        // Обратный ход кодирует распакованное - второе поколение ошибки. Замер:
        // синус с шумом 1.3 ошибки кодека, пила со скачками во всю шкалу 2.7.
        CHECK(rev_rms <= (harsh ? 3.0 : 1.5) * fwd_rms + 1.0);
        std::printf("    ошибка против исходника: прямой ход %.1f, обратный %.1f (СКО)\n", fwd_rms, rev_rms);
    } else {
        CHECK_EQ(diff, 0u);
        CHECK_EQ(mirror_diff, 0u);
    }
    std::printf("  %s %s n=%u петля %u..%u кусок %u: отсчётов %u, отличий %u (худшее %d), от зеркала %u (%d)\n",
                e == ResidentEncoding::Raw8 ? "Raw8" : e == ResidentEncoding::Raw16 ? "Raw16" : "Dpcm8",
                mode == LoopUnroll::Xm ? "XM" : "IT", n, ls, le, cut, r.total_samples, diff, worst, mirror_diff,
                mirror_worst);
    memory::psram_destroy(psram);
}

void test_unroll_packs_mirrored_loop() {
    std::printf("test_unroll_packs_mirrored_loop\n");
    const ResidentEncoding codecs[] = {ResidentEncoding::Raw8, ResidentEncoding::Raw16, ResidentEncoding::Dpcm8};
    for (const ResidentEncoding e : codecs) {
        for (const LoopUnroll mode : {LoopUnroll::Xm, LoopUnroll::It}) {
            // Петля через несколько страниц, хвост за петлёй, куски поперёк
            // конца петли.
            check_unroll(e, mode, 6000, 700, 5100, 999);
            // Петля до конца сэмпла: обратный проход пишется на последнем куске.
            check_unroll(e, mode, 3000, 10, 3000, 4096);
            // Короткая петля и кусок в один отсчёт.
            check_unroll(e, mode, 200, 50, 57, 1);
            // Сигнал как у настоящего сэмпла.
            check_unroll(e, mode, 6000, 700, 5100, 999, false);
        }
    }
}

// Петля в два отсчёта не разворачивается: обратного хода нет.
void test_unroll_skips_tiny_loop() {
    std::printf("test_unroll_skips_tiny_loop\n");
    memory::PsramStore psram;
    memory::psram_create(psram);
    std::vector<int16_t> s(100, 5);
    sample_pack::SamplePacker packer(psram, ResidentEncoding::Raw8);
    packer.set_loop_unroll(LoopUnroll::Xm, 40, 42);
    CHECK(packer.add_samples(s.data(), 100));
    const sample_pack::PackResult r = packer.finish();
    CHECK(r.ok);
    CHECK_EQ(r.total_samples, 100u);
    memory::psram_destroy(psram);
}

// Поток короче конца петли: развёрнутого сэмпла нет, finish отказывает.
void test_unroll_short_stream_fails() {
    std::printf("test_unroll_short_stream_fails\n");
    memory::PsramStore psram;
    memory::psram_create(psram);
    std::vector<int16_t> s(100, 5);
    sample_pack::SamplePacker packer(psram, ResidentEncoding::Raw8);
    packer.set_loop_unroll(LoopUnroll::It, 10, 150);
    CHECK(packer.add_samples(s.data(), 100));
    CHECK(!packer.finish().ok);
    memory::psram_destroy(psram);
}

soundsinth::model::SampleDescriptor pingpong(uint32_t len, uint32_t ls, uint32_t le) {
    soundsinth::model::SampleDescriptor sd;
    sd.resident_encoding = ResidentEncoding::Raw8;
    sd.length_samples = len;
    sd.loop_start = ls;
    sd.loop_end = le;
    sd.loop_enabled = true;
    sd.loop_bidirectional = true;
    return sd;
}

// Планировщик: только остаток после всех сэмплов, короткие первыми, петли
// в два отсчёта и прямые не трогаются.
void test_unroll_pingpong_loops_budget() {
    std::printf("test_unroll_pingpong_loops_budget\n");
    soundsinth::model::SampleDescriptor samples[6] = {
        pingpong(8192, 0, 8192),  // L 8192 - 8 страниц сверху
        pingpong(4096, 1024, 3072), // L 2048 - 2 страницы
        pingpong(2048, 0, 1024),  // L 1024 - 1 страница
        pingpong(1024, 100, 102), // L 2 - не разворачивается
        pingpong(1024, 0, 512),   // прямая петля
        pingpong(0, 0, 0),        // пустой
    };
    samples[4].loop_bidirectional = false;
    soundsinth::model::Song song;
    song.samples = samples;
    song.sample_count = 6;

    // Без разворота: 8 + 4 + 2 + 1 + 1 = 16 страниц. Сверху 3: влезают петли
    // в 1024 и 2048 (1 + 2), в 8192 (8) - нет.
    const uint16_t done = soundsinth::model::unroll_pingpong_loops(song, 16 + 3, LoopUnroll::Xm);
    CHECK_EQ(done, 2u);
    CHECK(samples[0].loop_unroll == LoopUnroll::None);
    CHECK(samples[1].loop_unroll == LoopUnroll::Xm);
    CHECK_EQ(samples[1].loop_end, 3072u + 2048u);
    CHECK_EQ(samples[1].length_samples, 4096u + 2048u);
    CHECK_EQ(soundsinth::model::loop_end_before_unroll(samples[1]), 3072u);
    CHECK(samples[2].loop_unroll == LoopUnroll::Xm);
    CHECK(samples[3].loop_unroll == LoopUnroll::None);
    CHECK(samples[4].loop_unroll == LoopUnroll::None);

    // IT: период 2L - 1, исходный конец восстанавливается.
    soundsinth::model::SampleDescriptor it = pingpong(1000, 100, 600);
    soundsinth::model::Song song_it;
    song_it.samples = &it;
    song_it.sample_count = 1;
    CHECK_EQ(soundsinth::model::unroll_pingpong_loops(song_it, 100, LoopUnroll::It), 1u);
    CHECK_EQ(it.loop_end, 100u + 2u * 500u - 1u);
    CHECK_EQ(it.length_samples, 1000u + 499u);
    CHECK_EQ(soundsinth::model::loop_end_before_unroll(it), 600u);

    // Трек не помещается и без разворота - ничего не разворачивается.
    soundsinth::model::SampleDescriptor big = pingpong(4096, 0, 1024);
    soundsinth::model::Song song_big;
    song_big.samples = &big;
    song_big.sample_count = 1;
    CHECK_EQ(soundsinth::model::unroll_pingpong_loops(song_big, 4, LoopUnroll::Xm), 0u);
    CHECK(big.loop_unroll == LoopUnroll::None);

    // Вид None - ничего не разворачивается и дескрипторы не тронуты (без ветки
    // None планировщик выбирал бы тот же сэмпл без конца).
    soundsinth::model::SampleDescriptor none = pingpong(1000, 100, 600);
    soundsinth::model::Song song_none;
    song_none.samples = &none;
    song_none.sample_count = 1;
    CHECK_EQ(soundsinth::model::unroll_pingpong_loops(song_none, 100, LoopUnroll::None), 0u);
    CHECK(none.loop_unroll == LoopUnroll::None);
    CHECK_EQ(none.loop_end, 600u);
    CHECK_EQ(none.length_samples, 1000u);
}

// Голос: смещение Oxx - в отсчётах файла, выход за конец петли - по
// исходному концу. У XM нота за концом петли не звучит, внутри - звучит.
void test_voice_offset_uses_loop_end_before_unroll() {
    std::printf("test_voice_offset_uses_loop_end_before_unroll\n");
    memory::PsramStore psram;
    memory::psram_create(psram);
    std::vector<int16_t> s(2000);
    for (uint32_t i = 0; i < s.size(); ++i) s[i] = static_cast<int16_t>(i % 100);
    sample_pack::SamplePacker packer(psram, ResidentEncoding::Raw8);
    packer.set_loop_unroll(LoopUnroll::Xm, 500, 1500);
    CHECK(packer.add_samples(s.data(), 2000));
    const sample_pack::PackResult r = packer.finish();
    CHECK(r.ok);

    soundsinth::model::SampleDescriptor sd = pingpong(2000 + 1000, 500, 2500);
    sd.loop_unroll = LoopUnroll::Xm;

    engine::Voice v;
    engine::voice_trigger(v, psram, sd, r.first_page, 60, soundsinth::model::FrequencyModel::Linear, 1500, 0);
    CHECK(!v.active); // 1500 - исходный конец петли: за ним у XM тишина
    engine::voice_trigger(v, psram, sd, r.first_page, 60, soundsinth::model::FrequencyModel::Linear, 1499, 0);
    CHECK(v.active);
    CHECK_EQ(v.loop_end, 2500u); // играет развёрнутую петлю
    memory::psram_destroy(psram);
}

} // namespace

// resident_pages - ровно столько страниц, сколько занимает SamplePacker: на
// этом держится правило "трек, который помещался, помещается и после
// разворота". Все кодировки, с прореживанием и без, длины у границ страниц
// и блоков точек; то же с развёрнутой петлей - от длины после разворота.
void test_resident_pages_matches_packer() {
    std::printf("test_resident_pages_matches_packer\n");
    static memory::PsramStore psram;
    memory::psram_create(psram);
    const uint32_t lengths[] = {1, 255, 256, 257, 1023, 1024, 1025, 131072, 131073};
    const ResidentEncoding encodings[] = {ResidentEncoding::Raw8, ResidentEncoding::Raw16, ResidentEncoding::Dpcm8};
    std::vector<int16_t> src(131073);
    for (uint32_t i = 0; i < src.size(); ++i) src[i] = static_cast<int16_t>((i * 37u) % 200u) - 100;
    uint32_t cases = 0, bad = 0;
    for (ResidentEncoding e : encodings) {
        for (int dec = 0; dec < 2; ++dec) {
            for (uint32_t n : lengths) {
                for (int unroll = 0; unroll < 2; ++unroll) {
                    const uint32_t stored = dec ? (n + 1) / 2 : n; // после прореживания
                    if (unroll && stored < 16) continue;
                    memory::psram_reset_track(psram);
                    const uint32_t free0 = memory::psram_free_page_count(psram);
                    sample_pack::SamplePacker p(psram, e, dec != 0);
                    uint32_t expect_len = stored;
                    if (unroll) {
                        const uint32_t ls = stored / 4, le = stored / 2;
                        p.set_loop_unroll(LoopUnroll::It, ls, le);
                        expect_len += soundsinth::model::loop_unroll_extra(LoopUnroll::It, le - ls);
                    }
                    for (uint32_t done = 0; done < n;) {
                        const uint32_t k = n - done < 1000u ? n - done : 1000u;
                        CHECK(p.add_samples(src.data() + done, k));
                        done += k;
                    }
                    const sample_pack::PackResult r = p.finish();
                    CHECK(r.ok);
                    const uint32_t used = free0 - memory::psram_free_page_count(psram);
                    const uint32_t want = soundsinth::model::resident_pages(e, expect_len);
                    ++cases;
                    if (r.total_samples != expect_len || used != want) {
                        if (++bad <= 5) {
                            std::printf("  кодек %u прореж %d n %u разворот %d: отсчётов %u (ждали %u), страниц %u (формула %u)\n",
                                        static_cast<unsigned>(e), dec, n, unroll, r.total_samples, expect_len, used, want);
                        }
                    }
                }
            }
        }
    }
    std::printf("  случаев %u, расхождений %u\n", cases, bad);
    CHECK_EQ(bad, 0u);
    memory::psram_destroy(psram);
}

void run_loop_unroll_tests() {
    test_resident_pages_matches_packer();
    test_unroll_packs_mirrored_loop();
    test_unroll_skips_tiny_loop();
    test_unroll_short_stream_fails();
    test_unroll_pingpong_loops_budget();
    test_voice_offset_uses_loop_end_before_unroll();
}
