#include "testing.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "player/protocol/host_frame.h"
#include "player/protocol/host_protocol.h"

using namespace player::protocol;

namespace {

// --- Модель хоста ---
//
// Держит то же, что держала бы платформа: слова двух портов и кадр. Кадр
// ведёт тот же код, что у прошивки (host_frame.h), слово ответа - сам байт.
struct Host {
    HostProtocol* p;
    uint8_t status = HostProtocol::kStNone; // слово порта статуса
    uint8_t dat = 0;                        // слово порта данных
    uint32_t words[host_frame::kWords] = {};
    uint8_t pos = 0;
    int arms = 0;

    explicit Host(HostProtocol& proto) : p(&proto) {
        p->set_arm(&arm_cb, this, &hide_cb);
    }

    // Байты команды хоста, которые придут "во время вооружения": ISR
    // принимает их между записью аргументов и статуса.
    const uint8_t* inject = nullptr;
    uint8_t inject_n = 0;
    std::vector<uint8_t> executed; // коды, которые хост видел и исполнил бы

    // Приёмник кадра. Барьер публикации - то место, где на железе байт хоста
    // может прийти между аргументами и статусом.
    struct Sink {
        static constexpr uint8_t kCmd = 0;
        static constexpr uint8_t kDat = 1;
        Host* host;
        void set(uint8_t port, uint32_t word) {
            (port == kCmd ? host->status : host->dat) = static_cast<uint8_t>(word);
        }
        uint32_t encode(uint8_t byte) { return byte; }
        void publish_fence() {
            const uint8_t* bytes = host->inject;
            const uint8_t count = host->inject_n;
            host->inject = nullptr;
            host->inject_n = 0;
            for (uint8_t i = 0; i < count; ++i) host_frame::command_byte(*this, host->p, bytes[i]);
        }
    };

    static void arm_cb(void* user, uint8_t code, const uint8_t* a, uint8_t n) {
        Host* h = static_cast<Host*>(user);
        Sink sink{h};
        host_frame::arm(sink, h->words, h->pos, code, a, n);
        ++h->arms;
    }

    static void hide_cb(void* user) {
        Sink sink{static_cast<Host*>(user)};
        host_frame::hide(sink);
    }

    // Хост опрашивает статус. 0xFF трактуется как 0 (шина без платы).
    uint8_t poll_status() const {
        return (status == HostProtocol::kStNoneAlt) ? HostProtocol::kStNone : status;
    }

    // Чтение аргументов: байт на шине, после чтения плата готовит следующий.
    uint8_t read_arg() {
        const uint8_t v = dat;
        Sink sink{this};
        host_frame::read_done(sink, words, pos);
        return v;
    }

    bool read_args(uint8_t* dst, uint8_t n) {
        uint8_t sum = 0;
        for (uint8_t i = 0; i < n; ++i) dst[i] = read_arg();
        for (uint8_t i = 0; i + 1 < n; ++i) sum = static_cast<uint8_t>(sum + dst[i]);
        return sum == dst[n - 1];
    }

    void command(uint8_t code, uint8_t b1 = 0, uint8_t b2 = 0, uint8_t b3 = 0, uint8_t b4 = 0, uint8_t b5 = 0,
                  uint8_t b6 = 0, uint8_t b7 = 0) {
        const uint8_t bytes[8] = {code, b1, b2, b3, b4, b5, b6, b7};
        Sink sink{this};
        for (uint8_t b : bytes) host_frame::command_byte(sink, p, b);
        p->poll();
    }

    // Команда без опроса и не целиком - чтобы поймать плату в середине
    // записи, как бывает на шине: хост пишет восемь байт не мгновенно, а
    // Core1 всё это время крутит poll().
    void command_bytes(uint8_t code, uint8_t b1, uint8_t count) {
        const uint8_t bytes[8] = {code, b1, 0, 0, 0, 0, 0, 0};
        Sink sink{this};
        for (uint8_t i = 0; i < count; ++i) host_frame::command_byte(sink, p, bytes[i]);
    }

    void send_data(const uint8_t* data, uint16_t n) {
        for (uint16_t i = 0; i < n; ++i) p->on_data_byte(data[i]);
    }

    void done(uint16_t written) {
        command(HostProtocol::kHcDone, static_cast<uint8_t>(written), static_cast<uint8_t>(written >> 8));
    }
};

// Доставка окна ожидающему: take_received после шага, как цикл источника.
struct Received {
    int calls = 0;
    uint16_t length = 0xFFFF;
    std::vector<uint8_t> bytes;
    void collect(HostProtocol& p) {
        uint16_t len = 0;
        while (p.take_received(len)) {
            ++calls;
            length = len;
            bytes.assign(p.data_buffer(), p.data_buffer() + len);
        }
    }
};

// --- Тесты ---

void test_reset_gives_board_name() {
    std::printf("test_reset_gives_board_name\n");

    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcReset);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStResetDone));

    uint8_t name[17];
    CHECK(h.read_args(name, 17)); // 16 байт имени + контрольный байт
    CHECK_EQ(name[0], static_cast<uint8_t>('P'));
    CHECK_EQ(name[9], static_cast<uint8_t>(' '));

    // Подтверждение освобождает плату: команды больше нет.
    h.command(HostProtocol::kHcDone);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
}

// Плата закрывает сессию, пока имя платы ещё на шине: NAK хоста по битой
// сумме имени обязан вернуть имя, а не пропасть.
void test_end_session_keeps_board_name_nak() {
    std::printf("test_end_session_keeps_board_name_nak\n");
    HostProtocol p;
    Host h(p);
    h.command(HostProtocol::kHcReset);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStResetDone));
    p.end_session();
    h.command(HostProtocol::kHcNak);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStResetDone));
}

// Плата закрывает сессию посреди восьми байт команды хоста: кадр не
// сдвигается, следующий RESET узнаётся.
void test_end_session_mid_command_keeps_frame() {
    std::printf("test_end_session_mid_command_keeps_frame\n");
    HostProtocol p;
    Host h(p);
    h.command_bytes(HostProtocol::kHcDone, 0, 3);
    p.end_session();
    for (uint8_t i = 3; i < 8; ++i) p.on_command_byte(0);
    p.poll();
    h.command(HostProtocol::kHcReset);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStResetDone));
    CHECK_EQ(p.unknown_commands(), 0u);
}

// Хост подтвердил полное окно, а ISR насчитал на байт меньше: окно
// просится снова (kMaxNak раз подряд), потом короткое чтение - не вечный
// цикл и не сдвинутые данные.
void test_lost_data_byte_rerequests_window() {
    std::printf("test_lost_data_byte_rerequests_window\n");
    Received got;
    HostProtocol p;
    Host h(p);
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, /*sector=512*/ 2, /*flags*/ 0);
    p.request_file_chunk(8192, 4096);
    std::vector<uint8_t> window(4096, 0x55);
    int requests = 0;
    for (int round = 0; round < 10 && (got.collect(p), got.calls == 0); ++round) {
        p.poll();
        if (h.poll_status() != HostProtocol::kStReadFast) break;
        ++requests;
        uint8_t a[13];
        CHECK(h.read_args(a, 13));
        h.send_data(window.data(), 4095); // байт потерян по дороге
        h.done(4096);
    }
    CHECK_EQ(requests, static_cast<int>(HostProtocol::kMaxNak));
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, 0u);
    CHECK_EQ(p.data_short(), static_cast<uint32_t>(HostProtocol::kMaxNak));
}

// Три кадра команд подряд без poll(): первые два разобраны целыми, третий
// отброшен и посчитан, а не лёг поверх неразобранного.
void test_command_ring_keeps_two_frames() {
    std::printf("test_command_ring_keeps_two_frames\n");
    HostProtocol p;
    Host h(p);
    h.command(HostProtocol::kHcReset);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStResetDone));
    auto frame = [&](uint8_t code, uint8_t b1) {
        const uint8_t bytes[8] = {code, b1, 0, 0, 0, 0, 0, 0};
        for (uint8_t b : bytes) p.on_command_byte(b);
    };
    h.status = HostProtocol::kStNone;
    frame(HostProtocol::kHcDone, 0);  // имя платы принято
    frame(HostProtocol::kHcTrace, 7); // маркер
    frame(HostProtocol::kHcReset, 0); // третий - в полное кольцо
    p.poll();
    CHECK_EQ(p.cmd_overruns(), 1u);
    CHECK_EQ(p.unknown_commands(), 0u);
    // DONE закрыл имя, маркер ничего не вооружил, RESET отброшен.
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    // Кольцо свободно: следующий RESET узнаётся.
    h.command(HostProtocol::kHcReset);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStResetDone));
}

void test_file_request_round_trip() {
    std::printf("test_file_request_round_trip\n");

    Received got;

    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, /*sector=512*/ 2, /*flags*/ 0);
    CHECK_EQ(p.sector_size_bytes(), 512u);

    p.request_file_chunk(8192, 4096);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));

    uint8_t a[13];
    CHECK(h.read_args(a, 13));
    const uint32_t off = static_cast<uint32_t>(a[0]) | (static_cast<uint32_t>(a[1]) << 8) |
                          (static_cast<uint32_t>(a[2]) << 16) | (static_cast<uint32_t>(a[3]) << 24);
    CHECK_EQ(off, 8192u);
    CHECK_EQ(static_cast<uint16_t>(a[4] | (a[5] << 8)), static_cast<uint16_t>(4096));

    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 3);
    h.send_data(data.data(), 4096);
    h.done(4096);

    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, static_cast<uint16_t>(4096));
    CHECK(got.bytes == data);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
}

// Нельзя вооружать поверх неподтверждённой команды.
//
// Отказ, ради которого написан тест (железо, 2026-08-24): плата
// выставила телеметрию, тут же выставила поверх неё чтение - и
// подтверждение, которое хост слал про телеметрию, досталось чтению. В
// подтверждении телеметрии длина нулевая, поэтому наверх ушло пустое
// окно, и первый же загрузчик отвалился на коротком чтении.
//
// Щель была в том, что вооружение разрешалось по флагу, который снимался
// на первом байте команды хоста - задолго до того, как команда разобрана.
void test_no_arm_over_unacked_command() {
    std::printf("test_no_arm_over_unacked_command\n");

    Received got;

    HostProtocol p;
    Host h(p);

    // Сессия с включённой телеметрией позиции - она и выставится первой.
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryPosition);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));

    // Хост вычитывает телеметрию и начинает слать подтверждение, но пока
    // написал только первый байт. Здесь и была щель: этот байт уже снял
    // статус, а команда разберётся только после восьмого.
    uint8_t pos[4];
    CHECK(h.read_args(pos, 4));
    h.command_bytes(HostProtocol::kHcDone, 0, 1);

    // В этот момент загрузчик просит окно, и плата опрашивается.
    p.request_file_chunk(0, 4096);
    p.poll();

    // Главное: чтение не должно влезть в середину чужого подтверждения.
    got.collect(p);
    CHECK_EQ(got.calls, 0);

    // Хост дописывает подтверждение.
    for (uint8_t i = 1; i < 8; ++i) p.on_command_byte(0);
    p.poll();

    // Пустого окна наверх не ушло, и только теперь выставлено чтение.
    got.collect(p);
    CHECK_EQ(got.calls, 0);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));

    // И оно доводится до конца обычным путём.
    uint8_t a[13];
    CHECK(h.read_args(a, 13));
    std::vector<uint8_t> data(4096, 0x5A);
    h.send_data(data.data(), 4096);
    h.done(4096);
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, static_cast<uint16_t>(4096));
}

// Оба параметра приезжают дважды, и копии должны совпадать. Проверка
// ортогональна контрольному байту: сумма говорит, цел ли кадр, а две
// копии в разных местах буфера - как именно он поломался. Это и
// раскрыло отказ v1: копии разошлись не значениями, а позициями.
void test_read_frame_echoes_offset() {
    std::printf("test_read_frame_echoes_offset\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(0x003B8200u, 4096);
    p.poll();

    uint8_t a[13];
    CHECK(h.read_args(a, 13));

    CHECK_EQ(a[6], a[0]);
    CHECK_EQ(a[7], a[1]);
    CHECK_EQ(a[8], a[2]);
    CHECK_EQ(a[9], a[3]);
    CHECK_EQ(a[10], a[4]); // длина тоже дублируется: она решает,
    CHECK_EQ(a[11], a[5]); // сколько байт хост нам пришлёт

    // И копии - это запрошенные значения, а не что попало, совпавшее само с
    // собой.
    const uint32_t echo = static_cast<uint32_t>(a[6]) | (static_cast<uint32_t>(a[7]) << 8) |
                           (static_cast<uint32_t>(a[8]) << 16) | (static_cast<uint32_t>(a[9]) << 24);
    CHECK_EQ(echo, 0x003B8200u);
    CHECK_EQ(static_cast<uint16_t>(a[10] | (a[11] << 8)), static_cast<uint16_t>(4096));
}

// Кадр аргументов можно перечитать - новая возможность v2. В v1 это
// было невозможно: индекс уходил вперёд, второго шанса не было, и один
// испорченный байт заголовка превращался в дыру в сэмпле.
void test_nak_rearms_identical_frame() {
    std::printf("test_nak_rearms_identical_frame\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(4096, 4096);
    p.poll();

    uint8_t first[13];
    CHECK(h.read_args(first, 13));

    // Хост "не разобрал" кадр и просит заново - дважды подряд.
    for (int attempt = 0; attempt < 2; ++attempt) {
        h.command(HostProtocol::kHcNak);
        CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));

        uint8_t again[13];
        CHECK(h.read_args(again, 13));
        CHECK_EQ(std::memcmp(first, again, sizeof(first)), 0); // байт в байт
    }
    CHECK_EQ(p.nak_total(), 2u);
    CHECK_EQ(p.nak_giveups(), 0u);
}

// Кольцо событий журнала: без разбора 40 событий - 32 прочитано, 8
// потеряно и посчитано; через оборот 8-битных индексов порядок сохраняется.
void test_debug_event_ring_counts_losses() {
    std::printf("test_debug_event_ring_counts_losses\n");
    HostProtocol p;
    HostProtocol::DebugEvent e;
    while (p.try_pop_debug_event(e)) {
    }
    for (int i = 0; i < 40; ++i) p.reset(); // каждый reset - событие Reset
    uint32_t read = 0;
    while (p.try_pop_debug_event(e)) ++read;
    CHECK_EQ(read, 32u);
    CHECK_EQ(p.debug_events_lost(), 8u);
    for (int i = 0; i < 300; ++i) {
        p.reset();
        CHECK(p.try_pop_debug_event(e));
        CHECK(e.kind == HostProtocol::DebugEventKind::Reset);
        CHECK(!p.try_pop_debug_event(e));
    }
    CHECK_EQ(p.debug_events_lost(), 8u);
}

// Сбои за сессию - разность против снимка на kHcStart: NAK первой сессии
// во второй не видны.
void test_session_counters_reset_on_start() {
    std::printf("test_session_counters_reset_on_start\n");
    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(4096, 4096);
    p.poll();
    h.command(HostProtocol::kHcNak);
    h.command(HostProtocol::kHcNak);
    CHECK_EQ(p.session_counters().naks, 2u);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    CHECK_EQ(p.session_counters().naks, 0u);
    CHECK_EQ(p.counters().naks, 2u); // накопительный счётчик цел
}

void test_nak_gives_up_after_limit() {
    std::printf("test_nak_gives_up_after_limit\n");

    Received got;
    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(4096, 4096);
    p.poll();

    for (uint8_t i = 0; i < HostProtocol::kMaxNak; ++i) h.command(HostProtocol::kHcNak);

    // Сдались: команда снята, наверх ушло короткое чтение - загрузка
    // провалится явно, а не повиснет в ожидании данных, которых не будет.
    CHECK_EQ(p.nak_giveups(), 1u);
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, 0u);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
}

// Кадр позиции с Ended сдан после трёх NAK - он предлагается снова: по нему
// одному хост уходит на следующий файл, а оркестратор шлёт Ended один раз.
// Позиция Playing после сдачи не повторяется - следующая секунда придёт сама.
void test_ended_position_survives_giveup() {
    std::printf("test_ended_position_survives_giveup\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    // Старт сессии сам вооружает позицию Loading 00:00 - подтвердить её.
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryPosition);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    h.command(HostProtocol::kHcDone);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));

    p.set_position(3, 12, PlaybackState::Playing);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    for (uint8_t i = 0; i < HostProtocol::kMaxNak; ++i) h.command(HostProtocol::kHcNak);
    CHECK_EQ(p.nak_giveups(), 1u);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));

    p.set_position(3, 13, PlaybackState::Ended);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    for (uint8_t i = 0; i < HostProtocol::kMaxNak; ++i) h.command(HostProtocol::kHcNak);
    CHECK_EQ(p.nak_giveups(), 2u);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    uint8_t a[4];
    CHECK(h.read_args(a, 4));
    CHECK_EQ(a[2], static_cast<uint8_t>(PlaybackState::Ended));
    // Снимок состояния: кадр вооружён и ждёт подтверждения.
    HostProtocol::StateSnapshot ps = p.state_snapshot();
    CHECK_EQ(ps.armed_code, static_cast<uint8_t>(HostProtocol::kStPosition));
    CHECK(ps.awaiting_ack);
    CHECK(!ps.awaiting_data);
    h.command(HostProtocol::kHcDone);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    CHECK(!p.state_snapshot().awaiting_ack);
    // Журнал: доставка Ended - одно событие, с минутами и секундами кадра.
    int delivered = 0;
    HostProtocol::DebugEvent e;
    while (p.try_pop_debug_event(e)) {
        if (e.kind != HostProtocol::DebugEventKind::EndedDelivered) continue;
        ++delivered;
        CHECK_EQ(e.arg0, static_cast<uint8_t>(0x03));
        CHECK_EQ(e.arg1, static_cast<uint16_t>(0x13));
    }
    CHECK_EQ(delivered, 1);
}

void test_short_tail_uses_slow_path() {
    std::printf("test_short_tail_uses_slow_path\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    // Файл 5000 байт: с 4096 остаётся 904 - полных 4096 по проводу тянуть
    // нельзя, иначе хост читает за концом и уводит свою позицию потока.
    h.command(HostProtocol::kHcStart, 0x88, 0x13, 0x00, 0x00, 2, 0);
    p.request_file_chunk(4096, 904);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadSlow));
}

// Быстрый путь: смещение кратно сектору, длина не меньше 3072 и все 4096
// байт есть в файле; иначе медленный. Код ответа решает, сколько байт хост
// льёт по проводу (4096 или длину), ошибка сбивает его позицию в потоке.
void test_fast_path_table() {
    std::printf("test_fast_path_table\n");
    struct Case {
        uint32_t file;
        uint8_t sector; // 0xff - сессия не начата
        uint32_t off;
        uint16_t len;
        bool fast;
    };
    const Case cases[] = {
        {8192, 2, 4096, 4096, true},     // окно ровно до конца файла
        {8191, 2, 4096, 4096, false},    // байта не хватает
        {100000, 2, 4097, 4096, false},  // не с начала сектора
        {1000, 2, 4096, 4096, false},    // смещение за концом, без заворота разности
        {100000, 2, 4096, 3071, false},  // короче порога
        {100000, 2, 4096, 3072, true},   // ровно порог
        {100000, 0, 128, 4096, true},    // сектор 128
        {100000, 1, 128, 4096, false},   // сектор 256
        {100000, 1, 256, 4096, true},
        {100000, 2, 256, 4096, false},   // сектор 512
        {100000, 3, 512, 4096, false},   // сектор 1024
        {100000, 3, 1024, 4096, true},
        {100000, 0xff, 4096, 4096, false}, // до старта сессии сектора нет
    };
    for (const Case& c : cases) {
        HostProtocol p;
        HostProtocol::Callbacks cb;
        p.set_callbacks(cb);
        Host h(p);
        if (c.sector != 0xff) {
            h.command(HostProtocol::kHcStart, static_cast<uint8_t>(c.file), static_cast<uint8_t>(c.file >> 8),
                      static_cast<uint8_t>(c.file >> 16), static_cast<uint8_t>(c.file >> 24), c.sector, 0);
        }
        p.request_file_chunk(c.off, c.len);
        p.poll();
        const uint8_t want = c.fast ? HostProtocol::kStReadFast : HostProtocol::kStReadSlow;
        if (h.poll_status() != want) {
            std::printf("  файл %u сектор %u смещение %u длина %u: ждали %s\n", c.file, c.sector, c.off, c.len,
                        c.fast ? "быстрый" : "медленный");
        }
        CHECK_EQ(h.poll_status(), want);
    }
}

// Разбор старта сессии: байт 7 (порядок загрузки) доходит до колбэка;
// размер сектора вне 0..3 - сессия не начинается. Старт поверх
// неподтверждённой команды без сброса закреплён как есть: вооружённое
// остаётся неподтверждённым, нового плата не вооружает (хосты шлют сброс
// перед стартом; менять - решение автора).
void test_session_start_fields() {
    std::printf("test_session_start_fields\n");
    struct Got {
        int calls = 0;
        uint8_t load_order = 0xff;
    };
    auto on_start = [](void* user, uint32_t, uint32_t, uint8_t, uint8_t load_order) {
        Got* g = static_cast<Got*>(user);
        ++g->calls;
        g->load_order = load_order;
    };
    {
        HostProtocol p;
        Got g;
        HostProtocol::Callbacks cb;
        cb.user = &g;
        cb.on_session_start = on_start;
        p.set_callbacks(cb);
        Host h(p);
        h.command(HostProtocol::kHcStart, 0x00, 0x10, 0x00, 0x00, 2, 0, 1);
        CHECK_EQ(g.calls, 1);
        CHECK_EQ(g.load_order, 1u);
        h.command(HostProtocol::kHcStart, 0x00, 0x10, 0x00, 0x00, 4, 0, 0);
        CHECK_EQ(g.calls, 1); // сектор 4 - колбэка нет, но есть событие отказа
        HostProtocol::DebugEvent e{};
        bool rejected = false;
        uint16_t start_arg1 = 0;
        while (p.try_pop_debug_event(e)) {
            if (e.kind == HostProtocol::DebugEventKind::SessionRejected && e.arg0 == 4) rejected = true;
            if (e.kind == HostProtocol::DebugEventKind::SessionStart) start_arg1 = e.arg1;
        }
        CHECK(rejected);
        // В событии старта - сектор (код 2 - 512) и порядок загрузки в старших битах.
        CHECK_EQ(start_arg1, static_cast<uint16_t>(512u | (1u << 12)));
    }
    {
        HostProtocol p;
        Got g;
        HostProtocol::Callbacks cb;
        cb.user = &g;
        cb.on_session_start = on_start;
        p.set_callbacks(cb);
        Host h(p);
        h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
        p.request_file_chunk(0, 4096);
        p.poll();
        CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
        h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0); // без сброса и DONE
        CHECK_EQ(g.calls, 2);
        p.request_file_chunk(4096, 4096);
        p.poll();
        CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    }
}

void test_telemetry_is_just_another_command() {
    std::printf("test_telemetry_is_just_another_command\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryVu);
    p.set_vu(11, 22);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStVu));

    uint8_t a[3];
    CHECK(h.read_args(a, 3));
    CHECK_EQ(a[0], static_cast<uint8_t>(11));
    CHECK_EQ(a[1], static_cast<uint8_t>(22));

    h.command(HostProtocol::kHcDone);
    p.poll();
    // Значение не менялось - повторять его незачем.
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
}

// Круговой обход видов телеметрии: все шесть изменились - уходят подряд,
// 0x15..0x1A; дальше - от вида за последним отправленным, с заворотом.
void test_telemetry_round_robin_order() {
    std::printf("test_telemetry_round_robin_order\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    // Вооружённое - в статусе; подтверждение (с poll внутри) вооружает следующее.
    auto expect_then_ack = [&](uint8_t code) {
        CHECK_EQ(h.poll_status(), code);
        h.command(HostProtocol::kHcDone);
    };

    // Значения - до старта сессии: старт сам зовёт poll() и вооружает первое.
    const uint8_t bands[16] = {1, 2, 3};
    p.set_file_info(1, 2, 3, 4, 5);
    p.set_vu(1, 2);
    p.set_spectrum(bands);
    p.set_psram_stats(1, 2, 3);
    p.set_engine_load(1, 2, 3, 4);
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0x3f); // позицию взводит сам старт
    for (uint8_t code = HostProtocol::kStFileInfo; code <= HostProtocol::kStEngineLoad; ++code) expect_then_ack(code);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));

    // Курсор после EngineLoad - на FileInfo.
    p.set_vu(3, 4);
    p.set_file_info(1, 2, 3, 4, 5);
    p.poll();
    expect_then_ack(HostProtocol::kStFileInfo);
    expect_then_ack(HostProtocol::kStVu);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    // Курсор после Vu - на Spectrum: EngineLoad раньше Position.
    p.set_position(1, 2, PlaybackState::Playing);
    p.set_engine_load(5, 6, 7, 8);
    p.poll();
    expect_then_ack(HostProtocol::kStEngineLoad);
    expect_then_ack(HostProtocol::kStPosition);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
}

// Файловый запрос обгоняет телеметрию: данные важнее рассказов о них.
void test_file_request_beats_telemetry() {
    std::printf("test_file_request_beats_telemetry\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryVu);
    p.set_vu(1, 2);
    p.request_file_chunk(0, 4096);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
}

// Маркер плагина - чистая диагностика: он не должен снимать команду,
// которую хост ещё не исполнил.
void test_trace_does_not_disturb_armed_command() {
    std::printf("test_trace_does_not_disturb_armed_command\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(2048, 4096);
    p.poll();
    const uint8_t before = h.poll_status();
    CHECK_EQ(before, static_cast<uint8_t>(HostProtocol::kStReadFast));

    h.command(HostProtocol::kHcTrace, 7, 1, 0, 2, 0);
    CHECK_EQ(h.poll_status(), before);

    uint8_t a[13];
    CHECK(h.read_args(a, 13)); // и кадр по-прежнему читается целиком
}

// Последний байт кадра хоста приходит, пока poll вооружает команду: ISR
// снимает статус раньше, чем платформа выставит новый. Без сверки хост видел
// новую команду до разбора своего кадра; маркер потом выставлял её снова -
// хост исполнял её дважды, а второе подтверждение доставалось следующей
// команде (окно с длиной 0).
void test_host_byte_during_arming_hides_command() {
    std::printf("test_host_byte_during_arming_hides_command\n");

    HostProtocol p;
    Host h(p);
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryPosition);
    uint8_t pos[4];
    CHECK(h.read_args(pos, 4));
    h.command(HostProtocol::kHcDone);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));

    // Маркер хоста: семь байт до poll, восьмой - внутри вооружения позиции.
    p.set_position(1, 2, PlaybackState::Playing);
    h.command_bytes(HostProtocol::kHcTrace, 7, 7);
    static const uint8_t kLast = 0;
    h.inject = &kLast;
    h.inject_n = 1;
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone)); // спрятана до разбора маркера

    p.poll(); // маркер разобран - позиция снова на шине
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    CHECK(h.read_args(pos, 4));
    CHECK_EQ(pos[1], static_cast<uint8_t>(0x02));
    h.command(HostProtocol::kHcDone);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone)); // исполнена один раз

    // Окно после этого доходит целиком, без короткого чтения.
    Received got;
    p.request_file_chunk(0, 4096);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
    uint8_t a[13];
    CHECK(h.read_args(a, 13));
    std::vector<uint8_t> data(4096, 0x33);
    h.send_data(data.data(), 4096);
    h.done(4096);
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, static_cast<uint16_t>(4096));
}

// То же, но поверх вооружения приходит START: он команду не возвращает, её
// выставляет сам poll после разбора кадра. Иначе спрятанная команда висела
// бы неподтверждённой, и poll больше ничего не вооружал.
void test_start_during_arming_returns_hidden_command() {
    std::printf("test_start_during_arming_returns_hidden_command\n");

    HostProtocol p;
    Host h(p);
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryPosition);
    uint8_t pos[4];
    CHECK(h.read_args(pos, 4));
    h.command(HostProtocol::kHcDone);

    p.set_position(0, 5, PlaybackState::Playing);
    const uint8_t start[8] = {HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryPosition, 0};
    for (uint8_t i = 0; i < 7; ++i) p.on_command_byte(start[i]);
    h.inject = &start[7];
    h.inject_n = 1;
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    CHECK(h.read_args(pos, 4));
    h.command(HostProtocol::kHcDone);
    CHECK_EQ(p.nak_giveups(), 0u);
}

// Записи кадра в порты - код прошивки (host_frame.h), приёмник пишет журнал.
// Вооружение: слово порта данных с первым байтом до барьера, статус - после
// него и последним. 25 чтений порта данных - 20 байт кадра, затем нули,
// индекс на последнем слове стоит. Байт команды снимает статус.
struct RecordingSink {
    static constexpr uint8_t kCmd = 0x10;
    static constexpr uint8_t kDat = 0x20;
    struct Write {
        uint8_t port;
        uint32_t word;
        bool after_fence;
    };
    std::vector<Write> log;
    bool fenced = false;
    void set(uint8_t port, uint32_t word) { log.push_back({port, word, fenced}); }
    uint32_t encode(uint8_t byte) { return 0x100u | byte; } // слово отличимо от байта
    void publish_fence() { fenced = true; }
};

void test_frame_publish_order() {
    std::printf("test_frame_publish_order\n");

    RecordingSink sink;
    uint32_t words[host_frame::kWords] = {};
    uint8_t pos = 7;
    const uint8_t args[5] = {11, 12, 13, 14, 15};
    host_frame::arm(sink, words, pos, HostProtocol::kStPosition, args, 5);
    CHECK_EQ(sink.log.size(), static_cast<size_t>(2));
    CHECK_EQ(sink.log[0].port, RecordingSink::kDat);
    CHECK_EQ(sink.log[0].word, 0x100u | 11u);
    CHECK(!sink.log[0].after_fence);
    CHECK_EQ(sink.log[1].port, RecordingSink::kCmd);
    CHECK_EQ(sink.log[1].word, 0x100u | HostProtocol::kStPosition);
    CHECK(sink.log[1].after_fence);
    CHECK_EQ(pos, static_cast<uint8_t>(0));

    // Хост читает порт данных 25 раз: слово на шине, потом плата готовит
    // следующее.
    uint32_t on_bus = sink.log[0].word;
    sink.log.clear();
    for (uint32_t i = 0; i < 25; ++i) {
        const uint8_t want = i < 5 ? args[i] : 0;
        CHECK_EQ(on_bus, 0x100u | want);
        host_frame::read_done(sink, words, pos);
        CHECK_EQ(sink.log.back().port, RecordingSink::kDat);
        on_bus = sink.log.back().word;
    }
    CHECK_EQ(sink.log.size(), static_cast<size_t>(25));
    CHECK_EQ(pos, static_cast<uint8_t>(host_frame::kWords - 1));

    // Байт команды хоста: статус пуст, кадр не тронут.
    HostProtocol p;
    HostProtocol* proto = &p;
    sink.log.clear();
    host_frame::command_byte(sink, proto, HostProtocol::kHcTrace);
    CHECK_EQ(sink.log.size(), static_cast<size_t>(1));
    CHECK_EQ(sink.log[0].port, RecordingSink::kCmd);
    CHECK_EQ(sink.log[0].word, 0x100u | HostProtocol::kStNone);
    CHECK_EQ(words[0], 0x100u | 11u);
}

// Маркер и NAK после подтверждения не возвращают уже исполненную команду.
//
// Иначе poll() тут же вооружал поверх неё новую, и подтверждение хоста за
// старую доставалось новой: окно чтения с длиной 0 уходило наверх как
// короткое чтение, а сам запрос терялся. Плагин WC шлёт маркеры
// постоянно, в том числе между сэмплами фоновой догрузки.
void test_done_command_is_not_rearmed() {
    std::printf("test_done_command_is_not_rearmed\n");

    Received got;

    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, HostProtocol::kTelemetryPosition);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStPosition));
    uint8_t pos[4];
    CHECK(h.read_args(pos, 4));
    h.command(HostProtocol::kHcDone);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));

    // Маркер после подтверждения: позиция исполнена, возвращать нечего.
    h.command(HostProtocol::kHcTrace, 7, 1, 0, 2, 0);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    // NAK на мусорный статус - тоже: только считается.
    const uint32_t naks = p.nak_total();
    h.command(HostProtocol::kHcNak);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
    CHECK_EQ(p.nak_total(), naks + 1u);
    CHECK_EQ(p.nak_giveups(), 0u);

    // Чтение вооружается и доходит целиком, без короткого чтения.
    p.request_file_chunk(0, 4096);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
    uint8_t a[13];
    CHECK(h.read_args(a, 13));
    std::vector<uint8_t> data(4096, 0x5A);
    h.send_data(data.data(), 4096);
    h.done(4096);
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, static_cast<uint16_t>(4096));
}

// Неизвестная команда поверх неподтверждённой: её первый байт уже снял
// статус на шине, и без возврата хост команду платы больше не увидел бы.
// Возвращается как по NAK, с тем же пределом попыток.
void test_unknown_command_rearms_unacked() {
    std::printf("test_unknown_command_rearms_unacked\n");

    Received got;
    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(4096, 4096);
    p.poll();
    uint8_t first[13];
    CHECK(h.read_args(first, 13));

    h.command(0x7E);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
    uint8_t again[13];
    CHECK(h.read_args(again, 13));
    CHECK_EQ(std::memcmp(first, again, sizeof(first)), 0);

    for (uint8_t i = 1; i < HostProtocol::kMaxNak; ++i) h.command(0x7E);
    CHECK_EQ(p.nak_giveups(), 1u);
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.length, 0u);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone));
}

// NAK посреди приёма окна: TR-DOS-приложение шлёт окно прямо с карты и
// при сбое чтения на середине отвечает NAK поверх уже отданных байт.
// Повтор обязан лечь с начала окна, а не за отданными байтами.
void test_nak_mid_data_restarts_window() {
    std::printf("test_nak_mid_data_restarts_window\n");

    Received got;

    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(8192, 4096);
    p.poll();
    uint8_t a[13];
    CHECK(h.read_args(a, 13));

    // Без периода 256: сдвиг повтора на 1024 байта на таком шаблоне виден.
    std::vector<uint8_t> first(1024), data(4096);
    for (size_t i = 0; i < first.size(); ++i) first[i] = static_cast<uint8_t>(0xA5u ^ i);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + (i >> 8) * 13 + 3);

    h.send_data(first.data(), 1024);
    h.command(HostProtocol::kHcNak);
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
    CHECK(h.read_args(a, 13));
    h.send_data(data.data(), 4096);
    h.done(4096);

    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK_EQ(got.bytes.size(), data.size());
    CHECK(got.bytes == data);
    CHECK_EQ(p.data_overflow(), 0u);
}

void test_session_ready_announced_once() {
    std::printf("test_session_ready_announced_once\n");

    HostProtocol p;
    HostProtocol::Callbacks cb;
    p.set_callbacks(cb);
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.mark_session_ready();
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStSessionReady));

    h.command(HostProtocol::kHcDone);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStNone)); // второй раз не объявляем
}

// Оба буфера должны отсекать лишнее и считать отсечённое. Молча отсечь -
// значит получить тот же отказ, что ловили неделю: данные не те, и никто
// об этом не знает.
// Сброс хоста посреди окна и новая сессия: признак принятого окна прошлой
// сессии не доживает, первое окно новой - по её запросу.
void test_reset_mid_window_drops_old_window() {
    std::printf("test_reset_mid_window_drops_old_window\n");
    Received got;
    HostProtocol p;
    Host h(p);
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(4096, 4096);
    p.poll();
    uint8_t a[13];
    CHECK(h.read_args(a, 13));
    std::vector<uint8_t> old_data(4096, 0x33);
    h.send_data(old_data.data(), 2000);
    h.command(HostProtocol::kHcReset);
    got.collect(p);
    CHECK_EQ(got.calls, 0);
    h.command(HostProtocol::kHcDone); // имя платы принято
    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(8192, 4096);
    p.poll();
    CHECK_EQ(h.poll_status(), static_cast<uint8_t>(HostProtocol::kStReadFast));
    CHECK(h.read_args(a, 13));
    CHECK_EQ(a[1], 0x20u); // смещение 8192
    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 5 + 1);
    h.send_data(data.data(), 4096);
    h.done(4096);
    got.collect(p);
    CHECK_EQ(got.calls, 1);
    CHECK(got.bytes == data);
}

void test_overflow_is_clamped_and_counted() {
    std::printf("test_overflow_is_clamped_and_counted\n");

    HostProtocol p;
    Host h(p);

    h.command(HostProtocol::kHcStart, 0x00, 0x00, 0x10, 0x00, 2, 0);
    p.request_file_chunk(0, 4096);
    p.poll();

    // Хост прислал больше, чем окно. Лишнее должно быть отброшено, а не
    // записано за буфер: цена такой ошибки - чужая память (у хоста однажды
    // так затёрло экран и саму WC).
    std::vector<uint8_t> data(HostProtocol::kDataBufferBytes + 64, 0x11);
    h.send_data(data.data(), static_cast<uint16_t>(data.size()));
    CHECK_EQ(p.data_overflow(), 64u);

    // Длина в подтверждении тоже приходит с шины: заявив больше окна, хост
    // заставил бы нас считать сумму за буфером.
    h.done(static_cast<uint16_t>(HostProtocol::kDataBufferBytes + 8));
    CHECK_EQ(p.done_overflow(), 1u);
    CHECK_EQ(p.data_overflow(), 64u);
    uint16_t len = 0;
    CHECK(p.take_received(len));
    CHECK_EQ(len, HostProtocol::kDataBufferBytes); // наверх - не больше окна

    // Слишком длинная полезная часть - ошибка нашего кода, не хоста.
    CHECK_EQ(p.arg_overflow(), 0u);
}

} // namespace

void run_host_protocol_tests() {
    test_overflow_is_clamped_and_counted();
    test_reset_gives_board_name();
    test_end_session_keeps_board_name_nak();
    test_end_session_mid_command_keeps_frame();
    test_lost_data_byte_rerequests_window();
    test_command_ring_keeps_two_frames();
    test_file_request_round_trip();
    test_nak_rearms_identical_frame();
    test_no_arm_over_unacked_command();
    test_read_frame_echoes_offset();
    test_nak_gives_up_after_limit();
    test_ended_position_survives_giveup();
    test_debug_event_ring_counts_losses();
    test_session_counters_reset_on_start();
    test_short_tail_uses_slow_path();
    test_fast_path_table();
    test_session_start_fields();
    test_telemetry_is_just_another_command();
    test_telemetry_round_robin_order();
    test_file_request_beats_telemetry();
    test_trace_does_not_disturb_armed_command();
    test_host_byte_during_arming_hides_command();
    test_start_during_arming_returns_hidden_command();
    test_frame_publish_order();
    test_done_command_is_not_rearmed();
    test_unknown_command_rearms_unacked();
    test_nak_mid_data_restarts_window();
    test_reset_mid_window_drops_old_window();
    test_session_ready_announced_once();
}
