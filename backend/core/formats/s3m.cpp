// SPDX-License-Identifier: MIT
#include "core/formats/s3m.h"

#include <algorithm>
#include <cstring>

#include "platform/compiler.h"
#include "core/formats/load_stats.h"
#include "core/codec/loop_unroll.h"
#include "core/codec/pack_file_pcm.h"
#include "core/formats/binary_reader.h"
#include "core/codec/pattern_packer.h"

namespace soundsinth::formats::s3m {

namespace {

using formats::BinaryReader;
using soundsinth::model::Effect;
using soundsinth::model::EffectCommand;
using soundsinth::model::Instrument;
using soundsinth::model::kGlobalVolumeMax;
using soundsinth::model::kPanCenter;
using soundsinth::model::kVolumeMax;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::SampleDescriptor;
using soundsinth::model::SampleEncoding;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

// Заголовок песни (S3MFileHeader), 96 байт.
constexpr uint32_t kTitleBytes            = 28;
constexpr uint32_t kOrdNumOffset          = 0x20;
constexpr uint32_t kSmpNumOffset          = 0x22;
constexpr uint32_t kPatNumOffset          = 0x24;
constexpr uint32_t kFlagsOffset           = 0x26;
constexpr uint32_t kCwtvOffset            = 0x28;
constexpr uint32_t kFfiOffset             = 0x2a; // 1 - сэмплы знаковые, 2 - беззнаковые
constexpr uint32_t kMagicOffset           = 0x2c; // "SCRM"
constexpr uint32_t kGlobalVolOffset       = 0x30;
constexpr uint32_t kSpeedOffset           = 0x31;
constexpr uint32_t kTempoOffset           = 0x32;
constexpr uint32_t kMasterVolumeOffset    = 0x33; // mixing volume и бит стерео
constexpr uint32_t kUltraClicksOffset     = 0x34;
constexpr uint32_t kUsePanningTableOffset = 0x35;
constexpr uint32_t kReserved2Offset       = 0x36; // у OpenMPT и Schism - продолжение версии
constexpr uint32_t kSpecialOffset         = 0x3e;
constexpr uint32_t kChannelsOffset        = 0x40; // channels[32]
constexpr uint32_t kOrderTableOffset      = 0x60; // order, сразу за заголовком
constexpr uint32_t kHeaderChannels        = 32;
constexpr uint32_t kParagraphBytes        = 16; // единица парапоинтеров

constexpr uint16_t kTrackerMask          = 0xf000u;
constexpr uint16_t kTrackerScreamTracker = 0x1000u;
constexpr uint16_t kCwtvSt300            = 0x1300u; // Scream Tracker 3.00: адресов GUS ещё не писал
constexpr uint16_t kFlagAmigaLimits      = 0x10u;
constexpr uint8_t kMasterVolumeStereo    = 0x80u;
constexpr uint8_t kPanningTablePresent   = 0xfcu; // usePanningTable
constexpr uint8_t kChannelUnused         = 0xffu;
constexpr uint8_t kChannelDisabled       = 0x80u;
constexpr uint8_t kChannelTypeMask       = 0x7fu;
constexpr uint8_t kChannelRight          = 0x08u; // тип 8..15 - правые PCM-каналы

// Заголовок сэмпла (S3MSampleHeader), 80 байт.
constexpr uint32_t kSampleHeaderBytes       = 80;
constexpr uint32_t kSampleTypeOffset        = 0;  // 1 - PCM, 2.. - AdLib
constexpr uint32_t kSampleDataPointerOffset = 13; // 3 байта: старший, младший, средний
constexpr uint32_t kSampleLengthOffset      = 16;
constexpr uint32_t kSampleLoopStartOffset   = 20;
constexpr uint32_t kSampleLoopEndOffset     = 24;
constexpr uint32_t kSampleVolumeOffset      = 28;
constexpr uint32_t kSamplePackOffset        = 30;
constexpr uint32_t kSampleFlagsOffset       = 31;
constexpr uint32_t kSampleC5SpeedOffset     = 32;
constexpr uint32_t kSampleGusAddressOffset  = 40; // адрес в памяти GUS, пишет ST3
constexpr uint8_t kSampleTypePcm            = 1;
constexpr uint8_t kSampleLoop               = 0x01u;
constexpr uint8_t kSampleStereo             = 0x02u;
constexpr uint8_t kSample16Bit              = 0x04u;

// Маркер ячейки упакованного паттерна.
constexpr uint8_t kCellChannelMask    = 0x1fu;
constexpr uint8_t kCellNoteInstrument = 0x20u;
constexpr uint8_t kCellVolume         = 0x40u;
constexpr uint8_t kCellEffect         = 0x80u;

constexpr uint32_t kRowsPerPattern = 64;
constexpr uint8_t kMaxChannels     = 32; // предел формата: 5-битный номер канала в маркере ячейки
static_assert(kMaxChannels == kHeaderChannels, "the cell channel number is within channels[32]");
static_assert(kMaxChannels <= soundsinth::model::kMaxPatternChannels, "the packer row mask is 64 channels");

uint16_t read_u16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Смещение данных сэмпла в байтах: 24-битный номер параграфа, байты в
// порядке старший, младший, средний (сверено с OpenMPT).
uint32_t sample_data_offset(const uint8_t* sample_header) {
    const uint8_t* dp = sample_header + kSampleDataPointerOffset;
    return ((static_cast<uint32_t>(dp[0]) << 16) | (static_cast<uint32_t>(dp[2]) << 8) | dp[1]) * kParagraphBytes;
}

// Буквенная команда S3M (1='A'..26='Z') -> EffectCommand, как у OpenMPT.
// Тонкость слайда - в параметре (kQuirkFineSlideInParam), rate не ставится.
void decode_effect(uint8_t command, uint8_t param, EffectCommand& out) {
    switch (command + '@') {
        case 'A':
            out.type  = Effect::SetSpeed;
            out.param = param;
            break;
        case 'B':
            out.type  = Effect::PositionJump;
            out.param = param;
            break;
        case 'C': {
            // Pattern Break: параметр BCD, как у MOD Dxx; OpenMPT делает так же
            // для файлов не из IT.
            const uint8_t row = static_cast<uint8_t>((param >> 4) * 10 + (param & 0x0fu));
            out.type          = Effect::PatternBreak;
            out.param         = (row > 63) ? 0 : row;
            break;
        }
        case 'D':
            out.type  = Effect::VolumeSlide;
            out.param = param;
            break;
        case 'E':
            out.type  = Effect::PortaDown;
            out.param = param;
            break;
        case 'F':
            out.type  = Effect::PortaUp;
            out.param = param;
            break;
        case 'G':
            out.type  = Effect::TonePorta;
            out.param = param;
            break;
        case 'H':
            out.type  = Effect::Vibrato;
            out.param = param;
            break;
        case 'I':
            out.type  = Effect::Tremor;
            out.param = param;
            break;
        case 'J':
            out.type  = Effect::Arpeggio;
            out.param = param;
            break;
        case 'K':
            out.type  = Effect::VibratoVolSlide;
            out.param = param;
            break;
        case 'L':
            out.type  = Effect::TonePortaVolSlide;
            out.param = param;
            break;
        case 'M':
            out.type  = Effect::SetChannelVolume;
            out.param = param;
            break;
        case 'N':
            out.type  = Effect::ChannelVolumeSlide;
            out.param = param;
            break;
        case 'O':
            out.type  = Effect::SampleOffset;
            out.param = param;
            break;
        case 'P':
            out.type  = Effect::PanningSlide;
            out.param = param;
            break;
        case 'Q':
            out.type  = Effect::Retrigger;
            out.param = param;
            break;
        case 'R':
            out.type  = Effect::Tremolo;
            out.param = param;
            break;
        case 'S': { // расширенная семья Sxx (та же группа, что MOD Exx)
            const uint8_t sub       = param >> 4;
            const uint8_t sub_param = param & 0x0fu;
            switch (sub) {
                case 0x1:
                    out.type  = Effect::GlissandoControl;
                    out.param = sub_param;
                    break;
                case 0x2:
                    out.type  = Effect::SetFinetune;
                    out.param = sub_param;
                    break;
                case 0x3:
                    out.type  = Effect::SetVibratoWaveform;
                    out.param = sub_param;
                    break;
                case 0x4:
                    out.type  = Effect::SetTremoloWaveform;
                    out.param = sub_param;
                    break;
                case 0x5:
                    out.type  = Effect::SetPanbrelloWaveform;
                    out.param = sub_param;
                    break;
                case 0x6:
                    out.type  = Effect::FinePatternDelay;
                    out.param = sub_param;
                    break;
                case 0x8:
                    out.type  = Effect::SetPanning4Bit;
                    out.param = sub_param;
                    break; // как у IT
                case 0x9:
                    out.type  = Effect::SoundControl;
                    out.param = sub_param;
                    break;
                case 0xA:
                    out.type  = Effect::HighOffset;
                    out.param = sub_param;
                    break;
                case 0xB:
                    out.type  = Effect::PatternLoop;
                    out.param = sub_param;
                    break;
                case 0xC:
                    out.type  = Effect::NoteCut;
                    out.param = sub_param;
                    break;
                case 0xD:
                    out.type  = Effect::NoteDelay;
                    out.param = sub_param;
                    break;
                case 0xE:
                    out.type  = Effect::PatternDelay;
                    out.param = sub_param;
                    break;
                case 0xF:
                    out.type  = Effect::SetActiveMidiMacro;
                    out.param = sub_param;
                    break;
                default:
                    ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped;
                    break; // S0x, S7x не разбираются, Effect::None
            }
            break;
        }
        case 'T':
            out.type  = Effect::SetTempo;
            out.param = param;
            break;
        case 'U':
            out.type  = Effect::FineVibrato;
            out.param = param;
            break;
        // Vxx/Wxy в S3M - шкала 0..64 (как GlobalVolume заголовка), а
        // PlayState::global_volume - 0..128 (как IT): Vxx умножается на 2
        // здесь, при загрузке; Wxy - как есть, шаг вдвое берёт диспетчер
        // (ниблы x2 в байт не влезают).
        case 'V':
            out.type  = Effect::SetGlobalVolume;
            out.param = static_cast<uint8_t>(param > kVolumeMax ? kGlobalVolumeMax : param * 2);
            break;
        case 'W':
            out.type  = Effect::GlobalVolumeSlide;
            out.param = param;
            break;
        case 'X':
            out.type  = Effect::SetPanning;
            out.param = param;
            break;
        case 'Y':
            out.type  = Effect::Panbrello;
            out.param = param;
            break;
        case 'Z':
            out.type  = Effect::SetMidiMacro;
            out.param = param;
            break;
        default: // 0 ('@') - нет эффекта; за пределами A..Z не разбирается
            if (command != 0) ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped;
            break;
    }
}

// Нота S3M: октава в старшем нибле, полутон в младшем; 0xFE - Note-Off,
// 0xF0..0xFD - не ноты, пропускаются (инструмент остаётся).
uint8_t decode_note(uint8_t note_raw) {
    if (note_raw == 0xfe) return soundsinth::model::kNoteOff;
    if (note_raw >= 0xf0) return soundsinth::model::kNoteNone;
    return static_cast<uint8_t>((note_raw >> 4) * 12 + (note_raw & 0x0fu));
}

// Одна упакованная строка: записи до байта 0x00. Канал >= channel_count в
// потоке возможен: байты читаются, ячейка не сохраняется; так же у
// выключенного канала ST3 (dropped_channels). false - файл оборвался.
// Отдельной функцией во флеше: встроенная растит s3m::load на 398 байт.
SOUNDSINTH_NOINLINE bool read_packed_row(BinaryReader& r, uint8_t channel_count, uint32_t dropped_channels, PatternCell* cells) {
    for (uint32_t ch = 0; ch < channel_count; ++ch) {
        cells[ch] = PatternCell{};
    }
    for (;;) {
        const uint8_t info = r.u8();
        if (!r.ok()) return false;
        if (info == 0x00) return true;
        const uint8_t channel = info & kCellChannelMask;

        PatternCell cell;
        if (info & kCellNoteInstrument) {
            cell.note       = decode_note(r.u8());
            cell.instrument = r.u8();
        }
        if (info & kCellVolume) {
            const uint8_t vol = r.u8();
            // 128..192 - панорама 0..64: так пишут ModPlug, OpenMPT и Schism (ST3
            // таких значений не пишет). Остальное - громкость до 64.
            if (vol >= 128 && vol <= 192) {
                cell.volume.type  = VolumeColumnType::SetPanning;
                cell.volume.param = static_cast<uint8_t>(vol - 128);
            } else {
                cell.volume.type  = VolumeColumnType::SetVolume;
                cell.volume.param = (vol > kVolumeMax) ? kVolumeMax : vol;
            }
        }
        if (info & kCellEffect) {
            const uint8_t command = r.u8();
            const uint8_t param   = r.u8();
            decode_effect(command, param, cell.effect);
        }
        if (!r.ok()) return false;
        if (channel < channel_count && ((dropped_channels >> channel) & 1u) == 0) {
            cells[channel] = cell;
        }
    }
}

// Панорама каналов по байтам channels[] заголовка и таблице панорамы.
void set_channel_panning(const uint8_t* channels_raw, uint8_t channel_count, bool is_stereo, bool genuine_st3, const uint8_t* panning_table, Song& out) {
    // Аппаратная L/R-разводка по типу канала: 8..15 - правые каналы
    // классической PCM-разводки ScreamTracker, 0..7 - левые. Только у
    // стерео-файлов, иначе ST3 сводил всё в моно. Значения - 0x33/0xCC у
    // OpenMPT (шкала 0..256) в нашей 0..64 с округлением.
    if (is_stereo) {
        constexpr uint8_t kHardwarePanLeft  = 13;
        constexpr uint8_t kHardwarePanRight = 51;
        for (uint32_t i = 0; i < channel_count; ++i) {
            if (channels_raw[i] == kChannelUnused) continue;
            const uint8_t ctype = channels_raw[i] & kChannelTypeMask;
            out.channel_pan[i]  = (ctype & kChannelRight) ? kHardwarePanRight : kHardwarePanLeft;
        }
    }
    // Каналы AdLib (тип 16..29) - в центр, как у OpenMPT.
    bool adlib[kHeaderChannels] = {};
    for (uint32_t i = 0; i < channel_count; ++i) {
        const uint8_t ctype = channels_raw[i] & kChannelTypeMask;
        if (channels_raw[i] != kChannelUnused && ctype >= 16 && ctype <= 29) {
            out.channel_pan[i] = kPanCenter;
            adlib[i]           = true;
        }
    }
    // Таблица поверх разводки, и у моно-файла тоже: бит 0x20 - панорама задана,
    // младшие 4 бита - 0..15 во всю шкалу. У настоящего ST3 не на
    // AdLib-каналах. Шкала OpenMPT 0..256 - в нашу 0..64 с округлением, как у
    // аппаратной разводки.
    for (uint32_t i = 0; i < channel_count; ++i) {
        if ((panning_table[i] & 0x20u) == 0 || (genuine_st3 && adlib[i])) continue;
        const uint32_t pan256 = ((panning_table[i] & 0x0fu) * 256u + 8u) / 15u;
        out.channel_pan[i]    = static_cast<uint8_t>((pan256 + 2u) / 4u);
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
    const uint32_t file_size = r.size();

    // Заголовок одним чтением, поля - по смещениям, как S3MFileHeader у OpenMPT.
    uint8_t hdr[kOrderTableOffset];
    r.bytes(hdr, sizeof(hdr));
    if (!r.ok()) return fail("file too small (no S3M header)");
    if (std::memcmp(hdr + kMagicOffset, "SCRM", 4) != 0) return fail("no SCRM signature");
    std::memcpy(out.title, hdr, kTitleBytes);

    const uint16_t order_count      = read_u16le(hdr + kOrdNumOffset);
    const uint16_t sample_count     = read_u16le(hdr + kSmpNumOffset);
    const uint16_t pattern_count    = read_u16le(hdr + kPatNumOffset);
    const uint16_t header_flags     = read_u16le(hdr + kFlagsOffset);
    const uint16_t cwtv             = read_u16le(hdr + kCwtvOffset); // трекер и версия, старший нибл 1 - Scream Tracker
    const uint16_t ffi              = read_u16le(hdr + kFfiOffset);
    const uint8_t global_vol_raw    = hdr[kGlobalVolOffset];
    const uint8_t speed_raw         = hdr[kSpeedOffset];
    const uint8_t tempo_raw         = hdr[kTempoOffset];
    const uint8_t master_volume_raw = hdr[kMasterVolumeOffset];
    const uint8_t ultra_clicks      = hdr[kUltraClicksOffset];
    const uint8_t use_panning_table = hdr[kUsePanningTableOffset]; // за парапоинтерами таблица панорамы каналов
    const uint16_t reserved2        = read_u16le(hdr + kReserved2Offset);
    const uint16_t special          = read_u16le(hdr + kSpecialOffset);
    const uint8_t* channels_raw     = hdr + kChannelsOffset;

    uint8_t channel_count = 1;
    for (uint32_t i = 0; i < kHeaderChannels; ++i) {
        if (channels_raw[i] != kChannelUnused) channel_count = static_cast<uint8_t>(i + 1);
    }
    // Выключенный канал: ST3 его ячейки не хранит, даже Axx/Bxx/Cxx; у других
    // трекеров (cwtv не 0x1xxx) эффекты идут, звука нет, как у OpenMPT.
    // ModPlug с cwtv 0x1320 идёт как ST3. Ноты в выключенных каналах - у 42 из
    // 8455 S3M архива, у 41 из них ST3.
    const bool st3            = (cwtv & kTrackerMask) == kTrackerScreamTracker;
    uint32_t dropped_channels = 0;
    for (uint32_t i = 0; i < channel_count; ++i) {
        if (channels_raw[i] == kChannelUnused || (channels_raw[i] & kChannelDisabled) == 0) continue;
        if (st3) {
            dropped_channels |= static_cast<uint32_t>(1) << i;
        } else {
            out.channel_muted |= static_cast<uint64_t>(1) << i;
        }
    }

    // Резидентные записи - до таблиц: таблицы читаются прямо в них. Order -
    // байтами в начало своего массива, парапоинтер сэмпла - в
    // SampleDescriptor::file_offset, паттерна - в Pattern::psram_offset; разбор
    // записи забирает его и ставит своё. Копии таблиц нет, число записей
    // ограничивает резидентная арена, а не шапка файла.
    uint16_t* order = nullptr;
    if (order_count > 0) {
        order = memory::arena_new<uint16_t>(mem.resident, order_count);
        if (!order) return fail("resident memory overflowed (order)");
    }
    Pattern* patterns = nullptr;
    if (pattern_count > 0) {
        patterns = memory::arena_new<Pattern>(mem.resident, pattern_count);
        if (!patterns) return fail("resident memory overflowed (patterns)");
    }
    SampleDescriptor* samples = nullptr;
    Instrument* instruments   = nullptr;
    if (sample_count > 0) {
        samples = memory::arena_new<SampleDescriptor>(mem.resident, sample_count);
        if (!samples) return fail("resident memory overflowed (samples)");
        instruments = memory::arena_new<Instrument>(mem.resident, sample_count);
        if (!instruments) return fail("resident memory overflowed (instruments)");
    }

    if (!r.seek(kOrderTableOffset)) return fail("file truncated before the order table");
    auto* order_bytes = reinterpret_cast<uint8_t*>(order);
    if (order_count > 0) r.bytes(order_bytes, order_count);
    if (!r.ok()) return fail("file truncated inside the order table");
    for (uint32_t i = 0; i < sample_count; ++i) {
        samples[i].file_offset = r.u16();
    }
    for (uint32_t i = 0; i < pattern_count; ++i) {
        patterns[i].psram_offset = r.u16();
    }
    if (!r.ok()) return fail("file truncated inside the parapointer tables");

    // Стерео по правилу OpenMPT: бит 0x80 masterVolume или файл, в котором
    // OpenMPT узнаёт версию, - ModPlug по отпечатку заголовка (cwtv 0x1320,
    // special 0, число order кратно 16, ultraClicks 0, флаги только из 0x50,
    // таблица панорамы, паттерны после сэмплов; ModPlug 1.0 alpha бит стерео не
    // ставил) и OpenMPT по cwtv 0x5xxx (кроме NESMusa, Graoumf Tracker и
    // Liquid Tracker с тем же номером).
    const bool offsets_canonical = sample_count > 0 && pattern_count > 0 && patterns[0].psram_offset > samples[0].file_offset;
    const bool modplug_or_schism = cwtv == 0x1320 && special == 0 && (order_count & 0x01u) == 0 && ultra_clicks == 0 && (header_flags & ~0x50u) == 0 &&
                                   use_panning_table == kPanningTablePresent && offsets_canonical;
    const bool modplug        = modplug_or_schism && (order_count & 0x0fu) == 0;
    const bool liquid_tracker = reserved2 == 0 && ultra_clicks == 16 && channels_raw[1] != 1;
    const bool openmpt        = (cwtv & kTrackerMask) == 0x5000u && (cwtv & 0xff00u) != 0x5700u && cwtv != 0x5447u && !liquid_tracker && (cwtv & 0x0fffu) != 0;
    const bool is_stereo      = (master_volume_raw & kMasterVolumeStereo) != 0 || modplug || openmpt;
    // Настоящий ST3 - cwtv 0x1xxx без отпечатков других трекеров с тем же
    // номером: ModPlug и Schism (как выше, число order чётное), Velvet Studio,
    // PlayerPRO и Impulse Tracker до 1.03 (флаги 0 или 8, без таблицы
    // панорамы). Sound Club 2 не отличается.
    const bool other_as_st3_20 =
        cwtv == 0x1320 && special == 0 && ultra_clicks == 0 && use_panning_table != kPanningTablePresent && (header_flags == 0 || header_flags == 8);
    const bool genuine_st3 = st3 && !modplug_or_schism && !other_as_st3_20;

    // Таблица панорамы каналов - сразу за парапоинтерами.
    uint8_t panning_table[kHeaderChannels] = {};
    if (use_panning_table == kPanningTablePresent) {
        r.bytes(panning_table, sizeof(panning_table));
        if (!r.ok()) return fail("file truncated inside the channel panning table");
    }

    for (uint32_t i = 0; i < pattern_count; ++i) {
        const uint32_t pp = patterns[i].psram_offset;
        if (pp != 0 && pp * kParagraphBytes >= file_size) return fail("pattern parapointer runs past the end of the file");
    }

    // --- Резидентные метаданные ---
    out.channel_count = channel_count;
    out.default_speed = (speed_raw == 0) ? soundsinth::model::kDefaultSpeed : speed_raw;
    out.default_tempo = (tempo_raw < soundsinth::model::kMinTempo) ? soundsinth::model::kDefaultTempo : tempo_raw;
    // GlobalVolume S3M 0..64 -> шкала 0..128.
    out.default_global_volume = static_cast<uint8_t>((global_vol_raw > kVolumeMax ? kVolumeMax : global_vol_raw) * 2);
    // Предусиление как у OpenMPT: max(masterVolume & 0x7F, 0x10); 2 и 0x12 -
    // 0x20, 0 в младших битах - 48 (так читает и ST3). У моно-файла - *8/11
    // с округлением: оба эталона делят, иначе моно на 2.8 дБ громче. Ветка
    // старого формата с masterVolume меньше 8 не воспроизводится.
    if (master_volume_raw == 2 || master_volume_raw == (2 | 0x10u)) {
        out.sample_preamp = 0x20;
    } else if ((master_volume_raw & ~kMasterVolumeStereo) == 0) {
        out.sample_preamp = 48;
    } else {
        out.sample_preamp = std::max<uint8_t>(master_volume_raw & ~kMasterVolumeStereo, 0x10);
    }
    if (!is_stereo) out.sample_preamp = static_cast<uint8_t>((out.sample_preamp * 8u + 5u) / 11u);

    set_channel_panning(channels_raw, channel_count, is_stereo, genuine_st3, panning_table, out);

    out.frequency_model = soundsinth::model::FrequencyModel::Amiga; // у S3M питч по периодам, как у MOD, с приведением к c5speed сэмпла
    out.quirks = soundsinth::model::kQuirkS3mSharedEffectMemory | soundsinth::model::kQuirkS3mVolSlideDownPriority | soundsinth::model::kQuirkFineSlideInParam |
                 soundsinth::model::kQuirkS3mRetrigOnZero | soundsinth::model::kQuirkS3mArpeggioMemory | soundsinth::model::kQuirkS3mOffsetWrapInLoop;
    // amigaLimits: период в границах 113..856, как у OpenMPT. Без него ноты вне
    // C-3..B-5 не режутся.
    if (header_flags & kFlagAmigaLimits) out.quirks |= soundsinth::model::kQuirkAmigaLimits;
    out.flow_mode        = soundsinth::model::kFlowLoopGlobalTarget | soundsinth::model::kFlowLoopEndAdvancesRow;
    out.restart_position = 0; // отдельного байта restart у S3M нет, повтор через order-таблицу (0xFF/0xFE)

    out.order_count = order_count;
    if (order_count > 0) {
        // Байты - в начале того же массива: расширение до 16 бит с конца не
        // затирает ещё не прочитанные.
        for (uint32_t i = order_count; i-- > 0;) {
            const uint8_t b = order_bytes[i];
            order[i]        = (b == 0xff) ? soundsinth::model::kOrderEnd : (b == 0xfe) ? soundsinth::model::kOrderSkip : static_cast<uint16_t>(b);
        }
        out.order = order;
    }

    out.pattern_count    = pattern_count;
    out.patterns         = patterns;
    out.sample_count     = sample_count;
    out.samples          = samples;
    out.instrument_count = sample_count;
    out.instruments      = instruments;

    // --- Заголовки сэмплов ---
    struct RawSampleInfo {
        uint8_t sample_type     = 0;
        uint32_t data_offset    = 0;
        uint32_t length_samples = 0;
        uint8_t pack            = 0;
        bool is16bit            = false;
        bool is_stereo          = false;
    };

    // Адреса GUS в заголовках сэмплов пишут все версии ST3, кроме ранних 3.00:
    // файл с непустыми сэмплами без них сделан не в ST3, как у OpenMPT.
    bool any_samples       = false;
    uint16_t gus_addresses = 0;
    uint8_t shdr[kSampleHeaderBytes];
    for (uint32_t i = 0; i < sample_count; ++i) {
        SampleDescriptor& sd = samples[i];
        const uint32_t pp    = sd.file_offset; // парапоинтер из таблицы
        sd.file_offset       = 0;
        const uint32_t base  = pp * kParagraphBytes;
        if (pp == 0 || base + kSampleHeaderBytes > file_size) continue; // пустой слот, SampleDescriptor остаётся по умолчанию

        if (!r.seek(base)) continue;
        r.bytes(shdr, kSampleHeaderBytes);
        if (!r.ok()) continue;
        if (shdr[kSampleTypeOffset] <= kSampleTypePcm) {
            if (read_u32le(shdr + kSampleLengthOffset) != 0) any_samples = true;
            gus_addresses |= read_u16le(shdr + kSampleGusAddressOffset);
        }

        RawSampleInfo ri;
        ri.sample_type         = shdr[kSampleTypeOffset];
        ri.data_offset         = sample_data_offset(shdr);
        ri.length_samples      = read_u32le(shdr + kSampleLengthOffset);
        const uint8_t volume   = shdr[kSampleVolumeOffset];
        ri.pack                = shdr[kSamplePackOffset];
        const uint8_t flags    = shdr[kSampleFlagsOffset];
        const bool loop_flag   = (flags & kSampleLoop) != 0;
        ri.is_stereo           = (flags & kSampleStereo) != 0;
        ri.is16bit             = (flags & kSample16Bit) != 0;
        const uint32_t c5speed = read_u32le(shdr + kSampleC5SpeedOffset);

        if (ri.sample_type != kSampleTypePcm) { // AdLib/FM - слот пустой
            ri.length_samples = 0;
            continue;
        }
        const uint32_t bytes_per_frame = (ri.is16bit ? 2u : 1u) * (ri.is_stereo ? 2u : 1u);
        const uint32_t length_bytes    = ri.length_samples * bytes_per_frame;
        if (ri.pack == 0 && ri.data_offset + length_bytes > file_size) {
            ri.length_samples = 0; // заявленная длина не влезает в файл: битый сэмпл
            ++soundsinth::model::g_tracker_load_stats.samples_dropped;
            continue;
        }

        sd.encoding = (ri.pack != 0) ? SampleEncoding::S3mAdpcm4 : (ri.is16bit ? SampleEncoding::Pcm16 : SampleEncoding::Pcm8);
        // Стерео берётся левым каналом, как у IT: S3M хранит каналы
        // раздельно, левый первым, и первые length_samples значений -
        // готовое моно.
        sd.channels       = 1;
        sd.length_samples = ri.length_samples;
        sd.loop_start     = read_u32le(shdr + kSampleLoopStartOffset);
        uint32_t loop_end = read_u32le(shdr + kSampleLoopEndOffset);
        if (loop_end > sd.length_samples) loop_end = sd.length_samples;
        sd.loop_end           = loop_end;
        sd.loop_enabled       = loop_flag && loop_end > sd.loop_start;
        sd.loop_bidirectional = false; // у S3M нет ping-pong петли (в отличие от XM/IT)
        sd.file_offset        = ri.data_offset;
        sd.signed_pcm         = (ffi != 2); // ffi - на весь файл
        // c5 у ST3 не выше 65535, у всех не ниже 1024 - как у OpenMPT.
        const uint32_t c5_limited = (genuine_st3 && c5speed > 0xffff) ? 0xffff : c5speed;
        sd.c5_speed               = (c5_limited == 0) ? 8363 : std::max<uint32_t>(c5_limited, 1024);
        sd.relative_note          = 0;
        sd.finetune               = 0; // у S3M нет поля finetune, питч целиком из c5speed

        // Кодек и прореживание - после паттернов (choose_resident_encoding).
        sd.source_length_samples = ri.length_samples;
        sd.default_volume        = (volume > kVolumeMax) ? kVolumeMax : volume;
        sd.default_panning       = -1; // панорама в S3M - свойство канала, не сэмпла

        Instrument& ins           = instruments[i];
        ins.global_volume         = kGlobalVolumeMax;
        ins.default_sample_index  = static_cast<uint16_t>(i);
        ins.note_to_sample_ranges = nullptr; // S3M: инструмент == сэмпл, как MOD
    }

    if (genuine_st3 && !(any_samples && gus_addresses == 0 && cwtv != kCwtvSt300)) {
        out.quirks |= soundsinth::model::kQuirkS3mIgnoreCombinedFineSlides;
    }

    // --- Паттерны ---
    PatternCell cells[kMaxChannels];
    for (uint32_t p = 0; p < pattern_count; ++p) {
        // Упаковщику берётся всё свободное, но не больше нужного худшему
        // паттерну архива: буфер и арена - один пул, и на файле с большой
        // ареной упаковщик обходится остатком.
        uint32_t pack_bytes = 0;
        uint8_t* pack_buf   = memory::scratch_take_upto(mem.scratch, memory::Scratch::PatternPack, memory::kPatternPackBufferBytes, pack_bytes);
        if (!pack_buf) return fail("resident memory overflowed (packer buffer)");
        patterns::PatternPacker packer(pack_buf, pack_bytes, kRowsPerPattern, channel_count);
        const uint32_t pp        = patterns[p].psram_offset; // парапоинтер из таблицы
        patterns[p].psram_offset = Pattern::kInvalidOffset;

        if (pp == 0) {
            // Пустой паттерн - законная дырка в списке паттернов.
            for (uint32_t ch = 0; ch < channel_count; ++ch) {
                cells[ch] = PatternCell{};
            }
            for (uint32_t row = 0; row < kRowsPerPattern; ++row) {
                if (!packer.add_row(cells)) return fail("pattern does not fit the packer buffer");
            }
        } else {
            // +2: поле длины упакованных данных пропускается, ему не доверяют (как OpenMPT).
            if (!r.seek(pp * kParagraphBytes + 2)) return fail("file truncated inside pattern data");
            for (uint32_t row = 0; row < kRowsPerPattern; ++row) {
                if (!read_packed_row(r, channel_count, dropped_channels, cells)) return fail("file truncated inside pattern data");
                if (!packer.add_row(cells)) return fail("pattern does not fit the packer buffer");
            }
        }

        const uint32_t offset = packer.finish(mem.psram);
        memory::scratch_leave(mem.scratch, memory::Scratch::PatternPack);
        if (offset == memory::kPatternAllocFailed) return fail("PSRAM pattern area overflowed");
        patterns[p].row_count     = kRowsPerPattern;
        patterns[p].channel_count = channel_count;
        patterns[p].psram_offset  = offset;
    }

    // Паттерны упакованы, объём известен точно, остаток PSRAM отдаётся
    // сэмплам. Строго после упаковки паттернов и до первого сэмпла: если
    // раньше, паттерны дорастают поверх зоны сэмплов.
    memory::psram_freeze_pattern_zone(mem.psram);
    // Кодек по точной сумме страниц всех сэмплов.
    soundsinth::model::choose_resident_encoding(out, memory::psram_free_page_count(mem.psram));

    // --- Сэмплы: перепаковка в Raw8/Dpcm8/Raw16, прореживание больших
    // высокочастотных 16-битных сэмплов. ---
    soundsinth::model::count_nonresident_samples(out);
    soundsinth::model::note_source_end(out);
    // metadata_only: PCM вытянет load_sample_pcm() позже, по одному.
    if (metadata_only) return true;
    for (uint32_t i = 0; i < sample_count; ++i) {
        const SampleDescriptor& sd = out.samples[i];
        if (!soundsinth::model::sample_is_resident(sd)) continue;
        // Свой reader на сэмпл: обрыв одного сэмпла не гасит остальные.
        BinaryReader sample_reader(src);
        const char* reason = nullptr;
        if (!soundsinth::model::pack_file_pcm(mem, sample_reader, sd, static_cast<uint16_t>(i), &reason)) {
            soundsinth::model::note_sample_failed(static_cast<uint16_t>(i), reason);
        }
    }

    return true;
}

bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song, uint16_t sample_index, const char** reason_out) {
    if (sample_index >= song.sample_count) {
        if (reason_out) *reason_out = "sample number outside the song";
        return false;
    }
    BinaryReader r(src);
    return soundsinth::model::pack_file_pcm(mem, r, song.samples[sample_index], sample_index, reason_out);
}

} // namespace soundsinth::formats::s3m
