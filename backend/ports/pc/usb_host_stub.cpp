// SPDX-License-Identifier: MIT
// USB-хоста на ПК нет: контракт platform/usb_host.h закрывается пустышкой.
// Цикл сеанса зовёт виток на обеих сборках, и без этого хостовая не
// собиралась бы.

#include "platform/usb_host.h"

void platform::usb_host_task() {}

void platform::usb_host_task_in_wait() {}

void platform::usb_host_set_wait_service(void (*)(void*), void*) {}

uint32_t platform::usb_host_devices(UsbDevice*, uint32_t) {
    return 0;
}

uint32_t platform::usb_host_hid(UsbHidItf*, uint32_t) {
    return 0;
}
