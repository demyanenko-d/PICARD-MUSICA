// SPDX-License-Identifier: MIT
#pragma once

// Микрослово КР1818ВГ93, разобранное в действия: таблица на 256 адресов,
// общая для ядер.

#include <array>
#include <cstdint>

#include "vg93_rom.h"

namespace devices::bdi::ucode {

// --- Действия микрослова ---

enum : uint64_t {
    A_JUMP_LO       = 1ull << 0, // переход грузит младший полубайт PC
    A_JUMP_HI       = 1ull << 1, // и старший
    A_EI            = 1ull << 2, // команда D0h..DFh уводит на 002
    A_JUMP_ALWAYS   = 1ull << 3, // rw = 110
    A_CNT1_LOAD     = 1ull << 4,
    A_CNT2_LOAD     = 1ull << 5,
    A_BYTE_SYNC     = 1ull << 6, // mod = 100: байтовый строб в этом слове
    A_CNT2_BYTE     = 1ull << 7,
    A_CNT2_STEP     = 1ull << 8,
    A_SER_INC       = 1ull << 9,
    A_INC_UP        = 1ull << 10,
    A_CMP           = 1ull << 11,
    A_READ          = 1ull << 12, // слово читает диск
    A_ID_CAPTURE    = 1ull << 13,
    A_WRITE         = 1ull << 14, // слово пишет на диск
    A_SET_TYPE1     = 1ull << 15,
    A_CLR_TYPE1     = 1ull << 16,
    A_SET_BUSY      = 1ull << 17,
    A_CLR_BUSY      = 1ull << 18,
    A_SET_CRC_ERR   = 1ull << 19,
    A_CLR_CRC_ERR   = 1ull << 20,
    A_SET_RNF       = 1ull << 21,
    A_SET_WRT_FAULT = 1ull << 22,
    A_SET_WRT_PROT  = 1ull << 23,
    A_SET_LOST      = 1ull << 24,
    A_INT_WORD      = 1ull << 25,
    A_SET_HLD       = 1ull << 26,
    A_CLR_HLD       = 1ull << 27,
    A_STEP          = 1ull << 28,
    A_SET_DIRC      = 1ull << 29,
    A_CLR_DIRC      = 1ull << 30,
    A_DIRC_CMP      = 1ull << 31,
    A_DET_A1        = 1ull << 33,
    A_DET_TRANS     = 1ull << 34,
    A_DET_READ_AM   = 1ull << 35,
    A_CNT1_DEC      = 1ull << 36,
    A_CNT1_SHIFT    = 1ull << 37,
    A_ID_COUNT      = 1ull << 38,
    A_SYNC_MARK     = 1ull << 39, // байтовый строб по найденной метке
    A_SYNC_PREAMBLE = 1ull << 40,
    A_TRK_SHIFT     = 1ull << 41,
    A_SEC_SHIFT     = 1ull << 42,
    A_READ_BYTE     = 1ull << 43,
    A_WRITE_BYTE    = 1ull << 44,
    A_FORMAT        = 1ull << 45,
    A_CRC_FFFF      = 1ull << 46,
    A_CRC_A1        = 1ull << 47,
    A_CLOCK_DROP    = 1ull << 48,
    A_CNT1_INDEX    = 1ull << 49, // rw[2] = 1: CNT1 считает индексные импульсы
    A_TRK_FILL_ONE  = 1ull << 50, // TRK_SH1
    A_TRK_FILL_INC  = 1ull << 51, // TRK_ROL
    A_SEC_FILL_RD   = 1ull << 52, // SEC_READ; SEC_ROL - выход инкрементора
};

// Условие COND микрослова.
enum : uint8_t {
    T_NONE,
    T_ALWAYS,
    T_NOT_READY,
    T_TR00_OFF_OR_IN,
    T_NOT_WPRT,
    T_HEAD_NOT_LOADED,
    T_RD_ZERO,
    T_WRITE_DRQ_OFF,
    T_ID_NONZERO,
    T_BYTE_STB,
    T_CMP_DIFFER,
    T_CRC_NONZERO,
    T_CNT1_RUNNING,
    T_OP_BIT_CLEAR,
};
// Слово с управляющим полем стоит на себе, пока ...
enum : uint8_t { S_NONE, S_PREAMBLE, S_UNTIL_INDEX, S_CNT1, S_UNTIL_BYTE, S_CNT2, S_BYTE_CNT2 };
// Когда слово пропускает следующее (кроме найденной метки и F7h в FORMAT).
enum : uint8_t { K_NONE, K_NEW_CMD, K_PREAMBLE_BYTE, K_PREAMBLE };
// Бит, который пишет слово записи.
enum : uint8_t { WR_CRC, WR_SHIFT, WR_ZERO, WR_ONE };
// Бит, который берёт сравнение: 1, старший разряд сдвига данных, бит чтения.
enum : uint8_t { CMP_NONE, CMP_ONE, CMP_SHIFT, CMP_RD };

enum : unsigned {
    C_SET_LOST        = 0x00,
    C_READY_TST       = 0x01,
    C_TR0_TST         = 0x02,
    C_CLK_DROP        = 0x03,
    C_TRK_ROL         = 0x04,
    C_TRK_SH0         = 0x06,
    C_TRK_SH1         = 0x07,
    C_HLD_HLT_TST     = 0x08,
    C_SET_BASE        = 0x0a,
    C_SET_HLD         = 0x0b,
    C_CLR_HLD_INT     = 0x0c,
    C_CNT1_TST_INT    = 0x0d,
    C_CLR_HLD         = 0x0e,
    C_RDATA_TST       = 0x0f,
    C_READ            = 0x11,
    C_SEC_READ        = 0x12,
    C_READ_AM         = 0x13,
    C_STEP            = 0x16,
    C_TRANS_AM        = 0x17,
    C_A1_DET_CRC      = 0x18,
    C_WR_DRQ_TST      = 0x19,
    C_GOTO            = 0x1a,
    C_PREAM_STB_TST   = 0x1b,
    C_WPRT_TST        = 0x1c,
    C_PREAM_INI_TST   = 0x1d,
    C_OP0             = 0x20,
    C_OP7             = 0x27,
    C_SET_BUSY        = 0x28,
    C_ID_CNT_CALC     = 0x2a,
    C_SET_CRC_ERR     = 0x2b,
    C_SET_REC_NOT_FND = 0x2c,
    C_SET_WRT_ERR     = 0x2d,
    C_SET_WRT_PRT     = 0x2e,
    C_A1_DET          = 0x2f,
    C_CLR_BUSY        = 0x30,
    C_CRC_CLR         = 0x31,
    C_SEC_ROL         = 0x32,
    C_CLR_CRC_ERR     = 0x33,
    C_CLR_DIRC        = 0x34,
    C_UPD_DIRC        = 0x35,
    C_SET_DIRC        = 0x36,
    C_CMP_MATCH       = 0x37,
    C_WRITE_STB_TST   = 0x38,
    C_WRITE           = 0x3a,
    C_FORMAT          = 0x3b,
    C_CNT1_TST_DEC    = 0x3c,
    C_CNT1_TST        = 0x3d,
    C_CNT1_DEC        = 0x3e,
};

struct Act {
    uint64_t f;
    uint8_t test, stay, skip;
    uint8_t cmp_op : 2, wsrc : 2; // одним байтом: запись 16 байт
    uint8_t preset;               // ~imm: начальное значение CNT1 / CNT2
    uint8_t jump;                 // ~{mod[0], hi, imm}: адрес перехода
    uint8_t op_bit;               // OP0..OP7
    bool read_stay;               // слово ожидания в режиме чтения: быстрый путь
};
static_assert(sizeof(Act) == 16, "Act is 16 bytes");

// Поля: hi 18..16, cmd 15..10, mod 9..7, rw 6..4, imm 3..0.
constexpr Act decode(uint32_t w) {
    const unsigned hi = (w >> 16) & 7, cmd = (w >> 10) & 0x3f, mod = (w >> 7) & 7, rw = (w >> 4) & 7, imm = w & 15;
    Act a{};
    a.jump     = static_cast<uint8_t>(~(((mod & 1) << 7) | (hi << 4) | imm));
    a.preset   = static_cast<uint8_t>(~imm & 15);
    a.op_bit   = static_cast<uint8_t>(cmd & 7);
    a.cmp_op   = imm & 3;
    a.wsrc     = rw & 3;
    uint64_t f = 0;
    switch (mod) {
        case 0:
            f |= A_CNT1_LOAD;
            break;
        case 1:
            f |= A_CNT2_LOAD;
            break;
        case 2:
        case 3:
            f |= A_JUMP_HI | A_JUMP_LO;
            break;
        case 4:
            f |= A_JUMP_LO | A_BYTE_SYNC;
            break;
        case 5:
            f |= A_EI;
            break;
        case 6:
            f |= A_JUMP_LO | A_EI;
            break;
        default:
            break;
    }
    // mod = 1x1: imm - управляющее поле.
    if ((mod & 5) == 5) {
        switch (imm) {
            case 4:
            case 5:
            case 6:
            case 7:
                f |= A_CMP;
                break;
            case 8:
                f |= A_CNT2_BYTE | A_SER_INC;
                break;
            case 9:
                f |= A_CNT2_STEP;
                break;
            case 10:
                f |= A_CNT2_BYTE;
                break;
            case 12:
                f |= A_CNT2_BYTE | A_SER_INC | A_INC_UP;
                break;
            default:
                break;
        }
        switch (imm) {
            case 0:
                a.stay = S_PREAMBLE;
                break;
            case 2:
                a.stay = S_UNTIL_INDEX;
                break;
            case 3:
                a.stay = S_CNT1;
                break;
            case 6:
            case 7:
            case 14:
                a.stay = S_UNTIL_BYTE;
                break;
            case 8:
            case 10:
            case 12:
                a.stay = S_BYTE_CNT2;
                break;
            case 9:
            case 11:
            case 13:
                a.stay = S_CNT2;
                break;
            default:
                break;
        }
    }
    switch (rw) {
        case 4:
            f |= A_READ | A_ID_CAPTURE;
            break;
        case 5:
            f |= A_READ;
            break;
        case 6:
            f |= A_JUMP_ALWAYS;
            break;
        case 7:
            break;
        default:
            f |= A_WRITE;
            break;
    }
    if ((rw & 4) != 0) f |= A_CNT1_INDEX;
    switch (cmd) {
        case C_SET_BASE:
            f |= A_CLR_TYPE1;
            break;
        case C_SET_BUSY:
            f |= A_SET_BUSY | A_SET_TYPE1;
            break;
        case C_CLR_BUSY:
            f |= A_CLR_BUSY;
            break;
        case C_SET_CRC_ERR:
            f      |= A_SET_CRC_ERR;
            a.test  = T_CRC_NONZERO;
            break;
        case C_CLR_CRC_ERR:
            f |= A_CLR_CRC_ERR;
            break;
        case C_SET_REC_NOT_FND:
            f |= A_SET_RNF;
            break;
        case C_SET_WRT_ERR:
            f |= A_SET_WRT_FAULT;
            break;
        case C_SET_WRT_PRT:
            f |= A_SET_WRT_PROT;
            break;
        case C_SET_LOST:
            f |= A_SET_LOST;
            break;
        case C_SET_HLD:
            f |= A_SET_HLD;
            break;
        case C_CLR_HLD:
            f |= A_CLR_HLD;
            break;
        case C_CLR_HLD_INT:
            f      |= A_CLR_HLD | A_INT_WORD;
            a.skip  = K_NEW_CMD;
            break;
        case C_CNT1_TST_INT:
            f      |= A_INT_WORD;
            a.test  = T_CNT1_RUNNING;
            a.skip  = K_NEW_CMD;
            break;
        case C_HLD_HLT_TST:
            a.test = T_HEAD_NOT_LOADED;
            break;
        case C_STEP:
            f |= A_STEP;
            break;
        case C_CLR_DIRC:
            f |= A_CLR_DIRC;
            break;
        case C_UPD_DIRC:
            f |= A_DIRC_CMP;
            break;
        case C_SET_DIRC:
            f |= A_SET_DIRC;
            break;
        case C_GOTO:
            a.test = T_ALWAYS;
            break;
        case C_READY_TST:
            a.test = T_NOT_READY;
            break;
        case C_TR0_TST:
            a.test = T_TR00_OFF_OR_IN;
            break;
        case C_ID_CNT_CALC:
            a.test = T_ID_NONZERO;
            if ((hi & 1) != 0) f |= A_CNT1_SHIFT;
            if (rw != 4) f |= A_ID_COUNT;
            break;
        case C_CMP_MATCH:
            a.test = T_CMP_DIFFER;
            break;
        case C_TRK_ROL:
            f |= A_TRK_SHIFT | A_TRK_FILL_INC;
            break;
        case C_TRK_SH0:
            f |= A_TRK_SHIFT;
            break;
        case C_TRK_SH1:
            f |= A_TRK_SHIFT | A_TRK_FILL_ONE;
            break;
        case C_SEC_ROL:
            f |= A_SEC_SHIFT;
            break;
        case C_CNT1_TST:
            a.test = T_CNT1_RUNNING;
            break;
        case C_CNT1_TST_DEC:
            f      |= A_CNT1_DEC;
            a.test  = T_CNT1_RUNNING;
            break;
        case C_CNT1_DEC:
            f |= A_CNT1_DEC;
            break;
        case C_CRC_CLR:
            f |= A_CRC_FFFF;
            break;
        case C_WPRT_TST:
            a.test = T_NOT_WPRT;
            break;
        case C_WR_DRQ_TST:
            a.test = T_WRITE_DRQ_OFF;
            break;
        case C_WRITE:
            f |= A_WRITE_BYTE;
            break;
        case C_WRITE_STB_TST:
            f      |= A_WRITE_BYTE;
            a.test  = T_BYTE_STB;
            break;
        case C_FORMAT:
            f |= A_WRITE_BYTE | A_FORMAT;
            break;
        case C_READ:
            f |= A_READ_BYTE;
            break;
        case C_SEC_READ:
            f |= A_READ_BYTE | A_SEC_SHIFT | A_SEC_FILL_RD;
            break;
        case C_READ_AM:
            f |= A_READ_BYTE | A_DET_A1 | A_DET_READ_AM | A_SYNC_MARK;
            break;
        case C_RDATA_TST:
            a.test = T_RD_ZERO;
            break;
        case C_CLK_DROP:
            f |= A_CLOCK_DROP;
            break;
        case C_TRANS_AM:
            f |= A_DET_TRANS;
            break;
        case C_A1_DET:
            f |= A_DET_A1;
            break;
        case C_A1_DET_CRC:
            f |= A_DET_A1 | A_CRC_A1 | A_SYNC_MARK;
            break;
        case C_PREAM_STB_TST:
            a.skip = K_PREAMBLE_BYTE;
            break;
        case C_PREAM_INI_TST:
            f      |= A_SYNC_PREAMBLE;
            a.skip  = K_PREAMBLE;
            break;
        default:
            if (cmd >= C_OP0 && cmd <= C_OP7) a.test = T_OP_BIT_CLEAR;
            break;
    }
    a.f = f;
    // Ждёт счётчика или байта, читая диск, и больше ничего не делает.
    constexpr uint64_t kReadStay = A_EI | A_READ | A_CNT1_INDEX | A_SYNC_PREAMBLE | A_READ_BYTE | A_CNT2_BYTE | A_DET_A1 | A_DET_TRANS | A_DET_READ_AM |
                                   A_SYNC_MARK | A_CRC_A1 | A_CRC_FFFF;
    const bool stays = a.stay != S_NONE;
    a.read_stay      = (mod == 5 || mod == 7) && rw == 5 && a.test == T_NONE && (a.skip == K_NONE || a.skip == K_PREAMBLE) && stays && (f & ~kReadStay) == 0;
    return a;
}

constexpr std::array<Act, 256> make_acts() {
    std::array<Act, 256> t{};
    for (unsigned i = 0; i < 256; i++) {
        t[i] = decode(kVg93Rom[i]);
    }
    return t;
}

inline constexpr std::array<Act, 256> kActs = make_acts();
inline constexpr Act kActZero               = decode(0); // слово в регистре до включения

// Слово, в конце которого могут смениться INTRQ или DRQ.
constexpr bool pins_word(const Act& a) {
    return (a.f & (A_CLR_BUSY | A_SET_BUSY | A_INT_WORD | A_READ_BYTE | A_WRITE_BYTE)) != 0;
}

// Слово, которое меняет регистр снаружи помимо выводов: флаги статуса,
// TYPE1, HLD (он же READY), шаг (TR00 следующего слова), TRK, SEC.
inline constexpr uint64_t kVisOnly = A_SET_TYPE1 | A_CLR_TYPE1 | A_SET_CRC_ERR | A_CLR_CRC_ERR | A_SET_RNF | A_SET_WRT_FAULT | A_SET_WRT_PROT | A_SET_LOST |
                                     A_SET_HLD | A_CLR_HLD | A_STEP | A_TRK_SHIFT | A_SEC_SHIFT;

// Сколько концов слов подряд с этого адреса заведомо не сменят INTRQ и DRQ
// (vis - и регистры): путь до ближайшего такого слова по переходам +1, +2
// (пропуск), адресу перехода (полубайты от PC + 0/1/2); with_ei - и вектор
// 002 слов EI (команда D0h..DFh в регистре). TYPE1 ставится по следующему
// слову в конце текущего: слово перед таким - уже со сменой.
constexpr std::array<uint8_t, 256> make_pin_dist(bool with_ei, bool vis = false) {
    std::array<uint8_t, 256> d{};
    for (unsigned i = 0; i < 256; i++) {
        d[i] = pins_word(kActs[i]) || (vis && (kActs[i].f & kVisOnly) != 0) ? 0 : 255;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = 0; i < 256; i++) {
            if (d[i] == 0) continue;
            const Act& a    = kActs[i];
            unsigned best   = 255;
            bool pre        = false;
            const auto take = [&](unsigned n) {
                if (d[n & 255] < best) best = d[n & 255];
                if (vis && (kActs[n & 255].f & (A_SET_TYPE1 | A_CLR_TYPE1)) != 0) pre = true;
            };
            take(i + 1);
            take(i + 2);
            if ((a.f & (A_JUMP_LO | A_JUMP_HI)) != 0) {
                for (unsigned inc = 0; inc < 3; inc++) {
                    const unsigned seq = (i + inc) & 255;
                    take((((a.f & A_JUMP_HI) != 0 ? a.jump : seq) & 0xf0u) | (((a.f & A_JUMP_LO) != 0 ? a.jump : seq) & 0x0fu));
                }
            }
            if (with_ei && (a.f & A_EI) != 0) take(2);
            const unsigned nd = pre ? 0 : best >= 254 ? 255 : best + 1;
            if (nd < d[i]) {
                d[i]    = static_cast<uint8_t>(nd);
                changed = true;
            }
        }
    }
    return d;
}

inline constexpr std::array<uint8_t, 256> kPinDist   = make_pin_dist(false);
inline constexpr std::array<uint8_t, 256> kPinDistEi = make_pin_dist(true);
inline constexpr std::array<uint8_t, 256> kVisDist   = make_pin_dist(false, true);
inline constexpr std::array<uint8_t, 256> kVisDistEi = make_pin_dist(true, true);

} // namespace devices::bdi::ucode
