#pragma once

// Конвертер MIDI -> ячейки строки трекера: правила MIDI и состояние каналов.
// Одно ядро у загрузчика .mid (события файла по строкам сетки) и у живого
// входа (события потока по мере прихода): begin_row, handle на каждое
// событие строки, finish_row.
//
// Здесь же сборка инструментов и сэмплов песни из банка.

#include <cstdint>
#include <cstring>

#include "platform/compiler.h"
#include "core/bank/bank_reader.h"
#include "core/model/song.h"
#include "core/formats/midi.h"
#include "core/memory/track_memory.h"
#include "core/codec/pattern_cell_codec.h"

namespace soundsinth::formats::midi {

using soundsinth::model::Effect;
using soundsinth::model::Envelope;
using soundsinth::model::Instrument;
using soundsinth::model::KeymapRange;
using soundsinth::model::kNoteNone;
using soundsinth::model::kNoteOff;
using soundsinth::model::PatternCell;
using soundsinth::model::SampleDescriptor;
using soundsinth::model::SlideRate;
using soundsinth::model::Song;
using soundsinth::model::VolumeColumnType;

constexpr uint8_t kMaxChannels = 64; // столько же голосов у движка
static_assert(kMaxChannels <= soundsinth::model::kMaxPatternChannels, "маска строки упаковщика - 64 канала");
// Потолок слоёв на ноту - общий с выбором слоёв банка.
constexpr uint8_t kMaxLayers = bank::kMaxNoteLayers;
// Бенд канала трекера "не выписан": таких значений бенд не принимает
// (диапазон до 24 полутонов - 1536 единиц 1/64 полутона).
constexpr int16_t kBendUnwritten = -32768;
// Выбор RPN пуст (RPN Null, 127/127): Data Entry ничего не меняет.
constexpr uint8_t kRpnNone = 127;
// RPN 0/0 - диапазон питч-бенда.
constexpr uint8_t kRpnPitchBendRange = 0;
// Общая громкость из SysEx едет в таймлайн своим событием: у неё своё время,
// а канала нет. Значения 0xf1..0xf6 в дорожке SMF не встречаются.
constexpr uint8_t kMasterVolumeStatus = 0xf1;
// Объявление канала ударным из SysEx - тоже своим событием: у живого потока
// оно приходит по ходу игры, а не известно заранее, как у файла. d1 - канал,
// d2 - ударный он теперь или нет.
constexpr uint8_t kDrumChannelStatus = 0xf2;

// Кэш выбора слоёв ноты: строка - пресет, клавиша и полоса силы удара.
constexpr uint32_t kLayerCacheSlots = 1024;
struct LayerCacheEntry {
    uint32_t key; // 0 - строка пуста
    uint8_t count;
    bank::NoteLayer items[kMaxLayers];
};

constexpr uint16_t kMaxInstruments = patterns::kMaxCellInstrument; // 9 бит номера в ячейке

// Сетка: строк на долю и тиков на строку.
struct Grid {
    uint8_t rows_per_beat;
    uint8_t ticks_per_row;
};

// Событие канала или служебное (kMasterVolumeStatus) на тике MIDI.
struct Event {
    uint32_t tick;
    uint8_t status;
    uint8_t d1;
    uint8_t d2;
    uint8_t pad;
};

// Состояние канала MIDI с умолчаниями GM. Доли файлов в комментариях - по
// архиву из 126 тысяч .mid, числа библиотеки - по её 726 файлам.
struct MidiChannel {
    uint8_t program = 0, bank_msb = 0;
    uint8_t cc7 = 100, cc11 = 127, cc10 = 64;
    uint8_t sustain = 0;
    // Питч-бенд в 1/64 полутона (шкала движка).
    int16_t bend = 0;
    // Диапазон бенда по RPN0 в полутонах, умолчание GM 2.
    uint8_t bend_range = 2;
    // CC1, колесо модуляции. Явных модуляторов от CC1 в банке нет, работает
    // умолчание SF2 (модулятор 5): CC1 -> vibLfoToPitch, размах 50 центов.
    // Частота LFO банка по freqVibLFO - 5.0 Гц.
    uint8_t cc1 = 0;
    // Давление канала: у SF2 модулятор по умолчанию качает высоту так же, как
    // CC1, размах те же 50 центов. Складывается с CC1 в глубине вибрато.
    uint8_t pressure = 0;
    // CC91 - посыл в общий ревербератор, умолчание 0, а не 40 по GM2: эталонный
    // синтезатор без CC91 реверберации не добавляет. CC91 стоит у 51% архива.
    uint8_t cc91 = 0;
    // Портаменто: CC65 включает, CC5 задаёт время; задевает 2.25% архива.
    // Трекерный TonePorta делает то же: тянет высоту к ноте ячейки, не
    // перезапуская сэмпл.
    uint8_t porta_on = 0, porta_time = 0, last_note = 0xff;
    // Выбор RPN (CC101/CC100) изначально пустой (RPN Null, 127/127): Data Entry
    // (CC6) без выбора ничего не делает. С выбором 0/0 CC6 в начале канала
    // переписывает диапазон бенда - так у 50 файлов библиотеки.
    uint8_t rpn_lsb = kRpnNone, rpn_msb = kRpnNone;

    // Reset All Controllers (CC121, 19% архива), как у OpenMPT и сверх него
    // CC1 0 по RP-015: CC11 127, бенд в центр, диапазон 2, выбор RPN пустой.
    // CC7, CC10, CC91, портаменто, программа и банк не сбрасываются; педаль
    // снимает вызывающий - удержанные ноты уходят, как при её отпускании.
    void reset_controllers() {
        cc11 = 127;
        cc1 = 0;
        pressure = 0;
        bend = 0;
        bend_range = 2;
        rpn_msb = kRpnNone;
        rpn_lsb = kRpnNone;
    }
};

// Канал трекера: что на нём звучит и что в него уже выписано. Каналы
// раздаются динамически и переходят от одного канала MIDI к другому, поэтому
// сверка идёт с последним выписанным, а не с фактом события.
struct TrackerChannel {
    bool pend_off = false;
    uint8_t midi = 0xff, note = 0xff, excl = 0, vel = 100, span = 32, pend_pan = 0xff;
    // Нота, снятая с канала и доигрывающая релиз. Помнится затем, чтобы её
    // повтор лёг на этот же канал: на рояле новый удар по клавише глушит
    // прежнюю струну, а не звучит вместе с ней.
    uint8_t rel_note = 0xff;
    // Громкость (CC7/CC11) ждёт свободной колонки громкости - как pend_pan.
    bool pend_vol = false;
    int16_t bend = 0;
    // Скорость вибрато зависит только от частоты тиков - выписывается один раз
    // на канал, дальше только глубина; после смены темпа - заново.
    bool vib_speed_set = false;
    uint8_t rev = 0xff;
    uint16_t inst = 0xffff; // инструмент канала: скользить можно только к тому же тембру
    // Сколько строк ещё дописывать TonePorta. Команда действует одну строку
    // (tone_porta_active сбрасывается построчно), поэтому скольжение
    // ведётся явно до конца - как Gxx на каждой строке перехода в модуле.
    uint16_t glide = 0;
    uint16_t bs = bank::kNoSample; // сэмпл банка на канале - условие скольжения
    uint32_t age = 0;
    // Модульный тик, когда отзвучит релиз снятой ноты: снятие плюс fadeout_ms
    // инструмента (затухание до -48 дБ, после него голос гаснет). 32 бит
    // хватает: тик модуля меньше 4096 x 128 x 255 (паттерны, строки, тики на
    // строку), релиз прибавляет не больше 6686.
    uint32_t quiet_at = 0;
    // Затухание инструмента ноты, мс банка: снятие не ходит по таблицам банка.
    uint16_t rel_ms = 0;
};

// Громкость колонки 1..64 по velocity, CC7 и CC11.
uint8_t cell_volume(uint8_t velocity, uint8_t cc7, uint8_t cc11);

// Тиков в секунду у движка - 2 * tempo / 5, с отбрасыванием: скорость
// вибрато и скольжения. Темп не ниже kMinTempo (32), тиков не меньше 12.
inline uint32_t engine_ticks_per_second(uint32_t tempo) {
    return 2u * tempo / 5u;
}

// Миллисекунды в тики движка точной дробью, без округления частоты тиков.
// В 32 битах: ms из uint16 банка, темп до 255 - произведение меньше 2^25.
inline uint32_t ms_to_engine_ticks(uint32_t ms, uint32_t tempo) {
    return ms * 2u * tempo / 5000u;
}

// Скорость вибрато CC1 при темпе трекера tempo.
uint8_t vibrato_speed_at(uint32_t tempo);

// Заголовок Song .mid: модель частот, квирки, сведение, фильтр банка, темп
// и скорость сетки.
void init_song_header(Song& out, Grid grid, uint32_t tracker_tempo);

// Инструмент банка -> поля Instrument (без огибающих и keymap).
void instrument_from_bank(const bank::BankInstrument& bi, uint32_t ticks_per_second_rounded, Instrument& ins);

// Огибающая банка -> огибающая песни; миллисекунды - в тики.
void envelope_from_bank(const bank::BankEnvelope& be, uint32_t ticks_per_second_rounded, Envelope& env);

// Запись сэмпла песни из сэмпла банка bs (номер банка - в file_offset).
void sample_from_bank(const bank::Bank& bank, uint16_t bs, SampleDescriptor& d);

// Инструмент песни i из инструмента банка used_instruments[i]; инструменты
// 0..i-1 уже построены - огибающие с ними общие. Сэмпл keymap без номера в
// bank_to_song_sample (0xffff) - kNoSample. nullptr - готово, иначе причина
// отказа; env_copies растёт на число новых огибающих в арене.
const char* build_instrument(Song& out, memory::TrackMemory& mem, const bank::Bank& bank,
                             const uint16_t* used_instruments, uint16_t i, const uint16_t* bank_to_song_sample,
                             uint32_t ticks_per_second_rounded, uint32_t& env_copies);

// Keymap инструмента банка в km (bi.keymap_count мест): сэмпл банка -
// запись песни по bank_to_song_sample, без записи (0xffff) - kNoSample.
void fill_keymap(const bank::Bank& bank, const bank::BankInstrument& bi, const uint16_t* bank_to_song_sample,
                 KeymapRange* km);

// Инструменты и сэмплы песни из банка, все разом; nullptr - готово, иначе
// причина отказа; env_copies - огибающих в арене.
const char* build_instruments(Song& out, memory::TrackMemory& mem, const bank::Bank& bank,
                              const uint16_t* used_instruments, uint16_t used_instrument_count,
                              uint16_t used_sample_count, const uint16_t* bank_to_song_sample,
                              uint32_t ticks_per_second_rounded, uint32_t& env_copies);

// Правила MIDI и состояние каналов - в одном объекте, запись в ячейки -
// его же методами. Живому входу нужны те же правила, но вместо ячеек
// паттерна - вызовы движка: делить приходится по этой границе.
//
// Лежит в scratch: загрузка идёт на Core1 со стеком 4 КБ.
struct Converter {
    MidiChannel mch[16];
    TrackerChannel tch[kMaxChannels];
    PatternCell cells[kMaxChannels];
    // Инструменты песни - по порядку появления; номер даётся в момент
    // постановки ноты, отдельного прохода по событиям нет.
    uint16_t used_instruments[kMaxInstruments];
    // Копия указателей блоба: обращений к банку на ноту десятки.
    bank::Bank bank;
    // Счётчики потерь раскладки: загрузчик - свои LoadStats, живой вход - свои.
    LoadStats* stats = nullptr;
    Grid grid;
    bool drum_channel[16];
    uint8_t* held = nullptr;              // [канал MIDI][нота][слой] -> канал трекера
    uint8_t* pending = nullptr;           // клавиша отпущена, держит педаль
    LayerCacheEntry* layer_cache = nullptr;
    uint16_t* song_inst_of = nullptr;
    uint16_t* bank_to_song_sample = nullptr;
    uint16_t used_instrument_count = 0;
    uint16_t used_sample_count = 0;
    uint32_t age = 0;
    uint32_t mt = 0;                      // тик модуля разбираемого события
    uint8_t cur_tempo = 0;
    uint8_t vibrato_speed = 0;
    // Общая громкость, запрошенная SysEx: выписывает её проход строки.
    uint8_t want_global = 0;
    bool reverb_used = false;
    // Живой вход: потолки песни и крюки. У загрузчика файла потолок
    // инструментов - номер в ячейке, сэмплов - без потолка, крюков нет.
    // alloc_instrument - номер новому инструменту (0xffff - нет, нота без
    // этого слоя); on_new_instrument - номер дан, строить сразу (false - не
    // построился, слой отпадает); on_sample - нота взяла сэмпл банка
    // инструментом песни song_inst (подгрузить PCM, отметить звучание).
    uint16_t max_instruments = kMaxInstruments;
    uint16_t max_samples = 0xffff;
    uint16_t (*alloc_instrument)(void* user) = nullptr;
    // Запись песни сэмплу банка без записи (0xffff - нет, слой отпадает).
    uint16_t (*alloc_sample)(void* user, uint16_t bank_sample) = nullptr;
    bool (*on_new_instrument)(void* user, uint16_t song_inst) = nullptr;
    void (*on_sample)(void* user, uint16_t song_inst, uint16_t bank_sample) = nullptr;
    void* hook_user = nullptr;

    // Заново с начала трека, сохранив нумерацию. Номера инструментов и
    // записей сэмплов песни раздаются по ходу разбора и уже запечены в неё -
    // сбросить их значило бы получить другую песню. Сбрасывается только то,
    // что ведёт игру: состояние каналов MIDI и трекера, ячейки строки,
    // держащиеся ноты и педаль.
    //
    // held/pending - чужие буферы, их размеры знает вызывающий.
    void reset_for_replay(uint32_t held_bytes, uint32_t pending_bytes, uint8_t start_tempo, uint8_t global_volume) {
        for (uint32_t i = 0; i < 16; ++i) mch[i] = MidiChannel{};
        for (uint32_t i = 0; i < kMaxChannels; ++i) tch[i] = TrackerChannel{};
        for (uint32_t i = 0; i < kMaxChannels; ++i) cells[i] = PatternCell{};
        if (held != nullptr) std::memset(held, 0xff, held_bytes);
        if (pending != nullptr) std::memset(pending, 0, pending_bytes);
        age = 0;
        mt = 0;
        cur_tempo = start_tempo;
        want_global = global_volume;
        reverb_used = false;
    }

    // --- Запись в ячейки строки ---

    // Панорама - в колонку громкости (там шкала сразу 0..64, без огрубления
    // /4 колонки эффекта), колонка эффекта остаётся таймингу и бенду. На строке
    // с нотой колонка громкости занята velocity - тогда откладываем.
    SOUNDSINTH_NOINLINE void put_pan(uint32_t c, uint8_t pan) {
        if (cells[c].volume.type == VolumeColumnType::None) {
            cells[c].volume.type = VolumeColumnType::SetPanning;
            cells[c].volume.param = pan;
            tch[c].pend_pan = 0xff;
        } else {
            tch[c].pend_pan = pan;
        }
    }
    // Панорама канала: зона банка плюс CC10 канала MIDI, 0..64.
    uint8_t channel_pan(uint32_t c, uint8_t cc10) const {
        const int32_t pv = static_cast<int32_t>(tch[c].span) + (static_cast<int32_t>(cc10) - 64) / 2;
        return static_cast<uint8_t>(pv < 0 ? 0 : (pv > 64 ? 64 : pv));
    }
    // Громкость звучащего канала по CC7/CC11 его канала MIDI. Громкость
    // канала и экспрессия меняются по ходу ноты - на этом держатся наплывы и
    // приглушения партий. Колонка громкости бывает занята (velocity ноты этой
    // строки, панорама) - тогда громкость откладывается на следующую строку,
    // иначе звучащая нота осталась бы на старой громкости до следующей ноты.
    SOUNDSINTH_NOINLINE void refresh_volume(uint32_t c) {
        if (cells[c].volume.type != VolumeColumnType::None) {
            tch[c].pend_vol = true;
            return;
        }
        cells[c].volume.type = VolumeColumnType::SetVolume;
        cells[c].volume.param = cell_volume(tch[c].vel, mch[tch[c].midi].cc7, mch[tch[c].midi].cc11);
        tch[c].pend_vol = false;
    }
    // Бенд абсолютный: пропустить нельзя, только отложить, но откладывать
    // некуда - колонка эффекта одна. Кладём, если свободна; иначе значение
    // доедет следующим изменением или строкой через сверку tch[c].bend.
    SOUNDSINTH_NOINLINE void put_bend(uint32_t c, int32_t units) {
        // Панорама строки ноты уступает бенду и доезжает колонкой громкости
        // следующей строкой: промах панорамы на строку тише промаха высоты.
        if (cells[c].effect.type == Effect::SetPanning) {
            tch[c].pend_pan = static_cast<uint8_t>(cells[c].effect.param >> 2);
            cells[c].effect.type = Effect::None;
        }
        // Свой же SetPitchOffset этой строки переписывается: в ячейку идёт
        // последнее событие строки, а не первое. У плавного бенда события
        // идут чаще строки, и первое отстаёт от файла на строку.
        if (cells[c].effect.type != Effect::None && cells[c].effect.type != Effect::SetPitchOffset) {
            return;
        }
        // Множитель поднимается, только если значение не влезает в -128..128:
        // +128 (бывает при нечётном диапазоне) лучше обрезать до +127, чем
        // переходить на вчетверо более грубый шаг ради одной 1/64 полутона.
        // Округление к ближайшему: отбрасывание вниз даёт ошибку до 98 центов
        // на шаге 64, округление - вдвое меньше и без смещения вниз.
        auto scaled = [units](int32_t bits) {
            if (bits == 0) return units;
            const int32_t step = 1 << bits;
            return units >= 0 ? (units + step / 2) / step : -((-units + step / 2) / step);
        };
        int32_t shift = 0, v = units;
        while ((v < -128 || v > 128) && shift < 3) {
            ++shift;
            v = scaled(shift * 2);
        }
        if (v < -128) {
            v = -128;
        } else if (v > 127) {
            v = 127;
        }
        cells[c].effect.type = Effect::SetPitchOffset;
        cells[c].effect.rate = static_cast<SlideRate>(shift);
        cells[c].effect.param = static_cast<uint8_t>(v + 128);
        // Запоминается запрошенное значение, а не округлённое: догон сверяет
        // tch[c].bend с бендом канала MIDI, и при +128 или ненулевых младших битах
        // (shift > 0) переписывал бы SetPitchOffset на каждой строке, пока
        // звучит нота, занимая колонку эффекта.
        tch[c].bend = static_cast<int16_t>(units);
    }
    // Очередь колонки эффекта: тайминг ноты (TonePorta и NoteDelay на note-on)
    // вытесняет всё, прочие писатели кладут только в пустую. Вытесненный бенд
    // помечается невыписанным - догон повторит его следующей строкой, иначе
    // сверка tch[c].bend считала бы его выписанным до следующего изменения.
    SOUNDSINTH_NOINLINE void put_note_effect(uint32_t c, Effect type, uint8_t param) {
        if (cells[c].effect.type == Effect::SetPitchOffset) {
            tch[c].bend = kBendUnwritten;
            cells[c].effect.rate = SlideRate::PerTick;
        } else if (cells[c].effect.type == Effect::SetPanning) {
            tch[c].pend_pan = static_cast<uint8_t>(cells[c].effect.param >> 2);
        }
        cells[c].effect.type = type;
        cells[c].effect.param = param;
    }

    // --- Снятие ноты ---

    // Снятие ноты: общий путь для note-off и отпускания педали.
    // Снятие одного канала: kNoteOff в свободную колонку ноты, иначе на
    // следующей строке; канал больше не звучащий.
    // hard - глушить сразу (CC120 All Sound Off): в колонку ноты kNoteCut,
    // а не kNoteOff с релизом, как у SF2-синтезаторов.
    SOUNDSINTH_NOINLINE void release_channel(uint8_t c, uint8_t at_delay, bool hard = false) {
        if (cells[c].note == kNoteNone) {
            cells[c].note = hard ? soundsinth::model::kNoteCut : kNoteOff;
            // Задержка снятия - тайминг ноты: она вытесняет бенд и панораму
            // этой строки, у обоих есть догон (невыписанный бенд и pend_pan).
            // Иначе снятие уходило на тик 0 - раньше файла почти на строку.
            if (at_delay && (cells[c].effect.type == Effect::None ||
                             cells[c].effect.type == Effect::SetPitchOffset ||
                             cells[c].effect.type == Effect::SetPanning)) {
                put_note_effect(c, Effect::NoteDelay, at_delay);
            } else if (at_delay) {
                ++stats->off_delay_lost;
            }
        } else {
            // В ячейке уже стоит нота: взята на этой же строке и снимается раньше
            // конца строки. Колонка ноты одна, снятие откладывается на следующую
            // строку: нота длиннее не более чем на строку, но не зависает навсегда.
            tch[c].pend_off = true;
        }
        tch[c].rel_note = tch[c].note; // чья клавиша доигрывает - для её повтора
        tch[c].note = 0xff;
        if (hard) {
            tch[c].quiet_at = mt + 1u;   // голос снят, релиза нет
        } else if (tch[c].inst < used_instrument_count) {
            tch[c].quiet_at = mt + ms_to_engine_ticks(tch[c].rel_ms, cur_tempo) + 1u;
        }
        // excl не сбрасывается: класс живёт, пока может звучать голос, - у ударных
        // note-off идёт сразу за note-on, а звон длится секунды. Класс держится, пока канал не займут заново (там excl
        // перезаписывается); лишний обрыв давно замолчавшего голоса безвреден.
    }
    SOUNDSINTH_NOINLINE void release_note(uint8_t mch, uint8_t note, uint8_t at_delay, bool hard = false) {
        uint8_t* slot = held + (mch * 128 + note) * kMaxLayers;
        for (uint32_t k = 0; k < kMaxLayers; ++k) {
            const uint8_t c = slot[k];
            if (c >= kMaxChannels) continue;
            release_channel(c, at_delay, hard);
            slot[k] = 0xff;
        }
        pending[mch * 128 + note] = 0;
    }
    // Забыть канал за его клавишей: канал забрала другая нота (кража
    // самого давнего, обрыв по классу). Иначе снятие прежней клавиши
    // писало бы kNoteOff в канал уже новой ноты.
    SOUNDSINTH_NOINLINE void forget_channel(uint32_t c) {
        if (tch[c].midi >= 16 || tch[c].note >= 128) return;
        uint8_t* other = held + (tch[c].midi * 128 + tch[c].note) * kMaxLayers;
        for (uint32_t k = 0; k < kMaxLayers; ++k) {
            if (other[k] == c) other[k] = 0xff;
        }
    }

    // --- Строка и события ---

    // Начало строки: то, что не влезло в прошлую.
    void begin_row() {
        // До событий - один проход: каждый шаг трогает только ячейку и состояние
        // своего канала.
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            cells[c] = PatternCell{};
            // Снятия, не влезшие в прошлую строку (release_note).
            if (tch[c].pend_off) {
                cells[c].note = kNoteOff;
                tch[c].pend_off = false;
            }
            // Отложенная панорама: CC10 на строке, где колонку громкости заняла
            // velocity, и панорама ноты, не взявшая колонку эффекта (задержка,
            // крайнее правое, бенд). Доезжает строкой позже, 30-75 мс по темпу и
            // сетке.
            if (tch[c].pend_pan != 0xff) {
                cells[c].volume.type = VolumeColumnType::SetPanning;
                cells[c].volume.param = tch[c].pend_pan;
                tch[c].pend_pan = 0xff;
            }
            // Отложенная громкость - в колонку, которую не заняла панорама; значение
            // по текущим CC7/CC11, а не по тем, что были при откладывании.
            if (tch[c].pend_vol) {
                if (tch[c].midi >= 16 || tch[c].note == 0xff) {
                    tch[c].pend_vol = false;
                } else {
                    refresh_volume(c);
                }
            }
        }
    }

    // Событие на тике модуля at_mt; delay - его тики от начала строки.
    // Слои ноты: пресет банка, кэш выбора и признание слоя - номер
    // инструмента, запись сэмпла, заказ PCM. Идемпотентно: живой вход зовёт
    // это заранее, по упреждению, а потом ещё раз на самой ноте.
    uint32_t select_layers(uint8_t bank_no, uint8_t program, uint8_t note, uint8_t velocity,
                           bank::NoteLayer (&picked)[bank::kMaxNoteLayers], bool& capped) {
        const bank::BankPreset& preset = bank::bank_preset(bank, bank_no, program);
        // Выбор слоёв повторяется: одна и та же клавиша с той же полосой силы
        // удара звучит в треке сотни раз, а перебор идёт по всем слоям пресета
        // в PSRAM. Ключ - пресет, клавиша и полоса; номера слоёв от порядка нот
        // не зависят, а признание слоя вызывается и на попадании - оно
        // идемпотентно.
        const uint32_t layer_key = (static_cast<uint32_t>(bank_no) << 24) |
                                   (static_cast<uint32_t>(program) << 16) |
                                   (static_cast<uint32_t>(note) << 8) |
                                   bank::quantize_velocity(velocity);
        auto accept_layer = [&](uint16_t li, uint16_t bs) {
                const uint16_t bi_index = bank.layers[li].instrument;
                if (song_inst_of[bi_index] == 0xffff) {
                    uint16_t slot;
                    if (alloc_instrument) {
                        // Живой вход: номер выдаёт он, при нужде вытесняя молчащий.
                        slot = alloc_instrument(hook_user);
                        if (slot == 0xffff) {
                            capped = true;
                            return false;
                        }
                    } else {
                        if (used_instrument_count >= max_instruments) {
                            capped = true;
                            return false;
                        }
                        slot = used_instrument_count++;
                    }
                    song_inst_of[bi_index] = slot;
                    used_instruments[slot] = bi_index;
                    if (on_new_instrument && !on_new_instrument(hook_user, slot)) {
                        song_inst_of[bi_index] = 0xffff;
                        capped = true;
                        return false;
                    }
                }
                if (bank_to_song_sample[bs] == 0xffff) {
                    uint16_t rec;
                    if (alloc_sample) {
                        // Живой вход: запись выдаёт он.
                        rec = alloc_sample(hook_user, bs);
                    } else {
                        rec = used_sample_count < max_samples ? used_sample_count++ : 0xffff;
                    }
                    if (rec == 0xffff) {
                        capped = true;
                        return false;
                    }
                    bank_to_song_sample[bs] = rec;
                }
                if (on_sample) on_sample(hook_user, song_inst_of[bi_index], bs);
                return true;
            };
        uint32_t picked_count = 0;
        // Номер строки - перемешанный ключ: остаток от деления клал все ноты
        // одной полосы в одну строку, и кэш не попадал ни разу.
        const uint32_t cache_slot = (layer_key * 2654435761u) >> 22;
        LayerCacheEntry& slot_cache = layer_cache[cache_slot & (kLayerCacheSlots - 1u)];
        if (slot_cache.key == layer_key) {
            for (uint8_t k = 0; k < slot_cache.count; ++k) {
                const bank::NoteLayer& nl = slot_cache.items[k];
                if (accept_layer(nl.layer, nl.bank_sample)) picked[picked_count++] = nl;
            }
        } else {
            picked_count = bank::select_note_layers(bank, preset, note, velocity, accept_layer, picked);
            slot_cache.key = layer_key;
            slot_cache.count = static_cast<uint8_t>(picked_count);
            for (uint32_t k = 0; k < picked_count; ++k) slot_cache.items[k] = picked[k];
        }
        return picked_count;
    }

    void handle(const Event& e, uint32_t at_mt, uint8_t delay) {
        mt = at_mt;
        const uint8_t kind = e.status >> 4, ch = e.status & 0x0fu;
        if (e.status == kMasterVolumeStatus) {
            // Общая громкость 0..127 -> шкала движка 0..128, по квадрату (40 lg),
            // как velocity и CC7/CC11 в громкости ячейки. Линейная шкала давала
            // вдвое более мелкое затухание, чем у эталона. Выписывает её проход
            // строки, там же, где темп: канала у неё нет.
            const uint32_t mv = e.d1;
            want_global = static_cast<uint8_t>((mv * mv * 128u + 8064u) / 16129u);
        } else if (e.status == kDrumChannelStatus) {
            if (e.d1 < 16) drum_channel[e.d1] = e.d2 != 0;
        } else if (kind == 0xe) {
            // Питч-бенд: 14 бит со смещением 8192 -> 1/64 полутона; величина
            // абсолютная - SetPitchOffset, а не портаменто.
            const int32_t raw = (static_cast<int32_t>(e.d2) << 7 | static_cast<int32_t>(e.d1)) - 8192;
            mch[ch].bend = static_cast<int16_t>(raw * static_cast<int32_t>(mch[ch].bend_range) * 64 / 8192);
            // И отпущенным, пока звучит релиз: бенд SF2 гнёт все голоса канала,
            // а канал трекера держит смещение до следующей ноты - иначе новая нота с
            // задержкой (колонка эффекта занята) начинается со старого бенда.
            for (uint32_t c = 0; c < kMaxChannels; ++c) {
                if (tch[c].midi != ch || (tch[c].note == 0xff && tch[c].quiet_at <= mt)) continue;
                if (tch[c].bend != mch[ch].bend) put_bend(c, mch[ch].bend);
            }
        } else if (kind == 0xd) {
            mch[ch].pressure = e.d1;
        } else if (kind == 0xc) {
            mch[ch].program = e.d1;
        } else if (kind == 0xb) {
            if (e.d1 == 0) {
                mch[ch].bank_msb = e.d2;
            } else if (e.d1 == 10) {
                mch[ch].cc10 = e.d2;
                for (uint32_t c = 0; c < kMaxChannels; ++c) {
                    if (tch[c].midi != ch || tch[c].note == 0xff) continue;
                    put_pan(c, channel_pan(c, e.d2));
                }
            } else if (e.d1 == 1) {
                mch[ch].cc1 = e.d2;
            } else if (e.d1 == 91) {
                mch[ch].cc91 = e.d2;
            } else if (e.d1 == 5) {
                mch[ch].porta_time = e.d2;
            } else if (e.d1 == 65) {
                mch[ch].porta_on = e.d2 >= 64 ? 1 : 0;
            } else if (e.d1 == 100) {
                mch[ch].rpn_lsb = e.d2;
            } else if (e.d1 == 101) {
                mch[ch].rpn_msb = e.d2;
            }
            // Выбор NRPN снимает выбор RPN (как OpenMPT): Data Entry параметра NRPN
            // (вибрато, срез, настройки ударных GS) иначе переписывал диапазон бенда.
            else if (e.d1 == 98 || e.d1 == 99) {
                mch[ch].rpn_msb = kRpnNone;
                mch[ch].rpn_lsb = kRpnNone;
            } else if (e.d1 == 6 && mch[ch].rpn_msb == kRpnPitchBendRange &&
                       mch[ch].rpn_lsb == kRpnPitchBendRange) {
                // RPN 0 - диапазон питч-бенда в полутонах. Умолчание GM 2; без чтения
                // широкий бенд играл бы узким. Ноль означает ноль: канал с ним
                // бенду не поддаётся, как у эталонного SF2-синтезатора.
                mch[ch].bend_range = e.d2;
            } else if (e.d1 == 7 || e.d1 == 11) {
                if (e.d1 == 7) {
                    mch[ch].cc7 = e.d2;
                } else {
                    mch[ch].cc11 = e.d2;
                }
                for (uint32_t c = 0; c < kMaxChannels; ++c) {
                    if (tch[c].midi == ch && tch[c].note != 0xff) refresh_volume(c);
                }
            } else if (e.d1 == 64) {
                const bool on = e.d2 >= 64;
                if (mch[ch].sustain && !on) {
                    for (uint32_t n = 0; n < 128; ++n) {
                        if (pending[ch * 128 + n]) release_note(ch, static_cast<uint8_t>(n), delay);
                    }
                }
                mch[ch].sustain = on ? 1 : 0;
            } else if (e.d1 == 121) {
                // Reset All Controllers. Педаль снимается здесь: удержанные ноты
                // уходят, как при её отпускании.
                mch[ch].reset_controllers();
                if (mch[ch].sustain) {
                    mch[ch].sustain = 0;
                    for (uint32_t n = 0; n < 128; ++n) {
                        if (pending[ch * 128 + n]) release_note(ch, static_cast<uint8_t>(n), delay);
                    }
                }
                // Сброшенную экспрессию - звучащим каналам.
                for (uint32_t c = 0; c < kMaxChannels; ++c) {
                    if (tch[c].midi == ch && tch[c].note != 0xff) refresh_volume(c);
                }
            } else if (e.d1 == 120 || e.d1 == 123) {
                // All sound off / all notes off - снимаем всё на канале, включая
                // удержанное педалью. Каждую ноту, а не только с живым первым
                // слоем: обрыв по классу и скольжение снимают слои поштучно, и
                // нота с пустым первым слоем и живым вторым иначе висела бы.
                // На ноте без каналов release_note только гасит pending.
                // CC120 глушит голоса сразу, CC123 отпускает клавиши с релизом.
                const bool hard = e.d1 == 120;
                for (uint32_t n = 0; n < 128; ++n) {
                    release_note(ch, static_cast<uint8_t>(n), delay, hard);
                }
            }
        } else if ((kind == 0x9 && e.d2 == 0) || kind == 0x8) {
            // Под педалью нота не гаснет, а ждёт её отпускания.
            if (mch[ch].sustain) {
                pending[ch * 128 + e.d1] = 1;
            } else {
                release_note(ch, e.d1, delay);
            }
        } else if (kind == 0x9) {
            // Клавиша снова нажата - пометка pending снимается: иначе следующее
            // отпускание педали гасит свежую ноту с нажатой клавишей (педаль
            // перехватывают мгновенно: CC64=0 и через миллисекунду 127).
            pending[ch * 128 + e.d1] = 0;
            const uint8_t bank_no = drum_channel[ch] ? 128 : mch[ch].bank_msb;
            uint8_t* slot = held + (ch * 128 + e.d1) * kMaxLayers;
            // Прежние каналы клавиши: повторное нажатие без снятия (две дорожки
            // удваивают партию) берёт новые каналы, а слот перезаписывается.
            uint8_t prev_slot[kMaxLayers];
            std::memcpy(prev_slot, slot, kMaxLayers);
            uint32_t used = 0;
            // Потолок слоёв на ноту тот же, что при воспроизведении (kMaxLayers):
            // сэмплы слоёв, которые звучать не могут, не загружаются. Слою - номер
            // инструмента (потолок ячейки паттерна), сэмплу - номер сэмпла песни.
            bool capped = false;
            bank::NoteLayer picked[kMaxLayers];
            const uint32_t picked_count = select_layers(bank_no, mch[ch].program, e.d1, e.d2, picked, capped);
            if (picked_count == 0) ++(capped ? stats->notes_over_cap : stats->notes_no_zone);
            for (uint32_t k = 0; k < picked_count; ++k) {
                const uint16_t li = picked[k].layer;
                const bank::BankLayer& layer = bank.layers[li];
                const uint16_t inst = song_inst_of[layer.instrument];
                const uint16_t bs_idx = picked[k].bank_sample;
                const int8_t sample_pan = bank.samples[bs_idx].default_panning;
                // Портаменто: если оно включено и предыдущая нота канала ещё звучит тем же
                // инструментом, новая нота не берёт свой канал, а тянет высоту на старом.
                // Тембр обязан совпадать: TonePorta не меняет сэмпл.
                uint32_t pick = kMaxChannels;
                bool glide = false;
                if (mch[ch].porta_on && mch[ch].last_note != 0xff && mch[ch].last_note != e.d1) {
                    for (uint32_t c = 0; c < kMaxChannels; ++c) {
                        if (tch[c].midi != ch || tch[c].note != mch[ch].last_note) continue;
                        if (tch[c].inst != inst || cells[c].note != kNoteNone) continue;
                        // Обе ноты обязан обслуживать один сэмпл. TonePorta сэмпл не переключает, а
                        // инструменты банка мультисэмпловые: у соседней зоны свой строй, и движок
                        // увёл бы высоту к цели, посчитанной по чужой зоне (скольжение C4->C5
                        // доезжает до 405 Гц вместо 523). Переход шире одной зоны keymap играется
                        // обычной нотой.
                        if (tch[c].bs != bs_idx) continue;
                        pick = c;
                        glide = true;
                        break;
                    }
                }
                // Выбор канала. Первым - канал, где эта нота того же канала MIDI ещё
                // звучит (взята заново без снятия); кандидаты копятся в том же проходе
                // и нужны, только если такого нет - звучащий канал в свободные не
                // попадает.
                if (pick == kMaxChannels) {
                    // "Свободен" - клавиша отпущена, но голос ещё может доигрывать релиз.
                    // Взятый канал уводит этот релиз в фоновый пул NNA, где на канал не больше
                    // одного хвоста (второй увод обрывает первый) и всего 32 слота; у
                    // инструментов с exclusiveClass релиз обрывается сразу.
                    // Порядок: канал, где релиз уже отзвучал (сначала свой канал MIDI, потом
                    // любой, внутри - самый давний); затем тот, где релиз кончится раньше
                    // всех; если свободных нет - самый давний вообще.
                    uint32_t same = kMaxChannels, any_free = kMaxChannels, loud = kMaxChannels, oldest = 0;
                    uint32_t rel_same = kMaxChannels;
                    for (uint32_t c = 0; c < kMaxChannels; ++c) {
                        if (tch[c].midi == ch && tch[c].note == e.d1 && cells[c].note == kNoteNone) {
                            pick = c;
                            break;
                        }
                        if (tch[c].note == 0xff && cells[c].note == kNoteNone) {
                            if (tch[c].quiet_at > mt) {
                                // Та же клавиша того же канала MIDI, ещё доигрывающая релиз:
                                // её повтор кладём сюда же. Иначе каждый удар тремоло уходит
                                // на новый канал, и одна клавиша копит десятки хвостов.
                                if (tch[c].midi == ch && tch[c].rel_note == e.d1) {
                                    if (rel_same == kMaxChannels || tch[c].age > tch[rel_same].age) rel_same = c;
                                } else if (loud == kMaxChannels || tch[c].quiet_at < tch[loud].quiet_at) {
                                    loud = c;
                                }
                            } else if (tch[c].midi == ch) {
                                if (same == kMaxChannels || tch[c].age < tch[same].age) same = c;
                            } else if (any_free == kMaxChannels || tch[c].age < tch[any_free].age) {
                                any_free = c;
                            }
                        }
                        if (tch[c].age < tch[oldest].age) oldest = c;
                    }
                    if (pick == kMaxChannels) {
                        // Своя же клавиша - вперёд свободных каналов: новый удар по
                        // ней глушит прежний на любом инструменте, и держать оба
                        // незачем. Иначе тремоло копит по два десятка копий одной
                        // ноты, а вместе с ними и громкость.
                        pick = rel_same != kMaxChannels   ? rel_same
                               : same != kMaxChannels     ? same
                               : any_free != kMaxChannels ? any_free
                               : loud != kMaxChannels     ? loud
                                                          : oldest;
                        if (pick == rel_same) ++stats->note_restacks;
                        if (tch[pick].note != 0xff) ++stats->steals; // все заняты - самый давний
                    }
                }
                // Обрыв по exclusiveClass выписывает конвертер: DCT трекера ищет дубликаты
                // в одном канале, а класс действует на весь канал MIDI, и слои одной ноты
                // лежат на разных каналах.
                const uint8_t excl = glide ? 0 : bank.instruments[layer.instrument].exclusive_class;
                if (excl) {
                    for (uint32_t c = 0; c < kMaxChannels; ++c) {
                        if (c == pick || tch[c].midi != ch || tch[c].excl != excl) continue;
                        // Настоящая нота этой строки (второй слой той же ноты или нота того
                        // же тика) не обрывается и остаётся на учёте: иначе её note-off не
                        // дошёл бы, и следующий удар класса её не оборвал.
                        if (soundsinth::model::is_real_note(cells[c].note)) continue;
                        // Обрыв - нотой kNoteCut на задержке новой ноты, снятие этой строки
                        // (kNoteOff) он поглощает. Канал с ней на этой строке занят: следующий
                        // слой не ляжет поверх и не будет оборван на тике 1, как с NoteCut в
                        // колонке эффекта.
                        cells[c].note = soundsinth::model::kNoteCut;
                        if (delay) {
                            put_note_effect(c, Effect::NoteDelay, delay);
                        } else if (cells[c].effect.type == Effect::NoteDelay) {
                            cells[c].effect = soundsinth::model::EffectCommand{};
                        }
                        forget_channel(c);
                        tch[c].note = 0xff;
                        tch[c].excl = 0;
                        tch[c].quiet_at = mt; // оборван: тихо сразу
                    }
                }

                if (tch[pick].midi != ch || tch[pick].note != e.d1) forget_channel(pick);
                cells[pick].note = e.d1;
                // При портаменто колонка инструмента пустая: с ней движок перезапустил бы
                // сэмпл и огибающую.
                if (!glide) {
                    cells[pick].instrument = static_cast<uint16_t>(inst + 1); // в ячейке индекс с единицы
                } else {
                    // Скорость в 1/64 полутона за тик = param*4. Время CC5 -> темп скольжения:
                    // 0 - мгновенно, 127 - октава примерно за 2.5 с. Точной шкалы у CC5 нет ни
                    // в GM, ни в GS.
                    // Тиков в секунду - по текущему темпу, а не стартовому: иначе после
                    // смены темпа скольжение шло бы быстрее или медленнее задуманного.
                    const uint32_t rate = 600u / (static_cast<uint32_t>(mch[ch].porta_time) + 1u);
                    uint32_t pp = rate * 16u / engine_ticks_per_second(cur_tempo);
                    if (pp < 1) pp = 1;
                    if (pp > 255) pp = 255;
                    put_note_effect(pick, Effect::TonePorta, static_cast<uint8_t>(pp));
                    // Скорость pp*4 единиц 1/64 полутона за тик - отсюда число строк, на
                    // которых команду придётся повторять.
                    const uint32_t dist =
                        static_cast<uint32_t>((e.d1 > mch[ch].last_note ? e.d1 - mch[ch].last_note
                                                                        : mch[ch].last_note - e.d1)) *
                        64u;
                    const uint32_t ticks = (dist + pp * 4u - 1u) / (pp * 4u);
                    uint32_t rows = (ticks + grid.ticks_per_row - 1u) / grid.ticks_per_row;
                    if (rows > 0xffffu) rows = 0xffffu;
                    tch[pick].glide = static_cast<uint16_t>(rows);
                    // Канал переезжает со старой ноты на новую, иначе снятие старой погасило
                    // бы новую.
                    uint8_t* prev = held + (ch * 128 + mch[ch].last_note) * kMaxLayers;
                    for (uint32_t k = 0; k < kMaxLayers; ++k) {
                        if (prev[k] == pick) prev[k] = 0xff;
                    }
                }
                cells[pick].volume.type = VolumeColumnType::SetVolume;
                cells[pick].volume.param = cell_volume(e.d2, mch[ch].cc7, mch[ch].cc11);
                tch[pick].pend_vol = false; // velocity новой ноты уже по текущим CC7/CC11
                // При скольжении колонку уже занял TonePorta - он и есть смысл ноты,
                // подстройка на долю строки для легатной ноты значит меньше.
                if (delay && !glide) put_note_effect(pick, Effect::NoteDelay, delay);
                tch[pick].midi = ch;
                tch[pick].note = e.d1;
                tch[pick].excl = excl;
                tch[pick].vel = e.d2;
                tch[pick].inst = inst;
                tch[pick].rel_ms = bank.instruments[layer.instrument].fadeout_ms;
                tch[pick].bs = bs_idx;
                tch[pick].span = static_cast<uint8_t>(sample_pan < 0 ? 32 : sample_pan);
                // Канал трекера мог прийти от другого канала MIDI со своим бендом:
                // bend_offset движка живёт, пока не переуказан, поэтому сверка всегда.
                if (tch[pick].bend != mch[ch].bend) put_bend(pick, mch[ch].bend);
                // Панорама канала MIDI - поверх панорамы зоны банка: у ударных набор
                // разведён по стерео самим банком.
                // На строке ноты без задержки - колонкой эффекта: она идёт после
                // постановки ноты, и панорама встаёт с тика 0, а не строкой позже.
                // Эффект делит на 4 и держит 0..63: крайнее правое 64 (целый край
                // стереопары) и задержанная нота - колонкой громкости следующей
                // строкой.
                if (mch[ch].cc10 != 64) {
                    const uint8_t pv = channel_pan(pick, mch[ch].cc10);
                    if (!delay && pv < 64 && cells[pick].effect.type == Effect::None) {
                        cells[pick].effect.type = Effect::SetPanning;
                        cells[pick].effect.param = static_cast<uint8_t>(pv * 4);
                        tch[pick].pend_pan = 0xff;
                    } else {
                        put_pan(pick, pv);
                    }
                }
                tch[pick].age = ++age;
                slot[used++] = static_cast<uint8_t>(pick);
            }
            // Прежние каналы клавиши, не взятые заново, не теряются: иначе они не
            // получили бы note-off и висели до следующего нажатия той же клавиши.
            // Дописываются в слот, пока есть место, - первый note-off снимет оба
            // экземпляра, как эталонный синтезатор; лишние снимаются сразу. Только
            // каналы, всё ещё звучащие этой клавишей: украденный уже чужой.
            for (uint32_t k = 0; k < kMaxLayers; ++k) {
                const uint8_t c = prev_slot[k];
                if (c >= kMaxChannels || tch[c].midi != ch || tch[c].note != e.d1) continue;
                bool taken = false;
                for (uint32_t j = 0; j < used; ++j) {
                    taken = taken || slot[j] == c;
                }
                if (taken) continue;
                if (used < kMaxLayers) {
                    slot[used++] = c;
                } else {
                    release_channel(c, delay);
                }
            }
            for (uint32_t k = used; k < kMaxLayers; ++k) {
                slot[k] = 0xff;
            }
            // Откуда тянуть следующую ноту при портаменто. Ставится всегда: портаменто
            // могут включить между двумя нотами.
            mch[ch].last_note = e.d1;
        }
    }

    // Конец строки: посыл реверберации, скольжение, догон бенда, вибрато.
    void finish_row(uint64_t row_mt) {
        // После событий и смены темпа - один проход, для канала в прежнем порядке:
        // посыл реверберации, скольжение, бенд, вибрато. Каждый шаг трогает только
        // ячейку и состояние своего канала.
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            const bool sounding = tch[c].midi < 16 && tch[c].note != 0xff;
            // Посыл реверберации держится в канале, пока не переуказан, и меняется в
            // файлах редко - колонку берёт только при расхождении.
            if (sounding) {
                const uint8_t want = mch[tch[c].midi].cc91;
                if (tch[c].rev != want && cells[c].effect.type == Effect::None) {
                    cells[c].effect.type = Effect::SetReverbSend;
                    cells[c].effect.param = want;
                    tch[c].rev = want;
                    if (want != 0) reverb_used = true;
                }
            }

            // Ведение скольжения - после посыла реверберации, до бенда и вибрато: это
            // сама высота ноты, бенд её смещает, вибрато качает. Параметр 0 -
            // повторить запомненную скорость, как Gxx без параметра. Счётчик строк
            // убывает только на строке, где TonePorta выписан: на строке с занятой
            // колонкой эффекта скольжение стоит, иначе оно обрывается до цели.
            if (tch[c].glide != 0) {
                if (tch[c].note == 0xff) {
                    tch[c].glide = 0;
                } else if (cells[c].effect.type == Effect::None) {
                    --tch[c].glide;
                    cells[c].effect.type = Effect::TonePorta;
                    cells[c].effect.param = 0;
                }
            }

            // Догон бенда - после всех событий строки. Очередь на колонку эффекта:
            // тайминг ноты (задержка, обрыв, снятие) важнее бенда - промах по времени
            // слышен, промах по высоте на одну строку нет. Занятую колонку закроет
            // следующая строка: бенд абсолютный, пропасть не может. Сверка по
            // последнему выписанному значению.
            if (tch[c].midi < 16 && (tch[c].note != 0xff || tch[c].quiet_at > row_mt) &&
                tch[c].bend != mch[tch[c].midi].bend) {
                put_bend(c, mch[tch[c].midi].bend);
            }

            // Вибрато по CC1 - последним: пропуск строки - строка без покачивания, а
            // промах тайминга слышен сразу, панорама и бенд держат значение.
            // Место вибрато - колонка громкости: команду приходится повторять каждую
            // строку (vibrato_active сбрасывается построчно), а колонка эффекта нужна
            // бенду. На строке ноты колонку громкости занимает velocity - тогда колонка
            // эффекта, если свободна.
            if (!sounding) continue;
            // Размах = 255 * (param<<2) / 64 единиц 1/64 полутона, 15.94 на шаг
            // параметра. Полные 50 центов SF2 - 32 единицы, ровно param 2; у SF2
            // глубина линейна по CC1, здесь - ближайший шаг: CC1 ниже 32 - без
            // вибрато, 32..95 - 1, от 96 - 2. Шкала одна на обе колонки, иначе
            // глубина прыгала бы от строки к строке.
            // CC1 и давление канала качают высоту одинаково и складываются;
            // потолок 3 - нибл FineVibrato держит глубину до 15 (3 * 4).
            const uint32_t wheel =
                static_cast<uint32_t>(mch[tch[c].midi].cc1) + mch[tch[c].midi].pressure;
            const uint32_t steps = (wheel * 2u + 63u) / 127u;
            const uint8_t depth = static_cast<uint8_t>(steps > 3u ? 3u : steps);
            // Ноль не выписываем, скорость тоже: без команды вибрато гаснет к
            // следующей строке.
            if (depth == 0) continue;
            if (!tch[c].vib_speed_set) {
                if (cells[c].volume.type != VolumeColumnType::None) {
                    ++stats->vib_lost;
                    continue;
                }
                cells[c].volume.type = VolumeColumnType::VibratoSpeed;
                cells[c].volume.param = vibrato_speed;
                tch[c].vib_speed_set = true;
                continue;
            }
            const bool own_volume = cells[c].note == kNoteNone &&
                                    cells[c].volume.type == VolumeColumnType::SetVolume;
            if (cells[c].volume.type == VolumeColumnType::None) {
                cells[c].volume.type = VolumeColumnType::VibratoDepth;
                cells[c].volume.param = depth;
            } else if (cells[c].effect.type == Effect::None) {
                // FineVibrato берёт глубину без сдвига: тот же размах даёт нибл depth*4.
                cells[c].effect.type = Effect::FineVibrato;
                cells[c].effect.param = static_cast<uint8_t>((vibrato_speed << 4) | (depth * 4));
            } else if (cells[c].effect.type == Effect::SetPanning) {
                // Панорама уступает: промах панорамы на строку тише промаха высоты,
                // и её догоняет pend_pan.
                tch[c].pend_pan = static_cast<uint8_t>(cells[c].effect.param >> 2);
                cells[c].effect.type = Effect::FineVibrato;
                cells[c].effect.param = static_cast<uint8_t>((vibrato_speed << 4) | (depth * 4));
            } else if (own_volume) {
                // Громкость канала (не velocity этой строки) уезжает на следующую
                // строку через pend_vol - туда же, куда её кладёт refresh_volume.
                tch[c].pend_vol = true;
                cells[c].volume.type = VolumeColumnType::VibratoDepth;
                cells[c].volume.param = depth;
            } else {
                ++stats->vib_lost;
            }
        }
    }

    // Первая свободная колонка эффекта строки - темпу и общей громкости.
    // false - все заняты, писателю ждать следующей строки.
    bool write_row_effect(Effect type, uint8_t param) {
        for (uint32_t c = 0; c < kMaxChannels; ++c) {
            if (cells[c].effect.type != Effect::None) continue;
            cells[c].effect.type = type;
            cells[c].effect.param = param;
            return true;
        }
        return false;
    }

    // Новый темп. Частота вибрато - тиков в секунду * speed / 64: с новым
    // темпом скорость выписывается заново, иначе вместо 5 Гц было бы
    // 5 * новый / старый.
    void set_tempo(uint8_t tempo) {
        cur_tempo = tempo;
        const uint8_t vs = vibrato_speed_at(cur_tempo);
        if (vs == vibrato_speed) return;
        vibrato_speed = vs;
        for (uint32_t c = 0; c < kMaxChannels; ++c) tch[c].vib_speed_set = false;
    }
};

} // namespace soundsinth::formats::midi
