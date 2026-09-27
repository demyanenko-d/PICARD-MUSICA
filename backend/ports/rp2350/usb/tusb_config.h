#pragma once

// TinyUSB: только хост, встроенный контроллер RP2350.
//
// Каналов DMA не берёт ни одного: драйвер копает байты между буфером и
// двухпортовой памятью USB своим memcpy, дальше их забирает сам контроллер.
// Это проверено по исходникам порта - hardware/dma.h там не включается
// нигде.
//
// Операционной системы у стека нет, и это обязательное условие, а не
// удобство: стек крутится в цикле того ядра, которое ведёт шину.
//
// При OPT_OS_PICO очередь событий хоста заперта critical_section_t, то есть
// save_and_disable_interrupts(): PRIMASK гасит все прерывания ядра, в том
// числе шинные, и делает это на каждом витке tuh_task(). У Z80 нет
// аппаратного /WAIT - пропущенный цикл означает мусор на шине. OPT_OS_NONE
// запирает очередь только запретом прерывания USB (usbh_int_set).
//
// Значение приходит из корневого CMakeLists (TINYUSB_OPT_OS), потому что
// определение идёт командной строкой; здесь только проверка.
//
// Трогать стек с Core0 нельзя - ни из задачи, ни из прерывания.
#if CFG_TUSB_OS != OPT_OS_NONE
#error "TinyUSB: нужен OPT_OS_NONE - иначе стек гасит шинные прерывания (TINYUSB_OPT_OS в CMakeLists)"
#endif

#define CFG_TUSB_DEBUG 0

#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0
#define CFG_TUH_RPI_PIO_USB 0 // встроенный контроллер, не битбанг на PIO
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

// Концентратор: у платы один порт, а устройств просят четыре - клавиатура,
// мышь, джойстик, флешка. Без него в разъём входит ровно одно.
#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX 5 // сам концентратор адреса не занимает

#define CFG_TUH_HID 4 // у клавиатуры бывает два интерфейса, у джойстика один
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

#define CFG_TUH_MSC 1
#define CFG_TUH_CDC 0
#define CFG_TUH_VENDOR 0

// Дескрипторы устройства целиком: у флешек с несколькими интерфейсами
// умолчания в 256 байт не всегда хватает.
#define CFG_TUH_ENUMERATION_BUFSIZE 512
