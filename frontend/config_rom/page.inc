;; page.inc - страница конфигуратора глазами машины: экран-терминал.
;;
;; Раскладка обязана совпадать с core/config/config_term.h платы. Порядок
;; байт у обоих младшим вперёд.

PAGE            .equ    0x2000

TM_UPD          .equ    6
TM_SHOWN        .equ    7
TM_REQ          .equ    8
TM_ACK          .equ    9
TM_COUNT        .equ    10
TM_KEYS         .equ    12
TM_SPANS        .equ    20
TM_CHARS        .equ    PAGE + 0x100
TM_ATTRS        .equ    PAGE + 0x400
TM_STARTS       .equ    PAGE + 0x700    ; счётчик стартов: пишет только ПЗУ
TM_KEY_SLOTS    .equ    8
TM_ALL          .equ    0xFF
