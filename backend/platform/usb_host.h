// SPDX-License-Identifier: MIT
#pragma once

// USB-хост глазами цикла Core1. Реализует порт: на плате встроенный
// контроллер RP2350, на ПК пусто.
//
// Виток стека зовётся из того же цикла, что и обслуживание шины, потому
// что другого планировщика на этом ядре нет. Долгих операций внутри нет:
// стек разбирает то, что уже пришло, и возвращается.

#include <cstdint>

namespace platform {

void usb_host_task();

// Виток из долгих ожиданий загрузки (окна с машины, сэмплы банка с карты):
// не чаще раза в миллисекунду и не изнутри самого витка. Без него сэмпл
// грузится секундами без витка, клавиатура теряет нажатия и отпускания, а
// сброс и F12 с неё не работают.
void usb_host_task_in_wait();

// Чем занять ожидание обмена с флешкой. Стек асинхронный, а носитель
// читается вызовом, который возвращается с данными: ожидание крутит виток
// стека и зовёт это. Без него шина замирает на каждом секторе.
void usb_host_set_wait_service(void (*fn)(void*), void* user);

// Подключённое устройство для экрана сведений: роли - биты kUsbRole*.
inline constexpr uint8_t kUsbRoleKeyboard = 1u << 0;
inline constexpr uint8_t kUsbRoleMouse    = 1u << 1;
inline constexpr uint8_t kUsbRoleGamepad  = 1u << 2;
inline constexpr uint8_t kUsbRoleDisk     = 1u << 3;
inline constexpr uint8_t kUsbRoleHub      = 1u << 4;
struct UsbDevice {
    uint8_t addr;
    uint8_t speed; // 0 full, 1 low, 2 high
    uint16_t vid;
    uint16_t pid;
    uint8_t roles;
};

// Сколько устройств записано в out, не больше max. Только с Core1.
uint32_t usb_host_devices(UsbDevice* out, uint32_t max);

// Интерфейс HID для утилиты проверки: что прислало устройство и что из
// этого взяла плата. Дескриптор и отчёт хранятся началом.
inline constexpr uint16_t kUsbHidDescKeep  = 256;
inline constexpr uint8_t kUsbHidReportKeep = 16;
struct UsbHidItf {
    uint8_t addr;
    uint8_t idx;
    uint8_t role;         // kUsbRole* того, чем взят; 0 - не взят
    uint8_t itf_protocol; // протокол интерфейса: 0 нет, 1 клавиатура, 2 мышь
    bool boot;            // работает в загрузочном протоколе
    uint16_t vid;
    uint16_t pid;
    uint16_t top_page; // верхняя коллекция дескриптора
    uint16_t top_usage;
    uint16_t need;     // длина отчёта по дескриптору
    uint16_t desc_len; // полная; хранится не больше kUsbHidDescKeep
    const uint8_t* desc;
    uint16_t last_len; // полная длина последнего отчёта
    const uint8_t* last;
    uint32_t reports;
};

// Подключённые интерфейсы HID, не больше max. Только с Core1.
uint32_t usb_host_hid(UsbHidItf* out, uint32_t max);

} // namespace platform
