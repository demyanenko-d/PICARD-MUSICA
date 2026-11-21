// SPDX-License-Identifier: MIT
#include "devices/bdi/vg93.h"

namespace devices::bdi {

namespace {

// Времена фаз, полупериоды; замер Vg93w на дорожке TR-DOS.
constexpr uint64_t kBusyAt    = 32;   // BUSY и сброс флагов после записи команды
constexpr uint64_t kCmpSeek   = 192;  // тип I: первое сравнение TRK с целью
constexpr uint64_t kCmpRest   = 264;  // у Restore - после FF в TRK
constexpr uint64_t kTr00Done  = 608;  // TR00 при шаге наружу: конец через 304 мкс
constexpr uint64_t kLeadIn    = 564;  // импульс - на тике не раньше 282 мкс после сравнения
constexpr uint64_t kLeadOut   = 636;  // наружу - 318 мкс (проверка TR00)
constexpr uint64_t kLeadStep  = 88;   // у Step без сравнения импульс раньше на 44 мкс
constexpr uint64_t kStepGrid  = 2000; // тик делителя шагов, 1 мс
constexpr uint64_t kGridPhase = 56;   // тики - на 28 мкс каждой миллисекунды
constexpr uint64_t kCmpBefore = 1872; // сравнение - за 936 мкс до тика следующего шага
constexpr uint64_t kRate[4]   = {6000, 12000, 20000, 30000};
constexpr uint64_t kSettle    = 30000; // успокоение головки и флаг E, 15 мс
constexpr uint64_t kSearchAt  = 38;    // тип II/III: начало поиска
constexpr uint64_t kNotReady  = 62;    // тип II/III без READY или с защитой записи: конец
constexpr uint64_t kDrqLag    = 20;    // DRQ - через 10 мкс после конца байта
constexpr uint64_t kReadEnd   = 60;    // INTRQ - через 30 мкс после CRC
constexpr uint64_t kAddrEnd   = 52;    // Read Address: после CRC заголовка
constexpr uint64_t kVerifyEnd = 108;   // проверка дорожки: после CRC заголовка
constexpr uint64_t kRnfLag    = 52;    // RNF - после шестого индекса
constexpr uint64_t kWrCheck   = 36;    // запись: первый байт обязан быть через 8 байтов после DRQ
constexpr uint64_t kWrLost    = 44;
constexpr uint64_t kTrackEnd  = 28;  // Read/Write Track: конец после индекса
constexpr uint64_t kFmtDrq    = 208; // Write Track: первый DRQ
constexpr uint64_t kForceAt   = 28;
constexpr uint64_t kIdxLen    = 125 * kByte; // импульс индекса 4 мс
constexpr uint64_t kIdxSeen   = 4;           // статус видит индекс с конца микрослова
constexpr uint64_t kUnloadLag = 24;          // HLD снимается после 15-го индекса простоя
constexpr int kIdxTimeout     = 6;           // поиск - до шестого индекса
constexpr int kMaxCyl         = 83;          // головка встаёт на упор за 80-м цилиндром
constexpr uint32_t kLastCmp   = ~0u;         // k_ типа I: следующее сравнение - конец команды

// Ближайший момент не раньше t, когда начинается байт p оборота.
uint64_t at(uint64_t t, uint32_t p) {
    const uint64_t x = t - t % kRev + p * kByte;
    return x >= t ? x : x + kRev;
}

uint32_t dist(uint32_t from, uint32_t to) {
    return (to + kTrackLen - from) % kTrackLen;
}

// Команды по старшим разрядам.
bool is_seek(uint8_t c) { // Restore, Seek
    return c < 0x20;
}
bool is_write(uint8_t c) {
    return (c & 0xe0) == 0xa0;
}
bool is_addr(uint8_t c) {
    return (c & 0xf0) == 0xc0;
}
bool is_rtrack(uint8_t c) {
    return (c & 0xf0) == 0xe0;
}
bool is_wtrack(uint8_t c) {
    return (c & 0xf0) == 0xf0;
}

// Байт p дорожки с переносом через индекс.
void put(Track& tr, uint32_t p, uint8_t v, bool mark = false) {
    tr.put(p % kTrackLen, v, mark);
}

} // namespace

void Vg93::reset(uint64_t t) {
    sys_ = 0xff;
    write(4, 0, t);
}

Track* Vg93::track() {
    if (!selected()) return nullptr;
    bool started   = false;
    Track* const t = drv().track(side(), now_, started);
    if (started) st_.waits++;
    return t;
}

void Vg93::write(unsigned reg, uint8_t v, uint64_t t) {
    now_ = t;
    if (reg == 4) {
        const uint8_t old = sys_;
        sys_              = v;
        if (((v ^ old) & kSysDriveMask) != 0) {
            cur_ = v & kSysDriveMask;
            drv().drop();
        }
        const bool mr = (v & kSysResetNBit) == 0;
        if (mr && !mr_) {
            // /MR: всё стоит, SEC = 1, после снятия - RESTORE 03h.
            mr_     = true;
            busy_   = false;
            drq_    = false;
            intrq_  = false;
            hld_    = false;
            sec_    = 1;
            ph_     = Ph::Idle;
            unload_ = kNever;
        } else if (!mr && mr_) {
            mr_  = false;
            cmd_ = 0x03;
            ph_  = Ph::Start;
            due_ = t + kBusyAt;
        }
        return;
    }
    if (mr_) return;
    // Защёлка шины у кристалла одна: записи в одно микрослово отдают
    // прежнему регистру байт второй.
    if (t / 8 == wr_t_ / 8 && reg != wr_reg_) {
        if (wr_reg_ == 1) trk_ = v;
        if (wr_reg_ == 2) sec_ = v;
        if (wr_reg_ == 3) dat_ = v;
    }
    wr_t_   = t;
    wr_reg_ = reg;
    switch (reg) {
        case 0:
            intrq_   = false;
            int_now_ = false;
            if ((v & 0xf0) == 0xd0) {
                cmd_       = busy_ ? cmd_ : v;
                int_index_ = (v & 0x04) != 0;
                int_now_   = (v & 0x08) != 0;
                ph_        = Ph::Force;
                due_       = t + kForceAt;
            } else if (!busy_ && ph_ != Ph::Start) {
                cmd_       = v;
                int_index_ = false;
                ph_        = Ph::Start;
                due_       = t + kBusyAt;
            }
            break;
        case 1:
            trk_ = v;
            break;
        case 2:
            sec_ = v;
            break;
        default:
            dat_ = v;
            drq_ = false;
    }
}

void Vg93::read(unsigned reg, uint64_t t) {
    now_ = t;
    if (reg == 0 && !int_now_) intrq_ = false;
    if (reg == 3) drq_ = false;
}

uint8_t Vg93::status(uint64_t t) const {
    uint8_t s = static_cast<uint8_t>(flags_ | (busy_ ? kStBusyBit : 0) | (!selected() ? kStNotReadyBit : 0));
    if (!t1_) return static_cast<uint8_t>(s | (drq_ ? kStDrqBit : 0));
    const Drive& d = drv();
    if (selected() && d.write_protect()) s |= kStWprotBit;
    if (selected() && (sys_ & kSysHltBit) != 0) s |= kStHeadBit;
    if (selected() && d.cyl == 0) s |= kStTr00Bit;
    if ((sys_ & kSysHltBit) == 0 || (spins() && (t + kRev - kIdxSeen) % kRev < kIdxLen)) s |= kStIndexBit;
    return s;
}

uint64_t Vg93::due() const {
    uint64_t d = ph_ == Ph::Idle ? unload_ : due_;
    // Края импульса индекса: в статусе типа I и для D4.
    if ((t1_ || int_index_) && spins()) {
        const uint64_t r = (now_ + kRev - kIdxSeen) % kRev;
        const uint64_t e = now_ - r + (r < kIdxLen ? kIdxLen : kRev);
        if (e < d) d = e;
    }
    return d;
}

void Vg93::step() {
    const uint64_t t     = due();
    const uint64_t phase = ph_ == Ph::Idle ? unload_ : due_;
    now_                 = t;
    if (t != phase) {
        if (int_index_ && t % kRev == kIdxSeen) intrq_ = true;
        return;
    }
    const uint8_t c = cmd_;
    Track* tr       = nullptr;
    switch (ph_) {
        case Ph::Idle: // простой 15 оборотов: головка поднимается
            hld_         = false;
            unload_      = kNever;
            drv().tracks = nullptr;
            return;
        case Ph::Start:
            start(t);
            return;
        case Ph::Force:
            // Прерывание: идущая команда стоит, флаги как были; без
            // команды статус - типа I.
            if (!busy_) {
                t1_    = true;
                flags_ = 0;
            }
            busy_  = false;
            intrq_ = int_now_;
            ph_    = Ph::Idle;
            return;
        case Ph::Cmp: {
            bool done = is_seek(c) ? trk_ == dat_ : k_ != 0;
            if (is_seek(c) && !done) dir_ = dat_ > trk_ ? 1 : -1;
            if (!done && dir_ < 0 && selected() && drv().cyl == 0) {
                trk_ = 0;
                due_ = t + kTr00Done;
                ph_  = Ph::Cmp;
                k_   = kLastCmp;
                return;
            }
            if (k_ == kLastCmp) done = true;
            if (!done) {
                const uint64_t x = t + (dir_ > 0 ? kLeadIn : kLeadOut) - (is_seek(c) ? 0 : kLeadStep) - kGridPhase;
                due_             = x + (kStepGrid - x % kStepGrid) % kStepGrid + kGridPhase;
                ph_              = Ph::Pulse;
            } else if ((c & 0x04) != 0) {
                hld_ = true;
                search(t + kSettle);
            } else {
                finish(t, 0);
            }
            return;
        }
        case Ph::Pulse:
            if (is_seek(c) || (c & 0x10) != 0) trk_ = static_cast<uint8_t>(trk_ + dir_);
            if (selected() && drv().step(dir_, kMaxCyl)) (void)track(); // цилиндр заказан с шагом, а не с поиском
            k_++;
            due_ = t + kRate[c & 3] - kCmpBefore;
            ph_  = Ph::Cmp;
            return;
        case Ph::Search:
            search(s_);
            return;
        case Ph::Read: {
            tr = track();
            if (drq_) flags_ |= kStLostBit;
            dat_ = tr != nullptr ? tr->at(pos_ + k_) : 0;
            if (k_ == 0 && is_addr(c)) sec_ = dat_;
            drq_ = true;
            if (++k_ < n_) {
                due_ += kByte;
                return;
            }
            const uint64_t crc_end = t - kDrqLag + (is_addr(c) || is_rtrack(c) ? 0 : 2 * kByte);
            if (is_rtrack(c)) {
                finish(base_ + kRev + kTrackEnd, 0);
            } else if (is_addr(c)) {
                finish(crc_end + kAddrEnd, id_->id_ok ? 0 : kStCrcBit);
            } else if (!id_->data_ok) {
                finish(crc_end + kReadEnd, kStCrcBit);
            } else if ((c & 0x10) != 0) {
                due_ = crc_end;
                ph_  = Ph::Next;
            } else {
                finish(crc_end + kReadEnd, 0);
            }
            return;
        }
        case Ph::Next: // несколько секторов: следующий
            sec_++;
            search(t);
            return;
        case Ph::WrFirst: // DRQ первого байта; Write Track проверяет его через три байта
            drq_ = true;
            due_ = is_wtrack(c) ? t + 3 * kByte : base_ + 20 * kByte + kWrCheck;
            ph_  = is_wtrack(c) ? Ph::FmtCheck : Ph::WrCheck;
            return;
        case Ph::WrCheck: {
            tr = track();
            if (drq_ || tr == nullptr) {
                flags_ |= kStLostBit;
                finish(t - kWrCheck + kWrLost, 0);
                return;
            }
            // 12 нулей, три A1 и метка - перед данными.
            for (uint32_t i = 0; i < 12; i++) {
                put(*tr, pos_ + kTrackLen - 16 + i, 0);
            }
            for (uint32_t i = 0; i < 3; i++) {
                put(*tr, pos_ + kTrackLen - 4 + i, 0xa1, true);
            }
            const uint8_t mark = (c & 1) != 0 ? 0xf8 : 0xfb;
            put(*tr, pos_ + kTrackLen - 1, mark);
            crc_ = crc16(crc16(0xffff, kA1x3, 3), &mark, 1);
            k_   = 1;
            due_ = base_ + 48 * kByte + kDrqLag;
            ph_  = Ph::Write;
            return;
        }
        case Ph::Write: {
            tr        = track();
            uint8_t v = dat_;
            if (k_ > 1 && drq_) {
                flags_ |= kStLostBit;
                v       = 0;
            }
            if (tr != nullptr) put(*tr, pos_ + k_ - 1, v);
            crc_ = crc16(crc_, &v, 1);
            if (k_ < n_) {
                drq_ = true;
                k_++;
                due_ += kByte;
                return;
            }
            if (tr != nullptr) {
                put(*tr, pos_ + n_, static_cast<uint8_t>(crc_ >> 8));
                put(*tr, pos_ + n_ + 1, static_cast<uint8_t>(crc_));
                put(*tr, pos_ + n_ + 2, 0xff);
                TrackId& id = const_cast<TrackId&>(*id_);
                id.dam      = static_cast<uint16_t>((pos_ + kTrackLen - 1) % kTrackLen);
                id.mark     = (c & 1) != 0 ? 0xf8 : 0xfb;
                id.data_ok  = true;
                drv().disk->written(drv().cyl, side());
            }
            if ((c & 0x10) != 0) { // несколько секторов: следующий, как у чтения
                due_ = t - kDrqLag + 4 * kByte;
                ph_  = Ph::Next;
                return;
            }
            finish(t - kDrqLag + 4 * kByte + kReadEnd, 0);
            return;
        }
        case Ph::FmtCheck:
            if (drq_) {
                flags_ |= kStLostBit;
                finish(t + kDrqLag, 0);
                return;
            }
            search(t);
            return;
        case Ph::Format: {
            // Байт k дорожки от индекса: F5 - A1-метка и начало CRC, F6 -
            // C2-метка, F7 - два байта CRC.
            tr        = track();
            uint8_t v = dat_;
            if (k_ > 0 && drq_) {
                flags_ |= kStLostBit;
                v       = 0;
            }
            if (tr != nullptr) {
                if (v == 0xf5) {
                    if (k_ == 0 || !tr->mark(k_ - 1) || tr->data[k_ - 1] != 0xa1) crc_ = 0xffff;
                    put(*tr, k_, 0xa1, true);
                    crc_ = crc16(crc_, kA1x3, 1);
                } else if (v == 0xf6) {
                    put(*tr, k_, 0xc2, true);
                } else if (v == 0xf7) {
                    put(*tr, k_, static_cast<uint8_t>(crc_ >> 8));
                    if (k_ + 1 < kTrackLen) put(*tr, ++k_, static_cast<uint8_t>(crc_));
                } else {
                    put(*tr, k_, v);
                    crc_ = crc16(crc_, &v, 1);
                }
            }
            drq_ = true;
            if (++k_ < kTrackLen) {
                due_ = base_ + k_ * kByte + kDrqLag;
                return;
            }
            if (tr != nullptr) {
                track_index(*tr);
                drv().disk->written(drv().cyl, side());
            }
            finish(base_ + kRev + kTrackEnd, 0);
            return;
        }
        case Ph::End:
            end(t);
            return;
    }
}

void Vg93::start(uint64_t t) {
    const uint8_t c   = cmd_;
    const uint64_t t0 = t - kBusyAt;
    busy_             = true;
    drq_              = false;
    flags_            = 0;
    t1_               = c < 0x80;
    unload_           = kNever;
    if (t1_) {
        if ((c & 0x08) != 0) {
            hld_ = true;
        } else if ((c & 0x04) == 0) {
            hld_ = false;
        }
        if (c < 0x10) {
            trk_ = 0xff;
            dat_ = 0;
        }
        if ((c & 0xe0) == 0x40) dir_ = 1;
        if ((c & 0xe0) == 0x60) dir_ = -1;
        k_   = 0;
        due_ = t0 + (c < 0x10 ? kCmpRest : kCmpSeek);
        ph_  = Ph::Cmp;
        return;
    }
    const bool wr = is_write(c) || is_wtrack(c);
    if (!selected()) {
        finish(t0 + kNotReady, 0);
        return;
    }
    if (wr && drv().write_protect()) {
        finish(t0 + kNotReady, kStWprotBit);
        return;
    }
    const uint64_t s = t0 + kSearchAt + ((c & 0x04) != 0 ? kSettle : 0);
    if (is_wtrack(c)) {
        due_ = t0 + kFmtDrq;
        ph_  = Ph::WrFirst;
        return;
    }
    search(s);
}

// Поиск от момента s: ближайший подходящий заголовок, у дорожек - индекс;
// до шестого индекса, иначе RNF (у проверки типа I - Seek Error). Дорожки
// ещё нет - ждать источника.
void Vg93::search(uint64_t s) {
    s_  = s;
    ph_ = Ph::Search;
    if (s > now_) {
        due_ = s;
        return;
    }
    const Track* tr = track();
    if (tr == nullptr) {
        due_ = kNever;
        return;
    }
    const uint8_t c    = cmd_;
    const uint64_t tmo = spins() ? at(s, 0) + (kIdxTimeout - 1) * kRev : kNever;
    if (is_rtrack(c) || is_wtrack(c)) {
        base_ = at(s, 0);
        if (!spins()) base_ = kNever;
        k_   = 0;
        n_   = kTrackLen - 1; // байт под индексом DRQ уже не даёт
        pos_ = 0;
        due_ = base_ == kNever ? kNever : base_ + (is_rtrack(c) ? kByte : 0) + kDrqLag;
        ph_  = is_rtrack(c) ? Ph::Read : Ph::Format;
        crc_ = 0xffff;
        return;
    }
    uint64_t best = kNever, bad = kNever;
    const TrackId* id = nullptr;
    for (int i = 0; i < tr->nid; i++) {
        const TrackId& d = tr->id[i];
        bool match       = is_addr(c) || d.c == trk_;
        if (!t1_ && !is_addr(c)) match = match && d.r == sec_ && ((c & 0x02) == 0 || (d.h != 0) == ((c & 0x08) != 0));
        if (!match) continue;
        const uint64_t f = at(s + 4 * kByte, d.a1);
        if (!d.id_ok && !is_addr(c)) {
            if (f < bad) bad = f;
        } else if (f < best) {
            best = f;
            id   = &d;
        }
    }
    if (id == nullptr || best >= tmo) {
        if (bad < tmo) flags_ |= kStCrcBit;
        if (tmo == kNever) {
            due_ = kNever;
            return;
        }
        finish(tmo + kRnfLag, t1_ ? kStSeekErrBit : kStRnfBit);
        return;
    }
    if (bad < best) flags_ |= kStCrcBit;
    id_   = id;
    base_ = best;
    k_    = 0;
    if (t1_) {
        flags_ &= static_cast<uint8_t>(~kStCrcBit);
        finish(best + 10 * kByte + kVerifyEnd, 0);
    } else if (is_addr(c)) {
        pos_ = id->a1 + 4u;
        n_   = 6;
        due_ = best + 5 * kByte + kDrqLag;
        ph_  = Ph::Read;
    } else if (is_write(c) && !drv().disk->stores(drv().cyl, side(), id->r)) {
        // За концом короткого образа: писать некуда - ошибка записи, а не
        // потеря молча (файл образа не дописывается).
        finish(best + 12 * kByte, kStWrtFaultBit);
    } else if (is_write(c)) {
        pos_ = (id->a1 + 48u) % kTrackLen;
        n_   = sector_len(id->n);
        due_ = best + 12 * kByte + kDrqLag;
        ph_  = Ph::WrFirst;
    } else if (id->dam == kNoDam) {
        finish(best + 58 * kByte, kStRnfBit);
    } else {
        pos_ = (id->dam + 1u) % kTrackLen;
        n_   = sector_len(id->n);
        if ((id->mark & 0xfe) == 0xf8) flags_ |= kStWrtFaultBit;
        due_ = best + (dist(id->a1, pos_) + 1) * kByte + kDrqLag;
        ph_  = Ph::Read;
    }
}

// INTRQ микрокод ставит на конце слова: t % 8 == 7.
void Vg93::finish(uint64_t t, uint8_t flags) {
    fin_ = flags;
    due_ = t | 7;
    ph_  = Ph::End;
}

void Vg93::end(uint64_t t) {
    flags_ |= fin_;
    busy_   = false;
    intrq_  = true;
    ph_     = Ph::Idle;
    if (!t1_) {
        st_.ends++;
        if ((flags_ & kStLostBit) != 0) st_.lost++;
        if ((flags_ & kStCrcBit) != 0) st_.crc++;
        if ((flags_ & kStRnfBit) != 0) st_.rnf++;
    }
    // 15 индексов простоя - головка поднимается.
    unload_ = spins() ? at(t + 1, 0) + 14 * kRev + kUnloadLag : kNever;
}

// Смена диска выбранного дисковода посреди команды: позиция в дорожке и
// заголовок - от прежнего диска. Чтение и запись кончаются ошибкой (TR-DOS
// повторит на новом), иначе остаток сектора ушёл бы в чужой образ. Поиск,
// ждавший цилиндра, ищет заново: drop() снял ожидание, и будильника от
// источника больше не будет.
void Vg93::insert(int drive, Disk* src, uint64_t t) {
    now_               = t;
    const bool changed = drv_[drive].disk != src;
    drv_[drive].disk   = src;
    if (drive != cur_) return;
    drv().drop();
    if (ph_ == Ph::Search && due_ == kNever) {
        search(s_ > t ? s_ : t);
    } else if (changed && ph_ >= Ph::Read && ph_ <= Ph::Format && ph_ != Ph::Next) {
        finish(t, kStRnfBit);
    }
}

void Vg93::source_ready(int drive, uint64_t t) {
    now_     = t;
    Drive& d = drv();
    if (drive != cur_ || d.tracks != nullptr || !d.waiting) return;
    (void)track();
    if (d.tracks == nullptr) return;
    d.waiting        = false;
    const uint64_t w = t - d.wait_t;
    if (w > st_.wait_max) st_.wait_max = static_cast<uint32_t>(w);
    if (ph_ == Ph::Search && due_ == kNever) search(s_ > t ? s_ : t);
}

void Vg93::swap(uint64_t t) {
    Disk* const a = drv_[0].disk;
    drv_[0].disk  = drv_[1].disk;
    drv_[1].disk  = a;
    insert(cur_, drv().disk, t);
}

} // namespace devices::bdi
