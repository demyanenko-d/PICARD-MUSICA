// SPDX-License-Identifier: MIT
#include "core/formats/it.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform/compiler.h"
#include "core/formats/load_stats.h"
#include "core/codec/loop_unroll.h"
#include "core/codec/pack_file_pcm.h"
#include "core/formats/it_decompress.h"
#include "core/formats/binary_reader.h"
#include "core/memory/scratch_arena.h"
#include "core/codec/pattern_packer.h"
#include "core/codec/sample_pack.h"

namespace soundsinth::formats::it {

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
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

constexpr uint32_t kOrderTableOffset         = 192; // заголовок файла, 192 байта
constexpr uint32_t kSampleHeaderBytes        = 80;  // заголовок сэмпла IMPS
constexpr uint32_t kInstrumentHeaderBytes    = 554; // заголовок инструмента IMPI (новый формат)
constexpr uint32_t kInstrumentExtMagicOffset = 550; // метка расширения keymap
constexpr uint32_t kKeymapSlots              = 120; // клавиш в note-sample таблице
static_assert(kKeymapSlots == soundsinth::model::kNoteMapSize, "an IT keymap key is an engine note");
constexpr uint32_t kKeyboardOffset = 64;  // от начала IMPI
constexpr uint32_t kVolEnvOffset   = 304; // огибающая громкости
constexpr uint32_t kPanEnvOffset   = 386;
constexpr uint32_t kPitchEnvOffset = 468;
// Узлы огибающей старого формата: 64 + 240 (keyboard) + 200 (предрасчитанная огибающая).
constexpr uint32_t kOldEnvNodesOffset = 504;
static_assert(kOldEnvNodesOffset + 2 * soundsinth::model::kMaxEnvelopePoints <= kInstrumentHeaderBytes, "the nodes are within IMPI");
// IFC/IFR внутри IMPI - начальные срез и резонанс фильтра, смещения
// 0x3A/0x3B от начала инструмента.
constexpr uint32_t kInstrumentFilterCutoffOffset    = 0x3a;
constexpr uint32_t kInstrumentFilterResonanceOffset = 0x3b;
constexpr uint32_t kEnvelopeBytes                   = 82; // огибающая
// Каналов у IT не больше 64 (ChnPan[64] и ChnVol[64]); поле номера в паттерне 7-битное.
constexpr uint8_t kMaxChannels = 64;
static_assert(kMaxChannels <= soundsinth::model::kMaxPatternChannels, "the packer row mask is 64 channels");
constexpr uint32_t kChnPanOffset = 0x40; // ChnPan[64], за ним ChnVol[64]
constexpr uint32_t kChnVolOffset = 0x80;
static_assert(kChnVolOffset == kChnPanOffset + kMaxChannels, "ChnVol immediately after ChnPan");
constexpr uint8_t kChannelMuted            = 0x80u;   // бит ChnPan
constexpr uint16_t kHdrExtendedFilterRange = 0x1000u; // бит flags заголовка
constexpr uint8_t kCvtSignedSample         = 0x01u;
constexpr uint8_t kCvtDelta                = 0x04u; // у сжатого - вариант IT 2.15
constexpr uint8_t kSampleSustainLoop       = 0x20u; // бит flags сэмпла: петля удержания
constexpr uint32_t kSampleVibDepthOffset   = 77;    // ViD - глубина автовибрато сэмпла

// Буквы A-Z те же, что у S3M, кроме: Pattern Break не в BCD, Vxx/Wxy уже в
// шкале 0..128. Из расширений OpenMPT 27-31 разбирается только 28
// (SmoothMidiMacro).
void decode_effect(uint8_t command, uint8_t param, soundsinth::model::EffectCommand& out) {
    switch (command + '@') {
        case 'A':
            out.type  = Effect::SetSpeed;
            out.param = param;
            break;
        case 'B':
            out.type  = Effect::PositionJump;
            out.param = param;
            break;
        case 'C':
            out.type  = Effect::PatternBreak;
            out.param = param;
            break; // IT: не BCD
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
        case 'S': { // расширенная Sxx-семья, как у S3M
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
                // S8x - панорама 4 битами (0..15), Effect::SetPanning4Bit.
                case 0x8:
                    out.type  = Effect::SetPanning4Bit;
                    out.param = sub_param;
                    break;
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
                    break; // S0x, S7x не разбираются
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
        case 'V':
            out.type  = Effect::SetGlobalVolume;
            out.param = param;
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
        case '\\':
            out.type  = Effect::SmoothMidiMacro;
            out.param = param;
            break;
        default: // '[', ']', '^', '_' (расширения OpenMPT XParam, DelayCut, Finetune) не разбираются
            if (command != 0) ++soundsinth::model::g_tracker_load_stats.effect_cells_dropped;
            break;
    }
}

// Раскладка байта громкости/панорамы IT (mask & 0x04), сверено с OpenMPT.
void decode_volume(uint8_t vol, soundsinth::model::VolumeColumnCommand& out) {
    if (vol <= 64) {
        out.type  = VolumeColumnType::SetVolume;
        out.param = vol;
        return;
    }
    if (vol >= 128 && vol <= 192) {
        out.type  = VolumeColumnType::SetPanning;
        out.param = static_cast<uint8_t>(vol - 128);
        return;
    }
    if (vol < 75) {
        out.type  = VolumeColumnType::FineSlideUp;
        out.param = static_cast<uint8_t>(vol - 65);
        return;
    }
    if (vol < 85) {
        out.type  = VolumeColumnType::FineSlideDown;
        out.param = static_cast<uint8_t>(vol - 75);
        return;
    }
    if (vol < 95) {
        out.type  = VolumeColumnType::SlideUp;
        out.param = static_cast<uint8_t>(vol - 85);
        return;
    }
    if (vol < 105) {
        out.type  = VolumeColumnType::SlideDown;
        out.param = static_cast<uint8_t>(vol - 95);
        return;
    }
    if (vol < 115) {
        out.type  = VolumeColumnType::PortamentoDown;
        out.param = static_cast<uint8_t>(vol - 105);
        return;
    }
    if (vol < 125) {
        out.type  = VolumeColumnType::PortamentoUp;
        out.param = static_cast<uint8_t>(vol - 115);
        return;
    }
    if (vol >= 193 && vol <= 202) {
        out.type  = VolumeColumnType::TonePorta;
        out.param = static_cast<uint8_t>(vol - 193);
        return;
    }
    if (vol >= 203 && vol <= 212) {
        out.type  = VolumeColumnType::VibratoDepth;
        out.param = static_cast<uint8_t>(vol - 203);
        return;
    }
    if (vol >= 223 && vol <= 232) {
        out.type  = VolumeColumnType::Offset;
        out.param = static_cast<uint8_t>(vol - 223);
        return;
    }
    // остальное (213-222, 125-127, 233-255) - None
}

// Ноты 120..0xFC - затухание (^^^), 0xFD - пусто, как у OpenMPT для IT.
uint8_t decode_note(uint8_t raw) {
    if (raw == 0xff) return soundsinth::model::kNoteOff;
    if (raw == 0xfe) return soundsinth::model::kNoteCut;
    if (raw == 0xfd) return soundsinth::model::kNoteNone;
    if (raw < 120) return raw; // у IT нота уже 0..119
    return soundsinth::model::kNoteFade;
}

struct RawSampleInfo {
    uint32_t file_offset = 0;
    bool is16bit         = false;
    bool is_stereo       = false;
    bool is_compressed   = false;
    bool is215           = false;
    bool signed_pcm      = true;
};

// Сжатый блок читается только вперёд, хватает окна 8 КБ. Окно подливается
// до шага, а не по нехватке.
constexpr uint32_t kWindowBytes = 8192;
static_assert(kStepWorstBytes < kWindowBytes, "the step must fit the window");

// Один сжатый блок: block_bytes байт потока с текущей позиции источника -
// до max_samples отсчётов в упаковщик, written - сколько отдано. Блок,
// которому не хватило данных, отдаёт то, что успел, - true. false - источник
// оборвался или PSRAM кончилась, причина в reason_out.
bool unpack_it_block(BinaryReader& r, uint32_t block_bytes, uint32_t max_samples, bool is16bit, bool is215, uint8_t* window, int16_t* pcm_chunk,
                     sample_pack::SamplePacker& packer, uint32_t& written, const char** reason_out) {
    auto fail = [&](const char* msg) {
        if (reason_out) *reason_out = msg;
        return false;
    };
    written           = 0;
    uint32_t win_base = 0; // смещение окна от начала блока
    uint32_t win_len  = std::min(block_bytes, kWindowBytes);
    r.bytes(window, win_len);
    if (!r.ok()) return fail("reading the compressed block broke off");

    DecompressState state{};
    state.is16bit = is16bit;
    state.is215   = is215;
    while (written < max_samples) {
        // Хвост окна сдвигается вниз, дочитывается продолжение: каждый байт
        // блока читается один раз.
        const uint32_t ahead = win_len - state.byte_pos;
        if (ahead < kStepWorstBytes && win_base + win_len < block_bytes) {
            std::memmove(window, window + state.byte_pos, ahead);
            win_base            += state.byte_pos;
            state.byte_pos       = 0;
            const uint32_t left  = block_bytes - (win_base + ahead);
            const uint32_t room  = kWindowBytes - ahead;
            const uint32_t want  = std::min(left, room);
            // Seek не нужен: источник стоит ровно на продолжении блока.
            r.bytes(window + ahead, want);
            if (!r.ok()) return fail("reading the compressed block window broke off");
            win_len = ahead + want;
        }
        const uint32_t chunk = std::min(max_samples - written, kStepSamples);
        // 8-битный результат уже со знаком в int16, без *256.
        const uint32_t got = decompress_step(state, window, win_len, chunk, pcm_chunk);
        if (got == 0) break; // данных блока не хватило - дальше следующий блок
        if (!packer.add_samples(pcm_chunk, got)) return fail("PSRAM exhausted while packing (compressed)");
        written += got;
    }
    return true;
}

// Распаковка PCM одного сэмпла в резидентные страницы PSRAM (кодек из
// дескриптора) потоково: блок файла сразу конвертируется, при необходимости
// децимируется и уходит в PSRAM, целиком сэмпл нигде не собирается.
// Отдельная функция, чтобы сэмпл можно было распаковать в любой момент:
// прогрессивная загрузка сначала тянет сэмплы первых паттернов, запускает
// звук и догружает остальное. Всё нужное - из Song и дескриптора, от
// scratch-структур загрузчика вызов не зависит. true - сэмпл упакован и
// опубликован в каталоге. Одна попытка: шаг назад по шине - перемотка файла.
bool load_sample_pcm_impl(memory::TrackMemory& mem, BinaryReader& r, const Song& song, uint16_t sample_index, const char** reason_out) {
    auto fail = [&](const char* msg) {
        if (reason_out) *reason_out = msg;
        memory::scratch_release_all(mem.scratch); // отказ выселяет сценарий: место возвращается арене
        return false;
    };
    // Внутри цикла причина ставится без выхода: после первой add_samples
    // цепочка страниц обязана дойти до finish_and_publish.
    auto set_reason = [&](const char* msg) {
        if (reason_out) *reason_out = msg;
    };

    constexpr uint32_t kPcmChunkSamples = 4096; // кусок нулей хвоста
    // Предел на объявленный размер блока. По формату блок -
    // 32768 отсчётов (8 бит) или 16384 (16 бит) при ширине поля 9 и 17 бит, то
    // есть 36864 и 34816 байт. Больше - битый файл или не IT; поле длины
    // 16-битное и позволяет объявить до 65535.
    constexpr uint32_t kMaxSaneBlockBytes = 40960;

    static_assert(kWindowBytes + kPcmChunkSamples * sizeof(int16_t) <= memory::kSampleRepackBufferBytes, "the IT repack buffer overflowed");
    const SampleDescriptor& sd = song.samples[sample_index];
    if (sd.length_samples == 0) return fail("zero length");
    if (sd.unsupported_codec) return fail("ModPlug-ADPCM is not supported");
    const bool is_compressed = (sd.encoding == SampleEncoding::ItCompressed8 || sd.encoding == SampleEncoding::ItCompressed16);
    // Несжатый путь берёт тот же сценарий сам, поэтому до его развилки
    // место не занимается: жилец в буфере один.
    if (!is_compressed) return soundsinth::model::pack_file_pcm(mem, r, sd, sample_index, reason_out);

    // Окно сжатого блока, за ним отсчёты шага.
    uint8_t* window = memory::scratch_take(mem.scratch, memory::Scratch::SampleRepack, memory::kSampleRepackBufferBytes);
    if (!window) return fail("resident memory overflowed (repack buffer)");
    auto* pcm_chunk    = reinterpret_cast<int16_t*>(window + kWindowBytes);
    const bool is16bit = (sd.encoding == SampleEncoding::ItCompressed16);
    if (!soundsinth::model::resident_pages_fit(mem, sd)) return fail(soundsinth::model::kPsramFull);
    if (!r.seek(sd.file_offset)) return fail("cannot seek to compressed sample data");

    // Решение о кодеке уже в дескрипторе. sd.length_samples к этому моменту
    // децимирован, поэтому из файла читается sd.source_length_samples.
    const uint32_t source_length = sd.source_length_samples;
    sample_pack::SamplePacker packer(mem.psram, sd.resident_encoding, sd.decimated);
    if (sd.loop_unroll != soundsinth::model::LoopUnroll::None) {
        packer.set_loop_unroll(sd.loop_unroll, sd.loop_start, soundsinth::model::loop_end_before_unroll(sd));
    }
    bool ok = true;

    const uint32_t block_capacity = is16bit ? 16384u : 32768u;
    const uint32_t file_size      = r.size();
    uint32_t written_total        = 0;
    // Как у OpenMPT: блок, которому не хватило данных, отдаёт то, что
    // успел, распаковка идёт со следующего блока; пустой блок
    // пропускается; файл кончился - остаток сэмпла нули. Сэмпл звучит, а
    // не теряется целиком. Недочтение внутри файла - сбой источника, отказ.
    while (written_total < source_length) {
        const uint32_t size_field_offset = r.tell();   // для диагностики ниже
        if (size_field_offset + 2u > file_size) break; // файл кончился
        const uint16_t compressed_size = r.u16();
        // Числа в сообщении: по ним видно, на каком байте файла и сколько
        // готово.
        if (!r.ok()) {
            if (reason_out) {
                std::snprintf(soundsinth::model::load_reason_buffer(), soundsinth::model::kLoadReasonBytes,
                              "compressed block size not read: @%lu (%lu of %lu samples done)", static_cast<unsigned long>(size_field_offset),
                              static_cast<unsigned long>(written_total), static_cast<unsigned long>(source_length));
                *reason_out = soundsinth::model::load_reason_buffer();
            }
            ok = false;
            break;
        }
        if (compressed_size == 0) continue;
        // Предел - от формата, а не от буфера: блок целиком в буфер не кладётся.
        if (compressed_size > kMaxSaneBlockBytes) {
            if (reason_out) {
                std::snprintf(soundsinth::model::load_reason_buffer(), soundsinth::model::kLoadReasonBytes,
                              "compressed block size %u > format cap %lu: @%lu (%lu of %lu samples done)", static_cast<unsigned>(compressed_size),
                              static_cast<unsigned long>(kMaxSaneBlockBytes), static_cast<unsigned long>(size_field_offset),
                              static_cast<unsigned long>(written_total), static_cast<unsigned long>(source_length));
                *reason_out = soundsinth::model::load_reason_buffer();
            }
            ok = false;
            break;
        }
        const uint32_t block_file_offset = r.tell();
        // Блок за концом файла - только то, что в файле есть.
        const uint32_t block_bytes = std::min<uint32_t>(compressed_size, file_size - block_file_offset);
        uint32_t written           = 0;
        ok = unpack_it_block(r, block_bytes, std::min(block_capacity, source_length - written_total), is16bit, sd.it_is215, window, pcm_chunk, packer, written,
                             reason_out);
        written_total += written;
        if (!ok) break;
        if (block_bytes < compressed_size) break; // блок оборван концом файла
        // Встать на конец блока явно: чтение окнами останавливается там, где
        // кончилось последнее окно, и до конца блока может остаться хвост, а
        // следующий r.u16() должен попасть ровно на длину следующего блока.
        if (!r.seek(block_file_offset + compressed_size)) {
            set_reason("seek to the end of the compressed block failed");
            ok = false;
            break;
        }
    }
    // Файл кончился раньше сэмпла - остаток нулями, длина прежняя.
    if (ok && written_total < source_length) {
        for (uint32_t k = 0; k < kPcmChunkSamples; ++k) {
            pcm_chunk[k] = 0;
        }
        while (written_total < source_length) {
            const uint32_t chunk = std::min(source_length - written_total, kPcmChunkSamples);
            if (!packer.add_samples(pcm_chunk, chunk)) {
                set_reason("PSRAM exhausted while packing (compressed)");
                ok = false;
                break;
            }
            written_total += chunk;
        }
    }

    // Пропускается только этот сэмпл, а не весь файл.
    memory::scratch_leave(mem.scratch, memory::Scratch::SampleRepack);
    return sample_pack::finish_and_publish(packer, ok, mem.psram, mem.sample_cache, sample_index, reason_out);
}

// Память канала упакованного паттерна IT: последняя маска и значения полей.
struct PackedChannelMemory {
    uint8_t mask = 0;
    PatternCell last;
};

// Инструмент IMPI по прочитанному заголовку hdr (554 байта) и старшим байтам
// номеров сэмплов keyboard_hi (нули без расширения XTPM/MPTX). nullptr -
// разобран, иначе причина отказа (арена переполнена).
SOUNDSINTH_NOINLINE const char* parse_instrument(const uint8_t* hdr, const uint8_t* keyboard_hi, uint16_t sample_count, bool old_instrument_format,
                                                 KeymapRange* keymap_scratch, memory::Arena& arena, Instrument& ins) {
    // Keymap - пары (нота, сэмпл) на смещении 64 в обоих форматах
    // (у старого формата поля до keyboard другие, смещение то же). Нота
    // здесь - целевая нота питча для клавиши и может отличаться от индекса n:
    // один сэмпл растянут на несколько октав с явной разметкой питча под
    // каждую клавишу, обычный приём для ударных инструментов IT.
    // Строится как RLE по диапазонам (KeymapRange), а не плоский массив на
    // 120 записей; клавиши-пробелы (сэмпл вне 1..sample_count) - отдельный
    // диапазон с kNoSample и смещением 0. Диапазон держит сэмпл и одно из
    // двух: смещение (целевая нота - клавиша + смещение) или саму целевую
    // ноту (kKeymapFixedNote). Что именно - решают первые две клавиши
    // диапазона: выбор по каждой паре соседних склеил бы клавиши двух
    // режимов. Один проход во временный массив, в арену - готовое.
    const uint8_t* keyboard = hdr + kKeyboardOffset;
    uint16_t first_distinct = 0;
    bool have_first         = false;
    bool uniform            = true; // сэмпл везде один и смещение везде 0 - keymap не нужен (default_sample_index)
    uint32_t range_count    = 0;
    enum class Mode : uint8_t { Single, Offset, Fixed };
    Mode mode            = Mode::Single;
    uint8_t range_target = 0; // целевая нота первой клавиши диапазона
    for (int n = 0; n < static_cast<int>(kKeymapSlots); ++n) {
        const uint16_t sample_num    = static_cast<uint16_t>(keyboard[n * 2 + 1] | (static_cast<uint16_t>(keyboard_hi[n]) << 8));
        const uint16_t sample_1based = (sample_num != 0 && sample_num <= sample_count) ? sample_num : 0;
        const int8_t offset          = sample_1based != 0 ? static_cast<int8_t>(static_cast<int>(decode_note(keyboard[n * 2])) - n) : 0;
        const uint8_t target         = soundsinth::model::apply_relative_note(static_cast<uint8_t>(n), offset);
        // Ноль в таблице IT - "нота не размечена", а не сэмпл 1 (kNoSample).
        const uint16_t global_idx = sample_1based != 0 ? static_cast<uint16_t>(sample_1based - 1) : soundsinth::model::kNoSample;
        if (sample_1based != 0) {
            if (!have_first) {
                first_distinct = global_idx;
                have_first     = true;
            }
            if (global_idx != first_distinct || offset != 0) uniform = false;
        }
        KeymapRange* cur = range_count != 0 ? &keymap_scratch[range_count - 1] : nullptr;
        bool extend      = false;
        if (cur != nullptr && cur->sample_index == global_idx) {
            if (mode == Mode::Single) {
                if (offset == cur->note_offset) {
                    mode   = Mode::Offset;
                    extend = true;
                } else if (target == range_target) {
                    mode              = Mode::Fixed;
                    cur->start_note  |= soundsinth::model::kKeymapFixedNote;
                    cur->note_offset  = static_cast<int8_t>(target);
                    extend            = true;
                }
            } else {
                extend = mode == Mode::Offset ? offset == cur->note_offset : target == range_target;
            }
        }
        if (!extend) {
            KeymapRange& r = keymap_scratch[range_count++];
            r.start_note   = static_cast<uint8_t>(n);
            r.sample_index = global_idx;
            r.note_offset  = offset;
            mode           = Mode::Single;
            range_target   = target;
        }
    }

    if (!uniform) {
        auto* ranges = memory::arena_new<KeymapRange>(arena, range_count);
        if (!ranges) return ("resident memory overflowed (keymap)");
        std::memcpy(ranges, keymap_scratch, range_count * sizeof(KeymapRange));
        ins.note_to_sample_ranges      = ranges;
        ins.note_to_sample_range_count = static_cast<uint8_t>(range_count);
        ins.default_sample_index       = 0;
    } else {
        ins.note_to_sample_ranges = nullptr;
        // Инструмент не размечает ни одной ноты (keymap нулевой) - в файлах бывает.
        ins.default_sample_index = have_first ? first_distinct : soundsinth::model::kNoSample;
    }

    if (old_instrument_format) {
        // Старый формат (cmwt < 2.00): другая раскладка
        // полей до keyboard, одна огибающая (громкости), нет DCA и глобальной
        // громкости - ins.global_volume остаётся 128.
        ins.nna                = static_cast<soundsinth::model::NewNoteAction>(hdr[26] > 3 ? 0 : hdr[26]);
        ins.dct                = static_cast<soundsinth::model::DuplicateCheckType>(hdr[27] > 3 ? 0 : hdr[27]);
        const uint16_t fadeout = static_cast<uint16_t>(hdr[24] | (hdr[25] << 8));
        ins.fadeout_rate       = static_cast<uint32_t>(fadeout) << 7; // старый формат инструмента (cmwt < 2.00)

        const uint8_t env_flags = hdr[17];
        if (env_flags & 0x01u) { // envEnabled
            auto* env = memory::arena_new<Envelope>(arena);
            if (!env) return ("resident memory overflowed (envelope)");
            {
                env->enabled         = true;
                env->loop_enabled    = (env_flags & 0x02u) != 0;
                env->sustain_enabled = (env_flags & 0x04u) != 0;
                env->carry           = false; // старый формат не знает carry (появился в IT2+)
                env->loop_start      = hdr[18];
                env->loop_end        = hdr[19];
                env->sustain_point   = hdr[20];
                env->sustain_end     = hdr[21];
                const uint8_t* nodes = hdr + kOldEnvNodesOffset;
                uint32_t point_count = 0;
                for (uint32_t pt = 0; pt < soundsinth::model::kMaxEnvelopePoints; ++pt) {
                    const uint8_t tick = nodes[pt * 2];
                    if (tick == 0xff) break; // конец списка, как у OpenMPT
                    env->points[point_count] = EnvelopePoint{tick, static_cast<int8_t>(nodes[pt * 2 + 1])};
                    ++point_count;
                }
                env->point_count = static_cast<uint8_t>(point_count);
                // Старый формат: номер петли или удержания за точками выключает её.
                if (std::max(env->loop_start, env->loop_end) >= point_count) env->loop_enabled = false;
                if (std::max(env->sustain_point, env->sustain_end) >= point_count) env->sustain_enabled = false;
                soundsinth::model::sanitize_tracker_envelope(*env);
                ins.volume_envelope = env;
            }
        }
        // В старом формате нет огибающих панорамы и питча - nullptr.
    } else {
        ins.nna                = static_cast<soundsinth::model::NewNoteAction>(hdr[17] > 3 ? 0 : hdr[17]);
        ins.dct                = static_cast<soundsinth::model::DuplicateCheckType>(hdr[18] > 3 ? 0 : hdr[18]);
        ins.dca                = static_cast<soundsinth::model::DuplicateCheckAction>(hdr[19] > 2 ? 0 : hdr[19]);
        const uint16_t fadeout = static_cast<uint16_t>(hdr[20] | (hdr[21] << 8));
        ins.fadeout_rate       = static_cast<uint32_t>(fadeout) << 6; // новый формат инструмента (cmwt >= 2.00)
        // pps/ppc (смещения 22/23) - Pitch-Pan Separation.
        ins.pitch_pan_separation = static_cast<int8_t>(hdr[22]);
        ins.pitch_pan_center     = hdr[23];
        ins.global_volume        = (hdr[24] > 128) ? 128 : hdr[24];
        // dfp (смещение 25) - панорама инструмента; старший бит - "не применять",
        // у сэмпла наоборот.
        {
            const uint8_t inst_dfp = hdr[25];
            // За шкалой - центр, как у OpenMPT.
            ins.instrument_panning = (inst_dfp & 0x80u) ? -1 : static_cast<int8_t>((inst_dfp & 0x7fu) > 64 ? 32 : (inst_dfp & 0x7fu));
        }

        // Значение = data + value_offset в пределах 0..64, как у OpenMPT. Громкость
        // в файле 0..64, панорама и питч - знаковые -32..32, смещение 32 переводит
        // их в 0..64. Номера петли и удержания зажаты, как у OpenMPT, затем
        // sanitize_tracker_envelope. Включённая огибающая без точек у громкости
        // остаётся (у OpenMPT она включена и не обрабатывается), у панорамы,
        // питча и фильтра - nullptr: иначе край панорамы и +16 полутонов.
        // false - арена переполнена.
        auto read_envelope = [&](uint32_t env_offset, int32_t value_offset, bool keep_empty, const Envelope*& out) -> bool {
            out                     = nullptr;
            const uint8_t* e        = hdr + env_offset;
            const uint8_t env_flags = e[0];
            if (!(env_flags & 0x01u)) return true; // envEnabled
            if (e[1] == 0 && !keep_empty) return true;
            auto* env = memory::arena_new<Envelope>(arena);
            if (!env) return false;
            env->enabled            = true;
            env->loop_enabled       = (env_flags & 0x02u) != 0;
            env->sustain_enabled    = (env_flags & 0x04u) != 0;
            env->carry              = (env_flags & 0x08u) != 0;
            const uint8_t num       = e[1];
            env->point_count        = (num > soundsinth::model::kMaxEnvelopePoints) ? soundsinth::model::kMaxEnvelopePoints : num;
            const uint8_t max_nodes = soundsinth::model::kMaxEnvelopePoints;
            env->loop_start         = std::min(e[2], max_nodes);
            env->loop_end           = std::clamp(e[3], env->loop_start, max_nodes);
            env->sustain_point      = std::min(e[4], max_nodes);
            env->sustain_end        = std::clamp(e[5], env->sustain_point, max_nodes);
            for (uint32_t pt = 0; pt < env->point_count; ++pt) {
                // Узел: int8 значение, uint16le тик - значение первым, не как у XM.
                const uint8_t* node = e + 6 + pt * 3;
                int32_t value       = static_cast<int32_t>(static_cast<int8_t>(node[0])) + value_offset;
                if (value < 0) value = 0;
                if (value > 64) value = 64;
                const uint16_t tick = static_cast<uint16_t>(node[1] | (node[2] << 8));
                env->points[pt]     = EnvelopePoint{tick, static_cast<int8_t>(value)};
            }
            soundsinth::model::sanitize_tracker_envelope(*env);
            out = env;
            return true;
        };
        const char* const kEnvelopeArenaFull = "resident memory overflowed (envelope)";
        if (!read_envelope(kVolEnvOffset, 0, true, ins.volume_envelope)) return (kEnvelopeArenaFull);
        if (!read_envelope(kPanEnvOffset, 32, false, ins.panning_envelope)) return (kEnvelopeArenaFull);
        // Огибающая питча с флагом 0x80 - огибающая фильтра: срез вместо высоты.
        // Заполняется одно из двух полей.
        if (hdr[kPitchEnvOffset] & 0x80u) {
            if (!read_envelope(kPitchEnvOffset, 32, false, ins.filter_envelope)) return (kEnvelopeArenaFull);
            ins.pitch_envelope = nullptr;
        } else {
            if (!read_envelope(kPitchEnvOffset, 32, false, ins.pitch_envelope)) return (kEnvelopeArenaFull);
            ins.filter_envelope = nullptr;
        }

        // IFC/IFR - начальные срез и резонанс инструмента, сырые: бит 0x80 -
        // "значение задано", младшие 7 бит - значение. Бит проверяет движок
        // (как у OpenMPT).
        ins.filter_cutoff    = hdr[kInstrumentFilterCutoffOffset];
        ins.filter_resonance = hdr[kInstrumentFilterResonanceOffset];
    }
    return nullptr;
}

// Сэмпл IMPS по прочитанному заголовку hdr (80 байт).
SOUNDSINTH_NOINLINE void parse_sample_header(const uint8_t* hdr, uint16_t cwtv, uint32_t file_size, SampleDescriptor& sd) {
    RawSampleInfo ri;

    const uint8_t sample_gvl      = hdr[17]; // GvL - глобальная громкость сэмпла, 0..64
    const uint8_t sflags          = hdr[18];
    const uint8_t vol             = hdr[19];
    const uint8_t cvt             = hdr[46];
    const uint8_t dfp             = hdr[47];
    const uint32_t length_samples = static_cast<uint32_t>(hdr[48]) | (static_cast<uint32_t>(hdr[49]) << 8) | (static_cast<uint32_t>(hdr[50]) << 16) |
                                    (static_cast<uint32_t>(hdr[51]) << 24);
    const uint32_t loop_begin = static_cast<uint32_t>(hdr[52]) | (static_cast<uint32_t>(hdr[53]) << 8) | (static_cast<uint32_t>(hdr[54]) << 16) |
                                (static_cast<uint32_t>(hdr[55]) << 24);
    const uint32_t loop_end_raw = static_cast<uint32_t>(hdr[56]) | (static_cast<uint32_t>(hdr[57]) << 8) | (static_cast<uint32_t>(hdr[58]) << 16) |
                                  (static_cast<uint32_t>(hdr[59]) << 24);
    const uint32_t c5speed = static_cast<uint32_t>(hdr[60]) | (static_cast<uint32_t>(hdr[61]) << 8) | (static_cast<uint32_t>(hdr[62]) << 16) |
                             (static_cast<uint32_t>(hdr[63]) << 24);
    const uint32_t sample_pointer = static_cast<uint32_t>(hdr[72]) | (static_cast<uint32_t>(hdr[73]) << 8) | (static_cast<uint32_t>(hdr[74]) << 16) |
                                    (static_cast<uint32_t>(hdr[75]) << 24);

    ri.is16bit = (sflags & 0x02u) != 0;
    // Старый IT (cwtv < 0x214) не снимал флаг стерео при импорте - сэмпл
    // моно, как у OpenMPT.
    ri.is_stereo     = (sflags & 0x04u) != 0 && cwtv >= 0x214;
    ri.is_compressed = (sflags & 0x08u) != 0;
    // Вариант сжатия 2.15 - по сэмплу (бит 2 cvt), а не по версии
    // трекера на весь файл: у 145 файлов архива они расходятся, по версии
    // выходит не тот вариант.
    ri.is215       = (cvt & kCvtDelta) != 0;
    ri.signed_pcm  = (cvt & kCvtSignedSample) != 0;
    ri.file_offset = sample_pointer;
    // cvt 0xFF у несжатого 8-битного - ModPlug-ADPCM. Бит дельты у
    // несжатого не применяется, как у libxmp: в архиве у всех таких
    // сэмплов данные - обычный PCM, сумма дельт даёт шум.
    sd.unsupported_codec = !ri.is_compressed && !ri.is16bit && cvt == 0xff;

    const bool data_present = (sflags & 0x01u) != 0;
    if (!data_present || length_samples == 0) return;

    const uint32_t bytes_per_frame  = (ri.is16bit ? 2u : 1u) * (ri.is_stereo ? 2u : 1u);
    const uint64_t length_bytes_raw = static_cast<uint64_t>(length_samples) * bytes_per_frame;
    if (!ri.is_compressed && sample_pointer + length_bytes_raw > file_size) { // битая длина
        ++soundsinth::model::g_tracker_load_stats.samples_dropped;
        return;
    }
    if (sflags & kSampleSustainLoop) ++soundsinth::model::g_tracker_load_stats.ignored_sustain_loops;
    if (hdr[kSampleVibDepthOffset] != 0) ++soundsinth::model::g_tracker_load_stats.ignored_autovibrato;

    sd.encoding = ri.is_compressed ? (ri.is16bit ? SampleEncoding::ItCompressed16 : SampleEncoding::ItCompressed8)
                                   : (ri.is16bit ? SampleEncoding::Pcm16 : SampleEncoding::Pcm8);
    // Стерео - левый канал (0.66% сэмплов архива IT/S3M): каналы лежат подряд,
    // левый первым. (L+R)/2 требует двух курсоров в разных концах данных, а у
    // Wild Commander шаг назад - перемотка файла с начала.
    sd.channels       = 1;
    sd.length_samples = length_samples;
    sd.loop_start     = loop_begin;
    uint32_t loop_end = loop_end_raw;
    if (loop_end > length_samples) loop_end = length_samples;
    sd.loop_end           = loop_end;
    sd.loop_enabled       = (sflags & 0x10u) != 0 && loop_end > sd.loop_start;
    sd.loop_bidirectional = (sflags & 0x40u) != 0;
    sd.file_offset        = sample_pointer;
    // C5Speed 0 - 8363, меньше 256 - 256, как у OpenMPT.
    sd.c5_speed        = (c5speed == 0) ? 8363 : (c5speed < 256 ? 256 : c5speed);
    sd.relative_note   = 0;
    sd.finetune        = 0; // отдельного finetune у IT нет - питч целиком через C5Speed
    sd.default_volume  = (vol > 64) ? 64 : vol;
    sd.global_volume   = (sample_gvl > 64) ? 64 : sample_gvl; // GvL
    sd.default_panning = (dfp & 0x80u) ? static_cast<int8_t>((dfp & 0x7fu) > 64 ? 64 : (dfp & 0x7fu)) : -1;
    sd.signed_pcm      = ri.signed_pcm; // по сэмплу, из cvt
    sd.it_is215        = ri.is215;

    // Кодек и прореживание - после паттернов (choose_resident_encoding).
    sd.source_length_samples = length_samples;
}

// Паттерны: разбор потока и упаковка в PSRAM на 64 канала. На входе в
// patterns[p].psram_offset - указатель паттерна из таблицы файла, на выходе -
// смещение упакованного. max_channel_used - наибольший номер канала плюс 1.
// nullptr - разобраны, иначе причина.
SOUNDSINTH_NOINLINE const char* unpack_patterns(BinaryReader& r, memory::TrackMemory& mem, uint32_t file_size, uint16_t pattern_count, Pattern* patterns,
                                                PatternCell* cells, PackedChannelMemory* channel_state, uint8_t& max_channel_used) {
    for (uint32_t p = 0; p < pattern_count; ++p) {
        const uint32_t ptr       = patterns[p].psram_offset; // указатель из таблицы
        patterns[p].psram_offset = Pattern::kInvalidOffset;

        if (ptr == 0) {
            // Пустой паттерн-заглушка по конвенции IT - всегда 64 строки (как у
            // OpenMPT).
            // Упаковщику берётся всё свободное, но не больше нужного худшему
            // паттерну архива: буфер и арена - один пул, и на файле с большой
            // ареной упаковщик обходится остатком.
            uint32_t pack_bytes = 0;
            uint8_t* pack_buf   = memory::scratch_take_upto(mem.scratch, memory::Scratch::PatternPack, memory::kPatternPackBufferBytes, pack_bytes);
            if (!pack_buf) return "resident memory overflowed (packer buffer)";
            patterns::PatternPacker packer(pack_buf, pack_bytes, 64, kMaxChannels);
            for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
                cells[ch] = PatternCell{};
            }
            for (uint32_t row = 0; row < 64; ++row) {
                if (!packer.add_row(cells)) return ("pattern does not fit the packer buffer");
            }
            const uint32_t offset = packer.finish(mem.psram);
            memory::scratch_leave(mem.scratch, memory::Scratch::PatternPack);
            if (offset == memory::kPatternAllocFailed) return ("PSRAM pattern area overflowed");
            patterns[p].row_count    = 64;
            patterns[p].psram_offset = offset;
            continue;
        }

        if (!r.seek(ptr)) return ("file truncated inside the pattern header");
        const uint16_t packed_length = r.u16();
        const uint16_t row_count     = r.u16();
        r.skip(4); // reserved
        if (!r.ok()) return ("file truncated inside the pattern header");
        if (row_count == 0 || row_count > soundsinth::model::kMaxPatternRows) return ("pattern row count outside the IT specification range (1..256)");

        // У IT число строк своё у каждого паттерна (1..256), а не 64, как у
        // MOD/S3M. PatternPacker размечает таблицу смещений строк по row_count в
        // конструкторе и лишние строки не принимает.
        // Упаковщику берётся всё свободное, но не больше нужного худшему
        // паттерну архива: буфер и арена - один пул, и на файле с большой
        // ареной упаковщик обходится остатком.
        uint32_t pack_bytes = 0;
        uint8_t* pack_buf   = memory::scratch_take_upto(mem.scratch, memory::Scratch::PatternPack, memory::kPatternPackBufferBytes, pack_bytes);
        if (!pack_buf) return "resident memory overflowed (packer buffer)";
        patterns::PatternPacker packer(pack_buf, pack_bytes, row_count, kMaxChannels);

        const uint32_t data_start = ptr + 8;
        const uint32_t data_end   = data_start + packed_length;
        if (data_end > file_size) return ("pattern data runs past the end of the file");
        if (!r.seek(data_start)) return ("file truncated inside pattern data");

        for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
            channel_state[ch] = PackedChannelMemory{};
        }
        for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
            cells[ch] = PatternCell{};
        }

        uint32_t row = 0;
        while (row < row_count) {
            if (r.tell() >= data_end) return ("pattern data truncated (not enough for the declared row count)");
            const uint8_t b = r.u8();
            if (!r.ok()) return ("file truncated inside pattern data");
            if (b == 0) {
                if (!packer.add_row(cells)) return ("pattern does not fit the packer buffer");
                for (uint32_t ch = 0; ch < kMaxChannels; ++ch) {
                    cells[ch] = PatternCell{};
                }
                ++row;
                continue;
            }

            uint8_t ch_idx = b & 0x7fu;
            if (ch_idx) ch_idx = static_cast<uint8_t>(ch_idx - 1);
            // Под номер канала отведено 7 бит (0..127), но реальных каналов у IT не
            // больше 64. Значение вне диапазона встречается в повреждённых файлах;
            // channel_state[] рассчитан на kMaxChannels.
            if (ch_idx >= kMaxChannels) return ("channel number in the pattern is out of range (corrupt pattern data)");
            if (ch_idx + 1 > max_channel_used) max_channel_used = static_cast<uint8_t>(ch_idx + 1);

            PackedChannelMemory& st = channel_state[ch_idx];
            if (b & 0x80u) st.mask = r.u8();
            if (!r.ok()) return ("file truncated inside pattern data");
            const uint8_t mask = st.mask;
            PatternCell& cell  = cells[ch_idx];

            if (mask & 0x10u) cell.note = st.last.note;
            if (mask & 0x20u) cell.instrument = st.last.instrument;
            if (mask & 0x40u) cell.volume = st.last.volume;
            if (mask & 0x80u) cell.effect = st.last.effect;
            if (mask & 0x01u) {
                cell.note    = decode_note(r.u8());
                st.last.note = cell.note;
            }
            if (mask & 0x02u) {
                cell.instrument    = r.u8();
                st.last.instrument = cell.instrument;
            }
            if (mask & 0x04u) {
                decode_volume(r.u8(), cell.volume);
                st.last.volume = cell.volume;
            }
            if (mask & 0x08u) {
                const uint8_t command = r.u8();
                const uint8_t param   = r.u8();
                decode_effect(command, param, cell.effect);
                st.last.effect = cell.effect;
            }
            if (!r.ok()) return ("file truncated inside pattern data");
        }

        const uint32_t offset = packer.finish(mem.psram);
        memory::scratch_leave(mem.scratch, memory::Scratch::PatternPack);
        if (offset == memory::kPatternAllocFailed) return ("PSRAM pattern area overflowed");
        patterns[p].row_count    = row_count;
        patterns[p].psram_offset = offset;
    }
    return nullptr;
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
    char sig[4];
    r.bytes(sig, 4);
    // Два разных отказа: "не смогли прочитать" лечится в источнике байтов,
    // "прочитали, там не IMPM" - в разборе формата.
    if (!r.ok()) return fail("could not read 4 signature bytes (short read)");
    if (std::memcmp(sig, "IMPM", 4) != 0) return fail("no IMPM signature");

    r.seek(4);
    r.bytes(out.title, 26);

    r.seek(32);
    const uint16_t order_count      = r.u16();
    const uint16_t instrument_count = r.u16();
    const uint16_t sample_count     = r.u16();
    const uint16_t pattern_count    = r.u16();
    // Версия трекера, сохранившего файл (cmwt - совместимая версия формата); нужна для mix_levels и flow_mode.
    const uint16_t cwtv  = r.u16();
    const uint16_t cmwt  = r.u16();
    const uint16_t flags = r.u16();
    r.skip(2); // special
    const uint8_t global_volume_raw = r.u8();
    const uint8_t mv_raw            = r.u8(); // mv (mixing volume)
    const uint8_t speed_raw         = r.u8();
    const uint8_t tempo_raw         = r.u8();
    const uint8_t sep               = r.u8();
    const uint8_t pwd               = r.u8();
    r.skip(2);                         // msglength
    r.skip(4);                         // msgoffset
    const uint32_t reserved = r.u32(); // 0x3C - здесь ModPlug оставляет свою метку
    uint8_t chn_raw[2 * kMaxChannels]; // ChnPan и ChnVol - сразу за полями
    r.bytes(chn_raw, sizeof(chn_raw));
    if (!r.ok()) return fail("file truncated inside the header");

    // Подпись ModPlug Tracker включает тихое сведение MixLevels::Original, как
    // у OpenMPT: "OMPT" при cwtv 0x5xxx, версия 0x888, пары cwtv/cmwt
    // 0x0214/0x0202, 0x0217/0x0200, 0x0300/0x0300. Исключения BeRoTracker
    // (чанк MODU) нет: в архиве IT на этом месте его нет ни у одного файла.
    const bool ompt_mark    = reserved == 0x54504d4fu; // "OMPT" little-endian
    const bool modplug_made = (ompt_mark && (cwtv & 0xf000u) == 0x5000u) || cmwt == 0x888 || cwtv == 0x888 ||
                              (cwtv == 0x0214 && cmwt == 0x0202 && reserved == 0) || (cwtv == 0x0217 && cmwt == 0x0200 && reserved == 0) ||
                              (cwtv == 0x0300 && cmwt == 0x0300 && reserved == 0 && order_count == 256 && sep == 128 && pwd == 0);
    out.mix_levels = modplug_made ? soundsinth::model::Song::MixLevels::Original : soundsinth::model::Song::MixLevels::Compatible;
    // cmwt < 2.00 - старая раскладка инструмента (ITOldInstrument), общий
    // размер тот же, 554 байта; читается ниже отдельной веткой.
    const bool old_instrument_format = cmwt < 0x200;

    const bool instrument_mode = (flags & 0x04u) != 0; // ITHeaderFlags::instrumentMode, иначе режим сэмплов
    const bool old_effects     = (flags & 0x10u) != 0;
    const bool compat_gxx      = (flags & 0x20u) != 0;

    // --- Резидентные метаданные ---

    // ChnPan[64] и ChnVol[64] - начальные панорама и громкость каждого
    // канала, смещения 0x40 и 0x80 в заголовке, подряд. Панорама 0..64 - та
    // же шкала, что панорама канала; бит 0x80 - канал выключен, как у
    // OpenMPT; 100 - surround из центра (Song::channel_surround). Громкость
    // 0..64, больше - режется до 64. Канал с ChnPan 0xFF (запись не
    // заполнена) остаётся с умолчаниями, как у OpenMPT. Разбираются все 64:
    // число каналов известно только после паттернов, движок читает записи
    // до channel_count.
    for (uint8_t ch = 0; ch < kMaxChannels; ++ch) {
        if (chn_raw[ch] == 0xffu) continue;
        if (chn_raw[ch] & kChannelMuted) out.channel_muted |= static_cast<uint64_t>(1) << ch;
        const uint8_t pan   = chn_raw[ch] & 0x7fu;
        out.channel_pan[ch] = (pan > 64) ? 32 : pan; // surround и прочий мусор - из центра
        if (pan == 100) out.channel_surround |= static_cast<uint64_t>(1) << ch;
        const uint8_t vol      = chn_raw[kChnVolOffset - kChnPanOffset + ch];
        out.channel_volume[ch] = (vol > 64) ? 64 : vol;
    }
    out.default_speed         = (speed_raw == 0) ? soundsinth::model::kDefaultSpeed : speed_raw;
    out.default_tempo         = (tempo_raw < soundsinth::model::kMinTempo) ? soundsinth::model::kDefaultTempo : tempo_raw;
    out.default_global_volume = (global_volume_raw > 128) ? 128 : global_volume_raw; // уже 0..128
    out.sample_preamp         = (mv_raw > 128) ? 128 : mv_raw;                       // как у OpenMPT
    out.frequency_model       = (flags & 0x08u) ? soundsinth::model::FrequencyModel::Linear : soundsinth::model::FrequencyModel::Amiga;
    out.quirks                = soundsinth::model::kQuirkItEffectBeforeVolColumn | soundsinth::model::kQuirkItKeyOffPreservesFadeout |
                 soundsinth::model::kQuirkItDctRequiresInstrumentMatch | soundsinth::model::kQuirkItDctComparesPatternNote |
                 soundsinth::model::kQuirkItVibratoTable | soundsinth::model::kQuirkItEnvelopeSustainLoop | soundsinth::model::kQuirkItSilentEnvelopeEndStops |
                 soundsinth::model::kQuirkItVolColumnPortaTable; // всегда для IT, от версии не зависит (как у OpenMPT)
    if (flags & 0x08u) out.quirks |= soundsinth::model::kQuirkItLinearC5Reference;
    // Расширенный диапазон фильтра (ITFileHeader::extendedFilterRange, ставят
    // ModPlug и OpenMPT): делитель шкалы среза 20 вместо 24, срез до ~10 кГц
    // (SONG_EXFILTERRANGE, CutOffToFrequency в Snd_flt.cpp OpenMPT).
    if (flags & kHdrExtendedFilterRange) out.filter_units_per_octave = soundsinth::model::kFilterUnitsItExtended;
    if (old_effects) out.quirks |= soundsinth::model::kQuirkItOldEffects;
    // Oxx за концом сэмпла у IT не глушит ноту - свойство формата, безусловно.
    out.quirks |= soundsinth::model::kQuirkItOffsetPastEndRestarts;
    // Нота на сэмпл без данных обрывает канал; нота вне keymap - нет, старая
    // продолжает звучать.
    out.quirks |= soundsinth::model::kQuirkCutOnEmptySample;
    out.quirks |= soundsinth::model::kQuirkFineSlideInParam;
    if (compat_gxx) {
        out.quirks |= soundsinth::model::kQuirkItCompatGxx;
    } else {
        out.quirks |= soundsinth::model::kQuirkGxxSharesPortaMemory;
    }
    // Pattern Loop у IT менялся между версиями (cwtv): у ранних цель и счётчик
    // общие на все каналы, как у ST3. Порога нет ни у OpenMPT, ни у libxmp:
    // пороги ниже - предположение.
    if (cwtv < 0x104) out.flow_mode |= soundsinth::model::kFlowLoopGlobalTarget;
    if (cwtv >= 0x210) out.flow_mode |= soundsinth::model::kFlowLoopEndAdvancesRow;
    if (cwtv >= 0x200) out.flow_mode |= soundsinth::model::kFlowLoopDelaysSameRowBreak;
    out.restart_position = 0;

    out.order_count = order_count;
    uint16_t* order = nullptr;
    if (order_count > 0) {
        order = memory::arena_new<uint16_t>(mem.resident, order_count);
        if (!order) return fail("resident memory overflowed (order)");
        out.order = order;
    }

    out.pattern_count = pattern_count;
    Pattern* patterns = nullptr;
    if (pattern_count > 0) {
        patterns = memory::arena_new<Pattern>(mem.resident, pattern_count);
        if (!patterns) return fail("resident memory overflowed (patterns)");
        out.patterns = patterns;
    }

    out.sample_count          = sample_count;
    SampleDescriptor* samples = nullptr;
    if (sample_count > 0) {
        samples = memory::arena_new<SampleDescriptor>(mem.resident, sample_count);
        if (!samples) return fail("resident memory overflowed (samples)");
        out.samples = samples;
    }

    // --- Инструменты (или синтетические 1:1 в режиме сэмплов) ---
    const uint32_t effective_instrument_count = instrument_mode ? instrument_count : sample_count;
    out.instrument_count                      = static_cast<uint16_t>(effective_instrument_count);
    Instrument* instruments                   = nullptr;
    if (effective_instrument_count > 0) {
        instruments = memory::arena_new<Instrument>(mem.resident, effective_instrument_count);
        if (!instruments) return fail("resident memory overflowed (instruments)");
        out.instruments = instruments;
    }

    // Таблицы читаются прямо в резидентные записи. Order - байтами в начало
    // своего массива, указатель инструмента - в Instrument::fadeout_rate,
    // сэмпла - в SampleDescriptor::file_offset, паттерна - в
    // Pattern::psram_offset; разбор записи забирает его и ставит своё. Копии
    // таблиц нет, число записей ограничивает резидентная арена, а не шапка
    // файла. В режиме сэмплов указатели инструментов не нужны.
    if (!r.seek(kOrderTableOffset)) return fail("file truncated before the order table");
    auto* order_bytes = reinterpret_cast<uint8_t*>(order);
    if (order_count > 0) r.bytes(order_bytes, order_count);
    if (!r.ok()) return fail("file truncated inside the order table");
    for (uint32_t i = 0; i < instrument_count; ++i) {
        const uint32_t ptr = r.u32();
        if (instrument_mode) instruments[i].fadeout_rate = ptr;
    }
    for (uint32_t i = 0; i < sample_count; ++i) {
        samples[i].file_offset = r.u32();
    }
    for (uint32_t i = 0; i < pattern_count; ++i) {
        patterns[i].psram_offset = r.u32();
    }
    if (!r.ok()) return fail("file truncated inside the pointer tables");
    for (uint32_t i = 0; i < pattern_count; ++i) {
        const uint32_t pp = patterns[i].psram_offset;
        if (pp != 0 && pp + 8 > file_size) return fail("pattern pointer runs past the end of the file");
    }
    // Байты order - в начале того же массива: расширение до 16 бит с конца не
    // затирает ещё не прочитанные.
    for (uint32_t i = order_count; i-- > 0;) {
        const uint8_t b = order_bytes[i];
        order[i]        = (b == 0xff)           ? soundsinth::model::kOrderEnd
                          : (b == 0xfe)         ? soundsinth::model::kOrderSkip
                          : (b < pattern_count) ? static_cast<uint16_t>(b)
                                                : soundsinth::model::kOrderSkip;
    }

    // Scratch - только постоянные буферы: заголовок инструмента, keymap,
    // строка паттерна и память каналов.
    memory::ScratchArena scratch(mem.loader_scratch_buffer, memory::kLoaderScratchBytes);
    static_assert(memory::ScratchPlan{}
                          .add<uint8_t>(kInstrumentHeaderBytes)
                          .add<uint8_t>(kKeymapSlots)
                          .add<KeymapRange>(kKeymapSlots)
                          .add<PatternCell>(kMaxChannels)
                          .add<PackedChannelMemory>(kMaxChannels)
                          .end <= memory::kLoaderScratchBytes,
                  "IT loader buffers do not fit loader_scratch_buffer");

    if (!instrument_mode) {
        // Режим сэмплов (без NNA и кражи голосов): синтезируется маппинг 1:1, как
        // у MOD/S3M, чтобы PatternCell::instrument везде значил индекс в
        // Song::instruments.
        for (uint32_t i = 0; i < sample_count; ++i) {
            instruments[i].default_sample_index = static_cast<uint16_t>(i);
        }
    } else {
        // Заголовок, старшие байты keymap и диапазоны - в scratch: кадр it::load на стеке Core1.
        uint8_t* hdr                = scratch.alloc<uint8_t>(kInstrumentHeaderBytes);
        uint8_t* keyboard_hi        = scratch.alloc<uint8_t>(kKeymapSlots);
        KeymapRange* keymap_scratch = scratch.alloc<KeymapRange>(kKeymapSlots);
        if (!hdr || !keyboard_hi || !keymap_scratch) return fail("file too large for the loader scratch buffer (keymap)");
        for (uint32_t i = 0; i < instrument_count; ++i) {
            const uint32_t ptr          = instruments[i].fadeout_rate; // указатель из таблицы
            instruments[i].fadeout_rate = 0;
            if (ptr == 0 || ptr + kInstrumentHeaderBytes > file_size) continue; // пустой слот
            if (!r.seek(ptr)) continue;
            r.bytes(hdr, kInstrumentHeaderBytes);
            if (!r.ok()) continue;
            if (std::memcmp(hdr, "IMPI", 4) != 0) continue;

            // Расширение keymap: номер сэмпла в keymap - один байт (1..255).
            // Файлам с большим числом сэмплов этого мало, и OpenMPT дописывает старшие
            // байты сразу за 554-байтным заголовком, помечая это словом "XTPM" (в
            // версиях 1.20-1.22 по ошибке "MPTX") в четырёх байтах на смещении 550.
            // Номер = младший | старший << 8. Без этого берётся чужой сэмпл: у
            // bz_ult9.it расширение у 49 инструментов из 99.
            std::memset(keyboard_hi, 0, kKeymapSlots);
            if (std::memcmp(hdr + kInstrumentExtMagicOffset, "XTPM", 4) == 0 || std::memcmp(hdr + kInstrumentExtMagicOffset, "MPTX", 4) == 0) {
                if (ptr + kInstrumentHeaderBytes + kKeymapSlots <= file_size) {
                    r.bytes(keyboard_hi, kKeymapSlots);
                    if (!r.ok()) std::memset(keyboard_hi, 0, kKeymapSlots);
                }
            }

            if (const char* e = parse_instrument(hdr, keyboard_hi, sample_count, old_instrument_format, keymap_scratch, mem.resident, instruments[i])) {
                return fail(e);
            }
        }
    }

    // --- Сэмплы (IMPS) ---
    if (sample_count > 0) {
        uint8_t hdr[kSampleHeaderBytes];
        for (uint32_t i = 0; i < sample_count; ++i) {
            const uint32_t ptr     = samples[i].file_offset; // указатель из таблицы
            samples[i].file_offset = 0;
            if (ptr == 0 || ptr + kSampleHeaderBytes > file_size) continue;
            if (!r.seek(ptr)) continue;
            r.bytes(hdr, kSampleHeaderBytes);
            if (!r.ok() || std::memcmp(hdr, "IMPS", 4) != 0) continue;

            parse_sample_header(hdr, cwtv, file_size, samples[i]);
        }
        // Сетка периодов Amiga - как у S3M: c5_speed звучит на ноте 48 (C-4).
        // У IT - на C-5 (60), у OpenMPT это одна и та же нота.
        if (out.frequency_model == soundsinth::model::FrequencyModel::Amiga) {
            for (uint32_t i = 0; i < sample_count; ++i) {
                samples[i].relative_note = -12;
            }
        }
    }

    // --- Паттерны ---
    // Число каналов в заголовке IT не хранится: наибольший номер канала в
    // потоке, известен после всех паттернов. Упаковка - на все 64 канала
    // (маска строки у упакованного паттерна 64-битная при любом числе
    // каналов), channel_count всем паттернам - после цикла.
    // Ячейки строки и память каналов - в scratch: кадр it::load на стеке Core1.
    PatternCell* cells                 = scratch.alloc<PatternCell>(kMaxChannels);
    PackedChannelMemory* channel_state = scratch.alloc<PackedChannelMemory>(kMaxChannels);
    if (!cells || !channel_state) return fail("file too large for the loader scratch buffer (patterns)");
    uint8_t max_channel_used = 0;
    if (const char* e = unpack_patterns(r, mem, file_size, pattern_count, patterns, cells, channel_state, max_channel_used)) {
        return fail(e);
    }
    // Паттерны пусты - 4 канала.
    const uint8_t song_channel_count = max_channel_used == 0 ? 4 : max_channel_used;
    out.channel_count                = song_channel_count;
    for (uint32_t p = 0; p < pattern_count; ++p) {
        patterns[p].channel_count = song_channel_count;
    }

    // Паттерны упакованы: зона сэмплов сдвигается вплотную к ним. Паттерн
    // после этого вызова лёг бы поверх сэмплов.
    memory::psram_freeze_pattern_zone(mem.psram);

    // Кодек по точной сумме страниц, затем ping-pong петли разворачиваются в
    // остаток зоны сэмплов: все заголовки уже разобраны, PCM ещё не читался.
    const uint32_t free_pages = memory::psram_free_page_count(mem.psram);
    soundsinth::model::choose_resident_encoding(out, free_pages);
    soundsinth::model::unroll_pingpong_loops(out, free_pages, soundsinth::model::LoopUnroll::It);

    // --- Сэмплы: перепаковка в резидентный кодек дескриптора ---
    soundsinth::model::count_nonresident_samples(out);
    soundsinth::model::note_source_end(out);
    // metadata_only: PCM вытянет load_sample_pcm() позже, по одному.
    if (metadata_only) return true;
    for (uint32_t i = 0; i < sample_count; ++i) {
        if (!soundsinth::model::sample_is_resident(out.samples[i])) continue;
        // Свой reader на сэмпл: обрыв одного сэмпла не гасит остальные.
        BinaryReader sample_reader(src);
        const char* reason = nullptr;
        if (!load_sample_pcm_impl(mem, sample_reader, out, static_cast<uint16_t>(i), &reason)) {
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
    return load_sample_pcm_impl(mem, r, song, sample_index, reason_out);
}

} // namespace soundsinth::formats::it
