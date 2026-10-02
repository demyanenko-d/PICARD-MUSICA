// SPDX-License-Identifier: MIT
#include "hostlink.h"

#include <cinttypes>
#include <cstdio>
#include <iterator>

#include "hardware/sync.h"
#include "pico/time.h"

#include "bus.h"
#include "platform/log.h"
#include "divmmc.h"
#include "platform/compiler.h"
#include "player/shared_state.h" // чем занято ядро в самый долгий промежуток

namespace hostlink {

namespace {

// Как часто печатается диагностика связи. poll() зовётся на каждом
// вызове, без ограничения.
constexpr uint32_t kDiagIntervalUs = 20000; // 50 раз в секунду

player::protocol::HostProtocol* s_protocol = nullptr;

// Отметка последней печати диагностики; service_and_wait() идёт через
// service(), таймер один.
uint32_t s_last_diag_us = 0;

// Наибольший промежуток между двумя опросами протокола за период. Хост
// опрашивает порт по своему времени, и всё, что цикл провёл в чужой
// работе, он видит как молчание платы. Число отличает "теряем байты" от
// "не успеваем ответить": первое видно по lost и serve_late, второе -
// только отсюда.
uint32_t s_last_poll_us    = 0;
uint32_t s_poll_gap_max_us = 0;
// Чем ядро было занято, когда промежуток оказался наибольшим: без этого
// число говорит "кто-то держал цикл", но не кто именно.
uint8_t s_poll_gap_phase = 0;

// Сколько событий протокола разбирать за проход: всплеск не становится
// чередой строк подряд.
constexpr uint8_t kProtocolDiagDrainPerPass = 8;

const char* core1_phase_name(uint8_t phase) {
    switch (static_cast<shared::Core1Phase>(phase)) {
        case shared::Core1Phase::Loop:
            return "loop";
        case shared::Core1Phase::Load:
            return "load";
        case shared::Core1Phase::TeardownWait:
            return "engine teardown";
        case shared::Core1Phase::GsModule:
            return "GS module";
        default:
            return "?";
    }
}

// Здоровье шины: переполнения RX FIFO записи - при изменении; сводка - раз
// в 10 с. Не встраивается: снимки счётчиков легли бы в кадр
// service_and_wait.
SOUNDSINTH_NOINLINE void print_bus_health(const player::protocol::HostProtocol& protocol) {
    static uint32_t s_last_overruns  = 0;
    static uint32_t s_last_report_ms = 0;
    // Статический, а не на стеке: зовётся и из service_and_wait() посреди
    // загрузки, на самом глубоком стеке Core1. Только основной поток Core1.
    static char s_out[192];
    const bus::PioStats st = bus::z80_bus_pio_get_stats();

    // Ненулевое - хост пишет чаще, чем bus_wr_isr успевает разбирать.
    if (st.wr_fifo_overruns != s_last_overruns) {
        snprintf(s_out, sizeof(s_out), "proto: WR FIFO OVERRUN total=%" PRIu32 " (+%" PRIu32 ")\n", st.wr_fifo_overruns, st.wr_fifo_overruns - s_last_overruns);
        debug_log(s_out);
        s_last_overruns = st.wr_fifo_overruns;
    }

    // Раз в 10 секунд, а не по изменению: каждая строка лога - тоже
    // нагрузка.
    // От 64-битного счётчика: миллисекунды из time_us_32() прыгают на ноль
    // раз в 71.6 минуты.
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - s_last_report_ms < 10000u) return;
    s_last_report_ms = now_ms;
    // Счётчика состоявшихся чтений нет: плата на чтение кодом не отвечает,
    // ответ выдаёт PIO из таблицы, ISR готовит следующее слово.
    // Путь портов один на оба режима: что отвечается по портам,
    // serve_late (байт не выставлен: RD_N уже высокий) и потерянные записи
    // bus_wr (lost, должно быть 0).
    const bus::GenericPortsState g = bus::generic_ports_state();
    snprintf(s_out, sizeof(s_out),
             "proto: command bytes %" PRIu32 ", data bytes %" PRIu32 " | porttab[CMD]=%08" PRIX32 " val=%02X porttab[DAT]=%08" PRIX32
             " val=%02X serve_late=%" PRIu32 " lost=%" PRIu32 "\n",
             st.cmd_bytes, st.dat_bytes, g.tab_cmd, static_cast<unsigned>(g.val_cmd), g.tab_dat, static_cast<unsigned>(g.val_dat), g.serve_late,
             st.wr_fifo_overruns);
    debug_log(s_out);
    // Промежуток и фаза - своей строкой: вместе со счётчиками выше это 231
    // байт из 192 буфера, и обрезок уходит без перевода строки, слипаясь со
    // следующей. Имя фазы - единственное, что говорит, кто держал цикл.
    snprintf(s_out, sizeof(s_out), "proto: poll not called for up to %" PRIu32 " us (phase %s)\n", s_poll_gap_max_us, core1_phase_name(s_poll_gap_phase));
    s_poll_gap_max_us = 0;
    debug_log(s_out);
    // Хост не опрашивает (armed, ack=1, байты не растут), плата ждёт окна
    // (data=1) или вооружать нечего (всё нули) - по первой строке одинаковы.
    const player::protocol::HostProtocol::StateSnapshot ps = protocol.state_snapshot();
    snprintf(s_out, sizeof(s_out),
             "proto: t=%" PRIu32 " armed=0x%02X ack=%u ready=%u q=%u data=%u pos=%u dirty=0x%02X"
             " overruns=%" PRIu32 " unknown=%" PRIu32 " short=%" PRIu32 "/%" PRIu32 "\n",
             now_ms, static_cast<unsigned>(ps.armed_code), static_cast<unsigned>(ps.awaiting_ack), static_cast<unsigned>(ps.readiness),
             static_cast<unsigned>(ps.queued_request), static_cast<unsigned>(ps.awaiting_data), static_cast<unsigned>(ps.data_pos),
             static_cast<unsigned>(ps.telemetry_dirty), ps.cmd_overruns, ps.unknown_commands, ps.data_short, ps.short_done);
    debug_log(s_out);
}

// Кольцо событий разбирается всегда: без разбора оно переполняется, и
// потери связи в строке "window check" растут ложно. Не встраивается:
// буфер строки иначе лёг бы в кадр service_and_wait на самом глубоком стеке
// Core1.
SOUNDSINTH_NOINLINE void drain_protocol_diag(player::protocol::HostProtocol& protocol) {
    using Kind = player::protocol::HostProtocol::DebugEventKind;

    player::protocol::HostProtocol::DebugEvent e;
    for (uint8_t i = 0; i < kProtocolDiagDrainPerPass; ++i) {
        if (!protocol.try_pop_debug_event(e)) break;
        // Пустая строка до разбора: необработанный вид события иначе отдал бы
        // в журнал неинициализированный буфер.
        char m[128] = {};
        switch (e.kind) {
            case Kind::Reset:
                snprintf(m, sizeof(m), "proto: reset\n");
                break;
            case Kind::SessionStart: {
                const uint32_t length = e.arg2;
                const unsigned sector = e.arg1 & 0x0fffu;
                const unsigned order  = e.arg1 >> 12;
                const unsigned tflags = e.arg0;
                snprintf(m, sizeof(m), "proto: session start len=%" PRIu32 " sector=%u tflags=0x%02X order=%u\n", length, sector, tflags, order);
                break;
            }
            case Kind::SessionReady:
                snprintf(m, sizeof(m), "proto: session ready\n");
                break;
            // Старт с размером сектора вне 0..3: хост ждал бы готовности вечно.
            case Kind::SessionRejected:
                snprintf(m, sizeof(m), "proto: session start REJECTED, sector size code %u\n", static_cast<unsigned>(e.arg0));
                break;
            // Хост не разобрал аргументы: мера чистоты канала к хосту, ноль -
            // кадры доходят целыми.
            // arg2 - команда ещё не подтверждена: NAK по живой команде. Ноль -
            // NAK по уже подтверждённой, retry_armed его глотает.
            case Kind::Nak:
                snprintf(m, sizeof(m), "proto: host did not parse frame 0x%02X, %u in a row, %s\n", static_cast<unsigned>(e.arg0),
                         static_cast<unsigned>(e.arg1), e.arg2 ? "unacked" : "already acked");
                break;
            case Kind::EndedDelivered:
                snprintf(m, sizeof(m), "proto: Ended delivered to the host at %02X:%02X\n", static_cast<unsigned>(e.arg0), static_cast<unsigned>(e.arg1));
                break;
            case Kind::NakGiveup:
                snprintf(m, sizeof(m), "proto: GAVE UP on frame 0x%02X -- retries exhausted\n", static_cast<unsigned>(e.arg0));
                break;
            // Управление от хоста: пауза, перемотка.
            case Kind::Transport: {
                using Op = player::protocol::TransportOp;
                // Значение приходит с шины и в enum может не попасть: тогда
                // switch не совпадёт ни с одной веткой и останется "?".
                // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores)
                const char* op = "?";
                switch (static_cast<Op>(e.arg0)) {
                    case Op::None:
                        op = "none";
                        break;
                    case Op::PauseToggle:
                        op = "pause";
                        break;
                    case Op::SeekForward:
                        op = "seek forward";
                        break;
                    case Op::SeekBackward:
                        op = "seek backward";
                        break;
                }
                snprintf(m, sizeof(m), "proto: transport %s\n", op);
                break;
            }
            case Kind::PluginTrace: {
                // Подписи точек здесь, на плате: у плагина каждый байт на счету
                // (слот 16 КБ), плагин шлёт только номер. Номера обязаны совпадать
                // с TRACE_* в frontend/common/bus_client.h; синхронизация ручная,
                // цена ошибки - неверная подпись в логе.
                static constexpr const char* kNames[] = {
                    "?",                       // 0
                    "plugin entry",            // 1  a, b = высота и ширина окна
                    "board probed",            // 2  a = 1 нашлась, 0 нет
                    "session opened",          // 3  a, b = размер файла (мл./ст. слово)
                    "track loaded",            // 4  a = 1 успех, 0 отказ
                    "waiting for key release", // 5
                    "keys released",           // 6
                    "main loop",               // 7  a = exit_code по умолчанию
                    "heartbeat",               // 8  a = круги цикла, b = bus_state (0 загрузка, 1 играет, 2 доиграл)
                    "entering halt",           // 9
                    "loop exit",               // 10 a = exit_code
                    "plugin end",              // 11 a = exit_code
                };
                const char* name = (e.arg0 < std::size(kNames)) ? kNames[e.arg0] : "?";
                snprintf(m, sizeof(m), "plugin: [%u] %s a=%u b=%u\n", static_cast<unsigned>(e.arg0), name, static_cast<unsigned>(e.arg1),
                         static_cast<unsigned>(e.arg2));
                break;
            }
        }
        debug_log(m);
    }
}

} // namespace

void init(player::protocol::HostProtocol& protocol, const Callbacks& callbacks) {
    s_protocol = &protocol;
    protocol.set_callbacks(callbacks);
    bus::plugin_ports_init(protocol);
}

void __not_in_flash_func(service)() {
    if (s_protocol == nullptr) return;
    s_protocol->poll();
    const uint32_t now = time_us_32();
    if (s_last_poll_us != 0) {
        const uint32_t gap = now - s_last_poll_us;
        if (gap > s_poll_gap_max_us) {
            s_poll_gap_max_us = gap;
            s_poll_gap_phase  = static_cast<uint8_t>(shared::g_core1_phase.load(std::memory_order_relaxed));
        }
    }
    s_last_poll_us = now;
    if (now - s_last_diag_us < kDiagIntervalUs) return;
    print_bus_health(*s_protocol);
    drain_protocol_diag(*s_protocol);
    s_last_diag_us = now;
}

void __not_in_flash_func(service_and_wait)() {
    service();
    // Ждать прерывания, а не крутить пустой цикл: данные приходят в ISR,
    // прерывание и есть сигнал.
    __wfi();
}

void set_file_info(uint8_t minutes, uint8_t seconds, uint16_t num_samples, uint16_t num_patterns, uint16_t num_instruments) {
    if (s_protocol != nullptr) {
        s_protocol->set_file_info(minutes, seconds, num_samples, num_patterns, num_instruments);
    }
}

} // namespace hostlink
