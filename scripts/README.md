# scripts

Всё, что запускается вручную. Каждый скрипт можно вызывать из любого
каталога - путь до корня они вычисляют сами.

| Скрипт | Что делает |
|---|---|
| `build.bat` | полная сборка: прошивки в `release/picard/`, программы Z80 в `release/` |
| `build_plugin.bat` | плагин Wild Commander -> `release/UMPLAYER.WMF` и `SD/WC/` |
| `build_trdos.bat` | приложение TR-DOS -> `release/UNIMOD.$C`, `release/UNIMOD.sna` |
| `build_z80fw.bat` | тесты библиотеки Z80 -> `frontend/z80_fw/obj/*.bin` |
| `build_sd.bat` | образ SD-карты из `SD/` -> `build/sd.img` |
| `flash_zc.cmd` | собрать и записать на плату прошивку с эмуляцией Z-Controller |
| `flash_plain.cmd` | то же без неё |
| `flash_bank.cmd` | записать банк инструментов отдельной областью флеш-памяти |

Прошивка пишется по SWD через отладочный щуп. Аргумент `bootsel` у
`flash_zc.cmd` и `flash_plain.cmd` переключает на запись через кнопку
BOOTSEL, когда щупа нет под рукой.

Подкаталог `tools/` - то, что эти скрипты зовут: упаковщики образов
(`ihx2wmf.js`, `mkhobeta.js`, `mksna.js`, `cutbin.js`,
`uf2_embed_bank.js`), запись во флеш (`flash_swd.cmd`, `flash_uf2.cmd`) и
конвертация банков (`bake_banks.js`). Отдельно их звать не нужно.

`robimg.exe` - сборщик образов FAT, его зовёт `build_sd.bat`.

`node scripts/tools/lint.js` - линтер clang-tidy по коду платы (ядро,
прошивка, платформа) с правилами из `.clang-tidy` в корне. Нужны LLVM и
собранная прошивка `build/zc`: флаги компиляции берутся из её базы.
Аргументами можно дать каталог или файлы, `--fix` правит на месте.

Хостовая сборка (плеер, тесты, препроцессор банков) скриптом не обёрнута -
это обычные две команды CMake, см. `docs/build.md`.
