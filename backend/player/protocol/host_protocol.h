// SPDX-License-Identifier: MIT
#pragma once

// Протокол с хостом, версия 2: плата командует, хост исполняет.
//
// Чистая логика без обращений к железу: физический уровень (hostlink на
// плате) только перекладывает байты, PC-тесты гоняют этот класс напрямую.
//
// Плата не считает, сколько байт хост прочитал. Команда стоит
// выставленной, пока хост не скажет "выполнено"; перечитать кадр можно
// в любой момент, тем же вооружением.

#include <cstdint>

namespace player::protocol {

// Состояние воспроизведения для нижней строки хоста: третий байт кадра
// позиции (kStPosition).
enum class PlaybackState : uint8_t {
    Loading = 0, // идёт загрузка, звука ещё нет
    Playing = 1,
    Ended   = 2,
};

// Старшие биты того же байта: состояние занимает младшие два, остальное -
// признаки управления. Хост обязан брать состояние маской, иначе пауза
// прочтётся как неизвестное состояние.
inline constexpr uint8_t kStateMask     = 0x03;
inline constexpr uint8_t kStatePaused   = 0x04; // воспроизведение остановлено
inline constexpr uint8_t kStateSeekable = 0x08; // перемотка доступна

// Что хост просит сделать с воспроизведением (kHcTransport).
enum class TransportOp : uint8_t {
    None         = 0,
    PauseToggle  = 1,
    SeekForward  = 2,
    SeekBackward = 3,
};

// Чем кончается работа конфигуратора (kHcConfigExit).
enum class ConfigExit : uint8_t {
    None    = 0,
    Discard = 1, // выйти в обычный режим, настройки не сохранять
    Save    = 2, // сохранить и выйти в обычный режим
    Reenter = 3, // перезагрузиться снова в конфигуратор
};

class HostProtocol {
public:
    static constexpr uint16_t kDataBufferBytes = 4096;

    // Кадр аргументов на плате. Хост читает кадр в буфер на 17 байт: полезная
    // часть длиннее 16 байт (спектр, имя платы) требует правки и плагина.
    // kMaxPayload на байт меньше: контрольный байт есть у всех команд.
    static constexpr uint8_t kArgBytes     = 20;
    static constexpr uint8_t kMaxPayload   = kArgBytes - 1;
    static constexpr uint8_t kCommandBytes = 8;

    // --- Команды платы (значения статуса) ---
    // 0x00 и 0xFF - "команды нет". 0xFF - то, что Z80 читает с
    // неподведённой шины, поэтому неудачное чтение статуса безвредно.
    static constexpr uint8_t kStNone         = 0x00;
    static constexpr uint8_t kStNoneAlt      = 0xff;
    static constexpr uint8_t kStResetDone    = 0x11; // имя платы + версия
    static constexpr uint8_t kStReadFast     = 0x12; // 4096 байт одним потоком
    static constexpr uint8_t kStReadSlow     = 0x13; // ровно len байт побайтно
    static constexpr uint8_t kStSessionReady = 0x14;
    static constexpr uint8_t kStFileInfo     = 0x15;
    static constexpr uint8_t kStPosition     = 0x16;
    static constexpr uint8_t kStVu           = 0x17;
    static constexpr uint8_t kStSpectrum     = 0x18;
    static constexpr uint8_t kStPsramStats   = 0x19;
    static constexpr uint8_t kStEngineLoad   = 0x1a; // средняя полифония и загрузка ядра
    // Страница настроек пересчитана: ПЗУ-конфигуратору можно перерисовывать
    // экран. Аргумент - признак, приняты ли значения.
    static constexpr uint8_t kStConfigDone = 0x1b;

    // --- Команды хоста (запись в порт статуса) ---
    static constexpr uint8_t kHcReset = 0x01;
    static constexpr uint8_t kHcStart = 0x02;
    static constexpr uint8_t kHcDone  = 0x03;
    static constexpr uint8_t kHcNak   = 0x04;
    static constexpr uint8_t kHcTrace = 0x05;
    // Управление воспроизведением: arg0 - TransportOp. Обмен не меняет, как
    // и маркер: неподтверждённая команда платы возвращается на шину.
    static constexpr uint8_t kHcTransport = 0x06;
    // Конфигуратор: пересчитать страницу настроек по значениям, которые
    // ПЗУ в неё положило. Ответ - kStConfigDone.
    static constexpr uint8_t kHcConfigRefresh = 0x07;
    // Конфигуратор: уйти в перезагрузку. arg0 - ConfigExit.
    static constexpr uint8_t kHcConfigExit = 0x08;

    // Вид телеметрии - номер k: флаг в полезной части kHcStart - бит 1 << k,
    // код ответа - kStFileInfo + k. На этом держится обход в try_arm_telemetry.
    static constexpr uint8_t kTelemetryFileInfoLsb   = 0;
    static constexpr uint8_t kTelemetryPositionLsb   = 1;
    static constexpr uint8_t kTelemetryVuLsb         = 2;
    static constexpr uint8_t kTelemetrySpectrumLsb   = 3;
    static constexpr uint8_t kTelemetryPsramStatsLsb = 4;
    static constexpr uint8_t kTelemetryEngineLoadLsb = 5;
    static constexpr uint8_t kTelemetryKindCount     = 6;
    static constexpr uint8_t kTelemetryFileInfo      = 1u << kTelemetryFileInfoLsb;
    static constexpr uint8_t kTelemetryPosition      = 1u << kTelemetryPositionLsb;
    static constexpr uint8_t kTelemetryVu            = 1u << kTelemetryVuLsb;
    static constexpr uint8_t kTelemetrySpectrum      = 1u << kTelemetrySpectrumLsb;
    static constexpr uint8_t kTelemetryPsramStats    = 1u << kTelemetryPsramStatsLsb;
    static constexpr uint8_t kTelemetryEngineLoad    = 1u << kTelemetryEngineLoadLsb;

    // Сколько раз подряд принимается "не понял" на одну команду. Без
    // предела физически нечитаемое место давало бы вечный цикл.
    static constexpr uint8_t kMaxNak = 3;

    struct Callbacks {
        void* user                                                                                                                          = nullptr;
        void (*on_session_start)(void* user, uint32_t sector_size_bytes, uint32_t file_length, uint8_t telemetry_flags, uint8_t load_order) = nullptr;
        void (*on_reset)(void* user)                                                                                                        = nullptr;
    };

    // Вооружение делает платформа: кодирует слова ответа и публикует статус
    // последним действием. Контрольный байт уже включён в args/n: его
    // считает протокол, чтобы стороны не разошлись в том, что суммируется.
    using ArmFn = void (*)(void* user, uint8_t code, const uint8_t* args, uint8_t n);
    // Снять статус, не трогая слова аргументов: байт команды хоста пришёл во
    // время вооружения (poll).
    using HideFn = void (*)(void* user);

    HostProtocol() { reset(); }

    void set_callbacks(const Callbacks& cb) { callbacks_ = cb; }
    void set_arm(ArmFn fn, void* user, HideFn hide = nullptr) {
        arm_fn_   = fn;
        arm_user_ = user;
        hide_fn_  = hide;
    }

    // --- Физический уровень (вызывается из ISR записи) ---
    //
    // Две точки входа, обе - приём. Чтения протокол не видит: платформа
    // двигает индекс по готовым словам сама.
    void on_command_byte(uint8_t byte);
    void on_data_byte(uint8_t byte);

    // --- Основной поток ---
    //
    // Разобрать пришедшую команду хоста и, если ничего не выставлено,
    // вооружить следующую. Звать можно сколь угодно часто.
    void poll();

    void reset();

    // Плата закрывает сессию сама (загрузка не удалась): сбросить запрос и
    // готовность. Кадр команды хоста, подтверждение и счётчик NAK не
    // трогаются - команда на шине остаётся делом хоста.
    void end_session();

    // Запрос куска файла от логики платы. Очередь из одного элемента,
    // вооружается, когда освободится статус. Запрос в полёте один: окно
    // относится к смещению последнего запроса вызывающего.
    void request_file_chunk(uint32_t offset, uint16_t length);
    void mark_session_ready();

    // true один раз после DONE (length - принятая длина) или после сдачи
    // запроса по NAK (length 0). Новый запрос и сброс гасят признак.
    bool take_received(uint16_t& length);
    // Окно - буфер приёма; годно до следующего request_file_chunk.
    const uint8_t* data_buffer() const { return data_buf_; }

    // Степень двойки; 0 - сессии нет.
    uint32_t sector_size_bytes() const { return sector_size_bytes_; }

    // Сколько раз хост сообщил, что не разобрал аргументы, и сколько команд
    // сдано после исчерпания попыток. На исправной связке ноль; рост первого
    // - единственный внешний признак, что кадр аргументов портится по дороге.
    uint32_t nak_total() const { return nak_total_; }
    uint32_t nak_giveups() const { return nak_giveups_; }
    // Команд хоста с неизвестным кодом: на исправной связке ноль, рост -
    // сдвиг кадра команд.
    uint32_t unknown_commands() const { return unknown_commands_; }
    // Кадров команд, отброшенных при полном кольце: хост прислал три, пока
    // poll() не звали.
    uint32_t cmd_overruns() const { return cmd_overruns_; }
    // Окон, где байт потерян (счёт ISR меньше подтверждённого), и окон,
    // подтверждённых короче запроса. На исправной связке ноль, кроме хвоста
    // файла у второго.
    uint32_t data_short() const { return data_short_; }
    uint32_t short_done() const { return short_done_; }

    // Отсечённое обоими буферами считается: иначе порча не видна.
    //
    // data_overflow - байт в порт данных сверх окна (пишет ISR), done_overflow -
    // длина в DONE больше окна (пишет poll). На исправной связке оба ноль: хост
    // шлёт не больше kDataBufferBytes.
    // arg_overflow - вооружение с полезной частью длиннее kMaxPayload.
    // Ошибка нашего кода: появилась команда, под которую не увеличили
    // kArgBytes.
    uint32_t data_overflow() const { return data_overflow_; }
    uint32_t done_overflow() const { return done_overflow_; }
    uint32_t arg_overflow() const { return arg_overflow_; }
    // Событий журнала, потерянных при полном кольце: разбор не успевал.
    uint32_t debug_events_lost() const { return debug_events_lost_; }

    // Счётчики сбоев связи разом. Все накопительные с включения; разность
    // против снимка на старте сессии (kHcStart) - сбои этой загрузки.
    struct Counters {
        uint32_t naks          = 0;
        uint32_t nak_giveups   = 0;
        uint32_t data_overflow = 0;
        uint32_t done_overflow = 0;
        uint32_t arg_overflow  = 0;
        uint32_t events_lost   = 0;
        uint32_t unknown       = 0;
        uint32_t data_short    = 0;
        uint32_t short_done    = 0;
    };
    Counters counters() const;
    // Сбои с последнего kHcStart.
    Counters session_counters() const;
    // Сбои прошлой сессии - между двумя последними kHcStart: итог сессии,
    // которую сменили до конца её плана, печатается уже на старте следующей.
    Counters previous_session_counters() const { return prev_session_; }

    // Кто кого ждёт - для строки журнала при залипании: хост не опрашивает
    // (вооружено, не подтверждено), плата ждёт окна (awaiting_data) или
    // вооружать нечего (всё по нулям).
    struct StateSnapshot {
        uint8_t armed_code        = 0;
        bool awaiting_ack         = false;
        uint8_t readiness         = 0; // 0 не готова, 1 готова, 2 объявлена
        bool queued_request       = false;
        bool awaiting_data        = false;
        uint8_t telemetry_dirty   = 0;
        uint16_t data_pos         = 0;
        uint32_t cmd_overruns     = 0;
        uint32_t unknown_commands = 0;
        uint32_t data_short       = 0;
        uint32_t short_done       = 0;
    };
    StateSnapshot state_snapshot() const;

    // --- Телеметрия ---
    // Длительность ещё не известна (крупный .mid дочитывается фоном) -
    // kTimeUnknown в минутах: хост рисует прочерки и ждёт повторного кадра.
    // Кадр публикуется без сравнения с прежним, поэтому досылка - просто
    // второй вызов с настоящим временем.
    static constexpr uint8_t kTimeUnknown = 0xff;
    void set_file_info(uint8_t minutes, uint8_t seconds, uint16_t num_samples, uint16_t num_patterns, uint16_t num_instruments);
    // flags - kStatePaused/kStateSeekable поверх состояния.
    void set_position(uint8_t minutes, uint8_t seconds, PlaybackState state, uint8_t flags = 0);

    // Запрос управления от хоста; забирает его и обнуляет. Зовёт сеанс.
    TransportOp take_transport_request();

    // Конфигуратор просит пересчитать страницу. Снимается чтением, как и
    // транспорт: решение принимает цикл сеанса, не прерывание.
    bool take_config_refresh();
    // Чем конфигуратор просит кончить. None - не просил.
    ConfigExit take_config_exit();
    // Ответ конфигуратору: страница пересчитана (или значения отвергнуты).
    void arm_config_done(bool accepted);
    void set_vu(uint8_t left, uint8_t right);
    void set_spectrum(const uint8_t bands16[16]);
    void set_psram_stats(uint16_t total_samples, uint16_t queued, uint16_t loaded);

    // За период опроса: typical_voices - квадратичное среднее голосов (вверх),
    // peak_voices - максимум, cpu_percent - доля Core0 на звук 0..100,
    // cull_level 0..3 - насколько движок режет голоса при перегрузке.
    void set_engine_load(uint8_t typical_voices, uint8_t peak_voices, uint8_t cpu_percent, uint8_t cull_level);

    // --- Диагностика ---
    enum class DebugEventKind : uint8_t {
        Reset,
        SessionStart, // arg0=флаги, arg1=размер сектора | порядок загрузки << 12, arg2=длина файла
        SessionReady,
        Nak,             // arg0=код команды, arg1=сколько подряд
        NakGiveup,       // arg0=код команды
        PluginTrace,     // arg0=код точки, arg1=A, arg2=B
        SessionRejected, // arg0=номер размера сектора вне 0..3
        EndedDelivered,  // хост подтвердил кадр позиции с Ended; arg0=минуты, arg1=секунды (BCD)
        Transport,       // хост попросил управление; arg0=TransportOp
    };
    struct DebugEvent {
        DebugEventKind kind;
        uint8_t arg0;
        uint16_t arg1;
        uint32_t arg2;
    };
    static constexpr uint8_t kDebugEventCapacity = 32;
    // Индексы кольца - свободно бегущие uint8_t, ячейка - индекс по модулю
    // ёмкости: верно, только если ёмкость делит 256.
    static_assert(256 % kDebugEventCapacity == 0, "the event ring capacity must divide 256");
    bool try_pop_debug_event(DebugEvent& out);

private:
    void dispatch_host_command();
    void start_session();
    // kHcStart: прошлая сессия - в prev_session_, отсчёт новой - с этой точки.
    void rotate_session_counters();
    // Позиция без сравнения с прежней: новой сессии кадр нужен всегда.
    void store_position(uint8_t minutes_bcd, uint8_t seconds_bcd, uint8_t state);
    void arm_next();
    void arm(uint8_t code, const uint8_t* payload, uint8_t payload_len);
    void rearm();
    // NAK или неизвестная команда: вернуть неподтверждённую команду, после
    // kMaxNak попыток сдаться.
    void retry_armed();
    void try_arm_telemetry();
    void handle_done();
    void push_debug_event(DebugEventKind kind, uint8_t arg0, uint16_t arg1, uint32_t arg2);

    Callbacks callbacks_;
    ArmFn arm_fn_   = nullptr;
    HideFn hide_fn_ = nullptr;
    void* arm_user_ = nullptr;

    // Команда спрятана: байт команды хоста пришёл во время её вооружения.
    // Возвращается, когда кадр хоста разобран (poll). published_ - за
    // текущий вызов poll команда уже выставлена (arm или rearm).
    bool status_hidden_ = false;
    bool published_     = false;

    // Команда хоста. Пишет ISR записи, читает основной поток.
    uint8_t cmd_[kCommandBytes] = {}; // разбираемый кадр, копия слота
    // Кольцо кадров команд: пишет ISR (слоты, cmd_head_, cmd_pos_,
    // cmd_drop_), читает poll() (cmd_tail_).
    uint8_t cmd_slots_[2][kCommandBytes] = {};
    volatile uint8_t cmd_head_           = 0;
    volatile uint8_t cmd_tail_           = 0;
    uint8_t cmd_pos_                     = 0;
    bool cmd_drop_                       = false;
    uint32_t cmd_overruns_               = 0;

    // Команда вооружена и ещё не подтверждена. Статус на шине снимает
    // платформа на первом байте команды хоста, а этот флаг - только
    // подтверждение. Пока он взведён, вооружать новое нельзя, иначе
    // подтверждение достанется не той команде (poll()), и возвращать по
    // NAK или маркеру можно только такую команду (retry_armed).
    bool awaiting_ack_ = false;

    // Последняя вооружённая команда - для перевыставления по "не понял".
    uint8_t armed_code_            = kStNone;
    uint8_t armed_args_[kArgBytes] = {};
    uint8_t armed_len_             = 0;
    uint8_t nak_streak_            = 0;
    uint32_t nak_total_            = 0;
    uint32_t nak_giveups_          = 0;
    uint32_t unknown_commands_     = 0;
    uint32_t data_short_           = 0;
    uint32_t short_done_           = 0;
    uint8_t short_streak_          = 0;
    uint32_t data_overflow_        = 0; // пишет ISR записи
    uint32_t done_overflow_        = 0;
    uint32_t arg_overflow_         = 0;
    uint32_t debug_events_lost_    = 0;
    Counters session_base_{}; // counters() на последнем kHcStart
    Counters prev_session_{}; // session_counters() на последнем kHcStart

    // Готова - mark_session_ready, объявлена хосту - arm_next, один раз.
    enum class Readiness : uint8_t { NotReady, Ready, Announced };
    Readiness readiness_        = Readiness::NotReady;
    uint32_t sector_size_bytes_ = 0;
    uint32_t file_length_       = 0;
    uint8_t telemetry_flags_    = 0;

    // Данные от хоста.
    uint8_t data_buf_[kDataBufferBytes] = {};
    uint16_t data_pos_                  = 0;
    bool awaiting_data_                 = false;
    uint32_t request_offset_            = 0;
    uint16_t request_length_            = 0;
    // Окно принято (или запрос сдан) и ещё не забрано take_received.
    bool received_         = false;
    uint16_t received_len_ = 0;

    bool has_queued_request_ = false;
    uint32_t queued_offset_  = 0;
    uint16_t queued_length_  = 0;

    // Телеметрия: последние значения и маска "изменилось, ещё не отправлено"
    // (биты kTelemetry*).
    uint8_t file_info_[8] = {};
    uint8_t position_[3]  = {};
    // Последний запрос управления от хоста; ISR пишет, сеанс забирает.
    TransportOp transport_request_  = TransportOp::None;
    bool config_refresh_request_    = false;
    ConfigExit config_exit_request_ = ConfigExit::None;
    uint8_t vu_[2]                  = {};
    uint8_t spectrum_[16]           = {};
    uint8_t psram_[6]               = {};
    uint8_t engine_load_[4]         = {};
    static_assert(sizeof(file_info_) <= kMaxPayload && sizeof(spectrum_) <= kMaxPayload && sizeof(psram_) <= kMaxPayload, "the telemetry kind fits the frame");
    uint8_t telemetry_dirty_  = 0;
    uint8_t telemetry_cursor_ = 0; // вид, с которого начнётся следующий обход

    // Пишет poll, читает try_pop_debug_event - один поток Core1.
    DebugEvent debug_events_[kDebugEventCapacity] = {};
    uint8_t debug_head_                           = 0;
    uint8_t debug_tail_                           = 0;
};

} // namespace player::protocol
