// SPDX-License-Identifier: MIT
#include "devices/gs/gs_device.h"

#include <cstring>

#include "platform/hot_path.h"

// Сумма та же, что у банка и настроек: реестр узнаваемых программ построен
// на ней, и своя копия алгоритма разошлась бы с ним молча.
#include "core/util/crc32.h"

namespace devices::gs {

namespace {

// Коды команд по руководству GS v1.04 ред.006.
constexpr uint8_t kCmdResetFlags = 0x00u;
// Прямой вывод в ЦАП и громкости, #01..#0B.
constexpr uint8_t kCmdSetSilence     = 0x01u;
constexpr uint8_t kCmdSetLowVolume   = 0x02u;
constexpr uint8_t kCmdSetHighVolume  = 0x03u;
constexpr uint8_t kCmdSetE3Bits      = 0x04u;
constexpr uint8_t kCmdOutVolumePort  = 0x05u;
constexpr uint8_t kCmdSendToDac      = 0x06u;
constexpr uint8_t kCmdSendToDacVol   = 0x07u;
constexpr uint8_t kCmdResetFlags2    = 0x08u; // то же, что #00
constexpr uint8_t kCmdSetByteVolume  = 0x09u;
constexpr uint8_t kCmdDacOutput      = 0x0au;
constexpr uint8_t kCmdDacAndVolume   = 0x0bu;
constexpr uint8_t kCmdOutPort        = 0x10u;
constexpr uint8_t kCmdInPort         = 0x11u;
constexpr uint8_t kCmdOutPort0       = 0x12u;
constexpr uint8_t kCmdJump           = 0x13u;
constexpr uint8_t kCmdLoadBlock      = 0x14u;
constexpr uint8_t kCmdGetBlock       = 0x15u;
constexpr uint8_t kCmdPoke           = 0x16u;
constexpr uint8_t kCmdPeek           = 0x17u;
constexpr uint8_t kCmdLoadDe         = 0x18u;
constexpr uint8_t kCmdPokeDe         = 0x19u;
constexpr uint8_t kCmdPeekDe         = 0x1au;
constexpr uint8_t kCmdIncDe          = 0x1bu;
constexpr uint8_t kCmdPoke20         = 0x1cu;
constexpr uint8_t kCmdPeek20         = 0x1du;
constexpr uint16_t kPeek20Base       = 0x2000u; // #1D читает 0x2000 + аргумент
constexpr uint8_t kCmdTotalRam       = 0x20u;
constexpr uint8_t kCmdFreeRam        = 0x21u;
constexpr uint8_t kCmdPages          = 0x23u;
constexpr uint8_t kCmdModuleVolume   = 0x2au;
constexpr uint8_t kCmdFxMasterVolume = 0x2bu;
constexpr uint8_t kCmdSetCurFx       = 0x2eu;
constexpr uint8_t kCmdLoadModule     = 0x30u;
// #31..#33 (транспорт модуля) объявлены в заголовке: их называет и мост.
constexpr uint8_t kCmdModuleVolumeAlt = 0x35u; // тот же #2A другим кодом
constexpr uint8_t kCmdDataOn          = 0x36u; // положить в регистр вывода 0xff
constexpr uint8_t kCmdReinit          = 0x37u;
constexpr uint8_t kCmdLoadFx          = 0x38u; // сэмпл эффекта, беззнаковый
constexpr uint8_t kCmdPlayFx          = 0x39u;
constexpr uint8_t kCmdStopFx          = 0x3au;
constexpr uint8_t kCmdFxVolume        = 0x3du;
constexpr uint8_t kCmdLoadFxExt       = 0x3eu; // он же со знаком при аргументе #01
// Параметры текущего сэмпла, #40..#49. #43, #44 и #4A..#4F зарезервированы
// руководством и остаются неопознанными.
constexpr uint8_t kCmdFxNote          = 0x40u;
constexpr uint8_t kCmdFxVolumeOne     = 0x41u;
constexpr uint8_t kCmdFxFinetune      = 0x42u;
constexpr uint8_t kCmdFxPriority      = 0x45u;
constexpr uint8_t kCmdFxSeekFirst     = 0x46u;
constexpr uint8_t kCmdFxSeekLast      = 0x47u;
constexpr uint8_t kCmdFxLoopBegin     = 0x48u;
constexpr uint8_t kCmdFxLoopEnd       = 0x49u;
constexpr uint8_t kCmdSongPosition    = 0x60u;
constexpr uint8_t kCmdPatternPosition = 0x61u;
constexpr uint8_t kCmdMixPosition     = 0x62u;
constexpr uint8_t kCmdPlayerMode      = 0x6au;
constexpr uint8_t kCmdRelooper        = 0x6bu;
constexpr uint8_t kCmdStreamOpen      = 0xd1u;
constexpr uint8_t kCmdStreamClose     = 0xd2u;
constexpr uint8_t kCmdWarmReset       = 0xf3u;
constexpr uint8_t kCmdColdReset       = 0xf4u;
constexpr uint8_t kCmdBusyOn          = 0xf5u;
constexpr uint8_t kCmdBusyOff         = 0xf6u;
constexpr uint8_t kCmdGetHx           = 0xf7u;

// Команды залитого проигрывателя MIDI (блоба), #00..#04. Коды пересекаются
// с набором прошивки: пока блоб работает, их разбирает он.
constexpr uint8_t kMidiCmdFrame = 0x00u; // порция байт очередного кадра
constexpr uint8_t kMidiCmdState = 0x01u; // блок состояния, следующий код - аргумент
constexpr uint8_t kMidiCmdReady = 0x02u; // готовность; в обычном наборе - Set low volume
constexpr uint8_t kMidiCmdNames = 0x03u; // список имён дорожек
constexpr uint8_t kMidiCmdInfo  = 0x04u; // справка о файле

// Внутренние порты карты.
constexpr uint8_t kInnerPortMask = GsDevice::kInnerPortCount - 1u;
static_assert((GsDevice::kInnerPortCount & kInnerPortMask) == 0, "the number of internal ports is a power of two");
constexpr uint8_t kInnerPortPages             = 0x00u;
constexpr uint8_t kInnerPortCommand           = 0x01u;
constexpr uint8_t kInnerPortData              = 0x02u;
constexpr uint8_t kInnerPortOut               = 0x03u;
constexpr uint8_t kInnerPortStatus            = 0x04u;
constexpr uint8_t kInnerPortClearCommand      = 0x05u;
constexpr uint8_t kInnerPortVolume4           = 0x09u;
constexpr uint8_t kInnerPortDataFromPages     = 0x0au;
constexpr uint8_t kInnerPortCommandFromVolume = 0x0bu;
// Биты, которые читают #0A и #0B.
constexpr uint8_t kPagesDataBit     = 0x01u; // порт #00, D0
constexpr uint8_t kVolumeCommandBit = 0x20u; // порт #09, D5

constexpr uint8_t kHxBusyBit = 0x80u; // регистр HX (#F7), D7

// Ответ #62: биты 7-6 - младшие два бита позиции, биты 5-0 - строка.
constexpr uint8_t kMixOrderBits = 0x03u;
constexpr uint8_t kMixOrderLsb  = 6;
constexpr uint8_t kMixRowMask   = 0x3fu;

// Длина хвоста #14/#15: LEN.H, ADR.L, ADR.H (LEN.L - аргумент до кода).
constexpr uint8_t kBlockTailBytes = 3;
// Длина хвоста #48/#49: LEN.M и LEN.H (LEN.L - аргумент до кода).
constexpr uint8_t kLoopTailBytes = 2;

// Прямой запуск сэмпла в названный канал, #80-#9B. Шестнадцать кодов, и
// различаются они только тем, сколько байт идёт после кода: бит 3 - следом
// нота, бит 4 - следом громкость, биты 0-1 - номер канала. Ловятся маской,
// а не шестнадцатью ветвями: каждая ветвь в write_command стоит десятки
// байт ОЗУ.
constexpr uint8_t kDirectPlayMask = 0xe4u;
constexpr uint8_t kDirectPlayCode = 0x80u;
// Смена ноты и громкости канала на лету, #A0-#A3 и #A8-#AB. Значение -
// аргументом до кода, хвоста нет; бит 3 выбирает, что меняется.
constexpr uint8_t kChannelSetMask = 0xf4u;
constexpr uint8_t kChannelSetCode = 0xa0u;
constexpr uint8_t kWithNoteBit    = 0x08u;
constexpr uint8_t kWithVolumeBit  = 0x10u;
constexpr uint8_t kDirectChanMask = 0x03u;
// Старший байт начала петли #FF означает "петли нет" - так пишет руководство.
constexpr uint8_t kLoopOffHigh = 0xffu;

// Переменные ПЗУ карты: значения, которые ПЗУ GS 1.04 и 1.05a пишет при
// инициализации. Z-Player 4.1 читает #4151 и при нуле ждёт бит данных вечно.
struct RomVar {
    uint16_t addr;
    uint8_t value;
};
SOUNDSINTH_HOT_PATH_ATTR("gs_rom_vars")
// Строка на адрес: это запись замера ПЗУ, и сверять её с картой надо
// построчно. Форматтер иначе собирает все записи в одну строку.
// clang-format off
constexpr RomVar kRomVars[] = {
    // Версия прошивки: софт читает #0004 и #0005 и печатает их как L.H
    // в шестнадцатеричных тетрадах. Нули дали бы версию 0.00, какой нет
    // ни у одной карты.
    {0x0004u, kRomVersionHigh},
    {0x0005u, kRomVersionLow},
    {0x409fu, 0x0fu},
    {0x40a4u, 0x40u},
    {0x4151u, 0xc3u},
};
// clang-format on

// Память карты не эмулируется: известные переменные ПЗУ - из таблицы,
// остальное - ноль.
SOUNDSINTH_HOT_PATH_ATTR("gs_mem_peek")
uint8_t mem_peek(uint16_t addr) {
    for (const RomVar& v : kRomVars) {
        if (v.addr == addr) return v.value;
    }
    return 0;
}

// Слово из младшего и старшего байта хвоста.
constexpr uint16_t make_word(uint8_t lo, uint8_t hi) {
    return static_cast<uint16_t>(lo | (hi << 8));
}

// Счётчик гистограммы команд насыщается.
constexpr uint8_t kHistMax = 0xffu;

} // namespace

SOUNDSINTH_HOT_PATH_ATTR("gs_reset")
void GsDevice::reset() {
    status_               = kStatusIdle;
    data_pending_         = false;
    last_cmd_             = 0;
    arg_                  = 0;
    has_arg_              = false;
    tail_left_            = 0;
    de_                   = 0;
    sink_left_            = 0;
    zeros_left_           = 0;
    resp_n_               = 0;
    resp_pos_             = 0;
    stream_               = Stream::None;
    stream_bytes_         = 0;
    out_reg_              = 0;
    hx_busy_              = false;
    cur_fx_               = 0;
    blob_window_          = 0;
    blob_is_loader_       = false;
    blob_crc_             = soundsinth::util::kCrc32Init;
    blob_skip_at_         = 0;
    blob_len_             = 0;
    midi_blob_loaded_     = false;
    midi_active_          = false;
    midi_ready_           = false;
    midi_await_state_arg_ = false;
    midi_facts_           = MidiFileFacts{};
    midi_frame_           = nullptr;
    midi_frame_len_       = 0;
    midi_frame_wanted_.store(false, std::memory_order_relaxed);
    midi_tracks_      = nullptr;
    midi_track_count_ = 0;
    for (uint8_t& b : midi_state_) {
        b = 0;
    }
    blob_answer_len_        = 0;
    block_ptr_              = nullptr;
    block_left_             = 0;
    module_image_           = nullptr;
    module_len_             = 0;
    resident_active_        = false;
    resident_await_pattern_ = false;
    fx_count_               = 0;
    fx_loading_             = kFxNone;
    fx_active_              = 0;
    fx_stop_mask_           = 0;
    fx_pending_handle_      = 0;
    pending_event_          = Event::None;
    fx_start_               = FxStart{};
    for (uint8_t& p : fx_ch_priority_) {
        p = 0;
    }
    // Таблица сэмплов не стирается: читают её только через fx_slot, а тот
    // пускает номера не дальше fx_count_, обнулённого выше; новую запись
    // стирает #38. Стирание 32 записей стоит #F3 около 550 тактов в
    // обработчике шины.
    for (uint8_t& n : served_note_) {
        n = 0xffu; // не отдавали ещё: первая выдача будет "сменилась"
    }
    module_volume_    = kModuleVolumeMax; // карта включается на полной громкости
    fx_master_volume_ = kFxVolumeDefault;
    fx_volume_        = kFxVolumeDefault;
    fx_volume_eff_    = kFxVolumeDefault;
    // unknown_* не сбрасываются: это мера покрытия за всё время работы.
}

SOUNDSINTH_HOT_PATH_ATTR("gs_refresh_data_status")
void GsDevice::refresh_data_status() {
    status_ = (block_left_ > 0 || zeros_left_ > 0 || resp_pos_ < resp_n_ || data_pending_) ? kStatusData : kStatusIdle;
}

SOUNDSINTH_HOT_PATH_ATTR("gs_respond")
void GsDevice::respond(const uint8_t* bytes, uint8_t n) {
    if (n > sizeof(resp_)) {
        n = sizeof(resp_);
    }
    std::memcpy(resp_, bytes, n);
    resp_n_   = n;
    resp_pos_ = 0;
    refresh_data_status(); // софт ждёт на бите данных (WN)
}

SOUNDSINTH_HOT_PATH_ATTR("gs_respond_u24")
void GsDevice::respond_u24(uint32_t v) {
    const uint8_t b[3] = {static_cast<uint8_t>(v & 0xffu), static_cast<uint8_t>((v >> 8) & 0xffu), static_cast<uint8_t>((v >> 16) & 0xffu)};
    respond(b, 3);
}

SOUNDSINTH_HOT_PATH_ATTR("gs_respond_u8")
void GsDevice::respond_u8(uint8_t v) {
    respond(&v, 1);
}

SOUNDSINTH_HOT_PATH_ATTR("gs_write_data")
bool GsDevice::write_data(uint8_t value) {
    last_data_ = value; // порт связи #02 отдаёт его обратно
    // Хвост команды разбирается раньше потока: #13 открывает поток, а
    // старший байт адреса приходит уже после него, и в модуль он не идёт.
    // Вне этого случая хвост и поток не встречаются - команда посреди
    // потока проглатывается целиком.
    if (tail_left_ > 0) {
        take_tail(value);
        return false;
    }
    if (stream_ != Stream::None) {
        // Бит данных остаётся снятым: "байт принят, давай следующий".
        // Обратного давления (взвести бит при забитом кольце приёма)
        // нет.
        ++stream_bytes_;
        return true;
    }
    if (sink_left_ > 0) {
        --sink_left_;
        return false;
    }
    // Вне потока запись в регистр данных - аргумент будущей команды:
    // софт кладёт их до подачи кода (SD ... SC).
    if (!has_arg_) {
        arg_     = value;
        has_arg_ = true;
    }
    // Байт лежит в регистре, и забрать его некому: главный цикл прошивки
    // регистр данных не читает вовсе. Бит данных остаётся взведённым до
    // команды - замерено прогоном ПЗУ 1.08, и на этом стоит опознание
    // карты у Z-Player 5.1 (#D1E8).
    data_pending_ = true;
    refresh_data_status();
    return false;
}

namespace {

// Узнанные программы, которые плеер заливает в карту. Процессора карты нет,
// залитый код не исполняется: программа опознаётся по сумме и длине, а
// вместо неё отдаётся то же, что отдала бы она. Суммы сняты с исходников
// Z-Player, восстановленных побайтно.
//
// Адрес запуска в ключ не входит: программы различаются суммой и длиной.
struct BlobAnswer {
    uint32_t crc;
    uint16_t len;
    uint8_t bytes; // сколько байт ответа
    uint8_t low;   // первый
    uint8_t high;  // второй, если bytes == 2
};

// Частота карты, которую мы о себе объявляем: у классической GS 12 МГц.
constexpr uint8_t kDeclaredClockMhz = 12;

// Резидент плеера: отвечает на свои команды и живёт, пока не придёт чужая.
// Три разных тела на всю линейку Z-Player, назначение одно.
struct ResidentBlob {
    uint32_t crc;
    uint16_t len;
};
constexpr ResidentBlob kResidentBlobs[] = {
    {0x5111d33du, 188}, // 3.1
    {0x8f6c6b6bu, 370}, // 3.2f, 3.3, 3.4
    {0x050898d8u, 428}, // 4.0, 4.1, 5.0, 5.1
};

constexpr BlobAnswer kBlobAnswers[] = {
    // Замер тактовой частоты, версии 4.0 и 4.1: один байт - частота в МГц.
    {0xdf9a53b5u, 125, 1, kDeclaredClockMhz, 0},
    // Он же в 5.0 и 5.1: два байта счёта, младший вперёд. Цикл откалиброван
    // так, что счёт равен мегагерцам.
    {0x678b0a0bu, 103, 2, kDeclaredClockMhz, 0},
    // Проба регистра настроек клона (внутренний порт #0F). Ноль значит
    // "регистра нет", то есть карта не NeoGS; дальше плеер проверяет шлейф
    // командой #10 и печатает Classic. Без ответа он читает остаток
    // регистра вывода и по ненулевому объявляет нас NeoGS.
    {0x0fa4c7a8u, 64, 1, 0x00u, 0},
    // Проба процессора: #FF - КМОП Z84C, то есть настоящая карта. По #00
    // плеер печатает Z084 и считает, что запущен под эмулятором.
    {0xc0f282c7u, 15, 1, 0xffu, 0},
};

// Проигрыватель MIDI 5.0 и 5.1: 2356 байт по #5000. Семь байт плеер правит под
// файл; в сумму они идут нулём, и сумма у обеих версий одна. Прочие заливки
// короче самого раннего из этих мест.
constexpr uint16_t kMidiPatchOffsets[] = {0x62du, 0x62eu, 0x63cu, 0x76bu, 0x7bcu, 0x853u, 0x854u};
constexpr uint8_t kMidiPatchCount      = static_cast<uint8_t>(sizeof(kMidiPatchOffsets) / sizeof(kMidiPatchOffsets[0]));
constexpr uint16_t kMidiBlobLen        = 2356;
constexpr uint32_t kMidiBlobCrc        = 0x5502a4f0u;

// Ответ на пробу #02. Нулём мы бы сказали "проигрыватель MIDI в карте
// работает", и плеер встал бы навсегда в ожидании 1536 байт списка
// дорожек. Любое ненулевое значение он понимает как "карты нет" и
// отказывается от MIDI, не теряя управления.
constexpr uint8_t kMidiNotReady = 0xffu;
// Конец порции кадра у команды #00.
constexpr uint8_t kMidiFrameEnd = 0xffu;

} // namespace

void GsDevice::set_telemetry(uint8_t order, uint8_t row, const uint8_t notes[kModuleChannels], const uint8_t levels[kModuleChannels], uint8_t speed,
                             bool playing) {
    telemetry_seq_ = telemetry_seq_ + 1u; // нечётный: запись идёт
    std::atomic_signal_fence(std::memory_order_seq_cst);
    uint8_t* const t   = telemetry_staged_;
    t[kTelemetryOrder] = order;
    t[kTelemetryRow]   = static_cast<uint8_t>(row & 0x3fu); // плеер всё равно маскирует
    bool sounding      = false;
    for (uint8_t c = 0; c < kModuleChannels; ++c) {
        // Уровень канала карта отдаёт отсчётом вокруг средней точки #80;
        // плеер рисует столбик как половину отклонения. Тишина - ровно #80.
        const uint8_t v         = levels[c] > kChannelVolumeMax ? kChannelVolumeMax : levels[c];
        t[kTelemetryLevels + c] = v == 0 ? kSilenceLevel : static_cast<uint8_t>(kSilenceLevel + v);
        if (v != 0) sounding = true;
        // Молчащий канал карта объявляет значением 127, а не последней
        // сыгранной нотой. Старший бит у ноты свой (признак "не менялось"),
        // поэтому в неё он попасть не должен.
        uint8_t note = notes[c];
        if (v == 0 || note >= kNoteNoneIn || note > kNoteSilent) note = kNoteSilent;
        t[kTelemetryNotes + c] = note;
    }
    t[kTelemetrySpeed] = playing ? static_cast<uint8_t>((speed & 0x7fu) | (sounding ? 0x80u : 0u)) : 0u;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    telemetry_seq_ = telemetry_seq_ + 1u;
}

// Отдать килобайт паттерна по номеру позиции. Раскладка ProTracker: 20
// байт названия, 31 запись о сэмплах по 30, длина порядка, таблица
// порядка на 128 байт, метка, дальше паттерны подряд по 1024 байта. Номер
// паттерна берётся из таблицы порядка, как это делает и прошивка карты.
//
// Модуля нет или позиция за его пределами - отдаются нули: плеер покажет
// пустой паттерн, но не встанет.
void GsDevice::arm_pattern(uint8_t position) {
    block_left_ = 0;
    block_ptr_  = nullptr;
    ++pattern_asks_;
    last_pattern_pos_       = position;
    last_pattern_num_       = 0xffu; // не нашлось - видно в журнале
    const uint32_t order_at = static_cast<uint32_t>(kModOrderTable) + position;
    if (module_image_ != nullptr && position < kModOrderCount && module_len_ > order_at) {
        const uint8_t pattern = module_image_[order_at];
        last_pattern_num_     = pattern;
        const uint32_t at     = kModPatternsAt + static_cast<uint32_t>(pattern) * kPatternBytes;
        if (at + kPatternBytes <= module_len_) {
            block_ptr_  = module_image_ + at;
            block_left_ = kPatternBytes;
            ++pattern_served_;
            refresh_data_status();
            return;
        }
    }
    zeros_left_ = kPatternBytes;
    refresh_data_status();
}

// Залитое - проигрыватель MIDI? Сумма считается в обход семи мест, которые
// плеер правит под файл.
SOUNDSINTH_HOT_PATH_ATTR("gs_blob_is_midi")
bool GsDevice::blob_is_midi() const {
    return blob_len_ == kMidiBlobLen && blob_crc_ == kMidiBlobCrc;
}

// Команды залитого проигрывателя MIDI. Коды #00-#04 перекрывают обычные,
// поэтому разбираются только пока блоб жив; любой другой код его кончает.
//
// Что отдаёт каждая - из разбора плеера: #00 порция байт кадра с #FF в
// конце, #01 блок состояния на 135 байт (аргумент приходит ВТОРЫМ байтом
// в порт команд), #02 байт готовности (ноль - блоб работает), #03 список
// имён дорожек, #04 справка о файле.
SOUNDSINTH_HOT_PATH_ATTR("gs_midi_command")
Event GsDevice::midi_command(uint8_t value) {
    if (midi_await_state_arg_) {
        // Это аргумент #01, а не команда: с него начинается видимое окно
        // списка дорожек. Плеер красит по нему тринадцать строк, а байты
        // по краям - стрелки "есть выше" и "есть ниже".
        midi_await_state_arg_ = false;
        const uint8_t first   = value;
        uint8_t above         = 0;
        uint8_t below         = 0;
        for (uint8_t i = 0; i < midi_track_count_; ++i) {
            const uint8_t flag = midi_tracks_ != nullptr ? midi_tracks_[i] : 0u;
            if (i < first) {
                above |= flag;
            } else if (i >= static_cast<uint8_t>(first + kMidiStateRows)) {
                below |= flag;
            }
        }
        midi_state_[0] = above;
        for (uint8_t r = 0; r < kMidiStateRows; ++r) {
            const uint8_t track = static_cast<uint8_t>(first + r);
            midi_state_[1 + r]  = (midi_tracks_ != nullptr && track < midi_track_count_) ? midi_tracks_[track] : 0u;
        }
        midi_state_[1 + kMidiStateRows] = below;
        block_ptr_                      = midi_state_;
        block_left_                     = kMidiStateBytes;
        refresh_data_status();
        return Event::None;
    }
    switch (value) {
        case kMidiCmdFrame:
            if (const uint16_t len = midi_frame_len_; len != 0 && midi_frame_ != nullptr) {
                std::atomic_signal_fence(std::memory_order_acquire);
                block_ptr_  = midi_frame_;
                block_left_ = len;
                // Отданную порцию забываем: не успей основной поток
                // приготовить следующую, плеер получил бы ту же самую
                // второй раз, то есть те же ноты дважды. Пустая порция -
                // это пропущенный кадр, слышно её не будет.
                midi_frame_len_ = 0;
            } else {
                // Играть нечего: пустая порция - это один #FF.
                respond_u8(kMidiFrameEnd);
                ++midi_frames_empty_;
            }
            ++midi_frames_served_;
            midi_frame_wanted_.store(true, std::memory_order_release);
            refresh_data_status();
            return Event::MidiFrame;
        case kMidiCmdState: // следующий код - аргумент
            midi_await_state_arg_ = true;
            return Event::None;
        case kMidiCmdReady: // ноль - блоб работает
            respond_u8(midi_ready_ ? 0u : kMidiNotReady);
            return Event::None;
        case kMidiCmdNames:
            if (midi_names_ != nullptr) {
                block_ptr_  = midi_names_;
                block_left_ = kMidiNamesBytes;
            } else {
                zeros_left_ = kMidiNamesBytes;
            }
            refresh_data_status();
            return Event::None;
        case kMidiCmdInfo:
            if (midi_info_ != nullptr) {
                block_ptr_  = midi_info_;
                block_left_ = kMidiInfoBytes;
            } else {
                zeros_left_ = kMidiInfoBytes;
            }
            refresh_data_status();
            return Event::None;
        default:
            midi_active_ = false;
            return Event::None;
    }
}

// Выбрать подмену по тому, что залили. Ответ не отдаётся сразу: программа
// отвечает на следующую команду плеера.
void GsDevice::arm_blob_answer(uint16_t addr) {
    (void)addr;
    blob_answer_len_ = 0;
    // Проигрыватель MIDI опознан и запущен ещё на коде команды, поток за
    // ним открывается там же; подмены ему нет.
    if (blob_is_midi()) return;
    // Резидент на следующую команду не отвечает, а обслуживает свои
    // команды. Тел три - у ранних версий плеера свои, у поздних общее.
    for (const ResidentBlob& r : kResidentBlobs) {
        if (r.crc == blob_crc_ && r.len == blob_len_) {
            resident_active_        = true;
            resident_await_pattern_ = false;
            return;
        }
    }
    for (const BlobAnswer& a : kBlobAnswers) {
        if (a.crc != blob_crc_ || a.len != blob_len_) continue;
        blob_answer_[0]  = a.low;
        blob_answer_[1]  = a.high;
        blob_answer_len_ = a.bytes;
        return;
    }
}

// Очередной байт хвоста команды.
SOUNDSINTH_HOT_PATH_ATTR("gs_take_tail")
void GsDevice::take_tail(uint8_t value) {
    const bool first_tail_byte = tail_left_ == kBlockTailBytes;
    --tail_left_;
    // Хвост прямого запуска: по руководству сперва нота, потом громкость.
    // Событие отсюда уходит отложенным - у записи в регистр данных своего
    // возврата нет, его забирает платформа через take_event().
    if ((last_cmd_ & kDirectPlayMask) == kDirectPlayCode) {
        if ((last_cmd_ & kWithNoteBit) != 0 && fx_start_.note == kFxKeepDefault) {
            fx_start_.note = value > kNoteMax ? kNoteMax : value;
        } else {
            fx_start_.volume = value > kModuleVolumeMax ? kModuleVolumeMax : value;
        }
        if (tail_left_ == 0) {
            pending_event_ = arm_fx_start(fx_start_.channel, fx_pending_handle_, fx_start_.note, fx_start_.volume);
        }
        return;
    }
    switch (last_cmd_) {
        case kCmdOutPort:
            // Вторая фаза #10: SD Port / SC #10 / WC / SD Data.
            inner_write(static_cast<uint8_t>(tail_word_), value);
            break;
        case kCmdLoadBlock:
        case kCmdGetBlock:
            // Первый байт хвоста - LEN.H, адрес не нужен.
            if (first_tail_byte) tail_word_ = make_word(static_cast<uint8_t>(tail_word_), value);
            if (tail_left_ == 0) {
                if (last_cmd_ == kCmdLoadBlock) {
                    sink_left_ = tail_word_;
                } else {
                    zeros_left_ = tail_word_;
                    refresh_data_status();
                }
            }
            break;
        case kCmdPeek:
            // SD ADR.L / SC #17 / WD / SD ADR.H / GD Byte.
            respond_u8(mem_peek(make_word(static_cast<uint8_t>(tail_word_), value)));
            break;
        case kCmdLoadDe:
            // SD E / SC #18 / WC / SD D.
            de_ = make_word(static_cast<uint8_t>(de_), value);
            break;
        case kCmdFxLoopBegin:
        case kCmdFxLoopEnd: {
            // SD LEN.L / SC код / SD LEN.M / SD LEN.H.
            if (tail_left_ > 0) {
                tail_word_ = make_word(static_cast<uint8_t>(tail_word_), value);
                break;
            }
            FxSample* s = current_slot(0);
            if (s == nullptr) break;
            const uint32_t len = static_cast<uint32_t>(tail_word_) | (static_cast<uint32_t>(value) << 16);
            if (last_cmd_ == kCmdFxLoopBegin) {
                // Старший байт #FF - это не адрес, а признак "петли нет".
                // Границу в таком случае не трогаем: читать её некому.
                s->loop = value != kLoopOffHigh;
                if (s->loop) s->loop_begin = len;
            } else {
                s->loop_end = len;
            }
            break;
        }
        case kCmdJump:
            // Адрес запуска собран целиком - здесь и решается, узнали ли
            // мы залитую программу.
            arm_blob_answer(make_word(static_cast<uint8_t>(tail_word_), value));
            break;
        default:
            // #6B (старший байт длины), #16 (адрес), #1C (байт): значение
            // не нужно.
            break;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_peek_data")
uint8_t GsDevice::peek_data() const {
    if (block_left_ > 0) {
        return *block_ptr_;
    }
    if (zeros_left_ > 0) {
        return 0;
    }
    return resp_pos_ < resp_n_ ? resp_[resp_pos_] : out_reg_;
}

SOUNDSINTH_HOT_PATH_ATTR("gs_read_data")
uint8_t GsDevice::read_data() {
    // Байт - тот же, что показал peek_data; дальше - продвинуть источник.
    // Отдавать нечего - читается регистр вывода: у настоящей карты он
    // физический, содержимое переживает конец ответа.
    const uint8_t byte = peek_data();
    if (block_left_ > 0) {
        out_reg_ = *block_ptr_++; // кусок принятого модуля, без копии
        --block_left_;
    } else if (zeros_left_ > 0) {
        --zeros_left_; // блок #15: вместо памяти карты нули
        out_reg_ = 0;
    } else if (resp_pos_ < resp_n_) {
        out_reg_ = resp_[resp_pos_++];
    }
    refresh_data_status();
    return byte;
}

// --- Внутренние порты ---
//
// Два из них связаны с битами состояния, на этом построен детект: софт
// пишет известное значение в порт страниц, трогает #0A и проверяет, что
// бит данных стал его инверсией. Без #10/#11 такой детект объявляет карту
// отсутствующей.
SOUNDSINTH_HOT_PATH_ATTR("gs_inner_write")
void GsDevice::inner_write(uint8_t port, uint8_t value) {
    port         &= kInnerPortMask;
    inner_[port]  = value;
    switch (port) {
        case kInnerPortOut:
            // Регистр вывода в Spectrum: записанное отдаётся через GSDAT
            // (детект плеера). Бит данных взводится: так
            // карта сообщает, что байт готов (WN).
            respond_u8(value);
            break;
        case kInnerPortClearCommand:
            // Сброс бита команд - "команда выполнена".
            status_ &= static_cast<uint8_t>(~kCommandBit);
            break;
        case kInnerPortDataFromPages:
        case kInnerPortCommandFromVolume:
            apply_status_link(port);
            break;
        default:
            break;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_apply_status_link")
void GsDevice::apply_status_link(uint8_t port) {
    if (port == kInnerPortDataFromPages) {
        // Бит данных = инверсия бита 0 порта страниц.
        if ((inner_[kInnerPortPages] & kPagesDataBit) != 0u) {
            status_ &= static_cast<uint8_t>(~kDataBit);
        } else {
            status_ |= kDataBit;
        }
    } else {
        // Бит команд = бит 5 громкости четвёртого канала.
        if ((inner_[kInnerPortVolume4] & kVolumeCommandBit) != 0u) {
            status_ |= kCommandBit;
        } else {
            status_ &= static_cast<uint8_t>(~kCommandBit);
        }
    }
}

SOUNDSINTH_HOT_PATH_ATTR("gs_inner_read")
uint8_t GsDevice::inner_read(uint8_t port) {
    port &= kInnerPortMask;
    switch (port) {
        case kInnerPortCommand:
            return last_cmd_; // код последней команды от ZX
        case kInnerPortData:
            return last_data_; // последний байт данных от ZX
        case kInnerPortStatus:
            return status_; // слово состояния целиком
        case kInnerPortDataFromPages:
        case kInnerPortCommandFromVolume:
            apply_status_link(port); // эти два двигают биты и при чтении
            return inner_[port];
        default:
            return inner_[port];
    }
}

// Состояние по умолчанию для любой команды: выполнена, данных нет, бит
// команды не взводится. Недособранный хвост и блок прошлой команды
// бросаются.
SOUNDSINTH_HOT_PATH_ATTR("gs_drop_pending_exchange")
void GsDevice::drop_pending_exchange() {
    status_       = kStatusIdle;
    data_pending_ = false;
    resp_n_       = 0;
    resp_pos_     = 0;
    tail_left_    = 0;
    sink_left_    = 0;
    zeros_left_   = 0;
}

void GsDevice::fx_forget_all() {
    for (FxSample& s : fx_) {
        s.length = 0;
    }
    fx_count_   = 0;
    fx_loading_ = kFxNone;
    fx_active_  = 0;
}

// Занятые каналы: запущенные и не освобождённые, кроме доигравших сами.
SOUNDSINTH_HOT_PATH_ATTR("gs_fx_busy")
uint8_t GsDevice::fx_busy() const {
    uint8_t busy = fx_active_;
    if (fx_ended_ == nullptr) return busy;
    for (uint8_t c = 0; c < kFxChannels; ++c) {
        const uint8_t bit = static_cast<uint8_t>(1u << c);
        if ((busy & bit) != 0 && fx_ended_[c].load(std::memory_order_relaxed) == fx_seq_[c]) busy = static_cast<uint8_t>(busy & ~bit);
    }
    return busy;
}

// Куда поставить сэмпл. Порядок из руководства: сперва свободный канал
// среди SeekFirst, потом свободный среди SeekLast, и только если свободных
// нет - самый низкоприоритетный из SeekLast, и то лишь когда наш приоритет
// ВЫШЕ.
//
// Биты масок - прямым порядком, бит 0 это канал 0: замерено запуском по
// одному биту на ПЗУ 1.08. Старшая половина маски не выбирает ничего.
SOUNDSINTH_HOT_PATH_ATTR("gs_pick_fx_channel")
uint8_t GsDevice::pick_fx_channel(const FxSample& s) const {
    const uint8_t busy  = fx_busy();
    const uint8_t first = static_cast<uint8_t>(s.seek_first & kFxChannelMask);
    const uint8_t last  = static_cast<uint8_t>(s.seek_last & kFxChannelMask);
    for (uint8_t c = 0; c < kFxChannels; ++c) {
        const uint8_t bit = static_cast<uint8_t>(1u << c);
        if ((first & bit) != 0 && (busy & bit) == 0) return c;
    }
    for (uint8_t c = 0; c < kFxChannels; ++c) {
        const uint8_t bit = static_cast<uint8_t>(1u << c);
        if ((last & bit) != 0 && (busy & bit) == 0) return c;
    }
    uint8_t victim = kFxNoChannel;
    uint8_t lowest = 0xffu;
    for (uint8_t c = 0; c < kFxChannels; ++c) {
        const uint8_t bit = static_cast<uint8_t>(1u << c);
        if ((last & bit) == 0) continue;
        if (victim == kFxNoChannel || fx_ch_priority_[c] < lowest) {
            victim = c;
            lowest = fx_ch_priority_[c];
        }
    }
    if (victim == kFxNoChannel || s.priority <= lowest) return kFxNoChannel;
    return victim;
}

// Пустить сэмпл в названный канал: #39 канал выбрал сам, #80-#9B его
// назвал. Общее - заполнить заявку и запомнить приоритет, по которому
// канал потом можно будет отнять.
SOUNDSINTH_HOT_PATH_ATTR("gs_arm_fx_start")
Event GsDevice::arm_fx_start(uint8_t channel, uint8_t handle, uint8_t note, uint8_t volume) {
    const FxSample* s = current_slot(handle);
    if (s == nullptr || s->length == 0) return Event::None;
    fx_ch_priority_[channel] = s->priority;
    fx_start_.channel        = channel;
    fx_start_.handle         = handle == 0 ? cur_fx_ : handle;
    fx_start_.note           = note == kFxKeepDefault ? s->note : note;
    fx_start_.volume         = volume == kFxKeepDefault ? s->volume : volume;
    fx_seq_[channel]         = static_cast<uint16_t>(fx_seq_[channel] + 1u);
    fx_start_.seq            = fx_seq_[channel];
    fx_snapshot(fx_start_, *s);
    fx_active_ = static_cast<uint8_t>(fx_active_ | (1u << channel));
    return Event::FxPlay;
}

// Прямой запуск, #80-#9B. Номер сэмпла пришёл аргументом до кода, канал
// назван младшими битами, а нота и громкость - хвостом, и какие из них
// есть, говорят биты 3 и 4. Без хвоста запуск идёт сразу.
SOUNDSINTH_HOT_PATH_ATTR("gs_direct_play")
Event GsDevice::direct_play(uint8_t value) {
    const uint8_t tail = static_cast<uint8_t>(((value & kWithNoteBit) != 0 ? 1u : 0u) + ((value & kWithVolumeBit) != 0 ? 1u : 0u));
    if (tail == 0) {
        return arm_fx_start(static_cast<uint8_t>(value & kDirectChanMask), arg_, kFxKeepDefault, kFxKeepDefault);
    }
    // Номер и канал ждут хвоста, заявку отправит take_tail.
    fx_pending_handle_ = arg_;
    fx_start_.channel  = static_cast<uint8_t>(value & kDirectChanMask);
    fx_start_.note     = kFxKeepDefault;
    fx_start_.volume   = kFxKeepDefault;
    tail_left_         = tail;
    return Event::None;
}

// Память и процессор карты, #13..#1D. Процессора и ОЗУ карты нет: команда
// съедает ровно свои байты, записанное пропадает, чтение отдаёт известные
// переменные ПЗУ, прочее - ноль. Следующая команда получит свои аргументы.
SOUNDSINTH_HOT_PATH_ATTR("gs_card_memory_command")
Event GsDevice::card_memory_command(uint8_t value) {
    Event event = Event::None;
    switch (value) {
        case kCmdJump: // SD ADR.L / SC / WC / SD ADR.H
            // Запуск залитой программы. Если это загрузчик модуля, он
            // заменяет собой команду #30: дальше в порт данных пойдёт
            // модуль, а не аргументы. Старший байт адреса придёт хвостом
            // и в поток не попадёт - хвост разбирается раньше.
            tail_word_ = arg_; // ADR.L; старший байт придёт хвостом
            tail_left_ = 1;
            // Вид потока решается ЗДЕСЬ, а не на хвосте с адресом: поток
            // открывается этой же командой, и платформа узнаёт о нём
            // сразу. Сумма к этому моменту уже посчитана - байты кончились.
            //
            // Проигрыватель MIDI тоже зовёт приёмник потока из ПЗУ, то
            // есть тоже "загрузчик", но за ним идёт файл .mid, а не
            // модуль. Поэтому он проверяется первым.
            last_blob_len_ = blob_len_;
            last_blob_crc_ = blob_crc_;
            if (blob_is_midi()) {
                blob_is_loader_   = false;
                midi_blob_loaded_ = true;
                midi_active_      = true;
                midi_ready_       = false;
                stream_           = Stream::Midi;
                stream_bytes_     = 0;
                event             = Event::StreamBegin;
            } else if (blob_is_loader_) {
                blob_is_loader_ = false;
                stream_         = Stream::Module;
                stream_bytes_   = 0;
                event           = Event::StreamBegin;
            }
            break;
        case kCmdPoke20: // SD ADR.L / SC / WC / SD Byte
            tail_left_ = 1;
            break;
        case kCmdLoadDe: // SD E / SC / WC / SD D
            de_        = arg_;
            tail_left_ = 1;
            // Адрес задан - началась новая заливка, узнавание с нуля.
            blob_window_      = 0;
            blob_is_loader_   = false;
            blob_crc_         = soundsinth::util::kCrc32Init;
            blob_skip_at_     = 0;
            blob_len_         = 0;
            midi_blob_loaded_ = false;
            midi_facts_       = MidiFileFacts{};
            break;
        case kCmdPeek: // SD ADR.L / SC / WD / SD ADR.H / GD Byte
            tail_word_ = arg_;
            tail_left_ = 1;
            break;
        case kCmdPoke: // SD Byte / SC / WC / SD ADR.L / SD ADR.H
            tail_left_ = 2;
            break;
        case kCmdLoadBlock: // SD LEN.L / SC / SD LEN.H, ADR.L, ADR.H / SD x LEN
        case kCmdGetBlock:  // SD LEN.L / SC / SD LEN.H, ADR.L, ADR.H / GD x LEN
            tail_word_ = arg_;
            tail_left_ = kBlockTailBytes;
            break;
        case kCmdPokeDe: { // SD Byte / SC
            // Байт заливаемой программы. Сам он нам не нужен, но по трём
            // подряд видно, зовёт ли программа приёмник потока из ПЗУ
            // карты: `call #0C09` это CD 09 0C.
            blob_window_ = ((blob_window_ << 8) | arg_) & 0xffffffu;
            if (blob_window_ == kBlobLoaderCall) blob_is_loader_ = true;
            // Правимое место входит в сумму нулём. Смещения возрастают, так
            // что хватает указателя на ближайшее.
            uint8_t counted = arg_;
            if (blob_skip_at_ < kMidiPatchCount && blob_len_ == kMidiPatchOffsets[blob_skip_at_]) {
                // Эти байты плеер подставляет сам: разрешение, смещение
                // первой дорожки, их число и длина файла. Ловим здесь -
                // разбирать MThd заново незачем.
                switch (blob_skip_at_) {
                    case 0: // #62D - разрешение, младший
                        midi_facts_.division = static_cast<uint16_t>((midi_facts_.division & 0xff00u) | arg_);
                        break;
                    case 1: // #62E - разрешение, старший
                        midi_facts_.division = static_cast<uint16_t>((midi_facts_.division & 0x00ffu) | (arg_ << 8));
                        break;
                    case 2: // #63C - что это, не выяснено; плеер кладёт #FF
                        break;
                    case 3: // #76B - смещение первой дорожки
                        midi_facts_.first_track = arg_;
                        break;
                    case 4: // #7BC - число дорожек
                        midi_facts_.track_count = arg_;
                        break;
                    case 5: // #853 - длина файла, младший
                        midi_facts_.file_length = static_cast<uint16_t>((midi_facts_.file_length & 0xff00u) | arg_);
                        break;
                    default: // #854 - длина файла, старший
                        midi_facts_.file_length = static_cast<uint16_t>((midi_facts_.file_length & 0x00ffu) | (arg_ << 8));
                        break;
                }
                ++blob_skip_at_;
                counted = 0;
            }
            blob_crc_ = soundsinth::util::crc32_update(blob_crc_, &counted, 1);
            ++blob_len_;
            break;
        }
        case kCmdIncDe: // SC
            ++de_;
            break;
        case kCmdPeekDe: // SC / GD Byte
            respond_u8(mem_peek(de_));
            break;
        case kCmdPeek20: // SD ADR.L / SC / GD Byte
            respond_u8(mem_peek(static_cast<uint16_t>(kPeek20Base | arg_)));
            break;
    }
    return event;
}

SOUNDSINTH_HOT_PATH_ATTR("gs_write_command")
Event GsDevice::write_command(uint8_t value) {
    last_cmd_ = value;
    if (cmd_hist_[value] != kHistMax) ++cmd_hist_[value];
    drop_pending_exchange();

    Event event = Event::None;

    // Пока поток открыт, карта его и разбирает, а не главный цикл: любая
    // команда, кроме конца потока и сброса, молча проглатывается. Так
    // устроен приёмник в ПЗУ, и на это опираются плееры - они шлют посреди
    // потока и #00, и #31, и ждут, что это ничего не изменит.
    if (stream_ != Stream::None && value != kCmdStreamClose && value != kCmdWarmReset && value != kCmdColdReset) {
        return Event::None;
    }

    // Залитая программа узнана и запущена: следующую команду исполняет она,
    // а не прошивка. Отдаём то, что отдала бы она, и на этом она кончается,
    // как и настоящая.
    if (blob_answer_len_ != 0) {
        respond(blob_answer_, blob_answer_len_);
        blob_answer_len_ = 0;
        return Event::None;
    }

    // #D2 блобу не команда: файл принимает приёмник ПЗУ, и блоб
    // возвращается в свой цикл уже после него.
    //
    // Пробу #02 при залитом, но не запущенном блобе ПЗУ 1.08 не обслуживает;
    // ответ не нулём - плеер обходится без MIDI.
    if (midi_active_ && value != kCmdStreamClose) {
        const Event e = midi_command(value);
        if (midi_active_) return e;
        // Чужой код кончает блоб - дальше команду обслуживает прошивка,
        // то есть мы обычным порядком. Так же устроен и сам блоб.
    } else if (midi_blob_loaded_ && value == kMidiCmdReady) {
        respond_u8(kMidiNotReady);
        return Event::None;
    }

    // Залит резидент: пока он жив, команды разбирает он, а не прошивка.
    if (resident_active_) {
        if (resident_await_pattern_) {
            // Поддиалог после блока состояния: #FF - больше ничего не надо,
            // иначе это номер позиции и плеер ждёт килобайт паттерна.
            resident_await_pattern_ = false;
            if (value != kResidentNoPattern) arm_pattern(value);
            return Event::None;
        }
        switch (value) {
            case kResidentState:
            case kResidentStateVolume:
                // Блок, собранный потоком; посреди его записи - прошлый.
                if ((telemetry_seq_ & 1u) == 0u) {
                    for (uint8_t i = 0; i < kTelemetryBytes; ++i) {
                        telemetry_[i] = telemetry_staged_[i];
                    }
                }
                // Бит "не менялось" взводится при выдаче, как это делает
                // прошивка (#C8B3: отдать, затем set 7 в переменной). Без
                // него плеер считает, что нота сменилась на каждом опросе,
                // то есть полсотни раз в секунду.
                for (uint8_t c = 0; c < kModuleChannels; ++c) {
                    const uint8_t n = telemetry_[kTelemetryNotes + c];
                    if (n == served_note_[c]) {
                        telemetry_[kTelemetryNotes + c] = static_cast<uint8_t>(n | kNoteUnchanged);
                    } else {
                        served_note_[c] = n;
                    }
                }
                respond(telemetry_, static_cast<uint8_t>(sizeof(telemetry_)));
                resident_await_pattern_ = true;
                return Event::None;
            case kResidentSetPosition:
                // Ответа нет: настоящий встаёт на позицию и выходит в
                // прошивку, после чего плеер заливает его заново.
                resident_active_ = false;
                return Event::None;
            default:
                // Чужая команда кончает резидента - дальше её обслуживает
                // прошивка, то есть мы обычным порядком.
                resident_active_ = false;
                break;
        }
    }

    // Прямой запуск и смена на лету ловятся до разбора: это 24 кода, и
    // развёрнутыми ветвями они стоили бы килобайт ОЗУ.
    if ((value & kDirectPlayMask) == kDirectPlayCode) {
        event = direct_play(value);
    } else if ((value & kChannelSetMask) == kChannelSetCode) {
        // Смена ноты или громкости канала на лету. Значение - аргументом до
        // кода. По руководству команда действует и на сэмпл, и на модуль; у
        // нас модуль играет трекером, мимо каналов карты, поэтому до него
        // она не доходит.
        //
        // Бит 3 здесь значит ОБРАТНОЕ тому, что у прямого запуска: #A0-#A3
        // меняют ноту, #A8-#AB громкость.
        fx_start_.channel = static_cast<uint8_t>(value & kDirectChanMask);
        if ((value & kWithNoteBit) == 0) {
            fx_start_.note = arg_ > kNoteMax ? kNoteMax : arg_;
            event          = Event::FxSetNote;
        } else {
            fx_start_.volume = arg_ > kModuleVolumeMax ? kModuleVolumeMax : arg_;
            event            = Event::FxSetVolume;
        }
    } else if (value >= kCmdJump && value <= kCmdPeek20) {
        event = card_memory_command(value);
    } else {
        switch (value) {
            case kCmdResetFlags:
            case kCmdResetFlags2:
            case kCmdReinit:
                // Поток не трогают: закрывают его только #D2, #F3 и #F4.
                break;

            case kCmdWarmReset:
            case kCmdColdReset:
                reset();
                event = Event::Reset;
                break;

            case kCmdBusyOn:
                hx_busy_ = true;
                break;
            case kCmdBusyOff:
                hx_busy_ = false;
                break;

            case kCmdGetHx:
                // Регистр HX. По руководству значим только бит 7 - флаг
                // занятости.
                respond_u8(hx_busy_ ? kHxBusyBit : 0u);
                break;

            case kCmdTotalRam:
            case kCmdFreeRam:
                // Свободно столько же, сколько всего: модуль ложится в свой
                // приёмный буфер, объявленную память ничто не расходует.
                respond_u24(kDeclaredRamBytes);
                break;
            case kCmdPages:
                respond_u8(kDeclaredPages);
                break;

            case kCmdDataOn:
                // Установить регистр данных в 0xFF: проверка связи, софт
                // подаёт команду и ждёт 0xFF в GSDAT.
                respond_u8(0xffu);
                break;

            // --- Прямой вывод в ЦАП и громкости, #01..#0B ---
            //
            // Низкоуровневая группа: тишина, громкость канала, байт в ЦАП.
            // Принимаются молча, звука по ним нет (нужен отдельный источник в
            // микшере). Аргументы съедаются, состояние "выполнено". В счётчик
            // неопознанных не идут, сколько их пришло - видно в command_count.
            case kCmdSetSilence:
            case kCmdSetLowVolume:
            case kCmdSetHighVolume:
            case kCmdSetE3Bits:
            case kCmdOutVolumePort:
            case kCmdSendToDac:
            case kCmdSendToDacVol:
            case kCmdSetByteVolume: // ccvvvvvv
            case kCmdDacOutput:
            case kCmdDacAndVolume:
                break;

            case kCmdOutPort:
                // Порт назван аргументом, значение придёт хвостом.
                tail_word_ = static_cast<uint8_t>(arg_ & kInnerPortMask);
                tail_left_ = 1;
                break;
            case kCmdInPort:
                // Порт назван аргументом, ответ читается сразу; ответ перекрывает
                // биты, выставленные чтением #0A/#0B.
                respond_u8(inner_read(arg_));
                break;
            case kCmdOutPort0:
                // Частный случай записи в регистр страниц.
                inner_write(kInnerPortPages, arg_);
                break;

            // Громкость проигрывания модуля, #00..#40. Прежнее значение
            // отдаётся регистром вывода, а не ответом: карта кладёт его в
            // порт #03 ДО того, как заберёт аргумент из #02, и забор снимает
            // бит данных. Значит после команды бит снят, а байт в регистре
            // лежит - софт читает GSDAT без ожидания. Плеер шлёт эту команду
            // на каждое движение регулятора.
            case kCmdModuleVolume:
            case kCmdModuleVolumeAlt:
                out_reg_       = module_volume_;
                module_volume_ = arg_ > kModuleVolumeMax ? kModuleVolumeMax : arg_;
                event          = Event::Volume;
                break;

            case kCmdSetCurFx:
                out_reg_ = cur_fx_;
                cur_fx_  = arg_;
                break;

            // --- Громкости эффектов, #2B и #3D ---
            //
            // Две разные переменные карты (#40A5 и #40A6 в ОЗУ 1.08), обе
            // режутся по #40 и обе отдают прежнее значение регистром вывода.
            // Обе пересчитывают таблицы громкости каналов, различие по
            // прошивке не видно. Держим раздельно, как на карте.
            case kCmdFxMasterVolume:
                out_reg_          = fx_master_volume_;
                fx_master_volume_ = arg_ > kModuleVolumeMax ? kModuleVolumeMax : arg_;
                fx_volume_eff_    = fx_master_volume_;
                break;
            case kCmdFxVolume:
                out_reg_       = fx_volume_;
                fx_volume_     = arg_ > kModuleVolumeMax ? kModuleVolumeMax : arg_;
                fx_volume_eff_ = fx_volume_;
                break;

            // --- Параметры текущего сэмпла, #40..#49 ---
            //
            // Номер в них не передаётся: все работают над CurFX, который
            // ставит #2E или сама заливка. Сэмпла нет - аргумент съедается,
            // и всё.
            //
            // Что режется, замерено на ПЗУ 1.08: нота по 95, громкость по
            // #40, а finetune, приоритет и маски поиска проходят байтом как
            // есть - при том что руководство обещает им #00..#40 и #01..#FE.
            //
            // Прежнее значение в регистр вывода кладут #42, #45, #46 и #47;
            // #40, #41, #48 и #49 регистра не трогают. Особенность прошивки,
            // воспроизводится: софт может на неё опираться.
            case kCmdFxNote:
            case kCmdFxVolumeOne:
            case kCmdFxFinetune:
            case kCmdFxPriority:
            case kCmdFxSeekFirst:
            case kCmdFxSeekLast: {
                FxSample* s = current_slot(0);
                if (s == nullptr) break;
                // Шесть команд отличаются только полем, потолком и тем,
                // отдают ли прежнее значение. Развёрнутыми ветвями это
                // полкилобайта кода В ОЗУ: write_command зовётся из
                // прерывания и живёт на горячем пути.
                uint8_t* field = &s->seek_last;
                uint8_t max    = 0xffu;
                switch (value) {
                    case kCmdFxNote:
                        field = &s->note;
                        max   = kNoteMax;
                        break;
                    case kCmdFxVolumeOne:
                        field = &s->volume;
                        max   = kModuleVolumeMax;
                        break;
                    case kCmdFxFinetune:
                        field = &s->finetune;
                        break;
                    case kCmdFxPriority:
                        field = &s->priority;
                        break;
                    case kCmdFxSeekFirst:
                        field = &s->seek_first;
                        break;
                    default:
                        break; // kCmdFxSeekLast
                }
                if (value != kCmdFxNote && value != kCmdFxVolumeOne) out_reg_ = *field;
                *field = arg_ > max ? max : arg_;
                break;
            }
            // #39 Play FX. Номер сэмпла аргументом, ноль - текущий. Канал
            // выбирается здесь, звук только исполняет: выбор - протокол.
            case kCmdPlayFx: {
                const FxSample* s = current_slot(arg_);
                if (s == nullptr || s->length == 0) break;
                const uint8_t ch = pick_fx_channel(*s);
                if (ch == kFxNoChannel) break; // приоритета не хватило
                event = arm_fx_start(ch, arg_, kFxKeepDefault, kFxKeepDefault);
                break;
            }

            // #3A Stop FX in channels. Руководство признаёт здесь квирк: в
            // маске единица в бите 7 гасит нулевой канал, и так далее -
            // порядок перевёрнут. Замером не подтверждено: состояние каналов
            // прошивка ведёт подкачкой сэмплов, которой в модели ПЗУ нет.
            // Автор советует гасить всё маской #FF, и она работает в любом
            // прочтении.
            case kCmdStopFx: {
                uint8_t mask = 0;
                for (uint8_t c = 0; c < kFxChannels; ++c) {
                    if ((arg_ & (0x80u >> c)) != 0) mask = static_cast<uint8_t>(mask | (1u << c));
                }
                fx_stop_mask_ = mask;
                fx_active_    = static_cast<uint8_t>(fx_active_ & ~mask);
                event         = Event::FxStop;
                break;
            }

            // Границы петли приходят тремя байтами: LEN.L аргументом до кода,
            // LEN.M и LEN.H хвостом после. Хвост разбирает take_tail.
            case kCmdFxLoopBegin:
            case kCmdFxLoopEnd:
                tail_word_ = arg_;
                tail_left_ = kLoopTailBytes;
                break;

            case kCmdStreamOpen:
                // Ничего не делает: поток открыт уже командой #30. #D1 без #30 -
                // не ошибка, софт иногда шлёт его вхолостую.
                break;

            // #38 и #3E: заливка сэмпла эффекта. Карта сама выделяет место и
            // отдаёт номер, адресов игра не называет. Байты идут тем же
            // потоком, что и модуль: следом #D1 (он ничего не делает, поток
            // уже открыт) и #D2 в конце.
            //
            // Умолчания из руководства ставятся здесь же - команды #40-#49
            // меняют их потом, и до них сэмпл уже должен быть играбелен.
            case kCmdLoadFx:
            case kCmdLoadFxExt: {
                if (fx_count_ >= kFxSamplesMax) {
                    // Места нет: отдаём номер вне таблицы, поток не открываем.
                    // Настоящая карта отказала бы нехваткой памяти. CurFX
                    // гасится: иначе команды параметров отвергнутого и #39 с
                    // нулём ("текущий") портили бы и играли последний принятый.
                    respond_u8(kFxNone);
                    cur_fx_ = 0;
                    ++fx_refused_;
                    break;
                }
                // Номер с единицы: прошивка 1.08 на первый #38 отвечает
                // #01. Ноль занят подменой на CurFX, поэтому номером быть
                // не может.
                const uint8_t handle = ++fx_count_;
                FxSample& s          = fx_[handle - 1];
                s                    = FxSample{};
                s.note               = kFxDefaultNote;
                s.volume             = kFxDefaultVolume;
                s.seek_first         = kFxDefaultSeek;
                s.seek_last          = kFxDefaultSeek;
                s.priority           = kFxDefaultPriority;
                // #3E со знаком только при аргументе #01: без него он тот же #38.
                s.signed_pcm  = value == kCmdLoadFxExt && arg_ == 0x01u;
                cur_fx_       = handle; // руководство: CurFX становится номером
                fx_loading_   = handle;
                stream_       = s.signed_pcm ? Stream::FxSigned : Stream::FxUnsigned;
                stream_bytes_ = 0;
                respond_u8(handle);
                event = Event::StreamBegin;
                break;
            }

            case kCmdLoadModule:
                // Порядок по руководству: SC #30 / WC / GD handle, потом #D1.
                // Поток открывается уже здесь, #D1 ничего не делает: запись в
                // GSDAT между #30 и #D1 идёт в поток. Номер модуля всегда 1, у GS
                // их не бывает больше одного.
                stream_       = Stream::Module;
                stream_bytes_ = 0;
                respond_u8(1);
                event = Event::StreamBegin;
                break;

            // --- Обратная связь плееру ---
            //
            // Позиция в order-листе, строка паттерна и их смесь. Софт делает
            // AND #3F для строки и пару RLCA для позиции.
            case kCmdSongPosition:
                respond_u8(order_);
                break;
            case kCmdPatternPosition:
                respond_u8(row_);
                break;
            case kCmdMixPosition:
                respond_u8(static_cast<uint8_t>(((order_ & kMixOrderBits) << kMixOrderLsb) | (row_ & kMixRowMask)));
                break;

            // --- Настройки плеера, #6A и #6B ---
            //
            // Из дополнения руководства: #6A - не останавливаться по F00 в модуле,
            // #6B - релупер (обход зависания GS на очень коротких петлях). Трекеру
            // не нужны, разбор съедает аргументы.
            case kCmdPlayerMode:
                break;
            case kCmdRelooper:
                // Младший байт длины приходит до кода, старший может прийти
                // хвостом. Не пришёл - хвост бросает следующая команда. Её
                // аргумент, записанный раньше неё, будет принят за старший байт.
                // Короткая форма без аргументов - хвоста нет.
                tail_left_ = has_arg_ ? 1u : 0u;
                break;

            case kCmdPlayModule:
                event = Event::Play;
                break;
            case kCmdStopModule:
                event = Event::Stop;
                break;
            case kCmdContModule:
                event = Event::Resume;
                break;

            case kCmdStreamClose:
                if (stream_ != Stream::None) {
                    // Модуль - долгая команда: снос прежнего движка десятки
                    // миллисекунд, разбор до мегабайта единицы; плеер обязан
                    // дождаться. Отпустит платформа, через hold_parse_done().
                    //
                    // Сэмплу эффекта разбирать нечего, и удержание ему не
                    // ставится вовсе: забытое снятие оставило бы плеера ждать
                    // конца навсегда, а ждать тут нечего.
                    if (stream_ == Stream::Module || stream_ == Stream::Midi) hold_parse_begin();
                    fx_loading_ = kFxNone;
                    stream_     = Stream::None;
                    event       = Event::StreamEnd;
                }
                break;

            default:
                // Неопознанная команда запоминается: по ним видно, чего плеерам
                // под GS ещё не хватает.
                unknown_cmd_ = value;
                ++unknown_count_;
                break;
        }
    }

    // Аргументы съедены командой: следующая получила бы чужие.
    if (value != kCmdStreamOpen) {
        arg_     = 0;
        has_arg_ = false;
    }
    return event;
}

} // namespace devices::gs
