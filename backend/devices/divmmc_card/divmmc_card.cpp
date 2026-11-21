// SPDX-License-Identifier: MIT
// Обработчики портов 0xE7/0xEB.

#include "devices/divmmc_card/divmmc_card.h"

#include "platform/hot_path.h"

#include "devices/hal/z80_ports.h"
#include "devices/sd/sd_protocol.h"
#include "devices/sd/spi_emu.h"

namespace devices::divmmc_card {
namespace {

using sd::SdOwner;

constexpr uint8_t kCard1Bit = 0x01;
constexpr uint8_t kCard2Bit = 0x02;

bool s_on1 = false;
bool s_on2 = false;
// С кем идёт обмен через 0xEB. Невыбранная карта отвечает 0xFF, поэтому без
// выбора это первая: её автомат и отдаёт пустую шину.
volatile SdOwner s_cur = SdOwner::DivMmc;

// Защёлка ответа - как у Z-Controller: чтение отдаёт результат прошлого
// обмена. Ячейку пишут оба обработчика порта; вытесненный между обменом и
// записью положил бы свой, более ранний ответ поверх свежего: запись
// повторяется, пока ячейка не совпадёт с ответом последнего обмена.
void SOUNDSINTH_HOT_PATH(latch)(SdOwner who, uint8_t value) {
    for (;;) {
        hal::z80_port_set_read(kPortData, value);
        const uint8_t now = sd::sd_spi_latch(who);
        if (now == value) return;
        value = now;
    }
}

void SOUNDSINTH_HOT_PATH(port_write_data)(uint8_t, uint8_t data) {
    const SdOwner who = s_cur;
    latch(who, sd::sd_spi_byte(who, data));
}

void SOUNDSINTH_HOT_PATH(port_read_data_done)(uint8_t) {
    const SdOwner who = s_cur;
    latch(who, sd::sd_spi_byte(who, sd::kIdleByte));
}

// Обмена порт выбора не делает, защёлку не трогает: в ней результат
// последнего обмена.
void SOUNDSINTH_HOT_PATH(port_write_ctrl)(uint8_t, uint8_t data) {
    const bool c1 = s_on1 && (data & kCard1Bit) == 0u;
    const bool c2 = s_on2 && (data & kCard2Bit) == 0u && !c1;
    if (s_on1) sd::sd_spi_select(SdOwner::DivMmc, c1);
    if (s_on2) sd::sd_spi_select(SdOwner::DivMmc2, c2);
    s_cur = c2 ? SdOwner::DivMmc2 : SdOwner::DivMmc;
}

} // namespace

void divmmc_card_init(bool card1, bool card2) {
    sd::sd_spi_emu_ensure_up();
    s_on1 = card1;
    s_on2 = card2;
    s_cur = SdOwner::DivMmc;
    hal::z80_port_on_write(kPortCtrl, &port_write_ctrl);
    hal::z80_port_on_write(kPortData, &port_write_data);
    hal::z80_port_on_read_done(kPortData, &port_read_data_done);
    // До первого обмена - пустая шина.
    hal::z80_port_set_read(kPortData, sd::kIdleByte);
}

} // namespace devices::divmmc_card
