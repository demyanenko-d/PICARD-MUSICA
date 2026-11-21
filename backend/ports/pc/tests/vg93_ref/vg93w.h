// SPDX-License-Identifier: MIT
#pragma once

// КР1818ВГ93 с точностью до микрослова: ход микропрограммы (адрес слова,
// проверки команды, векторы EI, ожидания, счётчики, захват записи хоста,
// флаги, DRQ / INTRQ / LOST), время конца каждого слова и запись на дорожку -
// как у кристалла; что внутри слова, сведено к его концу. Дорожка - поток
// ячеек MFM (SlotDrive): бит слова, метки и преамбула - окна ячеек у конца слова.
//
// Время - полупериоды CLK (1 МГц), ячейка - слот 4 полупериода. Вне режима
// чтения слово кончается при t % 8 == 7, в режиме чтения - на границе слота
// после ячейки данных.

#include <cstdint>

#include "slot_drive.h"
#include "devices/bdi/vg93.h"

namespace devices::bdi {

namespace ucode {
struct Act;
}

class Vg93w {
public:
    // Включение и импульс /MR на 16 тактов CLK: RESTORE (03h). HLT от HLD.
    void init(SlotDrive* drive);
    // Копию ядра - на другой дисковод (снимок копии ленты - в ядро).
    // INTRQ, снятый копией ленты, а не чтением статуса Z80, - обратно.
    void intrq_unread() {
        no_intrq_ = false;
        intrq     = true;
    }
    void rebind(const SlotDrive* from, SlotDrive* to) {
        if (we_drive_ == from) we_drive_ = to;
        fdd = to;
    }

    // Шина: импульс /WE или /RE в текущий момент.
    void write(unsigned reg, uint8_t v);
    uint8_t read(unsigned reg);
    uint8_t peek(unsigned reg) const;
    uint8_t status() const { return peek(0); }

    // Вход сменился (mr, hlt, ready / wprot / индекс / TR00, диск, сторона,
    // угол головки, выбор дисковода): обязателен, иначе петля ожидания
    // может пройти мимо смены.
    void pins_changed();
    // Довести ленивые слова до t: регистры для осмотра снаружи.
    void settle();
    // Момент, раньше которого без обращений хоста и смены входов не сменится
    // ничего видимого снаружи: INTRQ, DRQ, статус, TRK, SEC, DATA.
    uint64_t quiet_until() const;
    // До этого момента досчёт дёшев: петля ожидания двигает только время и
    // головку, устойчивый /MR без диска перескакивает кругами.
    uint64_t cheap_until() const { return mr ? quiet_until() : (we_drive_ == fdd ? ff_lim_ : t); }

    void run(uint32_t clk) { run_half(2ull * clk); }
    // Петля ожидания до горизонта: только время и головка - без вызова.
    void run_half(uint64_t half) {
        const uint64_t end = t + half;
        if (end < ff_lim_ && lz_kind_ != Window::Write && we_drive_ == fdd && !mr) {
            uint32_t p = fdd->pos + (static_cast<uint32_t>(end >> 2) - static_cast<uint32_t>(t >> 2));
            if (p >= fdd->slots) p -= fdd->slots;
            fdd->pos = p;
            t        = end;
            ff_lazy_ = true;
            return;
        }
        run_slow(half);
    }
    void run_slow(uint64_t half);
    uint64_t clk() const { return (t + 1) >> 1; }

    // Входы.
    bool mr              = false; // /MR = 0
    bool hlt             = false;
    bool hlt_follows_hld = false;
    bool ready_from_hld  = false;

    // Выходы.
    bool intrq = false, drq = false, hld = false, dirc = false, wg = false, step_pin = false;

    SlotDrive* fdd = nullptr;
    uint64_t t     = 0; // полупериоды CLK
    uint8_t pc     = 0; // адрес микрослова
    uint64_t steps = 0; // концов микрослов

    bool marker_bit13_bug = true; // детекторы A1/C2 не видят канальный бит 13 - дефект кристалла

    friend class Vg93Tape; // лента читает op_ (отсев команд записи)

private:
    // Окно ленивых слов: петля вне чтения, чтение (биты в lz_bits_), запись
    // (ячейки в lz_cells_).
    enum class Window : uint8_t { Loop, Read, Write };

    struct Branch {
        bool byte_stb, ser_inc_sync, ip_edge, fsr;
        uint8_t next;
    };
    Branch branch(const ucode::Act& a) const;
    void word(bool power_on);
    void outputs();
    bool hld_pin() const { return hld_r_ && !mr_s_; }
    bool ready_in() const { return ready_from_hld ? hld_pin() : fdd->ready; }
    bool read_mode() const { return read_request_ && rclk_since_mr_; }
    void cell(bool sync);
    bool sync_w(uint16_t lo) const;                           // A1 / C2 в окне ячеек (новейшая - разряд 0)
    bool mark_w(uint32_t w, bool a1, bool am, bool tr) const; // метка для микрокода в окне 17 ячеек
    void fast_forward(uint64_t end);
    bool rep_ok() const;
    void read_words(uint32_t nt, uint32_t ji, bool rep);
    bool light_word(const uint8_t* trk);
    void skip_words(uint32_t n, bool pre_word);
    void write_words(uint32_t n, bool strobe);
    void count_words(uint32_t n);
    uint64_t reset_cycles(uint64_t half);
    void run_words(uint64_t half);
    void lz_apply(uint32_t k);
    void lz_put(uint64_t upto);
    bool loops() {
        const unsigned key = 1u << ((bit_count_ == 7 ? 1 : 0) | (cnt2_ == 0 ? 2 : 0) | (write_ready_ ? 4 : 0));
        if ((br_known_ & key) != 0) return (br_val_ & key) != 0;
        return loops_fill(key);
    }
    bool loops_fill(unsigned key);
    void ff_forget() {
        ff_ok_    = false;
        br_known_ = 0;
    }
    void count_strobes(uint32_t n);
    void mark_word_end();
    void index_after(uint64_t s0);
    uint32_t window(uint32_t last_cell) const; // 17 ячеек, кончающихся в last_cell

    const ucode::Act* act_ = nullptr;
    // Секвенсор, счётчики.
    uint8_t cnt1_ = 0, cnt2_ = 0, bit_count_ = 4, step_count_ = 0;
    bool id_bit0_ = false, id_bit1_ = false, inc_run_ = false;
    // Статус.
    uint8_t flags_ = 0; // BUSY, CRC, RNF, WRITE FAULT, WRITE PROT
    bool type1_ = false, lost_ = false;
    bool no_intrq_ = false, drq_l_ = false, read_pend_ = false, write_ready_ = false;
    // Регистры, АЛУ.
    uint8_t op_ = 0, track_ = 0, sector_ = 0, data_ = 0, data_shift_ = 0;
    bool inc_carry_ = false, cmp_differ_ = false, cmp_greater_ = false, cmp_en_d_ = false;
    // Чтение: метка в прошлом слове, преамбула.
    bool mark_found_ = false, preamble_ = false, rd_data_ = false;
    bool det_a1_ = false, det_trans_ = false, det_read_am_ = false, read_request_ = false;
    bool det_a1_d_ = false, det_trans_d_ = false, det_read_am_d_ = false; // детекторы слова раньше
    // Сетка слов: в режиме чтения слово кончается на границе слота после
    // ячейки данных, вне его - по CLK; вход и выход задержаны на две ячейки.
    bool clock_cell_ = true; // следующая ячейка - синхроимпульс; переключается каждой ячейкой, A1 / C2 ставит
    bool cells1_ = false, cells2_ = false, half2_ = false, rclk_since_mr_ = false;
    // Запись.
    uint8_t mark_pipe_ = 0, clock_pipe_ = 0, data_pipe_ = 0;
    bool clock_drop_ = false, no_write_d_ = false, pre_wg_ = false, wg_q_ = false;
    uint16_t crc_ = 0;
    // Дисковод.
    bool mr_s_ = false, ready_r_ = false, tr00_r_ = false, ip_r_ = false;
    bool ip_sample_ = false, ip_sample_d_ = false, hlt_s_ = false, hld_r_ = false, step_q_ = false;
    // Шина.
    uint8_t dal_in_ = 0, capture_ = 0, enable_ = 0;
    bool powered_ = false;
    // Пропуск: прошлое слово ушло на себя; преамбула, которую оно видело.
    bool ff_rep_ = false, ff_pre_ = false;
    uint64_t ff_lim_ = 0;                     // вызов, кончающийся раньше, лишь двигает время и головку
    bool ff_lazy_    = false;                 // слова от we_t_ до t ещё не применены
    bool mr_prev_ = false, no_defer_ = false; // /MR на прошлом pins_changed; середину не откладывать
    bool ff_part_     = false;                // середина слова от we_t_ до t не проиграна
    bool ff_ok_       = false;                // прошлый пропуск прошёл проверки и кончился без события
    uint8_t br_known_ = 0, br_val_ = 0;       // branch().next == pc по (строб, CNT2 == 0, write_ready_)
    Window lz_kind_    = Window::Loop;
    uint16_t lz_cells_ = 0; // запись: ячейки слов окна, слово i - разряды 2i, 2i + 1
    uint32_t lz_pos0_  = 0; // запись: слот головки в начале окна
    uint64_t lz_t0_    = 0; // запись: начало окна
    uint64_t lz_wt_    = 0; // запись: ячейки положены до этого конца слова
    uint8_t lz_bits_   = 0; // биты данных слов окна, младший - последнее слово lz_t_
    uint64_t lz_t_     = 0; // конец последнего слова окна
    // Состояние в конце последнего слова: возврат к нему из середины слова.
    uint64_t we_t_             = 0;
    const SlotDrive* we_drive_ = nullptr;
    bool we_cc_ = false, we_c1_ = false, we_c2_ = false, we_rsm_ = false;
};

} // namespace devices::bdi
