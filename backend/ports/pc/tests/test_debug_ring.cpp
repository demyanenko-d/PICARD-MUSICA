// Кольца строк лога прошивки на ПК: тот же debug_ring.h, что собирает
// debug_log.cpp, приёмник - строка вместо TX FIFO UART. На плате кольца
// уже ломались (строки ядер резались и склеивались), а видно это только по
// логу.

#include "testing.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "platform/debug_ring.h"

namespace {

using debug_ring::Ring;

// Приёмник: не больше cap байт за вызов, как FIFO UART.
struct Sink {
    std::string out;
    uint32_t cap = UINT32_MAX;
    bool operator()(uint8_t b) {
        if (out.size() >= cap) return false;
        out.push_back(static_cast<char>(b));
        return true;
    }
};

void reset(Ring& r, uint32_t at) {
    r.head.store(at);
    r.tail.store(at);
    r.dropped.store(0);
}

// Всё, что есть в кольце, построчно.
uint32_t drain_all(Ring& r, uint32_t& left, Sink& k) {
    uint32_t total = 0;
    for (;;) {
        bool full = false;
        const uint32_t n = debug_ring::drain_line(r, UINT32_MAX, left, k, full);
        total += n;
        if (full || n == 0) return total;
    }
}

std::string text(uint32_t len, uint32_t seed) {
    std::string s(len, ' ');
    for (uint32_t i = 0; i < len; ++i) s[i] = static_cast<char>('a' + (seed + i) % 26);
    return s;
}

// Длина пишется двумя байтами: 255 и 256 различаются вторым; 2046 -
// наибольшая, что влезает; 2047 не влезает никогда и уходит в потери.
void test_lengths() {
    static Ring r;
    for (uint32_t len : {1u, 255u, 256u, debug_ring::kMaxLineBytes}) {
        reset(r, 0);
        const std::string s = text(len, len);
        CHECK(debug_ring::push(r, s.data(), len));
        Sink k;
        uint32_t left = 0;
        CHECK_EQ(drain_all(r, left, k), len);
        CHECK(k.out == s);
        CHECK_EQ(left, 0u);
        CHECK_EQ(r.tail.load(), r.head.load());
    }
    reset(r, 0);
    const std::string big = text(debug_ring::kMaxLineBytes + 1, 7);
    CHECK(!debug_ring::push(r, big.data(), static_cast<uint32_t>(big.size())));
    CHECK_EQ(r.dropped.load(), 1u);
    CHECK_EQ(r.head.load(), 0u);
    // Пустая строка не пишется.
    CHECK(debug_ring::push(r, "", 0));
    CHECK_EQ(r.head.load(), 0u);
    uint32_t left = 0;
    // Полное кольцо: следующая строка теряется целиком, прежние целы.
    reset(r, 0);
    const std::string half = text(1000, 3);
    CHECK(debug_ring::push(r, half.data(), 1000));
    CHECK(debug_ring::push(r, half.data(), 1000));
    CHECK(!debug_ring::push(r, half.data(), 100));
    CHECK_EQ(r.dropped.load(), 1u);
    Sink all;
    CHECK_EQ(drain_all(r, left, all), 2000u);
    CHECK(all.out == half + half);
}

// head и tail заворачиваются через 2^32 посреди потока строк.
void test_wrap() {
    static Ring r;
    reset(r, 0xFFFFFF00u);
    std::string expect, got;
    for (uint32_t i = 0; i < 100; ++i) {
        const std::string s = text(20 + i % 37, i);
        CHECK(debug_ring::push(r, s.data(), static_cast<uint32_t>(s.size())));
        expect += s;
        Sink k;
        uint32_t left = 0;
        drain_all(r, left, k);
        got += k.out;
    }
    CHECK(got == expect);
    CHECK(r.head.load() < 0x10000u);
}

// Приёмник по cap байт за вызов, не больше max за вызов: строка выходит за
// много вызовов, кольца меняются только на её границе и по очереди.
void test_pair(uint32_t cap, uint32_t max) {
    static Ring rings[2];
    reset(rings[0], 0);
    reset(rings[1], 0xFFFFFFF0u);
    std::vector<std::string> sent[2];
    for (uint32_t i = 0; i < 10; ++i) {
        for (uint32_t c = 0; c < 2; ++c) {
            const std::string s = std::string(1, c ? 'B' : 'A') + text(30 + i * 7, i + c) + "\n";
            CHECK(debug_ring::push(rings[c], s.data(), static_cast<uint32_t>(s.size())));
            sent[c].push_back(s);
        }
    }
    debug_ring::DrainState st;
    std::string out;
    for (int guard = 0; guard < 100000 && debug_ring::pending(rings, st); ++guard) {
        Sink k;
        k.cap = cap;
        debug_ring::drain_pair(rings, st, max, k);
        out += k.out;
    }
    CHECK(!debug_ring::pending(rings, st));
    size_t next[2] = {0, 0};
    char prev = 0;
    bool alternate = true;
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t end = out.find('\n', pos);
        if (end == std::string::npos) break;
        const std::string l = out.substr(pos, end - pos + 1);
        pos = end + 1;
        const uint32_t c = l[0] == 'B' ? 1 : 0;
        CHECK(next[c] < sent[c].size() && l == sent[c][next[c]]);
        ++next[c];
        if (prev == l[0] && next[0] < sent[0].size() && next[1] < sent[1].size()) alternate = false;
        prev = l[0];
    }
    CHECK_EQ(pos, out.size());
    CHECK_EQ(next[0], sent[0].size());
    CHECK_EQ(next[1], sent[1].size());
    CHECK(alternate);
}

// Писатель и потребитель в разных потоках, как ядро и log_task: получено
// плюс потеряно - столько, сколько отправлено, рваных строк нет, порядок
// номеров растёт.
void test_threads() {
    static Ring r;
    reset(r, 0xFFFF0000u);
    constexpr uint32_t kLines = 200000;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        char buf[64];
        for (uint32_t i = 0; i < kLines; ++i) {
            const int n = std::snprintf(buf, sizeof(buf), "#%u:%s\n", i, text(i % 40, i).c_str());
            debug_ring::push(r, buf, static_cast<uint32_t>(n));
        }
        done.store(true);
    });
    std::string acc;
    uint32_t left = 0;
    uint32_t got = 0;
    uint32_t torn = 0;
    int64_t last = -1;
    bool order = true;
    for (;;) {
        const bool fin = done.load();
        Sink k;
        k.cap = 32;
        drain_all(r, left, k);
        acc += k.out;
        size_t pos = 0;
        for (size_t end; (end = acc.find('\n', pos)) != std::string::npos; pos = end + 1) {
            const std::string l = acc.substr(pos, end - pos);
            unsigned n = 0;
            int used = 0;
            if (std::sscanf(l.c_str(), "#%u:%n", &n, &used) != 1 || l.substr(used) != text(n % 40, n)) {
                ++torn;
                continue;
            }
            if (static_cast<int64_t>(n) <= last) order = false;
            last = n;
            ++got;
        }
        acc.erase(0, pos);
        if (fin && k.out.empty() && left == 0 && r.head.load() == r.tail.load()) break;
    }
    writer.join();
    CHECK_EQ(got + r.dropped.load(), kLines);
    CHECK_EQ(torn, 0u);
    CHECK(order);
    CHECK(acc.empty());
}

} // namespace

void run_debug_ring_tests() {
    test_lengths();
    test_wrap();
    test_pair(3, UINT32_MAX);
    test_pair(32, 64);
    test_pair(UINT32_MAX, 64);
    test_threads();
}
