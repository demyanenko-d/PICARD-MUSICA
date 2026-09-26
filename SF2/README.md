# SF2

Оригинальные банки SoundFont, из которых печётся всё, что уходит в релиз,
и их лицензии.

| Файл | Банк | Автор |
|---|---|---|
| `GeneralUser-GS.sf2` | GeneralUser GS 2.0.3 BETA | S. Christian Collins |
| `SGM-V2.01.sf2` | SGM-V2.01 | Shan |
| `Timbres of Heaven (XGM) 4.00(G).sf2` | Timbres of Heaven | Don Allen |

Сами `.sf2` в репозиторий не идут — качаются отдельно. Коммитятся только
лицензии и этот файл.

## Что из них получается

```
node scripts/tools/bake_banks.js
```

Кладёт в `release/banks/`: `GeneralUser-GS.ssb` (вшивается в образ
прошивки), `SGM.ssb` и `Timbres-of-Heaven.ssb` (для SD-карты). Параметры
конвертации заданы кодом там же, в `scripts/tools/bake_banks.js`.


## Лицензии

Рядом с каждым банком файл `*.LICENSE.txt`: условия использования и
сведения об авторстве из чанка INFO исходного `.sf2`.

| Банк | Лицензия |
|---|---|
| GeneralUser GS | GeneralUser GS License v2.0 |
| SGM-V2.01 | David Shan 2002-2007 |
| Timbres of Heaven | GNU GPL 2.0 |

Эти файлы `scripts/build.bat` кладёт в `release/banks/` рядом с банками.
