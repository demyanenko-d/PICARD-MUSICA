// SPDX-License-Identifier: MIT
#pragma once

// Живой AY на чтении: плата отвечает на #FFFD регистром выбранного чипа.
// Ответ обязан лежать до чтения, поэтому тень регистров ведёт обработчик
// записи шины: ay_port_write лежит в SRAM и читает только ОЗУ.

#include <cstdint>

#include "platform/compiler.h"

namespace player::live {

inline constexpr uint8_t kAyPort = 0xfd;

struct AyPort {
    // Всё с нуля: объект лежит в .bss.
    volatile uint8_t mode = 0;     // AY_EMULATION; 0 - записи не разбираются
    uint8_t chip          = 0;     // выбранный чип TurboSound
    bool status           = false; // TurboSound FM: на чтении состояние YM2203
    uint8_t reg[2]        = {};    // выбранный регистр чипа; от 16 - не SSG
    uint8_t mask[16]      = {};    // разряды регистра, которые чип хранит
    uint8_t regs[2][16]   = {};

    // Сброс чипов: регистры в ноль, регистр не выбран, чип первый.
    void reset() {
        chip   = 0;
        status = false;
        for (uint32_t c = 0; c < 2; ++c) {
            reg[c] = 0xff;
            for (uint8_t& r : regs[c]) {
                r = 0;
            }
        }
    }

    // Запись в #FFFD. 1111xxxx у TurboSound - управление: разряд 0 - чип
    // (единица - первый), разряд 1 в нуле - читать состояние YM2203.
    SOUNDSINTH_ALWAYS_INLINE void select(uint8_t data) {
        if (mode >= 3 && (data & 0xf0u) == 0xf0u) {
            chip   = (data & 1u) != 0 ? 0 : 1;
            status = mode == 4 && (data & 0x02u) == 0;
        }
        reg[chip] = data;
    }

    // Запись в #BFFD.
    SOUNDSINTH_ALWAYS_INLINE void write(uint8_t data) {
        const uint8_t r = reg[chip];
        if (r < 16) regs[chip][r] = data & mask[r];
    }

    SOUNDSINTH_ALWAYS_INLINE uint8_t answer() const {
        // YM2203 не занят; таймеров нет, их флаги в нуле.
        if (status) return 0;
        const uint8_t r = reg[chip];
        // Вне SSG YM2203 читает ноль; AY с чужим адресом шину не ведёт.
        if (r >= 16) return mode == 4 ? 0 : 0xff;
        // Порт ввода-вывода, включённый на вход: выводы подтянуты к единице.
        if (r >= 14 && (regs[chip][7] & (0x40u << (r - 14))) == 0) return 0xff;
        return regs[chip][r];
    }
};

extern AyPort g_ay_port;
// Страница ответов порта #FD: байт выбирает старшая половина адреса. AY -
// A15 = A14 = 1, прочие адреса отвечают #FF. Словами: заполнение - 16
// записей без вызова memset.
extern uint32_t g_ay_port_page[64];

SOUNDSINTH_ALWAYS_INLINE void ay_port_publish() {
    const uint32_t v = g_ay_port.answer() * 0x01010101u;
    for (uint32_t i = 48; i < 64; ++i) {
        g_ay_port_page[i] = v;
    }
}

// Запись в #FFFD (select) или #BFFD из обработчика шины.
void ay_port_write(bool select, uint8_t data);

} // namespace player::live
