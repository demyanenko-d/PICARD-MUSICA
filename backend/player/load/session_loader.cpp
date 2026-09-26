#include "player/load/session_loader.h"

#include <iterator>
#include <new>
#include <type_traits>

#include "core/bank/bank_reader.h"
#include "core/engine/song_duration.h"
#include "core/formats/it.h"
#include "core/formats/midi.h"
#include "core/formats/mod.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"

namespace player::load {

namespace {

using LoadFn = bool (*)(soundsinth::formats::ByteSource, soundsinth::memory::TrackMemory&, soundsinth::model::Song&, const char**, bool);
using LoadSampleFn = bool (*)(soundsinth::formats::ByteSource, soundsinth::memory::TrackMemory&, const soundsinth::model::Song&, uint16_t,
                               const char**);

struct Loader {
    TrackFormat format;
    const char* name;
    LoadFn load;
    LoadSampleFn load_sample;
};

// Банк для .mid, ставится раз при старте (на плате с карты или из флеша,
// на ПК ключом --bank). Указатель: Bank - вид на блоб, который живёт
// снаружи.
const soundsinth::bank::Bank* s_bank = nullptr;

// Переходник под общую сигнатуру таблицы. У MIDI своё: размер файла
// (берётся из источника) и банк.
bool midi_load(soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, soundsinth::model::Song& out, const char** error_out,
                bool metadata_only) {
    // Сигнатура проверяется до банка: иначе на плате без банка каждый
    // .it и .mod получал бы первой строкой отказа "нет банка".
    uint8_t head[4] = {};
    if (!src.seek(src.self, 0) || src.read(src.self, head, sizeof(head)) != sizeof(head) ||
        !soundsinth::formats::midi::sniff(head, sizeof(head))) {
        if (error_out) *error_out = "нет сигнатуры MThd";
        return false;
    }
    if (s_bank == nullptr || !s_bank->valid()) {
        if (error_out) *error_out = "банк инструментов не прошит, .mid играть нечем";
        return false;
    }
    const uint32_t len = src.size ? src.size(src.self) : 0;
    return soundsinth::formats::midi::load(src, len, mem, *s_bank, out, error_out, metadata_only);
}

// Догрузка сэмпла .mid из банка, soundsinth::formats::ByteSource не нужен, хост не
// участвует.
//
// Плата грузит трек в две фазы, метаданные отдельно от PCM, и сэмплы .mid
// попадают в PSRAM только через этот вызов.
bool midi_load_sample(soundsinth::formats::ByteSource, soundsinth::memory::TrackMemory& mem, const soundsinth::model::Song& song, uint16_t index,
                       const char** error_out) {
    if (s_bank == nullptr || !s_bank->valid()) {
        if (error_out) *error_out = "банк инструментов не прошит";
        return false;
    }
    return soundsinth::formats::midi::load_sample_from_bank(mem, *s_bank, song, index, error_out);
}

// Порядок - от самой специфичной сигнатуры к самой широкой (MOD в конце).
constexpr Loader kLoaders[] = {
    {TrackFormat::Midi, "MIDI", &midi_load, &midi_load_sample},
    {TrackFormat::It, "IT", &soundsinth::formats::it::load, &soundsinth::formats::it::load_sample_pcm},
    {TrackFormat::Xm, "XM", &soundsinth::formats::xm::load, &soundsinth::formats::xm::load_sample_pcm},
    {TrackFormat::S3m, "S3M", &soundsinth::formats::s3m::load, &soundsinth::formats::s3m::load_sample_pcm},
    {TrackFormat::Mod, "MOD", &soundsinth::formats::mod::load, &soundsinth::formats::mod::load_sample_pcm},
};
static_assert(std::size(kLoaders) == kLoaderCount, "kLoaderCount - число загрузчиков в kLoaders");

// Загрузчик формата f - kLoaders[f - 1].
constexpr bool loaders_in_format_order() {
    for (uint8_t i = 0; i < kLoaderCount; ++i) {
        if (static_cast<uint8_t>(kLoaders[i].format) != i + 1) return false;
    }
    return true;
}
static_assert(loaders_in_format_order(), "kLoaders идут в порядке TrackFormat");

} // namespace

const char* session_loader_name(uint8_t i) { return i < kLoaderCount ? kLoaders[i].name : "?"; }

void session_loader_set_bank(const soundsinth::bank::Bank* bank) { s_bank = bank; }

bool run_session_load(soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, soundsinth::model::Song& out_song,
                       SessionLoadResult& result, bool metadata_only) {
    result = SessionLoadResult{};
    for (uint8_t i = 0; i < kLoaderCount; ++i) {
        const Loader& loader = kLoaders[i];
        soundsinth::memory::track_memory_reset_for_new_track(mem);
        // Song заново: на плате один g_song на все треки, загрузчик пишет
        // только свои поля (иначе XM после IT наследует ChnPan, ChnVol,
        // сведение ModPlug). На месте: копия Song (248 байт на плате) на стеке Core1 не
        // нужна; прежний объект не разрушается - деструктор тривиален.
        static_assert(std::is_trivially_destructible_v<soundsinth::model::Song>, "Song создаётся поверх прежнего");
        new (&out_song) soundsinth::model::Song();
        if (!src.seek(src.self, 0)) {
            result.attempt_errors[i] = "не удалось перемотать файл в начало";
            continue;
        }
        if (loader.load(src, mem, out_song, &result.attempt_errors[i], metadata_only)) {
            result.attempt_errors[i] = nullptr;
            result.format = loader.format;
            uint8_t* walk =
                soundsinth::memory::scratch_take(mem.scratch, soundsinth::memory::Scratch::DurationPass, soundsinth::memory::kDurationPassBytes);
            // Не влезло - длительность неизвестна (0): трек играет, конец
            // определяется по song_ended.
            result.total_frames = walk != nullptr ? soundsinth::engine::compute_song_total_frames(out_song, mem.psram, walk,
                                                                                      soundsinth::memory::kDurationPassBytes,
                                                                                      &result.duration)
                                                  : 0u;
            if (walk != nullptr) soundsinth::memory::scratch_leave(mem.scratch, soundsinth::memory::Scratch::DurationPass);
            return true;
        }
    }

    result.error = "неизвестный формат файла (не MIDI/IT/XM/S3M/MOD)";
    for (const char* e : result.attempt_errors) {
        if (e != nullptr) result.error = e;
    }
    return false;
}

// Без повтора, намеренно: повтор читает сэмпл заново, а у WC прыжок
// назад - перемотка файла в начало и посекторное чтение вперёд. На файле
// в 6 МБ это десятки секунд на каждый прыжок, и догрузка встаёт при живом
// звуке. Повтор после вытеснения (фоновая догрузка) дешёв: место
// проверяется до чтения, не влезающий сэмпл отказывает, не читая.
bool load_track_sample(TrackFormat format, soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem,
                        const soundsinth::model::Song& song, uint16_t sample_index, const char** reason_out) {
    const uint8_t f = static_cast<uint8_t>(format);
    if (f == 0 || f > kLoaderCount) {
        if (reason_out) *reason_out = "формат трека неизвестен";
        return false;
    }
    return kLoaders[f - 1].load_sample(src, mem, song, sample_index, reason_out);
}

} // namespace player::load
