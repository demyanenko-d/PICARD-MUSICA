;; fw_glue.s - переходники между z80_fw и соглашением вызовов SDCC.
;;
;; Конфигуратору из рамки нужны только экран и клавиатура, и почти всё в
;; них берёт аргумент там, где его кладёт SDCC. Переходник нужен одному
;; вызову.

        .module fw_glue

        .globl  _fw_scr_at
        .globl  _fw_scr_at_hl

        .area   _CODE

;; void fw_scr_at(u8 x, u8 y) - SDCC кладёт x в A, y в L; библиотека ждёт
;; координаты в H и L.
_fw_scr_at::
        ld      h, a
        jp      _fw_scr_at_hl
