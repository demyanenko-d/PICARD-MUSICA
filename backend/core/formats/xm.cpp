#include "core/formats/xm.h"

#include <algorithm>
#include <cstring>

#include "core/formats/load_stats.h"
#include "core/codec/loop_unroll.h"
#include "core/codec/pack_file_pcm.h"
#include "core/formats/binary_reader.h"
#include "core/codec/pattern_packer.h"

namespace soundsinth::formats::xm {

namespace {

using formats::BinaryReader;
using soundsinth::model::Effect;
using soundsinth::model::Envelope;
using soundsinth::model::EnvelopePoint;
using soundsinth::model::Instrument;
using soundsinth::model::KeymapRange;
using soundsinth::model::Pattern;
using soundsinth::model::PatternCell;
using soundsinth::model::SampleDescriptor;
using soundsinth::model::SampleEncoding;
using soundsinth::model::SlideRate;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

constexpr uint32_t kFileHeaderOffset = 60;  // за sig[17]+songName[20]+eof(1)+trackerName[20]+version(2)
constexpr uint32_t kFileHeaderFixedBytes = 20; // headerSize(4) + orders..tempo (8 полей по 2 байта)
// Спецификация FT2 - 32 канала, но расширенный XM (OpenMPT и другие)
// пишет до 64 в том же 16-битном поле, в архиве модулей есть файлы с 34-64
// каналами. PatternPacker рассчитан на маску до 64 каналов.
constexpr uint8_t kMaxChannels = 64;
static_assert(kMaxChannels <= soundsinth::model::kMaxPatternChannels, "маска строки упаковщика - 64 канала");
constexpr uint32_t kInstrumentHeaderMinBytes = 29; // size(4)+name(22)+type(1)+numSamples(2)
// За 29 байтами - sampleHeaderSize (4 байта), за ним XMInstrument: без
// пропуска флаги огибающих и keymap сдвинуты.
constexpr uint32_t kSampleHeaderSizeFieldBytes = 4;
constexpr uint32_t kInstrumentStructBytes = 230;  // XMInstrument: sampleMap, огибающие и прочее
constexpr uint32_t kSampleHeaderBytes = 40;        // XMSample
constexpr uint32_t kSampleMapKeys = 96;            // клавиш в sampleMap
constexpr uint8_t kEnvelopePoints = 12;            // точек огибающей, в таблице по два слова на точку
constexpr uint8_t kEnvEnabled = 0x01u;              // биты флагов огибающей
constexpr uint8_t kEnvSustain = 0x02u;
constexpr uint8_t kEnvLoop = 0x04u;

// Эффекты XM: 0-9, затем A-Z = 10-35, как у OpenMPT; E-семья та же, что у
// MOD.
constexpr uint8_t fx(char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'A' + 10); }

void decode_effect(uint8_t command, uint8_t param, soundsinth::model::EffectCommand& out) {
    switch (command) {
        case fx('0'): out.type = Effect::Arpeggio; out.param = param; break;
        case fx('1'): out.type = Effect::PortaUp; out.param = param; break;
        case fx('2'): out.type = Effect::PortaDown; out.param = param; break;
        case fx('3'): out.type = Effect::TonePorta; out.param = param; break;
        case fx('4'): out.type = Effect::Vibrato; out.param = param; break;
        case fx('5'): out.type = Effect::TonePortaVolSlide; out.param = param; break;
        case fx('6'): out.type = Effect::VibratoVolSlide; out.param = param; break;
        case fx('7'): out.type = Effect::Tremolo; out.param = param; break;
        case fx('8'): out.type = Effect::SetPanning; out.param = param; break;
        case fx('9'): out.type = Effect::SampleOffset; out.param = param; break;
        case fx('A'): out.type = Effect::VolumeSlide; out.param = param; break;
        case fx('B'): out.type = Effect::PositionJump; out.param = param; break;
        case fx('C'): out.type = Effect::SetVolume; out.param = (param > 64) ? 64 : param; break;
        case fx('D'): {
            const uint8_t row = static_cast<uint8_t>((param >> 4) * 10 + (param & 0x0fu)); // BCD, как у MOD/S3M
            out.type = Effect::PatternBreak;
            out.param = row; // до 165, у XM 63 не предел
            break;
        }
        case fx('E'): { // та же семья, что у MOD
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
                case 0x8: out.type = Effect::SetPanning4Bit; out.param = sub_param; break; // FT2 его не знает, OpenMPT - панорама, как S8x
                case 0x9: out.type = Effect::Retrigger; out.param = sub_param; break;
                case 0xA: out.type = Effect::VolumeSlide; out.rate = SlideRate::Fine; out.param = static_cast<uint8_t>(sub_param << 4); break;
                case 0xB: out.type = Effect::VolumeSlide; out.rate = SlideRate::Fine; out.param = sub_param; break;
                case 0xC: out.type = Effect::NoteCut; out.param = sub_param; break;
                case 0xD: out.type = Effect::NoteDelay; out.param = sub_param; break;
                case 0xE: out.type = Effect::PatternDelay; out.param = sub_param; break;
                default: ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped; break; // EFx не разбирается
            }
            break;
        }
        case fx('F'): out.type = (param < 0x20) ? Effect::SetSpeed : Effect::SetTempo; out.param = param; break;
        // Gxx/Hxy в XM - шкала 0..64, PlayState::global_volume - 0..128 (как
        // IT): Gxx умножается на 2 здесь, при загрузке, как и в S3M; Hxy - как
        // есть, шаг вдвое берёт диспетчер (ниблы x2 в байт не влезают).
        case fx('G'): out.type = Effect::SetGlobalVolume; out.param = static_cast<uint8_t>(param > 64 ? 128 : param * 2); break;
        case fx('H'): out.type = Effect::GlobalVolumeSlide; out.param = param; break;
        case fx('K'): out.type = Effect::KeyOff; out.param = param; break;
        case fx('L'): out.type = Effect::SetEnvelopePosition; out.param = param; break;
        case fx('P'): out.type = Effect::PanningSlide; out.param = param; break;
        case fx('R'): out.type = Effect::RetriggerXm; out.param = param; break;
        case fx('T'): out.type = Effect::Tremor; out.param = param; break;
        case fx('X'): { // сверхтонкое портаменто, X1x/X2x
            const uint8_t sub = param >> 4;
            const uint8_t sub_param = param & 0x0fu;
            if (sub == 1) { out.type = Effect::PortaUp; out.rate = SlideRate::ExtraFine; out.param = sub_param; }
            else if (sub == 2) { out.type = Effect::PortaDown; out.rate = SlideRate::ExtraFine; out.param = sub_param; }
            else { ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped; }
            break;
        }
        case fx('Y'): out.type = Effect::Panbrello; out.param = param; break;
        case fx('Z'): out.type = Effect::SetMidiMacro; out.param = param; break;
        default: ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped; break; // I, J, M, N, O, Q, S, U, V, W - в FT2 не задействованы, Effect::None
    }
}

// Колонка громкости XM, как у OpenMPT.
void decode_volume(uint8_t vol, soundsinth::model::VolumeColumnCommand& out) {
    if (vol >= 0x10 && vol <= 0x50) {
        out.type = VolumeColumnType::SetVolume;
        out.param = static_cast<uint8_t>(vol - 0x10);
        return;
    }
    if (vol < 0x60) return; // None
    const uint8_t idx = (vol - 0x60) >> 4;
    const uint8_t p = vol & 0x0fu;
    switch (idx) {
        case 0: out.type = VolumeColumnType::SlideDown; out.param = p; break;
        case 1: out.type = VolumeColumnType::SlideUp; out.param = p; break;
        case 2: out.type = VolumeColumnType::FineSlideDown; out.param = p; break;
        case 3: out.type = VolumeColumnType::FineSlideUp; out.param = p; break;
        case 4: out.type = VolumeColumnType::VibratoSpeed; out.param = p; break;
        case 5: out.type = VolumeColumnType::VibratoDepth; out.param = p; break;
        case 6: out.type = VolumeColumnType::SetPanning; out.param = static_cast<uint8_t>(p * 4); break; // 0x0..0xF -> 0..60, до 64 не доходит, как у FT2
        case 7: out.type = VolumeColumnType::PanSlideLeft; out.param = p; break;
        case 8: out.type = VolumeColumnType::PanSlideRight; out.param = p; break;
        case 9: out.type = VolumeColumnType::TonePorta; out.param = p; break;
        default: break;
    }
}

uint8_t decode_note(uint8_t raw) {
    if (raw == 97) return soundsinth::model::kNoteOff;
    if (raw >= 1 && raw <= 96) return static_cast<uint8_t>(raw - 1);
    return soundsinth::model::kNoteNone;
}

struct RawSampleHdr {
    uint32_t length_bytes, loop_start_bytes, loop_len_bytes;
    uint32_t file_offset; // начало PCM: подряд за заголовками сэмплов инструмента
    uint8_t vol;
    int8_t finetune;
    uint8_t flags;
    uint8_t pan;
    int8_t relnote;
    uint8_t reserved;
};

// Сжатие ModPlug-ADPCM: признак - байт reserved заголовка сэмпла 0xAD, и
// только у 8-битного моно. Данных у такого сэмпла в файле не length, а 16
// байт таблицы плюс по полбайта на отсчёт; шаг на length сбивает разбор всех
// следующих инструментов.
constexpr uint8_t kSampleAdpcm = 0xadu;

// Биты flags заголовка сэмпла.
constexpr uint8_t kSampleLoop = 0x01u;
constexpr uint8_t kSampleBidiLoop = 0x02u;
constexpr uint8_t kSample16Bit = 0x10u;
constexpr uint8_t kSampleStereo = 0x20u;

bool sample_is_adpcm(uint8_t flags, uint8_t reserved) {
    return reserved == kSampleAdpcm && (flags & (kSample16Bit | kSampleStereo)) == 0;
}

uint32_t sample_data_bytes(uint32_t length_bytes, bool adpcm) {
    return adpcm ? 16u + (length_bytes + 1u) / 2u : length_bytes;
}

uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Заголовок сэмпла XMSample: поля по смещениям 0, 4, 8, 12..17, имя - 18..39.
RawSampleHdr parse_sample_header(const uint8_t (&h)[kSampleHeaderBytes]) {
    RawSampleHdr rs{};
    rs.length_bytes = get_u32(h + 0);
    rs.loop_start_bytes = get_u32(h + 4);
    rs.loop_len_bytes = get_u32(h + 8);
    rs.vol = h[12];
    rs.finetune = static_cast<int8_t>(h[13]);
    rs.flags = h[14];
    rs.pan = h[15];
    rs.relnote = static_cast<int8_t>(h[16]);
    rs.reserved = h[17];
    return rs;
}

// Дескриптор из заголовка сэмпла. ft2_finetune_precision - младшие три бита
// finetune отброшены, как у FT2.
void fill_sample_descriptor(const RawSampleHdr& rs, bool ft2_finetune_precision, SampleDescriptor& sd) {
    const bool is16bit = (rs.flags & kSample16Bit) != 0;
    // Стерео (0x20): в файле левый канал, за ним правый, у каждого дельты с
    // нуля. Звучит левый, как у IT и S3M: длина и границы петли пополам.
    const uint32_t channel_div = (rs.flags & kSampleStereo) != 0 ? 2u : 1u;
    const uint32_t length_samples = (is16bit ? rs.length_bytes / 2 : rs.length_bytes) / channel_div;
    const uint32_t loop_start = is16bit ? rs.loop_start_bytes / 2 : rs.loop_start_bytes;
    const uint32_t loop_len = is16bit ? rs.loop_len_bytes / 2 : rs.loop_len_bytes;

    sd.encoding = is16bit ? SampleEncoding::XmDelta16 : SampleEncoding::XmDelta8;
    sd.channels = 1;
    sd.length_samples = length_samples;
    sd.loop_start = loop_start / channel_div;
    uint32_t loop_end = (loop_start + loop_len) / channel_div;
    if (loop_end > length_samples) loop_end = length_samples;
    sd.loop_end = loop_end;
    // Петлю включает любой из битов 0x01 и 0x02: 0x02 (ping-pong) бывает без
    // 0x01.
    sd.loop_enabled = (rs.flags & (kSampleLoop | kSampleBidiLoop)) != 0 && loop_end > sd.loop_start;
    sd.loop_bidirectional = (rs.flags & kSampleBidiLoop) != 0;
    sd.file_offset = rs.file_offset;
    sd.c5_speed = 8363; // поля c5speed у XM нет, питч через relative_note и finetune
    sd.relative_note = rs.relnote;
    sd.finetune = ft2_finetune_precision ? static_cast<int8_t>(rs.finetune & ~7) : rs.finetune;
    sd.default_volume = (rs.vol > 64) ? 64 : rs.vol;
    sd.default_panning = static_cast<int8_t>(rs.pan / 4); // 0..255 -> 0..63
    sd.unsupported_codec = sample_is_adpcm(rs.flags, rs.reserved);
    // Кодек и прореживание - после всех заголовков (choose_resident_encoding).
    sd.source_length_samples = length_samples;
}

// Ячейка паттерна: первый байт со старшим битом - маска полей, иначе это
// нота и за ней все четыре поля, как у OpenMPT.
constexpr uint8_t kCellPacked = 0x80u;
constexpr uint8_t kCellNote = 0x01u;
constexpr uint8_t kCellInstrument = 0x02u;
constexpr uint8_t kCellVolume = 0x04u;
constexpr uint8_t kCellCommand = 0x08u;
constexpr uint8_t kCellParam = 0x10u;

PatternCell read_cell(BinaryReader& r) {
    PatternCell cell;
    const uint8_t b0 = r.u8();
    const uint8_t fields = (b0 & kCellPacked) ? b0 : 0xffu;
    if (fields & kCellNote) cell.note = decode_note((b0 & kCellPacked) ? r.u8() : b0);
    if (fields & kCellInstrument) {
        const uint8_t ins = r.u8();
        cell.instrument = (ins == 0xff) ? 0 : ins;
    }
    if (fields & kCellVolume) decode_volume(r.u8(), cell.volume);
    const uint8_t command = (fields & kCellCommand) ? r.u8() : 0;
    const uint8_t param = (fields & kCellParam) ? r.u8() : 0;
    if (command | param) decode_effect(command, param, cell.effect);
    return cell;
}

constexpr bool name_has(const char* name, uint32_t len, const char* word, uint32_t word_len) {
    for (uint32_t i = 0; i + word_len <= len; ++i) {
        uint32_t k = 0;
        while (k < word_len && name[i + k] == word[k]) ++k;
        if (k == word_len) return true;
    }
    return false;
}

// Версия OpenMPT из имени трекера: "1.32.09.00" -> 0x01320900, по полю на
// байт, каждое поле читается как шестнадцатеричное; поле кончается точкой,
// любой другой символ кончает разбор.
constexpr uint32_t openmpt_version(const char* s, uint32_t len) {
    uint32_t version = 0;
    uint32_t value = 0;
    uint32_t field = 0;
    for (uint32_t i = 0; i <= len && field < 4; ++i) {
        const char c = (i < len) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            value = value * 16u + static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = value * 16u + static_cast<uint32_t>(c - 'a' + 10);
        } else {
            version = (version << 8) | (value & 0xffu);
            value = 0;
            ++field;
            if (c != '.') break;
        }
    }
    for (; field < 4; ++field) version <<= 8;
    return version;
}
constexpr uint32_t kOpenMptFt2Rules = 0x01220719u; // 1.22.07.19
static_assert(openmpt_version("1.22.07.19", 10) == kOpenMptFt2Rules, "разбор версии OpenMPT");

constexpr uint32_t kTrackerNameBytes = 20;

// Правила FT2 по имени трекера: закон панорамирования по корню (Song::PanLaw)
// - у FastTracker II и тех, кто за ним повторяет, ветки как у OpenMPT:
//   FastTracker II                   - всегда;
//   MilkyTracker с версией в строке  - с 0.90.87;
//   OpenMPT                          - с 1.22.07.19.
// Остальные (старые конвертеры, MilkyTracker без версии) - линейно.
// Младшие три бита finetune FT2 отбрасывает, OpenMPT повторяет это с
// 1.22.07.19.
struct TrackerRules {
    bool ft2 = false;              // квирки FT2
    bool ft2_pan = false;
    bool ft2_finetune_precision = true;
};

TrackerRules tracker_rules(const char (&name)[kTrackerNameBytes]) {
    TrackerRules t;
    t.ft2 = name_has(name, kTrackerNameBytes, "FastTracker", 11);
    t.ft2_pan = t.ft2;
    if (!t.ft2_pan && std::memcmp(name, "MilkyTracker", 12) == 0) {
        for (uint32_t i = 12; i < kTrackerNameBytes; ++i) {
            if (name[i] != ' ' && name[i] != 0) {
                t.ft2_pan = true;
                break;
            }
        }
    }
    if (!t.ft2_pan && std::memcmp(name, "OpenMPT ", 8) == 0) {
        t.ft2_pan = openmpt_version(name + 8, kTrackerNameBytes - 8) >= kOpenMptFt2Rules;
        t.ft2_finetune_precision = t.ft2_pan;
    }
    return t;
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

    // Заголовок файла (XMFileHeader) - сигнатура, затем остаток до таблицы
    // order, без прыжков.
    uint8_t hdr[kFileHeaderOffset + kFileHeaderFixedBytes];
    constexpr uint32_t kSignatureBytes = 17;
    r.bytes(hdr, kSignatureBytes);
    if (!r.ok() || std::memcmp(hdr, "Extended Module: ", kSignatureBytes) != 0) return fail("нет сигнатуры XM");
    r.bytes(hdr + kSignatureBytes, sizeof(hdr) - kSignatureBytes);
    if (!r.ok()) return fail("файл обрезан в заголовке");
    std::memcpy(out.title, hdr + 17, 20);
    char tracker_name[kTrackerNameBytes];
    std::memcpy(tracker_name, hdr + 38, kTrackerNameBytes);

    const uint16_t xm_version = get_u16(hdr + 58);
    if (xm_version < 0x0104) return fail("старые XM 1.02/1.03 (8-байтный заголовок паттерна) не поддерживаются");
    const uint32_t header_size = get_u32(hdr + 60);
    const uint16_t order_count = get_u16(hdr + 64);
    const uint16_t restart_raw = get_u16(hdr + 66); // restartPos: с какой позиции order играть после конца
    const uint16_t channel_count_raw = get_u16(hdr + 68);
    const uint16_t pattern_count = get_u16(hdr + 70);
    const uint16_t instrument_count = get_u16(hdr + 72);
    const uint16_t header_flags = get_u16(hdr + 74);
    const uint16_t speed_raw = get_u16(hdr + 76);
    const uint16_t tempo_raw = get_u16(hdr + 78);
    if (channel_count_raw == 0 || channel_count_raw > kMaxChannels) return fail("число каналов вне поддерживаемого диапазона");
    if (order_count > 256 || pattern_count > 256 || instrument_count > 255) return fail("заголовок вне разумных пределов (не XM?)");
    const uint8_t channel_count = static_cast<uint8_t>(channel_count_raw);

    const TrackerRules tracker = tracker_rules(tracker_name);
    out.pan_law = tracker.ft2_pan ? soundsinth::model::Song::PanLaw::Ft2Sqrt : soundsinth::model::Song::PanLaw::Linear;
    const bool ft2_finetune_precision = tracker.ft2_finetune_precision;

    const uint32_t order_table_bytes = (header_size > kFileHeaderFixedBytes) ? (header_size - kFileHeaderFixedBytes) : 0;
    uint8_t order_bytes[256] = {};
    const uint32_t order_bytes_to_read = std::min<uint32_t>(order_table_bytes, sizeof(order_bytes));
    r.bytes(order_bytes, order_bytes_to_read);
    if (!r.ok()) return fail("файл обрезан в таблице воспроизведения");

    // --- Резидентные метаданные ---
    out.channel_count = channel_count;
    out.default_speed = (speed_raw == 0) ? soundsinth::model::kDefaultSpeed : speed_raw;
    out.default_tempo = (tempo_raw < soundsinth::model::kMinTempo) ? soundsinth::model::kDefaultTempo : tempo_raw;
    out.default_global_volume = 128; // у XM нет поля глобальной громкости в заголовке
    // Поля preamp у XM нет: 48, умолчание OpenMPT.
    out.sample_preamp = 48;
    out.frequency_model = (header_flags & 0x01u) ? soundsinth::model::FrequencyModel::Linear : soundsinth::model::FrequencyModel::Amiga;
    out.quirks = soundsinth::model::kQuirkXmVolColumnBeforeEffect; // свойство формата XM, от трекера не зависит
    // FT2 обрывает канал в обоих случаях: сэмпл пустой и инструмент не даёт
    // сэмпла на эту ноту.
    out.quirks |= soundsinth::model::kQuirkCutOnEmptySample | soundsinth::model::kQuirkCutOnUnmappedNote |
                  soundsinth::model::kQuirkGlissandoNearest;
    if (tracker.ft2) {
        out.quirks |= soundsinth::model::kQuirkXmFt2ArpeggioTable | soundsinth::model::kQuirkXmFt2SeparatePortaMemory |
                      soundsinth::model::kQuirkXmFt2OffsetMemoryOnActivate | soundsinth::model::kQuirkXmFt2PanEnvelopeSustainBug |
                      soundsinth::model::kQuirkXmFt2E60RowClobber | soundsinth::model::kQuirkXmFt2TremoloUsesVibratoPos |
                      soundsinth::model::kQuirkXmFt2ZeroTremorStaysZero;
    }
    // Позиция повтора за концом order - 0, как у libxmp: иначе следующая
    // позиция не находится, песня обрывается.
    out.restart_position = restart_raw < order_count ? restart_raw : 0;

    out.order_count = order_count;
    if (order_count > 0) {
        auto* order = memory::arena_new<uint16_t>(mem.resident, order_count);
        if (!order) return fail("резидентная память переполнена (order)");
        for (uint32_t i = 0; i < order_count; ++i) {
            const uint8_t b = (i < order_bytes_to_read) ? order_bytes[i] : 0;
            // Своих маркеров skip/end у XM нет: номер вне диапазона - пропуск позиции, позиция не теряется.
            order[i] = (b < pattern_count) ? b : soundsinth::model::kOrderSkip;
        }
        out.order = order;
    }

    out.pattern_count = pattern_count;
    Pattern* patterns = nullptr;
    if (pattern_count > 0) {
        patterns = memory::arena_new<Pattern>(mem.resident, pattern_count);
        if (!patterns) return fail("резидентная память переполнена (patterns)");
        out.patterns = patterns;
    }

    out.sample_count = 0;      // в файле общего счётчика нет, считается при чтении инструментов
    out.instrument_count = instrument_count;

    Instrument* instruments = nullptr;
    if (instrument_count > 0) {
        instruments = memory::arena_new<Instrument>(mem.resident, instrument_count);
        if (!instruments) return fail("резидентная память переполнена (instruments)");
        out.instruments = instruments;
    }

    // --- Паттерны (в файле до инструментов и сэмплов) ---
    PatternCell cells[kMaxChannels];
    uint32_t pos = kFileHeaderOffset + header_size;
    for (uint32_t p = 0; p < pattern_count; ++p) {
        if (!r.seek(pos)) return fail("файл обрезан в заголовке паттерна");
        const uint32_t pat_header_len = r.u32();
        r.skip(1); // packing_type, на практике всегда 0
        const uint16_t row_count = r.u16();
        const uint16_t packed_data_size = r.u16();
        if (!r.ok()) return fail("файл обрезан в заголовке паттерна");
        if (pat_header_len < 9) return fail("заголовок паттерна короче ожидаемого (не должно происходить для версии >= 0x0104)");
        if (row_count == 0 || row_count > soundsinth::model::kMaxPatternRows) return fail("число строк паттерна вне диапазона спецификации XM (1..256)");

        const uint32_t data_start = pos + pat_header_len;
        if (!r.seek(data_start)) return fail("файл обрезан в данных паттерна");

        // Упаковщику берётся всё свободное, но не больше нужного худшему
        // паттерну архива: буфер и арена - один пул, и на файле с большой
        // ареной упаковщик обходится остатком.
        uint32_t pack_bytes = 0;
        uint8_t* pack_buf = memory::scratch_take_upto(mem.scratch, memory::Scratch::PatternPack,
                                                      memory::kPatternPackBufferBytes, pack_bytes);
        if (!pack_buf) return fail("резидентная память переполнена (буфер упаковщика)");
        patterns::PatternPacker packer(pack_buf, pack_bytes, row_count, channel_count);
        // Паттерн без данных - заданное число пустых строк.
        for (uint32_t row = 0; row < row_count; ++row) {
            for (uint32_t ch = 0; ch < channel_count; ++ch) {
                cells[ch] = packed_data_size != 0 ? read_cell(r) : PatternCell{};
            }
            if (!r.ok()) return fail("файл обрезан в данных паттерна");
            if (!packer.add_row(cells)) return fail("паттерн не влезает в буфер упаковщика");
        }

        const uint32_t offset = packer.finish(mem.psram);
        memory::scratch_leave(mem.scratch, memory::Scratch::PatternPack);
        if (offset == memory::kPatternAllocFailed) return fail("зона паттернов PSRAM переполнена");
        patterns[p].row_count = row_count;
        patterns[p].channel_count = channel_count;
        patterns[p].psram_offset = offset;

        pos = data_start + packed_data_size;
    }
    const uint32_t instruments_pos = pos;

    // Паттерны упакованы: зона сэмплов сдвигается вплотную к ним. Паттерн
    // после этого вызова лёг бы поверх сэмплов.
    memory::psram_freeze_pattern_zone(mem.psram);

    // --- Инструменты и сэмплы (после всех паттернов), один проход ---
    // Общего числа сэмплов в файле нет: заголовки сэмплов собираются в
    // буфер сценариев (паттерны упакованы, до первого PCM он свободен),
    // дескрипторы - после цикла. PCM пропускается вперёд по длинам из
    // заголовков, назад загрузчик не ходит.
    uint8_t* hdr_buf =
        memory::scratch_take(mem.scratch, memory::Scratch::XmSampleHdrs, memory::kPatternPackBufferBytes);
    if (!hdr_buf) return fail("резидентная память переполнена (заголовки сэмплов)");
    auto* all_raw = reinterpret_cast<RawSampleHdr*>(hdr_buf);
    constexpr uint32_t kMaxRawSamples = memory::kPatternPackBufferBytes / sizeof(RawSampleHdr);
    static_assert(alignof(RawSampleHdr) <= 8, "буфер сценариев выровнен на 8");
    uint32_t total_samples = 0;
    uint32_t inst_start = instruments_pos;
    for (uint32_t i = 0; i < instrument_count; ++i) {
        Instrument& ins = instruments[i];
        if (!r.seek(inst_start) || inst_start + kInstrumentHeaderMinBytes > r.size()) {
            return fail("файл обрезан в заголовке инструмента");
        }
        const uint32_t inst_size = r.u32();
        r.skip(22 + 1); // name, type
        const uint16_t num_samples = r.u16();
        if (!r.ok() || inst_size < kInstrumentHeaderMinBytes) return fail("заголовок инструмента повреждён");
        const uint32_t sample_headers_pos = inst_start + inst_size;

        if (num_samples == 0) {
            // Инструмент без сэмплов - не "сэмпл 0", иначе его ноты
            // играли бы первый сэмпл файла.
            ins.default_sample_index = soundsinth::model::kNoSample;
            ins.note_to_sample_ranges = nullptr;
            inst_start = sample_headers_pos;
            continue;
        }
        if (sample_headers_pos + static_cast<uint64_t>(num_samples) * kSampleHeaderBytes > r.size()) {
            return fail("файл обрезан в заголовках сэмплов инструмента");
        }
        if (total_samples + num_samples > kMaxRawSamples) return fail("файл слишком велик для буфера заголовков сэмплов");
        const uint32_t global_sample_base = total_samples;

        const uint32_t instrument_struct_start = inst_start + kInstrumentHeaderMinBytes + kSampleHeaderSizeFieldBytes;
        uint8_t local_sample_map[kSampleMapKeys] = {};
        if (instrument_struct_start + kInstrumentStructBytes <= sample_headers_pos) {
            r.seek(instrument_struct_start);
            r.bytes(local_sample_map, kSampleMapKeys);

            uint16_t vol_env_raw[kEnvelopePoints * 2];
            for (auto& v : vol_env_raw) {
                v = r.u16();
            }
            uint16_t pan_env_raw[kEnvelopePoints * 2];
            for (auto& v : pan_env_raw) {
                v = r.u16();
            }
            const uint8_t vol_points = r.u8();
            const uint8_t pan_points = r.u8();
            const uint8_t vol_sustain = r.u8();
            const uint8_t vol_loop_start = r.u8();
            const uint8_t vol_loop_end = r.u8();
            const uint8_t pan_sustain = r.u8();
            const uint8_t pan_loop_start = r.u8();
            const uint8_t pan_loop_end = r.u8();
            const uint8_t vol_flags = r.u8();
            const uint8_t pan_flags = r.u8();
            r.skip(2); // автовибрато инструмента не воспроизводится: vibType, vibSweep
            if (r.u8() != 0) ++soundsinth::model::g_tracker_load_stats.ignored_autovibrato; // vibDepth
            r.skip(1); // vibRate
            const uint16_t vol_fade = r.u16();
            if (!r.ok()) return fail("файл обрезан в расширенном заголовке инструмента");

            ins.fadeout_rate = static_cast<uint32_t>(vol_fade) << 1; // как у libxmp
            ins.global_volume = 128;

            // Огибающая XM - не больше 12 точек: в заголовке две таблицы по 24
            // слова. Число точек больше 12 бывает только в битых файлах, и
            // читать дальше - за концом таблиц. Правила как у OpenMPT: точки
            // режутся до 12, sustain - только на существующем номере, петля -
            // только если её конец не за таблицей и не раньше начала, пустая
            // огибающая не включается.
            const auto read_envelope = [&](uint8_t env_flags, uint8_t points, uint8_t sustain, uint8_t loop_start,
                                           uint8_t loop_end, const uint16_t (&raw)[kEnvelopePoints * 2]) -> Envelope* {
                auto* env = memory::arena_new<Envelope>(mem.resident);
                if (!env) return nullptr;
                Envelope& e = *env;
                e.point_count = (points > kEnvelopePoints) ? kEnvelopePoints : points;
                e.enabled = e.point_count > 0;
                e.sustain_enabled = (env_flags & kEnvSustain) != 0 && sustain < kEnvelopePoints;
                e.loop_enabled = (env_flags & kEnvLoop) != 0 && loop_end < kEnvelopePoints && loop_end >= loop_start;
                e.sustain_point = sustain;
                e.sustain_end = sustain; // XM: одна sustain-точка, не диапазон
                e.loop_start = loop_start;
                e.loop_end = loop_end;
                for (uint32_t pt = 0; pt < e.point_count; ++pt) {
                    e.points[pt] = EnvelopePoint{raw[pt * 2], static_cast<int16_t>(raw[pt * 2 + 1])};
                }
                soundsinth::model::sanitize_tracker_envelope(e);
                return env;
            };
            // Включённая огибающая без точек - как выключенная (у OpenMPT и libxmp):
            // иначе панорама уходила к краю, а Key-Off вместо обрыва становился
            // отпусканием.
            if ((vol_flags & kEnvEnabled) && vol_points > 0) {
                ins.volume_envelope = read_envelope(vol_flags, vol_points, vol_sustain, vol_loop_start, vol_loop_end,
                                                    vol_env_raw);
                if (!ins.volume_envelope) return fail("резидентная память переполнена (volume envelope)");
            }
            if ((pan_flags & kEnvEnabled) && pan_points > 0) {
                ins.panning_envelope = read_envelope(pan_flags, pan_points, pan_sustain, pan_loop_start, pan_loop_end,
                                                     pan_env_raw);
                if (!ins.panning_envelope) return fail("резидентная память переполнена (panning envelope)");
            }
        }
        ins.pitch_envelope = nullptr; // у XM огибающей питча нет

        // --- Заголовки сэмплов этого инструмента, PCM - подряд за ними ---
        r.seek(sample_headers_pos);
        RawSampleHdr* raw = all_raw + global_sample_base;
        uint32_t pcm_pos = sample_headers_pos + static_cast<uint32_t>(num_samples) * kSampleHeaderBytes;
        for (uint32_t s = 0; s < num_samples; ++s) {
            uint8_t h[kSampleHeaderBytes];
            r.bytes(h, sizeof(h));
            RawSampleHdr& rs = raw[s];
            rs = parse_sample_header(h);
            rs.file_offset = pcm_pos;
            pcm_pos += sample_data_bytes(rs.length_bytes, sample_is_adpcm(rs.flags, rs.reserved));
        }
        if (!r.ok()) return fail("файл обрезан в заголовках сэмплов");

        // --- Keymap: локальный номер (0..num_samples-1) -> глобальный.
        // Keymap заводится, только если в sampleMap встречаются хотя бы
        // два разных сэмпла. ---
        uint16_t first_distinct = static_cast<uint16_t>(global_sample_base);
        bool have_first = false;
        bool has_multiple = false;
        for (uint8_t local_idx : local_sample_map) {
            if (local_idx >= num_samples) continue;
            const uint16_t global_idx = static_cast<uint16_t>(global_sample_base + local_idx);
            if (!have_first) { first_distinct = global_idx; have_first = true; }
            else if (global_idx != first_distinct) { has_multiple = true; break; }
        }

        if (has_multiple) {
            // Диапазоны (KeymapRange) ради места в арене, как у IT;
            // у XM note_offset всегда 0 (питч через
            // SampleDescriptor::relative_note), диапазоны режутся
            // только по смене сэмпла.
            auto sample_for_note = [&](uint32_t n) -> uint16_t {
                const uint8_t local_idx = (n < kSampleMapKeys) ? local_sample_map[n] : 0;
                return (local_idx < num_samples) ? static_cast<uint16_t>(global_sample_base + local_idx)
                                                  : static_cast<uint16_t>(global_sample_base);
            };
            uint32_t range_count = 0;
            uint16_t prev_idx = 0xffff;
            for (uint32_t n = 0; n < soundsinth::model::kNoteMapSize; ++n) {
                const uint16_t idx = sample_for_note(n);
                if (n == 0 || idx != prev_idx) ++range_count;
                prev_idx = idx;
            }
            auto* ranges = memory::arena_new<KeymapRange>(mem.resident, range_count);
            if (!ranges) return fail("резидентная память переполнена (keymap)");
            uint32_t out_idx = 0;
            prev_idx = 0xffff;
            for (uint32_t n = 0; n < soundsinth::model::kNoteMapSize; ++n) {
                const uint16_t idx = sample_for_note(n);
                if (n == 0 || idx != prev_idx) {
                    ranges[out_idx].start_note = static_cast<uint8_t>(n);
                    ranges[out_idx].sample_index = idx;
                    ranges[out_idx].note_offset = 0;
                    ++out_idx;
                }
                prev_idx = idx;
            }
            ins.note_to_sample_ranges = ranges;
            ins.note_to_sample_range_count = static_cast<uint8_t>(range_count);
            // При заданном keymap движок индекс не использует, но он валидный.
            ins.default_sample_index = static_cast<uint16_t>(global_sample_base);
        } else {
            ins.note_to_sample_ranges = nullptr;
            ins.default_sample_index = first_distinct;
        }

        total_samples += num_samples;
        inst_start = pcm_pos;
    }

    out.sample_count = static_cast<uint16_t>(total_samples);
    SampleDescriptor* samples = nullptr;
    if (total_samples > 0) {
        samples = memory::arena_new<SampleDescriptor>(mem.resident, total_samples);
        if (!samples) return fail("резидентная память переполнена (samples)");
        out.samples = samples;
    }
    for (uint32_t i = 0; i < total_samples; ++i) {
        fill_sample_descriptor(all_raw[i], ft2_finetune_precision, samples[i]);
    }
    // Сырые заголовки переписаны в дескрипторы: место возвращается арене
    // до первого PCM, дальше тот же буфер берёт перепаковка.
    memory::scratch_leave(mem.scratch, memory::Scratch::XmSampleHdrs);

    // PCM - после всех заголовков: кодек по точной сумме страниц и разворот
    // ping-pong петель решаются по всем сэмплам сразу.
    const uint32_t free_pages = memory::psram_free_page_count(mem.psram);
    soundsinth::model::choose_resident_encoding(out, free_pages);
    soundsinth::model::unroll_pingpong_loops(out, free_pages, soundsinth::model::LoopUnroll::Xm);
    soundsinth::model::count_nonresident_samples(out);
    soundsinth::model::note_source_end(out);
    // metadata_only: PCM вытянет load_sample_pcm() позже, по одному.
    if (metadata_only) return true;
    for (uint32_t i = 0; i < total_samples; ++i) {
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

bool load_sample_pcm(formats::ByteSource src, memory::TrackMemory& mem, const soundsinth::model::Song& song,
                      uint16_t sample_index, const char** reason_out) {
    if (sample_index >= song.sample_count) {
        if (reason_out) *reason_out = "номер сэмпла вне песни";
        return false;
    }
    BinaryReader r(src);
    return soundsinth::model::pack_file_pcm(mem, r, song.samples[sample_index], sample_index, reason_out);
}

} // namespace soundsinth::formats::xm
