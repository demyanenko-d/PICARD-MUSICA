# hostlink - связь платы с хостом ZX Spectrum

Модуль отвечает хосту по шине Z80, принимает от него файл частями и
отдаёт телеметрию. Ядро связи (bus, host_protocol, hostlink) о музыке и
PSRAM не знает: потребитель получает байты и решает сам. gs_bridge пишет
в PSRAM и зовёт загрузчик трека, divmmc - эмулятор SD.

## Файлы

```
hostlink/
  hostlink.*        фасад: колбэки, запуск, обслуживание, печать здоровья связи
  bus.*             логика шины Z80: пины, приём и выдача байта, эмуляция ПЗУ
  bus_setup.*       монтаж: раздача автоматов PIO, загрузка программ, DMA, IRQ
  ports.pio         автоматы цикла ввода-вывода (IORQ) и rom_serve
  memory.pio        автоматы цикла памяти (MREQ) и трапы
  divmmc.*          DivMMC: своё ПЗУ и ОЗУ в окне 0x0000-0x3FFF
  gs_bridge.*       General Sound: привязка протокола GS к портам

  rom_images/
    rom_image_divmmc.h  прошивка DivMMC, сгенерированный массив
```

Пути включений прописаны в CMakeLists, подкаталог в `#include` писать не
нужно.

## Использование

```cpp
#include "hostlink.h"

soundsinth::io::HostProtocol g_protocol;

void on_session(void* user, uint32_t sector, uint32_t length, uint8_t flags, uint8_t load_order) { /* ... */ }
void on_reset(void* user) { /* ... */ }

// На том ядре, которое дальше будет обслуживать шину:
hostlink::Callbacks cb{&my_state, &on_session, &on_reset};
hostlink::init(g_protocol, cb);
hostlink::release_host_reset();

for (;;) {
    hostlink::service();          // разобрать команду хоста, напечатать здоровье связи
    // ... своя работа ...
}
```

Ожидая данные, крутить `service_and_wait`, чтобы связь не замерла; окно - в буфере
приёма протокола до следующего запроса:

```cpp
g_protocol.request_file_chunk(offset, 4096);
uint16_t len = 0;
while (!g_protocol.take_received(len)) hostlink::service_and_wait();   // service() + wfi
const uint8_t* window = g_protocol.data_buffer();          // len байт, 0 - запрос сдан
```

Телеметрия и сессия - сеттерами самого `HostProtocol` (`set_position`,
`set_psram_stats`, `set_engine_load`, `mark_session_ready`, `end_session`);
`hostlink::set_file_info` - общая точка для загрузки и эмуляции GS.

## Перенос в другой проект

Скопировать этот каталог (без gs_bridge; divmmc нужен bus.cpp и тянет
эмулятор SD из `backend/devices/sd`) плюс:

- `backend/ports/rp2350/hw_config.h` - карта ресурсов PIO, DMA, IRQ
- `backend/player/protocol/host_protocol.{h,cpp}` и `host_frame.h`
- `backend/ports/rp2350/firmware_config.h` - порты протокола и ключи `SOUNDSINTH_*`
- `backend/platform/log.h` - или свою функцию `debug_log(const char*)`

Зависимости: pico-sdk (`hardware_pio`, `hardware_dma`, `hardware_irq`,
`hardware_clocks`, `pico_time`) и `debug_log()`.

Заголовки PIO-программ генерирует CMake:

```cmake
pico_generate_pio_header(my_target ${CMAKE_CURRENT_LIST_DIR}/hostlink/ports.pio)
pico_generate_pio_header(my_target ${CMAKE_CURRENT_LIST_DIR}/hostlink/memory.pio)
```

## Ограничения

**Всё живёт на одном ядре** - и обработчик прерывания, и протокол, и
фасад. Протокол не потокобезопасен, второе ядро к нему не обращается.
Исключение - `log_task` на Core0: только счётчики и печать.
`init()` вызывается с того ядра, которое будет обслуживать шину:
`irq_set_exclusive_handler` регистрирует обработчик на вызывающем ядре.

**У Z80 нет аппаратного /WAIT**, на цикл `IN` нужно ответить за ~825 нс.
Ответ выдаёт PIO из заранее подготовленного слова, обработчик прерывания
только готовит слово для следующего чтения. Маскировать прерывания на
этом пути нельзя: `mutex_t` и `critical_section_t` из pico-sdk гасят их на
всё время удержания, и несколько десятков тактов - это пропущенный цикл.

**Функции на пути шины обязаны быть в SRAM** (`__not_in_flash_func`).
Флеш и PSRAM делят один QMI, промах XIP-кэша ждёт освобождения
контроллера - до 6760 нс в обработчике против обычных 300.

**Подстановка DivMMC меняется со следующего цикла памяти.** Трап будит
ядро посреди выборки команды по своему адресу. Смена варианта таблицы и
блокировки ПЗУ машины (ROM_BLK) в том же цикле гасит ПЗУ, пока Z80 читает
из неё код (вход по 0x0038 и другим точкам), или включает её навстречу
нашему байту (выход по 1FF8-1FFF). Поэтому `apply_mapping` сначала ждёт,
пока поднимутся MREQ или RD (`wait_memory_read_end`). Без ожидания esxDOS
грузится с мусором на экране и встаёт. Окно 3D00-3DFF - исключение: там
вход немедленный, ПЗУ гасится сразу в обработчике трапа.

**Скорость обработчиков шины - часть их поведения.** Задержка, которую
даёт вызов на этом пути, может держать правильный порядок сигналов.
Убирать "лишний" вызов или переносить функцию из флеша в SRAM - то же, что
менять логику: проверять загрузкой esxDOS на железе.

**Телеметрия - это предложение, а не отправка.** Модуль держит готовый
кадр, хост забирает его, когда сам придёт. Сеттеры можно звать часто:
лишний вызов перезапишет ещё не забранное.

## Проверка

Нагрузочный самотест шины на железе (identify x4000, края файла, длины на
границах, невыровненные смещения, 1 МБ подряд; вхолостую и под нагрузкой)
расхождений не нашёл; из прошивки удалён 2026-09-10.

В штатной работе `service()` печатает переполнения WR FIFO при
изменении и раз в 10 с - число команд и байтов данных плагина: в
плагинном режиме с пиком WR FIFO, в режиме DivMMC с ответами портов
плагина и serve_late.
