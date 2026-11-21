// SPDX-License-Identifier: MIT
#pragma once

// Порты шины Z80 глазами устройства: чем отвечать на чтение и кто узнаёт о
// записи. Определяет порт.
//
// Ответ на чтение обязан лежать заранее: на плате его выдаёт автомат по
// таблице, задолго до того, как обработчик узнает о самом чтении. Поэтому
// "поставить ответ" и "чтение состоялось" - разные вызовы: первый готовит
// байт, второй сообщает, что его забрали.
//
// Обработчики зовутся из прерывания шины: только то, что укладывается в
// единицы микросекунд, без обращений к носителю и без журнала.

#include <cstdint>

namespace devices::hal {

using PortWriteFn    = void (*)(uint8_t port, uint8_t data);
using PortReadDoneFn = void (*)(uint8_t port);

// Байт, которым порт отвечает на следующее чтение.
void z80_port_set_read(uint8_t port, uint8_t value);

// Страница ответов на 256 байт: байт выбирает старшая половина адреса.
// Нужна портам, у которых ответ от неё зависит, - клавиатуре и мыши.
// Выравнивание страницы - 256 байт. Плата запоминает адрес, поэтому
// устройство переписывает страницу когда угодно, отдельно об этом не
// сообщая.
void z80_port_set_read_page(uint8_t port, const uint8_t* page);

// Порт больше не наш: на чтение отвечает машина.
void z80_port_clear_read(uint8_t port);

void z80_port_on_write(uint8_t port, PortWriteFn fn);
void z80_port_on_read_done(uint8_t port, PortReadDoneFn fn);

// Порты, которые отвечают только при включённом ПЗУ TR-DOS (Beta Disk):
// при DOS порт отвечает байтом ячейки устройства, вне DOS - как прежде;
// nullptr снимает. false - платформа так не умеет (режим DivMMC). Чтение
// такого порта при DOS - один хук на все порты.
bool z80_port_dos_cell(uint8_t port, const uint8_t* cell);
void z80_port_dos_on_read_done(PortReadDoneFn fn);
// Чтение такого порта при DOS держит /WAIT: fn(порт) зовётся из
// обработчика шины, пока Z80 стоит; ответ - z80_port_hold_answer, из
// обработчика или позже. Хук один на все порты. false - платформа не умеет.
bool z80_port_dos_hold(uint8_t port, PortReadDoneFn fn);
// То же для обычного порта; хук один на все такие порты, ставит последний
// вызов. Снимает удержание z80_port_set_read.
bool z80_port_hold(uint8_t port, PortReadDoneFn fn);
void z80_port_hold_answer(uint8_t v);
// Где порт не держит чтение до ответа: /WAIT сразу после чтения, Z80 встаёт
// на следующем цикле шины. false - и так нельзя.
bool z80_wait_late_ready();
void z80_wait_late(bool on);
// ПЗУ TR-DOS включено сейчас: запись в порт Beta Disk вне DOS не его.
bool z80_dos_active();

// Навигатор образов по NMI: страница обмена (у Z80 - 0x2000) или nullptr,
// если навигатора нет; последняя запись машины в 7FFD целиком.
uint8_t* z80_nav_page();
// Машина стоит в навигаторе: от выборки 0x0066 до RETN выхода.
bool z80_nav_open();
uint8_t z80_port_7ffd();

} // namespace devices::hal
