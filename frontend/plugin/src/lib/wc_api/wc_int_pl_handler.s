        .module wc_int_pl_handler
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_int_pl_handler
_wc_int_pl_handler::
        push    ix
        ; HL = handler_addr (sdcccall(1); WC тоже нужен HL)
        ld      a, #0xFF
        ex      af, af'                 ; A' = 0xFF
        ld      a, #0x56
        call    WC_ENTRY
        pop     ix
        ret
