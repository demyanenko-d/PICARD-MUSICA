        .module wc_prm_pl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_prm_pl
_wc_prm_pl::
        ex      af, af'
        push    ix
        ld      a, #0x53
        call    WC_ENTRY
        ; Z=нет параметра -> вернуть 0xFF, иначе A=номер опции
        jr      nz, _wc_prm_pl_end
        ld      a, #0xFF
_wc_prm_pl_end:
        pop     ix
        ret
