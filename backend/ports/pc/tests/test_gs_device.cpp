#include "testing.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "devices/gs/gs_device.h"

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
    // #D2 после #30 держит бит удержанием разбора - здесь
    // потока нет, #D2 без #30 не удерживает.
    for (uint16_t cmd = 0; cmd <= 0xFF; ++cmd) {
        d.write_command(static_cast<uint8_t>(cmd));
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
    const uint32_t hi = b.gd();
    const uint32_t total = lo | (mid << 8) | (hi << 16);
    std::printf("  всего памяти: %u байт\n", total);
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
    CHECK(!b.wc());          // разбор идёт, плеер ждёт
    d.hold_parse_done();
    CHECK(b.wc());
    CHECK(!b.sd(0x00));      // поток закрыт

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
    CHECK((d.read_status() & gs::kCommandBit) != 0);    // придержан
    CHECK((d.read_status() & gs::kDataBit) != 0);       // номер модуля готов
    CHECK(b.gd() == 1);                                  // GD handle
    CHECK((d.read_status() & gs::kCommandBit) != 0);    // чтение не сняло
    d.reset();                                           // и сброс карты не снимает
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
    b.sd(0x00); b.sc(0x13); CHECK(b.wc()); CHECK(!b.sd(0x80)); CHECK(b.wd());
    CHECK(pages_roundtrip(b, 0x21));

    // #14 Load block, LEN = 0x0103: хвост из трёх байт и 259 байт блока.
    b.sd(0x03); b.sc(0x14);
    b.sd(0x01); b.sd(0x00); b.sd(0x80);                  // LEN.H, ADR.L, ADR.H
    for (uint32_t i = 0; i < 0x103u; ++i) {
        CHECK(!b.sd(static_cast<uint8_t>(0xA0u + i)));
        CHECK(b.wd());
    }
    CHECK(pages_roundtrip(b, 0x22));

    // #15 Get block, LEN = 2: после хвоста два нуля, потом бит данных снят.
    b.sd(0x02); b.sc(0x15);
    b.sd(0x00); b.sd(0x00); b.sd(0x80);
    CHECK(b.wn()); CHECK(d.peek_data() == 0); CHECK(b.gd() == 0);
    CHECK(b.wn()); CHECK(b.gd() == 0);
    CHECK((d.read_status() & gs::kDataBit) == 0);
    CHECK(pages_roundtrip(b, 0x23));

    // #16 Poke: SD Byte / SC / WC / SD ADR.L / WD / SD ADR.H / WD.
    b.sd(0x55); b.sc(0x16); CHECK(b.wc()); b.sd(0x00); CHECK(b.wd()); b.sd(0x80); CHECK(b.wd());
    CHECK(pages_roundtrip(b, 0x24));

    // #17 Peek: SD ADR.L / SC / WD / SD ADR.H / GD Byte.
    b.sd(0x00); b.sc(0x17); CHECK(b.wd()); b.sd(0x80);
    CHECK(b.wn()); CHECK(b.gd() == 0);
    CHECK(pages_roundtrip(b, 0x25));

    // #18 Load DE, #19 Poke (DE), #1A Peek (DE), #1B Inc DE.
    b.sd(0x00); b.sc(0x18); CHECK(b.wc()); b.sd(0x80); CHECK(b.wd());
    b.sd(0x55); b.sc(0x19); CHECK(b.wc());
    b.sc(0x1A); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0);
    b.sc(0x1B); CHECK(b.wc());
    CHECK(pages_roundtrip(b, 0x26));

    // #1C Poke (#20XX): SD ADR.L / SC / WC / SD Byte. #1D Peek (#20XX).
    b.sd(0x10); b.sc(0x1C); CHECK(b.wc()); b.sd(0x55); CHECK(b.wd());
    b.sd(0x10); b.sc(0x1D); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0);
    CHECK(pages_roundtrip(b, 0x27));

    // Недособранный хвост бросает следующая команда: #14 с одним байтом
    // хвоста из трёх, затем #1B.
    b.sd(0x05); b.sc(0x14); b.sd(0x01);
    b.sc(0x1B); CHECK(b.wc());
    CHECK(pages_roundtrip(b, 0x28));

    CHECK(d.unknown_count() == 0);
}

// Переменные ПЗУ карты через DE: Z-Player 4.1 читает #4151 (#18 DE=#4151,
// #1A) и при нуле встаёт навсегда; настоящее ПЗУ пишет туда #C3. #1B
// двигает DE, #17 читает по адресу из аргумента и хвоста.
void test_rom_variables_through_de() {
    std::printf("test_gs_rom_variables_through_de\n");
    GsDevice d;
    Bus b{d};
    d.reset();

    b.sd(0x51); b.sc(0x18); CHECK(b.wc()); b.sd(0x41); CHECK(b.wd());
    b.sc(0x1A); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0xC3);

    // #1B: DE = #4152 - не переменная, ноль.
    b.sc(0x1B); CHECK(b.wc());
    b.sc(0x1A); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0x00);

    b.sd(0x9F); b.sc(0x18); CHECK(b.wc()); b.sd(0x40); CHECK(b.wd());
    b.sc(0x1A); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0x0F);

    // #17 Peek: SD ADR.L / SC / WD / SD ADR.H / GD Byte.
    b.sd(0xA4); b.sc(0x17); CHECK(b.wd()); b.sd(0x40);
    CHECK(b.wn()); CHECK(b.gd() == 0x40);

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
        b.sd(0x00); b.sc(0x10); CHECK(b.wc()); b.sd(static_cast<uint8_t>(bit0)); // страницы := bit0
        b.sd(0x0A); b.sc(0x10); CHECK(b.wc()); b.sd(0x00);                       // тронуть #0A
        const bool data_bit = (d.read_status() & gs::kDataBit) != 0;
        CHECK(data_bit == (bit0 == 0));
    }

    // Порт #0B ставит бит команды по биту 5 громкости четвёртого канала.
    for (int v = 0; v <= 1; ++v) {
        const uint8_t vol = static_cast<uint8_t>(v ? 0x20 : 0x00);
        b.sd(0x09); b.sc(0x10); CHECK(b.wc()); b.sd(vol);
        b.sd(0x0B); b.sc(0x10); CHECK(b.wc()); b.sd(0x00);
        CHECK(((d.read_status() & gs::kCommandBit) != 0) == (v == 1));
    }
    // Через #11 связь порта #0A с битом данных не проверить: чтение должно
    // взвести бит данных, чтобы отдать байт (примитив WN), и он перекрывает
    // эффект порта. Поэтому софт делает пробу записью. Здесь проверяем
    // остальное: #11 отдаёт значение и выставляет бит данных, как любая
    // команда с ответом.
    b.sd(0x00); b.sc(0x10); CHECK(b.wc()); b.sd(0x01);
    b.sd(0x0A); b.sc(0x11); CHECK(b.wc());
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
    b.sd(0x07); b.sc(0x10); CHECK(b.wc()); b.sd(0x2A); // громкость канала 2 := 0x2A
    b.sd(0x07); b.sc(0x11); CHECK(b.wc());
    CHECK(b.gd() == 0x2A);
    // #12 - та же запись в регистр страниц, но аргументом до команды.
    b.sd(0x35); b.sc(0x12); CHECK(b.wc());
    b.sd(0x00); b.sc(0x11); CHECK(b.wc());
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
    b.sc(0x20); CHECK(b.wc()); // сколько всего памяти - три байта
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
    auto next = [&x]() {
        x = x * 1664525u + 1013904223u;
        return x >> 16;
    };
    uint32_t reads = 0, mismatches = 0, moved = 0;
    for (int conv = 0; conv < 200; ++conv) {
        GsDevice d;
        d.reset();
        for (int op = 0; op < 5000; ++op) {
            const uint32_t r = next();
            const uint8_t byte = static_cast<uint8_t>(r >> 4);
            switch (r & 15u) {
                case 0: case 1: case 2: case 3: (void)d.write_command(byte); break;
                case 4: case 5: case 6: (void)d.write_data(byte); break;
                case 13: d.hold_reserve_begin(); break;
                case 14: d.hold_reserve_done(); break;
                case 15:
                    if (byte < 4) d.reset();
                    else d.set_position(byte, static_cast<uint8_t>(byte & 63u));
                    break;
                default: {
                    const uint8_t st = d.read_status();
                    const uint8_t seen = d.peek_data();
                    if (d.peek_data() != seen || d.read_status() != st) ++moved;
                    if (d.read_data() != seen) ++mismatches;
                    ++reads;
                    break;
                }
            }
        }
    }
    std::printf("  чтений %u, peek != read %u, peek сдвинул состояние %u\n", reads, mismatches, moved);
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
// сознательно. Плеер доходит до главного цикла; трижды шлёт #FC, которой
// нет.
void test_zplayer_session_replay() {
    std::printf("test_gs_zplayer_session_replay\n");
    std::ifstream in("backend/ports/pc/tests/data/zplayer41_gs.bin", std::ios::binary);
    if (!in) {
        std::printf("  журнала нет — ПРОПУСК\n");
        return;
    }
    const std::vector<uint8_t> log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    GsDevice d;
    d.reset();
    uint32_t reads = 0, bad = 0, first_bad = 0;
    for (uint32_t i = 0; i + 1 < log.size(); i += 2) {
        const uint8_t v = log[i + 1];
        uint8_t got = v;
        switch (log[i]) {
            case 0: (void)d.write_command(v); break;
            case 1: (void)d.write_data(v); break;
            case 2: got = d.read_status(); ++reads; break;
            default:
                got = d.peek_data();
                (void)d.read_data();
                ++reads;
                break;
        }
        if (got != v && bad++ == 0) first_bad = i / 2;
    }
    std::printf("  обращений %u, чтений %u, расхождений %u\n", static_cast<unsigned>(log.size() / 2), reads, bad);
    if (bad) std::printf("  первое расхождение на обращении %u\n", first_bad);
    CHECK_EQ(log.size(), 44196u);
    CHECK_EQ(bad, 0u);
    CHECK_EQ(d.unknown_command(), 0xFCu);
    CHECK_EQ(d.unknown_count(), 3u);
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

    b.sd(0x02); (void)b.gd();
    b.sd(0x01); (void)b.gd();
    b.sc(0x00);
    CHECK((d.read_status() & gs::kCommandBit) == 0); // карта отозвалась

    b.sd(0x03); b.sc(0x10); b.sd(0xAA);
    CHECK(b.gd() == 0xAA);
    b.sd(0x03); b.sc(0x10); b.sd(0x55);
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
    b.sd(0x02); b.sc(0x11); CHECK(b.wc()); CHECK(b.gd() == 0x02); // порт #02
    // Порт #01 отдаёт код последней команды - ею и оказывается #11.
    b.sd(0x01); b.sc(0x11); CHECK(b.wc()); CHECK(b.gd() == 0x11); // порт #01
    // Порт #04 - слово состояния в момент чтения, то есть до того, как ответ
    // на саму #11 взвёл бит данных. Порядок такой, и подменять его "удобным"
    // значением нельзя: софт по этому порту судит о карте.
    b.sd(0x04); b.sc(0x11); CHECK(b.wc()); CHECK(b.gd() == gs::kStatusIdle);

    // Порт #05 снимает бит команд - им карта отчитывается о готовности.
    b.sd(0x09); b.sc(0x10); CHECK(b.wc()); b.sd(0x20); // бит 5 громкости 4
    b.sd(0x0B); b.sc(0x10); b.sd(0x00);                // взвести бит команд
    CHECK((d.read_status() & gs::kCommandBit) != 0);
    b.sd(0x05); b.sc(0x10); b.sd(0x00);
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

    b.sc(0x23); CHECK(b.wc()); CHECK(b.wn());
    CHECK(b.gd() == gs::kDeclaredPages);   // объявляемся двухмегабайтной картой

    b.sd(0x01); b.sc(0x6A); CHECK(b.wc());
    b.sd(0x00); b.sc(0x6B); CHECK(b.wc());
    b.sc(0xF3); CHECK(b.wc());             // тёплый рестарт, старший байт длины не пришёл

    // Старший байт длины, если пришёл, съедается хвостом #6B и в
    // аргументы следующей команды не попадает.
    b.sd(0x40); b.sc(0x6B); b.sd(0x01);
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
    b.sc(0x60); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0x27);
    b.sc(0x61); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0x15);
    b.sc(0x62); CHECK(b.wc()); CHECK(b.wn());
    CHECK(b.gd() == static_cast<uint8_t>(((0x27u & 3u) << 6) | 0x15u));

    // Строка шире шести бит в упаковку не влезает, старшее отбрасывается;
    // потери нет: у MOD в паттерне 64 строки.
    d.set_position(0x03, 0x3F);
    b.sc(0x62); CHECK(b.wc()); CHECK(b.wn()); CHECK(b.gd() == 0xFF);

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
