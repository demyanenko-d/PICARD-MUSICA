// SPDX-License-Identifier: MIT
// Обработчики портов 0x57/0x77.

#include "devices/zcontroller/zcontroller.h"

#include "devices/hal/z80_ports.h"
#include "devices/sd/sd_protocol.h"
#include "devices/sd/spi_emu.h"
#include "devices/storage/storage.h"
#include "platform/hot_path.h"

namespace devices::zcontroller {
namespace {

namespace sd  = devices::sd;
namespace hal = devices::hal;

// Запись в порт управления: бит SSEL, ноль выбирает карту. Номера портов и
// значения чтения - в заголовке: их сверяет тест.
constexpr uint8_t kCtrlSselBit = 0x02;

// --- Защёлка ответа ---
//
// Z-Controller работает конвейером: чтение порта данных
// возвращает результат предыдущего обмена и сразу запускает следующий,
// холостой. Z80 не ждёт завершения обмена, а забирает готовое.
//
// С шиной это совпадает: ответ на чтение выдаёт PIO из таблицы, байт
// должен лежать там заранее, и здесь это всегда уже случившийся
// результат. Один обмен на одно обращение хоста.
//
// Лишний обмен на чтении ломает разбор: команда - шесть записей подряд, и
// она превращается в CMD0 с аргументом 0xFF00FF00.
void SOUNDSINTH_HOT_PATH(latch)(uint8_t value) {
    hal::z80_port_set_read(kPortData, value);
}

// --- Обработчики ---

void SOUNDSINTH_HOT_PATH(port_write_data)(uint8_t, uint8_t data) {
    // Обмен с байтом хоста; его результат - ответ на ближайшее чтение, как
    // у настоящего контроллера.
    latch(sd::sd_spi_byte(sd::SdOwner::ZController, data));
}

void SOUNDSINTH_HOT_PATH(port_write_ctrl)(uint8_t, uint8_t data) {
    // Бит 1: ноль выбирает карту. Порт управления обмена не делает, только двигает CS. Защёлка не
    // трогается: в ней результат последнего обмена.
    sd::sd_spi_select(sd::SdOwner::ZController, (data & kCtrlSselBit) == 0u);
}

// -1 - ещё не публиковали: первая публикация нужна при любом состоянии.
int8_t s_present_published = -1;
bool s_running             = false;

void SOUNDSINTH_HOT_PATH(publish_presence)(bool present) {
    const int8_t now = present ? 1 : 0;
    if (now == s_present_published) return;
    s_present_published = now;
    hal::z80_port_set_read(kPortCtrl, present ? kCtrlCardPresent : kCtrlNoCard);
}

void SOUNDSINTH_HOT_PATH(port_read_data_done)(uint8_t) {
    // Защёлкнутый байт уже ушёл на шину - запускается следующий холостой
    // обмен, его результат станет ответом на следующее чтение.
    latch(sd::sd_spi_byte(sd::SdOwner::ZController, sd::kIdleByte));
}

} // namespace

void zcontroller_init(bool emulator_up) {
    // Эмулятор карты поднимает только первый хозяин.
    if (!emulator_up) sd::sd_spi_emu_init();

    hal::z80_port_on_write(kPortData, &port_write_data);
    hal::z80_port_on_write(kPortCtrl, &port_write_ctrl);
    hal::z80_port_on_read_done(kPortData, &port_read_data_done);

    // Состояние карты в порту управления: драйвер WC смотрит только бит 1,
    // но чужой софт может смотреть и остальные.
    publish_presence(devices::storage::storage_emulator_present(static_cast<uint8_t>(sd::SdOwner::ZController)));

    // До первого обмена защёлка пуста - отдаётся то же, что невыбранная
    // карта.
    latch(sd::kIdleByte);
    s_running = true;
}

void SOUNDSINTH_HOT_PATH(zcontroller_tick)() {
    if (!s_running) return;
    // Опрос, а не событие: обе проверки читают готовый признак драйвера, ни
    // одного обращения к носителю здесь нет. Публикация - запись байта в
    // таблицу ответов, шину она не задерживает.
    publish_presence(devices::storage::storage_emulator_present(static_cast<uint8_t>(sd::SdOwner::ZController)));
}

} // namespace devices::zcontroller
