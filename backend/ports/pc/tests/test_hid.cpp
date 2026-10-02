// SPDX-License-Identifier: MIT
// Страницы ответов клавиатуры и мыши: то, что плата отдаёт на чтение порта,
// не считая ничего в цикле шины. Индекс страницы - старшая половина адреса,
// её же дописывает склейщик; здесь проверяется, что по этому индексу лежит
// верный байт.

#include "testing.h"

#include <cstdio>
#include <cstring>

#include "devices/hal/host_signals.h"
#include "devices/hid/joystick.h"
#include "devices/hid/keyboard.h"
#include "devices/hid/mouse.h"
#include "devices/hid/report_map.h"
#include "devices/hid/usb_map.h"

using namespace devices::hid;

// Счётчики подставной таблицы ответов (test_sd_spi_emu.cpp): страницы
// ставятся и снимаются там, а проверяет их тест клавиатуры.
extern uint32_t g_port_page_sets[256];
extern uint32_t g_port_clears[256];
extern const uint8_t* g_port_last_page;

// Линий сброса и NMI на ПК нет, поэтому контракт закрывается счётчиками:
// проверяется, что сигнал уходит на нажатие и не повторяется, пока
// клавиша держится.
namespace {
int s_reset_requests  = 0;
int s_nmi_requests    = 0;
int s_hard_requests   = 0;
int s_config_requests = 0;
} // namespace

void devices::hal::host_reset_request() {
    ++s_reset_requests;
}

void devices::hal::host_nmi_request() {
    ++s_nmi_requests;
}

void devices::hal::host_hard_reset_request() {
    ++s_hard_requests;
}

void devices::hal::host_configurator_request() {
    ++s_config_requests;
}

namespace {

// Как читает машина: в старшей половине адреса нули на выбранных
// полустроках.
uint8_t kbd_read(uint8_t hi) {
    return keyboard_page()[hi];
}

void keyboard_tests() {
    keyboard_reset();

    // Ничего не нажато - любая выборка отвечает поднятыми разрядами.
    CHECK_EQ(kbd_read(0xFE), 0xFF);
    CHECK_EQ(kbd_read(0x00), 0xFF);
    CHECK_EQ(kbd_read(0x7F), 0xFF);

    // CAPS SHIFT - полустрока A8, разряд 0.
    keyboard_set_key(0, 0, true);
    CHECK_EQ(kbd_read(0xFE), 0xFE); // A8 опущен - клавиша видна
    CHECK_EQ(kbd_read(0xFD), 0xFF); // выбрана другая полустрока
    CHECK_EQ(kbd_read(0x00), 0xFE); // все полустроки разом

    // Разряды 5..7 не гаснут ни при какой нажатой клавише.
    keyboard_set_key(0, 4, true);
    CHECK_EQ(static_cast<uint8_t>(kbd_read(0xFE) & kKeyboardIdleBits), kKeyboardIdleBits);
    CHECK_EQ(kbd_read(0xFE), 0xEE);

    // Объединение: клавиши из разных полустрок складываются И.
    keyboard_reset();
    keyboard_set_key(3, 0, true);   // "1", полустрока A11
    keyboard_set_key(4, 0, true);   // "0", полустрока A12
    CHECK_EQ(kbd_read(0xF7), 0xFE); // только A11
    CHECK_EQ(kbd_read(0xEF), 0xFE); // только A12
    CHECK_EQ(kbd_read(0xE7), 0xFE); // обе сразу, клавиши в одном разряде
    CHECK_EQ(kbd_read(0xFB), 0xFF); // ни одной из них

    keyboard_set_key(3, 1, true); // "2"
    CHECK_EQ(kbd_read(0xF7), 0xFC);
    CHECK_EQ(kbd_read(0xE7), 0xFC); // "0" в разряде 0 уже опущен

    // Отпускание возвращает разряд.
    keyboard_set_key(3, 0, false);
    keyboard_set_key(3, 1, false);
    CHECK_EQ(kbd_read(0xF7), 0xFF);

    // Матрица целиком: старшие разряды строк дописываются сами.
    const uint8_t rows[kKeyboardRows] = {0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x0F};
    keyboard_set_rows(rows);
    CHECK_EQ(kbd_read(0xFE), 0xFE);
    CHECK_EQ(kbd_read(0x7F), 0xEF);
    CHECK_EQ(kbd_read(0x7E), 0xEE);

    keyboard_reset();
    CHECK_EQ(kbd_read(0x7E), 0xFF);

    // Страница выровнена на 256: склейщик дописывает младший байт адреса, а
    // не складывает, и страница со сдвигом отвечала бы чужими байтами.
    CHECK_EQ(reinterpret_cast<uintptr_t>(keyboard_page()) & 0xFFu, 0u);
}

void mouse_tests() {
    mouse_reset();

    CHECK_EQ(mouse_page()[kMouseHiX], 0x00);
    CHECK_EQ(mouse_page()[kMouseHiY], 0x00);
    CHECK_EQ(mouse_page()[kMouseHiButtons], kMouseButtonsIdle);

    // Незанятые сочетания старшего байта отвечают как неподведённая шина.
    CHECK_EQ(mouse_page()[0x00], 0xFF);
    CHECK_EQ(mouse_page()[0xFC], 0xFF);

    mouse_move(5, -3);
    CHECK_EQ(mouse_page()[kMouseHiX], 5);
    CHECK_EQ(mouse_page()[kMouseHiY], 253);

    // Счётчики бегут по кругу: программа следит за переполнением сама.
    mouse_move(-10, 10);
    CHECK_EQ(mouse_page()[kMouseHiX], 251);
    CHECK_EQ(mouse_page()[kMouseHiY], 7);

    // Разряды: 0 левая, 1 правая, 2 средняя; ноль - нажата.
    mouse_set_buttons(static_cast<uint8_t>(kMouseButtonsIdle & ~kMouseButtonLeft));
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xFE);
    // Кнопки лежат отдельно от координат.
    CHECK_EQ(mouse_page()[kMouseHiX], 251);

    mouse_set_buttons(static_cast<uint8_t>(kMouseButtonsIdle & ~kMouseButtonRight));
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xFD);

    mouse_set_buttons(kMouseButtonsIdle);
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xFF);

    // Колесо - старшая половина байта. До первого щелчка она в единицах,
    // поэтому шаг вверх её обнуляет.
    mouse_wheel(1);
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0x0F);
    mouse_wheel(-1);
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xFF);
    mouse_wheel(-1);
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xEF);
    // Нажатие и щелчок не затирают друг друга.
    mouse_set_buttons(static_cast<uint8_t>(kMouseButtonsIdle & ~kMouseButtonMiddle));
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xEB);
    mouse_wheel(1);
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xFB);
    mouse_set_buttons(kMouseButtonsIdle);
    CHECK_EQ(mouse_page()[kMouseHiButtons], 0xFF);

    CHECK_EQ(reinterpret_cast<uintptr_t>(mouse_page()) & 0xFFu, 0u);
}

void joystick_tests() {
    joystick_reset();
    CHECK_EQ(joystick_state(), 0);

    joystick_set(kJoyUp | kJoyFire);
    CHECK_EQ(joystick_state(), static_cast<uint8_t>(kJoyUp | kJoyFire));

    // Противоположные направления разом не отдаются.
    joystick_set(kJoyLeft | kJoyRight);
    CHECK_EQ(joystick_state(), 0);
    joystick_set(kJoyUp | kJoyDown | kJoyFire);
    CHECK_EQ(joystick_state(), kJoyFire);

    joystick_reset();
    CHECK_EQ(joystick_state(), 0);
}

// --- Дескрипторы, на которых проверяется разбор ---

// clang-format off

// Мышь С НОМЕРАМИ ОТЧЁТОВ - та самая беда, которую видно на плате:
// комбо-приёмник шлёт шесть байт, первый из них номер, и раскладка
// загрузочной мыши к нему неприменима.
const uint8_t kMouseWithId[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01,
    0x81, 0x02,       //     Input - три кнопки по разряду
    0x95, 0x01, 0x75, 0x05,
    0x81, 0x01,       //     Input (Constant) - добивка до байта
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02,
    0x81, 0x06,       //     Input - X и Y по байту со знаком
    0xC0,
    0xC0
};

// Геймпад: оси 0..255, шляпка на восемь положений, восемь кнопок.
// Раскладка нарочно не такая, как у мыши: X в нулевом байте, кнопки в
// третьем. Жёсткие смещения тут ошиблись бы, разбор - нет.
const uint8_t kGamepad[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x02,
    0x81, 0x02,       //     Input - X и Y, 0..255
    0xC0,
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00, 0x25, 0x07,
    0x75, 0x04, 0x95, 0x01,
    0x81, 0x42,       //   Input с нулевым состоянием
    0x75, 0x04, 0x95, 0x01,
    0x81, 0x01,       //   Input (Constant) - добивка полубайта
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01, 0x29, 0x08,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,       //   Input - восемь кнопок
    0xC0
};
// clang-format on

// Загрузочный отчёт клавиатуры: модификаторы, запас, шесть кодов.
void kbd_report(uint8_t mods, uint8_t a = 0, uint8_t b = 0) {
    const ReportMap none;
    const uint8_t r[8] = {mods, 0, a, b, 0, 0, 0, 0};
    usb_map_keyboard(none, r, sizeof(r));
}

void usb_map_tests() {
    keyboard_reset();

    // A - полустрока A9, разряд 0.
    kbd_report(0, 0x04);
    CHECK_EQ(keyboard_page()[0xFD], 0xFE);
    CHECK_EQ(keyboard_page()[0xFE], 0xFF);

    // Отпускание приходит отчётом без кода.
    kbd_report(0);
    CHECK_EQ(keyboard_page()[0xFD], 0xFF);

    // Shift - это CAPS SHIFT, полустрока A8.
    kbd_report(0x02);
    CHECK_EQ(keyboard_page()[0xFE], 0xFE);
    // Ctrl - SYMBOL SHIFT, полустрока A15 разряд 1.
    kbd_report(0x01);
    CHECK_EQ(keyboard_page()[0x7F], 0xFD);

    // Стрелка влево - CAPS SHIFT и 5.
    kbd_report(0, 0x50);
    CHECK_EQ(keyboard_page()[0xFE], 0xFE);
    CHECK_EQ(keyboard_page()[0xF7], 0xEF);
    CHECK_EQ(keyboard_page()[0xF6], 0xEE);

    // Больше шести клавиш: матрица не отдаётся, состояние прежнее.
    const ReportMap none;
    const uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};
    usb_map_keyboard(none, rollover, sizeof(rollover));
    CHECK_EQ(keyboard_page()[0xFE], 0xFE);

    kbd_report(0);
    CHECK_EQ(keyboard_page()[0x00], 0xFF);
}

void mouse_boot_tests() {
    // Загрузочная мышь приходит такой же картой, как разобранная, - жёстких
    // раскладок в переводе не осталось ни одной.
    ReportMap map;
    report_map_boot_mouse(map);
    CHECK_EQ(static_cast<int>(map.button_count), 3);
    CHECK(map.axis[static_cast<uint8_t>(Axis::X)].present());

    mouse_reset();
    const uint8_t m[4] = {0x01, 5, 3, 0};
    usb_map_mouse(map, m, sizeof(m));
    CHECK_EQ(mouse_page()[kMouseHiButtons], static_cast<uint8_t>(0xFF & ~kMouseButtonLeft));
    CHECK_EQ(mouse_page()[kMouseHiX], 5);
    CHECK_EQ(mouse_page()[kMouseHiY], 253); // Y у Kempston растёт вверх
}

// Скорость и перестановка кнопок из настроек. Скорость считается в
// четвертях до обрезки байтом: иначе быстрое движение заворачивалось бы
// вместо упора в предел счётчика.
void mouse_tuning_tests() {
    ReportMap map;
    report_map_boot_mouse(map);

    // Вдвое быстрее: приращение удваивается.
    usb_map_set_mouse(MouseTuning{8, false});
    mouse_reset();
    const uint8_t fast[4] = {0x00, 5, 3, 0};
    usb_map_mouse(map, fast, sizeof(fast));
    CHECK_EQ(mouse_page()[kMouseHiX], 10);
    CHECK_EQ(mouse_page()[kMouseHiY], 250);

    // Вдвое медленнее: приращение делится, остаток отбрасывается.
    usb_map_set_mouse(MouseTuning{2, false});
    mouse_reset();
    usb_map_mouse(map, fast, sizeof(fast));
    CHECK_EQ(mouse_page()[kMouseHiX], 2);

    // Медленное движение не пропадает: датчик шлёт по единице, и при
    // скорости 1 четыре отчёта обязаны дать ровно один шаг, а не ноль.
    usb_map_set_mouse(MouseTuning{1, false});
    mouse_reset();
    const uint8_t slow[4] = {0x00, 1, 0, 0};
    for (int i = 0; i < 3; ++i)
        usb_map_mouse(map, slow, sizeof(slow));
    CHECK_EQ(mouse_page()[kMouseHiX], 0);
    usb_map_mouse(map, slow, sizeof(slow));
    CHECK_EQ(mouse_page()[kMouseHiX], 1);
    for (int i = 0; i < 4; ++i)
        usb_map_mouse(map, slow, sizeof(slow));
    CHECK_EQ(mouse_page()[kMouseHiX], 2);

    // Остаток одного знака с частным: движение туда-сюда не набирает сдвиг.
    usb_map_set_mouse(MouseTuning{2, false});
    mouse_reset();
    const uint8_t right1[4] = {0x00, 1, 0, 0};
    const uint8_t left1[4]  = {0x00, 0xFF, 0, 0};
    for (int i = 0; i < 8; ++i) {
        usb_map_mouse(map, right1, sizeof(right1));
        usb_map_mouse(map, left1, sizeof(left1));
    }
    CHECK_EQ(mouse_page()[kMouseHiX], 0);

    // Кнопки местами: нажата левая, на матрице гаснет правая.
    usb_map_set_mouse(MouseTuning{4, true});
    mouse_reset();
    const uint8_t left[4] = {0x01, 0, 0, 0};
    usb_map_mouse(map, left, sizeof(left));
    CHECK_EQ(mouse_page()[kMouseHiButtons], static_cast<uint8_t>(0xFF & ~kMouseButtonRight));

    // Средняя остаётся средней при любой перестановке.
    mouse_reset();
    const uint8_t middle[4] = {0x04, 0, 0, 0};
    usb_map_mouse(map, middle, sizeof(middle));
    CHECK_EQ(mouse_page()[kMouseHiButtons], static_cast<uint8_t>(0xFF & ~kMouseButtonMiddle));

    usb_map_set_mouse(MouseTuning{});
}

void mouse_report_id_tests() {
    ReportMap map;
    CHECK(report_map_build(kMouseWithId, sizeof(kMouseWithId), map));
    CHECK(map.uses_report_ids);
    CHECK_EQ(static_cast<int>(map.button_count), 3);

    const Field& x = map.axis[static_cast<uint8_t>(Axis::X)];
    const Field& y = map.axis[static_cast<uint8_t>(Axis::Y)];
    CHECK(x.present() && y.present());
    // Смещения считаются от данных, номер отчёта в них не входит.
    CHECK_EQ(static_cast<int>(x.bit_offset), 8);
    CHECK_EQ(static_cast<int>(y.bit_offset), 16);
    CHECK_EQ(static_cast<int>(x.report_id), 1);
    CHECK_EQ(static_cast<int>(x.logical_min), -127); // знак расширен
    CHECK_EQ(static_cast<int>(x.logical_max), 127);

    mouse_reset();
    // Настоящий отчёт: номер 1, нажата вторая кнопка, X = +5, Y = -3.
    const uint8_t r[6] = {0x01, 0x02, 0x05, 0xFD, 0x00, 0x00};
    usb_map_mouse(map, r, sizeof(r));
    CHECK_EQ(mouse_page()[kMouseHiX], 5);
    CHECK_EQ(mouse_page()[kMouseHiY], 3); // -(-3) = +3
    CHECK_EQ(mouse_page()[kMouseHiButtons], static_cast<uint8_t>(0xFF & ~kMouseButtonRight));

    // Отчёт с ЧУЖИМ номером не должен трогать состояние вовсе - именно на
    // этом ломалась мышь на плате.
    const uint8_t alien[6] = {0x02, 0x7F, 0x7F, 0x7F, 0x00, 0x00};
    usb_map_mouse(map, alien, sizeof(alien));
    CHECK_EQ(mouse_page()[kMouseHiX], 5);
    CHECK_EQ(mouse_page()[kMouseHiY], 3);
    CHECK_EQ(mouse_page()[kMouseHiButtons], static_cast<uint8_t>(0xFF & ~kMouseButtonRight));
}

void gamepad_tests() {
    ReportMap map;
    CHECK(report_map_build(kGamepad, sizeof(kGamepad), map));
    CHECK(!map.uses_report_ids);
    CHECK_EQ(static_cast<int>(map.button_count), 8);

    const Field& x = map.axis[static_cast<uint8_t>(Axis::X)];
    const Field& y = map.axis[static_cast<uint8_t>(Axis::Y)];
    CHECK_EQ(static_cast<int>(x.bit_offset), 0);
    CHECK_EQ(static_cast<int>(y.bit_offset), 8);
    CHECK_EQ(static_cast<int>(x.logical_max), 255);
    CHECK(map.hat.present());
    CHECK_EQ(static_cast<int>(map.hat.bit_offset), 16);
    CHECK_EQ(static_cast<int>(map.hat.bit_size), 4);
    // Кнопки идут за шляпкой и её добивкой.
    CHECK_EQ(static_cast<int>(map.button[0].bit_offset), 24);
    CHECK_EQ(static_cast<int>(map.button[7].bit_offset), 31);

    joystick_reset();
    // Середина осей и шляпка в покое (8 вне диапазона 0..7).
    const uint8_t centre[4] = {0x80, 0x80, 0x08, 0x00};
    usb_map_gamepad(map, centre, sizeof(centre));
    CHECK_EQ(joystick_state(), 0);

    // Влево и вверх: обе оси в нуле.
    const uint8_t up_left[4] = {0x00, 0x00, 0x08, 0x00};
    usb_map_gamepad(map, up_left, sizeof(up_left));
    CHECK_EQ(joystick_state(), static_cast<uint8_t>(kJoyLeft | kJoyUp));

    // Вниз и огонь первой кнопкой.
    const uint8_t fire[4] = {0x80, 0xFF, 0x08, 0x01};
    usb_map_gamepad(map, fire, sizeof(fire));
    CHECK_EQ(joystick_state(), static_cast<uint8_t>(kJoyDown | kJoyFire));

    // Мёртвая зона - восьмая доля диапазона, то есть +-31 от середины.
    const uint8_t drift[4] = {0x90, 0x70, 0x08, 0x00};
    usb_map_gamepad(map, drift, sizeof(drift));
    CHECK_EQ(joystick_state(), 0);

    // Шляпка: 0 вверх, 2 вправо, 3 вниз-вправо.
    const uint8_t hat_up[4] = {0x80, 0x80, 0x00, 0x00};
    usb_map_gamepad(map, hat_up, sizeof(hat_up));
    CHECK_EQ(joystick_state(), kJoyUp);
    const uint8_t hat_right[4] = {0x80, 0x80, 0x02, 0x00};
    usb_map_gamepad(map, hat_right, sizeof(hat_right));
    CHECK_EQ(joystick_state(), kJoyRight);
    const uint8_t hat_dr[4] = {0x80, 0x80, 0x03, 0x00};
    usb_map_gamepad(map, hat_dr, sizeof(hat_dr));
    CHECK_EQ(joystick_state(), static_cast<uint8_t>(kJoyDown | kJoyRight));

    // Кнопка восьмая - тоже огонь.
    const uint8_t b8[4] = {0x80, 0x80, 0x08, 0x80};
    usb_map_gamepad(map, b8, sizeof(b8));
    CHECK_EQ(joystick_state(), kJoyFire);

    usb_map_gamepad_release_all();
    CHECK_EQ(joystick_state(), 0);
}

// Клавиатура с номером отчёта: модификаторы разрядами, коды массивом из
// шести слотов. Загрузочная раскладка к такому отчёту неприменима -
// первый байт номер, а не модификаторы.
// clang-format off
const uint8_t kKeyboardWithId[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x02,       //   Report ID (2)
    0x05, 0x07,       //   Usage Page (Keyboard)
    0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,       //   Input - восемь модификаторов по разряду
    0x95, 0x01, 0x75, 0x08,
    0x81, 0x01,       //   Input (Constant) - байт запаса
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00, 0x29, 0x65,
    0x81, 0x00,       //   Input (Data, Array) - шесть слотов кодов
    0xC0
};
// clang-format on

void keyboard_descriptor_tests() {
    ReportMap map;
    CHECK(report_map_build(kKeyboardWithId, sizeof(kKeyboardWithId), map));
    CHECK(map.uses_report_ids);

    const KeyboardMap& k = map.keyboard;
    CHECK(k.present());
    CHECK_EQ(static_cast<int>(k.report_id), 2);
    CHECK_EQ(static_cast<int>(k.modifier_count), 8);
    // Модификаторы идут разрядами 0..7 с назначениями 0xE0..0xE7.
    CHECK_EQ(static_cast<int>(k.modifier[0].bit_offset), 0);
    CHECK_EQ(static_cast<int>(k.modifier_usage[0]), 0xE0);
    CHECK_EQ(static_cast<int>(k.modifier_usage[1]), 0xE1);
    CHECK_EQ(static_cast<int>(k.modifier_usage[7]), 0xE7);
    // Массив: шесть слотов по байту, за модификаторами и байтом запаса.
    CHECK_EQ(static_cast<int>(k.slot_count), 6);
    CHECK_EQ(static_cast<int>(k.slot_bit_size), 8);
    CHECK_EQ(static_cast<int>(k.slot_bit_offset), 16);

    // Левый Shift и клавиша A. Номер отчёта 2 первым байтом.
    keyboard_reset();
    const uint8_t r[8] = {0x02, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
    usb_map_keyboard(map, r, sizeof(r));
    CHECK_EQ(keyboard_page()[0xFE], 0xFE); // CAPS SHIFT в полустроке A8
    CHECK_EQ(keyboard_page()[0xFD], 0xFE); // A в полустроке A9

    // Отчёт с чужим номером состояние не трогает.
    const uint8_t alien[8] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    usb_map_keyboard(map, alien, sizeof(alien));
    CHECK_EQ(keyboard_page()[0xFE], 0xFE);
    CHECK_EQ(keyboard_page()[0xFD], 0xFE);

    // Отпускание: тот же отчёт без кодов и модификаторов.
    const uint8_t up[8] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    usb_map_keyboard(map, up, sizeof(up));
    CHECK_EQ(keyboard_page()[0x00], 0xFF);

    // Больше клавиш, чем отдаёт массив: признак переполнения, состояние
    // прежнее.
    const uint8_t rollover[8] = {0x02, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01};
    usb_map_keyboard(map, rollover, sizeof(rollover));
    CHECK_EQ(keyboard_page()[0x00], 0xFF);
}

// Левый Shift выбирает верхний знак клавиши, остальные модификаторы
// нажимают то, что им назначено настройкой.
void modifier_role_tests() {
    keyboard_reset();

    // Shift+2 - это "@", то есть SYMBOL SHIFT и 2. CAPS SHIFT при этом не
    // нажат: знак уже набран, добавлять к нему регистр нечего.
    kbd_report(0x02, 0x1F);
    CHECK_EQ(kbd_read(0x7F), 0xFD); // SYMBOL SHIFT
    CHECK_EQ(kbd_read(0xF7), 0xFD); // 2
    CHECK_EQ(kbd_read(0xFE), 0xFF); // CAPS SHIFT свободен

    // У буквы верхней записи нет, и тот же Shift становится CAPS SHIFT.
    kbd_report(0x02, 0x04);
    CHECK_EQ(kbd_read(0xFE), 0xFE); // CAPS SHIFT
    CHECK_EQ(kbd_read(0xFD), 0xFE); // A
    CHECK_EQ(kbd_read(0x7F), 0xFF); // SYMBOL SHIFT свободен

    // Правый Shift по умолчанию просто CAPS SHIFT: верхний знак он не
    // выбирает, поэтому выходит цифра, а не "@".
    kbd_report(0x20, 0x1F);
    CHECK_EQ(kbd_read(0xFE), 0xFE); // CAPS SHIFT
    CHECK_EQ(kbd_read(0xF7), 0xFD); // 2
    CHECK_EQ(kbd_read(0x7F), 0xFF); // SYMBOL SHIFT свободен

    // Alt по умолчанию машине не отдан вовсе.
    kbd_report(0x04, 0x1F);
    CHECK_EQ(kbd_read(0xFE), 0xFF);
    CHECK_EQ(kbd_read(0x7F), 0xFF);
    CHECK_EQ(kbd_read(0xF7), 0xFD);

    // Настройка меняет роль, раскладка при этом та же.
    ModifierRoles roles = usb_map_default_modifiers();
    roles.right_shift   = ModRole::SymbolShift;
    roles.left_alt      = ModRole::CapsShift;
    usb_map_set_modifiers(roles);
    CHECK_EQ(static_cast<int>(usb_map_modifiers().right_shift), static_cast<int>(ModRole::SymbolShift));

    kbd_report(0x20, 0x1F);
    CHECK_EQ(kbd_read(0x7F), 0xFD); // SYMBOL SHIFT
    CHECK_EQ(kbd_read(0xFE), 0xFF); // CAPS SHIFT свободен

    kbd_report(0x04);
    CHECK_EQ(kbd_read(0xFE), 0xFE); // CAPS SHIFT

    usb_map_set_modifiers(usb_map_default_modifiers());
    kbd_report(0);
    CHECK_EQ(kbd_read(0xFE), 0xFF);
}

// F11 и F12 стоят выше раскладки: в матрицу не попадают, а дёргают линии
// машины.
void signal_key_tests() {
    keyboard_reset();
    usb_map_keyboard_release_all();
    const int reset0 = s_reset_requests;
    const int nmi0   = s_nmi_requests;

    // Первым делом: клавишу, зажатую к моменту подключения, сигналом не
    // считаем - иначе зажатая тройка сбрасывала бы плату по кругу.
    kbd_report(0, 0x44);
    CHECK_EQ(s_nmi_requests - nmi0, 0);
    kbd_report(0); // отпустили - теперь взведено

    kbd_report(0, 0x44); // F11
    CHECK_EQ(s_nmi_requests - nmi0, 1);
    // Клавиша держится - сигнал уже подан, повтора нет.
    kbd_report(0, 0x44);
    CHECK_EQ(s_nmi_requests - nmi0, 1);
    kbd_report(0);
    kbd_report(0, 0x44);
    CHECK_EQ(s_nmi_requests - nmi0, 2);
    CHECK_EQ(s_reset_requests - reset0, 0);

    kbd_report(0);
    kbd_report(0, 0x45); // F12
    CHECK_EQ(s_reset_requests - reset0, 1);
    CHECK_EQ(s_nmi_requests - nmi0, 2);

    // Ни та, ни другая в матрице не отзывается ничем.
    kbd_report(0, 0x44, 0x45);
    const uint8_t halfrows[8] = {0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F};
    for (uint8_t hi : halfrows)
        CHECK_EQ(kbd_read(hi), 0xFF);

    // Устройство выдернули с зажатой клавишей: подключённое снова, оно
    // шлёт её же - сигнала быть не должно, пока не отпустят.
    usb_map_keyboard_release_all();
    kbd_report(0, 0x45);
    CHECK_EQ(s_reset_requests - reset0, 1);
    kbd_report(0);
    kbd_report(0, 0x45);
    CHECK_EQ(s_reset_requests - reset0, 2);
    kbd_report(0);

    // Полный сброс - только всей тройкой.
    const int hard0 = s_hard_requests;
    kbd_report(0x01, 0x4C); // Ctrl+Delete
    CHECK_EQ(s_hard_requests - hard0, 0);
    CHECK_EQ(kbd_read(0xEF), 0xFD); // Delete прошёл в матрицу: CS+9
    kbd_report(0);
    kbd_report(0x05, 0x4C); // Ctrl+Alt+Delete
    CHECK_EQ(s_hard_requests - hard0, 1);
    // Delete в матрицу не идёт: ни CAPS SHIFT, ни "9".
    CHECK_EQ(kbd_read(0xFE), 0xFF);
    CHECK_EQ(kbd_read(0xEF), 0xFF);
    // Ctrl при этом остаётся обычной клавишей - SYMBOL SHIFT.
    CHECK_EQ(kbd_read(0x7F), 0xFD);
    // Держат - повтора нет. Удержание отчётов не порождает вовсе, поэтому
    // и срок тут проверять не на чем: сигнал идёт по нажатию.
    kbd_report(0x05, 0x4C);
    CHECK_EQ(s_hard_requests - hard0, 1);
    kbd_report(0);
    kbd_report(0x05, 0x4C);
    CHECK_EQ(s_hard_requests - hard0, 2);
    kbd_report(0);

    // Win+F12 - вход в конфигуратор, и одиночного сброса при этом быть не
    // должно: иначе машина сбрасывалась бы дважды, своим сбросом и
    // перезагрузкой платы.
    const int config0 = s_config_requests;
    const int reset1  = s_reset_requests;
    kbd_report(0x08, 0x45); // левый Win + F12
    CHECK_EQ(s_config_requests - config0, 1);
    CHECK_EQ(s_reset_requests - reset1, 0);
    // Держат - повтора нет.
    kbd_report(0x08, 0x45);
    CHECK_EQ(s_config_requests - config0, 1);
    kbd_report(0);
    kbd_report(0x80, 0x45); // правый Win - то же самое
    CHECK_EQ(s_config_requests - config0, 2);
    kbd_report(0);

    // Без Win та же клавиша остаётся сбросом машины.
    kbd_report(0, 0x45);
    CHECK_EQ(s_reset_requests - reset1, 1);
    CHECK_EQ(s_config_requests - config0, 2);
    kbd_report(0);
}

// Отказные дескрипторы. До сих пор проверялись только заведомо годные, а
// код отказа разборщика выбрасывался: в журнале оставалось одно слово
// "НЕ РАЗОБРАН", и потолок номеров отчёта не отличить от незнакомой
// страницы назначений. Для пользователя это "клавиатура не работает".
void report_map_error_tests() {
    ReportMap map;

    // Пустой и нулевой длины - отказ до разборщика, код прошлого разбора не
    // меняется.
    CHECK(report_map_build(kKeyboardWithId, sizeof(kKeyboardWithId), map));
    const uint8_t after_good = report_map_last_error();
    CHECK(!report_map_build(nullptr, 0, map));
    CHECK_EQ(report_map_last_error(), after_good);
    CHECK(!report_map_build(kKeyboardWithId, 0, map));
    CHECK_EQ(report_map_last_error(), after_good);

    // Девять номеров отчёта: потолок HID_MAX_REPORT_IDS - восемь.
    // Комбо-приёмник "клавиатура плюс мышь плюс мультимедиа" его перебирает.
    uint8_t desc[256];
    uint16_t n = 0;
    desc[n++]  = 0x05;
    desc[n++]  = 0x01; // Usage Page (Generic Desktop)
    desc[n++]  = 0x09;
    desc[n++]  = 0x02; // Usage (Mouse)
    desc[n++]  = 0xA1;
    desc[n++]  = 0x01; // Collection (Application)
    for (uint8_t id = 1; id <= 9; ++id) {
        desc[n++] = 0x85;
        desc[n++] = id; // Report ID
        desc[n++] = 0x05;
        desc[n++] = 0x09; // Usage Page (Button)
        desc[n++] = 0x09;
        desc[n++] = 0x01; // Usage (Button 1)
        desc[n++] = 0x15;
        desc[n++] = 0x00; // Logical Minimum 0
        desc[n++] = 0x25;
        desc[n++] = 0x01; // Logical Maximum 1
        desc[n++] = 0x75;
        desc[n++] = 0x01; // Report Size 1
        desc[n++] = 0x95;
        desc[n++] = 0x01; // Report Count 1
        desc[n++] = 0x81;
        desc[n++] = 0x02; // Input (Data, Variable, Absolute)
    }
    desc[n++] = 0xC0; // End Collection
    CHECK(!report_map_build(desc, n, map));
    CHECK_EQ(report_map_last_error(), 7u); // HID_PARSE_InsufficientReportIDItems
    std::printf("  nine report numbers: code %u\n", report_map_last_error());

    // Восемь тем же дескриптором проходят: потолок именно восемь, а не
    // "около восьми".
    const uint16_t eight = static_cast<uint16_t>(n - 1u - 16u);
    uint8_t ok8[256];
    std::memcpy(ok8, desc, eight);
    ok8[eight] = 0xC0;
    CHECK(report_map_build(ok8, static_cast<uint16_t>(eight + 1u), map));
    // Восемь - это 8, HID_PARSE_NoUnfilteredReportItems: отбор разбирает
    // поле на месте и всегда отвечает "не класть", поэтому удачный разбор в
    // этой сборке кончается именно им, а не нулём.
    CHECK_EQ(report_map_last_error(), 8u);
    std::printf("  report numbers: eight is code %u, nine is code 7\n", report_map_last_error());

    // Обрыв посередине - ненулевой код, а не разобранная карта. Режем так,
    // чтобы последним остался незавершённый двухбайтовый предмет.
    CHECK(!report_map_build(desc, 7, map));
    CHECK(report_map_last_error() != 0u);
}

// Порт 0xFE занимается только под нажатой клавишей: на машине со своей
// клавиатурой его ведёт ULA, и занятый постоянно порт отдавал бы нашу
// страницу вместо неё на каждом чтении. Ради этого правила в keyboard.cpp
// заведены s_present, s_held и any_pressed, а исполнялось оно до сих пор
// только на плате: keyboard_attach в тестах не звали ни разу.
void keyboard_hold_tests() {
    const uint8_t port = devices::hid::kKeyboardPort;

    // Без attach нажатие порт не занимает: клавиатуры нет.
    keyboard_detach();
    keyboard_reset();
    uint32_t sets = g_port_page_sets[port];
    kbd_report(0, 0x04);
    CHECK_EQ(g_port_page_sets[port], sets);
    kbd_report(0);

    // После attach первое нажатие ставит страницу ровно один раз, второе её
    // не переставляет.
    keyboard_attach();
    sets            = g_port_page_sets[port];
    uint32_t clears = g_port_clears[port];
    kbd_report(0, 0x04);
    CHECK_EQ(g_port_page_sets[port], sets + 1u);
    CHECK(g_port_last_page == keyboard_page());
    kbd_report(0, 0x05);
    CHECK_EQ(g_port_page_sets[port], sets + 1u);

    // Отпускание всех клавиш порт снимает.
    kbd_report(0);
    CHECK_EQ(g_port_clears[port], clears + 1u);

    // Отключение под нажатой клавишей - тоже снимает: иначе страница осталась
    // бы висеть на порту ULA.
    kbd_report(0, 0x04);
    clears = g_port_clears[port];
    keyboard_detach();
    CHECK_EQ(g_port_clears[port], clears + 1u);

    // Снятие всего состояния оставляет порт снятым.
    keyboard_attach();
    kbd_report(0, 0x04);
    clears = g_port_clears[port];
    usb_map_keyboard_release_all();
    CHECK_EQ(g_port_clears[port], clears + 1u);
    keyboard_detach();
}

// Полный обход раскладки. До сих пор она сверялась десятком точек, причём
// только теми кодами, которые в ней ЕСТЬ. Код без записи ведёт себя как
// рабочая клавиша - молча ничего не нажимает, - и для пользователя это
// "клавиша не работает", неотличимо от промаха дескриптора и потери отчёта.
void keymap_sweep_tests() {
    keyboard_reset();
    const uint32_t unmapped_before = usb_map_unmapped_presses();
    uint32_t mapped                = 0;
    uint32_t unmapped              = 0;
    // С двух: 0 - "клавиши нет", 1 - ErrorRollOver, оба до раскладки.
    for (uint16_t code = 2; code < 256; ++code) {
        kbd_report(0, static_cast<uint8_t>(code));
        bool any = false;
        for (uint32_t p = 0; p < 256; ++p) {
            if (keyboard_page()[p] != 0xFF) any = true;
        }
        if (any) {
            ++mapped;
        } else {
            ++unmapped;
        }
        kbd_report(0);
    }
    std::printf("  layout: with a record %u, without a record %u\n", mapped, unmapped);
    CHECK_EQ(mapped + unmapped, 254u);
    // Числа закреплены: опечатка в kKeymap сдвинет их, и это заметит прогон,
    // а не пользователь на машине.
    CHECK_EQ(mapped, 79u);
    CHECK_EQ(unmapped, 175u);
    // Счётчик платы растёт ровно на число кодов без записи.
    CHECK_EQ(usb_map_unmapped_presses() - unmapped_before, unmapped);

    // Поимённо: A есть, F1 нет.
    const uint32_t before_a = usb_map_unmapped_presses();
    kbd_report(0, 0x04);
    CHECK_EQ(keyboard_page()[0xFD], 0xFE);
    CHECK_EQ(usb_map_unmapped_presses(), before_a);
    kbd_report(0, 0x3A); // F1
    CHECK_EQ(usb_map_unmapped_presses(), before_a + 1u);

    // Обход мог взвести сигнальные клавиши: состояние снимается целиком.
    usb_map_keyboard_release_all();
}

} // namespace

void run_hid_tests() {
    std::printf("hid: response pages, joystick and the USB layout\n");
    keyboard_tests();
    mouse_tests();
    joystick_tests();
    usb_map_tests();
    keymap_sweep_tests();
    keyboard_hold_tests();
    modifier_role_tests();
    signal_key_tests();
    mouse_boot_tests();
    mouse_tuning_tests();
    mouse_report_id_tests();
    gamepad_tests();
    keyboard_descriptor_tests();
    report_map_error_tests();
}
