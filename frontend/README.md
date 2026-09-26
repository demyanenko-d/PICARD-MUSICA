# frontend - сторона ZX Spectrum

Код, выполняющийся на Z80: выбор файла, отдача его байтов по шине на
плату, экран.

## Каталоги

| Каталог | Что это |
|---|---|
| `common/` | клиент шины, общий для обоих приложений |
| `plugin/` | плагин WildCommander |
| `trdos/` | приложение TR-DOS, свой навигатор и работа с SD через `z80_fw` |
| `z80_fw/` | библиотека на ассемблере: SD, FAT, каталоги, экран, клавиатура, прерывания |

## Сборка

Скрипты лежат в `scripts/`, исходники - здесь.

| Скрипт | Результат |
|---|---|
| `scripts\build_plugin.bat` | `release\UMPLAYER.WMF` |
| `scripts\build_trdos.bat` | `release\UNIMOD.$C`, `release\UNIMOD.sna` |
| `scripts\build_z80fw.bat` | `z80_fw\obj\*.bin` - тесты для эмулятора |

`scripts\build.bat` вызывает первые два вместе с прошивкой.
Промежуточные файлы - в `obj\` рядом с исходниками.

## Зависимости

- SDCC не ниже 4.5.0 - `sdcc`, `sdasz80`, `sdar`, `makebin`.
- Node.js - упаковщики из `scripts/tools/`: `ihx2wmf.js`, `cutbin.js`,
  `mkhobeta.js`, `mksna.js`.

