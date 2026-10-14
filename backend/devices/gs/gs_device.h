// SPDX-License-Identifier: MIT
#pragma once

// Эмуляция General Sound на уровне регистров и системы команд.
//
// Здесь только протокол, без звука. Музыку делает трекер: модуль уходит
// платформе. Эффекты (#38/#3E) не поддержаны.
//
// --- Четыре регистра, два адреса ---
//
//   0xBB  запись  GSCOM   регистр команд
//   0xBB  чтение  GSSTAT  D0 command bit, D7 data bit, остальное не определено
//   0xB3  запись  GSDAT   данные ZX -> GS
//   0xB3  чтение  GSDAT   данные GS -> ZX (другой регистр по тому же адресу)
//
// Регистр данных и регистр вывода - два разных байта по одному адресу,
// записанное в один из другого не читается.
//
// --- Бит команды ---
//
// Простые команды его не взводят: ответ готов сразу, софт ждёт нуля в D0 (без
// карты чтение даёт 0xFF, бит стоит всегда). Долгие команды держат бит, пока
// платформа не отпустит. Срок у обработчика есть: ответ ложится в таблицу до
// GD - около 33 тактов Z80 после OUT (2.4 мкс на 14 МГц), удержание - до
// первого чтения состояния, около 11 тактов. Поэтому методы - в SRAM.
//
// --- Ответ длиной больше байта ---
//
// Команды вроде "сколько всего памяти" отдают три байта подряд: каждое
// чтение GSDAT выдаёт очередной и продвигает указатель. Пока байты есть,
// в состоянии стоит бит данных (0xFE), и софт крутит ожидание на нём.

#include <atomic>
#include <cstdint>

namespace devices::gs {

// Биты состояния (GSSTAT).
inline constexpr uint8_t kCommandBit = 0x01u;
inline constexpr uint8_t kDataBit    = 0x80u;

// Состояния "команда выполнена, данных нет" и "готов байт ответа".
// Единицы в неопределённых битах (0x7E/0xFE): плееры на
// них не рассчитывают, а байт заметнее в логе и не путается с "шина
// молчит" (0x00).
inline constexpr uint8_t kStatusIdle = 0x7eu;
inline constexpr uint8_t kStatusData = 0xfeu;

// Слотов под эффекты: у оригинала 32, у Unreal 64. Пока не используется:
// эффекты не поддержаны.
inline constexpr uint8_t kMaxFxSlots = 64;

// Потолок громкости проигрывания модуля по руководству: #00..#40.
inline constexpr uint8_t kModuleVolumeMax = 0x40u;

// Громкость канала в трекерной модели и средняя точка, вокруг которой
// карта отдаёт уровень канала в телеметрии.
inline constexpr uint8_t kChannelVolumeMax = 64;
inline constexpr uint8_t kSilenceLevel     = 0x80u;
// Нота канала по руководству GS (#63 Get Channel Notes): младшие семь бит -
// нота 0..95, 127 - в канале ничего не играет, бит 7 - "значение не
// менялось с прошлой выдачи". Прошивка 1.08 взводит его в самой переменной
// (#C8B3: out (#03),a / set 7,(hl)), поэтому через резидент идёт то же.
inline constexpr uint8_t kNoteMax       = 95;
inline constexpr uint8_t kNoteSilent    = 127;
inline constexpr uint8_t kNoteUnchanged = 0x80u;
// Шкала движка без сдвига: со сдвигом к индексу таблицы ProTracker (минус
// 36) Z-Player 4.1 и 5.1 гасят подписи нот. Нумерация нот у карты замером
// не установлена.
//
// "Ноты нет" на входе: движок отдаёт 0xFF (kNoteNone модели).
inline constexpr uint8_t kNoteNoneIn = 0xffu;

// Своя система команд резидента, который плеер заливает в карту: встать на
// позицию, отдать блок состояния, он же с громкостью. После блока резидент
// ждёт ещё команду: #FF - больше ничего, иначе номер позиции для паттерна.
inline constexpr uint8_t kResidentSetPosition = 0xedu;
inline constexpr uint8_t kResidentState       = 0xeeu;
inline constexpr uint8_t kResidentStateVolume = 0xefu;
inline constexpr uint8_t kResidentNoPattern   = 0xffu;
// Транспорт модуля по руководству GS: играть, стоп, продолжить.
inline constexpr uint8_t kCmdPlayModule = 0x31u;
inline constexpr uint8_t kCmdStopModule = 0x32u;
inline constexpr uint8_t kCmdContModule = 0x33u;
// Паттерн ProTracker: 64 строки по 4 канала, четыре байта на ячейку.
inline constexpr uint16_t kPatternBytes = 1024;
// Раскладка MOD: таблица порядка на 128 записей и начало паттернов.
inline constexpr uint16_t kModOrderTable = 952;
inline constexpr uint16_t kModOrderCount = 128;
inline constexpr uint16_t kModPatternsAt = 1084;

// `call #0C09` в теле заливаемой программы. #0C09 - приёмник потока в ПЗУ
// карты, поэтому такая программа заменяет собой команду #30: после запуска
// в порт данных пойдёт модуль. Из восьми известных программ Z-Player этот
// вызов есть ровно у двух - у загрузчика модуля и у проигрывателя MIDI.
inline constexpr uint32_t kBlobLoaderCall = 0xcd090cu;

// Что эмуляция сообщает о себе по #20/#21/#23. Объявляется 2 МБ, как у
// NeoGS: 63 страницы по 32 КБ плюс 16 КБ фиксированной области. Софт опознаёт
// по объёму модель карты и с другими признаками не сверяет; модуль больше
// приёмного буфера (1 МБ) отвергается целиком.
// Версия прошивки, которую мы о себе объявляем. Софт читает её из памяти
// карты по #0004 и #0005 и печатает как L.H: здесь выходит 1.08. Числа
// взяты из настоящего ПЗУ General Sound.
inline constexpr uint8_t kRomVersionHigh = 0x08u; // #0004
inline constexpr uint8_t kRomVersionLow  = 0x01u; // #0005

inline constexpr uint8_t kDeclaredPages     = 63;
inline constexpr uint32_t kDeclaredRamBytes = 32768u * (static_cast<uint32_t>(kDeclaredPages) + 1u) - 16384u;

// Режим приёма: что льётся в регистр данных от команды, задавшей вид, до
// #D2.
// Руководство, #38 и #3E: беззнаковые сэмплы (PC) грузит #38, знаковые
// (Amiga) - #3E с аргументом #01.
enum class Stream : uint8_t {
    None = 0,
    Module,     // #30 - файл модуля целиком
    FxUnsigned, // #38 - сэмпл эффекта, беззнаковый
    FxSigned,   // #3E - он же знаковый
    Covox,      // #0E - байты идут прямо в ЦАП, потока нет
    Midi,       // файл .mid: его принимает залитый плеером проигрыватель MIDI
};

// --- Проигрыватель MIDI, залитый плеером ---
//
// Карта только разбирает файл и расписывает его по кадрам, байты Спектрум
// выдаёт битбангом в порт A AY, как линией MIDI OUT. Их играет живой MIDI
// с порта AY.
//
// Команды блоба перекрывают обычные #00-#04, поэтому разбираются, только
// пока он жив; любой другой код его кончает - так же устроен и сам блоб
// (всё, кроме #00-#04, уводит его в ПЗУ карты).
inline constexpr uint16_t kMidiNamesBytes = 1536; // 64 дорожки по 24 знака
inline constexpr uint16_t kMidiInfoBytes  = 84;   // 4 байта к надписям, 80 таблица позиций
inline constexpr uint16_t kMidiStateBytes = 135;  // блок состояния, #01
// Сколько строк списка дорожек плеер показывает разом: по ним он красит
// тринадцать строк атрибутов, а соседние байты - стрелки прокрутки.
inline constexpr uint8_t kMidiStateRows = 13;

// Поля, которые плеер правит в теле блоба перед заливкой: всё, что блобу
// нужно о файле. Ловить их дешевле, чем разбирать MThd.
struct MidiFileFacts {
    uint16_t division    = 0; // PPQN из MThd
    uint8_t first_track  = 0; // смещение первой MTrk от начала файла
    uint8_t track_count  = 0;
    uint16_t file_length = 0;
};

// Сэмпл эффекта. Параметры и умолчания - из руководства (#38): после
// загрузки нота 60, громкость #40, finetune 0, SeekFirst и SeekLast #0F,
// приоритет #80, петли нет. Меняются командами #40-#49.
//
// Все поля нулями: ненулевой инициализатор переносит весь объект автомата
// из .bss в .data. Умолчания ставит приём команды.
struct FxSample {
    uint32_t offset     = 0; // где лежат байты в хранилище платформы
    uint32_t length     = 0;
    uint32_t loop_begin = 0;
    // Конец звука: после заливки равен длине сэмпла, #49 его двигает, а
    // петля только зацикливает до него - так заполняет заголовок карта
    // (замерено на ПЗУ 1.08). Признак петли отдельный от границ: выключить
    // петлю карта умеет, не трогая их.
    uint32_t loop_end  = 0;
    bool loop          = false;
    uint8_t note       = 0;
    uint8_t volume     = 0;
    uint8_t finetune   = 0;
    uint8_t seek_first = 0;
    uint8_t seek_last  = 0;
    uint8_t priority   = 0;
    bool signed_pcm    = false;
};

// Сколько сэмплов держим. У исходной карты их помещалось около полусотни
// (112 КБ памяти при заголовке в 80 байт на сэмпл); игры заливают единицы.
inline constexpr uint8_t kFxSamplesMax = 32;
// Умолчания из руководства.
inline constexpr uint8_t kFxDefaultNote     = 60;
inline constexpr uint8_t kFxDefaultVolume   = 0x40u;
inline constexpr uint8_t kFxDefaultSeek     = 0x0fu;
inline constexpr uint8_t kFxDefaultPriority = 0x80u;
// Громкости эффектов (#2B и #3D) после включения карты - полные.
inline constexpr uint8_t kFxVolumeDefault = 0x40u;
// Каналов эффектов четыре, как у карты. Панорама жёсткая: 0 и 1 налево,
// 2 и 3 направо - это часть характера карты.
inline constexpr uint8_t kFxChannels    = 4;
inline constexpr uint8_t kFxChannelMask = 0x0fu;
// Каналов модуля четыре: у них свои ноты и уровни в блоке состояния.
inline constexpr uint8_t kModuleChannels = 4;
// Блок состояния резидента, смещения в порядке выдачи: позиция, строка,
// ноты каналов модуля, уровни каналов, скорость со старшим битом "звучит".
inline constexpr uint8_t kTelemetryOrder  = 0;
inline constexpr uint8_t kTelemetryRow    = 1;
inline constexpr uint8_t kTelemetryNotes  = 2;
inline constexpr uint8_t kTelemetryLevels = kTelemetryNotes + kModuleChannels;
inline constexpr uint8_t kTelemetrySpeed  = kTelemetryLevels + kModuleChannels;
inline constexpr uint8_t kTelemetryBytes  = kTelemetrySpeed + 1u;
static_assert(kTelemetryLevels == 6 && kTelemetrySpeed == 10 && kTelemetryBytes == 11, "the status block layout is fixed by Z-Player");
// "Взять из заголовка сэмпла": ноты выше 95 и громкости выше #40 не бывает,
// так что #FF ни с чем не спутается.
inline constexpr uint8_t kFxKeepDefault = 0xffu;

// Что автомат просит у звука по Event::FxPlay. Канал выбирает автомат:
// выбор канала - часть протокола карты.
struct FxStart {
    uint8_t channel = 0;
    uint8_t handle  = 0;
    uint8_t note    = 0;
    uint8_t volume  = 0;
    // Номер запуска в канале. Звук возвращает его, когда сэмпл доиграл
    // сам: конец прежнего звука не освобождает канал, отданный новому.
    uint16_t seq = 0;
    // Заголовок сэмпла на момент запуска, как у карты: звук играет его, а
    // не таблицу, которую шина за время до следующего буфера меняет дальше
    // (#48/#49 для следующего выстрела, #F3 и новый #38).
    bool loop           = false;
    bool signed_pcm     = false;
    uint8_t finetune    = 0;
    uint32_t offset     = 0;
    uint32_t length     = 0;
    uint32_t end        = 0; // граница звука: конец петли, не дальше длины
    uint32_t loop_begin = 0;
};

// Снимок заголовка в заявку запуска. Петля - только если её начало до
// границы звука.
inline void fx_snapshot(FxStart& st, const FxSample& s) {
    st.offset     = s.offset;
    st.length     = s.length;
    st.end        = s.loop_end < s.length ? s.loop_end : s.length;
    st.loop_begin = s.loop_begin;
    st.loop       = s.loop && s.loop_begin < st.end;
    st.signed_pcm = s.signed_pcm;
    st.finetune   = s.finetune;
}

// Что команда просит от платформы. Трекера и PSRAM автомат не знает:
// платформа разбирает событие, когда write_command уже отработала, и
// переводит его в вызовы загрузчика и движка.
enum class Event : uint8_t {
    None = 0,
    StreamBegin, // #30: начат приём модуля, дальше байты потока
    StreamEnd,   // #D2: поток закрыт, накопленное можно разбирать
    Reset,       // #F3/#F4: карта сброшена, принятое не нужно
    Play,        // #31
    Stop,        // #32
    Resume,      // #33
    Volume,      // #2A: сменилась громкость проигрывания модуля
    MidiFrame,   // #00: порция кадра забрана, готовить следующую
    FxPlay,      // #39 и #80-#9B: пустить сэмпл в канал, fx_start()
    FxStop,      // #3A: погасить каналы по маске, fx_stop_mask()
    FxSetNote,   // #A0-#A3: сменить ноту канала на лету, fx_start()
    FxSetVolume, // #A8-#AB: сменить громкость канала на лету, fx_start()
};

class GsDevice {
public:
    // Внутренних портов карты; номер порта - младшие биты аргумента.
    static constexpr uint8_t kInnerPortCount = 16;

    // Сброс в состояние "карта только что включена".
    void reset();

    // --- Шинные обращения, все четыре из обработчика прерывания ---

    // OUT (0xBB).
    Event write_command(uint8_t value);
    // OUT (0xB3). true - байт потока: платформа кладёт его в свой приёмный
    // буфер. Вызывается из обработчика прерывания, поэтому байт не
    // копируется здесь никуда.
    bool write_data(uint8_t value);
    // IN (0xBB). Удержание платформы добавляется к биту команды поверх
    // status_: ответ команды и чтение байта ответа его не стирают.
    uint8_t read_status() const { return (hold_reserve_ || hold_parse_) ? static_cast<uint8_t>(status_ | kCommandBit) : status_; }
    // IN (0xB3), продвигает ответ.
    uint8_t read_data();

    // Какой байт отдаст следующее чтение регистра данных, без побочных
    // действий.
    //
    // Байт уходит на шину переносом DMA задолго до того, как о чтении узнает
    // обработчик: платформа обязана положить ответ в таблицу заранее. Без
    // этого ни один ответ карты до Z80 не доходит (ни дескриптор модуля от
    // #30, ни объём памяти от #20).
    uint8_t peek_data() const;

    // Где сейчас играет: позиция в order-листе и строка паттерна. #60/#61/#62
    // отдают их плееру, по ним софт синхронизирует картинку с музыкой
    // (плеер под GS спрашивает каждое прерывание). Платформа кладёт
    // значения перед write_command: автомат о трекере не знает.
    void set_position(uint8_t order, uint8_t row) {
        order_ = order;
        row_   = row;
    }

    // Телеметрия для залитой плеером программы: позиция, строка, нота и
    // уровень по четырём каналам, тиков на строку. Платформа отдаёт факты,
    // блок из них собирает автомат - раскладка его, а не её.
    //
    // notes: номер полутона; годным плеер считает #24..#47, прочее - "нет
    // ноты". levels: громкость 0..64, автомат переводит её в отклонение от
    // средней точки #80, как делает карта.
    //
    // Зовётся из потока, не с пути шины: блок ложится в запасной буфер, а
    // обработчик берёт его при выдаче блока состояния.
    void set_telemetry(uint8_t order, uint8_t row, const uint8_t notes[kModuleChannels], const uint8_t levels[kModuleChannels], uint8_t speed, bool playing);

    // Принятый модуль: из него залитая плеером программа отдаёт паттерн по
    // номеру позиции. Указатель живёт, пока жив приёмный буфер; платформа
    // снимает его сбросом или новым потоком, передав nullptr.
    void set_module_image(const uint8_t* image, uint32_t length) {
        module_image_ = image;
        module_len_   = length;
    }

    // --- Проигрыватель MIDI ---
    //
    // Таблицы готовит платформа при разборе файла: список имён дорожек и
    // справка о файле. Автомат их только отдаёт - держать полторы тысячи
    // байт в SRAM незачем, они живут в области GS.
    void set_midi_tables(const uint8_t* names, const uint8_t* info) {
        midi_names_ = names;
        midi_info_  = info;
    }
    // Порция байт очередного кадра, последним байтом #FF. Платформа готовит
    // её заранее: плеер спрашивает раз в кадр и читает сразу, считать в
    // прерывании нечего.
    //
    // Писатель (основной поток) и читатель (обработчик порта) на одном ядре:
    // длина - признак "порция готова", и пишется после указателя с
    // барьером компилятора, иначе обработчик увидел бы новую длину при
    // прежнем указателе и отдал прошлую порцию второй раз.
    void set_midi_frame(const uint8_t* bytes, uint16_t len) {
        midi_frame_ = bytes;
        std::atomic_signal_fence(std::memory_order_release);
        midi_frame_len_ = len;
    }
    // Готовая порция ещё не забрана: следующую собирать некуда - слотов
    // два, и второй отдаётся, пока собирается первый.
    bool midi_frame_pending() const { return midi_frame_len_ != 0; }
    // Блок состояния на 135 байт (#01) живёт здесь, в SRAM, а не в области
    // GS: первые пятнадцать байт собираются по аргументу команды, то есть
    // в прерывании, а писать в PSRAM оттуда нельзя. Остальное наполняет
    // платформа прямо в этот буфер.
    uint8_t* midi_state_buffer() { return midi_state_; }
    // Признаки "дорожка только что играла" - по одному на дорожку. Из них
    // автомат собирает строки видимого окна списка.
    void set_midi_tracks(const uint8_t* flags, uint8_t count) {
        midi_tracks_      = flags;
        midi_track_count_ = count;
    }
    // Файл разобран - можно отвечать на пробу #02 нулём.
    void set_midi_ready(bool ready) { midi_ready_ = ready; }
    bool midi_active() const { return midi_active_; }
    const MidiFileFacts& midi_facts() const { return midi_facts_; }
    // Кадр забран, платформе готовить следующий. Читает и гасит основной
    // поток, одним обменом: иначе просьба между чтением и гашением теряется.
    bool take_midi_frame_request() { return midi_frame_wanted_.exchange(false, std::memory_order_acquire); }
    bool midi_frame_wanted() const { return midi_frame_wanted_.load(std::memory_order_relaxed); }
    uint32_t midi_frames_served() const { return midi_frames_served_; }
    // Сколько раз плеер получил пустую порцию: следующая не успела.
    uint32_t midi_frames_empty() const { return midi_frames_empty_; }
    // Что запускали последней командой #13: по длине и сумме видно, какой
    // блоб прошёл мимо опознания.
    uint16_t last_blob_len() const { return last_blob_len_; }
    uint32_t last_blob_crc() const { return last_blob_crc_; }

    // Сколько места у платформы под сэмплы эффектов. Байты складывает она,
    // автомат держит только таблицу: заливка идёт тем же потоком, что и
    // модуль, и писать в PSRAM с горячего пути нельзя.
    void set_fx_capacity(uint32_t bytes) { fx_capacity_ = bytes; }
    // Байты сэмплов ушли (область вернули треку): таблица забывается так
    // же, как по сбросу карты. Иначе #39 со старым номером запустил бы
    // голос по прежнему смещению - в то, что теперь лежит в области.
    void fx_forget_all();
    // Сэмпл, который сейчас принимается: платформе некуда класть байты, пока
    // она не знает, чей он. kFxNone - потока сэмпла нет.
    //
    // Номера идут с единицы: прошивка 1.08 на первый #38 отвечает #01, и
    // ноль у неё занят - им подменяют номер, чтобы команда взяла CurFX.
    // Поэтому ноль и значит "никакого".
    static constexpr uint8_t kFxNone = 0;
    uint8_t fx_loading() const { return fx_loading_; }
    // Сколько #38 отвергнуто: таблица полна.
    uint32_t fx_refused() const { return fx_refused_; }
    // Длина принятого сэмпла ставится платформой: автомат байтов не считает,
    // их могло потеряться в кольце. Конец петли по умолчанию - длина сэмпла,
    // как у карты: она ставит его при закрытии потока, и команда #49 потом
    // перекрывает.
    void set_fx_length(uint8_t handle, uint32_t offset, uint32_t length) {
        FxSample* s = fx_slot(handle);
        if (s == nullptr) return;
        s->offset   = offset;
        s->length   = length;
        s->loop_end = length;
    }

    // --- Наблюдение (диагностика и тесты) ---

    // Занятость каналов эффектов ведёт сам автомат, по своим командам:
    // запуск занимает канал сразу, #3A и сброс освобождают. Как у карты:
    // выбор канала и запуск у неё в одном цикле прошивки, и второй #39 в
    // том же кадре видит первый. Конец сэмпла без петли знает только звук:
    // по каналу он кладёт номер доигравшего запуска (FxStart::seq), и
    // канал свободен, когда этот номер равен последнему выданному.
    void set_fx_end_source(const std::atomic<uint16_t>* ended) { fx_ended_ = ended; }
    // Маска занятых без учёта доигравших - для тестов: занять каналы, не
    // запуская их.
    void set_fx_active(uint8_t mask) { fx_active_ = static_cast<uint8_t>(mask & kFxChannelMask); }
    uint8_t fx_active() const { return fx_busy(); }
    // Разбор события: что запускать и что глушить.
    const FxStart& fx_start() const { return fx_start_; }
    uint8_t fx_stop_mask() const { return fx_stop_mask_; }
    // Событие, созревшее на записи в регистр данных: хвост прямого запуска
    // доходит туда, а у write_data возврата под событие нет. Платформа
    // забирает его сразу после записи.
    Event take_event() {
        const Event e  = pending_event_;
        pending_event_ = Event::None;
        return e;
    }

    uint8_t fx_count() const { return fx_count_; }
    uint8_t current_fx() const { return cur_fx_; }
    // По номеру, как его видит софт: с единицы. Номера вне таблицы отдают
    // нулевую запись - читателю диагностики нужен байт, а не отказ.
    // Номер за счётчиком (в том числе после сброса, таблицу он не стирает) -
    // пустая запись.
    const FxSample& fx_sample(uint8_t handle) const {
        static const FxSample kNone{};
        return (handle >= 1 && handle <= fx_count_) ? fx_[handle - 1] : kNone;
    }
    Stream stream() const { return stream_; }
    uint8_t last_command() const { return last_cmd_; }
    // Последняя незнакомая команда, ноль - таких не было. Мера готовности:
    // прогнать плееры и посмотреть, что осталось.
    uint8_t unknown_command() const { return unknown_cmd_; }
    uint32_t unknown_count() const { return unknown_count_; }
    uint32_t stream_bytes() const { return stream_bytes_; }
    // Громкость проигрывания модуля, #00..#40; читает платформа по Event::Volume.
    uint8_t module_volume() const { return module_volume_; }
    // Громкости эффектов, #00..#40: #2B и #3D соответственно. Хранятся
    // раздельно, потому что раздельно их хранит и карта, но на звук идёт
    // одна - последняя записанная (fx_volume_effective).
    uint8_t fx_master_volume() const { return fx_master_volume_; }
    uint8_t fx_volume() const { return fx_volume_; }
    uint8_t fx_volume_effective() const { return fx_volume_eff_; }

    // Гистограмма команд. Порты GS попадают в диапазон ATA у divIDE
    // (0xA3..0xBF), на них стучит и ПЗУ esxDOS: по кодам видно, кто говорит.
    // Насыщается на 255.
    uint8_t command_count(uint8_t cmd) const { return cmd_hist_[cmd]; }

    // Поддиалог паттерна: сколько раз его просили и сколько раз было чем
    // ответить. Разница - отданные нули, то есть пустой паттерн на экране
    // плеера. Позиция и выбранный по ней номер паттерна - последние.
    uint32_t pattern_asks() const { return pattern_asks_; }
    uint32_t pattern_served() const { return pattern_served_; }
    uint8_t last_pattern_pos() const { return last_pattern_pos_; }
    uint8_t last_pattern_num() const { return last_pattern_num_; }
    uint32_t module_len() const { return module_len_; }
    // Байт блока состояния как он уйдёт в карту: журнал должен показывать
    // отданное, а не то, из чего оно получено.
    uint8_t telemetry_byte(uint8_t i) const { return i < sizeof(telemetry_) ? telemetry_[i] : 0u; }
    bool resident_active() const { return resident_active_; }

    // Долгая работа держит бит команды, иначе плеер пойдёт дальше с
    // недогруженным треком. Флаг свой у резерва под приём (#30) и у разбора
    // (#D2): общий снимал бы чужое удержание. Отдельно от status_, который
    // переписывает прерывание; сброс карты их не снимает.
    void hold_reserve_begin() { hold_reserve_ = true; }
    void hold_reserve_done() { hold_reserve_ = false; }
    void hold_parse_begin() { hold_parse_ = true; }
    void hold_parse_done() { hold_parse_ = false; }

private:
    // Поставить ответ длиной n байт, младший первым (как у #20/#21).
    void respond(const uint8_t* bytes, uint8_t n);
    void respond_u24(uint32_t v);
    void respond_u8(uint8_t v);
    // Бит данных стоит, пока есть что отдать: нули блока #15 или ответ.
    void refresh_data_status();
    // Связь бита состояния с внутренним портом #0A/#0B (модель железа карты).
    void apply_status_link(uint8_t port);
    // Бросить ответ, хвост и блок прошлой команды.
    void drop_pending_exchange();
    // Команды памяти карты #13..#1D.
    Event card_memory_command(uint8_t value);
    void arm_blob_answer(uint16_t addr);
    void arm_pattern(uint8_t position);
    void take_tail(uint8_t value);
    void inner_write(uint8_t port, uint8_t value);
    uint8_t inner_read(uint8_t port);
    // Запись сэмпла по номеру (с единицы), nullptr - номера такого нет.
    FxSample* fx_slot(uint8_t handle) { return (handle >= 1 && handle <= fx_count_) ? &fx_[handle - 1] : nullptr; }
    const FxSample* fx_slot(uint8_t handle) const { return (handle >= 1 && handle <= fx_count_) ? &fx_[handle - 1] : nullptr; }
    // Ноль вместо номера значит "текущий": так договаривается руководство,
    // и этим пользуются команды параметров и запуска.
    FxSample* current_slot(uint8_t handle) { return fx_slot(handle == 0 ? cur_fx_ : handle); }
    // Куда поставить сэмпл с таким приоритетом. kFxNoChannel - некуда.
    static constexpr uint8_t kFxNoChannel = 0xffu;
    uint8_t pick_fx_channel(const FxSample& s) const;
    Event arm_fx_start(uint8_t channel, uint8_t handle, uint8_t note, uint8_t volume);
    uint8_t fx_busy() const;
    Event direct_play(uint8_t value);
    Event midi_command(uint8_t value);
    bool blob_is_midi() const;

    // Ноль, а не kStatusIdle: ненулевой инициализатор кладёт весь объект в
    // .data. Рабочее значение ставит reset() до первого обращения с шины.
    uint8_t status_ = 0;
    // Удержание платформой: резерв памяти под приём и разбор модуля.
    volatile bool hold_reserve_ = false;
    volatile bool hold_parse_   = false;
    uint8_t last_cmd_           = 0;
    uint8_t last_data_          = 0; // последний байт от ZX, его отдаёт порт #02
    uint8_t unknown_cmd_        = 0;
    uint32_t unknown_count_     = 0;
    // Байт на команду: нужен порядок величин, не точный счёт; слово
    // прибавило бы 256 байт самому крупному объекту устройств.
    uint8_t cmd_hist_[256] = {};

    // Аргумент команды - первый байт GSDAT до её подачи; 0, если его не было.
    uint8_t arg_  = 0;
    bool has_arg_ = false;
    // В регистр данных записали, и никто ещё не забрал: бит данных взведён.
    // Главный цикл прошивки регистр не читает, поэтому снимает его только
    // команда. На этом Z-Player 5.1 опознаёт карту.
    bool data_pending_ = false;

    // --- Хвост команды ---
    //
    // Часть команд берёт байты и после кода (SC ... WC ... SD): #10 значение
    // для порта, #6B старший байт длины, #13-#1C адреса и данные. Пока хвост
    // не собран, запись в регистр данных - его байт, а не аргумент будущей
    // команды. Новая команда недособранный хвост бросает: иначе её аргумент
    // ушёл бы в чужой хвост, а сама она получила бы пустоту.
    uint8_t tail_left_  = 0; // сколько байт хвоста ещё придёт; чей - last_cmd_
    uint16_t tail_word_ = 0; // собранное: порт у #10, LEN у #14/#15, ADR.L у #17
    // Регистр DE процессора карты для #18-#1B: плееры читают через него
    // переменные ПЗУ карты (mem_peek).
    uint16_t de_ = 0;
    // Блок памяти карты после хвоста #14/#15, LEN байт. ОЗУ карты нет:
    // принятые байты глотаются, отдаваемые - нули.
    uint16_t sink_left_  = 0; // #14: сколько байт блока ещё проглотить
    uint16_t zeros_left_ = 0; // #15: сколько нулей ещё отдать

    // Регистр вывода - байт, который читается из GSDAT, когда отдавать
    // больше нечего. ПЗУ карты при старте кладёт туда ноль (адрес #014B:
    // XOR A / OUT (3),A), по нему софт после #F3 ждёт готовности карты.
    uint8_t out_reg_ = 0;

    // Ответ: байты и сколько ещё не отдано.
    uint8_t resp_[16] = {};
    uint8_t resp_n_   = 0;
    uint8_t resp_pos_ = 0;

    Stream stream_         = Stream::None;
    uint32_t stream_bytes_ = 0;

    // --- Внутренние порты карты ---
    //
    // Снаружи к ним ходят командами #10 (запись) и #11 (чтение). Через них
    // же классический детект: два порта связаны с битами состояния, и
    // подделка, которая этого не воспроизводит, выдаёт себя сразу.
    //
    //   #00        расширенная память, D0..D3 - страницы по 32 КБ
    //   #01  чт.   код команды от ZX
    //   #02  чт.   байт данных от ZX
    //   #03  зап.  регистр вывода: байт, который ZX прочитает из GSDAT
    //   #04  чт.   слово состояния
    //   #05  зап.  сброс бита команд
    //   #06..#09   громкости каналов 1..4, шесть бит
    //   #0A        обращение ставит бит данных в инверсию бита 0 порта #00
    //   #0B        обращение ставит бит команд по биту 5 громкости
    //
    // #03 - на нём детект плеера под GS: пишет туда 0xAA, читает порт
    // данных, потом 0x55 и читает снова. Записанное должно вернуться через
    // GSDAT.
    //
    // #0B берёт бит 5 порта #09, а не #06: текстовая раскладка называет #06,
    // схема старой платы GS - #09.
    uint8_t inner_[kInnerPortCount] = {};

    uint8_t order_  = 0; // set_position
    uint8_t row_    = 0;
    bool hx_busy_   = false; // #F5/#F6: не играть эффекты (регистр HX)
    uint8_t cur_fx_ = 0;     // #2E, текущий эффект
    // Сэмплы эффектов: таблица здесь, байты у платформы. Номер n лежит в
    // fx_[n-1] - нумерация карты начинается с единицы.
    FxSample fx_[kFxSamplesMax] = {};
    uint8_t fx_count_           = 0;
    uint32_t fx_refused_        = 0; // #38 без места в таблице
    uint8_t fx_loading_         = kFxNone;
    uint32_t fx_capacity_       = 0;
    // Приоритет того, что играет в канале: по нему #39 решает, можно ли
    // отнять канал.
    uint8_t fx_ch_priority_[kFxChannels] = {};
    // Каналы, занятые запуском и не освобождённые #3A или сбросом. Пишет
    // только обработчик шины.
    volatile uint8_t fx_active_ = 0;
    // Номер последнего запуска по каналу и номера доигравших от звука.
    uint16_t fx_seq_[kFxChannels]          = {};
    const std::atomic<uint16_t>* fx_ended_ = nullptr;
    FxStart fx_start_                      = {};
    uint8_t fx_stop_mask_                  = 0;
    uint8_t fx_pending_handle_             = 0; // номер сэмпла, пока идёт хвост #88-#9B
    Event pending_event_                   = Event::None;
    // Начальные значения ставит reset(): ненулевой инициализатор увёл бы
    // объект в .data.
    uint8_t module_volume_    = 0; // #2A и #35
    uint8_t fx_master_volume_ = 0; // #2B
    uint8_t fx_volume_        = 0; // #3D
    uint8_t fx_volume_eff_    = 0; // та из двух, что записана последней

    // Узнавание заливаемой программы: три последних байта и признак того,
    // что среди них попался вызов приёмника потока из ПЗУ карты.
    uint32_t blob_window_ = 0;
    bool blob_is_loader_  = false;
    // Сумма и длина заливаемой программы - по ним она и опознаётся, а
    // дальше вместо неё отдаётся готовый ответ.
    uint32_t blob_crc_       = 0;
    uint16_t blob_len_       = 0;
    uint8_t blob_answer_[2]  = {};
    uint8_t blob_answer_len_ = 0;
    // Места, которые плеер правит под конкретный файл, идут в сумму нулём:
    // иначе проигрыватель MIDI не опознать, на проводе он каждый раз свой.
    uint8_t blob_skip_at_ = 0; // сколько таких мест уже пройдено
    // Залит проигрыватель MIDI. Подмены у него нет, и признак нужен ровно
    // затем, чтобы на пробу #02 ответить не нулём.
    bool midi_blob_loaded_ = false;
    // Блоб запущен и обслуживает свои команды.
    bool midi_active_ = false;
    bool midi_ready_  = false;
    // Аргумент #01 приходит ВТОРЫМ байтом в порт команд, а не в порт
    // данных: следующий код - это он, а не команда.
    bool midi_await_state_arg_           = false;
    MidiFileFacts midi_facts_            = {};
    const uint8_t* midi_names_           = nullptr;
    const uint8_t* midi_info_            = nullptr;
    uint8_t midi_state_[kMidiStateBytes] = {};
    const uint8_t* midi_tracks_          = nullptr;
    uint8_t midi_track_count_            = 0;
    const uint8_t* midi_frame_           = nullptr;
    volatile uint16_t midi_frame_len_    = 0;
    std::atomic<bool> midi_frame_wanted_{false};
    uint32_t midi_frames_empty_  = 0;
    uint16_t last_blob_len_      = 0;
    uint32_t last_blob_crc_      = 0;
    uint32_t midi_frames_served_ = 0;

    // Залит резидент плеера: он отвечает на свои команды #ED/#EE/#EF и
    // живёт, пока не придёт чужая - тогда настоящий возвращается в прошивку,
    // и плеер заливает его заново.
    bool resident_active_ = false;
    // Счётчики поддиалога паттерна - только для журнала.
    uint32_t pattern_asks_    = 0;
    uint32_t pattern_served_  = 0;
    uint8_t last_pattern_pos_ = 0;
    uint8_t last_pattern_num_ = 0;
    // После блока состояния резидент ждёт ещё одну команду: #FF - больше
    // ничего не надо, иначе это номер позиции и нужен паттерн.
    bool resident_await_pattern_ = false;
    // Нота, отданная плееру в прошлый раз: по совпадению взводится бит
    // "не менялось". 0xFF - не отдавали ещё ни разу.
    uint8_t served_note_[kModuleChannels] = {};
    // Готовый блок состояния в порядке выдачи (смещения kTelemetry*).
    uint8_t telemetry_[kTelemetryBytes] = {};
    // Блок, собранный потоком, и счётчик его записи: нечётный - запись идёт
    // (обработчик вытеснил поток посреди неё), выдаётся прошлый блок.
    uint8_t telemetry_staged_[kTelemetryBytes] = {};
    volatile uint32_t telemetry_seq_           = 0;

    // Принятый модуль и выдача куска из него: паттерн длиннее буфера
    // ответа, поэтому отдаётся прямо из приёмного буфера, без копии.
    const uint8_t* module_image_ = nullptr;
    uint32_t module_len_         = 0;
    const uint8_t* block_ptr_    = nullptr;
    uint16_t block_left_         = 0;
};

} // namespace devices::gs
