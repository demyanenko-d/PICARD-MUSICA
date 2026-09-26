#include "testing.h"

#include "core/engine/song_duration.h"
#include "player/load/session_loader.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include <xmp.h>

#include "core/engine/effect_dispatch.h"
#include "core/engine/sequencer.h"
#include "core/formats/mod.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"

// Сверка диспетчера эффектов с libxmp: у libxmp независимые и разбор, и
// плеер (xmp_play_frame()/xmp_get_frame_info()), нашего кода там нет.
// Первый срез - Effect::SetVolume/VolumeSlide на реальном MOD:
// channels[].volume сравнивается с info.channel_info[].volume на каждом
// тике.
//
// PortaUp/PortaDown и Vibrato сверяются по звучащему периоду (test_period_matches_libxmp):
// период libxmp - info.channel_info[].period / 4096, у нас period +
// pitch_offset; finetune сэмпла досчитывается непрерывной формулой, от
// таблицы ProTracker расхождение меньше 2.5.

namespace {

using namespace soundsinth;

// Диагностическая обёртка: запоминает сырые ячейки последней строки рядом
// с engine::dispatch_row_effects, чтобы при расхождении напечатать, что
// видит наш загрузчик, а не только libxmp.
struct DiagContext {
    engine::DispatchContext dispatch_ctx;
    std::vector<soundsinth::model::PatternCell>* last_row;
};

void diag_row_callback(void* user, const soundsinth::model::PatternCell* cells, uint8_t channel_count) {
    auto* dc = static_cast<DiagContext*>(user);
    dc->last_row->assign(cells, cells + channel_count);
    engine::dispatch_row_effects(&dc->dispatch_ctx, cells, channel_count);
}

std::vector<uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void test_setvolume_volumeslide_matches_libxmp() {
    std::printf("test_sequencer_libxmp_setvolume_volumeslide_matches\n");

    const char* path = "SD/test_music/mod/00_00_00.mod"; // SetVolume(Cxx) и VolumeSlide(Axy) встречаются в первых паттернах, проверено
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("  файл не найден (%s) — пропуск\n", path);
        return;
    }
    std::vector<uint8_t> file_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // --- Наша сторона ---
    formats::MemoryByteSource mbs(file_bytes.data(), static_cast<uint32_t>(file_bytes.size()));
    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    const char* error = nullptr;
    CHECK(formats::mod::load(mbs.as_byte_source(), mem, song, &error));

    std::vector<engine::ChannelState> channels(song.channel_count);
    std::vector<soundsinth::model::PatternCell> last_row;
    DiagContext ctx{{channels.data(), &song}, &last_row};
    engine::PlayState ps;
    ctx.dispatch_ctx.ps = &ps;
    engine::channels_init(song, channels.data());
    CHECK(engine::sequencer_init(song, mem.psram, ps, diag_row_callback, &ctx));

    // --- libxmp сторона (независимый парсер + плеер) ---
    xmp_context xmp = xmp_create_context();
    CHECK(xmp != nullptr);
    CHECK_EQ(xmp_load_module_from_memory(xmp, file_bytes.data(), static_cast<long>(file_bytes.size())), 0);
    CHECK_EQ(xmp_start_player(xmp, static_cast<int>(engine::kSampleRateHz), 0), 0);

    struct xmp_module_info mod_info {};
    xmp_get_module_info(xmp, &mod_info);
    CHECK_EQ(mod_info.vol_base, 64); // одна и та же шкала громкости 0..64, иначе сравнение без приведения некорректно

    // Выравнивание: xmp_start_player() ещё не обработал строку 0 (её эффекты
    // применяются внутри первого xmp_play_frame()), а наш sequencer_init()
    // строку 0 уже применил. Один play_frame() здесь приводит обе стороны к
    // одной точке: "тик 0 строки 0 применён, дальше тики 1..speed-1".
    CHECK_EQ(xmp_play_frame(xmp), 0);
    struct xmp_frame_info info {};
    xmp_get_frame_info(xmp, &info);

    // 50, а не 400: с тика 51 на этом файле сэмпл 9 (канал 3, ins=31)
    // доигрывает до конца (channel_info.position у libxmp застывает на длине
    // сэмпла, 796) и канал замолкает. Это отслеживание позиции относительно
    // длины сэмпла, область голосов/микшера, а не диспетчера эффектов
    // ("естественное окончание сэмпла" разобрано и подтверждено отдельно).
    // До этой границы совпадение точное на каждом тике, после исправления
    // нескольких найденных по пути багов: невалидная ссылка на пустой сэмпл
    // игнорируется ProTracker'ом, молчащий канал должен сбрасывать volume в 0.
    constexpr int kTicks = 50;
    int mismatches = 0;
    for (int t = 0; t < kTicks; ++t) {
        for (uint8_t ch = 0; ch < song.channel_count && ch < XMP_MAX_CHANNELS; ++ch) {
            if (channels[ch].volume != static_cast<uint8_t>(info.channel_info[ch].volume)) {
                ++mismatches;
                if (mismatches <= 20) {
                    const auto& our_cell = ch < last_row.size() ? last_row[ch] : soundsinth::model::PatternCell{};
                    std::printf(
                        "  расхождение тик=%d канал=%u наш_volume=%u(active=%d) libxmp_volume=%u (наш row=%u libxmp "
                        "row=%d) наш: note=%u ins=%u fx=%d fxparam=%u | libxmp: note=%u ins=%u fxt=%u fxp=%u "
                        "sample=%u position=%u\n",
                        t, ch, channels[ch].volume, channels[ch].voice_active, info.channel_info[ch].volume, ps.row,
                        info.row, our_cell.note, our_cell.instrument, static_cast<int>(our_cell.effect.type),
                        our_cell.effect.param, info.channel_info[ch].event.note, info.channel_info[ch].event.ins,
                        info.channel_info[ch].event.fxt, info.channel_info[ch].event.fxp, info.channel_info[ch].sample,
                        info.channel_info[ch].position);
                }
            }
        }

        const bool our_ok = engine::sequencer_tick(song, mem.psram, ps, diag_row_callback, &ctx);
        engine::apply_continuous_effects(ps, channels.data(), song.channel_count, song.quirks, song.frequency_model,
                                         engine::kQ8One, ps.tick_in_row != 0);
        const int xmp_rc = xmp_play_frame(xmp);
        xmp_get_frame_info(xmp, &info);

        if (!our_ok || xmp_rc != 0) break;
    }
    CHECK_EQ(mismatches, 0);

    xmp_end_player(xmp);
    xmp_release_module(xmp);
    xmp_free_context(xmp);
    memory::track_memory_destroy(mem);
}

// Звучащий период каналов с PortaUp/PortaDown и Vibrato в строке против
// libxmp потиково (MOD, модель Amiga): расхождение не больше 2.5. Вибрато на
// тике 0 строки не сверяется: ProTracker там его не применяет (у libxmp
// QUIRK_PROTRACK), у нас этого квирка нет. Остальные эффекты здесь не
// сверяются: у части прочих каналов расхождения есть.
void test_period_matches_libxmp() {
    std::printf("test_period_matches_libxmp\n");
    static const char* kFiles[] = {"SD/test_music/mod/megaman.mod", "SD/test_music/mod/1pattern.mod",
                                   "music/src/mod/sunburn_at_night.mod", "music/src/mod/dope.mod"};
    for (const char* path : kFiles) {
        const std::vector<uint8_t> bytes = read_file(path);
        if (bytes.empty()) { std::printf("  ПРОПУСК (нет файла): %s\n", path); continue; }
        formats::MemoryByteSource mbs(bytes.data(), static_cast<uint32_t>(bytes.size()));
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        soundsinth::model::Song song;
        const char* error = nullptr;
        CHECK(formats::mod::load(mbs.as_byte_source(), mem, song, &error));
        std::vector<engine::ChannelState> channels(song.channel_count);
        std::vector<soundsinth::model::PatternCell> last_row;
        DiagContext ctx{{channels.data(), &song}, &last_row};
        engine::PlayState ps;
        ctx.dispatch_ctx.ps = &ps;
        engine::channels_init(song, channels.data());
        CHECK(engine::sequencer_init(song, mem.psram, ps, diag_row_callback, &ctx));

        xmp_context xmp = xmp_create_context();
        CHECK_EQ(xmp_load_module_from_memory(xmp, const_cast<uint8_t*>(bytes.data()), static_cast<long>(bytes.size())), 0);
        CHECK_EQ(xmp_start_player(xmp, static_cast<int>(engine::kSampleRateHz), 0), 0);
        CHECK_EQ(xmp_play_frame(xmp), 0); // выравнивание, как в сверке громкости
        struct xmp_frame_info info {};
        xmp_get_frame_info(xmp, &info);

        uint32_t porta_ticks = 0, vibrato_ticks = 0, mismatches = 0;
        for (int t = 0; t < 6000; ++t) {
            for (uint8_t ch = 0; ch < song.channel_count && ch < XMP_MAX_CHANNELS; ++ch) {
                const engine::ChannelState& cs = channels[ch];
                const auto& ci = info.channel_info[ch];
                if (!cs.voice_active || ci.period == 0 || (ci.volume == 0 && cs.volume == 0)) continue;
                if (ch >= last_row.size()) continue;
                const soundsinth::model::Effect fx = last_row[ch].effect.type;
                const bool porta = fx == soundsinth::model::Effect::PortaUp || fx == soundsinth::model::Effect::PortaDown;
                const bool vibrato = fx == soundsinth::model::Effect::Vibrato && ps.tick_in_row != 0;
                if (!porta && !vibrato) continue;
                ++(porta ? porta_ticks : vibrato_ticks);
                const double ours = double(int32_t(cs.period) + cs.pitch_offset);
                const double theirs = double(ci.period) / 4096.0;
                if (ours - theirs > 2.5 || theirs - ours > 2.5) {
                    if (++mismatches <= 5) std::printf("  тик %d канал %u: наш %.0f libxmp %.2f\n", t, ch, ours, theirs);
                }
            }
            if (!engine::sequencer_tick(song, mem.psram, ps, diag_row_callback, &ctx)) break;
            engine::apply_continuous_effects(ps, channels.data(), song.channel_count, song.quirks, song.frequency_model,
                                             engine::kQ8One, ps.tick_in_row != 0);
            if (xmp_play_frame(xmp) != 0) break;
            xmp_get_frame_info(xmp, &info);
            if (info.loop_count > 0) break;
        }
        std::printf("  %s: канало-тиков с порто %u, с вибрато %u, расхождений больше 2.5: %u\n", path, porta_ticks,
                    vibrato_ticks, mismatches);
        CHECK(porta_ticks > 0);
        CHECK_EQ(mismatches, 0u);
        xmp_end_player(xmp);
        xmp_release_module(xmp);
        xmp_free_context(xmp);
        memory::track_memory_destroy(mem);
    }
}

} // namespace

// Наша вычисленная длительность против libxmp как независимого плеера.
// Эта длительность уходит в телеметрию file_info и показывается на
// экране, по ней же оркестратор решает, что трек доиграл. Пользователь
// сообщал, что время "не совпадает"; без независимого эталона это не
// проверить.
//
// Допуск 0.1%: у нас тик - целое число отсчётов, у libxmp - нет. Файлы из
// music/ (Pattern Delay, Pattern Loop) в git не лежат и без них
// пропускаются.
void test_song_duration_matches_libxmp() {
    std::printf("test_song_duration_matches_libxmp\n");
    static const char* kFiles[] = {
        "SD/test_music/mod/star_wars.mod", "SD/test_music/mod/legend_of_zelda.mod", "SD/test_music/mod/megaman.mod",
        "SD/test_music/xm/final_fantasy.xm", "SD/test_music/xm/001.xm",
        "SD/test_music/s3m/2nd_reality.s3m", "SD/test_music/s3m/starwars.s3m",
        "SD/test_music/it/00009.it", "SD/test_music/it/ivi-lite__v61.it", "SD/test_music/it/life_d__v40.it",
        "music/src/s3m/mario1_1.s3m", "music/src/mod/00_00_00.mod", "music/src/it/dg_pcorn__v50.it",
        "music/src/s3m/41096877.s3m",
    };
    for (const char* path : kFiles) {
        const std::vector<uint8_t> bytes = read_file(path);
        if (bytes.empty()) { std::printf("  ПРОПУСК (нет файла): %s\n", path); continue; }

        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        soundsinth::model::Song song;
        formats::MemoryByteSource mbs(bytes.data(), static_cast<uint32_t>(bytes.size()));
        player::load::SessionLoadResult load;
        if (!player::load::run_session_load(mbs.as_byte_source(), mem, song, load)) {
            std::printf("  ПРОПУСК (не загрузился): %s\n", path);
            memory::track_memory_destroy(mem);
            continue;
        }
        const uint32_t our_ms =
            static_cast<uint32_t>((static_cast<uint64_t>(engine::compute_song_total_frames(song, mem.psram, memory::scratch_take(mem.scratch, memory::Scratch::DurationPass, memory::kDurationPassBytes), memory::kDurationPassBytes)) * 1000u) /
                                   engine::kSampleRateHz);
        memory::scratch_leave(mem.scratch, memory::Scratch::DurationPass);

        xmp_context xmp = xmp_create_context();
        int their_ms = -1;
        if (xmp != nullptr &&
            xmp_load_module_from_memory(xmp, bytes.data(), static_cast<long>(bytes.size())) == 0) {
            if (xmp_start_player(xmp, static_cast<int>(engine::kSampleRateHz), 0) == 0) {
                struct xmp_frame_info info {};
                xmp_get_frame_info(xmp, &info);
                their_ms = info.total_time;
                xmp_end_player(xmp);
            }
            xmp_release_module(xmp);
        }
        if (xmp != nullptr) xmp_free_context(xmp);

        if (their_ms <= 0) {
            std::printf("  %s: наши %u.%03u c, libxmp не сказал\n", path, our_ms / 1000u, our_ms % 1000u);
        } else {
            const uint32_t t = static_cast<uint32_t>(their_ms);
            const uint32_t lo = t < our_ms ? t : our_ms;
            const uint32_t hi = t < our_ms ? our_ms : t;
            std::printf("  %s: наши %u.%03u c | libxmp %u.%03u c\n", path, our_ms / 1000u, our_ms % 1000u,
                        t / 1000u, t % 1000u);
            CHECK(static_cast<uint64_t>(hi - lo) * 1000u <= static_cast<uint64_t>(hi));
        }
        memory::track_memory_destroy(mem);
    }
}
// Ход секвенсора по тикам против libxmp: (позиция order, строка) на каждом
// тике до первого оборота песни у libxmp. Закрепляет Pattern Delay (SEx,
// mario1_1.s3m), Pattern Loop (E6x 00_00_00.mod, SBx dg_pcorn__v50.it) и
// переходы Bxx/Dxx.
void test_position_row_by_tick_matches_libxmp() {
    std::printf("test_sequencer_libxmp_position_row_by_tick\n");
    static const char* kFiles[] = {
        "SD/test_music/mod/star_wars.mod", "SD/test_music/xm/final_fantasy.xm", "SD/test_music/s3m/2nd_reality.s3m",
        "SD/test_music/it/00009.it", "music/src/s3m/mario1_1.s3m", "music/src/mod/00_00_00.mod",
        "music/src/it/dg_pcorn__v50.it",
    };
    for (const char* path : kFiles) {
        const std::vector<uint8_t> bytes = read_file(path);
        if (bytes.empty()) { std::printf("  ПРОПУСК (нет файла): %s\n", path); continue; }
        memory::TrackMemory mem;
        memory::track_memory_create(mem);
        soundsinth::model::Song song;
        formats::MemoryByteSource mbs(bytes.data(), static_cast<uint32_t>(bytes.size()));
        player::load::SessionLoadResult load;
        CHECK(player::load::run_session_load(mbs.as_byte_source(), mem, song, load));
        xmp_context xmp = xmp_create_context();
        CHECK_EQ(xmp_load_module_from_memory(xmp, bytes.data(), static_cast<long>(bytes.size())), 0);
        CHECK_EQ(xmp_start_player(xmp, static_cast<int>(engine::kSampleRateHz), 0), 0);
        engine::PlayState ps;
        CHECK(engine::sequencer_init(song, mem.psram, ps, nullptr, nullptr));
        CHECK_EQ(xmp_play_frame(xmp), 0); // строка 0 у libxmp применяется в первом кадре
        struct xmp_frame_info info {};
        xmp_get_frame_info(xmp, &info);
        uint32_t ticks = 0, mismatches = 0;
        for (; ticks < 60000; ++ticks) {
            if (info.pos != ps.order_pos || info.row != ps.row) {
                if (mismatches == 0) {
                    std::printf("  %s: тик %u наш (%u,%u) libxmp (%d,%d)\n", path, ticks, ps.order_pos, ps.row, info.pos,
                                info.row);
                }
                ++mismatches;
            }
            if (!engine::sequencer_tick(song, mem.psram, ps, nullptr, nullptr)) break;
            if (xmp_play_frame(xmp) != 0) break;
            xmp_get_frame_info(xmp, &info);
            if (info.loop_count > 0) break;
        }
        std::printf("  %s: тиков %u, расхождений %u\n", path, ticks, mismatches);
        CHECK(ticks > 100u);
        CHECK_EQ(mismatches, 0u);
        xmp_end_player(xmp);
        xmp_release_module(xmp);
        xmp_free_context(xmp);
        memory::track_memory_destroy(mem);
    }
}

void run_sequencer_libxmp_tests() {
    test_position_row_by_tick_matches_libxmp();
    test_song_duration_matches_libxmp();
    test_setvolume_volumeslide_matches_libxmp();
    test_period_matches_libxmp();
}
