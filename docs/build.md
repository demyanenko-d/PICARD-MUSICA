# Сборка

Три независимые сборки: прошивка, хостовый проект и сторона Z80. У каждой
свой корень конфигурации, друг другу они не мешают.

Все готовые команды собраны в `scripts/`, там же их перечень.

## Раскладка репозитория

```
backend/
  platform/      контракты платформы: задачи, журнал, часы, память,
                 флеш настроек, USB-хост
  core/          синтезатор: загрузчики форматов, движок, микшер, кодеки,
                 ядра чипов (AY, SN76489, SAA1099, SCC), живой MIDI, настройки
  devices/       эмуляция устройств ZX: карта SD и носители, DivMMC,
                 Z-Controller, Beta Disk, General Sound, USB HID
  net/           сетевое содержимое: архив ZX-Art, виртуальный том FAT32
  player/        приложение платы: сеанс трека, загрузка, звуковая цепочка,
                 плеер VGM, живые звуковые устройства, General Sound,
                 конфигуратор, меню образов дисков, протокол хоста
  ports/
    rp2350/        прошивка: шина Z80 на PIO и DMA, I2S, PSRAM, FreeRTOS,
                   USB-хост, Wi-Fi, образы ПЗУ
    pc/            хостовая сборка: плеер, тесты, инструменты банков
  lib/           сторонние библиотеки: FatFs, TinyUSB 0.21.0, ymfm,
                 разборщик HID из LUFA
frontend/        сторона Z80
  common/          клиент шины
  plugin/          плагин Wild Commander
  trdos/           плеер TR-DOS
  config_rom/      ПЗУ конфигуратора, 8 КБ
  nav_rom/         ПЗУ меню образов дисков, 8 КБ
  z80_fw/          библиотека: SD, FAT, экран, клавиатура, прерывания
scripts/         сборка, выпечка банков, запись на плату
SF2/             лицензии оригинальных SoundFont
release/         готовые прошивки, программы и банки
SD/              содержимое SD-карты: Wild Commander с плагином, test_music/
docs/            документация
```

Слои `core`, `devices`, `net` и `player` платформы не знают и собираются
как для платы, так и для ПК; всё, что нужно от железа, объявлено
контрактами в `platform/` и в `*/hal/`. Этим и проверяется граница: сборка
для ПК идёт без pico-sdk, и любое обращение к нему ломается сразу.

## Зависимости

| Что | Для чего | Проверено на |
|---|---|---|
| Pico SDK 2.3.0 | прошивка RP2350B | ARM GCC 15_2_Rel1 |
| CMake не ниже 3.13, Ninja | прошивка | CMake 3.31.5 |
| CMake 4.x | хостовая сборка: генератор Visual Studio 2026 есть только в нём | CMake 4.3.4 |
| MSVC | хостовая сборка | Visual Studio 2026 |
| SDCC не ниже 4.5.0 | сторона Z80 | 4.5.0 #15242 |
| Node.js | упаковщики и скрипты | 22.x |
| picotool | образ банка, запись через BOOTSEL | 2.3.0 |
| OpenOCD | запись по SWD, щуп CMSIS-DAP | 0.12.0+dev |

Скрипты ищут picotool и OpenOCD там, куда их ставит расширение Raspberry
Pi Pico для VS Code: `%USERPROFILE%\.pico-sdk\picotool` и
`%USERPROFILE%\.pico-sdk\openocd`. Там же лежит и CMake 4.3.4:
`%USERPROFILE%\.pico-sdk\cmake\v4.3.4\bin`.

TinyUSB, ymfm, FatFs и разборщик HID лежат в `backend/lib/`, качать их не
нужно. FreeRTOS-Kernel, SDL2 и libxmp (эталон для тестов) CMake
загружает сам при первой конфигурации:

- хостовая сборка кладёт их в `.deps32/` в корне - намеренно вне `build/`,
  чтобы удаление `build/` с последующей пересборкой не требовало сети;
- прошивка кладёт FreeRTOS-Kernel в `build/zc/_deps/`, поэтому после
  удаления `build/` ей снова нужна сеть.

## Полная сборка

Порядок с нуля:

1. хостовая сборка с тестами - из неё берутся `sf2bake.exe` для выпечки
   банков и `config_example.exe` для примеров настроек
   ([Сборка по частям](#сборка-по-частям));
2. выпечка банков, один раз - `node scripts/tools/bake_banks.js`
   ([ниже](#перед-первой-сборкой-исходники-банков));
3. `scripts\build.bat`.

```
scripts\build.bat
```

Собирает прошивку и обе программы Z80 и раскладывает результат:

```
release/
  ugly.$c  ugly.sna  UMPLAYER.WMF   программы для Спектрума
  example/   три set_config_*.txt        примеры настроек под три машины
  picard/    soundsinth.uf2, bank.uf2    прошивка платы и банк
  banks/     три .ssb и лицензии к ним   банки инструментов
SD/WC/UMPLAYER.WMF                       тот же плагин на карте
build/sd.img                             образ SD-карты из SD/
```

Банки в `banks/` кладёт не сборка, а `node scripts/tools/bake_banks.js` - сборка
только делает из GeneralUser файл `bank.uf2` и добавляет лицензии.

Примеры настроек пишет `config_example.exe` тем же кодом, каким их пишет
плата. Без хостовой сборки `build.bat` останавливается на этом шаге.

### Перед первой сборкой: исходники банков

`.sf2` в репозитории нет - их надо скачать и положить в `SF2/` с точными
именами `GeneralUser-GS.sf2`, `SGM-V2.01.sf2` и
`Timbres of Heaven (XGM) 4.00(G).sf2`, после чего один раз запустить
выпечку. Откуда брать - [docs/banks.md](banks.md).

Без этого шага сборка проходит, но релиз выходит без банков, а `.mid` не
играет: файл `bank.uf2` не из чего собрать. Об этом сборка говорит
строкой `no banks\GeneralUser-GS.ssb`.

## Сборка по частям

Прошивка - корневой проект, сборка ARM-кросскомпилятором:

```
cmake -S . -B build/zc -G Ninja
cmake --build build/zc -j 8
```

Результат - `build/zc/soundsinth_firmware.uf2` и `.elf`.

Хостовая сборка - отдельный самостоятельный проект: движок собирается из
тех же исходников, но компилятором для ПК. Сюда входят плеер `pc_player`,
набор тестов и инструменты: препроцессор звуковых банков `sf2bake` и
`config_example`:

```
cmake -S backend/ports/pc -B build/pc -A Win32 -DSOUNDSINTH_BUILD_TESTS=ON
cmake --build build/pc --config Release -j 8
build/pc/tests/Release/soundsinth_tests.exe
```

Параметр `-A Win32` собирает 32-битные программы: память в них считается
так же, как на плате. 64-битной сборки нет. Инструменты собираются только
с `-DSOUNDSINTH_BUILD_TESTS=ON`.

Разделение не косметическое: CMake допускает один набор инструментов на
один проход configure, а ARM-кросскомпилятор и MSVC в одной конфигурации
несовместимы.

Сторона Z80 - SDCC и командные файлы, CMake там не используется:

```
scripts\build_plugin.bat      ->  release\UMPLAYER.WMF и SD\WC\UMPLAYER.WMF
scripts\build_trdos.bat       ->  release\ugly.$c, release\ugly.sna
scripts\build_config_rom.bat  ->  ПЗУ конфигуратора
scripts\build_nav_rom.bat     ->  ПЗУ меню образов дисков
scripts\build_z80fw.bat       ->  тесты библиотеки для эмулятора
```

ПЗУ конфигуратора и меню образов дисков входят в прошивку: скрипты пишут
их заголовками `rom_image_config.h` и `rom_image_nav.h` в
`backend/ports/rp2350/hostlink/rom_images/`. Заголовки лежат в
репозитории, и `build.bat` их не пересобирает. После правки
`frontend/config_rom/` или `frontend/nav_rom/` запустить нужный скрипт, а
затем собрать прошивку - иначе в неё попадёт прежний образ.

`build_plugin.bat` кладёт копию плагина в `SD\WC\`, этот каталог должен
существовать.

## Запись на плату

```
scripts\flash_zc.cmd             собрать и записать прошивку по SWD
scripts\flash_zc.cmd bootsel     то же через кнопку BOOTSEL
scripts\flash_bank.cmd           записать банк отдельной областью
scripts\flash_bank.cmd <файл>    записать другой банк .ssb
```

`flash_zc.cmd` собирает прошивку в `build/zc` и записывает её: по умолчанию
по SWD из `.elf` через OpenOCD, с аргументом `bootsel` - `.uf2` через
picotool, когда плата в режиме BOOTSEL. `flash_plain.cmd` делает то же в
каталоге `build/plain`; образ получается тот же, потому что Z-Controller
теперь включается настройкой, а не сборкой.

`flash_bank.cmd` пишет банк по SWD, по умолчанию
`release\banks\GeneralUser-GS.ssb`; это занимает минуты. Банк лежит во
флеш-памяти выше 1 МБ, и обычная запись прошивки его не затрагивает -
перезаписывать после каждой сборки не нужно.

## Проверка

```
build/pc/tests/Release/soundsinth_tests.exe
```

Запускать из корня репозитория: тесты читают модули из `SD/test_music/`,
а тест FatFs - образ карты `build/sd.img` (`scripts\build_sd.bat`).

После правок движка к нему добавляется побайтовое сравнение рендеринга:

```
build/pc/app/Release/pc_player.exe --play-file SD/test_music/it/trch-mus__v32.it --render-wav out.wav
```

`pc_player` так же рендерит `.mid`, `.vgm` и `.vgz`. Результат
сравнивается побайтно с рендерингом до правки. Сравнивать сами
`.bin`/`.elf` для этого нельзя: перемещение файлов меняет порядок объектных
модулей при компоновке, и содержимое расходится без единой смысловой
правки.

Линтер clang-tidy по коду платы - `node scripts/tools/lint.js`, ему нужна
собранная прошивка `build/zc` ([scripts/README.md](../scripts/README.md)).
