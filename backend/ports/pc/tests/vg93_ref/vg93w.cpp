// SPDX-License-Identifier: MIT
// КР1818ВГ93 с точностью до микрослова. Слово исполняется целиком в момент
// своего конца: условие и пропуск - от регистров конца прошлого слова,
// входы дисковода - в этот момент, затем все регистры берут следующие
// значения.
#include "vg93w.h"

#include <cstring>

#include "platform/compiler.h"

#include "vg93_ucode.h"

namespace devices::bdi {

// Мнемоники микрокода (A_*, C_*, T_*, S_*, K_*, WR_*, CMP_*) - на каждом
// шагу слова, без квалификатора.
using namespace ucode;

// Кэш XIP 16 КБ на всю прошивку: крупные редкие пути - по размеру (на плате
// чтение при опросе 10 мкс 38 -> 22% реального времени, форматирование (F4) 27 -> 15%).
// Пропуски тоже: на 2.6 КБ меньше, по тактам M33 не медленнее.
#if defined(__GNUC__) && !defined(__clang__)
#define VG93W_SMALL __attribute__((optimize("Os")))
#else
#define VG93W_SMALL
#endif
#if defined(__GNUC__)
#define VG93W_INLINE_LAMBDA __attribute__((always_inline))
#else
#define VG93W_INLINE_LAMBDA
#endif

namespace {

constexpr uint8_t kStepLoad = 249; // делитель шагов: 250 слов на тик

// capture_ и enable_: разряд на регистр хоста.
constexpr uint8_t kRegCmdBit  = 0x01;
constexpr uint8_t kRegTrkBit  = 0x02;
constexpr uint8_t kRegSecBit  = 0x04;
constexpr uint8_t kRegDataBit = 0x08;
// int, как у литерала: с uint8_t GCC собирает тернарный выбор маски иначе.
constexpr int kRegNoDataMask = 0x07;
constexpr int kRegAllMask    = 0x0f;

// flags_: внутренние флаги статуса. В регистре статуса CRC и RNF, WRITE
// FAULT и WRITE PROT стоят на два разряда выше.
constexpr uint8_t kFlBusyBit     = 0x01;
constexpr uint8_t kFlCrcBit      = 0x02;
constexpr uint8_t kFlRnfBit      = 0x04;
constexpr uint8_t kFlWrtFaultBit = 0x08;
constexpr uint8_t kFlWrtProtBit  = 0x10;
static_assert(kFlBusyBit == kStBusyBit && (kFlCrcBit << 2) == kStCrcBit && (kFlRnfBit << 2) == kStRnfBit && (kFlWrtFaultBit << 2) == kStWrtFaultBit &&
                  (kFlWrtProtBit << 2) == kStWprotBit,
              "status bits are flags_ shifted by 2");

constexpr uint8_t wired_and(uint8_t hold, bool a_en, uint8_t a, bool b_en, uint8_t b) {
    return static_cast<uint8_t>((a_en ? a : 0xff) & (b_en ? b : 0xff) & ((a_en || b_en) ? 0xff : hold));
}

// CRC-16 CCITT: восемь шагов сдвига с нулевым входом.
struct CrcTab {
    uint16_t v[256];
};
constexpr CrcTab make_crc_tab() {
    CrcTab t{};
    for (unsigned i = 0; i < 256; i++) {
        auto r = static_cast<uint16_t>(i << 8);
        for (int k = 0; k < 8; k++) {
            r = static_cast<uint16_t>((r << 1) ^ ((r & 0x8000) != 0 ? 0x1021u : 0));
        }
        t.v[i] = r;
    }
    return t;
}
constexpr CrcTab kCrcZeros = make_crc_tab();

uint16_t crc_zeros(uint16_t crc, uint32_t n) {
    n %= 32767; // период нулевого входа CRC-CCITT
    for (; n >= 8; n -= 8) {
        crc = static_cast<uint16_t>((crc << 8) ^ kCrcZeros.v[crc >> 8]);
    }
    for (; n > 0; n--) {
        crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000) != 0 ? 0x1021u : 0));
    }
    return crc;
}

bool is_preamble(uint32_t w) {
    return w == 0x0aaaau || w == 0x15555u;
}

inline uint32_t rev32(uint32_t x) {
#if defined(__arm__)
    uint32_t r;
    __asm("rbit %0, %1" : "=r"(r) : "r"(x));
    return r;
#else
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
    x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
    return (x >> 16) | (x << 16);
#endif
}

// len <= 25 ячеек кольца, кончающихся в last, новейшая - разряд 0; без дорожки - 0.
uint32_t cells_to(const uint8_t* track, uint32_t n, uint32_t last, unsigned len) {
    if (track == nullptr) return 0;
    const uint32_t first = last + 1 >= len ? last + 1 - len : last + 1 + n - len;
    if (first + len <= n && (first >> 3) + 4 <= ((n + 7) >> 3)) {
        const uint8_t* p = track + (first >> 3);
        const uint32_t x =
            static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        return rev32(x >> (first & 7)) >> (32 - len);
    }
    uint32_t w = 0;
    for (uint32_t k = 0, q = first; k < len; k++, q = q + 1 >= n ? 0 : q + 1) {
        w = (w << 1) | static_cast<uint32_t>((track[q >> 3] >> (q & 7)) & 1u);
    }
    return w;
}

struct PreTab {
    uint32_t v[8];
};
constexpr bool uses_preamble_c(const Act& a) {
    return a.stay == S_PREAMBLE || a.skip == K_PREAMBLE || a.skip == K_PREAMBLE_BYTE || (a.f & A_SYNC_PREAMBLE) != 0;
}
constexpr PreTab make_pre() {
    PreTab t{};
    for (unsigned i = 0; i < 256; i++) {
        if (uses_preamble_c(kActs[i])) t.v[i >> 5] |= 1u << (i & 31);
    }
    return t;
}
constexpr PreTab kPre = make_pre();
inline bool uses_preamble_pc(unsigned i) {
    return ((kPre.v[i >> 5] >> (i & 31)) & 1u) != 0;
}

bool uses_preamble(const Act& a) {
    return a.stay == S_PREAMBLE || a.skip == K_PREAMBLE || a.skip == K_PREAMBLE_BYTE || (a.f & A_SYNC_PREAMBLE) != 0;
}

// Чётные ячейки байта дорожки (разряды 0, 2, 4, 6) - тетрада, разряд 0 - старший.
struct EvenRev {
    uint8_t v[256];
};
constexpr EvenRev make_even_rev() {
    EvenRev t{};
    for (unsigned i = 0; i < 256; i++) {
        t.v[i] = static_cast<uint8_t>(((i & 1) << 3) | ((i >> 2 & 1) << 2) | ((i >> 4 & 1) << 1) | (i >> 6 & 1));
    }
    return t;
}
constexpr EvenRev kEvenRevT       = make_even_rev();
constexpr const uint8_t* kEvenRev = kEvenRevT.v;

// Совпадения окна P (новейшая ячейка - разряд 0) по y в порядке дорожки:
// разряд b результата - окно из ячеек b .. b + 15 (b <= 16). Без bit13 -
// канальный бит 13 не сравнивается.
template <uint16_t P>
inline uint32_t match16(uint32_t y, bool bit13) {
    uint32_t r = ~0u;
#pragma GCC unroll 16
    for (int i = 0; i < 16; i++) {
        if (i == 2) continue;
        r &= ((P >> (15 - i)) & 1) != 0 ? (y >> i) : ~(y >> i);
    }
    if (bit13) r &= ((P >> 13) & 1) != 0 ? (y >> 2) : ~(y >> 2);
    return r;
}

} // namespace

// 17 ячеек, кончающихся в last_cell; новейшая - разряд 0.
uint32_t Vg93w::window(uint32_t last_cell) const {
    const SlotDrive* d = fdd;
    return cells_to(d->selected ? d->track : nullptr, d->slots, last_cell, 17);
}

bool Vg93w::mark_w(uint32_t w, bool a1, bool am, bool tr) const {
    const uint16_t m  = marker_bit13_bug ? static_cast<uint16_t>(~0x2000u) : 0xffffu;
    const uint16_t lo = static_cast<uint16_t>(w);
    return (a1 && (lo & m) == (0x4489 & m)) || (am && (lo & m) == (0x5224 & m)) || (tr && lo == 0x2555 && (w & 0x10000u) == 0);
}

bool Vg93w::sync_w(uint16_t lo) const {
    const uint16_t m = marker_bit13_bug ? static_cast<uint16_t>(~0x2000u) : 0xffffu;
    return (det_a1_ && (lo & m) == (0x4489 & m)) || (det_read_am_ && (lo & m) == (0x5224 & m));
}

void Vg93w::outputs() {
    intrq    = !no_intrq_;
    drq      = drq_l_;
    hld      = hld_pin();
    wg       = wg_q_;
    step_pin = step_q_;
    if (hlt_follows_hld) hlt = hld;
}

// Строб байта и следующий адрес слова a от регистров конца прошлого слова.
Vg93w::Branch Vg93w::branch(const Act& a) const {
    const uint64_t f  = a.f;
    const bool amp    = mark_found_;
    const bool rd     = rd_data_;
    const bool cnt1z  = cnt1_ == 0;
    const bool cnt2z  = cnt2_ == 0;
    const bool op_int = (op_ >> 4) == 0xd;
    Branch b{};
    b.ip_edge      = ip_sample_ && !ip_sample_d_;
    b.ser_inc_sync = !inc_run_ && (f & A_SER_INC) != 0;
    b.byte_stb = b.ser_inc_sync || (f & A_BYTE_SYNC) != 0 || ((f & A_SYNC_MARK) != 0 && amp) || ((f & A_SYNC_PREAMBLE) != 0 && preamble_) || bit_count_ == 7;
    b.fsr      = (f & A_FORMAT) != 0 && b.byte_stb && write_ready_;
    const bool byte_stb = b.byte_stb, ip_edge = b.ip_edge, fsr = b.fsr;
    const bool wdd = (cnt1z && cnt2z) || !write_ready_;
    bool test      = false;
    switch (a.test) {
        case T_ALWAYS:
            test = true;
            break;
        case T_NOT_READY:
            test = !ready_r_;
            break;
        case T_TR00_OFF_OR_IN:
            test = !tr00_r_ || dirc;
            break;
        case T_NOT_WPRT:
            test = !fdd->wprot_in();
            break;
        case T_HEAD_NOT_LOADED:
            test = !(hlt_s_ && hld_r_);
            break;
        case T_RD_ZERO:
            test = !rd;
            break;
        case T_WRITE_DRQ_OFF:
            test = wdd;
            break;
        case T_ID_NONZERO:
            test = id_bit0_ || id_bit1_;
            break;
        case T_BYTE_STB:
            test = byte_stb;
            break;
        case T_CMP_DIFFER:
            test = cmp_differ_;
            break;
        case T_CRC_NONZERO:
            test = crc_ != 0;
            break;
        case T_CNT1_RUNNING:
            test = !cnt1z;
            break;
        case T_OP_BIT_CLEAR:
            test = ((op_ >> a.op_bit) & 1) == 0;
            break;
        default:
            break;
    }
    bool stay = false;
    switch (a.stay) {
        case S_PREAMBLE:
            stay = preamble_;
            break;
        case S_UNTIL_INDEX:
            stay = !ip_edge;
            break;
        case S_CNT1:
            stay = !cnt1z;
            break;
        case S_UNTIL_BYTE:
            stay = !byte_stb;
            break;
        case S_CNT2:
            stay = !cnt2z;
            break;
        case S_BYTE_CNT2:
            stay = !byte_stb || !cnt2z;
            break;
        default:
            break;
    }
    const bool cond = test || stay || (f & A_JUMP_ALWAYS) != 0;
    bool skip       = amp || (data_ == 0xf7 && fsr);
    switch (a.skip) {
        case K_NEW_CMD:
            skip = skip || (enable_ & kRegCmdBit) != 0;
            break;
        case K_PREAMBLE_BYTE:
            skip = skip || (preamble_ && byte_stb);
            break;
        case K_PREAMBLE:
            skip = skip || preamble_;
            break;
        default:
            break;
    }

    // Следующий адрес.
    {
        const bool vec = (f & A_EI) != 0 && op_int;
        if (mr_s_ || vec) {
            b.next = vec ? 2 : 0;
        } else {
            const uint8_t seq = static_cast<uint8_t>(pc + (skip ? 2 : cond ? 0 : 1));
            const bool jump   = cond && !skip;
            const bool hi = jump && (f & A_JUMP_HI) != 0, lo = jump && (f & A_JUMP_LO) != 0;
            b.next = static_cast<uint8_t>(((hi ? a.jump : seq) & 0xf0) | ((lo ? a.jump : seq) & 0x0f));
        }
    }

    return b;
}

VG93W_SMALL void Vg93w::word(bool power_on) {
    const Act& a     = *act_;
    const uint64_t f = a.f;
    SlotDrive* dr    = fdd;
    const uint32_t n = dr->slots;

    // При /MR команда очищается до выбора адреса; защёлка прерывания видит
    // команду и до очистки, и после.
    const uint8_t op_old = op_;
    if (mr_s_) op_ &= 0x03;
    // Входы в конце слова.
    // Входы от HLD (выбор дисковода, READY) - уже с HLD этого слова.
    {
        const bool raw = (f & A_SET_HLD) != 0 || ((f & A_CLR_HLD) == 0 && hld_r_);
        if (dr->select_from_hld && raw != hld_r_) dr->selected = raw && !mr_s_;
    }
    const bool ready_now = ready_from_hld ? (((f & A_SET_HLD) != 0 || ((f & A_CLR_HLD) == 0 && hld_r_)) && !mr_s_) : fdd->ready;
    const bool tr00_now  = dr->tr00();
    const bool ip_now    = dr->index();

    // Сигналы слова от регистров конца прошлого слова.
    const bool pre_used     = preamble_;
    const uint8_t pc_now    = pc;
    const bool rd           = rd_data_;
    const bool cnt1z        = cnt1_ == 0;
    const bool cnt2z        = cnt2_ == 0;
    const Branch br         = branch(a);
    const bool ip_edge      = br.ip_edge;
    const bool ser_inc_sync = br.ser_inc_sync;
    const bool byte_stb     = br.byte_stb;
    const bool read_stb     = (f & A_READ_BYTE) != 0 && byte_stb;
    const bool write_stb    = (f & A_WRITE_BYTE) != 0 && byte_stb;
    const bool fsr          = br.fsr;
    const bool wdd          = (cnt1z && cnt2z) || !write_ready_;
    bool src                = true;
    switch (a.wsrc) {
        case WR_CRC:
            src = (crc_ & 0x8000) != 0;
            break;
        case WR_SHIFT:
            src = (data_shift_ & 0x80) != 0;
            break;
        case WR_ZERO:
            src = false;
            break;
        default:
            break;
    }
    const bool write_bit = (f & A_WRITE) != 0 && src;
    const bool mux       = ((track_ & 0x80) != 0 && (f & A_TRK_SHIFT) != 0) || ((sector_ & 0x80) != 0 && (f & A_SEC_SHIFT) != 0);
    const bool inc_out   = (inc_carry_ && byte_stb) != mux;
    bool cmp_bit         = false;
    switch (a.cmp_op) {
        case CMP_ONE:
            cmp_bit = true;
            break;
        case CMP_SHIFT:
            cmp_bit = (data_shift_ & 0x80) != 0;
            break;
        case CMP_RD:
            cmp_bit = rd;
            break;
        default:
            break;
    }
    const bool cmp_en  = (f & A_CMP) != 0;
    cmp_bit            = cmp_bit && cmp_en;
    const bool cmp_upd = cmp_en && !(cmp_en_d_ && cmp_differ_);
    const bool op_int  = (op_old >> 4) == 0xd;
    const uint8_t next = power_on ? pc : br.next;

    // Прерывание, DRQ, LOST.
    const bool set_busy    = (f & A_SET_BUSY) != 0;
    const bool o_write_rdy = write_ready_;
    const uint8_t o_enable = enable_;
    if ((f & (A_INT_WORD | A_CLR_BUSY | A_SET_BUSY | A_SET_LOST | A_READ_BYTE | A_WRITE_BYTE)) != 0) {
        const bool rdy_up = !ready_r_ && ready_now, rdy_dn = !ready_now && ready_r_;
        const bool cnd = (ip_edge && (op_old & 4) != 0) || (rdy_up && (op_old & 1) != 0) || (rdy_dn && (op_old & 2) != 0) || (op_old & 8) != 0;
        if ((cnd && (f & A_INT_WORD) != 0 && op_int) || ((!op_int || mr_s_) && (f & A_CLR_BUSY) != 0)) no_intrq_ = false;
        lost_ = (f & A_SET_LOST) != 0 || (write_stb && !write_ready_ && !type1_) || (!type1_ && read_stb && read_pend_) || (!set_busy && lost_);
        if (set_busy) {
            drq_l_ = false;
        } else if (((!wdd && write_stb) || read_stb) && !type1_) {
            drq_l_ = true;
        }
        write_ready_ = ((wdd || type1_ || !write_stb) && o_write_rdy) || (o_enable & kRegDataBit) != 0 || set_busy;
    } else if ((o_enable & kRegDataBit) != 0) {
        write_ready_ = true;
    }
    read_pend_ = drq_l_;

    // Статус.
    if ((f & (A_SET_BUSY | A_CLR_BUSY | A_SET_CRC_ERR | A_CLR_CRC_ERR | A_SET_RNF | A_SET_WRT_FAULT | A_SET_WRT_PROT)) != 0) {
        uint8_t fl = 0;
        if (((flags_ & kFlBusyBit) != 0 || set_busy) && (f & A_CLR_BUSY) == 0) fl |= kFlBusyBit;
        if (((flags_ & kFlCrcBit) != 0 && (f & A_CLR_CRC_ERR) == 0 && !set_busy) || (f & A_SET_CRC_ERR) != 0) fl |= kFlCrcBit;
        if (((flags_ & kFlRnfBit) != 0 && !set_busy) || (f & A_SET_RNF) != 0) fl |= kFlRnfBit;
        if (((flags_ & kFlWrtFaultBit) != 0 && !set_busy) || (f & A_SET_WRT_FAULT) != 0) fl |= kFlWrtFaultBit;
        if (((flags_ & kFlWrtProtBit) != 0 && !set_busy) || (f & A_SET_WRT_PROT) != 0) fl |= kFlWrtProtBit;
        flags_ = fl;
    }

    // Регистры.
    const bool o_mr_s = mr_s_;
    uint8_t new_data  = data_;
    if (o_enable != 0 || mr || o_mr_s || read_stb || (f & (A_TRK_SHIFT | A_SEC_SHIFT)) != 0) {
        new_data = wired_and(data_, (o_enable & kRegDataBit) != 0, dal_in_, read_stb, data_shift_);
        {
            const bool fill   = (f & A_TRK_FILL_ONE) != 0 || ((f & A_TRK_FILL_INC) != 0 && inc_out);
            const auto shift  = static_cast<uint8_t>((track_ << 1) | static_cast<unsigned>(fill));
            const uint8_t trk = wired_and(track_, (o_enable & kRegTrkBit) != 0, dal_in_, (f & A_TRK_SHIFT) != 0, shift);
            const bool sfill  = (f & A_SEC_FILL_RD) != 0 ? rd : inc_out;
            const auto sshift = static_cast<uint8_t>((sector_ << 1) | static_cast<unsigned>(sfill));
            uint8_t sec       = o_mr_s ? 0x01 : wired_and(sector_, (o_enable & kRegSecBit) != 0, dal_in_, (f & A_SEC_SHIFT) != 0, sshift);
            track_            = trk;
            if (mr) sec = 0x01;
            sector_ = sec;
        }
        {
            uint8_t cmd = op_;
            if (o_mr_s) cmd &= 0x03;
            if ((o_enable & kRegCmdBit) != 0) cmd = dal_in_;
            if (mr || o_mr_s) cmd |= 0x03;
            op_ = cmd;
        }
    }
    const bool ld_a1 = new_data == 0xf5 && fsr;
    const bool ld_c2 = data_ == 0xf6 && fsr;

    // Дорожка: ячейки слова.
    const uint32_t pos  = dr->pos;
    const uint32_t last = pos == 0 ? n - 1 : pos - 1;
    // 19 ячеек до last - 1, разряд k - ячейка last - 1 - k: бит слова (last - 2 или
    // last - 3), окна меток до last - 3 и last - 2, преамбула до last - 1.
    const uint8_t* trk_raw = dr->track;
    const bool sel_word    = dr->selected;
    const bool det_any     = det_a1_ || det_read_am_ || det_trans_ || det_a1_d_ || det_read_am_d_ || det_trans_d_;
    const Act& nx          = kActs[next];
    const bool need_cells  = (f & A_READ) != 0 || det_any || uses_preamble_pc(next);
    const uint32_t raw     = need_cells && sel_word ? cells_to(trk_raw, n, last == 0 ? n - 1 : last - 1, 19) : 0;
    const bool bit         = ((raw >> (clock_cell_ ? 2 : 1)) & 1u) != 0;
    if (wg_q_ && !dr->wprot) { // импульсы слова: синхроимпульс в прошлом слоте, бит данных в текущем; стирание - на границе слота
        const bool d2 = (data_pipe_ & 4) != 0, d3 = (data_pipe_ & 8) != 0, c2 = (clock_pipe_ & 16) != 0;
        const bool clk = c2 && !d2 && !d3;
        if (clk) dr->put(last, true);
        // Без PRE_WG половины ячейки не чередуются: и второе окно по правилу синхроимпульса.
        if (pre_wg_ ? d2 : clk) dr->put(pos, true);
    }
    // Метка в окне ячеек слова видна следующему слову.
    // Каждое окно - с детекторами слова, обсчитавшего его ячейку.
    const bool mark =
        mark_w((raw >> 2) & 0x1ffffu, det_a1_d_, det_read_am_d_, det_trans_d_) || mark_w((raw >> 1) & 0x1ffffu, det_a1_, det_read_am_, det_trans_);

    // CRC, сдвиговый регистр, тракт записи.
    const bool dec = (f & A_READ) != 0 && bit;
    if ((f & A_CRC_FFFF) != 0 || ld_a1 || (f & A_CRC_A1) != 0) {
        crc_ = (ld_a1 || (f & A_CRC_A1) != 0) ? 0x968b : 0xffff;
    } else {
        const bool in = (f & A_READ) != 0 ? rd : write_bit;
        crc_          = static_cast<uint16_t>((crc_ << 1) ^ ((in != ((crc_ & 0x8000) != 0)) ? 0x1021u : 0));
    }
    if (ld_a1) {
        data_shift_ = 0xa1;
    } else if (ld_c2) {
        data_shift_ = 0xc2;
    } else if (write_stb && o_write_rdy) {
        data_shift_ = data_;
    } else {
        data_shift_ = static_cast<uint8_t>((data_shift_ << 1) | static_cast<unsigned>(dec));
    }
    data_ = new_data;
    {
        const uint8_t mp  = mark_pipe_;
        const bool o_drop = clock_drop_;
        clock_drop_       = (mp & 4) != 0 || (f & A_CLOCK_DROP) != 0;
        mark_pipe_        = static_cast<uint8_t>((ld_a1 ? 1 : 0) | ((ld_c2 || (mp & 1) != 0) ? 2 : 0) | ((mp & 2) != 0 ? 4 : 0));
        clock_pipe_       = static_cast<uint8_t>(((clock_pipe_ << 1) | static_cast<unsigned>(!o_drop)) & 0x1f);
        data_pipe_        = static_cast<uint8_t>(((data_pipe_ << 1) | static_cast<unsigned>(write_bit)) & 0x0f);
        wg_q_             = pre_wg_;
        pre_wg_           = !no_write_d_;
        no_write_d_       = (f & A_WRITE) == 0;
    }

    // АЛУ, DIRC.
    if ((f & (A_SER_INC | A_CMP | A_DIRC_CMP | A_SET_DIRC | A_CLR_DIRC)) != 0 || inc_carry_ || cmp_en_d_) {
        const bool up        = (f & A_INC_UP) != 0 || dirc;
        const bool carry_p   = up == mux;
        inc_carry_           = (inc_carry_ && !byte_stb) || ser_inc_sync || (byte_stb && carry_p && inc_carry_ && (f & A_SER_INC) != 0);
        const bool o_greater = cmp_greater_;
        cmp_differ_          = ((cmp_bit != mux) && cmp_en) || (!cmp_upd && cmp_differ_);
        cmp_greater_         = (o_greater && !cmp_upd) || (cmp_upd && cmp_bit);
        cmp_en_d_            = cmp_en;
        dirc                 = (o_greater && (f & A_DIRC_CMP) != 0) || (f & A_SET_DIRC) != 0 || ((f & (A_DIRC_CMP | A_CLR_DIRC)) == 0 && dirc);
    }

    // Счётчики.
    if ((f & (A_CNT1_LOAD | A_CNT1_SHIFT | A_CNT1_DEC | A_CNT1_INDEX | A_CNT2_LOAD | A_CNT2_STEP | A_CNT2_BYTE | A_ID_CAPTURE | A_ID_COUNT)) != 0) {
        if ((f & A_CNT1_LOAD) != 0) {
            cnt1_ = a.preset;
        } else if ((f & A_CNT1_SHIFT) != 0) {
            cnt1_ = static_cast<uint8_t>(cnt1_ << 1);
        } else if ((f & A_CNT1_DEC) != 0 || ((f & A_CNT1_INDEX) != 0 && ip_edge)) {
            cnt1_ = static_cast<uint8_t>(cnt1_ - 1);
        }
        if ((f & A_CNT2_LOAD) != 0) {
            cnt2_ = a.preset;
        } else if (((f & A_CNT2_STEP) != 0 && !cnt2z && step_count_ == 0) || ((f & A_CNT2_BYTE) != 0 && byte_stb)) {
            cnt2_ = static_cast<uint8_t>((cnt2_ - 1) & 15);
        }
        if ((f & A_ID_CAPTURE) != 0) {
            id_bit1_ = id_bit0_;
            id_bit0_ = rd;
        } else if ((f & A_ID_COUNT) != 0) {
            if (!id_bit0_) id_bit1_ = !id_bit1_;
            id_bit0_ = !id_bit0_;
        }
    }
    inc_run_    = (f & A_SER_INC) != 0;
    bit_count_  = byte_stb ? 0 : (inc_run_ && bit_count_ == 3) ? 5 : static_cast<uint8_t>((bit_count_ + 1) & 7);
    step_count_ = (step_count_ == 0 || o_mr_s) ? kStepLoad : static_cast<uint8_t>(step_count_ - 1);

    // Чтение.
    rd_data_       = dec;
    mark_found_    = mark;
    det_a1_d_      = det_a1_;
    det_read_am_d_ = det_read_am_;
    det_trans_d_   = det_trans_;
    det_a1_        = (f & A_DET_A1) != 0;
    det_trans_     = (f & A_DET_TRANS) != 0;
    det_read_am_   = (f & A_DET_READ_AM) != 0;
    read_request_  = (f & A_READ) != 0;

    // Дисковод.
    const bool hld_next = (f & A_SET_HLD) != 0 || ((f & A_CLR_HLD) == 0 && hld_r_);
    const bool o_step   = step_q_;
    hlt_s_              = hlt_follows_hld ? (hld_next && !o_mr_s) : hlt;
    mr_s_               = mr;
    hld_r_              = hld_next && !mr;
    if (dr->select_from_hld) dr->selected = hld_pin();
    ready_r_     = ready_now;
    tr00_r_      = tr00_now;
    ip_r_        = ip_now;
    ip_sample_d_ = ip_sample_;
    ip_sample_   = dr->select_from_hld ? dr->index() : ip_now;
    step_q_      = (f & A_STEP) != 0;

    // Шина: захват - разрешение на слово.
    enable_   = static_cast<uint8_t>(~o_enable & capture_ & kRegAllMask);
    capture_ &= static_cast<uint8_t>(~enable_);

    pc     = next;
    act_   = &nx;
    type1_ = ((nx.f & A_SET_TYPE1) != 0 || type1_) && (nx.f & A_CLR_TYPE1) == 0;
    // Преамбула - окно до ячейки last - 1, если её смотрит следующее слово.
    if (uses_preamble_pc(next)) {
        // Выбор дисковода мог смениться в этом слове (HLD), дорожка - создаться записью.
        const uint32_t w = dr->selected ? (sel_word && dr->track == trk_raw ? raw & 0x1ffffu : window(last == 0 ? n - 1 : last - 1)) : 0;
        preamble_        = is_preamble(w);
    } else {
        preamble_ = false;
    }
    if (step_q_ && !o_step) dr->step(dirc ? +1 : -1);
    half2_  = false;
    ff_lim_ = 0;
    ff_forget();
    ff_rep_ = !power_on && next == pc_now; // слово ушло на себя: его однократные действия уже сделаны
    mark_word_end();
    ff_pre_ = pre_used;
    if (!power_on) steps++;
    outputs();
}

// branch().next == pc в пропуске: зависит только от строба, CNT2 == 0 и write_ready_.
SOUNDSINTH_NOINLINE bool Vg93w::loops_fill(unsigned key) {
    if ((act_->f & A_FORMAT) != 0) return branch(*act_).next == pc;
    br_known_ = static_cast<uint8_t>(br_known_ | key);
    if (branch(*act_).next == pc) {
        br_val_ = static_cast<uint8_t>(br_val_ | key);
        return true;
    }
    br_val_ = static_cast<uint8_t>(br_val_ & ~key);
    return false;
}

// Слово, ушедшее на себя, повторяется без изменений, пока не случится
// событие: строб байта (если слово его смотрит), тик шага при CNT2_STEP,
// смена уровня индекса, в чтении - метка, синхромаркер, смена преамбулы.
// До события меняются только счётчики, CRC, сдвиговый регистр и время -
// их можно продвинуть сразу на много слов.
bool Vg93w::rep_ok() const {
    const Act& a            = *act_;
    const uint64_t f        = a.f;
    constexpr uint64_t kOff = A_SEC_SHIFT | A_CMP | A_ID_CAPTURE | A_ID_COUNT | A_SET_BUSY | A_CNT1_DEC | A_CNT1_SHIFT | A_BYTE_SYNC | A_CLOCK_DROP;
    if (!ff_rep_ || mark_found_ || (f & kOff) != 0 || a.test == T_RD_ZERO || a.test == T_CRC_NONZERO) return false;
    if ((f & A_TRK_SHIFT) != 0 && (f & (A_READ | A_WRITE)) != 0) return false;
    if ((f & A_INT_WORD) != 0 && (op_ & 0xf8) == 0xd8) return false; // D8: каждое слово снова ставит INTRQ
    if (uses_preamble(a) && preamble_ != ff_pre_) return false;
    if ((f & A_SYNC_PREAMBLE) != 0 && preamble_) return false;
    return det_a1_ == det_a1_d_ && det_read_am_ == det_read_am_d_ && det_trans_ == det_trans_d_;
}

VG93W_SMALL void Vg93w::fast_forward(uint64_t end) {
    const Act& a        = *act_;
    const uint64_t f    = a.f;
    ff_lim_             = 0;
    SlotDrive* d        = fdd;
    const bool pre_word = uses_preamble(a);
    const bool det      = det_a1_ || det_read_am_ || det_trans_;
    const bool rm       = read_mode();
    const bool checked  = ff_ok_;
    bool writing        = (f & A_WRITE) != 0;
    // Проверки годности - один раз: ff_ok_ снимают слово, запись хоста, смена
    // входов. Слова чтения проверяет read_words.
    if (!ff_ok_) {
        if (!rm && !rep_ok()) return;
        // Запись - только установившаяся: WG и PRE_WG держатся, меток в конвейере нет.
        writing = writing && !rm && wg_q_ && pre_wg_ && !no_write_d_;
        // В записи хост пишет DATA на каждый байт: этот захват ведёт write_words.
        const uint8_t pend = static_cast<uint8_t>((capture_ | enable_) & (writing ? kRegNoDataMask : kRegAllMask));
        if (mr || mr_s_ || pend != 0 || clock_drop_ || mark_pipe_ != 0 || clock_pipe_ != 0x1f) return;
        if (!writing && ((f & A_WRITE) != 0 || wg_q_ || pre_wg_ || !no_write_d_ || data_pipe_ != 0)) return;
        if (ip_sample_ != ip_sample_d_ || ip_sample_ != d->index() || ready_r_ != ready_in() || tr00_r_ != d->tr00()) return;
        if (!hlt_follows_hld && hlt_s_ != hlt) return; // внешний HLT берёт следующее слово
    }
    ff_ok_ = false;
    if (rm) {
        if (!cells1_ || !cells2_ || !clock_cell_ || (t & 3) != 0) return;
    } else if (read_request_ || cells1_ || cells2_ || det || (f & A_READ) != 0 || (t & 7) != 7 || half2_) {
        return;
    }

    // Деления только 32-битные: 64-битные на M33 программные.
    const uint64_t words = (end - t) >> 3;
    const uint32_t nt    = words > 0x3fffffffu ? 0x3fffffffu : static_cast<uint32_t>(words);
    const uint32_t s     = d->slots;
    uint32_t ji          = 0xffffffffu;
    if (d->index_spins()) {
        // Первое слово, в конце которого уровень индекса другой, - последнее.
        const uint32_t p = d->pos;
        ji               = p < d->index_len ? (d->index_len - p + 1) / 2 : (s - p + 1) / 2;
    }
    if (rm) {
        read_words(nt, ji, checked);
        return;
    }

    // Вне чтения n - горизонт до события, без учёта конца вызова.
    uint32_t n        = 0x3fffffffu;
    const bool strobe = (f & (A_READ_BYTE | A_WRITE_BYTE | A_FORMAT | A_CNT2_BYTE | A_SER_INC | A_SYNC_MARK | A_SYNC_PREAMBLE)) != 0 ||
                        a.stay == S_UNTIL_BYTE || a.stay == S_BYTE_CNT2 || a.test == T_BYTE_STB || a.skip == K_PREAMBLE_BYTE || inc_carry_;
    if (strobe && !writing) {
        uint32_t k = 0;
        for (uint8_t b = bit_count_; b != 7 && k < 8; k++) {
            b = (inc_run_ && b == 3) ? 5 : static_cast<uint8_t>(b + 1);
        }
        n = k;
        // Строб без выводов (CNT2, инкрементор, сдвиг TRK): через него, пока адрес тот же.
        constexpr uint64_t kStbOut = A_READ_BYTE | A_WRITE_BYTE | A_FORMAT | A_SYNC_MARK | A_SYNC_PREAMBLE;
        if ((f & kStbOut) == 0 && a.test != T_BYTE_STB && a.skip != K_PREAMBLE_BYTE) {
            const uint8_t sbc = bit_count_, sc2 = cnt2_;
            int8_t st[2][2] = {{-1, -1}, {-1, -1}};
            auto stays      = [&](bool stb, bool c2z) {
                int8_t& c = st[stb][c2z];
                if (c < 0) {
                    bit_count_ = stb ? 7 : 0;
                    cnt2_      = c2z ? 0 : 1;
                    c          = branch(a).next == pc;
                }
                return c != 0;
            };
            const bool c2dec = (f & A_CNT2_BYTE) != 0 && (f & A_CNT2_LOAD) == 0;
            uint32_t w       = 0;
            uint8_t c2       = sc2;
            for (int it = 0; it < 24; it++) {
                if (!stays(false, c2 == 0)) break;
                w += (it == 0 ? k : (inc_run_ ? 6 : 7));
                if (!stays(true, c2 == 0)) break;
                w++;
                if (c2dec) c2 = static_cast<uint8_t>((c2 - 1) & 15);
            }
            bit_count_ = sbc;
            cnt2_      = sc2;
            n          = w;
        }
    }
    if ((f & A_CNT2_STEP) != 0 && cnt2_ != 0 && step_count_ < n) n = step_count_;
    if (ji < n) n = ji;
    if (n == 0 || !loops()) return;

    if (writing) {
        const uint32_t j = n < nt ? n : nt;
        write_words(j, strobe);
        ff_ok_ = ff_ok_ && ip_sample_ == ip_sample_d_;
        // Окно: слова до строба за концом вызова; ячейки - из конвейера записи.
        if (ff_ok_ && j < n && !inc_run_ && bit_count_ != 7 && (a.f & (A_CRC_FFFF | A_CRC_A1)) == 0) {
            uint32_t lzc = 7u - bit_count_;
            if (lzc > n - j) lzc = n - j;
            if (ji - 1 - j < lzc) lzc = ji - 1 - j;
            if (lzc != 0) {
                uint32_t cells = 0, dp = data_pipe_, ds = data_shift_, cr = crc_;
                for (uint32_t i = 0; i < lzc; i++) {
                    bool src = true;
                    switch (a.wsrc) {
                        case WR_CRC:
                            src = (cr & 0x8000) != 0;
                            break;
                        case WR_SHIFT:
                            src = (ds & 0x80) != 0;
                            break;
                        case WR_ZERO:
                            src = false;
                            break;
                        default:
                            break;
                    }
                    const bool d2 = (dp & 4) != 0, d3 = (dp & 8) != 0;
                    cells  |= (static_cast<uint32_t>(!d2 && !d3) | (static_cast<uint32_t>(d2) << 1)) << (2 * i);
                    cr      = (cr << 1) ^ ((src != ((cr & 0x8000) != 0)) ? 0x1021u : 0);
                    ds    <<= 1;
                    dp      = ((dp << 1) | static_cast<uint32_t>(src)) & 0x0f;
                }
                lz_kind_  = Window::Write;
                lz_cells_ = static_cast<uint16_t>(cells);
                lz_pos0_  = d->pos;
                lz_t0_    = we_t_;
                lz_wt_    = we_t_;
                lz_t_     = we_t_ + 8ull * lzc;
                ff_lim_   = lz_t_ + 8;
            }
        }
        return;
    }
    // Слова до конца вызова - сразу; дальше, до слова перед сменой уровня
    // индекса, - лениво: INTRQ, статус и выводы в этом окне не меняются.
    const uint32_t lz = n < ji ? n : ji - 1;
    const uint32_t k  = n < nt ? n : nt;
    if (k != 0) skip_words(k, pre_word);
    ff_ok_ = ip_sample_ == ip_sample_d_ && (!pre_word || preamble_ == ff_pre_);
    if (!pre_word && lz > k) {
        const uint32_t l = lz - k < 16384 ? lz - k : 16384; // головка не обходит дорожку больше раза
        lz_kind_         = Window::Loop;
        lz_t_            = we_t_ + 8ull * l;
        ff_lim_          = lz_t_ + 8;
    }
}

// Чтение: до nt слов, не дальше слова смены уровня индекса ji. Повторы
// слова - кусками, другие слова чтения между ними - по одному (light_word).
// rep - нынешнее слово уже проверено на повтор.
//
// Кусок - слова до следующего строба (строб - только первым словом
// куска), если в их окнах нет меток, синхромаркеров и смены преамбулы:
// y - 32 ячейки p - 17 .. p + 14 в порядке дорожки (разряд 0 - старейшая),
// бит слова j - разряд 15 + 2j, окна меток слова j кончаются на разрядах
// 2j + 15, 2j + 16 (сдвиг 2j, 2j + 1 у начала), преамбула слова j - от
// разряда 2j + 1. Иначе слово за словом: sr - последние 32 ячейки, разряд
// 0 - ячейка конца слова (last), окно до last - j - (sr >> j) & 0x1FFFF.
VG93W_SMALL void Vg93w::read_words(uint32_t nt, uint32_t ji, bool rep) {
    SlotDrive* d         = fdd;
    const uint32_t s     = d->slots;
    const uint8_t* track = d->selected ? d->track : nullptr;
    auto cell_bit        = [track](uint32_t p) { return track != nullptr ? (track[p >> 3] >> (p & 7)) & 1u : 0u; };
    const uint32_t n     = nt < ji ? nt : ji;
    const uint64_t s0    = steps;
    uint32_t k = 0, lzc = 0, lzb = 0; // окно за концом вызова: слова и их биты
    bool clean = false, nocell = false;
    while (k < n) {
        const Act& a     = *act_;
        const uint64_t f = a.f;
        if (!rep) rep = rep_ok() && (f & (A_WRITE | A_WRITE_BYTE | A_FORMAT)) == 0;
        if (rep && loops()) {
            const bool pre_word = uses_preamble(a);
            const bool sync_det = det_a1_ || det_read_am_;
            const bool strobe   = (f & (A_READ_BYTE | A_CNT2_BYTE | A_SER_INC | A_SYNC_MARK | A_SYNC_PREAMBLE)) != 0 || a.stay == S_UNTIL_BYTE ||
                                a.stay == S_BYTE_CNT2 || a.test == T_BYTE_STB || a.skip == K_PREAMBLE_BYTE || inc_carry_;
            const bool preset  = (f & (A_CRC_FFFF | A_CRC_A1)) != 0;
            const bool rdf     = (f & A_READ) != 0;
            const bool up      = (f & A_INC_UP) != 0 || dirc;
            const bool byte_ok = track != nullptr && !det_trans_ && !inc_run_ && (pre_word || !ff_pre_);
            // kall - горизонт без конца вызова, kend - с ним.
            uint32_t kall = ji;
            if ((f & A_CNT2_STEP) != 0 && cnt2_ != 0 && step_count_ < kall - k) kall = k + step_count_;
            const uint32_t kend = kall < nt ? kall : nt;
            uint32_t sr         = 0;
            bool sr_ok = false, ev = false;
            while (k < kend) {
                // Нет ячеек: меток, синхромаркеров и преамбулы нет, бит слова 0 -
                // до kend сразу, если на стробах слово остаётся и ничего не меняет.
                if (track == nullptr && !rd_data_ && !inc_carry_ && !ff_pre_ && (f & (A_READ_BYTE | A_CNT2_BYTE | A_SER_INC)) == 0) {
                    // Нынешнее состояние строба проверено loops() выше, второе - здесь.
                    const uint8_t bc = bit_count_;
                    bit_count_       = bc == 7 ? 0 : 7;
                    const bool st    = !strobe || loops();
                    bit_count_       = bc;
                    if (st) {
                        skip_words(kend - k, false);
                        k           = kend;
                        mark_found_ = false;
                        nocell      = true;
                        break;
                    }
                }
                if (byte_ok) {
                    const bool stb0 = bit_count_ == 7;
                    uint32_t ma     = stb0 ? 8 : 7u - bit_count_;
                    if (ma > kall - k) ma = kall - k;
                    const uint32_t m = ma > kend - k ? kend - k : ma;
                    const uint32_t p = d->pos, q = p - 17;
                    if (p >= 17 && p + 15 < s && (q >> 3) + 5 <= kTrackBytes) {
                        const uint8_t* b   = track + (q >> 3);
                        const uint64_t raw = static_cast<uint64_t>(b[0]) | static_cast<uint64_t>(b[1]) << 8 | static_cast<uint64_t>(b[2]) << 16 |
                                             static_cast<uint64_t>(b[3]) << 24 | static_cast<uint64_t>(b[4]) << 32;
                        const uint32_t y = static_cast<uint32_t>(raw >> (q & 7));
                        auto event       = [&](uint32_t mx) VG93W_INLINE_LAMBDA {
                            const uint32_t mm = (2u << (2 * mx)) - 1; // начала окон меток слов 0 .. mx - 1
                            bool e            = false;
                            if (det_a1_) e = (match16<0x4489>(y, !marker_bit13_bug) & mm) != 0;
                            if (!e && det_read_am_) e = (match16<0x5224>(y, !marker_bit13_bug) & mm) != 0;
                            if (!e && pre_word) {
                                uint32_t r         = y ^ (y >> 1);
                                r                 &= r >> 1;
                                r                 &= r >> 2;
                                r                 &= r >> 4;
                                r                 &= r >> 8;
                                const uint32_t pm  = 0xaaaau & ((1u << (2 * mx)) - 1);
                                e                  = (r & pm) != (ff_pre_ ? pm : 0u);
                            }
                            return e;
                        };
                        bool e = event(ma);
                        if (e && ma > m) {
                            ma = m;
                            e  = event(m);
                        }
                        if (!e && stb0 && strobe) e = !loops();
                        if (!e) {
                            if (stb0) { // слово 0 - строб
                                if (strobe) {
                                    if ((f & A_READ_BYTE) != 0) {
                                        lost_ = lost_ || (!type1_ && read_pend_);
                                        if (!type1_) drq_l_ = true;
                                        data_ = data_shift_;
                                    }
                                    if ((f & A_CNT2_BYTE) != 0 && (f & A_CNT2_LOAD) == 0) cnt2_ = static_cast<uint8_t>((cnt2_ - 1) & 15);
                                }
                                inc_carry_ = inc_carry_ && !up && (f & A_SER_INC) != 0;
                            }
                            read_pend_        = drq_l_;
                            const uint32_t x  = y >> 15;
                            const uint32_t b8 = rdf ? ((kEvenRev[x & 0xff] << 4) | kEvenRev[(x >> 8) & 0xff]) : 0;
                            const uint32_t ba = b8 >> (8 - ma);
                            const uint32_t bt = ba >> (ma - m); // биты слов 0 .. m - 1, старший - слово 0
                            if (ma > m) {
                                // Слова k + m + 1 .. k + ma - окно, до слова смены уровня индекса.
                                const uint32_t hi = ji - 1 - k < ma ? ji - 1 - k : ma;
                                lzc               = hi > m ? hi - m : 0;
                                lzb               = (ba >> (ma - m - lzc)) & ((1u << lzc) - 1);
                            }
                            const uint32_t in = (rd_data_ ? 1u << (m - 1) : 0u) | (bt >> 1);
                            if (preset) {
                                crc_ = (f & A_CRC_A1) != 0 ? 0x968b : 0xffff;
                            } else { // m бит разом: таблица - v * x^16 mod P для любых m <= 8
                                crc_ = static_cast<uint16_t>((crc_ << m) ^ kCrcZeros.v[(crc_ >> (16 - m)) ^ in]);
                            }
                            data_shift_  = static_cast<uint8_t>((data_shift_ << m) | bt);
                            rd_data_     = (bt & 1) != 0;
                            d->pos       = p + 2 * m >= s ? p + 2 * m - s : p + 2 * m;
                            t           += 8 * m;
                            steps       += m;
                            k           += m;
                            bit_count_   = static_cast<uint8_t>(stb0 ? m - 1 : bit_count_ + m);
                            step_count_  = step_count_ >= m ? static_cast<uint8_t>(step_count_ - m) : static_cast<uint8_t>(step_count_ + 250 - m);
                            mark_found_  = false;
                            preamble_    = pre_word && ff_pre_;
                            sr_ok        = false;
                            continue;
                        }
                    }
                }
                // Выход по стробу - до окон: и синхромаркер, и строб останавливают до слова.
                const bool stb = bit_count_ == 7 || ((f & A_SYNC_PREAMBLE) != 0 && preamble_);
                if (stb && strobe && !loops()) {
                    ev = true;
                    break;
                }
                if (!sr_ok) {
                    const uint32_t last = d->pos == 0 ? s - 1 : d->pos - 1;
                    sr                  = (cells_to(track, s, last >= 16 ? last - 16 : last + s - 16, 16) << 16) | cells_to(track, s, last, 16);
                    sr_ok               = true;
                }
                // Строб байта: в словах, которые его смотрят, - снова выбор адреса;
                // READ_BYTE грузит DATA, ставит DRQ и LOST, хост их видит только после вызова.
                const uint32_t p  = d->pos; // новые ячейки слова - p и p + 1
                const uint32_t p1 = p + 1 >= s ? 0 : p + 1;
                const uint32_t w  = (sr << 2) | (cell_bit(p) << 1) | cell_bit(p1);
                if (sync_det && (sync_w(static_cast<uint16_t>(w >> 2)) || sync_w(static_cast<uint16_t>(w >> 1)))) {
                    ev = true;
                    break;
                }
                if (stb && strobe) {
                    if ((f & A_READ_BYTE) != 0) {
                        lost_ = lost_ || (!type1_ && read_pend_);
                        if (!type1_) drq_l_ = true;
                        data_ = data_shift_;
                    }
                    if ((f & A_CNT2_BYTE) != 0 && (f & A_CNT2_LOAD) == 0) cnt2_ = static_cast<uint8_t>((cnt2_ - 1) & 15);
                }
                read_pend_ = drq_l_;
                if (stb) inc_carry_ = inc_carry_ && !up && (f & A_SER_INC) != 0;
                sr               = w;
                const bool dec   = rdf && ((w >> 3) & 1u) != 0;
                crc_             = preset ? ((f & A_CRC_A1) != 0 ? 0x968b : 0xffff)
                                          : static_cast<uint16_t>((crc_ << 1) ^ ((rd_data_ != ((crc_ & 0x8000) != 0)) ? 0x1021u : 0));
                data_shift_      = static_cast<uint8_t>((data_shift_ << 1) | static_cast<unsigned>(dec));
                rd_data_         = dec;
                const bool mark  = mark_w(w >> 3, det_a1_, det_read_am_, det_trans_) || mark_w(w >> 2, det_a1_, det_read_am_, det_trans_);
                const bool pre   = pre_word && is_preamble((w >> 1) & 0x1ffffu);
                d->pos           = p1 + 1 >= s ? 0 : p1 + 1;
                t               += 8;
                steps++;
                k++;
                bit_count_  = stb ? 0 : (inc_run_ && bit_count_ == 3) ? 5 : static_cast<uint8_t>((bit_count_ + 1) & 7);
                step_count_ = step_count_ == 0 ? kStepLoad : static_cast<uint8_t>(step_count_ - 1);
                mark_found_ = mark;
                preamble_   = pre;
                if (mark || pre != ff_pre_) {
                    ev = true;
                    break;
                }
            }
            clean = !ev;
            if (k >= n) break;
        }
        if (!light_word(track)) break;
        k++;
        rep   = false;
        clean = false;
    }
    // Уровень индекса меняется только в слове ji: до него оба отсчёта прежние.
    if (steps - s0 >= ji) index_after(s0);
    ff_ok_ = clean && ip_sample_ == ip_sample_d_;
    mark_word_end();
    if (lzc != 0) {
        lz_kind_ = Window::Read;
        lz_bits_ = static_cast<uint8_t>(lzb);
        lz_t_    = we_t_ + 8ull * lzc;
        ff_lim_  = lz_t_ + 8;
    } else if (nocell && clean && k == nt && ji - 1 > nt && ((act_->f & A_CNT2_STEP) == 0 || cnt2_ == 0)) {
        // Без ячеек слово стоит до слова перед сменой уровня индекса: дальше - лениво.
        const uint32_t l = ji - 1 - nt < 16384 ? ji - 1 - nt : 16384;
        lz_kind_         = Window::Loop;
        lz_t_            = we_t_ + 8ull * l;
        ff_lim_          = lz_t_ + 8;
    }
}

// Слово чтения - как word() при условиях пропуска: шина, запись и
// конвейеры меток пусты, /MR нет, входы и уровень индекса прежние. false -
// слово не такое или в его ячейках синхромаркер: ничего не сделано.
VG93W_SMALL bool Vg93w::light_word(const uint8_t* trk) {
    const Act& a              = *act_;
    const uint64_t f          = a.f;
    constexpr uint64_t kHeavy = A_WRITE | A_WRITE_BYTE | A_FORMAT | A_SET_BUSY | A_CLR_BUSY | A_SET_LOST | A_INT_WORD | A_SET_HLD | A_CLR_HLD | A_STEP |
                                A_SET_DIRC | A_CLR_DIRC | A_DIRC_CMP | A_CLOCK_DROP | A_CNT2_STEP | A_SET_TYPE1 | A_CLR_TYPE1;
    SlotDrive* d = fdd;
    if ((f & A_READ) == 0 || (f & kHeavy) != 0 || (d->select_from_hld && d->selected != hld_r_)) return false;
    const uint32_t s  = d->slots;
    const uint32_t p1 = d->pos + 1 >= s ? 0 : d->pos + 1;
    // Разряд j - ячейка p1 - j: бит слова и окно меток - до p1 - 3, второе окно - до p1 - 2.
    const uint32_t w = cells_to(trk, s, p1, 20);
    if ((det_a1_ || det_read_am_) && (sync_w(static_cast<uint16_t>(w >> 2)) || sync_w(static_cast<uint16_t>(w >> 1)))) return false;
    const Branch br     = branch(a);
    const bool rd       = rd_data_;
    const bool read_stb = (f & A_READ_BYTE) != 0 && br.byte_stb;
    const bool mux      = ((track_ & 0x80) != 0 && (f & A_TRK_SHIFT) != 0) || ((sector_ & 0x80) != 0 && (f & A_SEC_SHIFT) != 0);
    const bool inc_out  = (inc_carry_ && br.byte_stb) != mux;
    const bool cmp_en   = (f & A_CMP) != 0;
    const bool cmp_bit  = cmp_en && (a.cmp_op == CMP_ONE || (a.cmp_op == CMP_SHIFT && (data_shift_ & 0x80) != 0) || (a.cmp_op == CMP_RD && rd));
    const bool cmp_upd  = cmp_en && !(cmp_en_d_ && cmp_differ_);
    const bool dec      = ((w >> 3) & 1u) != 0;

    if (read_stb && !type1_) {
        lost_  = lost_ || read_pend_;
        drq_l_ = true;
    }
    read_pend_ = drq_l_;
    if ((f & (A_SET_CRC_ERR | A_CLR_CRC_ERR | A_SET_RNF | A_SET_WRT_FAULT | A_SET_WRT_PROT)) != 0) {
        uint8_t fl = flags_ & kFlBusyBit;
        if (((flags_ & kFlCrcBit) != 0 && (f & A_CLR_CRC_ERR) == 0) || (f & A_SET_CRC_ERR) != 0) fl |= kFlCrcBit;
        if ((flags_ & kFlRnfBit) != 0 || (f & A_SET_RNF) != 0) fl |= kFlRnfBit;
        if ((flags_ & kFlWrtFaultBit) != 0 || (f & A_SET_WRT_FAULT) != 0) fl |= kFlWrtFaultBit;
        if ((flags_ & kFlWrtProtBit) != 0 || (f & A_SET_WRT_PROT) != 0) fl |= kFlWrtProtBit;
        flags_ = fl;
    }
    if (read_stb) data_ = data_shift_;
    const bool fill = (f & A_TRK_FILL_ONE) != 0 || ((f & A_TRK_FILL_INC) != 0 && inc_out);
    if ((f & A_TRK_SHIFT) != 0) track_ = static_cast<uint8_t>((track_ << 1) | static_cast<unsigned>(fill));
    if ((f & A_SEC_SHIFT) != 0) sector_ = static_cast<uint8_t>((sector_ << 1) | static_cast<unsigned>((f & A_SEC_FILL_RD) != 0 ? rd : inc_out));
    if ((f & (A_CRC_FFFF | A_CRC_A1)) != 0) {
        crc_ = (f & A_CRC_A1) != 0 ? 0x968b : 0xffff;
    } else {
        crc_ = static_cast<uint16_t>((crc_ << 1) ^ ((rd != ((crc_ & 0x8000) != 0)) ? 0x1021u : 0));
    }
    data_shift_ = static_cast<uint8_t>((data_shift_ << 1) | static_cast<unsigned>(dec));
    if ((f & (A_SER_INC | A_CMP)) != 0 || inc_carry_ || cmp_en_d_) {
        const bool carry_p   = ((f & A_INC_UP) != 0 || dirc) == mux;
        inc_carry_           = (inc_carry_ && !br.byte_stb) || br.ser_inc_sync || (br.byte_stb && carry_p && inc_carry_ && (f & A_SER_INC) != 0);
        const bool o_greater = cmp_greater_;
        cmp_differ_          = ((cmp_bit != mux) && cmp_en) || (!cmp_upd && cmp_differ_);
        cmp_greater_         = (o_greater && !cmp_upd) || (cmp_upd && cmp_bit);
        cmp_en_d_            = cmp_en;
    }
    if ((f & A_CNT1_LOAD) != 0) {
        cnt1_ = a.preset;
    } else if ((f & A_CNT1_SHIFT) != 0) {
        cnt1_ = static_cast<uint8_t>(cnt1_ << 1);
    } else if ((f & A_CNT1_DEC) != 0) {
        cnt1_ = static_cast<uint8_t>(cnt1_ - 1);
    }
    if ((f & A_CNT2_LOAD) != 0) {
        cnt2_ = a.preset;
    } else if ((f & A_CNT2_BYTE) != 0 && br.byte_stb) {
        cnt2_ = static_cast<uint8_t>((cnt2_ - 1) & 15);
    }
    if ((f & A_ID_CAPTURE) != 0) {
        id_bit1_ = id_bit0_;
        id_bit0_ = rd;
    } else if ((f & A_ID_COUNT) != 0) {
        if (!id_bit0_) id_bit1_ = !id_bit1_;
        id_bit0_ = !id_bit0_;
    }
    inc_run_        = (f & A_SER_INC) != 0;
    bit_count_      = br.byte_stb ? 0 : (inc_run_ && bit_count_ == 3) ? 5 : static_cast<uint8_t>((bit_count_ + 1) & 7);
    step_count_     = step_count_ == 0 ? kStepLoad : static_cast<uint8_t>(step_count_ - 1);
    rd_data_        = dec;
    mark_found_     = mark_w(w >> 3, det_a1_d_, det_read_am_d_, det_trans_d_) || mark_w(w >> 2, det_a1_, det_read_am_, det_trans_);
    det_a1_d_       = det_a1_;
    det_read_am_d_  = det_read_am_;
    det_trans_d_    = det_trans_;
    det_a1_         = (f & A_DET_A1) != 0;
    det_trans_      = (f & A_DET_TRANS) != 0;
    det_read_am_    = (f & A_DET_READ_AM) != 0;
    hlt_s_          = hlt_follows_hld ? hld_r_ : hlt;
    step_q_         = false;
    ff_rep_         = br.next == pc;
    ff_pre_         = preamble_;
    pc              = br.next;
    act_            = &kActs[pc];
    type1_          = ((act_->f & A_SET_TYPE1) != 0 || type1_) && (act_->f & A_CLR_TYPE1) == 0;
    preamble_       = uses_preamble_pc(pc) && is_preamble((w >> 1) & 0x1ffffu);
    d->pos          = p1 + 1 >= s ? 0 : p1 + 1;
    t              += 8;
    steps++;
    half2_ = false;
    ff_forget();
    return true;
}

// До n слов записи: слово кладёт синхроимпульс в last (!d2 && !d3) и бит
// данных в pos (d2) - биты двух и трёх слов назад; строб байта - внутри
// (DATA в сдвиговый регистр, DRQ, LOST, CNT2). Выход перед стробом, после
// которого адрес другой, перед служебным байтом F5 / F6 / F7 форматирования
// и после смены уровня индекса.
VG93W_SMALL void Vg93w::write_words(uint32_t n, bool strobe) {
    const Act& a      = *act_;
    const uint64_t f  = a.f;
    SlotDrive* d      = fdd;
    const uint32_t s  = d->slots;
    const bool up     = (f & A_INC_UP) != 0 || dirc;
    const bool preset = (f & (A_CRC_FFFF | A_CRC_A1)) != 0;
    const uint64_t s0 = steps;
    // Ячейки копятся в cells (разряд i - ячейка first + i) и ложатся на
    // дорожку побайтно.
    uint8_t* track = nullptr;
    if (!d->wprot && d->selected) {
        if (d->track == nullptr && d->track_for != nullptr) d->track = d->track_for(d->user, *d, d->cyl, d->side, true);
        track = d->track;
        if (track != nullptr) d->written = static_cast<uint8_t>(d->written | (1u << d->side));
    }
    uint32_t first = d->pos + 1 >= s ? 0 : d->pos + 1, cells = 0, cn = 0;
    auto flush = [&]() {
        while (cn != 0) {
            const uint32_t sh = first & 7;
            uint32_t k        = 8 - sh;
            if (k > cn) k = cn;
            if (k > s - first) k = s - first;
            const uint32_t m = ((1u << k) - 1) << sh;
            if (track != nullptr) track[first >> 3] = static_cast<uint8_t>((track[first >> 3] & ~m) | ((cells << sh) & m));
            cells >>= k;
            cn     -= k;
            first   = first + k >= s ? 0 : first + k;
        }
    };
    bool brk = false;
    for (uint32_t k = 0; k < n; k++) {
        const bool stb  = bit_count_ == 7;
        const bool wstb = stb && (f & A_WRITE_BYTE) != 0;
        if (stb && strobe && k != 0) {
            if (!loops()) {
                brk = true;
                break;
            }
        }
        const uint8_t o_en     = enable_;
        const uint8_t new_data = (o_en & kRegDataBit) != 0 ? dal_in_ : data_;
        if (wstb && (f & A_FORMAT) != 0 && write_ready_ && (new_data == 0xf5 || data_ == 0xf6 || data_ == 0xf7)) {
            brk = true;
            break;
        }
        bool src = true;
        switch (a.wsrc) {
            case WR_CRC:
                src = (crc_ & 0x8000) != 0;
                break;
            case WR_SHIFT:
                src = (data_shift_ & 0x80) != 0;
                break;
            case WR_ZERO:
                src = false;
                break;
            default:
                break;
        }
        const uint32_t q  = d->pos + 2 >= s ? d->pos + 2 - s : d->pos + 2;
        const bool d2     = (data_pipe_ & 4) != 0;
        const bool d3     = (data_pipe_ & 8) != 0;
        cells            |= (static_cast<uint32_t>(!d2 && !d3) | (static_cast<uint32_t>(d2) << 1)) << cn;
        cn               += 2;
        if (cn >= 24) flush();
        if (wstb) {
            const bool o_wr = write_ready_;
            const bool wdd  = (cnt1_ == 0 && cnt2_ == 0) || !o_wr;
            lost_           = lost_ || (!o_wr && !type1_);
            if (!wdd && !type1_) drq_l_ = true;
            write_ready_ = ((wdd || type1_) && o_wr) || (o_en & kRegDataBit) != 0;
            data_shift_  = o_wr ? data_ : static_cast<uint8_t>(data_shift_ << 1);
        } else {
            data_shift_  = static_cast<uint8_t>(data_shift_ << 1);
            write_ready_ = write_ready_ || (o_en & kRegDataBit) != 0;
        }
        data_    = new_data;
        enable_  = static_cast<uint8_t>(~o_en & capture_ & kRegAllMask);
        capture_ = static_cast<uint8_t>(capture_ & ~enable_);
        if (stb) {
            if ((f & A_CNT2_BYTE) != 0 && (f & A_CNT2_LOAD) == 0) cnt2_ = static_cast<uint8_t>((cnt2_ - 1) & 15);
            inc_carry_ = inc_carry_ && !up && (f & A_SER_INC) != 0;
        }
        read_pend_  = drq_l_;
        crc_        = preset ? ((f & A_CRC_A1) != 0 ? 0x968b : 0xffff) : static_cast<uint16_t>((crc_ << 1) ^ ((src != ((crc_ & 0x8000) != 0)) ? 0x1021u : 0));
        data_pipe_  = static_cast<uint8_t>(((data_pipe_ << 1) | static_cast<unsigned>(src)) & 0x0f);
        rd_data_    = false;
        d->pos      = q;
        t          += 8;
        steps++;
        bit_count_  = stb ? 0 : (inc_run_ && bit_count_ == 3) ? 5 : static_cast<uint8_t>((bit_count_ + 1) & 7);
        step_count_ = step_count_ == 0 ? kStepLoad : static_cast<uint8_t>(step_count_ - 1);
    }
    flush();
    ff_ok_ = !brk;
    index_after(s0);
    mark_word_end();
}

// n слов петли без событий вне чтения или без ячеек; t - в конце слова.
void Vg93w::skip_words(uint32_t n, bool pre_word) {
    SlotDrive* d      = fdd;
    const uint32_t s  = d->slots;
    t                += 8ull * n;
    uint32_t p        = d->pos + 2 * n;
    if (p >= s) p = p - s < s ? p - s : p % s;
    d->pos = p;
    count_words(n);
    const uint32_t p2 = p >= 2 ? p - 2 : p + s - 2; // конец предпоследнего слова
    ip_sample_d_      = d->index_at(p2);
    ip_sample_        = d->index_at(p);
    ip_r_             = ip_sample_;
    preamble_         = pre_word && is_preamble(window(p2));
    mark_word_end();
}

// n слов петли вне чтения со стробами: CNT2 по байту, инкрементор, сдвиг TRK.
void Vg93w::count_strobes(uint32_t n) {
    const uint64_t f  = act_->f;
    const bool c2dec  = (f & A_CNT2_BYTE) != 0 && (f & A_CNT2_LOAD) == 0;
    const bool tsh    = (f & A_TRK_SHIFT) != 0;
    const bool up     = (f & A_INC_UP) != 0 || dirc;
    const bool serinc = (f & A_SER_INC) != 0;
    auto shift_k      = [&](uint32_t k) {
        if (!tsh || k == 0) return;
        if ((f & A_TRK_FILL_ONE) != 0) {
            track_ = k >= 8 ? 0xff : static_cast<uint8_t>((track_ << k) | ((1u << k) - 1));
        } else if ((f & A_TRK_FILL_INC) != 0) {
            const uint32_t r = k & 7;
            track_           = static_cast<uint8_t>((track_ << r) | (track_ >> (8 - r)));
        } else {
            track_ = k >= 8 ? 0 : static_cast<uint8_t>(track_ << k);
        }
    };
    while (n != 0) {
        uint32_t k = 0;
        for (uint8_t b = bit_count_; b != 7; k++) {
            b = (inc_run_ && b == 3) ? 5 : static_cast<uint8_t>(b + 1);
        }
        if (n <= k) {
            shift_k(n);
            for (; n != 0; n--) {
                bit_count_ = (inc_run_ && bit_count_ == 3) ? 5 : static_cast<uint8_t>(bit_count_ + 1);
            }
            return;
        }
        shift_k(k);
        const bool mux = tsh && (track_ & 0x80) != 0;
        if (tsh) {
            const bool inc_out = inc_carry_ != mux;
            const bool fill    = (f & A_TRK_FILL_ONE) != 0 || ((f & A_TRK_FILL_INC) != 0 && inc_out);
            track_             = static_cast<uint8_t>((track_ << 1) | static_cast<unsigned>(fill));
        }
        inc_carry_ = serinc && inc_carry_ && (up == mux);
        if (c2dec) cnt2_ = static_cast<uint8_t>((cnt2_ - 1) & 15);
        bit_count_  = 0;
        n          -= k + 1;
    }
}

// Счётчики, CRC и сдвиговый регистр после n слов петли вне чтения.
void Vg93w::count_words(uint32_t n) {
    const uint64_t f  = act_->f;
    steps            += n;
    if ((f & (A_CNT2_BYTE | A_SER_INC | A_TRK_SHIFT)) != 0 || inc_carry_) {
        count_strobes(n);
    } else if (inc_run_) {
        static constexpr uint8_t kSeq[7] = {0, 1, 2, 3, 5, 6, 7}, kIdx[8] = {0, 1, 2, 3, 3, 4, 5, 6};
        uint32_t i = kIdx[bit_count_] + n;
        i          = i < 7 ? i : i < 14 ? i - 7 : i % 7;
        bit_count_ = kSeq[i];
    } else {
        bit_count_ = static_cast<uint8_t>((bit_count_ + n) & 7);
    }
    if (n < 250) {
        const int sc = static_cast<int>(step_count_) - static_cast<int>(n);
        step_count_  = static_cast<uint8_t>(sc < 0 ? sc + 250 : sc);
    } else {
        step_count_ = static_cast<uint8_t>((step_count_ + 250 - n % 250) % 250);
    }
    if ((f & (A_CRC_FFFF | A_CRC_A1)) != 0) {
        crc_ = (f & A_CRC_A1) != 0 ? 0x968b : 0xffff;
    } else if (crc_ != 0) {
        crc_ = crc_zeros(crc_, n);
    }
    data_shift_ = n >= 8 ? 0 : static_cast<uint8_t>(data_shift_ << n);
    rd_data_    = false;
    read_pend_  = drq_l_;
}

// Ленивые слова от we_t_ до t: счётчики и середина нынешнего слова.
void Vg93w::settle() {
    if (!ff_lazy_) return;
    ff_lazy_          = false;
    const uint64_t dt = t - we_t_;
    const auto n      = static_cast<uint32_t>(dt >> 3);
    const auto r      = static_cast<uint32_t>(dt) & 7;
    if (n != 0) {
        lz_apply(n);
        we_rsm_ = true;
    }
    clock_cell_ = we_cc_ != (r >= 4);
    half2_      = !read_mode() && r >= 6;
    if (dt != 0) rclk_since_mr_ = true;
}

// Запись: ячейки слов окна, кончающихся до upto.
void Vg93w::lz_put(uint64_t upto) {
    if (upto > lz_t_) upto = lz_t_;
    if (upto <= lz_wt_) return;
    const auto i0 = static_cast<uint32_t>((lz_wt_ - lz_t0_) >> 3), i1 = static_cast<uint32_t>((upto - lz_t0_) >> 3);
    lz_wt_       = lz_t0_ + 8ull * i1;
    SlotDrive* d = fdd;
    uint8_t* tr  = d->track;
    if (tr == nullptr || d->wprot || !d->selected) return;
    const uint32_t s = d->slots;
    uint32_t q       = lz_pos0_ + 2 * i0 + 1;
    if (q >= s) q -= s;
    for (uint32_t c = 2 * i0; c < 2 * i1; c++) {
        const auto m = static_cast<uint8_t>(1u << (q & 7));
        if (((lz_cells_ >> c) & 1u) != 0) {
            tr[q >> 3] = static_cast<uint8_t>(tr[q >> 3] | m);
        } else {
            tr[q >> 3] = static_cast<uint8_t>(tr[q >> 3] & ~m);
        }
        q = q + 1 >= s ? 0 : q + 1;
    }
}

// k слов окна от we_t_: счётчики, в чтении - биты из lz_bits_.
void Vg93w::lz_apply(uint32_t k) {
    if (lz_kind_ == Window::Loop) {
        count_words(k);
        we_t_ += 8ull * k;
        return;
    }
    if (lz_kind_ == Window::Write) {
        // Запись: слова без строба, ячейки кладёт lz_put.
        const uint64_t f = act_->f;
        uint32_t sb      = 0;
        switch (act_->wsrc) {
            case WR_CRC:
                sb = crc_ >> (16 - k);
                break;
            case WR_SHIFT:
                sb = static_cast<uint32_t>(data_shift_) >> (8 - k);
                break;
            case WR_ZERO:
                break;
            default:
                sb = (1u << k) - 1;
                break;
        }
        if ((f & (A_CRC_FFFF | A_CRC_A1)) != 0) {
            crc_ = (f & A_CRC_A1) != 0 ? 0x968b : 0xffff;
        } else {
            crc_ = static_cast<uint16_t>((crc_ << k) ^ kCrcZeros.v[(crc_ >> (16 - k)) ^ sb]);
        }
        data_pipe_  = static_cast<uint8_t>(((data_pipe_ << k) | sb) & 0x0f);
        data_shift_ = static_cast<uint8_t>(data_shift_ << k);
        for (uint32_t i = 0; i < k && (capture_ | enable_) != 0; i++) {
            const uint8_t o_en = enable_;
            if ((o_en & kRegDataBit) != 0) {
                data_        = dal_in_;
                write_ready_ = true;
            }
            enable_  = static_cast<uint8_t>(~o_en & capture_ & kRegAllMask);
            capture_ = static_cast<uint8_t>(capture_ & ~enable_);
        }
        rd_data_     = false;
        read_pend_   = drq_l_;
        steps       += k;
        bit_count_   = static_cast<uint8_t>(bit_count_ + k);
        step_count_  = step_count_ >= k ? static_cast<uint8_t>(step_count_ - k) : static_cast<uint8_t>(step_count_ + 250 - k);
        we_t_       += 8ull * k;
        return;
    }
    const Act& a      = *act_;
    const uint64_t f  = a.f;
    const auto rem    = static_cast<uint32_t>((lz_t_ - we_t_) >> 3);
    const uint32_t bt = (static_cast<uint32_t>(lz_bits_) >> (rem - k)) & ((1u << k) - 1);
    const uint32_t in = (rd_data_ ? 1u << (k - 1) : 0u) | (bt >> 1);
    if ((f & (A_CRC_FFFF | A_CRC_A1)) != 0) {
        crc_ = (f & A_CRC_A1) != 0 ? 0x968b : 0xffff;
    } else {
        crc_ = static_cast<uint16_t>((crc_ << k) ^ kCrcZeros.v[(crc_ >> (16 - k)) ^ in]);
    }
    data_shift_  = static_cast<uint8_t>((data_shift_ << k) | bt);
    rd_data_     = (bt & 1) != 0;
    steps       += k;
    bit_count_   = static_cast<uint8_t>(bit_count_ + k);
    step_count_  = step_count_ >= k ? static_cast<uint8_t>(step_count_ - k) : static_cast<uint8_t>(step_count_ + 250 - k);
    mark_found_  = false;
    preamble_    = uses_preamble(a) && ff_pre_;
    read_pend_   = drq_l_;
    we_t_       += 8ull * k;
}

// Уровень индекса в конце двух последних слов пропуска; горизонт кончается
// не позже слова его смены, поэтому внутри пропуска он не проверяется.
void Vg93w::index_after(uint64_t s0) {
    if (steps == s0) return;
    const SlotDrive* d = fdd;
    const uint32_t s   = d->slots;
    ip_sample_d_       = d->index_at(d->pos >= 2 ? d->pos - 2 : d->pos + s - 2);
    ip_sample_         = d->index_at(d->pos);
    ip_r_              = ip_sample_;
}

void Vg93w::mark_word_end() {
    we_t_     = t;
    we_drive_ = fdd;
    we_cc_    = clock_cell_;
    we_c1_    = cells1_;
    we_c2_    = cells2_;
    we_rsm_   = rclk_since_mr_;
}

void Vg93w::cell(bool sync) {
    clock_cell_ = sync || !clock_cell_;
    cells2_     = cells1_;
    cells1_     = read_request_ && rclk_since_mr_;
}

// /MR держится: каждое слово уходит на адрес 0, видимое стоит. Круг из 8 слов
// проигрывается честно; если после него состояние то же, кроме времени и
// головки, - дальше круги перескакиваются. Первые слова сброса ещё меняют
// состояние: кругов-попыток несколько. Иначе минуты сброса машины (меню 128
// до входа в TR-DOS) стоили бы прогона по словам.
SOUNDSINTH_NOINLINE uint64_t Vg93w::reset_cycles(uint64_t half) {
    constexpr uint64_t kCycle = 64; // 8 слов по 8 полупериодов
    SlotDrive* d              = fdd;
    if (d->index_spins()) return half;
    for (int tries = 0; tries < 4 && half >= 3 * kCycle && mr && mr_s_; tries++) {
        // Снимки байтами: сравнивается сам объект, время и головка на
        // сравнение возвращаются к началу круга.
        unsigned char before[sizeof(Vg93w)], dbefore[sizeof(SlotDrive)];
        std::memcpy(before, static_cast<const void*>(this), sizeof(Vg93w));
        std::memcpy(dbefore, static_cast<const void*>(d), sizeof(SlotDrive));
        const uint64_t t0 = t, s0 = steps, w0 = we_t_;
        const uint32_t p0 = d->pos;
        run_words(kCycle);
        half              -= kCycle;
        const uint64_t t1 = t, s1 = steps, w1 = we_t_;
        const uint32_t p1 = d->pos;
        t                 = t0;
        steps             = s0;
        we_t_             = w0;
        d->pos            = p0;
        const bool cycle  = std::memcmp(static_cast<const void*>(this), before, sizeof(Vg93w)) == 0 &&
                           std::memcmp(static_cast<const void*>(d), dbefore, sizeof(SlotDrive)) == 0;
        t      = t1;
        steps  = s1;
        we_t_  = w1;
        d->pos = p1;
        if (!cycle) continue;
        const uint64_t n  = half / kCycle - 1; // последний круг - честно: конец вызова не на границе слова
        t                += n * kCycle;
        we_t_            += n * kCycle;
        steps            += n * 8;
        d->pos            = static_cast<uint32_t>((d->pos + (n * kCycle / 4) % d->slots) % d->slots);
        return half - n * kCycle;
    }
    return half;
}

void Vg93w::run_slow(uint64_t half) {
    if (mr && mr_s_) half = reset_cycles(half);
    run_words(half);
}

void Vg93w::run_words(uint64_t half) {
    SlotDrive* d = fdd;
    // События: граница слота (t % 4 == 0); вне режима чтения подъёмы CLK
    // (нечётные t): ячейка при t % 4 == 3, вторая половина слова при
    // t % 8 == 5, конец слова - на подъёме после неё.
    static constexpr uint8_t kNext[8] = {3, 2, 1, 1, 1, 2, 1, 1};
    const uint64_t end                = t + half;
    // Петля ожидания до горизонта: только время и головка.
    if (end < ff_lim_ && we_drive_ == d && !mr) {
        if (lz_kind_ == Window::Write) lz_put(end);
        uint32_t p = d->pos + (static_cast<uint32_t>(end >> 2) - static_cast<uint32_t>(t >> 2));
        if (p >= d->slots) p -= d->slots;
        d->pos   = p;
        t        = end;
        ff_lazy_ = true;
        return;
    }
    settle();
    // Середина слова-повтора: вернуться к концу прошлого слова и пропустить
    // отсюда; события середины слова детерминированы и, если пропуска не
    // вышло, проигрываются снова. ff_part_ - середина прошлого вызова не
    // проиграна вовсе.
    const bool part = ff_part_;
    const bool rew  = ff_rep_ && t >= we_t_ && we_drive_ == d && ((capture_ | enable_) & kRegNoDataMask) == 0 && !mr;
    ff_part_        = false;
    if (part || rew) {
        // Головка - от нынешнего положения: владелец мог её переставить.
        const uint32_t back = static_cast<uint32_t>(t >> 2) - static_cast<uint32_t>(we_t_ >> 2);
        t                   = we_t_;
        d->pos              = d->pos >= back ? d->pos - back : d->pos + d->slots - back;
        clock_cell_         = we_cc_;
        cells1_             = we_c1_;
        cells2_             = we_c2_;
        rclk_since_mr_      = we_rsm_;
        half2_              = false;
        if (rew) {
            // Окно, посчитанное прошлым вызовом: его слова, кончающиеся в этом вызове.
            if (ff_lim_ > we_t_ + 8 && we_drive_ == d && !mr) {
                const auto w     = static_cast<uint32_t>((lz_t_ - we_t_) >> 3);
                const auto e     = static_cast<uint32_t>((end - we_t_) >> 3);
                const uint32_t k = w < e ? w : e;
                if (k != 0) {
                    if (lz_kind_ == Window::Write) lz_put(we_t_ + 8ull * k);
                    lz_apply(k);
                    t          = we_t_;
                    uint32_t p = d->pos + 2 * k;
                    if (p >= d->slots) p -= d->slots;
                    d->pos = p;
                    mark_word_end();
                }
            }
            fast_forward(end);
        }
    }
    // Пропуск дошёл до конца вызова: середину слова не проигрывать.
    auto defer = [&]() {
        if (no_defer_ || !ff_rep_ || t != we_t_ || t + 8 <= end || we_drive_ != d) return false;
        uint32_t p = d->pos + (static_cast<uint32_t>(end >> 2) - static_cast<uint32_t>(t >> 2));
        if (p >= d->slots) p -= d->slots;
        d->pos   = p;
        t        = end;
        ff_part_ = true;
        return true;
    };
    if (defer()) {
        outputs();
        return;
    }
    while (t < end) {
        uint64_t step = 4 - (t & 3);
        if (!read_mode()) step = half2_ ? 1 : kNext[t & 7];
        if (t + step > end) {
            t = end;
            break;
        }
        t += step;
        if ((t & 3) == 0) {
            d->pos = d->pos + 1 >= d->slots ? 0 : d->pos + 1;
            if (wg_q_ && !d->wprot) d->put(d->pos, false);
            rclk_since_mr_ = !mr;
            if (read_mode()) {
                const uint32_t last = d->pos == 0 ? d->slots - 1 : d->pos - 1;
                const bool w        = cells2_ && !clock_cell_;
                // Синхромаркер: следующая ячейка - синхроимпульс, слово растягивается.
                cell((det_a1_ || det_read_am_) && sync_w(static_cast<uint16_t>(window((last + d->slots - 1) % d->slots))));
                if (w) {
                    word(false);
                    fast_forward(end);
                    if (defer()) break;
                }
            }
        } else if ((t & 1) != 0 && !read_mode()) {
            const unsigned u4 = static_cast<unsigned>((t + 1) >> 1) & 7;
            const bool w      = half2_;
            if ((u4 & 1) == 0) cell(false);
            if (w) {
                word(false);
                fast_forward(end);
                if (defer()) break;
            }
            if (!read_mode() && (u4 & 3) == 3 && !(cells2_ && rclk_since_mr_)) half2_ = true;
        }
    }
    outputs();
}

uint64_t Vg93w::quiet_until() const {
    // Слово не короче 7 полупериодов (выход из режима чтения); в ленивом окне
    // we_t_ не двигается, слова там по 8.
    const uint64_t we   = ff_lazy_ ? we_t_ + ((t - we_t_) & ~7ull) : we_t_;
    const uint64_t base = we + 7 > t + 1 ? we + 7 : t + 1;
    // Запись хоста, /MR или вход, ещё не защёлкнутые словом, - ближайший конец слова.
    const SlotDrive* d = fdd;
    const bool latched = (capture_ | enable_) == 0 && ready_r_ == ready_in() && ip_r_ == d->index() && tr00_r_ == d->tr00();
    // Устойчивый /MR: видимое стоит до записи хоста, а та будит поток сама.
    // Срок далёкий, но конечный: владелец прибавляет к нему своё смещение.
    if (mr && mr_s_ && latched && !d->index_spins()) return t + (1ull << 40);
    if (mr || mr_s_ || !latched) return base;
    const Act& a = *act_;
    // Петля, которая сама меняет регистр (сдвиг TRK, SEC), - каждое слово.
    if ((a.f & kVisOnly) != 0 && a.stay != S_NONE) return base;
    // Окно пропуска кончается перед стробом и перед сменой уровня индекса.
    if (ff_lim_ > t) return ff_lim_;
    uint64_t q = base + 7ull * ((op_ >> 4) == 0xd ? kVisDistEi[pc] : kVisDist[pc]);
    // Слово ожидания байта или индекса: DRQ - на слове строба (слово - 8),
    // уход с ожидания индекса - не раньше его фронта.
    const bool idx_wait = a.stay == S_UNTIL_INDEX && d->index_spins() && !(ip_sample_ && !ip_sample_d_);
    if ((a.stay == S_UNTIL_BYTE || a.stay == S_BYTE_CNT2 || idx_wait) && (a.f & (A_CLR_BUSY | A_SET_BUSY | A_INT_WORD | kVisOnly)) == 0 && !det_a1_ &&
        !det_read_am_ && !det_trans_) {
        unsigned k = 0;
        for (uint8_t b = bit_count_; b != 7; k++) {
            b = (inc_run_ && b == 3) ? 5 : static_cast<uint8_t>(b + 1);
        }
        q = base + 8ull * k;
        if (idx_wait) {
            const uint64_t qi = t + 4ull * (d->slots - d->pos) - 3;
            if (qi < q) q = qi;
        }
    }
    // Разряд индекса в статусе типа I - до его фронта.
    if (type1_ && d->index_spins()) {
        const uint32_t edge = d->pos < d->index_len ? d->index_len : d->slots;
        const uint64_t qi   = t + 4ull * (edge - d->pos) - 3;
        if (qi < q) q = qi;
    }
    return q;
}

uint8_t Vg93w::peek(unsigned reg) const {
    switch (reg & 3) {
        case 0: {
            const bool not_ready = mr_s_ || !ready_r_;
            uint8_t v = static_cast<uint8_t>((not_ready << 7) | ((flags_ & kFlRnfBit) != 0 ? kStRnfBit : 0) | ((flags_ & kFlCrcBit) != 0 ? kStCrcBit : 0) |
                                             (flags_ & kFlBusyBit));
            if (type1_) {
                v |= static_cast<uint8_t>((fdd->wprot_in() << 6) | ((hlt_s_ && hld_r_) << 5) | (tr00_r_ << 2) | (ip_r_ << 1));
            } else {
                v |= static_cast<uint8_t>(((flags_ & kFlWrtProtBit) != 0 ? kStWprotBit : 0) | ((flags_ & kFlWrtFaultBit) != 0 ? kStWrtFaultBit : 0) |
                                          (lost_ << 2) | (drq_l_ << 1));
            }
            return v;
        }
        case 1:
            return track_;
        case 2:
            return sector_;
        default:
            return data_;
    }
}

void Vg93w::write(unsigned reg, uint8_t v) {
    const unsigned r   = reg & 3;
    const unsigned bit = 1u << r;
    settle();
    dal_in_ = v;
    // DATA в окне записи: захват ведёт lz_apply, окно живо.
    if (r != 3 || lz_kind_ != Window::Write || ff_lim_ == 0) {
        ff_lim_ = 0;
        ff_forget();
    }
    capture_ = static_cast<uint8_t>((capture_ & ~bit) | ((enable_ & bit) != 0 ? 0u : bit));
    if (r == 0) no_intrq_ = true;
    if (r == 3) drq_l_ = false;
    outputs();
}

uint8_t Vg93w::read(unsigned reg) {
    const unsigned r = reg & 3;
    if (r == 1 || (r == 3 && drq_l_)) settle(); // TRK и read_pend_ ленивых слов
    const uint8_t v = peek(r);
    if (r == 0) no_intrq_ = true;
    if (r == 3) drq_l_ = false;
    outputs();
    return v;
}

// Фронт /MR: отложенная середина слова проигрывается с прежним /MR, возврата
// через фронт нет; подъём /MR сразу кончает режим чтения.
void Vg93w::pins_changed() {
    ff_lim_ = 0;
    ff_forget();
    if (!powered_ || mr == mr_prev_) {
        mr_prev_ = mr;
        return;
    }
    const bool now = mr;
    settle();
    if (ff_part_) {
        mr        = mr_prev_;
        no_defer_ = true;
        run_half(0);
        no_defer_ = false;
        mr        = now;
        ff_lim_   = 0;
    }
    mr_prev_ = now;
    ff_rep_  = false;
    if (!now || !rclk_since_mr_) return;
    const bool was = read_mode();
    rclk_since_mr_ = false;
    if (was && (((t + 1) >> 1) & 3) == 3 && !half2_) half2_ = true;
}

void Vg93w::init(SlotDrive* drive) {
    *this           = Vg93w{};
    fdd             = drive;
    hlt_follows_hld = true;
    hld_r_          = false;
    act_            = &kActZero;
    mr              = true;
    word(true);
    powered_ = true;
    run(16);
    mr = false;
}

} // namespace devices::bdi
