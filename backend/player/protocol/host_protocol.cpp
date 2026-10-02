// SPDX-License-Identifier: MIT
#include "player/protocol/host_protocol.h"

#include <atomic>
#include <cstring>

#include "platform/compiler.h"
#include "platform/hot_path.h"

// На RP2350 в SRAM (на PC макрос пустой) всё, кроме сеттеров телеметрии и
// try_pop_debug_event: on_command_byte и on_data_byte вызываются из ISR
// записи, poll() крутится в горячем цикле Core1 рядом с загрузкой. Флеш и
// PSRAM на одном QMI, промах кэша XIP во время догрузки сэмплов стоит
// микросекунд.

namespace player::protocol {

namespace {

// Размер сектора в START - номер 0..3: kSectorBytesMin << номер.
constexpr uint8_t kSectorEnumCount = 4;
constexpr uint32_t kSectorBytesMin = 128;

// Имя платы и версия, ASCII, ровно 16 байт без нуля: это байты на шину,
// а не C-строка (литерал добавил бы 17-й байт). Версия синхронизируется с
// CMakeLists.txt вручную: модуль платформонезависим, макросов pico-sdk
// не видит.
constexpr uint8_t kBoardName[16] = {'P', 'I', '-', 'M', 'U', 'S', 'I', 'C', 'A', ' ', 'V', '0', '.', '4', '5', ' '};
static_assert(sizeof(kBoardName) <= HostProtocol::kMaxPayload, "the board name fits the frame");

uint8_t SOUNDSINTH_HOT_PATH(to_bcd8)(uint8_t v) {
    return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
}

void SOUNDSINTH_HOT_PATH(put_u16)(uint8_t* out, uint16_t v) {
    out[0] = static_cast<uint8_t>(v & 0xffu);
    out[1] = static_cast<uint8_t>(v >> 8);
}

void SOUNDSINTH_HOT_PATH(put_u32)(uint8_t* out, uint32_t v) {
    put_u16(out, static_cast<uint16_t>(v));
    put_u16(out + 2, static_cast<uint16_t>(v >> 16));
}

uint16_t SOUNDSINTH_HOT_PATH(get_u16)(const uint8_t* in) {
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

uint32_t SOUNDSINTH_HOT_PATH(get_u32)(const uint8_t* in) {
    return static_cast<uint32_t>(get_u16(in)) | (static_cast<uint32_t>(get_u16(in + 2)) << 16);
}

// Кадр чтения: смещение и длина, затем их копии - вторая проверка сверх
// суммы: хост сверяет копии и при расхождении шлёт NAK.
constexpr uint8_t kReadOffset     = 0;
constexpr uint8_t kReadLength     = 4;
constexpr uint8_t kReadOffsetCopy = 6;
constexpr uint8_t kReadLengthCopy = 10;
constexpr uint8_t kReadArgBytes   = 12;
static_assert(kReadArgBytes <= HostProtocol::kMaxPayload, "the read frame fits the status frame");

// Быстрый путь: хост читает целыми секторами и отдаёт ровно 4096 байт, поэтому
// смещение кратно сектору и все 4096 есть в файле, иначе хост читает за концом.
// Короче 4096 просят только хвост файла.
constexpr uint16_t kFastReadMinBytes = 3072;

// Кадр START: [1..4] длина файла, [5] номер размера сектора, [6] флаги
// телеметрии, [7] порядок загрузки.
constexpr uint8_t kStartFileLength = 1;
constexpr uint8_t kStartSectorEnum = 5;
constexpr uint8_t kStartTelemetry  = 6;
constexpr uint8_t kStartLoadOrder  = 7;

} // namespace

// Не встраивается: сброс редкий, копия в poll только раздувала бы SRAM.
SOUNDSINTH_NOINLINE SOUNDSINTH_HOT_PATH_ATTR("hp_reset")
void HostProtocol::reset() {
    push_debug_event(DebugEventKind::Reset, 0, 0, 0);

    // Кольцо кадров команд (cmd_pos_, cmd_head_, cmd_tail_) не трогается:
    // пишет его ISR, а кадр RESET poll() уже забрал.
    awaiting_ack_  = false;
    status_hidden_ = false;

    armed_code_   = kStNone;
    armed_len_    = 0;
    nak_streak_   = 0;
    short_streak_ = 0;

    readiness_         = Readiness::NotReady;
    sector_size_bytes_ = 0;
    file_length_       = 0;
    telemetry_flags_   = 0;

    data_pos_       = 0;
    awaiting_data_  = false;
    request_offset_ = 0;
    request_length_ = 0;
    received_       = false;

    has_queued_request_ = false;

    telemetry_dirty_  = 0;
    telemetry_cursor_ = 0;

    if (callbacks_.on_reset) callbacks_.on_reset(callbacks_.user);
}

// --- Физический уровень ---

SOUNDSINTH_HOT_PATH_ATTR("hp_on_command_byte")
void HostProtocol::on_command_byte(uint8_t byte) {
    // Здесь, а не в основном потоке: хост начал писать команду - значит
    // текущую он уже забрал. Если ждать разбора, хост успеет опросить
    // статус ещё раз и исполнить ту же команду повторно.
    //
    // Саму ячейку статуса обнуляет платформа, одной записью. Это не
    // "команда закрыта": закрывает её только подтверждение (awaiting_ack_).
    //
    // Кольцо на два кадра: хост шлёт команды парами с паузой около 1.1 мс,
    // а Core1 бывает не зовёт poll() дольше. Решение о приёме - на первом
    // байте: при полном кольце кадр не пишется вовсе, иначе его байты легли
    // бы поверх ещё не разобранного.
    if (cmd_pos_ == 0) cmd_drop_ = static_cast<uint8_t>(cmd_head_ - cmd_tail_) >= 2u;
    if (!cmd_drop_) cmd_slots_[cmd_head_ & 1u][cmd_pos_] = byte;
    if (++cmd_pos_ >= kCommandBytes) {
        cmd_pos_ = 0;
        if (cmd_drop_) {
            ++cmd_overruns_;
        } else {
            std::atomic_signal_fence(std::memory_order_release);
            cmd_head_ = static_cast<uint8_t>(cmd_head_ + 1u);
        }
    }
}

SOUNDSINTH_HOT_PATH_ATTR("hp_on_data_byte")
void HostProtocol::on_data_byte(uint8_t byte) {
    // Проверка переполнения - здесь и безусловно: числом байт с шины
    // распоряжается хост, без проверки запись уходит за буфер. Отсечённое
    // считается (data_overflow).
    if (data_pos_ >= kDataBufferBytes) {
        ++data_overflow_;
        return;
    }
    data_buf_[data_pos_] = byte;
    ++data_pos_;
}

// --- Основной поток ---

SOUNDSINTH_HOT_PATH_ATTR("hp_poll")
void HostProtocol::poll() {
    // Снимок приёма команд хоста - до последней проверки кольца: байт,
    // пришедший после неё, покажет сверка после вооружения.
    const uint8_t head0   = cmd_head_;
    const uint8_t pos0    = *const_cast<volatile uint8_t*>(&cmd_pos_);
    const uint32_t drops0 = *const_cast<volatile uint32_t*>(&cmd_overruns_);
    published_            = false;
    while (cmd_tail_ != cmd_head_) {
        std::atomic_signal_fence(std::memory_order_acquire);
        std::memcpy(cmd_, cmd_slots_[cmd_tail_ & 1u], kCommandBytes);
        cmd_tail_ = static_cast<uint8_t>(cmd_tail_ + 1u); // слот свободен до разбора
        dispatch_host_command();
    }
    // Спрятанная команда возвращается, когда кадр хоста, пришедший поверх
    // её вооружения, разобран: кольцо пусто и новый кадр не начат. Маркер и
    // NAK возвращают её сами; START, например, - нет.
    if (status_hidden_ && cmd_tail_ == cmd_head_ && *const_cast<volatile uint8_t*>(&cmd_pos_) == 0) {
        status_hidden_ = false;
        if (awaiting_ack_ && !published_) rearm();
    }
    // Только при !awaiting_ack_: статус снят на первом байте команды хоста, а
    // разобрана она после восьмого; вооружённое в эту щель получит чужое
    // подтверждение.
    if (awaiting_ack_) return;
    arm_next();
    // Байт команды хоста во время вооружения: ISR снял статус раньше, чем
    // платформа выставила новый, и хост увидел бы команду до разбора своего
    // кадра (исполнил бы её дважды или отдал подтверждение не той). Статус
    // прячется до разбора кадра.
    std::atomic_signal_fence(std::memory_order_acquire);
    if (awaiting_ack_ &&
        (cmd_head_ != head0 || *const_cast<volatile uint8_t*>(&cmd_pos_) != pos0 || *const_cast<volatile uint32_t*>(&cmd_overruns_) != drops0)) {
        if (hide_fn_) hide_fn_(arm_user_);
        status_hidden_ = true;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("hp_dispatch_host_command")
void HostProtocol::dispatch_host_command() {
    switch (cmd_[0]) {
        case kHcReset:
            reset();
            arm(kStResetDone, kBoardName, sizeof(kBoardName));
            return;

        case kHcStart:
            start_session();
            return;

        case kHcDone:
            // Кадр конца трека дошёл: цепочка "трек доиграл - Ended доставлен -
            // сброс" читается по журналу.
            if (awaiting_ack_ && armed_code_ == kStPosition && (armed_args_[2] & kStateMask) == static_cast<uint8_t>(PlaybackState::Ended)) {
                push_debug_event(DebugEventKind::EndedDelivered, armed_args_[0], armed_args_[1], 0);
            }
            awaiting_ack_ = false;
            nak_streak_   = 0;
            handle_done();
            return;

        case kHcNak:
            ++nak_total_;
            push_debug_event(DebugEventKind::Nak, armed_code_, nak_streak_, awaiting_ack_ ? 1u : 0u);
            retry_armed();
            return;

        case kHcTransport:
            // Запрос копится до разбора сеансом: ISR решений о звуке не
            // принимает. Неизвестный код молча отбрасывается - искажение на
            // шине не должно ставить трек на паузу.
            if (cmd_[1] >= static_cast<uint8_t>(TransportOp::PauseToggle) && cmd_[1] <= static_cast<uint8_t>(TransportOp::SeekBackward)) {
                transport_request_ = static_cast<TransportOp>(cmd_[1]);
                push_debug_event(DebugEventKind::Transport, cmd_[1], 0, 0);
            }
            // Как и маркер: обмен не меняет, снятую команду вернуть.
            if (awaiting_ack_) rearm();
            return;

        case kHcConfigRefresh:
            // Разбирает цикл сеанса: пересчёт трогает настройки и страницу,
            // в прерывании такому не место. Ответ придёт kStConfigDone.
            config_refresh_request_ = true;
            return;

        case kHcConfigExit:
            if (cmd_[1] >= static_cast<uint8_t>(ConfigExit::Discard) && cmd_[1] <= static_cast<uint8_t>(ConfigExit::Reenter)) {
                config_exit_request_ = static_cast<ConfigExit>(cmd_[1]);
            }
            if (awaiting_ack_) rearm();
            return;

        case kHcTrace: {
            const uint16_t a = get_u16(&cmd_[2]);
            const uint16_t b = get_u16(&cmd_[4]);
            push_debug_event(DebugEventKind::PluginTrace, cmd_[1], a, b);
            // Маркер обмен не меняет: снятую с шины неподтверждённую команду вернуть.
            if (awaiting_ack_) rearm();
            return;
        }

        default:
            // Неизвестная команда (искажённая на шине): её первый байт уже
            // снял статус, и неподтверждённую команду платы хост больше не
            // увидит. Вернуть, как по NAK, с тем же пределом попыток.
            // Счётчик - единственный признак сдвига кадра команд.
            ++unknown_commands_;
            retry_armed();
            return;
    }
}

void HostProtocol::end_session() {
    readiness_          = Readiness::NotReady;
    awaiting_data_      = false;
    has_queued_request_ = false;
    telemetry_dirty_    = 0;
}

SOUNDSINTH_HOT_PATH_ATTR("hp_retry_armed")
void HostProtocol::retry_armed() {
    // Уже подтверждённую команду не возвращать: poll() тут же вооружил бы
    // поверх неё следующую, и подтверждение хоста за старую досталось бы
    // новой (окно с длиной 0 - короткое чтение, запрос потерян).
    if (!awaiting_ack_) return;
    ++nak_streak_;
    if (nak_streak_ < kMaxNak) {
        // Повтор окна ложится с начала: хост при сбое посреди окна (прямой
        // проброс с карты) шлёт NAK поверх уже отданных байт.
        if (awaiting_data_) data_pos_ = 0;
        rearm();
        return;
    }
    // Сдаться.
    ++nak_giveups_;
    nak_streak_   = 0;
    awaiting_ack_ = false; // кадр закрыт, хоть и без успеха
    push_debug_event(DebugEventKind::NakGiveup, armed_code_, 0, 0);
    // Позиция с Ended не теряется: только по ней хост уходит на следующий
    // файл, а повторного set_position с тем же значением не будет. Кадр
    // предлагается снова, пока хост его не заберёт или не сбросит сессию.
    if (armed_code_ == kStPosition && armed_args_[2] == static_cast<uint8_t>(PlaybackState::Ended)) {
        telemetry_dirty_ |= kTelemetryPosition;
    }
    if (awaiting_data_) {
        // Файловый запрос сдан: наверх уходит короткое чтение, и загрузка
        // проваливается заметно, а не висит в ожидании данных, которых не
        // будет.
        awaiting_data_ = false;
        received_len_  = 0;
        received_      = true;
    }
}

SOUNDSINTH_HOT_PATH_ATTR("hp_handle_done")
void HostProtocol::handle_done() {
    if (!awaiting_data_) {
        // Команда без данных: подтверждения достаточно.
        return;
    }
    awaiting_data_ = false;

    uint16_t written = get_u16(&cmd_[1]);

    // Длина пришла с шины и могла исказиться. Без отсечки испорченный
    // байт отправил бы чтение за буфер; больше окна хост не присылает.
    if (written > kDataBufferBytes) {
        ++done_overflow_;
        written = kDataBufferBytes;
    }

    // ISR насчитал меньше, чем хост подтвердил: байт окна потерян по дороге
    // (переполнение очереди записи), данные после него сдвинуты. Окно
    // просится снова, но не больше kMaxNak раз подряд, - потом короткое
    // чтение.
    uint16_t len = (written < request_length_) ? written : request_length_;
    if (data_pos_ < written) {
        ++data_short_;
        if (++short_streak_ < kMaxNak) {
            queued_offset_      = request_offset_;
            queued_length_      = request_length_;
            has_queued_request_ = true;
            return;
        }
        len = 0;
    }
    short_streak_ = 0;
    if (written < request_length_) ++short_done_;
    received_len_ = len;
    received_     = true;
}

SOUNDSINTH_HOT_PATH_ATTR("hp_take_received")
bool HostProtocol::take_received(uint16_t& length) {
    if (!received_) return false;
    received_ = false;
    length    = received_len_;
    return true;
}

// --- Вооружение ---

// Одна копия: зовут четыре места, встроенная во все стоила бы ~180 байт SRAM.
SOUNDSINTH_NOINLINE SOUNDSINTH_HOT_PATH_ATTR("hp_arm")
void HostProtocol::arm(uint8_t code, const uint8_t* payload, uint8_t payload_len) {
    // Длиннее kMaxPayload - отсечь (иначе затрёт соседние поля) и посчитать
    // в arg_overflow.
    if (payload_len > kMaxPayload) {
        ++arg_overflow_;
        payload_len = kMaxPayload;
    }

    uint8_t sum = 0;
    for (uint8_t i = 0; i < payload_len; ++i) {
        armed_args_[i] = payload[i];
        sum            = static_cast<uint8_t>(sum + payload[i]);
    }
    armed_args_[payload_len] = sum; // контрольный байт последним
    armed_len_               = static_cast<uint8_t>(payload_len + 1);
    awaiting_ack_            = true; // закроет только подтверждение
    armed_code_              = code;
    nak_streak_              = 0;
    published_               = true;

    if (arm_fn_) arm_fn_(arm_user_, code, armed_args_, armed_len_);
}

SOUNDSINTH_HOT_PATH_ATTR("hp_rearm")
void HostProtocol::rearm() {
    // Через тот же arm_fn_, что и arm(), а не отдельным сбросом индекса на
    // платформе: кадр публикуется одним путём. Сумма уже в armed_args_.
    published_     = true;
    status_hidden_ = false;
    if (arm_fn_) arm_fn_(arm_user_, armed_code_, armed_args_, armed_len_);
}

SOUNDSINTH_HOT_PATH_ATTR("hp_start_session")
void HostProtocol::start_session() {
    const uint8_t sector_enum = cmd_[kStartSectorEnum];
    if (sector_enum >= kSectorEnumCount) {
        // Мусорный размер сектора (у команд хоста нет контрольного байта):
        // сессию не начинать, но сказать об этом.
        push_debug_event(DebugEventKind::SessionRejected, sector_enum, 0, 0);
        return;
    }
    sector_size_bytes_ = kSectorBytesMin << sector_enum;
    file_length_       = get_u32(&cmd_[kStartFileLength]);
    telemetry_flags_   = cmd_[kStartTelemetry];
    readiness_         = Readiness::NotReady;
    // Позиция ставится сразу: пока идёт загрузка, хосту есть что показать.
    store_position(to_bcd8(0), to_bcd8(0), static_cast<uint8_t>(PlaybackState::Loading));
    rotate_session_counters();
    // Порядок загрузки - в старших битах: по нему видно, какой хост говорит
    // (плагин шлёт 0, TR-DOS-приложение 1). Сектор не больше 1024 - 11 бит.
    push_debug_event(DebugEventKind::SessionStart, telemetry_flags_, static_cast<uint16_t>(sector_size_bytes_ | ((cmd_[kStartLoadOrder] & 0x0fu) << 12)),
                     file_length_);
    // Готова сессия только по mark_session_ready.
    if (callbacks_.on_session_start) {
        callbacks_.on_session_start(callbacks_.user, sector_size_bytes_, file_length_, telemetry_flags_, cmd_[kStartLoadOrder]);
    }
}

SOUNDSINTH_HOT_PATH_ATTR("hp_store_position")
void HostProtocol::store_position(uint8_t minutes_bcd, uint8_t seconds_bcd, uint8_t state) {
    position_[0]      = minutes_bcd;
    position_[1]      = seconds_bcd;
    position_[2]      = state;
    telemetry_dirty_ |= kTelemetryPosition;
}

SOUNDSINTH_HOT_PATH_ATTR("hp_arm_next")
void HostProtocol::arm_next() {
    // Порядок и есть приоритет: данные важнее телеметрии.
    if (has_queued_request_) {
        has_queued_request_ = false;

        uint8_t a[kReadArgBytes];
        put_u32(&a[kReadOffset], queued_offset_);
        put_u16(&a[kReadLength], queued_length_);
        put_u32(&a[kReadOffsetCopy], queued_offset_);
        put_u16(&a[kReadLengthCopy], queued_length_);

        request_offset_ = queued_offset_;
        request_length_ = queued_length_;
        data_pos_       = 0;
        awaiting_data_  = true;

        const bool fits = (file_length_ >= queued_offset_) && (file_length_ - queued_offset_ >= kDataBufferBytes);
        const bool fast = (sector_size_bytes_ != 0) && (queued_offset_ & (sector_size_bytes_ - 1u)) == 0 && (queued_length_ >= kFastReadMinBytes) && fits;
        arm(fast ? kStReadFast : kStReadSlow, a, sizeof(a));
        return;
    }

    if (readiness_ == Readiness::Ready) {
        readiness_ = Readiness::Announced;
        arm(kStSessionReady, nullptr, 0);
        return;
    }

    try_arm_telemetry();
}

// Коды заданы проводом; вид k обязан отвечать кодом kStFileInfo + k.
static_assert(HostProtocol::kStFileInfo + HostProtocol::kTelemetryPositionLsb == HostProtocol::kStPosition &&
                  HostProtocol::kStFileInfo + HostProtocol::kTelemetryVuLsb == HostProtocol::kStVu &&
                  HostProtocol::kStFileInfo + HostProtocol::kTelemetrySpectrumLsb == HostProtocol::kStSpectrum &&
                  HostProtocol::kStFileInfo + HostProtocol::kTelemetryPsramStatsLsb == HostProtocol::kStPsramStats &&
                  HostProtocol::kStFileInfo + HostProtocol::kTelemetryEngineLoadLsb == HostProtocol::kStEngineLoad,
              "the telemetry kind code is kStFileInfo plus the kind number");

SOUNDSINTH_HOT_PATH_ATTR("hp_try_arm_telemetry")
void HostProtocol::try_arm_telemetry() {
    // Зовётся на каждом витке цикла Core1: когда отправлять нечего, выход
    // сразу, без обхода.
    const uint8_t pending = telemetry_dirty_ & telemetry_flags_;
    if (pending == 0) return;
    // Круговой обход от курсора: каждый вид телеметрии получает очередь, ни
    // один не вытеснит остальные, как бы часто ни обновлялся.
    auto next_kind = [](uint8_t k) { return k + 1u == kTelemetryKindCount ? uint8_t{0} : static_cast<uint8_t>(k + 1u); };
    uint8_t kind   = telemetry_cursor_;
    while ((pending & (1u << kind)) == 0) {
        kind = next_kind(kind);
    }
    telemetry_dirty_  = static_cast<uint8_t>(telemetry_dirty_ & ~(1u << kind));
    telemetry_cursor_ = next_kind(kind);
    // Буфер и длина - непосредственными значениями в коде: таблица легла бы
    // в .rodata во флеш, а функция - в SRAM.
    const uint8_t* payload = file_info_;
    uint8_t len            = sizeof(file_info_);
    switch (kind) {
        case kTelemetryPositionLsb:
            payload = position_;
            len     = sizeof(position_);
            break;
        case kTelemetryVuLsb:
            payload = vu_;
            len     = sizeof(vu_);
            break;
        case kTelemetrySpectrumLsb:
            payload = spectrum_;
            len     = sizeof(spectrum_);
            break;
        case kTelemetryPsramStatsLsb:
            payload = psram_;
            len     = sizeof(psram_);
            break;
        case kTelemetryEngineLoadLsb:
            payload = engine_load_;
            len     = sizeof(engine_load_);
            break;
        default: // kTelemetryFileInfoLsb
            break;
    }
    arm(static_cast<uint8_t>(kStFileInfo + kind), payload, len);
}

// --- Управление извне ---

SOUNDSINTH_HOT_PATH_ATTR("hp_request_file_chunk")
void HostProtocol::request_file_chunk(uint32_t offset, uint16_t length) {
    received_           = false; // окно прошлого запроса больше не отдаётся
    has_queued_request_ = true;
    queued_offset_      = offset;
    queued_length_      = length;
}

// Раз на сессию, вызывающий во флеше - как сеттеры телеметрии.
void HostProtocol::mark_session_ready() {
    readiness_ = Readiness::Ready;
    push_debug_event(DebugEventKind::SessionReady, 0, 0, 0);
}

HostProtocol::StateSnapshot HostProtocol::state_snapshot() const {
    StateSnapshot s;
    s.armed_code       = armed_code_;
    s.awaiting_ack     = awaiting_ack_;
    s.readiness        = static_cast<uint8_t>(readiness_);
    s.queued_request   = has_queued_request_;
    s.awaiting_data    = awaiting_data_;
    s.telemetry_dirty  = telemetry_dirty_;
    s.data_pos         = data_pos_;
    s.cmd_overruns     = cmd_overruns_;
    s.unknown_commands = unknown_commands_;
    s.data_short       = data_short_;
    s.short_done       = short_done_;
    return s;
}

void HostProtocol::set_file_info(uint8_t minutes, uint8_t seconds, uint16_t num_samples, uint16_t num_patterns, uint16_t num_instruments) {
    // Неизвестное время идёт как есть: 0xFF - не BCD ни в одной тетраде, с
    // настоящим значением не спутать.
    const bool unknown = minutes == kTimeUnknown;
    file_info_[0]      = unknown ? kTimeUnknown : to_bcd8(minutes);
    file_info_[1]      = unknown ? kTimeUnknown : to_bcd8(seconds);
    put_u16(&file_info_[2], num_samples);
    put_u16(&file_info_[4], num_patterns);
    put_u16(&file_info_[6], num_instruments);
    // Без сравнения: reset() значения не стирает, и второй трек с той же
    // длительностью и счётчиками остался бы без file_info.
    telemetry_dirty_ |= kTelemetryFileInfo;
}

void HostProtocol::set_position(uint8_t minutes, uint8_t seconds, PlaybackState state, uint8_t flags) {
    const uint8_t m  = to_bcd8(minutes);
    const uint8_t s  = to_bcd8(seconds);
    const uint8_t st = static_cast<uint8_t>((static_cast<uint8_t>(state) & kStateMask) | flags);
    if (position_[0] == m && position_[1] == s && position_[2] == st) return;
    store_position(m, s, st);
}

TransportOp HostProtocol::take_transport_request() {
    const TransportOp op = transport_request_;
    transport_request_   = TransportOp::None;
    return op;
}

bool HostProtocol::take_config_refresh() {
    const bool want         = config_refresh_request_;
    config_refresh_request_ = false;
    return want;
}

ConfigExit HostProtocol::take_config_exit() {
    const ConfigExit op  = config_exit_request_;
    config_exit_request_ = ConfigExit::None;
    return op;
}

void HostProtocol::arm_config_done(bool accepted) {
    const uint8_t payload = accepted ? 1u : 0u;
    arm(kStConfigDone, &payload, 1);
}

void HostProtocol::set_vu(uint8_t left, uint8_t right) {
    if (vu_[0] == left && vu_[1] == right) return;
    vu_[0]            = left;
    vu_[1]            = right;
    telemetry_dirty_ |= kTelemetryVu;
}

void HostProtocol::set_spectrum(const uint8_t bands16[16]) {
    if (std::memcmp(spectrum_, bands16, sizeof(spectrum_)) == 0) return;
    std::memcpy(spectrum_, bands16, sizeof(spectrum_));
    telemetry_dirty_ |= kTelemetrySpectrum;
}

void HostProtocol::set_psram_stats(uint16_t total_samples, uint16_t queued, uint16_t loaded) {
    uint8_t next[6];
    put_u16(&next[0], total_samples);
    put_u16(&next[2], queued);
    put_u16(&next[4], loaded);
    if (std::memcmp(psram_, next, sizeof(psram_)) == 0) return;
    std::memcpy(psram_, next, sizeof(psram_));
    telemetry_dirty_ |= kTelemetryPsramStats;
}

void HostProtocol::set_engine_load(uint8_t typical_voices, uint8_t peak_voices, uint8_t cpu_percent, uint8_t cull_level) {
    uint8_t next[4];
    next[0] = typical_voices;
    next[1] = peak_voices;
    next[2] = cpu_percent;
    next[3] = cull_level;
    if (std::memcmp(engine_load_, next, sizeof(engine_load_)) == 0) return;
    std::memcpy(engine_load_, next, sizeof(engine_load_));
    telemetry_dirty_ |= kTelemetryEngineLoad;
}

// --- Диагностика ---

// Во флеше: раз на сессию, а копии счётчиков в start_session раздували
// бы SRAM.
SOUNDSINTH_NOINLINE void HostProtocol::rotate_session_counters() {
    prev_session_ = session_counters();
    session_base_ = counters();
}

HostProtocol::Counters HostProtocol::counters() const {
    Counters c;
    c.naks          = nak_total_;
    c.nak_giveups   = nak_giveups_;
    c.data_overflow = data_overflow_;
    c.done_overflow = done_overflow_;
    c.arg_overflow  = arg_overflow_;
    c.events_lost   = debug_events_lost_;
    c.unknown       = unknown_commands_;
    c.data_short    = data_short_;
    c.short_done    = short_done_;
    return c;
}

HostProtocol::Counters HostProtocol::session_counters() const {
    const Counters now = counters();
    Counters d;
    d.naks          = now.naks - session_base_.naks;
    d.nak_giveups   = now.nak_giveups - session_base_.nak_giveups;
    d.data_overflow = now.data_overflow - session_base_.data_overflow;
    d.done_overflow = now.done_overflow - session_base_.done_overflow;
    d.arg_overflow  = now.arg_overflow - session_base_.arg_overflow;
    d.events_lost   = now.events_lost - session_base_.events_lost;
    d.unknown       = now.unknown - session_base_.unknown;
    d.data_short    = now.data_short - session_base_.data_short;
    d.short_done    = now.short_done - session_base_.short_done;
    return d;
}

// Не встраивается: мест вызова семь, все редкие, копии раздували бы SRAM.
SOUNDSINTH_NOINLINE SOUNDSINTH_HOT_PATH_ATTR("hp_push_debug_event")
void HostProtocol::push_debug_event(DebugEventKind kind, uint8_t arg0, uint16_t arg1, uint32_t arg2) {
    const uint8_t head = debug_head_;
    const uint8_t next = static_cast<uint8_t>(head + 1);
    if (static_cast<uint8_t>(next - debug_tail_) > kDebugEventCapacity) {
        ++debug_events_lost_; // переполнение: новое событие теряется
        return;
    }
    debug_events_[head % kDebugEventCapacity] = DebugEvent{kind, arg0, arg1, arg2};
    debug_head_                               = next;
}

bool HostProtocol::try_pop_debug_event(DebugEvent& out) {
    if (debug_tail_ == debug_head_) return false;
    out         = debug_events_[debug_tail_ % kDebugEventCapacity];
    debug_tail_ = static_cast<uint8_t>(debug_tail_ + 1);
    return true;
}

} // namespace player::protocol
