// SPDX-License-Identifier: MIT
// Приём UART1 в кольцо по прерыванию, команды AT и HTTP - в своей задаче на
// Core0. Тело ответа идёт на Core1 кольцом, там его забирает net_pump.

#include "wifi/esp_link.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#include "FreeRTOS.h"
#include "task.h"

#include "platform/log.h"
#include "platform/mono_time.h"

#include "net/hal/net.h"
#include "net/zxart/zxart.h"

#include "bus.h"
#include "wifi/esp_link_internal.h"

namespace rp2350::wifi {
namespace {

// Число в строке модуля: пробелы, знак, цифры основания 10 или 16 до первой
// чужой. Вместо strtol: тот ставит errno и тянет в SRAM структуру newlib
// (_impure_data, 516 Б .data).
long parse_num(const char* s, unsigned base) {
    while (*s == ' ') {
        ++s;
    }
    const bool neg = *s == '-';
    if (neg || *s == '+') {
        ++s;
    }
    long v = 0;
    for (;; ++s) {
        const unsigned c = static_cast<unsigned char>(*s);
        unsigned d       = c - '0';
        if (base == 16 && d > 9u) d = (c | 0x20u) - 'a' + 10u;
        if (d >= base) break;
        v = v * static_cast<long>(base) + static_cast<long>(d);
    }
    return neg ? -v : v;
}

constexpr uint32_t kTxPin    = 38;
constexpr uint32_t kRxPin    = 39;
constexpr uint32_t kBaudBoot = 115200;
// На 2000000 закачка та же: держит окно TCP модуля, а не UART.
constexpr uint32_t kBaudFast = 921600;
// Скорости, на которых модуль мог остаться с прошлой загрузки.
constexpr uint32_t kBaudProbe[] = {kBaudBoot, kBaudFast, 2000000};
// Активный приём: модуль шлёт данные сам ("+IPD,<n>:"), без запроса на
// каждый кусок; его понимает и AT ESP8266. Линий управления потоком нет:
// пока Core1 не забрал тело, данные копит кольцо тела (64 КБ в PSRAM).
// Пассивный (false): AT+CIPRECVLEN? и AT+CIPRECVDATA на каждый кусок;
// модуль не шлёт сверх запрошенного, но закачка вдвое медленнее: 640 КБ
// за 16.0 с (40 КБ/с) против 8.0 с (81 КБ/с) у активного, ESP32-C3 на
// 921600 без сна радио.
constexpr uint32_t kUartRing   = 4096; // степень двойки
constexpr uint32_t kRecvChunk  = 2048; // байт за один AT+CIPRECVDATA
constexpr uint32_t kLineMax    = 128;
constexpr uint32_t kPathMax    = 512;
constexpr uint32_t kQuietMs    = 20000;
constexpr uint32_t kBodyWaitMs = 10000; // место в кольце тела, активный приём
constexpr const char* kHost    = "zxart.ee";

// Разбор ответа по байтам: строка состояния, заголовки, тело - как есть,
// по Content-Length или кусками (chunked).
struct Http {
    enum class St : uint8_t { Status, Header, Body, ChunkSize, ChunkData, ChunkEnd, Trailer, Done };
    St st;
    char line[kLineMax];
    uint32_t len;
    int code;
    bool chunked;
    bool has_length;
    uint32_t left; // байт тела или куска
    uint32_t body; // байт тела принято
};

// Строки и разбор - в хвосте блока PSRAM за кольцом тела: модуль без
// блока не поднимается, и без Wi-Fi SRAM они не занимают. Номера и флаги
// между ядрами - в SRAM.
struct State {
    // Текст запроса целиком: собирает Core1 при приёме запроса и только при
    // свободном транспорте, Core0 шлёт его как есть. Строку пути поставщик
    // переписывает, пока Core0 ещё ведёт брошенный запрос, поэтому - копия.
    char req[kPathMax + 160];
    Http http;
    char rx[kLineMax]; // начатая строка переживает срок: следующий вызов её продолжит
    char ssid[soundsinth::config::kWifiSsidMax + 1];
    char pass[soundsinth::config::kWifiPassMax + 1];
    // Последняя строка ответа модуля, кроме OK и ERROR: причина отказа
    // ("ALREADY CONNECTED", "busy p...") для журнала.
    char last_line[40];
};
static_assert(sizeof(State) <= kLinkBlockBytes - kBodyRingBytes, "wifi state fits the block tail");
State* s_st = nullptr;

// --- Приём UART1: голову двигает прерывание, хвост - задача ---

uint8_t s_uart[kUartRing];
volatile uint32_t s_uart_head    = 0;
volatile uint32_t s_uart_tail    = 0;
volatile uint32_t s_uart_dropped = 0;
volatile uint32_t s_uart_overrun = 0; // байт потерян в FIFO UART до прерывания
volatile uint32_t s_uart_rx      = 0; // байт принято всего
uint32_t s_uart_peak             = 0; // наибольшее заполнение, видимое задаче: ISR только добавляет

void uart_rx_isr() {
    while (uart_is_readable(uart1)) {
        const uint32_t dr = uart_get_hw(uart1)->dr;
        if ((dr & UART_UARTDR_OE_BITS) != 0u) ++s_uart_overrun;
        const auto b = static_cast<uint8_t>(dr);
        ++s_uart_rx;
        const uint32_t h   = s_uart_head;
        const uint32_t nxt = (h + 1u) & (kUartRing - 1u);
        if (nxt == s_uart_tail) {
            ++s_uart_dropped;
            continue;
        }
        s_uart[h]   = b;
        s_uart_head = nxt;
    }
}

bool uart_byte(uint8_t& b) {
    const uint32_t t = s_uart_tail;
    const uint32_t h = s_uart_head;
    if (t == h) return false;
    const uint32_t fill = (h - t) & (kUartRing - 1u);
    if (fill > s_uart_peak) s_uart_peak = fill;
    b           = s_uart[t];
    s_uart_tail = (t + 1u) & (kUartRing - 1u);
    return true;
}

bool expired(uint32_t deadline) {
    return static_cast<int32_t>(platform::mono_ms() - deadline) >= 0;
}

// Байт с ожиданием до срока.
bool get_byte(uint8_t& b, uint32_t deadline) {
    while (!uart_byte(b)) {
        if (expired(deadline)) return false;
        vTaskDelay(1);
    }
    return true;
}

// --- Обмен с Core1 ---
//
// Запрос: Core1 кладёт путь и двигает req_seq. Ответ: Core0 ставит код и
// status_seq, потом кладёт тело в кольцо, потом end_ok и end_seq. Отмена:
// Core1 ставит cancel_seq, Core0 бросает соединение.

constexpr uint32_t kGetPrefix  = 4;  // "GET "
constexpr int kLogPath         = 90; // знаков пути в строке журнала, буфер журнала 192 Б
uint32_t s_req_len             = 0;
uint32_t s_path_len            = 0;
bool s_req_stream              = false; // тело по месту у поставщика: пассивный приём
bool s_passive_on              = false; // режим приёма модуля сейчас
volatile uint32_t s_req_seq    = 0;
volatile uint32_t s_cancel_seq = 0;
volatile uint32_t s_status_seq = 0;
volatile int s_status          = 0;
volatile uint32_t s_year       = 0;
volatile uint32_t s_end_seq    = 0;
volatile bool s_end_ok         = false;
volatile bool s_joined         = false;

// Кольцо тела - блок PSRAM (esp_link_attach); без него модуль не
// поднимается.
uint8_t* s_body               = nullptr;
uint32_t s_body_mask          = 0;
volatile uint32_t s_body_head = 0; // пишет Core0
volatile uint32_t s_body_tail = 0; // пишет Core1

uint32_t body_free() {
    return (s_body_tail - s_body_head - 1u) & s_body_mask;
}

void body_push(const uint8_t* p, uint32_t n) {
    uint32_t h = s_body_head;
    for (uint32_t i = 0; i < n; ++i) {
        s_body[h] = p[i];
        h         = (h + 1u) & s_body_mask;
    }
    __dmb();
    s_body_head = h;
}

// --- AT ---

bool s_closed = false; // модуль сообщил о закрытии соединения

void send(const char* text, uint32_t n) {
    uart_write_blocking(uart1, reinterpret_cast<const uint8_t*>(text), n);
}

void send(const char* text) {
    send(text, static_cast<uint32_t>(std::strlen(text)));
}

// Непрошеные строки модуля: закрытие соединения и извещения о данных.
bool unsolicited(const char* line) {
    const uint32_t n = static_cast<uint32_t>(std::strlen(line));
    if (std::strcmp(line, "CLOSED") == 0 || (n > 7u && std::strcmp(line + n - 7u, ",CLOSED") == 0)) {
        s_closed = true;
        return true;
    }
    return std::strncmp(line, "+IPD", 4) == 0 || std::strncmp(line, "WIFI ", 5) == 0;
}

// Данные +IPD идут запросу s_feed_seq; 0 - никому, байты выбрасываются.
volatile uint32_t s_feed_seq = 0;
uint32_t s_feed_ms           = 0;     // время последних данных соединения
bool s_feed_cut              = false; // +IPD оборвался посреди данных
bool s_feed_stalled          = false; // кольцо тела не освободилось за срок

void http_feed(const uint8_t* p, uint32_t n, uint32_t seq);
bool cancelled(uint32_t seq);

void ipd_feed(const uint8_t* p, uint32_t n) {
    const uint32_t seq = s_feed_seq;
    if (seq == 0u || cancelled(seq)) return;
    s_feed_ms = platform::mono_ms();
    http_feed(p, n, seq);
}

// n байт данных после "+IPD,<n>:". Они уже идут по UART, срок - свой, а
// не срок ожидающей строки, и отсчитывается заново после ожидания места в
// кольце тела.
void ipd_take(uint32_t count) {
    uint32_t deadline = platform::mono_ms() + 3000u;
    uint8_t piece[64];
    uint32_t k = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!get_byte(piece[k++], deadline)) {
            s_feed_cut = true;
            return;
        }
        if (k == sizeof(piece)) {
            ipd_feed(piece, k);
            k        = 0;
            deadline = platform::mono_ms() + 3000u;
        }
    }
    if (k != 0u) ipd_feed(piece, k);
}

uint32_t s_rx_len = 0;

void rx_flush() {
    s_uart_tail = s_uart_head;
    s_rx_len    = 0;
}

// Строка ответа без \r\n. false - срок вышел. Данные активного приёма
// ("+IPD,<n>:") разбираются по дороге; в пассивном модуль шлёт "+IPD,<n>"
// без данных - это строка.
// prompt - ждётся ещё приглашение ">": оно приходит без перевода строки, в
// начале строки; тогда true и *prompt = true.
bool read_line(char* line, uint32_t deadline, bool* prompt = nullptr) {
    for (;;) {
        uint8_t b = 0;
        if (!get_byte(b, deadline)) return false;
        if (b == '\r') continue;
        if (b == '>' && s_rx_len == 0u && prompt != nullptr) {
            *prompt = true;
            return true;
        }
        if (b == '\n') {
            if (s_rx_len == 0u) continue;
            std::memcpy(line, s_st->rx, s_rx_len);
            line[s_rx_len] = '\0';
            s_rx_len       = 0;
            return true;
        }
        if (b == ':' && s_rx_len > 5u && std::memcmp(s_st->rx, "+IPD,", 5) == 0) {
            s_st->rx[s_rx_len] = '\0';
            s_rx_len           = 0;
            ipd_take(static_cast<uint32_t>(parse_num(s_st->rx + 5, 10)));
            continue;
        }
        if (s_rx_len + 1u < kLineMax) s_st->rx[s_rx_len++] = (b >= 0x20 && b < 0x7f) ? static_cast<char>(b) : '?';
    }
}

struct Reply {
    bool ok;
    bool timed_out;
    char info[kLineMax]; // первая строка с "+": ответ команды
};

// Ответ до OK, ERROR или FAIL.

Reply wait_reply(uint32_t timeout_ms) {
    Reply r{};
    const uint32_t deadline = platform::mono_ms() + timeout_ms;
    char line[kLineMax];
    for (;;) {
        if (!read_line(line, deadline)) {
            r.timed_out = true;
            return r;
        }
        if (unsolicited(line)) continue;
        if (std::strcmp(line, "OK") != 0 && std::strcmp(line, "ERROR") != 0) {
            std::strncpy(s_st->last_line, line, sizeof(s_st->last_line) - 1u);
            s_st->last_line[sizeof(s_st->last_line) - 1u] = '\0';
        }
        if (std::strcmp(line, "OK") == 0 || std::strcmp(line, "SEND OK") == 0) {
            r.ok = true;
            return r;
        }
        if (std::strcmp(line, "ERROR") == 0 || std::strcmp(line, "FAIL") == 0 || std::strcmp(line, "SEND FAIL") == 0) return r;
        if (line[0] == '+' && r.info[0] == '\0') std::memcpy(r.info, line, sizeof(line));
    }
}

Reply at(const char* cmd, uint32_t timeout_ms) {
    send(cmd);
    send("\r\n");
    return wait_reply(timeout_ms);
}

// Версия AT модуля в журнал: от неё зависят команды приёма.
void log_version() {
    send("AT+GMR\r\n");
    const uint32_t deadline = platform::mono_ms() + 1000u;
    char line[kLineMax];
    while (read_line(line, deadline)) {
        if (std::strcmp(line, "OK") == 0 || std::strcmp(line, "ERROR") == 0) return;
        if (std::strncmp(line, "AT version", 10) == 0 || std::strncmp(line, "SDK version", 11) == 0) debug_logf("wifi: %s\n", line);
    }
}

// Строка в кавычках AT: обратная косая, кавычка и запятая экранируются.
void put_quoted(char* out, uint32_t cap, uint32_t& n, const char* s) {
    auto put = [&](char c) {
        if (n + 1u < cap) out[n++] = c;
    };
    put('"');
    for (; *s != '\0'; ++s) {
        if (*s == '\\' || *s == '"' || *s == ',') put('\\');
        put(*s);
    }
    put('"');
    out[n] = '\0';
}

void put_text(char* out, uint32_t cap, uint32_t& n, const char* s) {
    for (; *s != '\0' && n + 1u < cap; ++s) {
        out[n++] = *s;
    }
    out[n] = '\0';
}

void put_uint(char* out, uint32_t cap, uint32_t& n, uint32_t v) {
    char d[12];
    uint32_t k = 0;
    do {
        d[k++]  = static_cast<char>('0' + v % 10u);
        v      /= 10u;
    } while (v != 0u);
    while (k > 0u && n + 1u < cap) {
        out[n++] = d[--k];
    }
    out[n] = '\0';
}

// --- HTTP ---

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool starts_ci(const char* s, const char* prefix) {
    for (; *prefix != '\0'; ++s, ++prefix) {
        if (lower(*s) != *prefix) return false;
    }
    return true;
}

void header_done() {
    const char* l = s_st->http.line;
    if (starts_ci(l, "content-length:")) {
        s_st->http.has_length = true;
        s_st->http.left       = 0;
        for (const char* p = l + 15; *p != '\0'; ++p) {
            if (*p >= '0' && *p <= '9') s_st->http.left = s_st->http.left * 10u + static_cast<uint32_t>(*p - '0');
        }
    } else if (starts_ci(l, "transfer-encoding:")) {
        for (const char* p = l; *p != '\0'; ++p) {
            if (starts_ci(p, "chunked")) s_st->http.chunked = true;
        }
    } else if (starts_ci(l, "date:")) {
        // "Date: Thu, 16 Oct 2026 10:00:00 GMT" - год: первое число из 4 цифр.
        for (const char* p = l; p[0] != '\0'; ++p) {
            if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9') {
                s_year = static_cast<uint32_t>((p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0'));
                break;
            }
        }
    }
}

void publish_status(uint32_t seq) {
    s_status = s_st->http.code;
    __dmb();
    s_status_seq = seq;
}

// Место в кольце тела. В пассивном приёме оно есть всегда: кусок
// запрошен не больше свободного.
bool body_wait(uint32_t n, uint32_t seq) {
    const uint32_t deadline = platform::mono_ms() + kBodyWaitMs;
    while (body_free() < n) {
        if (cancelled(seq) || expired(deadline)) return false;
        vTaskDelay(1);
    }
    return true;
}

void http_feed(const uint8_t* p, uint32_t n, uint32_t seq) {
    Http& h = s_st->http;
    if (s_feed_stalled) return;
    for (uint32_t i = 0; i < n && h.st != Http::St::Done;) {
        const char c = static_cast<char>(p[i]);
        switch (h.st) {
            case Http::St::Body:
            case Http::St::ChunkData: {
                uint32_t take = n - i;
                if ((h.has_length || h.st == Http::St::ChunkData) && take > h.left) take = h.left;
                if (!body_wait(take, seq)) {
                    s_feed_stalled = true;
                    return;
                }
                body_push(p + i, take);
                h.body += take;
                i      += take;
                if (h.st == Http::St::ChunkData || h.has_length) h.left -= take;
                if (h.left == 0u && h.st == Http::St::ChunkData) h.st = Http::St::ChunkEnd;
                if (h.left == 0u && h.st == Http::St::Body && h.has_length) h.st = Http::St::Done;
                continue;
            }
            default:
                break;
        }
        ++i;
        if (c == '\r') continue;
        if (c != '\n') {
            if (h.len + 1u < kLineMax) h.line[h.len++] = c;
            continue;
        }
        h.line[h.len] = '\0';
        h.len         = 0;
        switch (h.st) {
            case Http::St::Status: {
                // "HTTP/1.1 200 OK"
                const char* sp = std::strchr(h.line, ' ');
                h.code         = sp != nullptr ? static_cast<int>(parse_num(sp + 1, 10)) : 0;
                h.st           = Http::St::Header;
                break;
            }
            case Http::St::Header:
                if (h.line[0] != '\0') {
                    header_done();
                    break;
                }
                publish_status(seq);
                if (h.chunked) {
                    h.st = Http::St::ChunkSize;
                } else if (h.has_length && h.left == 0u) {
                    h.st = Http::St::Done;
                } else {
                    h.st = Http::St::Body;
                }
                break;
            case Http::St::ChunkSize:
                h.left = static_cast<uint32_t>(parse_num(h.line, 16));
                h.st   = h.left == 0u ? Http::St::Trailer : Http::St::ChunkData;
                break;
            case Http::St::ChunkEnd:
                h.st = Http::St::ChunkSize;
                break;
            case Http::St::Trailer:
                if (h.line[0] == '\0') h.st = Http::St::Done;
                break;
            default:
                break;
        }
    }
}

// Данные соединения в пассивном приёме: AT+CIPRECVDATA и ответ
// "+CIPRECVDATA:<n>,<байты>". Байты сразу идут в разбор HTTP, мелкими
// кусками. Байт до max; -1 - модуль отказал.
int recv_data(uint32_t max, uint32_t seq) {
    char cmd[32];
    uint32_t n = 0;
    put_text(cmd, sizeof(cmd), n, "AT+CIPRECVDATA=");
    put_uint(cmd, sizeof(cmd), n, max);
    put_text(cmd, sizeof(cmd), n, "\r\n");
    send(cmd, n);
    const uint32_t deadline = platform::mono_ms() + 3000u;
    // ESP-AT: "+CIPRECVDATA:<n>,"; AT ESP8266 NONOS: "+CIPRECVDATA,<n>:".
    static const char kTag[]   = "+CIPRECVDATA";
    constexpr uint32_t kTagLen = sizeof(kTag) - 1u;
    char line[kLineMax];
    uint32_t len = 0;
    int got      = 0;
    for (;;) {
        uint8_t b = 0;
        if (!get_byte(b, deadline)) return -1;
        if (b == '\r') continue;
        if (b == '\n') {
            line[len] = '\0';
            len       = 0;
            if (line[0] == '\0' || unsolicited(line)) continue;
            if (std::strcmp(line, "OK") == 0) return got;
            if (std::strcmp(line, "ERROR") == 0) return -1;
            continue;
        }
        if (len + 1u < kLineMax) line[len++] = static_cast<char>(b);
        if (len == kTagLen + 1u && (line[kTagLen] == ':' || line[kTagLen] == ',') && std::memcmp(line, kTag, kTagLen) == 0) {
            uint32_t count = 0;
            for (;;) {
                if (!get_byte(b, deadline)) return -1;
                if (b == ',' || b == ':') break;
                if (b < '0' || b > '9') return -1;
                count = count * 10u + static_cast<uint32_t>(b - '0');
            }
            if (count > max) return -1;
            uint8_t piece[64];
            uint32_t k = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (!get_byte(piece[k++], deadline)) return -1;
                if (k == sizeof(piece)) {
                    http_feed(piece, k, seq);
                    k = 0;
                }
            }
            if (k != 0u) http_feed(piece, k, seq);
            got = static_cast<int>(count);
            len = 0;
        }
    }
}

// Сколько байт соединения ждёт в модуле.
int recv_len() {
    const Reply r = at("AT+CIPRECVLEN?", 1000);
    if (std::strncmp(r.info, "+CIPRECVLEN:", 12) != 0) return -1;
    // Соединение закрыто: модуль отдаёт остаток длиной и ERROR вслед. Это
    // конец ответа, а не сбой - решает, весь ли ответ пришёл, разбор HTTP.
    if (!r.ok) s_closed = true;
    const long n = parse_num(r.info + 12, 10);
    return n < 0 ? 0 : static_cast<int>(n);
}

bool cancelled(uint32_t seq) {
    return s_cancel_seq == seq;
}

// Ответ в активном приёме: данные разбирает read_line. CLOSED идёт по UART
// после всех данных соединения.
const char* receive_active(uint32_t seq) {
    char line[kLineMax];
    while (s_st->http.st != Http::St::Done) {
        if (s_feed_cut) return "ipd cut";
        if (s_feed_stalled) return "body stalled";
        if (cancelled(seq)) return "cancelled";
        if (s_closed) return s_st->http.st == Http::St::Body && !s_st->http.has_length ? nullptr : "closed early";
        if (platform::mono_ms() - s_feed_ms >= kQuietMs) return "quiet";
        if (read_line(line, platform::mono_ms() + 20u)) (void)unsolicited(line);
    }
    s_feed_seq = 0;
    at("AT+CIPCLOSE", 500);
    return nullptr;
}

// Режим приёма модуля: активный - модуль шлёт сам, быстрее вдвое;
// пассивный - отдаёт по запросу, и сеть ждёт, пока поставщику некуда класть.
// CIPRECVTYPE - ESP-AT 3.x и новее, CIPRECVMODE - ESP-AT 2.x и AT ESP8266.
// Ставится до соединения; модуль помнит его и после перезагрузки платы.
bool recv_mode(bool passive) {
    if (passive == s_passive_on) return true;
    const bool ok = at(passive ? "AT+CIPRECVTYPE=1" : "AT+CIPRECVTYPE=0", 1000).ok || at(passive ? "AT+CIPRECVMODE=1" : "AT+CIPRECVMODE=0", 1000).ok;
    if (ok) s_passive_on = passive;
    return ok;
}

// Один GET. nullptr - ответ дошёл до конца, иначе шаг, на котором обмен
// сорвался.
const char* http_get(uint32_t seq) {
    s_year             = 0; // год - только из заголовка этого ответа
    s_st->http         = Http{};
    s_st->last_line[0] = '\0';
    s_feed_seq         = 0;
    at("AT+CIPCLOSE", 500);
    if (!recv_mode(s_req_stream)) return "recv mode";
    s_closed       = false;
    s_feed_cut     = false;
    s_feed_stalled = false;
    char cmd[64];
    uint32_t n = 0;
    put_text(cmd, sizeof(cmd), n, "AT+CIPSTART=\"TCP\",\"");
    put_text(cmd, sizeof(cmd), n, kHost);
    put_text(cmd, sizeof(cmd), n, "\",80");
    if (!at(cmd, 10000).ok) return "connect";

    const uint32_t req_len = s_req_len;
    n                      = 0;
    put_text(cmd, sizeof(cmd), n, "AT+CIPSEND=");
    put_uint(cmd, sizeof(cmd), n, req_len);
    put_text(cmd, sizeof(cmd), n, "\r\n");
    send(cmd, n);
    // Приглашение ">" - в начале строки. Данные отменённого соединения
    // (+IPD) могут идти и после его CLOSED и OK и разбираются как данные:
    // ">" внутри них - не приглашение, иначе запрос ушёл бы раньше времени.
    const uint32_t deadline = platform::mono_ms() + 3000u;
    for (;;) {
        char line[kLineMax];
        bool prompt = false;
        if (!read_line(line, deadline, &prompt)) return "prompt";
        if (prompt) break;
        if (std::strcmp(line, "ERROR") == 0) return "prompt";
        (void)unsolicited(line);
    }
    s_feed_seq = seq;
    s_feed_ms  = platform::mono_ms();
    send(s_st->req, req_len);
    if (!wait_reply(5000).ok) return "send";
    if (!s_passive_on) return receive_active(seq);

    uint32_t q0 = platform::mono_ms();
    while (s_st->http.st != Http::St::Done) {
        if (cancelled(seq)) return "cancelled";
        if (platform::mono_ms() - q0 >= kQuietMs) return "quiet";
        const uint32_t space = body_free();
        if (space < 256u) {
            // Поставщик не забирает: ждём его, а не сеть.
            q0 = platform::mono_ms();
            vTaskDelay(1);
            continue;
        }
        const int waiting = recv_len();
        // Модуль закрыл соединение и на вопрос о длине ответил ERROR.
        if (waiting < 0) return s_closed ? "closed early" : "recv len";
        if (waiting == 0) {
            // Соединение закрыто и всё забрано: тело без длины кончилось.
            if (s_closed) return s_st->http.st == Http::St::Body && !s_st->http.has_length ? nullptr : "closed early";
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        uint32_t ask = static_cast<uint32_t>(waiting);
        if (ask > kRecvChunk) ask = kRecvChunk;
        if (ask > space) ask = space;
        const int got = recv_data(ask, seq);
        if (got < 0) return "recv data";
        if (got > 0) q0 = platform::mono_ms();
    }
    at("AT+CIPCLOSE", 500);
    return nullptr;
}

void park() {
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

// Поднять скорость UART: модуль переходит после ответа OK.
void speed_up(uint32_t from) {
    char cmd[48];
    uint32_t n = 0;
    put_text(cmd, sizeof(cmd), n, "AT+UART_CUR=");
    put_uint(cmd, sizeof(cmd), n, kBaudFast);
    put_text(cmd, sizeof(cmd), n, ",8,1,0,0");
    if (!at(cmd, 1000).ok) {
        debug_logf("wifi: module refused a faster UART, staying at %" PRIu32 "\n", from);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    uart_set_baudrate(uart1, kBaudFast);
    rx_flush();
    vTaskDelay(pdMS_TO_TICKS(20));
    if (at("AT", 500).ok || at("AT", 500).ok) {
        debug_logf("wifi: UART at %" PRIu32 "\n", kBaudFast);
        return;
    }
    debug_logf("wifi: no answer at the faster UART, back to %" PRIu32 "\n", from);
    uart_set_baudrate(uart1, from);
}

// Скорость, на которой модуль ответил. Модуль не сбрасывается вместе с
// платой: после прошлой загрузки он мог остаться на другой скорости.
// Сброс платы посреди запроса оставляет модуль в AT+CIPSEND: он ждёт байты
// запроса и команды принимает за них. 8192 байта - предел длины CIPSEND
// ESP-AT (у AT ESP8266 - 2048): после них модуль отвечает SEND OK или
// SEND FAIL и снова принимает команды. Заполнитель - пары CR LF: модулю в
// режиме команд это пустые строки.
void fill_send(uint32_t baud) {
    uint8_t fill[64];
    for (uint32_t i = 0; i < sizeof(fill); i += 2u) {
        fill[i]      = 13;
        fill[i + 1u] = 10;
    }
    uart_set_baudrate(uart1, baud);
    for (uint32_t sent = 0; sent < 8192u; sent += sizeof(fill)) {
        uart_write_blocking(uart1, fill, sizeof(fill));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    rx_flush();
}

SOUNDSINTH_NOINLINE uint32_t find_module() {
    Reply r{};
    uint32_t baud = kBaudBoot;
    // Байт пришло на каждой скорости первого прохода: тишина везде - провода
    // или питание, мусор - модуль жив, но скорость чужая.
    uint32_t heard[std::size(kBaudProbe)] = {};
    for (uint32_t i = 0; i < 2u * std::size(kBaudProbe) && !r.ok; ++i) {
        baud = kBaudProbe[i % std::size(kBaudProbe)];
        uart_set_baudrate(uart1, baud);
        rx_flush();
        const uint32_t rx0 = s_uart_rx;
        r                  = at("AT", 300);
        if (i < std::size(kBaudProbe)) heard[i] = s_uart_rx - rx0;
    }
    if (r.ok && baud != kBaudBoot) debug_logf("wifi: module answers at %" PRIu32 "\n", baud);
    for (uint32_t i = 0; i < std::size(kBaudProbe) && !r.ok; ++i) {
        baud = kBaudProbe[i];
        fill_send(baud);
        r = at("AT", 300);
        if (r.ok) debug_logf("wifi: module was inside CIPSEND at %" PRIu32 "\n", baud);
    }
    // Ответ не читается, но команда может дойти: вслепую вернуть модуль на
    // 115200 с каждой скорости.
    for (uint32_t i = 0; i < std::size(kBaudProbe) && !r.ok; ++i) {
        uart_set_baudrate(uart1, kBaudProbe[i]);
        at("AT+UART_CUR=115200,8,1,0,0", 100);
        uart_set_baudrate(uart1, kBaudBoot);
        vTaskDelay(pdMS_TO_TICKS(20));
        rx_flush();
        baud = kBaudBoot;
        r    = at("AT", 300);
        if (r.ok) debug_logf("wifi: module brought back from %" PRIu32 " to 115200\n", kBaudProbe[i]);
    }
    if (!r.ok) {
        debug_logf("wifi: module does not answer on UART1 (GPIO38/39), dropped %" PRIu32 ", bytes heard per speed:\n", s_uart_dropped);
        for (uint32_t i = 0; i < std::size(kBaudProbe); ++i) {
            debug_logf("wifi:   %" PRIu32 " baud: %" PRIu32 " B\n", kBaudProbe[i], heard[i]);
        }
        park();
    }
    return baud;
}

// Программный сброс на каждой загрузке: модуль не сбрасывается с платой и
// хранит соединение, режим приёма и скорость прошлой загрузки. После сброса
// загрузчик ESP пишет на 74880, прошивка AT - на скорости из своей флеш,
// затем "ready".
SOUNDSINTH_NOINLINE uint32_t restart_module(uint32_t baud) {
    if (!at("AT+RST", 1000).ok) {
        debug_log("wifi: AT+RST refused, module keeps its state\n");
        return baud;
    }
    const uint32_t t0 = platform::mono_ms();
    uart_set_baudrate(uart1, kBaudBoot);
    rx_flush();
    const uint32_t deadline = t0 + 5000u;
    char line[kLineMax];
    bool ready = false;
    while (!ready && read_line(line, deadline)) {
        ready = std::strcmp(line, "ready") == 0;
    }
    debug_logf("wifi: module restarted by AT+RST, %s in %" PRIu32 " ms\n", ready ? "ready" : "no ready line", platform::mono_ms() - t0);
    return find_module();
}

// Подключение к сети; на отказе задача паркуется.
SOUNDSINTH_NOINLINE void join_network() {
    char cmd[16 + 2u * (sizeof(s_st->ssid) + sizeof(s_st->pass)) + 8];
    uint32_t n = 0;
    put_text(cmd, sizeof(cmd), n, "AT+CWJAP=");
    put_quoted(cmd, sizeof(cmd), n, s_st->ssid);
    put_text(cmd, sizeof(cmd), n, ",");
    put_quoted(cmd, sizeof(cmd), n, s_st->pass);
    debug_logf("wifi: joining \"%s\"\n", s_st->ssid);
    const uint32_t t0   = platform::mono_ms();
    const Reply r       = at(cmd, 25000);
    const uint32_t took = platform::mono_ms() - t0;
    if (!r.ok) {
        // +CWJAP:1 - время вышло, 2 - неверный пароль, 3 - нет такой сети,
        // 4 - отказ подключения.
        debug_logf("wifi: join failed after %" PRIu32 " ms%s %s\n", took, r.timed_out ? " (no answer)" : "", r.info);
        park();
    }
    debug_logf("wifi: joined in %" PRIu32 " ms\n", took);
    const Reply a = at("AT+CIPSTA?", 2000);
    debug_logf("wifi: %s\n", a.info[0] != '\0' ? a.info : "no address");
}

void link_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(100));
    const uint32_t baud = restart_module(find_module());
    at("ATE0", 500);
    log_version();
    at("AT+SYSSTORE=0", 500);
    at("AT+CWMODE=1", 1000);
    if (s_st->ssid[0] == '\0') {
        debug_log("wifi: module answers, WIFI_SSID is empty - not connecting\n");
        park();
    }
    join_network();
    // Сон радио выключается: во сне модуль получает пакеты только по маякам
    // точки доступа (ESP32-C3 ESP-AT 4.1.1 отвечает +SLEEP:1, modem sleep).
    // Номера режимов в AT ESP8266 и ESP-AT расходятся, 0 - выключен у всех.
    const Reply sleep = at("AT+SLEEP?", 1000);
    const bool awake  = at("AT+SLEEP=0", 1000).ok;
    debug_logf("wifi: sleep mode was %s, off: %s\n", sleep.info[0] != '\0' ? sleep.info : sleep.ok ? "no value" : "command refused", awake ? "yes" : "REFUSED");
    at("AT+CIPMUX=0", 1000);
    // Режим приёма: CIPRECVTYPE - ESP-AT 3.x и новее, CIPRECVMODE - ESP-AT
    // 2.x и AT ESP8266. Модуль не сбрасывается с платой и помнит режим
    // прошлой загрузки, поэтому активный ставится явно.
    s_passive_on = true;
    if (!recv_mode(false)) debug_log("wifi: receive mode command refused, active by default\n");
    if (baud == kBaudFast) {
        debug_logf("wifi: module already at %" PRIu32 "\n", kBaudFast);
    } else {
        speed_up(baud);
    }
    s_joined = true;

    // Запросы Core1: по одному.
    uint32_t done = 0;
    for (;;) {
        const uint32_t seq = s_req_seq;
        if (seq == done) {
            vTaskDelay(2);
            continue;
        }
        __dmb();
        const uint32_t start  = platform::mono_ms();
        const char* const why = http_get(seq);
        s_feed_seq            = 0;
        const bool ok         = why == nullptr;
        if (!ok) at("AT+CIPCLOSE", 500);
        // Обрыв по нашей воле (взято начало файла) - не сбой.
        const bool cut = !ok && std::strcmp(why, "cancelled") == 0;
        // Путь длиннее kLogPath - начало и конец: строка журнала не длиннее
        // буфера, и причина с временем не теряются.
        const char* path    = s_st->req + kGetPrefix;
        const auto len      = static_cast<int>(s_path_len);
        const bool long_one = len > kLogPath;
        const int head      = long_one ? kLogPath * 2 / 3 : len;
        const int tail      = long_one ? kLogPath - head : 0;
        debug_logf("wifi: GET %.*s%s%.*s -> %d, %s%s in %" PRIu32 " ms, uart dropped %" PRIu32 ", overrun %" PRIu32 ", peak %" PRIu32 "\n", head, path,
                   long_one ? "..." : "", tail, path + len - tail, s_st->http.code, ok || cut ? "" : "FAILED ", ok ? "ok" : why, platform::mono_ms() - start,
                   s_uart_dropped, s_uart_overrun, s_uart_peak);
        if (!ok && !cut && s_st->last_line[0] != '\0') debug_logf("wifi:   module said \"%s\"\n", s_st->last_line);
        // Где встал разбор ответа: обрыв сайтом и потеря у нас различаются
        // числом недополученных байт.
        if (!ok && !cut) {
            debug_logf("wifi:   http state %u, chunked %d, length %d, left %" PRIu32 ", body %" PRIu32 ", closed %d\n", static_cast<unsigned>(s_st->http.st),
                       static_cast<int>(s_st->http.chunked), static_cast<int>(s_st->http.has_length), s_st->http.left, s_st->http.body,
                       static_cast<int>(s_closed));
        }
        s_end_ok = ok;
        __dmb();
        s_end_seq = seq;
        done      = seq;
    }
}

// --- Сторона Core1 ---

uint32_t s_cur              = 0;
bool s_busy                 = false;
bool s_cancelled            = false;
bool s_status_given         = false;
constexpr uint32_t kPumpMax = 2048; // байт тела за вызов

} // namespace

void esp_link_attach(uint8_t* block) {
    if (block == nullptr) return;
    s_body      = block;
    s_body_mask = kBodyRingBytes - 1u;
    s_st        = new (block + kBodyRingBytes) State{};
}

namespace {

TaskHandle_t s_task = nullptr;

// UART и задача; задача одна на оба режима, стек у неё общий.
void start(TaskFunction_t task) {
    if (s_body == nullptr) {
        debug_log("wifi: no PSRAM for the receive ring, the module is not started\n");
        return;
    }
    uart_init(uart1, kBaudBoot);
    gpio_set_function(kTxPin, GPIO_FUNC_UART_AUX);
    gpio_set_function(kRxPin, GPIO_FUNC_UART_AUX);
    gpio_pull_up(kRxPin);
    irq_set_exclusive_handler(UART1_IRQ, &uart_rx_isr);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(uart1, true, false);

    static StackType_t s_stack[768];
    static StaticTask_t s_tcb;
    s_task = xTaskCreateStatic(task, "wifi", std::size(s_stack), nullptr, tskIDLE_PRIORITY + 1, s_stack, &s_tcb);
}

} // namespace

uint32_t esp_link_stack_free() {
    return s_task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task)) * sizeof(StackType_t) : 0u;
}

void esp_link_start(const soundsinth::config::Settings& s) {
    // Без PSRAM состояния нет: start() скажет об этом строкой и не запустит.
    if (s_st != nullptr) {
        std::memcpy(s_st->ssid, s.wifi_ssid, sizeof(s_st->ssid));
        std::memcpy(s_st->pass, s.wifi_pass, sizeof(s_st->pass));
    }
    start(&link_task);
}

void esp_link_start_tools() {
    start(&esp_tools_task);
}

bool link_uart_poll(uint8_t& b) {
    return uart_byte(b);
}

void link_uart_put(const uint8_t* p, uint32_t n) {
    uart_write_blocking(uart1, p, n);
}

void link_uart_drop() {
    rx_flush();
}

void link_uart_baud(uint32_t baud) {
    uart_set_baudrate(uart1, baud);
    rx_flush();
}

uint32_t link_at_probe() {
    for (const uint32_t baud : kBaudProbe) {
        uart_set_baudrate(uart1, baud);
        rx_flush();
        if (at("AT", 300).ok) return baud;
    }
    uart_set_baudrate(uart1, kBaudBoot);
    rx_flush();
    return 0;
}

uint8_t* link_ring(uint32_t& bytes) {
    bytes = s_body_mask + 1u;
    return s_body;
}

} // namespace rp2350::wifi

namespace net::hal {

using namespace rp2350::wifi;

bool net_ready() {
    return s_joined;
}

bool net_http_get(const char* path, bool stream) {
    if (!s_joined || s_busy) return false;
    const uint32_t len = static_cast<uint32_t>(std::strlen(path));
    if (len >= kPathMax) return false;
    uint32_t n = 0;
    put_text(s_st->req, sizeof(s_st->req), n, "GET ");
    put_text(s_st->req, sizeof(s_st->req), n, path);
    put_text(s_st->req, sizeof(s_st->req), n, " HTTP/1.1\r\nHost: ");
    put_text(s_st->req, sizeof(s_st->req), n, kHost);
    put_text(s_st->req, sizeof(s_st->req), n, "\r\nUser-Agent: SoundSinth/" SOUNDSINTH_VERSION_STR "\r\nConnection: close\r\n\r\n");
    s_req_len      = n;
    s_path_len     = len;
    s_req_stream   = stream;
    s_busy         = true;
    s_cancelled    = false;
    s_status_given = false;
    s_cur          = s_req_seq + 1u;
    __dmb();
    s_req_seq = s_cur;
    return true;
}

void net_http_cancel() {
    if (!s_busy) return;
    s_cancelled  = true;
    s_cancel_seq = s_cur;
}

void net_pump() {
    if (!s_busy) return;
    const uint32_t end = s_end_seq;
    __dmb();
    if (!s_status_given && s_status_seq == s_cur) {
        // Код и год - после номера: Core0 пишет их до барьера и номера.
        __dmb();
        s_status_given = true;
        if (!s_cancelled) {
            if (s_year != 0u) net::zxart::zxart_set_year(s_year);
            net::zxart::zxart_http_status(s_status);
        }
    }
    // Тело - после кода: Core0 кладёт его в кольцо позже.
    if (s_status_given) {
        // Не больше, чем поставщику есть куда положить: остальное ждёт в
        // кольце, а пассивный приём придерживает сеть.
        uint32_t room  = s_cancelled ? kPumpMax : net::zxart::zxart_http_room();
        uint32_t taken = 0;
        while (taken < kPumpMax && room != 0u) {
            const uint32_t head = s_body_head;
            __dmb();
            const uint32_t tail = s_body_tail;
            if (tail == head) break;
            const uint32_t run = head > tail ? head - tail : s_body_mask + 1u - tail;
            uint32_t n         = run < kPumpMax - taken ? run : kPumpMax - taken;
            if (n > room) n = room;
            room -= n;
            if (!s_cancelled) net::zxart::zxart_http_body(s_body + tail, n);
            __dmb();
            s_body_tail  = (tail + n) & s_body_mask;
            taken       += n;
        }
    }
    if (end != s_cur || s_body_tail != s_body_head) return;
    s_busy = false;
    if (!s_cancelled) net::zxart::zxart_http_end(s_end_ok);
}

} // namespace net::hal
