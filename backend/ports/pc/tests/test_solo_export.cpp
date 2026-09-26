// Поканальный экспорт (TrackerEngine::set_solo_channel): у трекерных
// форматов сведение линейно, поэтому сумма поканальных рендеров на шине
// int32 (до MixBus) обязана совпасть с полным рендером побитово. Не совпадёт,
// если solo теряет или дублирует голоса или меняет их логику: голос чужого
// канала обязан двигаться, кончаться и занимать слоты как в полном рендере.

#include "testing.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include "core/engine/tracker_engine.h"
#include "core/formats/it.h"
#include "core/formats/memory_byte_source.h"
#include "core/memory/track_memory.h"

namespace {

using namespace soundsinth;

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Кусок длиннее discard и не кратен ему: заодно проверяется нарезка вызова в
// render_add.
constexpr uint32_t kChunk = 1000;

void render(engine::TrackerEngine& e, std::vector<int32_t>& l, std::vector<int32_t>& r, uint32_t n) {
    l.assign(n, 0);
    r.assign(n, 0);
    mixbus::SoundSource* s = e.as_sound_source();
    for (uint32_t d = 0; d < n; d += kChunk) {
        const uint32_t k = (n - d) < kChunk ? (n - d) : kChunk;
        s->render_add(s->self, l.data() + d, r.data() + d, k);
    }
}

void check_solo_sum(const char* path, uint32_t frames) {
    const std::vector<uint8_t> bytes = read_whole_file(path);
    if (bytes.empty()) {
        std::printf("  ПРОПУСК (файл не найден): %s\n", path);
        return;
    }

    memory::TrackMemory mem;
    memory::track_memory_create(mem);
    soundsinth::model::Song song;
    formats::MemoryByteSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const char* err = nullptr;
    if (!formats::it::load(src.as_byte_source(), mem, song, &err, false)) {
        CHECK(false);
        memory::track_memory_destroy(mem);
        return;
    }

    std::vector<int32_t> full_l, full_r, l, r;
    {
        engine::TrackerEngine e(song, mem);
        render(e, full_l, full_r, frames);
    }

    constexpr uint32_t kDiscardFrames = SOUNDSINTH_AUDIO_BUFFER_FRAMES;
    std::vector<int32_t> discard(kDiscardFrames * 2);
    std::vector<int64_t> sum_l(frames, 0), sum_r(frames, 0);
    uint32_t audible_channels = 0;
    for (uint8_t ch = 0; ch < song.channel_count && ch < SOUNDSINTH_MAX_VOICES; ++ch) {
        engine::TrackerEngine e(song, mem);
        e.set_solo_channel(ch, discard.data(), kDiscardFrames);
        render(e, l, r, frames);
        bool audible = false;
        for (uint32_t i = 0; i < frames; ++i) {
            sum_l[i] += l[i];
            sum_r[i] += r[i];
            if (l[i] != 0 || r[i] != 0) audible = true;
        }
        if (audible) ++audible_channels;
    }

    uint32_t diff = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        if (sum_l[i] != full_l[i] || sum_r[i] != full_r[i]) ++diff;
    }
    // Сверка не пустая: поодиночке звучит больше одного канала.
    CHECK(audible_channels > 1);
    CHECK_EQ(diff, 0u);
    std::printf("  %s: каналов %u, звучащих %u, кадров %u, отличий суммы %u\n", path, song.channel_count,
                audible_channels, frames, diff);

    memory::track_memory_destroy(mem);
}

// Чужой голос кончается или уходит в фон в первые 20 секунд: у 00009.it на
// 19-й, у filt_ace-light.it на 4-й.
void test_solo_channels_sum_to_full_mix() {
    std::printf("test_solo_channels_sum_to_full_mix\n");
    check_solo_sum("SD/test_music/it/00009.it", 44100 * 20);
    check_solo_sum("music/src/it/smoke/filt_ace-light.it", 44100 * 20);
}

} // namespace

void run_solo_export_tests() {
    test_solo_channels_sum_to_full_mix();
}
