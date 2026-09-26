#pragma once

// Сторона хоста для протокола v2, общая для всех тестов и инструментов.
//
// Держит то, что на железе держат платформа и плагин: слова двух портов,
// слова кадра и индекс чтения порта данных. Кадр ведёт тот же код, что у
// прошивки (host_frame.h), слово ответа здесь - сам байт. Индекс плата
// двигает по факту чтения - это главный инвариант v2: сколько байт хост
// вычитал, плата не считает.

#include <cstdint>

#include "player/protocol/host_frame.h"
#include "player/protocol/host_protocol.h"

namespace testing_host {

struct Link {
    player::protocol::HostProtocol* p = nullptr;

    uint8_t status = player::protocol::HostProtocol::kStNone; // слово порта статуса
    uint8_t dat = 0;                                         // слово порта данных
    uint32_t words[player::protocol::host_frame::kWords] = {};
    uint8_t pos = 0;
    uint8_t args_len = 0;
    int arms = 0;

    // Приёмник кадра: слово ответа - сам байт.
    struct Sink {
        static constexpr uint8_t kCmd = 0;
        static constexpr uint8_t kDat = 1;
        Link* link;
        void set(uint8_t port, uint32_t word) {
            (port == kCmd ? link->status : link->dat) = static_cast<uint8_t>(word);
        }
        uint32_t encode(uint8_t byte) { return byte; }
        void publish_fence() {}
    };
    // Запросы окна: не с начала сектора (у WC - перемотка на каждом) и
    // медленные (длина не полное окно - законно только на хвосте файла).
    uint32_t unaligned = 0;
    uint32_t slow = 0;

    void attach(player::protocol::HostProtocol& proto) {
        p = &proto;
        proto.set_arm(&arm_cb, this, &hide_cb);
    }

    static void hide_cb(void* user) {
        Sink sink{static_cast<Link*>(user)};
        player::protocol::host_frame::hide(sink);
    }

    static void arm_cb(void* user, uint8_t code, const uint8_t* a, uint8_t n) {
        Link* h = static_cast<Link*>(user);
        Sink sink{h};
        player::protocol::host_frame::arm(sink, h->words, h->pos, code, a, n);
        h->args_len = n;
        ++h->arms;
    }

    // 0xFF обрабатывается как 0x00: шина без платы читается как 0xFF.
    uint8_t poll() const {
        return (status == player::protocol::HostProtocol::kStNoneAlt) ? player::protocol::HostProtocol::kStNone : status;
    }

    // Чтение порта данных: байт на шине, после чтения плата готовит следующий.
    uint8_t read_arg() {
        const uint8_t v = dat;
        Sink sink{this};
        player::protocol::host_frame::read_done(sink, words, pos);
        return v;
    }

    // false - контрольный байт не сошёлся. Вызывающий шлёт "не понял" и
    // возвращается в цикл: плата вооружит ту же команду заново.
    bool read_args(uint8_t* dst, uint8_t n) {
        uint8_t sum = 0;
        for (uint8_t i = 0; i < n; ++i) dst[i] = read_arg();
        for (uint8_t i = 0; i + 1 < n; ++i) sum = static_cast<uint8_t>(sum + dst[i]);
        return sum == dst[n - 1];
    }

    void command(uint8_t code, uint8_t b1 = 0, uint8_t b2 = 0, uint8_t b3 = 0, uint8_t b4 = 0, uint8_t b5 = 0,
                  uint8_t b6 = 0) {
        const uint8_t bytes[8] = {code, b1, b2, b3, b4, b5, b6, 0};
        Sink sink{this};
        for (uint8_t b : bytes) player::protocol::host_frame::command_byte(sink, p, b);
        p->poll();
    }

    void send_data(const uint8_t* data, uint16_t n) {
        for (uint16_t i = 0; i < n; ++i) p->on_data_byte(data[i]);
    }

    // Сумма окна для DONE - младшие слова каждой четвёрки байт. Плата её не
    // проверяет.
    static uint16_t data_sum(const uint8_t* data, uint16_t bytes) {
        uint16_t sum = 0;
        for (uint16_t i = 0; i < bytes / 4; ++i) {
            const uint32_t off = static_cast<uint32_t>(i) * 4u;
            sum = static_cast<uint16_t>(sum + static_cast<uint16_t>(data[off] | (data[off + 1] << 8)));
        }
        return sum;
    }

    void done(uint16_t written, uint16_t sum) {
        command(player::protocol::HostProtocol::kHcDone, static_cast<uint8_t>(written),
                 static_cast<uint8_t>(written >> 8), static_cast<uint8_t>(sum), static_cast<uint8_t>(sum >> 8));
    }

    void done_no_data() { command(player::protocol::HostProtocol::kHcDone); }
    void nak() { command(player::protocol::HostProtocol::kHcNak); }
};

// Один шаг стороны хоста: опросить статус и исполнить команду, если она
// есть. То же, что делает главный цикл плагина.
//
// Тесты отличаются только тем, откуда берутся байты файла, поэтому это
// вынесено в read_window: идеальный хост читает по любому смещению, а
// модель WC - последовательным потоком с перемоткой. Остальное общее;
// дублировать его значило бы получить три слегка разных протокола вместо
// одного.
//
// scratch - буфер вызывающего, не меньше kDataBufferBytes.
// Возвращает true, если команда была и мы её исполнили.
inline bool host_step(Link& link, void* user, void (*read_window)(void*, uint32_t, uint16_t, uint8_t*),
                       uint8_t* scratch) {
    using HP = player::protocol::HostProtocol;

    const uint8_t st = link.poll();
    if (st == HP::kStNone) return false;

    if (st == HP::kStReadFast || st == HP::kStReadSlow) {
        uint8_t a[13]; // смещение u32 + длина u16, оба ещё раз, + контрольный
        if (!link.read_args(a, sizeof(a))) {
            link.nak(); // плата вооружит ту же команду заново
            return true;
        }
        // Обе копии должны совпасть: проверка ортогональна сумме кадра и
        // ловит уехавший буфер.
        if (a[0] != a[6] || a[1] != a[7] || a[2] != a[8] || a[3] != a[9] || a[4] != a[10] || a[5] != a[11]) {
            link.nak();
            return true;
        }
        const uint32_t offset = static_cast<uint32_t>(a[0]) | (static_cast<uint32_t>(a[1]) << 8) |
                                 (static_cast<uint32_t>(a[2]) << 16) | (static_cast<uint32_t>(a[3]) << 24);
        // Быстрый путь всегда тянет по проводу полное окно, сколько бы ни
        // просили в длине.
        const uint16_t length =
            (st == HP::kStReadFast) ? HP::kDataBufferBytes : static_cast<uint16_t>(a[4] | (a[5] << 8));

        if (offset % 512u != 0) ++link.unaligned;
        if (st == HP::kStReadSlow) ++link.slow;
        read_window(user, offset, length, scratch);
        link.send_data(scratch, length);
        link.done(length, Link::data_sum(scratch, length));
        return true;
    }

    // Остальные команды данных не возят: вычитать аргументы и подтвердить.
    uint8_t junk[HP::kArgBytes];
    if (link.args_len > 0) {
        if (!link.read_args(junk, link.args_len)) {
            link.nak();
            return true;
        }
    }
    link.done_no_data();
    return true;
}

} // namespace testing_host
