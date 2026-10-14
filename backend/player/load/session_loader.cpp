// SPDX-License-Identifier: MIT
#include "player/load/session_loader.h"

#include <cstring>
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
#include "player/load/sample_prefetch.h"

namespace player::load {

namespace {

using LoadFn       = bool (*)(soundsinth::formats::ByteSource, soundsinth::memory::TrackMemory&, soundsinth::model::Song&, const char**, bool);
using LoadSampleFn = bool (*)(soundsinth::formats::ByteSource, soundsinth::memory::TrackMemory&, const soundsinth::model::Song&, uint16_t, const char**);

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

// Крюк обслуживания долгих проходов: ставит оркестратор, у ПК пусто.
soundsinth::engine::ServiceFn s_service = nullptr;
void* s_service_user                    = nullptr;

// Переходник под общую сигнатуру таблицы. У MIDI своё: размер файла
// (берётся из источника) и банк.
bool midi_load(soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, soundsinth::model::Song& out, const char** error_out,
               bool metadata_only) {
    // Сигнатура проверяется до банка: иначе на плате без банка каждый
    // .it и .mod получал бы первой строкой отказа "instrument bank not flashed".
    uint8_t head[4] = {};
    if (!src.seek(src.self, 0) || src.read(src.self, head, sizeof(head)) != sizeof(head) || !soundsinth::formats::midi::sniff(head, sizeof(head))) {
        if (error_out) *error_out = "no MThd signature";
        return false;
    }
    if (s_bank == nullptr || !s_bank->valid()) {
        if (error_out) *error_out = "instrument bank not flashed, nothing to play .mid with";
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
        if (error_out) *error_out = "instrument bank not flashed";
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
static_assert(std::size(kLoaders) == kLoaderCount, "kLoaderCount is the number of loaders in kLoaders");

// Загрузчик формата f - kLoaders[f - 1].
constexpr bool loaders_in_format_order() {
    for (uint8_t i = 0; i < kLoaderCount; ++i) {
        if (static_cast<uint8_t>(kLoaders[i].format) != i + 1) return false;
    }
    return true;
}
static_assert(loaders_in_format_order(), "kLoaders are in TrackFormat order");

// Карта позиций - блоком страниц PSRAM после заморозки, до первого
// сэмпла: страницы вернёт смена трека. false - места нет.
bool take_use_map(soundsinth::memory::PsramStore& psram, const soundsinth::model::Song& song, UseMap& out) {
    out                  = use_map_layout(song.sample_count, song.order_count);
    const uint32_t bytes = use_map_bytes(out);
    if (bytes == 0) return false;
    const uint32_t pages                       = (bytes + soundsinth::memory::kPsramPageBytes - 1u) / soundsinth::memory::kPsramPageBytes;
    const soundsinth::memory::PsramBlock block = soundsinth::memory::psram_alloc(psram.alloc, static_cast<uint16_t>(pages));
    if (!block.valid()) {
        out = UseMap{};
        return false;
    }
    out.words = reinterpret_cast<uint32_t*>(psram.base + static_cast<uint32_t>(block.first) * soundsinth::memory::kPsramPageBytes);
    std::memset(out.words, 0, bytes);
    return true;
}

} // namespace

const char* session_loader_name(uint8_t i) {
    return i < kLoaderCount ? kLoaders[i].name : "?";
}

void session_loader_set_bank(const soundsinth::bank::Bank* bank) {
    s_bank = bank;
}

void session_loader_set_service(soundsinth::engine::ServiceFn fn, void* user) {
    s_service      = fn;
    s_service_user = user;
}

bool run_session_load(soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, soundsinth::model::Song& out_song, SessionLoadResult& result,
                      bool metadata_only, const PlanArrays& plan, UseMap* use_map_out) {
    result = SessionLoadResult{};
    if (use_map_out != nullptr) *use_map_out = UseMap{};
    for (uint8_t i = 0; i < kLoaderCount; ++i) {
        const Loader& loader = kLoaders[i];
        soundsinth::memory::track_memory_reset_for_new_track(mem);
        // Song заново: на плате один g_song на все треки, загрузчик пишет
        // только свои поля (иначе XM после IT наследует ChnPan, ChnVol,
        // сведение ModPlug). На месте: копия Song (248 байт на плате) на стеке Core1 не
        // нужна; прежний объект не разрушается - деструктор тривиален.
        static_assert(std::is_trivially_destructible_v<soundsinth::model::Song>, "Song is constructed over the previous one");
        new (&out_song) soundsinth::model::Song();
        if (!src.seek(src.self, 0)) {
            result.attempt_errors[i] = "could not seek the file to the start";
            continue;
        }
        if (loader.load(src, mem, out_song, &result.attempt_errors[i], metadata_only)) {
            result.attempt_errors[i] = nullptr;
            result.format            = loader.format;
            uint8_t* walk = soundsinth::memory::scratch_take(mem.scratch, soundsinth::memory::Scratch::DurationPass, soundsinth::memory::kDurationPassBytes);
            PlanCollector collector;
            // Сэмплов больше ёмкости массивов - плана нет: массивы last_use и
            // first_use идут по номеру сэмпла, и запись ушла бы за буфер
            // загрузчика (за ним PsramStore). Такой трек грузится целиком.
            const bool collect = plan.indices != nullptr && plan.capacity != 0 && out_song.sample_count <= plan.capacity;
            if (collect) {
                plan_collect_begin(collector, out_song, plan.indices, plan.capacity, plan.last_use, plan.first_use);
                if (use_map_out != nullptr && take_use_map(mem.psram, out_song, *use_map_out)) collector.use_map = use_map_out;
            }
            // Не влезло - длительность неизвестна (0): трек играет, конец
            // определяется по song_ended.
            result.total_frames =
                walk != nullptr
                    ? soundsinth::engine::compute_song_total_frames(out_song, mem.psram, walk, soundsinth::memory::kDurationPassBytes, &result.duration,
                                                                    collect ? &plan_collect_row : nullptr, &collector, s_service, s_service_user)
                    : 0u;
            if (walk != nullptr) {
                soundsinth::memory::scratch_leave(mem.scratch, soundsinth::memory::Scratch::DurationPass);
            }
            // Проход неполон - план неполон: сэмплы непройденного хвоста в
            // нём не отмечены, и вытеснение сочло бы их отыгравшими.
            result.plan_usable =
                collect && walk != nullptr &&
                (result.duration.stop == soundsinth::engine::DurationStop::Repeat || result.duration.stop == soundsinth::engine::DurationStop::SongEnd);
            result.plan_count = result.plan_usable ? collector.count : 0u;
            if (!result.plan_usable && use_map_out != nullptr) *use_map_out = UseMap{};
            return true;
        }
    }

    result.error = "unknown file format (not MIDI/IT/XM/S3M/MOD)";
    for (const char* e : result.attempt_errors) {
        if (e != nullptr) result.error = e;
    }
    return false;
}

// Без повтора, намеренно: повтор читает сэмпл заново, а у Wild Commander
// прыжок назад - перемотка файла в начало и посекторное чтение вперёд. На файле
// в 6 МБ это десятки секунд на каждый прыжок, и догрузка встаёт при живом
// звуке. Повтор после вытеснения (фоновая догрузка) дешёв: место
// проверяется до чтения, не влезающий сэмпл отказывает, не читая.
bool load_track_sample(TrackFormat format, soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem, const soundsinth::model::Song& song,
                       uint16_t sample_index, const char** reason_out) {
    const uint8_t f = static_cast<uint8_t>(format);
    if (f == 0 || f > kLoaderCount) {
        if (reason_out) *reason_out = "unknown track format";
        return false;
    }
    return kLoaders[f - 1].load_sample(src, mem, song, sample_index, reason_out);
}

} // namespace player::load
