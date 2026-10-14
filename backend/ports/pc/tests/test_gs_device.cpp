// SPDX-License-Identifier: MIT
#include "testing.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "devices/gs/gs_device.h"
#include "player/gs/fx_player.h"
#include "player/gs/midi_sched.h"

using namespace devices;
using gs::GsDevice;

namespace {

// Примитивы обмена - те же, что в руководстве программиста GS и в
// рабочем драйвере GS (пример nihirash). Тест общается с устройством так
// же, как Z80: пишет в порты, крутит ожидание по битам состояния. Иначе
// проверялись бы не те условия, на которые рассчитывает софт.
struct Bus {
    GsDevice& d;
    uint32_t spins = 0;

    gs::Event sc(uint8_t cmd) { return d.write_command(cmd); } // послать команду
    bool sd(uint8_t data) { return d.write_data(data); }       // послать данные
    uint8_t gd() { return d.read_data(); }                     // принять данные

    // WC - ждать сброса бита команды. На простых командах он не взводится,
    // но ждать всё равно нужно: так делает софт, и если бы бит застревал,
    // тест завис бы, а не прошёл молча.
    bool wc() {
        for (uint32_t i = 0; i < 1000; ++i) {
            ++spins;
            if ((d.read_status() & gs::kCommandBit) == 0) return true;
        }
        return false;
    }
    // WN - ждать установки бита данных (есть что принять).
    bool wn() {
        for (uint32_t i = 0; i < 1000; ++i) {
            ++spins;
            if ((d.read_status() & gs::kDataBit) != 0) return true;
        }
        return false;
    }
    // WD - ждать снятия бита данных (наш байт приняли).
    bool wd() {
        for (uint32_t i = 0; i < 1000; ++i) {
            ++spins;
            if ((d.read_status() & gs::kDataBit) == 0) return true;
        }
        return false;
    }
};

// Детект как в софте: подать сброс и дождаться нуля в бите команды. На
// машине без карты чтение порта даёт 0xFF, где этот бит стоит всегда, и
// ожидание отваливается по таймауту - так софт решает, что карты нет.
void test_detect_like_real_software() {
    std::printf("test_gs_detect_like_real_software\n");
    GsDevice d;
    d.reset();
    Bus b{d};

    // То же, что драйвер nihirash делает в init: холодный рестарт и
    // ожидание.
    b.sc(0xF4);
    CHECK(b.wc());
}

void test_status_never_sets_command_bit() {
    std::printf("test_gs_status_never_sets_command_bit\n");
    GsDevice d;
    d.reset();
    // Бит команды не должен вставать ни после одной команды: софт ждёт
    // нуля, и взведённый бит, который никто не снимет, повесил бы плеер.
    //
    // Удержания ставит автомат на долгих работах, снимает платформа: #30
    // открывает поток, и на #D2 взводится удержание разбора. Поэтому после
    // каждой команды удержания снимаются, как на платформе.
    for (uint16_t cmd = 0; cmd <= 0xFF; ++cmd) {
        d.write_command(static_cast<uint8_t>(cmd));
        d.hold_reserve_done();
        d.hold_parse_done();
        CHECK((d.read_status() & gs::kCommandBit) == 0);
    }
}

void test_memory_reports_three_bytes() {
    std::printf("test_gs_memory_reports_three_bytes\n");
    GsDevice d;
    d.reset();
    Bus b{d};

    // #20: SC / WC / GD / WN / GD / WN / GD, младший байт первым.
    b.sc(0x20);
    CHECK(b.wc());
    CHECK(b.wn());
    const uint32_t lo = b.gd();
    CHECK(b.wn());
    const uint32_t mid = b.gd();
    CHECK(b.wn());
    const uint32_t hi    = b.gd();
    const uint32_t total = lo | (mid << 8) | (hi << 16);
    std::printf("  memory in total: %u bytes\n", total);
    CHECK(total == gs::kDeclaredRamBytes);

    // Байты кончились - бит данных должен сняться, иначе софт будет
    // читать пустоту как данные.
    CHECK((d.read_status() & gs::kDataBit) == 0);

    // #21: свободно столько же - объявленную память ничто не расходует.
    b.sc(0x21);
    CHECK(b.wc());
    CHECK(b.wn());
    const uint32_t f0 = b.gd();
    CHECK(b.wn());
    const uint32_t f1 = b.gd();
    CHECK(b.wn());
    const uint32_t f2 = b.gd();
    CHECK((f0 | (f1 << 8) | (f2 << 16)) == gs::kDeclaredRamBytes);
}

void test_data_on_roundtrip() {
    std::printf("test_gs_data_on_roundtrip\n");
    GsDevice d;
    d.reset();
    Bus b{d};
    // #36 кладёт в регистр вывода 0xFF - этим софт проверяет связь.
    b.sc(0x36);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0xFF);
}

void test_busy_flag_visible_in_hx() {
    std::printf("test_gs_busy_flag_visible_in_hx\n");
    GsDevice d;
    d.reset();
    Bus b{d};
    b.sc(0xF5); // busy on
    CHECK(b.wc());
    b.sc(0xF7); // get HX
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK((b.gd() & 0x80u) != 0);

    b.sc(0xF6); // busy off
    CHECK(b.wc());
    b.sc(0xF7);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK((b.gd() & 0x80u) == 0);
}

void test_arguments_are_taken_before_command() {
    std::printf("test_gs_arguments_are_taken_before_command\n");
    GsDevice d;
    d.reset();
    Bus b{d};
    // #2E "выбрать текущий эффект": аргумент кладётся в регистр данных
    // до кода команды - так описано в руководстве и так делает софт.
    b.sd(7);
    b.sc(0x2E);
    CHECK(b.wc());
    // Аргументы съедаются командой, кроме #D1: #12 без своего аргумента
    // пишет в порт страниц то, что осталось, #11 с аргументом 0 его читает.
    auto page_after = [](uint8_t first_cmd) {
        GsDevice dev;
        dev.reset();
        Bus bus{dev};
        bus.sd(0x35);
        bus.sc(first_cmd);
        CHECK(bus.wc());
        bus.sc(0x12);
        CHECK(bus.wc());
        bus.sd(0);
        bus.sc(0x11);
        CHECK(bus.wn());
        return bus.gd();
    };
    CHECK_EQ(page_after(0x2E), 0x00);
    CHECK_EQ(page_after(0xD1), 0x35);
}

void test_unknown_commands_are_counted() {
    std::printf("test_gs_unknown_commands_are_counted\n");
    GsDevice d;
    d.reset();
    CHECK(d.unknown_count() == 0);
    d.write_command(0x7F); // зарезервировано, не реализовано
    CHECK(d.unknown_count() == 1);
    CHECK(d.unknown_command() == 0x7F);
    // Счётчик переживает рестарт карты: он мерит покрытие за всё время
    // работы, а не за сессию.
    d.reset();
    CHECK(d.unknown_count() == 1);
}

void test_reset_notifies_outside() {
    std::printf("test_gs_reset_notifies_outside\n");
    GsDevice d;
    d.reset();
    CHECK(d.write_command(0xF3) == gs::Event::Reset); // тёплый рестарт
    CHECK(d.write_command(0xF4) == gs::Event::Reset); // холодный
    CHECK(d.write_command(0x00) == gs::Event::None);  // сброс флагов - не сброс карты
}

// Модуль по руководству: SC #30 / WC / GD handle / SC #D1 / байты / SC #D2.
// Байты потока автомат не копит, а отдаёт платформе (write_data -> true);
// #D2 придерживает плеер, пока платформа разбирает модуль.
void test_module_stream_events() {
    std::printf("test_gs_module_stream_events\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    CHECK(!b.sd(0x11)); // вне потока - аргумент, не байт потока
    CHECK(b.sc(0x30) == gs::Event::StreamBegin);
    CHECK(d.stream() == gs::Stream::Module);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 1);
    CHECK(b.sc(0xD1) == gs::Event::None);
    const uint8_t module[] = {0x4D, 0x2E, 0x4B, 0x2E, 0x00, 0xFF};
    for (uint8_t v : module) {
        CHECK(b.sd(v));
    }
    CHECK(d.stream_bytes() == sizeof(module));

    CHECK(b.sc(0xD2) == gs::Event::StreamEnd);
    CHECK(d.stream() == gs::Stream::None);
    CHECK(!b.wc()); // разбор идёт, плеер ждёт
    d.hold_parse_done();
    CHECK(b.wc());
    CHECK(!b.sd(0x00)); // поток закрыт

    // #D2 без открытого потока - не событие и не удержание.
    CHECK(b.sc(0xD2) == gs::Event::None);
    CHECK(b.wc());

    CHECK(b.sc(0x31) == gs::Event::Play);
    CHECK(b.sc(0x32) == gs::Event::Stop);
    CHECK(b.sc(0x33) == gs::Event::Resume);
    CHECK(d.unknown_count() == 0);
}

// Мост придерживает плеер на первом #30, пока освобождает память. Удержание обязано пережить и ответ с номером
// модуля, и его чтение: иначе плеер не ждёт, и начало модуля теряется в
// кольце приёма.
void test_hold_survives_response_and_read() {
    std::printf("test_gs_hold_survives_response_and_read\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    CHECK(b.sc(0x30) == gs::Event::StreamBegin);
    d.hold_reserve_begin();
    CHECK((d.read_status() & gs::kCommandBit) != 0); // придержан
    CHECK((d.read_status() & gs::kDataBit) != 0);    // номер модуля готов
    CHECK(b.gd() == 1);                              // GD handle
    CHECK((d.read_status() & gs::kCommandBit) != 0); // чтение не сняло
    d.reset();                                       // и сброс карты не снимает
    CHECK((d.read_status() & gs::kCommandBit) != 0);
    d.hold_reserve_done();
    CHECK((d.read_status() & gs::kCommandBit) == 0);
}

// Удержаний два, и каждое снимает свой владелец: резерв памяти под приём
// (#30, снимает цикл, когда память нашлась) и разбор модуля (#D2, снимает
// разбор). С одним флагом на двоих короткий модуль успевал закрыться до
// отпуска резерва, и чужое снятие отпускало плеера посреди разбора; а
// брошенный разбор (сброс или новый #30 сразу за #D2) оставлял удержание
// навсегда, и плеер на WC вис до перезагрузки.
void test_two_holds_are_independent() {
    std::printf("test_gs_two_holds_are_independent\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Резерв взят, следом закрылся поток: держат оба.
    CHECK(b.sc(0x30) == gs::Event::StreamBegin);
    d.hold_reserve_begin();
    CHECK(!b.wc());
    CHECK(b.sc(0xD2) == gs::Event::StreamEnd); // hold_parse_begin внутри
    CHECK(!b.wc());

    // Резерв отпущен - плеер всё ещё ждёт разбора.
    d.hold_reserve_done();
    CHECK(!b.wc());
    d.hold_parse_done();
    CHECK(b.wc());

    // Брошенный разбор: удержание снимает тот, кто бросил.
    CHECK(b.sc(0x30) == gs::Event::StreamBegin);
    CHECK(b.sc(0xD2) == gs::Event::StreamEnd);
    CHECK(!b.wc());
    d.hold_parse_done(); // так делает мост, когда гасит s_load_pending
    CHECK(b.wc());
}

// --- Память и процессор карты, #13..#1D ---
//
// Заглушки: исполнять и хранить нечего, но каждая команда обязана съесть
// ровно свои байты. Иначе остаток адреса или блока ушёл бы аргументом в
// следующую команду. Проверка - #12 после каждой: её аргумент должен дойти
// до регистра страниц нетронутым (читается обратно через #11).
bool pages_roundtrip(Bus& b, uint8_t v) {
    b.sd(v);
    b.sc(0x12);
    if (!b.wc()) return false;
    b.sd(0x00);
    b.sc(0x11);
    if (!b.wc() || !b.wn()) return false;
    return b.gd() == v;
}

void test_card_memory_commands_are_stubbed() {
    std::printf("test_gs_card_memory_commands_are_stubbed\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // #13 Jump: SD ADR.L / SC / WC / SD ADR.H.
    b.sd(0x00);
    b.sc(0x13);
    CHECK(b.wc());
    CHECK(!b.sd(0x80));
    CHECK(b.wd());
    CHECK(pages_roundtrip(b, 0x21));

    // #14 Load block, LEN = 0x0103: хвост из трёх байт и 259 байт блока.
    b.sd(0x03);
    b.sc(0x14);
    b.sd(0x01);
    b.sd(0x00);
    b.sd(0x80); // LEN.H, ADR.L, ADR.H
    for (uint32_t i = 0; i < 0x103u; ++i) {
        CHECK(!b.sd(static_cast<uint8_t>(0xA0u + i)));
        CHECK(b.wd());
    }
    CHECK(pages_roundtrip(b, 0x22));

    // #15 Get block, LEN = 2: после хвоста два нуля, потом бит данных снят.
    b.sd(0x02);
    b.sc(0x15);
    b.sd(0x00);
    b.sd(0x00);
    b.sd(0x80);
    CHECK(b.wn());
    CHECK(d.peek_data() == 0);
    CHECK(b.gd() == 0);
    CHECK(b.wn());
    CHECK(b.gd() == 0);
    CHECK((d.read_status() & gs::kDataBit) == 0);
    CHECK(pages_roundtrip(b, 0x23));

    // #16 Poke: SD Byte / SC / WC / SD ADR.L / WD / SD ADR.H / WD.
    b.sd(0x55);
    b.sc(0x16);
    CHECK(b.wc());
    b.sd(0x00);
    CHECK(b.wd());
    b.sd(0x80);
    CHECK(b.wd());
    CHECK(pages_roundtrip(b, 0x24));

    // #17 Peek: SD ADR.L / SC / WD / SD ADR.H / GD Byte.
    b.sd(0x00);
    b.sc(0x17);
    CHECK(b.wd());
    b.sd(0x80);
    CHECK(b.wn());
    CHECK(b.gd() == 0);
    CHECK(pages_roundtrip(b, 0x25));

    // #18 Load DE, #19 Poke (DE), #1A Peek (DE), #1B Inc DE.
    b.sd(0x00);
    b.sc(0x18);
    CHECK(b.wc());
    b.sd(0x80);
    CHECK(b.wd());
    b.sd(0x55);
    b.sc(0x19);
    CHECK(b.wc());
    b.sc(0x1A);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0);
    b.sc(0x1B);
    CHECK(b.wc());
    CHECK(pages_roundtrip(b, 0x26));

    // #1C Poke (#20XX): SD ADR.L / SC / WC / SD Byte. #1D Peek (#20XX).
    b.sd(0x10);
    b.sc(0x1C);
    CHECK(b.wc());
    b.sd(0x55);
    CHECK(b.wd());
    b.sd(0x10);
    b.sc(0x1D);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0);
    CHECK(pages_roundtrip(b, 0x27));

    // Недособранный хвост бросает следующая команда: #14 с одним байтом
    // хвоста из трёх, затем #1B.
    b.sd(0x05);
    b.sc(0x14);
    b.sd(0x01);
    b.sc(0x1B);
    CHECK(b.wc());
    CHECK(pages_roundtrip(b, 0x28));

    CHECK(d.unknown_count() == 0);
}

// Переменные ПЗУ карты через DE: Z-Player 4.1 читает #4151 (#18 DE=#4151,
// #1A) и при нуле встаёт навсегда; настоящее ПЗУ пишет туда #C3. #1B
// двигает DE, #17 читает по адресу из аргумента и хвоста.
// Залить в карту программу и запустить её командой #13.
static void upload_blob(Bus& b, uint16_t addr, const std::vector<uint8_t>& body) {
    b.sd(static_cast<uint8_t>(addr & 0xFF));
    b.sc(0x18);
    CHECK(b.wc());
    b.sd(static_cast<uint8_t>(addr >> 8));
    CHECK(b.wd());
    for (uint8_t byte : body) {
        b.sd(byte);
        b.sc(0x19);
        CHECK(b.wc());
        b.sc(0x1B);
        CHECK(b.wc());
    }
    b.sd(static_cast<uint8_t>(addr & 0xFF));
    b.sc(0x13);
    CHECK(b.wc());
    b.sd(static_cast<uint8_t>(addr >> 8));
}

// Модуль попадает в карту не только командой #30. Начиная с версии 4.0
// Z-Player заливает свою программу и запускает её командой #13, а она
// зовёт приёмник потока в ПЗУ карты - `call #0C09`, байты CD 09 0C. Такая
// программа заменяет собой #30 целиком, и без её узнавания модуль пропал
// бы: записи в порт данных мы сочли бы аргументами команд.
void test_loader_blob_opens_stream() {
    std::printf("test_gs_loader_blob_opens_stream\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Программа без вызова приёмника потока: #13 поток не открывает.
    upload_blob(b, 0x5B00, {0xF3, 0xCD, 0x0A, 0x0C, 0xC9});
    CHECK(d.stream_bytes() == 0);
    b.sd(0x11); // пошёл бы в поток - сосчитался бы
    CHECK(d.stream_bytes() == 0);

    d.reset();

    // Та же, но с CD 09 0C: #13 открывает поток.
    upload_blob(b, 0x5B00, {0xF3, 0xCD, 0x09, 0x0C, 0xC9});
    CHECK(d.stream_bytes() == 0); // старший байт адреса - хвост, не поток
    b.sd(0x11);
    b.sd(0x22);
    b.sd(0x33);
    CHECK(d.stream_bytes() == 3);

    // Команда посреди потока проглатывается: поток цел, байты идут дальше.
    b.sc(0x00);
    b.sc(0x31);
    b.sc(0x37);
    b.sd(0x44);
    CHECK(d.stream_bytes() == 4);

    // Закрывают поток только #D2, #F3 и #F4.
    b.sc(0xD2);
    d.hold_parse_done();
    b.sd(0x55);
    CHECK(d.stream_bytes() == 4);
}

// Опознание карты и процессора: плеер заливает пробу и читает один байт.
// Без ответа он прочтёт остаток регистра вывода и по ненулевому объявит
// карту NeoGS.
void test_card_identity_blobs() {
    std::printf("test_gs_card_identity_blobs\n");
    GsDevice d;
    Bus b{d};

    struct Probe {
        const char* file;
        size_t len;
        uint8_t want;
    };
    const Probe probes[] = {
        {"ZPlayer/v41/gs_port0f_5B00.bin", 64, 0x00}, // не NeoGS
        {"ZPlayer/v41/gs_cpu_5B00.bin", 15, 0xFF},    // КМОП Z84C
        {"ZPlayer/v41/gs_clk_5B00.bin", 125, 12},     // 12 МГц
    };
    for (const Probe& p : probes) {
        std::ifstream in(p.file, std::ios::binary);
        if (!in) {
            std::printf("  нет %s - SKIP\n", p.file);
            continue;
        }
        const std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK_EQ(body.size(), p.len);

        d.reset();
        // Регистр вывода заранее испачкан: без подмены плеер прочёл бы его.
        b.sd(0x55);
        b.sc(0x12);
        CHECK(b.wc());

        upload_blob(b, 0x5B00, body);
        b.sc(0xFC); // будилка - любой байт
        CHECK(b.wc());
        CHECK(b.wn());
        CHECK(b.gd() == p.want);
    }
}

// Резидент, который плеер заливает в карту: он отдаёт блок состояния для
// экрана. Без него плеер встаёт на ожидании бита данных, и интерфейс
// замерзает, даже когда модуль уже играет.
void test_resident_telemetry() {
    std::printf("test_gs_resident_telemetry\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Тело резидента 4.x: узнаётся по сумме и длине, поэтому берём
    // настоящее. Длина 428, сумма 050898D8.
    std::ifstream in("ZPlayer/v41/gs_interp_5B00.bin", std::ios::binary);
    if (!in) {
        std::printf("  нет тела резидента - SKIP\n");
        return;
    }
    const std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(body.size(), 428u);

    const uint8_t notes[4]  = {0x24, 0x30, 0xFF, 0x47};
    const uint8_t levels[4] = {0, 64, 32, 0};
    d.set_telemetry(0x05, 0x11, notes, levels, 6, true);

    upload_blob(b, 0x5B00, body);

    // #EE: одиннадцать байт состояния.
    b.sc(0xEE);
    CHECK(b.wc());
    uint8_t got[11];
    for (uint8_t i = 0; i < 11; ++i) {
        CHECK(b.wn());
        got[i] = b.gd();
    }
    CHECK(got[0] == 0x05);      // позиция
    CHECK(got[1] == 0x11);      // строка
    CHECK(got[6] == 0x80);      // канал молчит - ровно средняя точка
    CHECK(got[7] == 0x80 + 64); // полная громкость
    CHECK(got[10] == (0x80 | 6));

    // Ноты (#63 Get Channel Notes): молчащий канал - 127, а не последняя
    // нота; первая выдача - без бита "не менялось". Шкала движка не
    // сдвигается: сдвиг на индекс ProTracker гасит подписи у 4.1 и 5.1.
    CHECK(got[2] == 127);  // уровень 0 - канал молчит
    CHECK(got[3] == 0x30); // нота как есть
    CHECK(got[4] == 127);  // ноты не было вовсе (0xFF на входе)
    CHECK(got[5] == 127);  // уровень 0

    // Следом резидент ждёт ещё команду. #FF - паттерн не нужен.
    b.sc(0xFF);
    CHECK(b.wc());
    CHECK((d.read_status() & gs::kDataBit) == 0);

    // Вторая выдача того же: прошивка взводит бит 7 в самой переменной
    // после выдачи, значит повтор обязан прийти с ним.
    b.sc(0xEE);
    CHECK(b.wc());
    for (uint8_t i = 0; i < 11; ++i) {
        CHECK(b.wn());
        got[i] = b.gd();
    }
    CHECK(got[2] == (127 | 0x80));
    CHECK(got[3] == (0x30 | 0x80));
    b.sc(0xFF);
    CHECK(b.wc());

    // А с номером позиции - отдаёт килобайт.
    b.sc(0xEE);
    CHECK(b.wc());
    for (uint8_t i = 0; i < 11; ++i) {
        CHECK(b.wn());
        b.gd();
    }
    b.sc(0x03);
    CHECK(b.wc());
    uint32_t pattern = 0;
    while (pattern < 2000 && (d.read_status() & gs::kDataBit) != 0) {
        b.gd();
        ++pattern;
    }
    CHECK_EQ(pattern, 1024u);

    // Чужая команда кончает резидента: дальше отвечает обычный разбор.
    b.sc(0x23);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == gs::kDeclaredPages);
}

// Паттерн по номеру позиции: резидент берёт номер паттерна из таблицы
// порядка модуля и отдаёт килобайт. Модуль собран здесь, чтобы проверялась
// арифметика раскладки, а не совпадение с чьим-то файлом.
void test_resident_pattern_from_module() {
    std::printf("test_gs_resident_pattern_from_module\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    std::ifstream in("ZPlayer/v41/gs_interp_5B00.bin", std::ios::binary);
    if (!in) {
        std::printf("  нет тела резидента - SKIP\n");
        return;
    }
    const std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Модуль на три паттерна; позиция 1 указывает на паттерн 2.
    std::vector<uint8_t> mod(1084 + 3 * 1024, 0);
    mod[952 + 0] = 0;
    mod[952 + 1] = 2;
    for (uint32_t i = 0; i < 1024; ++i)
        mod[1084 + 2 * 1024 + i] = static_cast<uint8_t>(i * 7 + 1);
    d.set_module_image(mod.data(), static_cast<uint32_t>(mod.size()));

    upload_blob(b, 0x5B00, body);
    b.sc(0xEE);
    CHECK(b.wc());
    for (uint8_t i = 0; i < 11; ++i) {
        CHECK(b.wn());
        b.gd();
    }

    b.sc(0x01); // позиция 1 -> паттерн 2
    CHECK(b.wc());
    uint32_t n = 0, wrong = 0;
    while (n < 1024) {
        CHECK(b.wn());
        if (b.gd() != static_cast<uint8_t>(n * 7 + 1)) ++wrong;
        ++n;
    }
    CHECK_EQ(wrong, 0u);
    CHECK((d.read_status() & gs::kDataBit) == 0); // ровно килобайт, не больше

    // Позиция за пределами таблицы - нули, а не мусор и не отказ.
    b.sc(0xEE);
    CHECK(b.wc());
    for (uint8_t i = 0; i < 11; ++i) {
        CHECK(b.wn());
        b.gd();
    }
    b.sc(0x7F);
    CHECK(b.wc());
    uint32_t zeros = 0;
    while (zeros < 1024) {
        CHECK(b.wn());
        if (b.gd() == 0)
            ++zeros;
        else
            break;
    }
    CHECK_EQ(zeros, 1024u);
}

// Опознание карты у Z-Player 5.1 (#D1E8): записать байт в регистр данных -
// бит данных обязан взвестись, - затем подать #00 и убедиться, что оба
// бита сброшены. Проба идёт дважды подряд и повторяется 92 раза, после
// чего плеер печатает "General Sound not present".
//
// Так ведёт себя настоящая карта: главный цикл прошивки регистр данных не
// читает вовсе, поэтому записанный байт висит в нём до команды. Замерено
// прогоном ПЗУ 1.08.
void test_data_write_raises_data_bit() {
    std::printf("test_gs_data_write_raises_data_bit\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    for (int pass = 0; pass < 2; ++pass) {
        CHECK((d.read_status() & gs::kDataBit) == 0); // до записи - чисто
        b.sd(0x00);
        CHECK((d.read_status() & gs::kDataBit) == gs::kDataBit);
        b.sc(0x00); // #00 Reset flags
        CHECK((d.read_status() & (gs::kDataBit | gs::kCommandBit)) == 0);
    }

    // Аргумент команды бит тоже взводит, а команда его снимает: иначе
    // разошлись бы проба и обычный порядок "SD аргумент / SC код".
    b.sd(0x2A);
    CHECK((d.read_status() & gs::kDataBit) == gs::kDataBit);
    b.sc(0x2A); // Set Module Master Volume: прежнее значение в регистр вывода
    CHECK(b.wc());
    CHECK((d.read_status() & gs::kDataBit) == 0); // аргумент съеден - бит снят
    CHECK(b.gd() == 0x40);
}

// Проба проигрывателя MIDI у 5.x: плеер шлёт #02 и читает #B3. Ноль
// значит "блоб в карте работает", и плеер уходит ждать 1536 байт списка
// дорожек - которых мы не пришлём. Отвечать нужно не нулём.
//
// Прошивка 1.08 на #02 регистр данных не трогает вовсе (замерено прогоном
// ПЗУ), поэтому ответ ставится ровно в одном случае - когда залит
// проигрыватель MIDI. Опознаётся он суммой в обход шести байт, которые
// плеер правит под конкретный файл.
void test_midi_blob_probe_refuses() {
    std::printf("test_gs_midi_blob_probe_refuses\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Без залитого проигрывателя #02 - обычная команда громкости: регистр
    // данных она не трогает, как и у настоящей карты.
    b.sc(0x36); // Data on: в регистр вывода #FF
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0xFF);
    b.sc(0x02);
    CHECK(b.wc());
    CHECK(!b.wn()); // ответа нет: бит данных не взведён

    const char* files[] = {"ZPlayer/v51/27_gs_midi.bin", "ZPlayer/v50/27_gs_midi.bin"};
    for (const char* file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::printf("  нет %s - SKIP\n", file);
            continue;
        }
        std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK_EQ(body.size(), 2356u);

        // Шесть правимых мест: в файле там нули, на проводе - параметры
        // конкретного файла. Ставим заведомо чужие байты - сумма в обход
        // обязана опознать блоб всё равно.
        const uint16_t patched[] = {0x62d, 0x62e, 0x63c, 0x76b, 0x7bc, 0x853, 0x854};
        uint8_t fill             = 0xA1;
        for (uint16_t at : patched)
            body[at] = fill++;

        d.reset();
        upload_blob(b, 0x5000, body);
        // Тела 5.0 и 5.1 отличаются только правимыми местами, поэтому
        // опознаются обе одной суммой, какие бы байты там ни стояли.
        CHECK(d.midi_active());
        CHECK(d.stream() == gs::Stream::Midi);

        // Блоб зовёт приёмник потока из ПЗУ (CD 09 0C), поэтому #13 открыл
        // поток: следом плеер льёт в карту сам файл и закрывает его #D2.
        // Пока поток открыт, команды карта не разбирает.
        b.sd(0x4D);
        b.sd(0x54);
        CHECK(d.stream_bytes() == 2);
        b.sc(0xD2);
        d.hold_parse_done();

        b.sc(0x02);
        CHECK(b.wc());
        CHECK(b.wn());
        const uint8_t answer = b.gd();
        CHECK(answer != 0x00); // ноль увёл бы плеер в вечное ожидание
    }
}

// #38 и #3E Load FX: заливка сэмпла эффекта. По руководству карта сама
// выделяет место и отдаёт номер, а байты идут обычным потоком - тем же,
// что и модуль. Сырая память карты играм для этого не нужна.
void test_fx_sample_load() {
    std::printf("test_gs_fx_sample_load\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // #38: номер приходит сразу, поток открывается здесь же.
    b.sc(0x38);
    CHECK(b.wc());
    CHECK(b.wn());
    // Номера с единицы: прошивка 1.08 на первый #38 отвечает #01, а ноль
    // у неё занят подменой на CurFX.
    CHECK(b.gd() == 1);
    CHECK(d.stream() == gs::Stream::FxUnsigned);
    CHECK(d.fx_loading() == 1);
    CHECK(d.current_fx() == 1); // руководство: CurFX становится номером

    // #D1 в этом месте ничего не делает: поток уже открыт, и затвор потока
    // команду проглатывает - ровно как у модуля.
    b.sc(0xD1);
    b.sd(0x11);
    b.sd(0x22);
    b.sd(0x33);
    CHECK(d.stream_bytes() == 3);
    b.sc(0xD2);
    // Удержания разбора у сэмпла нет: разбирать нечего, и плеер идёт
    // дальше сразу. У модуля оно есть, и снимает его платформа.
    CHECK((d.read_status() & gs::kCommandBit) == 0);
    CHECK(d.stream() == gs::Stream::None);
    CHECK(d.fx_loading() == GsDevice::kFxNone);

    // Умолчания из руководства.
    const gs::FxSample& s = d.fx_sample(1);
    CHECK(s.note == 60);
    CHECK(s.volume == 0x40);
    CHECK(s.finetune == 0);
    CHECK(s.seek_first == 0x0F);
    CHECK(s.seek_last == 0x0F);
    CHECK(s.priority == 0x80);
    CHECK(!s.signed_pcm);
    CHECK(!s.loop); // петли нет

    // #3E с аргументом #01 - знаковый сэмпл (Amiga), без аргумента - тот
    // же #38. Перепутать их - перевернуть полярность.
    b.sd(0x01);
    b.sc(0x3E);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 2);
    CHECK(d.stream() == gs::Stream::FxSigned);
    CHECK(d.fx_sample(2).signed_pcm);
    CHECK(d.current_fx() == 2);
    b.sc(0xD2);

    // Длину ставит платформа: автомат байтов не считает, в кольце могло
    // потеряться.
    d.set_fx_length(1, 0, 3);
    CHECK(d.fx_sample(1).length == 3);
    // Конец петли по умолчанию - длина: карта ставит его при закрытии потока.
    CHECK(d.fx_sample(1).loop_end == 3);
    CHECK(!d.fx_sample(1).loop);

    // Таблица кончилась - номер вне её, и поток НЕ открывается: иначе байты
    // ушли бы в никуда, а плеер ждал бы конца.
    while (d.fx_count() < gs::kFxSamplesMax) {
        b.sc(0x38);
        CHECK(b.wc());
        CHECK(b.wn());
        b.gd();
        b.sc(0xD2);
    }
    b.sc(0x38);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == GsDevice::kFxNone);
    CHECK(d.stream() == gs::Stream::None);
}

// Параметры эффекта, #40..#49 плюс громкости #2B и #3D. Всё, что здесь
// проверяется, замерено прогоном ПЗУ 1.08: потолки, кто отдаёт прежнее
// значение, раскладка трёхбайтовых границ петли.
void test_fx_parameters() {
    std::printf("test_gs_fx_parameters\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    CHECK(d.fx_master_volume() == 0x40); // карта включается на полной
    CHECK(d.fx_volume() == 0x40);

    // Два сэмпла: первый - подопытный, второй должен остаться нетронутым.
    b.sc(0x38);
    CHECK(b.gd() == 1);
    b.sd(0x11);
    b.sd(0x22);
    b.sd(0x33);
    b.sc(0xD2);
    d.set_fx_length(1, 0, 3);
    b.sc(0x38);
    CHECK(b.gd() == 2);
    b.sc(0xD2);
    CHECK(d.current_fx() == 2); // заливка сделала текущим второй

    // #2E возвращает прежний текущий регистром вывода.
    b.sd(0x01);
    b.sc(0x2E);
    CHECK(b.wc());
    CHECK((d.read_status() & gs::kDataBit) == 0);
    CHECK(b.gd() == 2);
    CHECK(d.current_fx() == 1);

    // #40 нота: режется по 95, регистр вывода не трогает.
    b.sd(72);
    b.sc(0x40);
    CHECK(b.gd() == 2); // прежний байт регистра на месте
    CHECK(d.fx_sample(1).note == 72);
    b.sd(0xFF);
    b.sc(0x40);
    CHECK(d.fx_sample(1).note == gs::kNoteMax);

    // #41 громкость: режется по #40, регистр вывода не трогает.
    b.sd(0x20);
    b.sc(0x41);
    CHECK(b.gd() == 2);
    CHECK(d.fx_sample(1).volume == 0x20);
    b.sd(0xFF);
    b.sc(0x41);
    CHECK(d.fx_sample(1).volume == 0x40);

    // #42, #45, #46, #47: байт как есть, прежнее значение в регистр вывода.
    b.sd(0x11);
    b.sc(0x42);
    CHECK(b.gd() == 0x00); // finetune по умолчанию
    CHECK(d.fx_sample(1).finetune == 0x11);
    b.sd(0xFF);
    b.sc(0x42);
    CHECK(d.fx_sample(1).finetune == 0xFF); // потолка у finetune нет

    b.sd(0x33);
    b.sc(0x45);
    CHECK(b.gd() == 0x80); // приоритет по умолчанию
    CHECK(d.fx_sample(1).priority == 0x33);
    b.sd(0x00);
    b.sc(0x45);
    CHECK(d.fx_sample(1).priority == 0x00); // #00 проходит, хотя руководство обещает #01..#FE

    b.sd(0x03);
    b.sc(0x46);
    CHECK(b.gd() == 0x0F);
    CHECK(d.fx_sample(1).seek_first == 0x03);
    b.sd(0x0C);
    b.sc(0x47);
    CHECK(b.gd() == 0x0F);
    CHECK(d.fx_sample(1).seek_last == 0x0C);

    // Границы петли: LEN.L аргументом, LEN.M и LEN.H хвостом.
    b.sd(0x34);
    b.sc(0x48);
    b.sd(0x12);
    b.sd(0x00);
    CHECK(d.fx_sample(1).loop);
    CHECK(d.fx_sample(1).loop_begin == 0x1234);
    b.sd(0x78);
    b.sc(0x49);
    b.sd(0x56);
    b.sd(0x00);
    CHECK(d.fx_sample(1).loop_end == 0x5678);

    // Старший байт #FF у #48 - "петли нет", а не адрес.
    b.sd(0x00);
    b.sc(0x48);
    b.sd(0x00);
    b.sd(0xFF);
    CHECK(!d.fx_sample(1).loop);
    CHECK(d.fx_sample(1).loop_begin == 0x1234); // границу не трогали

    // #2B и #3D: обе режутся по #40 и отдают прежнее значение.
    b.sd(0x30);
    b.sc(0x2B);
    CHECK(b.gd() == 0x40);
    CHECK(d.fx_master_volume() == 0x30);
    b.sd(0xFF);
    b.sc(0x2B);
    CHECK(b.gd() == 0x30);
    CHECK(d.fx_master_volume() == 0x40);
    b.sd(0x28);
    b.sc(0x3D);
    CHECK(b.gd() == 0x40);
    CHECK(d.fx_volume() == 0x28);

    // Второй сэмпл всё это время в стороне: команды работают над CurFX.
    CHECK(d.fx_sample(2).note == 60);
    CHECK(d.fx_sample(2).volume == 0x40);
    CHECK(d.fx_sample(2).priority == 0x80);

    // Текущего нет - команда съедает аргумент и молчит, а не портит чужое.
    b.sd(0x00);
    b.sc(0x2E);
    b.sd(99);
    b.sc(0x40);
    CHECK(d.fx_sample(1).note == gs::kNoteMax);
    CHECK(d.fx_sample(2).note == 60);

    CHECK(d.unknown_count() == 0);
}

// #39 Play FX и #3A Stop FX. Выбор канала - по руководству: свободный из
// SeekFirst, потом свободный из SeekLast, потом отнять самый
// низкоприоритетный, и только если наш приоритет выше.
void test_fx_play_and_stop() {
    std::printf("test_gs_fx_play_and_stop\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Один сэмпл с настоящей длиной: пустой играть нечем.
    b.sc(0x38);
    CHECK(b.gd() == 1);
    b.sc(0xD2);
    d.set_fx_length(1, 0, 256);

    // Все каналы свободны - первый из SeekFirst.
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 0);
    CHECK(d.fx_start().handle == 1);
    CHECK(d.fx_start().note == gs::kFxDefaultNote);
    CHECK(d.fx_start().volume == gs::kFxDefaultVolume);

    // Канал 0 занят: SeekFirst по умолчанию #0F, берётся следующий свободный.
    d.set_fx_active(0x01);
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 1);

    // SeekFirst сузим до канала 0, SeekLast оставим всем: занятый канал 0
    // уводит выбор в SeekLast.
    b.sd(0x01);
    b.sc(0x46);
    b.sd(0x0F);
    b.sc(0x47);
    d.set_fx_active(0x03);
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 2);

    // Четвёртый запуск - в последний свободный канал. Без него у канала 3
    // остался бы приоритет ноль, и отнимался бы всегда он.
    d.set_fx_active(0x07);
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 3);

    // Все заняты и приоритет равен - канал не отнимается.
    d.set_fx_active(0x0F);
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::None);

    // Приоритет выше - отнимается канал с самым низким. Прошлые запуски
    // поставили везде #80; опустим канал 2 и посмотрим, что возьмут его.
    b.sd(0x10);
    b.sc(0x45);
    b.sd(0x01);
    b.sc(0x46);
    b.sd(0x04);
    b.sc(0x47); // SeekLast - только канал 2
    d.set_fx_active(0x0F);
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::None); // #10 против #80 в канале 2 - мало

    b.sd(0x0F);
    b.sc(0x47); // SeekLast обратно на все четыре
    b.sd(0x81);
    b.sc(0x45);
    b.sd(0x01);
    CHECK(b.sc(0x39) == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 0); // приоритеты равны - берётся первый

    // Ноль вместо номера - текущий сэмпл.
    d.set_fx_active(0x00);
    b.sd(0x00);
    CHECK(b.sc(0x39) == gs::Event::FxPlay);
    CHECK(d.fx_start().handle == 1);

    // Сэмпла нет - команда молчит, а не запускает тишину.
    b.sd(0x05);
    CHECK(b.sc(0x39) == gs::Event::None);

    // #3A: порядок бит перевёрнут, бит 7 гасит нулевой канал.
    b.sd(0x80);
    CHECK(b.sc(0x3A) == gs::Event::FxStop);
    CHECK(d.fx_stop_mask() == 0x01);
    b.sd(0x40);
    b.sc(0x3A);
    CHECK(d.fx_stop_mask() == 0x02);
    b.sd(0xFF);
    b.sc(0x3A);
    CHECK(d.fx_stop_mask() == 0x0F); // рекомендованный способ - гасить всё
    b.sd(0x0F);
    b.sc(0x3A);
    CHECK(d.fx_stop_mask() == 0x00); // младшая половина не гасит ничего

    CHECK(d.unknown_count() == 0);
}

// Источник эффектов: сам звук. Проверяется то, что слышно, - разводка по
// каналам, уровень, конец сэмпла и петля.
void test_fx_player_renders() {
    std::printf("test_gs_fx_player_renders\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Беззнаковый сэмпл: середина шкалы #80, края дают полный размах.
    static const uint8_t pcm[8] = {0xFF, 0x80, 0x00, 0x80, 0xFF, 0x80, 0x00, 0x80};
    b.sc(0x38);
    CHECK(b.gd() == 1);
    b.sc(0xD2);
    d.set_fx_length(1, 0, sizeof(pcm));

    player::gs::FxPlayer fx;
    fx.attach(&d, pcm);
    fx.set_fx_volume(0x40);

    gs::FxStart cmd;
    cmd.channel = 0;
    cmd.handle  = 1;
    cmd.note    = gs::kFxDefaultNote;
    cmd.volume  = gs::kFxDefaultVolume;
    gs::fx_snapshot(cmd, d.fx_sample(cmd.handle));
    fx.request_start(cmd);

    int32_t l[16]                        = {};
    int32_t r[16]                        = {};
    soundsinth::mixbus::SoundSource* src = fx.as_sound_source();
    src->render_add(src->self, l, r, 16);

    // Канал 0 - левый, и только левый. Уровень - как у голоса трекера на
    // той же громкости: сэмпл на полной шкале при громкостях #40 даёт
    // 127 x 32768, то есть половину шкалы шины.
    CHECK(l[0] == (0xFF - 128) * 32768);
    CHECK(l[0] == (0xFF - 128) * 64 * 64 * 8);
    CHECK(r[0] == 0);
    CHECK(fx.active_mask() == 0x01);

    // Нота 60 это около 16.6 кГц против 44100 на выходе - шаг меньше
    // половины, первые кадры читают тот же отсчёт.
    CHECK(l[1] == l[0]);

    // Сэмпл кончается: восемь отсчётов при таком шаге - это чуть больше
    // двадцати кадров, поэтому домолчит он на следующем блоке.
    src->render_add(src->self, l, r, 16);
    CHECK(fx.active_mask() == 0);

    // Канал 2 - правый.
    cmd.channel = 2;
    gs::fx_snapshot(cmd, d.fx_sample(cmd.handle));
    fx.request_start(cmd);
    for (int32_t& v : l)
        v = 0;
    for (int32_t& v : r)
        v = 0;
    src->render_add(src->self, l, r, 4);
    CHECK(r[0] == (0xFF - 128) * 64 * 64 * 8);
    CHECK(l[0] == 0);

    // Громкость сэмпла вдвое тише - и вклад вдвое меньше.
    fx.request_stop(0x0F);
    cmd.channel = 0;
    cmd.volume  = 0x20;
    gs::fx_snapshot(cmd, d.fx_sample(cmd.handle));
    fx.request_start(cmd);
    for (int32_t& v : l)
        v = 0;
    src->render_add(src->self, l, r, 4);
    CHECK(l[0] == (0xFF - 128) * 32 * 64 * 8);

    // Петля: #48 с нулевым старшим байтом включает её, #49 задаёт конец.
    // Голос после неё не замолкает.
    b.sd(0x00);
    b.sc(0x48);
    b.sd(0x00);
    b.sd(0x00); // начало петли 0
    b.sd(0x04);
    b.sc(0x49);
    b.sd(0x00);
    b.sd(0x00); // конец петли 4
    CHECK(d.fx_sample(1).loop);

    fx.request_stop(0x0F);
    cmd.volume = gs::kFxDefaultVolume;
    gs::fx_snapshot(cmd, d.fx_sample(cmd.handle));
    fx.request_start(cmd);
    for (int i = 0; i < 8; ++i)
        src->render_add(src->self, l, r, 16);
    CHECK(fx.active_mask() == 0x01); // зациклен - играет и через 128 кадров

    // Глушение исполняется честно.
    fx.request_stop(0x01);
    src->render_add(src->self, l, r, 1);
    CHECK(fx.active_mask() == 0);
}

// Прямой запуск #80-#9B и смена на лету #A0-#AB. Канал назван в самом
// коде, выбор и приоритеты не работают.
void test_fx_direct_play() {
    std::printf("test_gs_fx_direct_play\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sc(0x38);
    CHECK(b.gd() == 1);
    b.sc(0xD2);
    d.set_fx_length(1, 0, 256);

    // #80..#83: только номер сэмпла, нота и громкость из заголовка.
    b.sd(0x01);
    CHECK(b.sc(0x82) == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 2);
    CHECK(d.fx_start().handle == 1);
    CHECK(d.fx_start().note == gs::kFxDefaultNote);
    CHECK(d.fx_start().volume == gs::kFxDefaultVolume);

    // #88..#8B: следом нота. Событие созревает на хвосте, а не на коде.
    b.sd(0x01);
    CHECK(b.sc(0x89) == gs::Event::None);
    b.sd(72);
    CHECK(d.take_event() == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 1);
    CHECK(d.fx_start().note == 72);
    CHECK(d.fx_start().volume == gs::kFxDefaultVolume);

    // #90..#93: следом громкость.
    b.sd(0x01);
    CHECK(b.sc(0x90) == gs::Event::None);
    b.sd(0x10);
    CHECK(d.take_event() == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 0);
    CHECK(d.fx_start().note == gs::kFxDefaultNote);
    CHECK(d.fx_start().volume == 0x10);

    // #98..#9B: сперва нота, потом громкость - порядок из руководства.
    b.sd(0x01);
    CHECK(b.sc(0x9B) == gs::Event::None);
    b.sd(48);
    CHECK(d.take_event() == gs::Event::None); // хвост ещё не кончился
    b.sd(0x30);
    CHECK(d.take_event() == gs::Event::FxPlay);
    CHECK(d.fx_start().channel == 3);
    CHECK(d.fx_start().note == 48);
    CHECK(d.fx_start().volume == 0x30);

    // Событие забирается один раз.
    CHECK(d.take_event() == gs::Event::None);

    // Сэмпла нет - запуск молчит, а хвост всё равно съедается.
    b.sd(0x07);
    b.sc(0x98);
    b.sd(40);
    b.sd(0x20);
    CHECK(d.take_event() == gs::Event::None);

    // #A0..#A3 и #A8..#AB: смена на лету, значение аргументом до кода.
    b.sd(80);
    CHECK(b.sc(0xA1) == gs::Event::FxSetNote);
    CHECK(d.fx_start().channel == 1);
    CHECK(d.fx_start().note == 80);
    b.sd(0x11);
    CHECK(b.sc(0xAA) == gs::Event::FxSetVolume);
    CHECK(d.fx_start().channel == 2);
    CHECK(d.fx_start().volume == 0x11);

    // Соседние коды не должны попасть под маску: #84 и #A4 зарезервированы.
    b.sd(0x01);
    CHECK(b.sc(0x84) == gs::Event::None);
    CHECK(d.unknown_command() == 0x84);
    b.sd(0x01);
    b.sc(0xA4);
    CHECK(d.unknown_command() == 0xA4);
}

// #35 - тот же #2A другим кодом, а #2B и #3D ставят одну и ту же громкость
// эффектов: раздельных величин у них в звуке нет.
void test_fx_volume_aliases() {
    std::printf("test_gs_fx_volume_aliases\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sd(0x20);
    b.sc(0x35);
    CHECK(b.gd() == gs::kModuleVolumeMax); // отдаёт прежнюю, как #2A
    CHECK(d.module_volume() == 0x20);

    CHECK(d.fx_volume_effective() == 0x40);
    b.sd(0x10);
    b.sc(0x2B);
    CHECK(d.fx_master_volume() == 0x10);
    CHECK(d.fx_volume_effective() == 0x10);
    b.sd(0x30);
    b.sc(0x3D);
    CHECK(d.fx_volume() == 0x30);
    CHECK(d.fx_master_volume() == 0x10); // своё значение каждая помнит
    CHECK(d.fx_volume_effective() == 0x30);

    CHECK(d.unknown_count() == 0);
}

// --- Расписание MIDI-файла по кадрам ---
//
// Плеер отдаёт карте .mid целиком, а потом раз в кадр просит порцию байт и
// сам выталкивает их в линию с порта A AY. Здесь проверяется то, что
// собирает порции.

// Синтетический .mid, собранный байт за байтом: две дорожки, у каждой имя,
// на первой темп и две ноты, на второй одна нота позже.
struct MidiFile {
    uint8_t bytes[512] = {};
    uint32_t n         = 0;

    void put(uint8_t b) { bytes[n++] = b; }
    void put_str(const char* s) {
        for (const char* p = s; *p != 0; ++p)
            put(static_cast<uint8_t>(*p));
    }
    void put_be16(uint16_t v) {
        put(static_cast<uint8_t>(v >> 8));
        put(static_cast<uint8_t>(v));
    }
    void put_be32(uint32_t v) {
        put(static_cast<uint8_t>(v >> 24));
        put(static_cast<uint8_t>(v >> 16));
        put(static_cast<uint8_t>(v >> 8));
        put(static_cast<uint8_t>(v));
    }
};

void build_synthetic_mid(MidiFile& f, uint16_t division) {
    f.put_str("MThd");
    f.put_be32(6);
    f.put_be16(1); // формат 1
    f.put_be16(2); // две дорожки
    f.put_be16(division);

    // Дорожка 1: имя, темп, нота в такте 0 и её снятие в такте 48.
    f.put_str("MTrk");
    const uint32_t len1_at = f.n;
    f.put_be32(0);
    const uint32_t body1 = f.n;
    f.put(0x00);
    f.put(0xFF);
    f.put(0x03);
    f.put(4);
    f.put_str("LEAD");
    f.put(0x00);
    f.put(0xFF);
    f.put(0x51);
    f.put(3);
    f.put(0x07); // 500000 мкс на долю - те же 120
    f.put(0xA1);
    f.put(0x20);
    f.put(0x00);
    f.put(0x90); // нота взята
    f.put(60);
    f.put(100);
    f.put(48); // через полдоли при 96 PPQN
    f.put(0x80);
    f.put(60);
    f.put(0);
    f.put(0x00);
    f.put(0xFF);
    f.put(0x2F);
    f.put(0);
    const uint32_t len1  = f.n - body1;
    f.bytes[len1_at]     = static_cast<uint8_t>(len1 >> 24);
    f.bytes[len1_at + 1] = static_cast<uint8_t>(len1 >> 16);
    f.bytes[len1_at + 2] = static_cast<uint8_t>(len1 >> 8);
    f.bytes[len1_at + 3] = static_cast<uint8_t>(len1);

    // Дорожка 2: имя и нота в такте 96, со сжатым статусом у второй.
    f.put_str("MTrk");
    const uint32_t len2_at = f.n;
    f.put_be32(0);
    const uint32_t body2 = f.n;
    f.put(0x00);
    f.put(0xFF);
    f.put(0x03);
    f.put(4);
    f.put_str("BASS");
    f.put(96); // через долю
    f.put(0x91);
    f.put(36);
    f.put(90);
    f.put(0x00); // тот же статус опущен - проверка сжатия
    f.put(38);
    f.put(90);
    f.put(0x00);
    f.put(0xFF);
    f.put(0x2F);
    f.put(0);
    const uint32_t len2  = f.n - body2;
    f.bytes[len2_at]     = static_cast<uint8_t>(len2 >> 24);
    f.bytes[len2_at + 1] = static_cast<uint8_t>(len2 >> 16);
    f.bytes[len2_at + 2] = static_cast<uint8_t>(len2 >> 8);
    f.bytes[len2_at + 3] = static_cast<uint8_t>(len2);
}

void test_midi_sched() {
    std::printf("test_gs_midi_sched\n");
    MidiFile f;
    build_synthetic_mid(f, 96);

    static uint8_t work[player::gs::kMidiWorkSize];
    static uint8_t state[gs::kMidiStateBytes];
    gs::MidiFileFacts facts;
    facts.division    = 96;
    facts.first_track = 14;
    facts.track_count = 2;
    facts.file_length = static_cast<uint16_t>(f.n);

    player::gs::MidiSched sched;
    CHECK(sched.begin(f.bytes, f.n, facts, work, state));
    CHECK(sched.track_count() == 2);

    // Имена дорожек - по 24 знака, добиты пробелами.
    const uint8_t* names = sched.names();
    CHECK(names[0] == 'L' && names[1] == 'E' && names[2] == 'A' && names[3] == 'D');
    CHECK(names[4] == ' ');
    CHECK(names[24] == 'B' && names[25] == 'A' && names[26] == 'S' && names[27] == 'S');

    // Справка о файле: первый байт гасит ведущие нули, дальше длина песни
    // в тактах старшим байтом вперёд. Плеер печатает её строкой Total.
    // В нашем файле самая длинная дорожка кончается на такте 96.
    CHECK(sched.total_ticks() == 96);
    CHECK(sched.info()[0] == 0);
    CHECK(sched.info()[1] == 0);
    CHECK(sched.info()[2] == 0);
    CHECK(sched.info()[3] == 96);

    // Первый кадр: нота такта 0 и замыкающий #FF.
    uint16_t n           = sched.next_frame();
    const uint8_t* frame = sched.frame();
    CHECK(n == 4);
    CHECK(frame[0] == 0x90);
    CHECK(frame[1] == 60);
    CHECK(frame[2] == 100);
    CHECK(frame[3] == 0xFF);
    CHECK(sched.tempo_us() == 500000); // темп прочитан из файла

    // 96 PPQN при 120 долях в минуту - 3.84 такта на кадр. Снятие ноты
    // стоит в такте 48, то есть в тринадцатом кадре.
    uint16_t at_off = 0;
    for (uint16_t i = 2; i <= 20; ++i) {
        n = sched.next_frame();
        if (n > 1 && sched.frame()[0] == 0x80) {
            at_off = i;
            break;
        }
    }
    CHECK(at_off == 13);
    CHECK(sched.frame()[1] == 60);

    // Нота второй дорожки в такте 96 - это 25-й кадр; статус у второй её
    // ноты в файле опущен, а в линию обязан уйти.
    uint16_t at_bass = 0;
    for (uint16_t i = 14; i <= 40; ++i) {
        n = sched.next_frame();
        if (n > 1 && sched.frame()[0] == 0x91) {
            at_bass = i;
            break;
        }
    }
    CHECK(at_bass == 25);
    CHECK(n == 7);
    CHECK(sched.frame()[1] == 36);
    CHECK(sched.frame()[2] == 90);
    CHECK(sched.frame()[3] == 0x91); // сжатый статус развёрнут
    CHECK(sched.frame()[4] == 38);
    CHECK(sched.frame()[5] == 90);
    CHECK(sched.frame()[6] == 0xFF);

    // Дальше играть нечего: порции пустые, и это один #FF.
    for (uint16_t i = 0; i < 4; ++i) {
        CHECK(sched.next_frame() == 1);
        CHECK(sched.frame()[0] == 0xFF);
    }
    CHECK(sched.finished());

    // Вдвое быстрее - вдвое раньше. Тот же файл при темпе 250000 мкс.
    MidiFile fast;
    build_synthetic_mid(fast, 96);
    // Темп лежит за MThd (14), заголовком дорожки (8), именем (8) и
    // четырьмя байтами самого события.
    fast.bytes[14 + 8 + 8 + 4] = 0x03; // 250000 = #03D090
    fast.bytes[14 + 8 + 8 + 5] = 0xD0;
    fast.bytes[14 + 8 + 8 + 6] = 0x90;
    player::gs::MidiSched quick;
    CHECK(quick.begin(fast.bytes, fast.n, facts, work, state));
    quick.next_frame();
    CHECK(quick.tempo_us() == 250000);
    uint16_t at_fast = 0;
    for (uint16_t i = 2; i <= 20; ++i) {
        quick.next_frame();
        if (quick.frame()[0] == 0x80) {
            at_fast = i;
            break;
        }
    }
    CHECK(at_fast == 7); // 7.68 такта на кадр: снятие в такте 48 - седьмой

    // Разрешение SMPTE не поддерживаем, и это отказ, а не мусор.
    gs::MidiFileFacts smpte = facts;
    smpte.division          = 0xE278;
    player::gs::MidiSched no;
    CHECK(!no.begin(f.bytes, f.n, smpte, work, state));
    CHECK(!no.ready());
}

// Залитый проигрыватель MIDI обслуживает свои команды: #00 порция кадра,
// #01 блок состояния, #02 готовность, #03 имена дорожек, #04 справка.
// Коды перекрывают обычные #00-#04, поэтому разбираются, только пока блоб
// жив, а чужой код его кончает - так же устроен и сам блоб.
void test_midi_blob_serves() {
    std::printf("test_gs_midi_blob_serves\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    std::ifstream in("ZPlayer/v51/27_gs_midi.bin", std::ios::binary);
    if (!in) {
        std::printf("  нет блоба - SKIP\n");
        return;
    }
    std::vector<uint8_t> body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(body.size(), 2356u);

    MidiFile f;
    build_synthetic_mid(f, 96);

    // Плеер подставляет в тело всё, что блоб знает о файле. Ловим это и
    // проверяем, что поймали верно.
    body[0x62d] = 96; // разрешение
    body[0x62e] = 0;
    body[0x63c] = 0xFF; // седьмое правимое место; что значит, не выяснено
    body[0x76b] = 14;   // первая дорожка сразу за MThd
    body[0x7bc] = 2;    // дорожек
    body[0x853] = static_cast<uint8_t>(f.n & 0xFF);
    body[0x854] = static_cast<uint8_t>(f.n >> 8);

    b.sd(0x00);
    b.sc(0x18); // Load DE = #5000
    b.sd(0x50);

    // Вид потока решается на самой команде #13, а не на хвостовом байте
    // адреса: на хвосте мост уже принял бы файл за модуль, и .mid ушёл бы
    // в разбор трекера.
    for (uint8_t byte : body) {
        b.sd(byte);
        b.sc(0x19);
        b.sc(0x1B);
    }
    b.sd(0x00);
    CHECK(b.sc(0x13) == gs::Event::StreamBegin);
    CHECK(d.stream() == gs::Stream::Midi);
    b.sd(0x50); // хвост с адресом приходит уже после

    CHECK(d.midi_active());
    CHECK(d.midi_facts().division == 96);
    CHECK(d.midi_facts().first_track == 14);
    CHECK(d.midi_facts().track_count == 2);
    CHECK(d.midi_facts().file_length == f.n);

    // Файл идёт потоком сразу после запуска: #D1 плеер не шлёт, блоб
    // начинается с вызова приёмника из ПЗУ карты.
    CHECK(d.stream() == gs::Stream::Midi);
    for (uint32_t i = 0; i < f.n; ++i)
        b.sd(f.bytes[i]);
    CHECK(d.stream_bytes() == f.n);
    b.sc(0xD2);

    // До разбора готовности нет: ноль увёл бы плеер в ожидание таблиц,
    // которых ещё не собрали.
    b.sc(0x02);
    CHECK(b.gd() != 0);

    // Платформа разобрала файл и отдала таблицы.
    static uint8_t work[player::gs::kMidiWorkSize];
    static player::gs::MidiSched sched;
    // Блок состояния автомат держит у себя: первые его байты собираются по
    // аргументу команды, то есть в прерывании.
    CHECK(sched.begin(f.bytes, f.n, d.midi_facts(), work, d.midi_state_buffer()));
    d.set_midi_tables(sched.names(), sched.info());
    d.set_midi_tracks(sched.track_flags(), sched.track_count());
    const uint16_t first = sched.next_frame();
    d.set_midi_frame(sched.frame(), first);
    d.set_midi_ready(true);
    d.hold_parse_done();

    b.sc(0x02);
    CHECK(b.wc());
    CHECK(b.gd() == 0); // теперь блоб "работает"

    // #03: 1536 байт имён, по 24 знака на дорожку.
    b.sc(0x03);
    CHECK(b.wn());
    CHECK(b.gd() == 'L');
    CHECK(b.gd() == 'E');
    uint16_t left = gs::kMidiNamesBytes - 2;
    while (left-- > 1)
        b.gd();
    CHECK(b.gd() == ' '); // последний байт таблицы
    CHECK((d.read_status() & gs::kDataBit) == 0);

    // #04: 84 байта справки, первым разрешение.
    b.sc(0x04);
    CHECK(b.wn());
    CHECK(b.gd() == 0); // ведущие нули гасить
    CHECK(b.gd() == 0); // длина, старший байт
    CHECK(b.gd() == 0);
    CHECK(b.gd() == 96); // младший: наш файл кончается на такте 96
    for (uint16_t i = 4; i < gs::kMidiInfoBytes; ++i)
        b.gd();
    CHECK((d.read_status() & gs::kDataBit) == 0);

    // #01: аргумент идёт ВТОРЫМ байтом в порт команд, а не в порт данных.
    // После него - 135 байт. Байт 21 ложится у плеера в операнд команды
    // "продолжать играть": ноль там уводит его на следующий трек, поэтому
    // пока расписание не кончилось, он обязан быть не ноль.
    b.sc(0x01);
    CHECK((d.read_status() & gs::kDataBit) == 0); // это ещё не ответ
    b.sc(0x00);                                   // аргумент - начало окна списка
    CHECK(b.wn());
    uint8_t block[gs::kMidiStateBytes] = {};
    for (uint16_t i = 0; i < gs::kMidiStateBytes; ++i) {
        block[i] = b.gd();
    }
    CHECK((d.read_status() & gs::kDataBit) == 0);
    CHECK(block[player::gs::kMidiStatePlaying] != 0);
    // Столбик ноты: нота 60 по модулю 36 - это двадцать четвёртый, а
    // высота - скорость 100 пополам.
    CHECK(block[player::gs::kMidiStateBars + (60u % player::gs::kMidiBars)] == 50);
    // Первая дорожка только что взяла ноту - её строка горит. Аргумент
    // был ноль, значит окно начинается с неё, и выше ничего нет.
    CHECK(block[1] != 0);
    CHECK(block[0] == 0);

    // Сдвинем окно на дорожку вперёд: та же дорожка окажется ВЫШЕ окна, и
    // загореться должна стрелка, а не строка.
    b.sc(0x01);
    b.sc(0x01); // аргумент - начало окна
    CHECK(b.wn());
    for (uint16_t i = 0; i < gs::kMidiStateBytes; ++i) {
        block[i] = b.gd();
    }
    CHECK(block[0] != 0);
    CHECK(block[1] == 0);

    // #00: порция кадра, последним байтом #FF.
    const uint32_t served0 = d.midi_frames_served();
    const uint32_t empty0  = d.midi_frames_empty();
    CHECK(d.midi_frame_pending()); // первая порция готова и ещё не забрана
    b.sc(0x00);
    CHECK(!d.midi_frame_pending());
    CHECK(d.midi_frame_wanted());
    CHECK(b.wn());
    CHECK(b.gd() == 0x90); // нота первого такта
    CHECK(b.gd() == 60);
    CHECK(b.gd() == 100);
    CHECK(b.gd() == 0xFF);
    CHECK(d.midi_frames_served() == served0 + 1);
    CHECK(d.take_midi_frame_request()); // платформе готовить следующую
    CHECK(!d.take_midi_frame_request());

    // Пока следующая не готова, та же порция второй раз НЕ уходит: это
    // были бы те же ноты дважды. Пустая порция - пропущенный кадр.
    b.sc(0x00);
    CHECK(b.wn());
    CHECK(b.gd() == 0xFF);
    CHECK(d.midi_frames_empty() == empty0 + 1);

    // Картинка нот: нота 60 (до пятой октавы) - это клавиша 36, третья
    // октава рисунка, первый полутон. Три пикселя подсветки и один
    // полоски; сняли ноту - погасло.
    {
        const uint8_t* rows = sched.state() + player::gs::kMidiStateKeys;
        const uint16_t x    = (36u / 12u) * player::gs::kMidiOctavePx + player::gs::kMidiKeyPixel[0];
        bool lit            = false;
        for (uint16_t d = 0; d < 3; ++d) {
            const uint16_t px = static_cast<uint16_t>(x + d);
            if ((rows[px >> 3] & (0x80u >> (px & 7u))) != 0) lit = true;
        }
        CHECK(lit); // нота первого кадра ещё звучит
        // Белая клавиша - полоска в первом ряду, не во втором.
        const uint16_t mark = static_cast<uint16_t>(x + 1u);
        CHECK((rows[player::gs::kMidiKeyRow + (mark >> 3)] & (0x80u >> (mark & 7u))) != 0);
        CHECK((rows[2u * player::gs::kMidiKeyRow + (mark >> 3)] & (0x80u >> (mark & 7u))) == 0);
    }

    // Положение идёт вперёд: по нему плеер печатает строку Ticks, и
    // стоячее число выглядит как замерший трек.
    const uint8_t* st         = sched.state();
    const uint32_t before_pos = (static_cast<uint32_t>(st[player::gs::kMidiStatePos + 1]) << 16) |
                                (static_cast<uint32_t>(st[player::gs::kMidiStatePos + 2]) << 8) | st[player::gs::kMidiStatePos + 3];
    for (uint16_t i = 0; i < 10; ++i)
        sched.next_frame();
    const uint32_t after_pos = (static_cast<uint32_t>(st[player::gs::kMidiStatePos + 1]) << 16) |
                               (static_cast<uint32_t>(st[player::gs::kMidiStatePos + 2]) << 8) | st[player::gs::kMidiStatePos + 3];
    CHECK(after_pos > before_pos);
    CHECK(st[player::gs::kMidiStateConst] == 0x78);

    // Расписание кончилось - признак гаснет, и плеер уходит сам.
    while (!sched.finished())
        sched.next_frame();
    CHECK(sched.state()[player::gs::kMidiStatePlaying] == 0);

    // Чужой код кончает блоб: дальше команды разбирает прошивка.
    b.sc(0x20); // Get total memory
    CHECK(!d.midi_active());
    CHECK(b.wn());
    b.gd();

    // А вот #02 отвечать не перестаёт, и это нарочно: блоб залит, и если
    // плеер спросит ещё раз, ноль увёл бы его в вечное ожидание таблиц.
    // Настоящая карта тут промолчала бы, но её молчание плеер прочёл бы
    // как случайный байт в регистре вывода.
    b.sc(0x02);
    CHECK(b.wn());
    CHECK(b.gd() != 0);
}

// #2A Set Module Master Volume: SD громкость / SC #2A / WC / [GD старая].
// Z-Player шлёт её на каждое движение регулятора во всех версиях.
void test_module_master_volume() {
    std::printf("test_gs_module_master_volume\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    CHECK(d.module_volume() == gs::kModuleVolumeMax); // включились на полной

    b.sd(0x20);
    b.sc(0x2A);
    CHECK(b.wc());
    // Бит данных снят: карта кладёт прежнюю громкость в регистр вывода, а
    // потом забирает аргумент, и забор бит снимает. Порядок обменов на
    // ПЗУ 1.08 - сперва запись ответа, потом чтение аргумента.
    CHECK(b.wd());
    CHECK(b.gd() == gs::kModuleVolumeMax); // отдаёт прежнюю
    CHECK(d.module_volume() == 0x20);

    // Прежняя теперь #20, и она же ложится в регистр на следующую смену.
    b.sd(0x00);
    b.sc(0x2A);
    CHECK(b.wc());
    CHECK(b.gd() == 0x20);
    CHECK(d.module_volume() == 0x00);

    // Выше потолка обрезается, а не заворачивается.
    b.sd(0xFF);
    b.sc(0x2A);
    CHECK(b.wc());
    CHECK(b.gd() == 0x00);
    CHECK(d.module_volume() == gs::kModuleVolumeMax);

    // Регистр вывода физический: повторное чтение даёт тот же байт.
    CHECK(b.gd() == 0x00);

    // Команда опознана.
    CHECK(d.unknown_count() == 0);
}

// Версию прошивки софт читает так же, как Z-Player: DE = #0004, байт,
// шаг, байт. Печатается как L.H, то есть 1.08. Нули на этих адресах
// дали бы "0.00" - небывалую версию.
void test_rom_version_readable() {
    std::printf("test_gs_rom_version_readable\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sd(0x04);
    b.sc(0x18);
    CHECK(b.wc());
    b.sd(0x00);
    CHECK(b.wd());

    b.sc(0x1A);
    CHECK(b.wc());
    CHECK(b.wn());
    const uint8_t high = b.gd();

    b.sc(0x1B);
    CHECK(b.wc());
    b.sc(0x1A);
    CHECK(b.wc());
    CHECK(b.wn());
    const uint8_t low = b.gd();

    CHECK(high == gs::kRomVersionHigh);
    CHECK(low == gs::kRomVersionLow);
    CHECK(high != 0 || low != 0); // "0.00" не бывает
}

void test_rom_variables_through_de() {
    std::printf("test_gs_rom_variables_through_de\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sd(0x51);
    b.sc(0x18);
    CHECK(b.wc());
    b.sd(0x41);
    CHECK(b.wd());
    b.sc(0x1A);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0xC3);

    // #1B: DE = #4152 - не переменная, ноль.
    b.sc(0x1B);
    CHECK(b.wc());
    b.sc(0x1A);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0x00);

    b.sd(0x9F);
    b.sc(0x18);
    CHECK(b.wc());
    b.sd(0x40);
    CHECK(b.wd());
    b.sc(0x1A);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0x0F);

    // #17 Peek: SD ADR.L / SC / WD / SD ADR.H / GD Byte.
    b.sd(0xA4);
    b.sc(0x17);
    CHECK(b.wd());
    b.sd(0x40);
    CHECK(b.wn());
    CHECK(b.gd() == 0x40);

    CHECK(d.unknown_count() == 0);
}

// --- Аппаратный детект через внутренние порты ---
//
// Самая жёсткая проверка, какую делает софт: два внутренних порта карты
// нарочно связаны с битами состояния, и подделка, которая эту связь не
// воспроизводит, объявляется отсутствующей картой. На этом мы и падали:
// плеер слал #10 дважды, получал "команда не опознана" и писал
// "GS not present".
void test_inner_ports_drive_status_bits() {
    std::printf("test_gs_inner_ports_drive_status_bits\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Порт #0A ставит бит данных в инверсию бита 0 регистра страниц.
    // Гоняем оба значения, чтобы случайное совпадение не прошло.
    for (int bit0 = 0; bit0 <= 1; ++bit0) {
        b.sd(0x00);
        b.sc(0x10);
        CHECK(b.wc());
        b.sd(static_cast<uint8_t>(bit0)); // страницы := bit0
        b.sd(0x0A);
        b.sc(0x10);
        CHECK(b.wc());
        b.sd(0x00); // тронуть #0A
        const bool data_bit = (d.read_status() & gs::kDataBit) != 0;
        CHECK(data_bit == (bit0 == 0));
    }

    // Порт #0B ставит бит команды по биту 5 громкости четвёртого канала.
    for (int v = 0; v <= 1; ++v) {
        const uint8_t vol = static_cast<uint8_t>(v ? 0x20 : 0x00);
        b.sd(0x09);
        b.sc(0x10);
        CHECK(b.wc());
        b.sd(vol);
        b.sd(0x0B);
        b.sc(0x10);
        CHECK(b.wc());
        b.sd(0x00);
        CHECK(((d.read_status() & gs::kCommandBit) != 0) == (v == 1));
    }
    // Через #11 связь порта #0A с битом данных не проверить: чтение должно
    // взвести бит данных, чтобы отдать байт (примитив WN), и он перекрывает
    // эффект порта. Поэтому софт делает пробу записью. Здесь проверяем
    // остальное: #11 отдаёт значение и выставляет бит данных, как любая
    // команда с ответом.
    b.sd(0x00);
    b.sc(0x10);
    CHECK(b.wc());
    b.sd(0x01);
    b.sd(0x0A);
    b.sc(0x11);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0x00);

    // Ни одна из этих команд не должна попасть в неопознанные.
    CHECK(d.unknown_count() == 0);
}

// Значение внутреннего порта должно читаться обратно: софт пишет
// известное и сверяет.
void test_inner_port_roundtrip() {
    std::printf("test_gs_inner_port_roundtrip\n");
    GsDevice d;
    Bus b{d};
    d.reset();
    b.sd(0x07);
    b.sc(0x10);
    CHECK(b.wc());
    b.sd(0x2A); // громкость канала 2 := 0x2A
    b.sd(0x07);
    b.sc(0x11);
    CHECK(b.wc());
    CHECK(b.gd() == 0x2A);
    // #12 - та же запись в регистр страниц, но аргументом до команды.
    b.sd(0x35);
    b.sc(0x12);
    CHECK(b.wc());
    b.sd(0x00);
    b.sc(0x11);
    CHECK(b.wc());
    CHECK(b.gd() == 0x35);
}

// Байт ответа уходит на шину переносом DMA до того, как о чтении узнает
// обработчик, поэтому платформа должна знать его заранее. Здесь
// проверяется контракт, на который она опирается: peek_data() показывает
// то же, что отдаст read_data(), и не двигает указатель.
void test_peek_matches_read() {
    std::printf("test_gs_peek_matches_read\n");
    GsDevice d;
    Bus b{d};
    d.reset();
    b.sc(0x20);
    CHECK(b.wc()); // сколько всего памяти - три байта
    for (int i = 0; i < 3; ++i) {
        CHECK(b.wn());
        const uint8_t seen = d.peek_data();
        CHECK(d.peek_data() == seen); // подглядывание не двигает указатель
        CHECK(b.gd() == seen);
    }
    CHECK((d.read_status() & gs::kDataBit) == 0);
}

// Мост публикует peek_data() после каждого обращения, и Z80 получает этот
// байт, а не результат read_data(): на любом разговоре они обязаны
// совпасть, а подглядывание - не менять состояние. Случайные разговоры с
// фиксированным зерном: команды, данные, чтения, удержание, сброс.
void test_peek_equals_read_fuzz() {
    std::printf("test_gs_peek_equals_read_fuzz\n");
    uint32_t x = 20260913u;
    auto next  = [&x]() {
        x = x * 1664525u + 1013904223u;
        return x >> 16;
    };
    uint32_t reads = 0, mismatches = 0, moved = 0;
    for (int conv = 0; conv < 200; ++conv) {
        GsDevice d;
        d.reset();
        for (int op = 0; op < 5000; ++op) {
            const uint32_t r   = next();
            const uint8_t byte = static_cast<uint8_t>(r >> 4);
            switch (r & 15u) {
                case 0:
                case 1:
                case 2:
                case 3:
                    (void)d.write_command(byte);
                    break;
                case 4:
                case 5:
                case 6:
                    (void)d.write_data(byte);
                    break;
                case 13:
                    d.hold_reserve_begin();
                    break;
                case 14:
                    d.hold_reserve_done();
                    break;
                case 15:
                    if (byte < 4)
                        d.reset();
                    else
                        d.set_position(byte, static_cast<uint8_t>(byte & 63u));
                    break;
                default: {
                    const uint8_t st   = d.read_status();
                    const uint8_t seen = d.peek_data();
                    if (d.peek_data() != seen || d.read_status() != st) ++moved;
                    if (d.read_data() != seen) ++mismatches;
                    ++reads;
                    break;
                }
            }
        }
    }
    std::printf("  reads %u, peek != read %u, peek moved the state %u\n", reads, mismatches, moved);
    CHECK(reads > 300000u);
    CHECK_EQ(mismatches, 0u);
    CHECK_EQ(moved, 0u);
}

// Разговор Z-Player 4.1 с автоматом за 2000 кадров: эмулятор Z80 гонял
// плеер с этим GsDevice, мост без задержки (ответ публикуется сразу после
// обращения). На обращение два байта: вид (0 - команда, 1 - данные, 2 -
// чтение состояния, 3 - чтение данных) и значение; у чтений - что получил
// плеер. Воспроизведение сверяет каждое чтение: расхождение - изменение
// поведения для настоящего плеера, журнал переписывается только
// сознательно. Плеер доходит до главного цикла.
//
// Журнал снимается стендом `zp_gs_run --dump` и переписывается только
// прогоном. Пробы опознания карты (тип, процессор, частота) получают ответы
// подмен, неопознанных команд нет; по нулю от пробы клона плеер проверяет
// шлейф командой #10.
void test_zplayer_session_replay() {
    std::printf("test_gs_zplayer_session_replay\n");
    std::ifstream in("backend/ports/pc/tests/data/zplayer41_gs.bin", std::ios::binary);
    if (!in) {
        std::printf("  no log - SKIP\n");
        return;
    }
    const std::vector<uint8_t> log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    GsDevice d;
    d.reset();
    uint32_t reads = 0, bad = 0, first_bad = 0;
    for (uint32_t i = 0; i + 1 < log.size(); i += 2) {
        const uint8_t v = log[i + 1];
        uint8_t got     = v;
        switch (log[i]) {
            case 0:
                (void)d.write_command(v);
                break;
            case 1:
                (void)d.write_data(v);
                break;
            case 2:
                got = d.read_status();
                ++reads;
                break;
            default:
                got = d.peek_data();
                (void)d.read_data();
                ++reads;
                break;
        }
        if (got != v && bad++ == 0) first_bad = i / 2;
    }
    std::printf("  accesses %u, reads %u, differences %u\n", static_cast<unsigned>(log.size() / 2), reads, bad);
    if (bad) std::printf("  first difference at access %u\n", first_bad);
    CHECK_EQ(log.size(), 44212u);
    CHECK_EQ(bad, 0u);
    CHECK_EQ(d.unknown_command(), 0x00u);
    CHECK_EQ(d.unknown_count(), 0u);
}

// --- Детект целевого плеера, снятый с живой шины ---
//
// Это четырнадцать обращений, записанных с платы, когда плеер отвечал
// "GS not present". В логе (C команда, S состояние, d данные от ZX,
// r данные к ZX):
//
//   d02 rFF d01 rFF C00 S7E d03 C10 dAA rFF d03 C10 d55 rFF
//
// Смысл виден с конца: плеер кладёт 0xAA в регистр вывода (порт
// связи #03) и читает порт данных, ожидая своё значение обратно, потом
// повторяет с 0x55. Два разных байта - чтобы не прошло случайное
// совпадение. Обе пробы возвращали 0xFF, потому что запись во
// внутренний порт никуда не вела.
void test_detect_sequence_from_real_player() {
    std::printf("test_gs_detect_sequence_from_real_player\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sd(0x02);
    (void)b.gd();
    b.sd(0x01);
    (void)b.gd();
    b.sc(0x00);
    CHECK((d.read_status() & gs::kCommandBit) == 0); // карта отозвалась

    b.sd(0x03);
    b.sc(0x10);
    b.sd(0xAA);
    CHECK(b.gd() == 0xAA);
    b.sd(0x03);
    b.sc(0x10);
    b.sd(0x55);
    CHECK(b.gd() == 0x55);

    CHECK(d.unknown_count() == 0);
}

// Остальные порты связи - их читают командой #11.
void test_link_ports_report_traffic() {
    std::printf("test_gs_link_ports_report_traffic\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    // Порт #02 отдаёт последний байт, пришедший в регистр данных, а это сам
    // номер порта: аргумент команды #11 идёт туда же. Так устроено железо -
    // регистр один.
    b.sd(0x02);
    b.sc(0x11);
    CHECK(b.wc());
    CHECK(b.gd() == 0x02); // порт #02
    // Порт #01 отдаёт код последней команды - ею и оказывается #11.
    b.sd(0x01);
    b.sc(0x11);
    CHECK(b.wc());
    CHECK(b.gd() == 0x11); // порт #01
    // Порт #04 - слово состояния в момент чтения, то есть до того, как ответ
    // на саму #11 взвёл бит данных. Порядок такой, и подменять его "удобным"
    // значением нельзя: софт по этому порту судит о карте.
    b.sd(0x04);
    b.sc(0x11);
    CHECK(b.wc());
    CHECK(b.gd() == gs::kStatusIdle);

    // Порт #05 снимает бит команд - им карта отчитывается о готовности.
    b.sd(0x09);
    b.sc(0x10);
    CHECK(b.wc());
    b.sd(0x20); // бит 5 громкости 4
    b.sd(0x0B);
    b.sc(0x10);
    b.sd(0x00); // взвести бит команд
    CHECK((d.read_status() & gs::kCommandBit) != 0);
    b.sd(0x05);
    b.sc(0x10);
    b.sd(0x00);
    CHECK((d.read_status() & gs::kCommandBit) == 0);
}

// --- Настройки плеера, снятые с живой шины ---
//
// Целевой плеер сразу после опроса памяти шлёт:
//
//   C23 r3F d01 C6A d00 C6B CF3
//
// то есть спрашивает число страниц, включает режим плеера (#6A),
// настраивает релупер (#6B) и делает тёплый рестарт. Обе команды описаны
// в позднем дополнении руководства, в основном списке их нет. На шине
// неопознанная команда от принятой не отличается, разбор нужен для меры
// покрытия (unknown_count) и чтобы съесть аргументы #6B.
void test_player_mode_and_relooper() {
    std::printf("test_gs_player_mode_and_relooper\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sc(0x23);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == gs::kDeclaredPages); // объявляемся двухмегабайтной картой

    b.sd(0x01);
    b.sc(0x6A);
    CHECK(b.wc());
    b.sd(0x00);
    b.sc(0x6B);
    CHECK(b.wc());
    b.sc(0xF3);
    CHECK(b.wc()); // тёплый рестарт, старший байт длины не пришёл

    // Старший байт длины, если пришёл, съедается хвостом #6B и в
    // аргументы следующей команды не попадает.
    b.sd(0x40);
    b.sc(0x6B);
    b.sd(0x01);
    CHECK(pages_roundtrip(b, 0x29));

    CHECK(d.unknown_count() == 0);
}

// --- Обратная связь плееру ---
//
// Целевой плеер спрашивает позицию каждое прерывание и по ней двигает
// картинку. Отдельно проверяется упаковка #62: биты 7-6 - младшие два
// бита позиции, биты 5-0 - строка (руководство, #62).
void test_position_feedback() {
    std::printf("test_gs_position_feedback\n");
    GsDevice d;
    d.reset();
    Bus b{d};

    d.set_position(0x27, 0x15);
    b.sc(0x60);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0x27);
    b.sc(0x61);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0x15);
    b.sc(0x62);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == static_cast<uint8_t>(((0x27u & 3u) << 6) | 0x15u));

    // Строка шире шести бит в упаковку не влезает, старшее отбрасывается;
    // потери нет: у MOD в паттерне 64 строки.
    d.set_position(0x03, 0x3F);
    b.sc(0x62);
    CHECK(b.wc());
    CHECK(b.wn());
    CHECK(b.gd() == 0xFF);

    CHECK(d.unknown_count() == 0);
}

} // namespace

void run_gs_device_tests() {
    test_detect_like_real_software();
    test_status_never_sets_command_bit();
    test_memory_reports_three_bytes();
    test_data_on_roundtrip();
    test_busy_flag_visible_in_hx();
    test_arguments_are_taken_before_command();
    test_unknown_commands_are_counted();
    test_reset_notifies_outside();
    test_module_stream_events();
    test_hold_survives_response_and_read();
    test_two_holds_are_independent();
    test_card_memory_commands_are_stubbed();
    test_loader_blob_opens_stream();
    test_card_identity_blobs();
    test_resident_telemetry();
    test_resident_pattern_from_module();
    test_fx_sample_load();
    test_fx_parameters();
    test_fx_play_and_stop();
    test_fx_player_renders();
    test_fx_direct_play();
    test_fx_volume_aliases();
    test_midi_sched();
    test_midi_blob_serves();
    test_data_write_raises_data_bit();
    test_midi_blob_probe_refuses();
    test_module_master_volume();
    test_rom_version_readable();
    test_rom_variables_through_de();
    test_inner_ports_drive_status_bits();
    test_inner_port_roundtrip();
    test_peek_matches_read();
    test_peek_equals_read_fuzz();
    test_zplayer_session_replay();
    test_player_mode_and_relooper();
    test_position_feedback();
    test_detect_sequence_from_real_player();
    test_link_ports_report_traffic();
}
