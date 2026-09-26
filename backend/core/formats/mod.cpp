#include "core/formats/mod.h"

#include <algorithm>
#include <cstring>

#include "core/model/amiga_period.h"
#include "core/formats/load_stats.h"
#include "core/codec/pack_file_pcm.h"
#include "core/formats/binary_reader.h"
#include "core/codec/pattern_packer.h"

namespace soundsinth::formats::mod {

namespace {

using formats::BinaryReader;
using soundsinth::model::Effect;
using soundsinth::model::EffectCommand;
using soundsinth::model::Instrument;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::SampleDescriptor;
using soundsinth::model::SampleEncoding;
using soundsinth::model::SlideRate;
using soundsinth::model::Song;
using soundsinth::model::kGlobalVolumeMax;
using soundsinth::model::kVolumeMax;

constexpr uint32_t kTitleBytes = 20;
constexpr uint32_t kSampleCount = 31;
constexpr uint32_t kSampleHeaderBytes = 30;
constexpr uint32_t kSampleNameBytes = 22;    // в начале заголовка сэмпла
constexpr uint32_t kOrderEntries = 128;
constexpr uint32_t kCellBytes = 4;
constexpr uint32_t kOrderTableOffset = 950;
constexpr uint32_t kSignatureOffset = 1080;
constexpr uint32_t kPatternDataOffset = 1084;
constexpr uint32_t kRowsPerPattern = 64;
constexpr uint8_t kMaxChannels = 64; // ширина маски PatternPacker, защита от мусора в xxCH
static_assert(kMaxChannels <= soundsinth::model::kMaxPatternChannels, "маска строки упаковщика - 64 канала");

// Старый 15-сэмпловый формат без сигнатуры (SoundTracker, D.O.C.
// Soundtracker и совместимые) - старше 4-байтной
// сигнатуры на 1080, поэтому узнаётся по правдоподобности заголовка
// (load()). Раскладка своя: 20 (title) + 15*30 (сэмплы) + 1 (song_length)
// + 1 (restart) + 128 (order) = 600, дальше данные паттернов; всегда 4
// канала.
constexpr uint32_t kOldSampleCount = 15;
constexpr uint32_t kOldOrderTableOffset = kTitleBytes + kOldSampleCount * kSampleHeaderBytes; // 470
constexpr uint32_t kOldPatternDataOffset = kOldOrderTableOffset + 2 + kOrderEntries;          // 600
static_assert(kTitleBytes + kSampleCount * kSampleHeaderBytes == kOrderTableOffset, "раскладка 31 сэмпла");
static_assert(kOrderTableOffset + 2 + kOrderEntries + 4 == kPatternDataOffset, "order и сигнатура перед паттернами");

const char* const kUnknownSignature = "неизвестная или неподдерживаемая сигнатура MOD";

uint16_t read_u16_be(BinaryReader& r) {
    const uint8_t hi = r.u8();
    const uint8_t lo = r.u8();
    return static_cast<uint16_t>((hi << 8) | lo);
}

struct ModLoop {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t header_end = 0; // конец до прижатия к длине
    bool enabled = false;
};

// Петля 31-сэмплового MOD в порядке OpenMPT: поправки по сырым словам, конец
// прижимается к длине последним. Сэмпл в одно слово у ProTracker пустой.
ModLoop mod_sample_loop(uint32_t length, uint16_t start_words, uint16_t size_words, bool four_channels) {
    ModLoop loop;
    if (length <= 2) return loop;
    uint32_t start = static_cast<uint32_t>(start_words) * 2u;
    const uint32_t size = static_cast<uint32_t>(size_words) * 2u;
    // Начало в байтах (так пишут трекеры, совместимые с SoundTracker): в
    // словах петля за концом сэмпла, в байтах влезает.
    if (size > 2 && start + size > length && start / 2 + size <= length) start /= 2;
    uint32_t end = start + size;
    if (start >= length) start = length - 1;
    if (start > end || end < 4 || end - start < 4) return loop;
    // Петля до 8 отсчётов в начале длинного сэмпла у 4-канального файла -
    // сбой записи, сэмпл играет целиком. У файлов с большим числом каналов
    // такая петля звучит.
    if (end <= 8 && start == 0 && length > end && four_channels) return loop;
    loop.header_end = end;
    if (end > length) end = length;
    if (start >= end) return loop;
    loop.start = start;
    loop.end = end;
    loop.enabled = true;
    return loop;
}

// Петля 15-сэмплового SoundTracker: начало всегда в байтах. Петля до 8
// отсчётов в начале длинного сэмпла выключена, как у 4-канального MOD.
ModLoop old_sample_loop(uint32_t length, uint16_t start_bytes, uint16_t size_words) {
    ModLoop loop;
    if (length <= 2 || size_words <= 1) return loop;
    const uint32_t size = static_cast<uint32_t>(size_words) * 2u;
    if (start_bytes == 0 && size <= 8 && length > size) return loop;
    const uint32_t end = std::min<uint32_t>(start_bytes + size, length);
    if (start_bytes >= end) return loop;
    loop.start = start_bytes;
    loop.end = end;
    loop.enabled = true;
    return loop;
}

// Что значит сигнатура, как CheckMODMagic у OpenMPT.
struct SignatureInfo {
    uint8_t channels = 0;       // 0 - сигнатура не узнана
    bool vblank_timing = false; // Fxx всегда скорость
    bool protracker = false;    // кандидат в режим ProTracker (дальше - по нотам и петлям)
};

// Кроме M.K. приняты варианты с той же 31-сэмпловой раскладкой на 1084
// байта (PATT/NSMS/LARD/WARD - единичные треки, OCTA/OKTA - Oktalyzer).
// Startrekker (FLT4/FLT8/EXO4/EXO8) не принят: у FLT8 своя раскладка
// сэмплов и необязательные AM/FM-синт-инструменты без PCM. Старый
// 15-сэмпловый формат сигнатуры не имеет и разбирается отдельно.
SignatureInfo parse_signature(const char sig[4]) {
    struct Entry {
        const char* sig;
        uint8_t channels;
        bool vblank_timing;
        bool protracker;
    };
    static constexpr Entry kTable[] = {
        {"M.K.", 4, false, true},  {"M!K!", 4, false, true},  {"M&K!", 4, true, false}, {"N.T.", 4, true, false},
        {"FEST", 4, true, false},  {"PATT", 4, false, true},  {"NSMS", 4, false, false}, {"LARD", 4, false, false},
        {"CD61", 6, false, false}, {"CD81", 8, false, false}, {"OCTA", 8, false, false}, {"OKTA", 8, false, false},
        {"WARD", 8, false, false}, {"FA04", 4, false, false}, {"FA06", 6, false, false}, {"FA08", 8, false, false},
        {"TDZ1", 1, false, false}, {"TDZ2", 2, false, false}, {"TDZ3", 3, false, false}, {"TDZ4", 4, false, false},
    };
    SignatureInfo info;
    for (const auto& e : kTable) {
        if (std::memcmp(sig, e.sig, 4) == 0) {
            info.channels = e.channels;
            info.vblank_timing = e.vblank_timing;
            info.protracker = e.protracker;
            return info;
        }
    }
    // xxCH / xxCN (2 цифры + "CH"/"CN") / xCHN (1 цифра + "CHN")
    if ((sig[2] == 'C' && (sig[3] == 'H' || sig[3] == 'N')) && sig[0] >= '0' && sig[0] <= '9' && sig[1] >= '0' &&
        sig[1] <= '9') {
        info.channels = static_cast<uint8_t>((sig[0] - '0') * 10 + (sig[1] - '0'));
    } else if (sig[1] == 'C' && sig[2] == 'H' && sig[3] == 'N' && sig[0] >= '0' && sig[0] <= '9') {
        info.channels = static_cast<uint8_t>(sig[0] - '0');
    }
    return info;
}

// Раскладка файла: 31 сэмпл с сигнатурой или старый формат на 15.
struct ModLayout {
    uint32_t sample_count;
    uint32_t order_table_offset;
    uint32_t pattern_data_offset;
    bool old_format;
};
constexpr ModLayout kLayout31{kSampleCount, kOrderTableOffset, kPatternDataOffset, false};
constexpr ModLayout kLayout15{kOldSampleCount, kOldOrderTableOffset, kOldPatternDataOffset, true};

// Признаки режима ProTracker из ячеек паттернов, как у OpenMPT: ноты в трёх
// октавах Amiga и то, как файл пользуется 8xx/E8x.
struct ModPatternStats {
    bool only_amiga_notes = true;
    uint8_t max_panning = 0;
    bool left_panning = false;     // 8xx меньше 0x80
    bool extended_panning = false; // 8xx больше 0x8F, кроме A4 (surround в 7-битной шкале)

    void observe(uint16_t period, uint8_t effect_nibble, uint8_t param) {
        if (period != 0 && (period < soundsinth::model::kAmigaPeriodMin || period > soundsinth::model::kAmigaPeriodMax)) {
            only_amiga_notes = false;
        }
        if (effect_nibble == 0x8) {
            // 880..88F в стиль панорамы не засчитываются: у 7-битной бывает
            // перелёт (LOOKATME.MOD - 88A), как у OpenMPT.
            if (param > max_panning) max_panning = param;
            if (param < 0x80) {
                left_panning = true;
            } else if (param > 0x8f && param != 0xa4) {
                extended_panning = true;
            }
        } else if (effect_nibble == 0xe && (param & 0xf0u) == 0x80) {
            const uint8_t pan4 = static_cast<uint8_t>((param & 0x0fu) << 4);
            if (pan4 > max_panning) max_panning = pan4;
        }
    }
};

// Квирки режима ProTracker и 7-битной панорамы (как у OpenMPT): режим -
// M.K., M!K!, PATT, ноты в трёх октавах Amiga, без петель длиной 0.
soundsinth::model::QuirkFlags mode_quirks(const ModPatternStats& stats, bool protracker_signature, bool loop_length_zero) {
    constexpr uint8_t kPanningThreshold = 0x30; // порог OpenMPT
    soundsinth::model::QuirkFlags quirks = 0;
    if (protracker_signature && stats.only_amiga_notes && !loop_length_zero) {
        // Новый темп Fxx >= 0x20 - со второго тика строки только в режиме
        // ProTracker; остальные MOD берут его с тика 0.
        quirks |= soundsinth::model::kQuirkGlissandoPtMode | soundsinth::model::kQuirkAmigaLimits | soundsinth::model::kQuirkModTempoOnSecondTick;
        if (stats.max_panning < kPanningThreshold) quirks |= soundsinth::model::kQuirkModIgnorePanning;
    }
    if (stats.left_panning && !stats.extended_panning && stats.max_panning >= kPanningThreshold) {
        quirks |= soundsinth::model::kQuirkMod7BitPanning;
    }
    return quirks;
}

// Защита от ложного срабатывания старого 15-сэмплового формата на файлах
// с настоящей, но не поддержанной сигнатурой (Startrekker и т.п.): если
// байты на месте сигнатуры читаются как ASCII-тег, это тег. У старого
// формата там середина таблицы сэмплов, байты непечатные.
bool looks_like_ascii_signature(const char sig[4]) {
    for (uint32_t i = 0; i < 4; ++i) {
        const auto c = static_cast<uint8_t>(sig[i]);
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

// Эффект-нибл MOD -> EffectCommand.
void decode_effect(uint8_t effect_nibble, uint8_t param, bool vblank_timing, EffectCommand& out) {
    switch (effect_nibble) {
        case 0x0:
            // 000 - эффекта нет (как ConvertModCommand в OpenMPT): иначе каждая пустая
            // ячейка MOD активна - полная маска строки и поиск в словаре.
            if (param != 0) {
                out.type = Effect::Arpeggio;
                out.param = param;
            }
            break;
        case 0x1: out.type = Effect::PortaUp; out.param = param; break;
        case 0x2: out.type = Effect::PortaDown; out.param = param; break;
        case 0x3: out.type = Effect::TonePorta; out.param = param; break;
        case 0x4: out.type = Effect::Vibrato; out.param = param; break;
        case 0x5: out.type = Effect::TonePortaVolSlide; out.param = param; break;
        case 0x6: out.type = Effect::VibratoVolSlide; out.param = param; break;
        case 0x7: out.type = Effect::Tremolo; out.param = param; break;
        case 0x8: out.type = Effect::SetPanning; out.param = param; break;
        case 0x9: out.type = Effect::SampleOffset; out.param = param; break;
        case 0xA: out.type = Effect::VolumeSlide; out.param = param; break;
        case 0xB: out.type = Effect::PositionJump; out.param = param; break;
        case 0xC: out.type = Effect::SetVolume; out.param = (param > kVolumeMax) ? kVolumeMax : param; break;
        case 0xD: {
            // Pattern Break: параметр в BCD (десятки в старшем
            // нибле, единицы в младшем). Приводится к номеру
            // строки здесь, движок работает с числом, как у
            // S3M/XM/IT.
            const uint8_t row = static_cast<uint8_t>((param >> 4) * 10 + (param & 0x0fu));
            out.type = Effect::PatternBreak;
            out.param = (row > 63) ? 0 : row;
            break;
        }
        case 0xE: {
            const uint8_t sub = param >> 4;
            const uint8_t sub_param = param & 0x0fu;
            switch (sub) {
                case 0x0: out.type = Effect::SetFilter; out.param = sub_param; break;
                case 0x1: out.type = Effect::PortaUp; out.rate = SlideRate::Fine; out.param = sub_param; break;
                case 0x2: out.type = Effect::PortaDown; out.rate = SlideRate::Fine; out.param = sub_param; break;
                case 0x3: out.type = Effect::GlissandoControl; out.param = sub_param; break;
                case 0x4: out.type = Effect::SetVibratoWaveform; out.param = sub_param; break;
                case 0x5: out.type = Effect::SetFinetune; out.param = sub_param; break;
                case 0x6: out.type = Effect::PatternLoop; out.param = sub_param; break;
                case 0x7: out.type = Effect::SetTremoloWaveform; out.param = sub_param; break;
                case 0x8: out.type = Effect::SetPanning4Bit; out.param = sub_param; break; // как S8x, у OpenMPT общий путь
                case 0x9: out.type = Effect::Retrigger; out.param = sub_param; break;
                case 0xA: out.type = Effect::VolumeSlide; out.rate = SlideRate::Fine; out.param = static_cast<uint8_t>(sub_param << 4); break;
                case 0xB: out.type = Effect::VolumeSlide; out.rate = SlideRate::Fine; out.param = sub_param; break;
                case 0xC: out.type = Effect::NoteCut; out.param = sub_param; break;
                case 0xD: out.type = Effect::NoteDelay; out.param = sub_param; break;
                case 0xE: out.type = Effect::PatternDelay; out.param = sub_param; break;
                default: ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped; break; // EFx (Invert Loop) не разбирается, Effect::None
            }
            break;
        }
        case 0xF:
            // При VBlank-тайминге (M&K!, N.T., FEST, старый 15-сэмпловый формат)
            // Fxx всегда speed; у остальных сигнатур Fxx >= 0x20 - BPM, как в XM.
            if (vblank_timing || param < 0x20) {
                out.type = Effect::SetSpeed;
            } else {
                out.type = Effect::SetTempo;
            }
            out.param = param;
            break;
        default: break;
    }
}

} // namespace

bool load(formats::ByteSource src, memory::TrackMemory& mem, Song& out, const char** error_out, bool metadata_only) {
    auto fail = [&](const char* msg) {
        if (error_out) *error_out = msg;
        memory::scratch_release_all(mem.scratch); // отказ выселяет сценарий: место возвращается арене
        return false;
    };

    soundsinth::model::g_tracker_load_stats = soundsinth::model::TrackerLoadStats{};
    BinaryReader r(src);

    // Сначала 4-байтная сигнатура на 1080 (M.K., xCHN и др., 31 сэмпл).
    // Если её нет или она не распознана - старый 15-сэмпловый формат
    // (константы kOld*). Подтверждающего байта у него
    // нет, поэтому признак - правдоподобность order-таблицы и громкостей
    // сэмплов. Вход в эту ветку успеха не гарантирует: дальше обычные
    // проверки r.ok() и диапазонов.
    uint8_t channel_count = 0;
    SignatureInfo signature;
    ModLayout layout = kLayout31;

    char sig[4] = {};
    if (r.seek(kSignatureOffset)) {
        r.bytes(sig, 4);
        if (r.ok()) {
            signature = parse_signature(sig);
            if (signature.channels > 0 && signature.channels <= kMaxChannels) {
                channel_count = static_cast<uint8_t>(signature.channels);
            }
        }
    }
    if (channel_count == 0) {
        // Байты на 1080 читаются как ASCII-тег (при коротком файле
        // r.ok() == false и sig нулевой, тоже непечатный) - значит это
        // настоящая, но не поддержанная сигнатура, а не старый формат.
        // Отказ, а не ложное совпадение.
        if (r.ok() && looks_like_ascii_signature(sig)) return fail(kUnknownSignature);
        if (r.size() < kOldPatternDataOffset) return fail(kUnknownSignature);
        channel_count = 4;
        signature = SignatureInfo{};
        signature.vblank_timing = true; // старый формат: Fxx всегда speed (упрощение)
        layout = kLayout15;
    }
    const bool vblank_timing = signature.vblank_timing;
    const uint32_t sample_count = layout.sample_count;
    const uint32_t pattern_data_offset = layout.pattern_data_offset;
    const bool old_format = layout.old_format;

    r.seek(0);
    r.bytes(out.title, kTitleBytes);

    struct RawSample {
        uint16_t length_words;
        int8_t finetune;
        uint8_t volume;
        uint16_t loop_start_words;
        uint16_t loop_size_words;
    };
    RawSample raw_samples[kSampleCount];

    r.seek(kTitleBytes);
    bool samples_plausible = true; // только для проверки старого формата ниже, вместо сигнатуры
    bool loop_length_zero = false;  // петля длиной 0 вешает ProTracker - файл не для Amiga
    bool empty_sample_with_volume = false; // пустой слот с громкостью 64 - так пишет Scream Tracker, не ProTracker
    for (uint32_t i = 0; i < sample_count; ++i) {
        r.skip(kSampleNameBytes); // имя сэмпла резидентно не хранится
        raw_samples[i].length_words = read_u16_be(r);
        // Finetune - знаковый нибл -8..7, шаг 1/8 полутона. В SampleDescriptor
        // шкала XM, 1/128 полутона: нибл умножается на 16, как у OpenMPT и у
        // E5x в диспетчере эффектов.
        const uint8_t finetune_raw = r.u8() & 0x0fu;
        raw_samples[i].finetune = static_cast<int8_t>((finetune_raw > 7 ? finetune_raw - 16 : finetune_raw) * 16);
        raw_samples[i].volume = r.u8();
        if (raw_samples[i].volume > kVolumeMax) samples_plausible = false;
        raw_samples[i].loop_start_words = read_u16_be(r);
        raw_samples[i].loop_size_words = read_u16_be(r);
        if (raw_samples[i].length_words != 0 && raw_samples[i].loop_size_words == 0) loop_length_zero = true;
        if (raw_samples[i].length_words == 0 && raw_samples[i].volume == kVolumeMax) empty_sample_with_volume = true;
    }
    if (!r.ok()) return fail("файл обрезан в заголовках сэмплов");

    if (r.tell() != layout.order_table_offset) return fail("внутренняя ошибка смещений (не должно происходить)");
    const uint8_t song_length = r.u8();
    const uint8_t restart_byte = r.u8();
    uint8_t order_bytes[kOrderEntries];
    r.bytes(order_bytes, kOrderEntries);
    if (!r.ok()) return fail("файл обрезан в таблице воспроизведения");

    // Как у libxmp (мусор в order у dragnet.mod): список обрывается на
    // первом байте > 0x7F. order_count обрезается здесь, дальше значений вне
    // [0, pattern_count) нет.
    uint16_t effective_order_count = 0;
    uint8_t max_pattern_seen = 0;
    bool any_pattern = false;
    for (uint32_t i = 0; i < song_length && i < kOrderEntries; ++i) {
        if (order_bytes[i] > 0x7f) break;
        effective_order_count = static_cast<uint16_t>(i + 1);
        if (!any_pattern || order_bytes[i] > max_pattern_seen) {
            max_pattern_seen = order_bytes[i];
            any_pattern = true;
        }
    }
    const uint16_t pattern_count = any_pattern ? static_cast<uint16_t>(max_pattern_seen + 1) : 0;

    // Старый формат не подтверждён сигнатурой: без этой проверки
    // любой файл без сигнатуры на 1080 читался бы как MOD с
    // бессмысленным содержимым вместо отказа.
    if (old_format && (!samples_plausible || !any_pattern || song_length == 0)) {
        return fail(kUnknownSignature);
    }

    // --- Резидентные метаданные (в mem.resident) ---
    out.channel_count = channel_count;
    out.default_speed = soundsinth::model::kDefaultSpeed;
    out.default_tempo = soundsinth::model::kDefaultTempo;
    out.default_global_volume = kGlobalVolumeMax;
    // Поля preamp у MOD нет, как у OpenMPT: 256 / каналов в пределах 32..128.
    out.sample_preamp = static_cast<uint8_t>(std::clamp(256 / channel_count, 32, 128));
    out.frequency_model = soundsinth::model::FrequencyModel::Amiga;
    out.quirks = soundsinth::model::kQuirkModArpeggioWrap |
                 soundsinth::model::kQuirkModSampleSwap | soundsinth::model::kQuirkModHardwarePanning |
                 soundsinth::model::kQuirkModOffsetPastLoopEnd |
                 (vblank_timing ? static_cast<soundsinth::model::QuirkFlags>(soundsinth::model::kQuirkModVBlankTiming) : 0u);
    // Как у libxmp: 0x7F и выше - маркер трекера, а 0x78 (120) NoiseTracker
    // пишет по умолчанию; ни то, ни другое не позиция.
    out.restart_position = (restart_byte < 0x7f && restart_byte != 0x78 && restart_byte < effective_order_count)
                                ? restart_byte
                                : 0;

    out.order_count = effective_order_count;
    if (effective_order_count > 0) {
        auto* order = memory::arena_new<uint16_t>(mem.resident, effective_order_count);
        if (!order) return fail("резидентная память переполнена (order)");
        for (uint32_t i = 0; i < effective_order_count; ++i) order[i] = order_bytes[i];
        out.order = order;
    }

    out.pattern_count = pattern_count;
    Pattern* patterns = nullptr;
    if (pattern_count > 0) {
        patterns = memory::arena_new<Pattern>(mem.resident, pattern_count);
        if (!patterns) return fail("резидентная память переполнена (patterns)");
        out.patterns = patterns;
    }

    out.sample_count = static_cast<uint16_t>(sample_count);
    auto* samples = memory::arena_new<SampleDescriptor>(mem.resident, sample_count);
    if (!samples) return fail("резидентная память переполнена (samples)");
    out.samples = samples;

    out.instrument_count = static_cast<uint16_t>(sample_count);
    auto* instruments = memory::arena_new<Instrument>(mem.resident, sample_count);
    if (!instruments) return fail("резидентная память переполнена (instruments)");
    out.instruments = instruments;

    // --- SampleDescriptor/Instrument из прочитанных заголовков ---
    const uint32_t pattern_bytes = static_cast<uint32_t>(channel_count) * kRowsPerPattern * kCellBytes;
    const uint32_t pattern_data_bytes = static_cast<uint32_t>(pattern_count) * pattern_bytes;
    uint32_t running_file_offset = pattern_data_offset + pattern_data_bytes;
    for (uint32_t i = 0; i < sample_count; ++i) {
        const RawSample& rs = raw_samples[i];
        SampleDescriptor& sd = samples[i];
        sd.encoding = SampleEncoding::Pcm8;
        sd.channels = 1;
        // Сэмпл в одно слово у ProTracker пустой; его 2 байта в файле есть.
        sd.length_samples = rs.length_words > 1 ? static_cast<uint32_t>(rs.length_words) * 2u : 0;
        // MOD не прореживается (длина заведомо меньше порога), исходная
        // длина равна итоговой; распаковка MOD читает length_samples.
        sd.source_length_samples = sd.length_samples;
        // Кодек у MOD всегда Raw8 (8-битный PCM); ставится здесь,
        // чтобы дескриптор был готов до чтения данных.
        sd.resident_encoding = soundsinth::model::ResidentEncoding::Raw8;
        const ModLoop loop =
            old_format ? old_sample_loop(sd.length_samples, rs.loop_start_words, rs.loop_size_words)
                       : mod_sample_loop(sd.length_samples, rs.loop_start_words, rs.loop_size_words, channel_count == 4);
        sd.loop_start = loop.start;
        sd.loop_end = loop.end;
        sd.loop_enabled = loop.enabled;
        sd.loop_bidirectional = false;
        sd.file_offset = running_file_offset;
        sd.c5_speed = 8363; // период 428 на такте Paula NTSC; finetune - отдельно, в sd.finetune
        sd.relative_note = 0;
        sd.finetune = rs.finetune;
        sd.default_volume = (rs.volume > kVolumeMax) ? kVolumeMax : rs.volume;
        sd.default_panning = -1;
        if (old_format) {
            // SoundTracker играет у зацикленного сэмпла только петлю: данные до
            // её начала не звучат и не читаются. Finetune у него нет.
            sd.finetune = 0;
            if (loop.enabled && loop.start > 0) {
                sd.file_offset += loop.start;
                sd.length_samples -= loop.start;
                sd.source_length_samples = sd.length_samples;
                sd.loop_end = loop.end - loop.start;
                sd.loop_start = 0;
            }
        }

        Instrument& ins = instruments[i];
        ins.global_volume = kGlobalVolumeMax;
        ins.default_sample_index = static_cast<uint16_t>(i);
        ins.note_to_sample_ranges = nullptr; // MOD: инструмент == сэмпл, keymap не нужен

        running_file_offset += static_cast<uint32_t>(rs.length_words) * 2u; // 8-битный PCM, 1 байт на отсчёт
    }

    // --- Паттерны: разбор и упаковка в PSRAM ---
    // Попутно - признаки режима ProTracker. Отдельного прохода нет: второе
    // чтение паттернов по шине стоило бы вдвое больше времени.
    ModPatternStats stats;
    PatternCell cells[kMaxChannels];
    for (uint32_t p = 0; p < pattern_count; ++p) {
        if (!r.seek(pattern_data_offset + p * pattern_bytes)) {
            return fail("файл обрезан в данных паттерна");
        }
        // Упаковщику берётся всё свободное, но не больше нужного худшему
        // паттерну архива: буфер и арена - один пул, и на файле с большой
        // ареной упаковщик обходится остатком.
        uint32_t pack_bytes = 0;
        uint8_t* pack_buf = memory::scratch_take_upto(mem.scratch, memory::Scratch::PatternPack,
                                                      memory::kPatternPackBufferBytes, pack_bytes);
        if (!pack_buf) return fail("резидентная память переполнена (буфер упаковщика)");
        patterns::PatternPacker packer(pack_buf, pack_bytes, kRowsPerPattern, channel_count);
        for (uint32_t row = 0; row < kRowsPerPattern; ++row) {
            for (uint32_t ch = 0; ch < channel_count; ++ch) {
                uint8_t b[kCellBytes];
                r.bytes(b, kCellBytes);
                const uint16_t period = static_cast<uint16_t>(((b[0] & 0x0fu) << 8) | b[1]);
                const uint8_t sample_num = static_cast<uint8_t>((b[0] & 0xf0u) | (b[2] >> 4));
                const uint8_t effect_nibble = b[2] & 0x0fu;
                const uint8_t param = b[3];
                stats.observe(period, effect_nibble, param);

                PatternCell cell;
                cell.note = soundsinth::model::amiga_period_to_note(period);
                cell.instrument = sample_num;
                decode_effect(effect_nibble, param, vblank_timing, cell.effect);
                cells[ch] = cell;
            }
            if (!packer.add_row(cells)) return fail("паттерн не влезает в буфер упаковщика");
        }
        if (!r.ok()) return fail("файл обрезан в данных паттерна");

        const uint32_t offset = packer.finish(mem.psram);
        memory::scratch_leave(mem.scratch, memory::Scratch::PatternPack);
        if (offset == memory::kPatternAllocFailed) return fail("зона паттернов PSRAM переполнена");
        patterns[p].row_count = kRowsPerPattern;
        patterns[p].channel_count = channel_count;
        patterns[p].psram_offset = offset;
    }

    out.quirks |= mode_quirks(stats, signature.protracker, loop_length_zero);

    // Петля за концом сэмпла: ProTracker читает дальше, данные следующего
    // сэмпла. У M.K. с нотами в трёх октавах сэмпл удлиняется до конца петли,
    // как у OpenMPT; последний - не дальше конца файла. Не у файлов с пустыми
    // слотами громкости 64: петли там битые.
    if (std::memcmp(sig, "M.K.", 4) == 0 && !old_format && stats.only_amiga_notes && !empty_sample_with_volume) {
        for (uint32_t i = 0; i < sample_count; ++i) {
            SampleDescriptor& sd = samples[i];
            const ModLoop loop = mod_sample_loop(sd.length_samples, raw_samples[i].loop_start_words,
                                                 raw_samples[i].loop_size_words, channel_count == 4);
            if (!loop.enabled || loop.header_end <= sd.length_samples) continue;
            const uint32_t file_left = sd.file_offset < r.size() ? r.size() - sd.file_offset : 0;
            const uint32_t length = std::min(loop.header_end, file_left);
            if (length <= sd.length_samples) continue;
            sd.length_samples = length;
            sd.source_length_samples = length;
            sd.loop_end = length;
        }
    }

    // Паттерны упакованы, объём известен точно, остаток PSRAM отдаётся
    // сэмплам. Строго после упаковки паттернов и до первого сэмпла: если
    // раньше, паттерны дорастают поверх зоны сэмплов.
    memory::psram_freeze_pattern_zone(mem.psram);

    // --- Сэмплы: перепаковка в Raw8-страницы PSRAM. MOD-сэмплы
    // всегда 8-битные, контрольные точки не нужны (декодера нет,
    // доступ по индексу). Прореживание (больше 1 МБ и выше 20 кГц)
    // невозможно: длина - u16 слов, максимум 131070 байт. ---
    soundsinth::model::note_source_end(out);
    // metadata_only: PCM вытянет load_sample_pcm() позже, по одному.
    if (metadata_only) return true;
    for (uint32_t i = 0; i < sample_count; ++i) {
        const SampleDescriptor& sd = out.samples[i];
        // Данные сэмпла начинаются за концом файла: у MOD отказ всей
        // загрузки, IT/S3M/XM пропускают сэмпл. Обрыв внутри данных
        // пропускает только этот сэмпл.
        if (sd.length_samples != 0 && !r.seek(sd.file_offset)) return fail("файл обрезан перед данными сэмпла");
        if (!soundsinth::model::sample_is_resident(sd)) continue;
        const char* reason = nullptr;
        if (!soundsinth::model::pack_file_pcm(mem, r, sd, static_cast<uint16_t>(i), &reason)) {
            soundsinth::model::note_sample_failed(static_cast<uint16_t>(i), reason);
        }
    }

    return true;
}

bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song,
                      uint16_t sample_index, const char** reason_out) {
    if (sample_index >= song.sample_count) {
        if (reason_out) *reason_out = "номер сэмпла вне песни";
        return false;
    }
    BinaryReader r(src);
    return soundsinth::model::pack_file_pcm(mem, r, song.samples[sample_index], sample_index, reason_out);
}

} // namespace soundsinth::formats::mod
