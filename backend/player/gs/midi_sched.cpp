// SPDX-License-Identifier: MIT
#include "player/gs/midi_sched.h"

#include <cstring>

namespace player::gs {
namespace {

// Длина события по статусному байту: сколько байт данных идёт следом.
uint8_t data_bytes(uint8_t status) {
    switch (status & 0xf0u) {
        case 0xc0u: // Program Change
        case 0xd0u: // Channel Pressure
            return 1;
        default:
            return 2;
    }
}

constexpr uint8_t kMetaStatus     = 0xffu;
constexpr uint8_t kMetaTrackName  = 0x03u;
constexpr uint8_t kMetaTempo      = 0x51u;
constexpr uint8_t kMetaEndOfTrack = 0x2fu;
constexpr uint8_t kSysExStart     = 0xf0u;
constexpr uint8_t kSysExCont      = 0xf7u;
constexpr uint8_t kFrameEnd       = 0xffu;

uint32_t be24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

} // namespace

bool MidiSched::read_varlen(uint32_t& pos, uint32_t end, uint32_t& out) const {
    uint32_t v = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        if (pos >= end) return false;
        const uint8_t b = file_[pos++];
        v               = (v << 7) | (b & 0x7fu);
        if ((b & 0x80u) == 0) {
            out = v;
            return true;
        }
    }
    return false;
}

void MidiSched::arm(Track& t) {
    uint32_t delta = 0;
    if (t.pos >= t.end || !read_varlen(t.pos, t.end, delta)) {
        t.done = true;
        return;
    }
    t.tick += delta;
}

// Имя дорожки берётся из первого же мета-события #03. Ищем не дальше
// начала: в настоящих файлах имя стоит до первой ноты, а полный проход по
// всем дорожкам стоил бы прохода по всему файлу.
void MidiSched::scan_name(const Track& t, uint8_t* dst) const {
    for (uint8_t i = 0; i < kMidiNameChars; ++i) {
        dst[i] = ' ';
    }
    uint32_t pos    = t.pos;
    uint8_t running = 0;
    for (uint8_t step = 0; step < 32 && pos < t.end; ++step) {
        uint32_t delta = 0;
        if (!read_varlen(pos, t.end, delta)) return;
        if (pos >= t.end) return;
        uint8_t status = file_[pos];
        if ((status & 0x80u) != 0) {
            ++pos;
            if (status < 0xf0u) running = status;
        } else {
            status = running;
            if (status == 0) return;
        }
        if (status == kMetaStatus) {
            if (pos >= t.end) return;
            const uint8_t type = file_[pos++];
            uint32_t n         = 0;
            if (!read_varlen(pos, t.end, n)) return;
            if (type == kMetaTrackName) {
                const uint32_t take = n < kMidiNameChars ? n : kMidiNameChars;
                for (uint32_t i = 0; i < take && pos + i < t.end; ++i) {
                    const uint8_t ch = file_[pos + i];
                    dst[i]           = (ch >= 0x20u && ch < 0x7fu) ? ch : ' ';
                }
                return;
            }
            pos += n;
            continue;
        }
        if (status == kSysExStart || status == kSysExCont) {
            uint32_t n = 0;
            if (!read_varlen(pos, t.end, n)) return;
            pos += n;
            continue;
        }
        pos += data_bytes(status);
    }
}

// Сколько тактов в самой длинной дорожке. События разбираются тем же
// порядком, что и при игре, но своими указателями: рабочие курсоры к этому
// времени уже стоят на первом событии.
uint32_t MidiSched::scan_total_ticks() {
    uint32_t longest = 0;
    for (uint8_t i = 0; i < track_count_; ++i) {
        uint32_t pos       = tracks_[i].pos;
        const uint32_t end = tracks_[i].end;
        uint32_t tick      = tracks_[i].tick; // первая задержка уже учтена
        uint8_t running    = 0;
        while (pos < end) {
            if (pos >= end) break;
            uint8_t status = file_[pos];
            if ((status & 0x80u) != 0) {
                ++pos;
                if (status < 0xf0u) running = status;
            } else {
                status = running;
                if (status == 0) break;
            }
            if (status == kMetaStatus) {
                if (pos >= end) break;
                const uint8_t type = file_[pos++];
                uint32_t len       = 0;
                if (!read_varlen(pos, end, len)) break;
                if (type == kMetaEndOfTrack) break;
                pos += len;
            } else if (status == kSysExStart || status == kSysExCont) {
                uint32_t len = 0;
                if (!read_varlen(pos, end, len)) break;
                pos += len;
            } else {
                pos += data_bytes(status);
            }
            uint32_t delta = 0;
            if (pos >= end || !read_varlen(pos, end, delta)) break;
            tick += delta;
        }
        if (tick > longest) longest = tick;
    }
    return longest;
}

bool MidiSched::begin(const uint8_t* file, uint32_t len, const devices::gs::MidiFileFacts& facts, uint8_t* work, uint8_t* state) {
    file_          = file;
    len_           = len;
    work_          = work;
    state_         = state;
    ready_         = false;
    finished_      = true;
    track_count_   = 0;
    frame_slot_    = 0;
    sysex_track_   = kMidiTracksMax;
    sysex_left_    = 0;
    tick_base_q16_ = 0;
    us_since_base_ = 0;
    now_q16_       = 0;
    tempo_us_      = 500000;
    events_        = 0;
    bytes_out_     = 0;
    skipped_       = 0;
    stopped_at_    = 0;
    total_ticks_   = 0;
    std::memset(keys_, 0, sizeof(keys_));
    if (file == nullptr || work == nullptr || state == nullptr || len < 14) return false;
    std::memset(state_, 0, devices::gs::kMidiStateBytes);
    std::memset(blink_, 0, sizeof(blink_));
    std::memset(bars_, 0, sizeof(bars_));

    std::memset(work_, 0, kMidiWorkSize);
    // Имена - пробелами: плеер печатает их как есть.
    std::memset(work_ + kMidiNamesAt, ' ', devices::gs::kMidiNamesBytes);

    division_ = facts.division;
    // SMPTE (старший бит разрешения) не поддерживается: таких файлов
    // единицы, а счёт времени у них другой.
    if (division_ == 0 || (division_ & 0x8000u) != 0) return false;

    // Граница - принятое, а не длина из блоба: поле там шестнадцатиразрядное,
    // и у файла длиннее 64 КБ по нему разбирается одна дорожка из двадцати.
    // Принятое - это ровно то, что прислал плеер, вместе с десятью байтами
    // добивки, и на "MTrk" добивка не похожа.
    const uint32_t limit = len;
    stopped_at_          = 0;

    uint32_t pos = facts.first_track;
    for (uint8_t i = 0; i < kMidiTracksMax; ++i) {
        // Заголовок дорожки ищется, а не берётся на веру: между кусками
        // встречается мусор, и сорваться на нём значило бы потерять весь
        // хвост файла. Ищем вперёд от ожидаемого места.
        while (pos + 8 <= limit && (file_[pos] != 'M' || file_[pos + 1] != 'T' || file_[pos + 2] != 'r' || file_[pos + 3] != 'k')) {
            ++pos;
            ++skipped_;
        }
        if (pos + 8 > limit) {
            stopped_at_ = pos;
            break;
        }
        const uint32_t chunk = be32(file_ + pos + 4);
        const uint32_t start = pos + 8;
        uint32_t end         = start + chunk;
        if (end > limit) end = limit; // обрезанный файл играем до конца принятого
        Track& t  = tracks_[track_count_];
        t.pos     = start;
        t.end     = end;
        t.tick    = 0;
        t.running = 0;
        t.done    = start >= end;
        scan_name(t, work_ + kMidiNamesAt + static_cast<uint32_t>(track_count_) * kMidiNameChars);
        if (!t.done) arm(t);
        ++track_count_;
        // Длина куска может врать; следующий заголовок всё равно ищется.
        pos = end;
    }
    if (track_count_ == 0) return false;

    total_ticks_ = scan_total_ticks();

    // Справка о файле (#04). Разбор #EE69: первый байт только выбирает,
    // печатать ли ведущие нули, а САМО ЧИСЛО - следующие три байта,
    // старшим вперёд. Плеер печатает его строкой Total и по нему же рисует
    // ход. Разрешение сюда не идёт: PPQN он читает из MThd сам.
    uint8_t* info = work_ + kMidiInfoAt;
    info[0]       = 0; // ведущие нули гасить
    info[1]       = static_cast<uint8_t>((total_ticks_ >> 16) & 0xffu);
    info[2]       = static_cast<uint8_t>((total_ticks_ >> 8) & 0xffu);
    info[3]       = static_cast<uint8_t>(total_ticks_ & 0xffu);

    ready_    = true;
    finished_ = false;
    // Признак "играем" - иначе плеер уйдёт на следующий трек, не начав.
    state_[kMidiStatePlaying]    = 1;
    state_[kMidiStateConst]      = 0x78u;
    state_[kMidiStateConst + 1u] = 0x00u;
    return true;
}

void MidiSched::key_change(uint8_t note, bool on) {
    if (note < kMidiKeyLowest) return;
    const uint8_t key = static_cast<uint8_t>(note - kMidiKeyLowest);
    if (key >= kMidiKeysMax) return;
    if (on) {
        if (keys_[key] < 0xffu) ++keys_[key];
    } else if (keys_[key] != 0) {
        --keys_[key];
    }
}

// Три ряда по 25 байт. Ряд 0 - клавиша тремя пикселями, ряды 1 и 2 -
// полоска в один пиксель: белым в первый, чёрным во второй, потому что
// рисуются они на разной высоте.
void MidiSched::draw_keys() {
    uint8_t* const rows = state_ + kMidiStateKeys;
    std::memset(rows, 0, kMidiKeyRow * 3u);
    for (uint8_t key = 0; key < kMidiKeysMax; ++key) {
        if (keys_[key] == 0) continue;
        const uint8_t semitone = static_cast<uint8_t>(key % 12u);
        const uint16_t x       = static_cast<uint16_t>(key / 12u) * kMidiOctavePx + kMidiKeyPixel[semitone];
        for (uint8_t d = 0; d < 3; ++d) {
            const uint16_t px = static_cast<uint16_t>(x + d);
            if (px >= kMidiKeyRow * 8u) break;
            rows[px >> 3] = static_cast<uint8_t>(rows[px >> 3] | (0x80u >> (px & 7u)));
        }
        // Полоска - по середине клавиши.
        const uint16_t mark = static_cast<uint16_t>(x + 1u);
        if (mark >= kMidiKeyRow * 8u) continue;
        const uint16_t row = kMidiKeyBlack[semitone] ? 2u : 1u;
        uint8_t* const r   = rows + row * kMidiKeyRow;
        r[mark >> 3]       = static_cast<uint8_t>(r[mark >> 3] | (0x80u >> (mark & 7u)));
    }
}

bool MidiSched::emit(Track& t, uint16_t& n) {
    uint8_t* out = work_ + kMidiFrameAt + static_cast<uint32_t>(frame_slot_) * kMidiFrameMax;
    if (t.pos >= t.end) {
        t.done = true;
        return true;
    }
    uint8_t status = file_[t.pos];
    if ((status & 0x80u) != 0) {
        ++t.pos;
        if (status < 0xf0u) t.running = status;
    } else {
        status = t.running;
        if (status == 0) { // мусор вместо события: дорожку дальше не читаем
            t.done = true;
            return true;
        }
    }

    if (status == kMetaStatus) {
        if (t.pos >= t.end) {
            t.done = true;
            return true;
        }
        const uint8_t type = file_[t.pos++];
        uint32_t len       = 0;
        if (!read_varlen(t.pos, t.end, len)) {
            t.done = true;
            return true;
        }
        if (type == kMetaTempo && len == 3 && t.pos + 3 <= t.end) {
            const uint32_t v = be24(file_ + t.pos);
            if (v != 0 && v != tempo_us_) {
                // Опора времени переставляется сюда: дальше счёт идёт уже
                // новым темпом, а пройденное не пересчитывается.
                tick_base_q16_ = now_q16_;
                us_since_base_ = 0;
                tempo_us_      = v;
            }
        }
        if (type == kMetaEndOfTrack) {
            t.done = true;
            return true;
        }
        t.pos += len;
        arm(t);
        return true;
    }

    if (status == kSysExStart || status == kSysExCont) {
        // Статус #F0..#F7 running-статусом не становится, поэтому курсор
        // двигать нельзя, пока событие не пошло в линию: иначе в следующем
        // кадре тело дампа прочтётся как события и уйдёт нотами. Статус
        // здесь всегда явный, и вернуть его на место - шаг назад.
        if (n >= kMidiFrameData) {
            --t.pos;
            return false;
        }
        uint32_t len = 0;
        if (!read_varlen(t.pos, t.end, len)) {
            t.done = true;
            return true;
        }
        // Байт статуса и тело: плеер пропускает #F0..#F7 в линию как есть.
        // Дамп длиннее порции режется между кадрами - байты идут подряд, и
        // получателю разрез не виден. Молча бросить его нельзя: в дампах
        // лежит настройка инструментов на всю песню.
        out[n++]      = status;
        uint32_t take = len;
        if (n + take > kMidiFrameData) take = kMidiFrameData - n;
        for (uint32_t i = 0; i < take; ++i) {
            out[n++] = t.pos < t.end ? file_[t.pos] : 0u;
            ++t.pos;
        }
        ++events_;
        if (take < len) {
            sysex_track_ = static_cast<uint8_t>(&t - tracks_);
            sysex_left_  = len - take;
            return false;
        }
        arm(t);
        return true;
    }

    const uint8_t nd = data_bytes(status);
    if (n + nd > kMidiFrameData) return false;
    // Статус пишем всегда, даже если в файле он опущен: плеер только
    // выталкивает байты в линию и состояния не ведёт.
    out[n++]               = status;
    const uint16_t data_at = n;
    for (uint8_t i = 0; i < nd; ++i) {
        out[n++] = t.pos < t.end ? file_[t.pos] : 0u;
        ++t.pos;
    }
    // Счёт звучащих клавиш - для картинки нот. Нота со скоростью ноль это
    // снятие, так условились в самом MIDI.
    const uint8_t hi = status & 0xf0u;
    if (hi == 0x90u) {
        key_change(out[data_at], out[data_at + 1] != 0);
        // Строка дорожки загорается на несколько кадров - так же гасит свои
        // счётчики и карта, по одному за кадр.
        if (out[data_at + 1] != 0) {
            const uint8_t idx = static_cast<uint8_t>(&t - tracks_);
            if (idx < kMidiTracksMax) blink_[idx] = kMidiBlinkFrames;
            // Столбик ноты: его высота - скорость, делённая пополам. Берём
            // громчайшую за кадр, а не последнюю: тихая нота поверх громкой
            // срезала бы столбик на ровном месте.
            const uint8_t bar = static_cast<uint8_t>(out[data_at] % kMidiBars);
            const uint8_t h   = static_cast<uint8_t>((out[data_at + 1] & 0x7fu) >> 1);
            if (h > bars_[bar]) bars_[bar] = h;
        }
    } else if (hi == 0x80u) {
        key_change(out[data_at], false);
    }
    ++events_;
    arm(t);
    return true;
}

uint16_t MidiSched::next_frame() {
    // Собираем в свободную половину: ту, что отдана плееру, трогать нельзя.
    frame_slot_  = static_cast<uint8_t>((frame_slot_ + 1u) % kMidiFrameSlots);
    uint8_t* out = work_ + kMidiFrameAt + static_cast<uint32_t>(frame_slot_) * kMidiFrameMax;
    uint16_t n   = 0;
    if (!ready_) {
        out[0] = kFrameEnd;
        return 1;
    }

    // Положение - от опоры, а не накоплением: такт кадра почти никогда не
    // целый, и накопленное усечение сдвигало бы события на кадр назад.
    us_since_base_ += kMidiFrameUs;
    now_q16_        = tick_base_q16_ + ((us_since_base_ * division_ << 16) / tempo_us_);

    // Хвост дампа с прошлого кадра - прежде всего: событие не кончилось, и
    // ничего другого до него в линию попасть не должно.
    if (sysex_track_ != kMidiTracksMax) {
        Track& t = tracks_[sysex_track_];
        while (sysex_left_ != 0 && n < kMidiFrameData) {
            out[n++] = t.pos < t.end ? file_[t.pos] : 0u;
            ++t.pos;
            --sysex_left_;
        }
        if (sysex_left_ == 0) {
            sysex_track_ = kMidiTracksMax;
            arm(t);
        }
    }

    for (;;) {
        uint8_t pick    = kMidiTracksMax;
        uint32_t lowest = 0;
        for (uint8_t i = 0; i < track_count_; ++i) {
            if (tracks_[i].done) continue;
            if (pick == kMidiTracksMax || tracks_[i].tick < lowest) {
                pick   = i;
                lowest = tracks_[i].tick;
            }
        }
        if (pick == kMidiTracksMax) {
            finished_ = true;
            break;
        }
        if ((static_cast<uint64_t>(lowest) << 16) > now_q16_) break;
        if (!emit(tracks_[pick], n)) break; // порция полна, остальное в следующий кадр
    }

    out[n++]    = kFrameEnd;
    bytes_out_ += n;
    // Признак "продолжать играть" держится здесь: конец расписания плеер
    // узнаёт только по нему.
    state_[kMidiStatePlaying] = finished_ ? 0u : 1u;
    // Положение в тактах: его плеер печатает строкой Ticks. Старшим байтом
    // вперёд, как складывает блок сама карта.
    const uint32_t pos        = position_ticks();
    state_[kMidiStatePos]     = 0;
    state_[kMidiStatePos + 1] = static_cast<uint8_t>((pos >> 16) & 0xffu);
    state_[kMidiStatePos + 2] = static_cast<uint8_t>((pos >> 8) & 0xffu);
    state_[kMidiStatePos + 3] = static_cast<uint8_t>(pos & 0xffu);
    draw_keys();
    // Столбики отдаются и гаснут: у карты их чистит само чтение блока.
    std::memcpy(state_ + kMidiStateBars, bars_, kMidiBars);
    std::memset(bars_, 0, sizeof(bars_));
    for (uint8_t i = 0; i < track_count_; ++i) {
        if (blink_[i] != 0) --blink_[i];
    }
    return n;
    // Указатель на собранное отдаёт frame(): он смотрит на ту же половину.
}

} // namespace player::gs
