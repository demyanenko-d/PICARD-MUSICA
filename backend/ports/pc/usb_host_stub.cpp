// USB-хоста на ПК нет: контракт platform/usb_host.h закрывается пустышкой.
// Цикл сеанса зовёт виток на обеих сборках, и без этого хостовая не
// собиралась бы.

#include "platform/usb_host.h"

void platform::usb_host_task() {}

void platform::usb_host_set_wait_service(void (*)(void*), void*) {}
