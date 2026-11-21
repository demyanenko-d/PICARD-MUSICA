// SPDX-License-Identifier: MIT
#include "disk_image.h"

#include <cstring>
#include <new>

#include "core/util/le_bytes.h"

#include "mfm.h"

namespace devices::bdi {

namespace {

constexpr size_t kTrdSize   = 80 * 2 * 16 * 256; // 655360
constexpr int kTrdFreeSecs  = 2544;              // 159 дорожек x 16
constexpr size_t kHfeSide   = kTrackBytes;       // 12500 байт на сторону
constexpr int kMaxSecPerTrk = 32;
constexpr size_t kUdiHeader = 16;

using soundsinth::util::le16;
using soundsinth::util::le32;
using soundsinth::util::put_le16;

bool ends_with(const char* name, const char* ext) {
    if (name == nullptr) return false;
    const size_t n = std::strlen(name), e = std::strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != ext[i]) return false;
    }
    return true;
}

} // namespace

ImageFormat detect_format(const uint8_t* d, size_t size, const char* name) {
    if (size >= 9 && std::memcmp(d, "SINCLAIR", 8) == 0) return ImageFormat::Scl;
    if (size >= 14 && std::memcmp(d, "FDI", 3) == 0) return ImageFormat::Fdi;
    if (size >= kUdiHeader && std::memcmp(d, "UDI!", 4) == 0) return ImageFormat::Udi;
    if (size >= 512 && std::memcmp(d, "HXCPICFE", 8) == 0) return ImageFormat::Hfe;
    if (size >= 12 && (std::memcmp(d, "TD", 2) == 0 || std::memcmp(d, "td", 2) == 0) && ends_with(name, ".td0")) {
        return ImageFormat::Td0;
    }
    if (size > 0 && size % 256 == 0 && size <= kTrdSize) return ImageFormat::Trd;
    return ImageFormat::Unknown;
}

void DiskImage::close() {
    *this = DiskImage{};
}

bool DiskImage::open(ImageFormat fmt, uint8_t* file, size_t size, Arena& arena) {
    close();
    fmt_       = fmt;
    file_      = file;
    file_size_ = size;
    bool ok    = false;
    switch (fmt) {
        case ImageFormat::Trd:
            ok = open_trd(file, size, arena);
            break;
        case ImageFormat::Scl: {
            // SCL разворачивается в TRD: каталог, сектор 9, файлы с дорожки 1.
            auto* trd = static_cast<uint8_t*>(arena.take(kTrdSize));
            if (trd == nullptr || size < 9) break;
            const int files  = file[8];
            const size_t hdr = 9 + 14 * static_cast<size_t>(files);
            if (files > 128 || size < hdr) break;
            std::memset(trd, 0, kTrdSize);
            size_t src = hdr;
            int track = 1, sector = 0, used = 0;
            bool fits = true;
            for (int i = 0; i < files && fits; i++) {
                const uint8_t* h = file + 9 + 14 * i;
                const int nsec   = h[13];
                if (src + static_cast<size_t>(nsec) * 256 > size || used + nsec > kTrdFreeSecs) {
                    fits = false;
                    break;
                }
                uint8_t* e = trd + 16 * i;
                std::memcpy(e, h, 14);
                e[14] = static_cast<uint8_t>(sector);
                e[15] = static_cast<uint8_t>(track);
                std::memcpy(trd + (static_cast<size_t>(track) * 16 + static_cast<size_t>(sector)) * 256, file + src, static_cast<size_t>(nsec) * 256);
                src    += static_cast<size_t>(nsec) * 256;
                used   += nsec;
                sector += nsec;
                track  += sector / 16;
                sector %= 16;
            }
            if (!fits) break;
            uint8_t* info = trd + 8 * 256; // дорожка 0, сектор 9
            info[0xe1]    = static_cast<uint8_t>(sector);
            info[0xe2]    = static_cast<uint8_t>(track);
            info[0xe3]    = 0x16; // 80 цилиндров, две стороны
            info[0xe4]    = static_cast<uint8_t>(files);
            put_le16(info + 0xe5, static_cast<unsigned>(kTrdFreeSecs - used));
            info[0xe7] = 0x10; // признак TR-DOS
            std::memset(info + 0xea, ' ', 9);
            std::memset(info + 0xf5, ' ', 8);
            ok = open_trd(trd, kTrdSize, arena);
            break;
        }
        case ImageFormat::Fdi:
            ok = open_fdi(file, size, arena);
            break;
        case ImageFormat::Udi:
            ok = open_udi(file, size);
            break;
        case ImageFormat::Td0:
            ok = open_td0(file, size, arena);
            break;
        case ImageFormat::Hfe:
            ok = open_hfe(file, size);
            break;
        default:
            break;
    }
    if (!ok) {
        close();
        return false;
    }
    fmt_ = fmt;
    return true;
}

// TRD: дорожка t - цилиндр t / 2, сторона t % 2, 16 секторов по 256 байт.
// Короткий образ дополняется чистыми дорожками.
bool DiskImage::open_trd(uint8_t* file, size_t size, Arena& arena) {
    if (size == 0 || size % 256 != 0 || size > kTrdSize) return false;
    uint8_t* buf = file;
    if (size < kTrdSize) {
        buf = static_cast<uint8_t*>(arena.take(kTrdSize));
        if (buf == nullptr) return false;
        std::memcpy(buf, file, size);
        std::memset(buf + size, 0, kTrdSize - size);
    }
    file_      = buf;
    file_size_ = kTrdSize;
    auto* secs = static_cast<ImgSector*>(arena.take(sizeof(ImgSector) * 16 * 160));
    if (secs == nullptr) return false;
    for (int t = 0; t < 160; t++) {
        ImgTrack& tr = tracks_[t / 2][t % 2];
        tr.kind      = TrackKind::Sectors;
        tr.count     = 16;
        tr.sectors   = secs + 16 * t;
        for (int i = 0; i < 16; i++) {
            const uint8_t r = kTrdosOrder[i];
            tr.sectors[i]   = ImgSector{
                static_cast<uint8_t>(t / 2), static_cast<uint8_t>(t % 2), r, 1, 0, buf + static_cast<size_t>(t) * 4096 + static_cast<size_t>(r - 1) * 256};
        }
    }
    ncyl_   = 80;
    nsides_ = 2;
    return true;
}

// FDI: заголовок 14 байт и доп. часть, затем заголовки дорожек (смещение
// данных 4, резерв 2, число секторов 1) и секторов (C H R N, флаги,
// смещение 2). Флаги: разряд N & 3 - CRC верен, 0x40 - нет данных, 0x80 -
// метка F8.
bool DiskImage::open_fdi(uint8_t* file, size_t size, Arena& arena) {
    const int cyls = le16(file + 4), heads = le16(file + 6);
    if (cyls < 1 || cyls > kMaxCylinders || heads < 1 || heads > 2) return false;
    const size_t data = le16(file + 10);
    size_t p          = 14 + static_cast<size_t>(le16(file + 12));
    wprot_            = file[3] != 0;
    for (int c = 0; c < cyls; c++) {
        for (int h = 0; h < heads; h++) {
            if (p + 7 > size) return false;
            const size_t tdata  = data + le32(file + p);
            const int ns        = file[p + 6];
            p                  += 7;
            if (ns > kMaxSecPerTrk || p + 7 * static_cast<size_t>(ns) > size) return false;
            ImgTrack& tr = tracks_[c][h];
            tr.kind      = TrackKind::Sectors;
            tr.count     = static_cast<uint8_t>(ns);
            tr.sectors   = static_cast<ImgSector*>(arena.take(sizeof(ImgSector) * static_cast<size_t>(ns ? ns : 1)));
            if (tr.sectors == nullptr) return false;
            for (int s = 0; s < ns; s++) {
                const uint8_t* e = file + p;
                ImgSector& sec   = tr.sectors[s];
                sec              = ImgSector{e[0], e[1], e[2], e[3], 0, nullptr};
                const uint8_t f  = e[4];
                if ((f & 0x40) != 0) {
                    sec.flags = kSecNoData;
                } else {
                    const size_t off = tdata + le16(e + 5);
                    if (off + (static_cast<size_t>(128) << (e[3] & 3)) > size) return false;
                    sec.data = file + off;
                    if ((f & (1u << (e[3] & 3))) == 0) sec.flags |= kSecBadDataCrc;
                    if ((f & 0x80) != 0) sec.flags |= kSecDeleted;
                }
                p += 7;
            }
        }
    }
    ncyl_   = cyls;
    nsides_ = heads;
    return true;
}

// UDI: "UDI!", размер, версия, цилиндров - 1, сторон - 1, флаги, длина
// доп. заголовка; дорожки: тип (0 - MFM), длина, байты, карта меток (бит на
// байт, младший первым).
bool DiskImage::open_udi(uint8_t* file, size_t size) {
    const int cyls = file[9] + 1, heads = file[10] + 1;
    if (cyls > kMaxCylinders || heads > 2) return false;
    size_t p = kUdiHeader + le32(file + 12);
    for (int c = 0; c < cyls; c++) {
        for (int h = 0; h < heads; h++) {
            if (p + 3 > size || file[p] != 0) return false;
            const size_t len = le16(file + p + 1);
            if (p + 3 + len + (len + 7) / 8 > size) return false;
            ImgTrack& tr  = tracks_[c][h];
            tr.kind       = TrackKind::RawBytes;
            tr.raw_len    = static_cast<uint16_t>(len);
            tr.raw        = file + p + 3;
            p            += 3 + len + (len + 7) / 8;
        }
    }
    ncyl_   = cyls;
    nsides_ = heads;
    return true;
}

namespace {

// Распаковка LZHUF (TD0 "td"): словарь 4 КБ, адаптивный Хаффман по
// Йошизаки.
class Lzh {
public:
    Lzh(const uint8_t* src, size_t n) : p_(src), end_(src + n) {}
    size_t unpack(uint8_t* out, size_t cap);

private:
    static constexpr int kN = 4096, kF = 60, kThreshold = 2;
    static constexpr int kNChar = 256 - kThreshold + kF, kT = kNChar * 2 - 1, kR = kT - 1;
    static constexpr unsigned kMaxFreq = 0x8000;

    int read_char() { return p_ < end_ ? *p_++ : -1; }
    int get_bit();
    int get_byte();
    void start();
    void reconst();
    void update(int c);
    int decode_char();
    int decode_position();

    const uint8_t* p_;
    const uint8_t* end_;
    uint8_t text_[kN + kF - 1] = {};
    uint16_t freq_[kT + 1]     = {};
    int16_t prnt_[kT + kNChar] = {};
    int16_t son_[kT]           = {};
    int r_                     = 0;
    unsigned getbuf_           = 0;
    unsigned getlen_           = 0;
};

// Старшие 6 разрядов позиции и длина её кода по первому байту.
uint8_t lzh_code(int i) {
    if (i < 32) return 0;
    if (i < 80) return static_cast<uint8_t>(1 + (i - 32) / 16);
    if (i < 144) return static_cast<uint8_t>(4 + (i - 80) / 8);
    if (i < 192) return static_cast<uint8_t>(12 + (i - 144) / 4);
    if (i < 240) return static_cast<uint8_t>(24 + (i - 192) / 2);
    return static_cast<uint8_t>(48 + (i - 240));
}
uint8_t lzh_len(int i) {
    if (i < 32) return 3;
    if (i < 80) return 4;
    if (i < 144) return 5;
    if (i < 192) return 6;
    if (i < 240) return 7;
    return 8;
}

int Lzh::get_bit() {
    while (getlen_ <= 8) {
        int i = read_char();
        if (i < 0) i = 0;
        getbuf_ |= static_cast<unsigned>(i) << (8 - getlen_);
        getlen_ += 8;
    }
    const unsigned i = getbuf_;
    getbuf_          = (getbuf_ << 1) & 0xffff;
    getlen_--;
    return static_cast<int>((i >> 15) & 1);
}

int Lzh::get_byte() {
    while (getlen_ <= 8) {
        int i = read_char();
        if (i < 0) i = 0;
        getbuf_ |= static_cast<unsigned>(i) << (8 - getlen_);
        getlen_ += 8;
    }
    const unsigned i  = getbuf_;
    getbuf_           = (getbuf_ << 8) & 0xffff;
    getlen_          -= 8;
    return static_cast<int>((i >> 8) & 0xff);
}

void Lzh::start() {
    getbuf_ = 0;
    getlen_ = 0;
    for (int i = 0; i < kNChar; i++) {
        freq_[i]      = 1;
        son_[i]       = static_cast<int16_t>(i + kT);
        prnt_[i + kT] = static_cast<int16_t>(i);
    }
    int i = 0, j = kNChar;
    while (j <= kR) {
        freq_[j] = static_cast<uint16_t>(freq_[i] + freq_[i + 1]);
        son_[j]  = static_cast<int16_t>(i);
        prnt_[i] = prnt_[i + 1]  = static_cast<int16_t>(j);
        i                       += 2;
        j++;
    }
    freq_[kT] = 0xffff;
    prnt_[kR] = 0;
    for (i = 0; i < kN - kF; i++) {
        text_[i] = ' ';
    }
    r_ = kN - kF;
}

void Lzh::reconst() {
    int j = 0;
    for (int i = 0; i < kT; i++) {
        if (son_[i] >= kT) {
            freq_[j] = static_cast<uint16_t>((freq_[i] + 1) / 2);
            son_[j]  = son_[i];
            j++;
        }
    }
    for (int i = 0, k = kNChar; k < kT; i += 2, k++) {
        const unsigned f = static_cast<unsigned>(freq_[i]) + freq_[i + 1];
        freq_[k]         = static_cast<uint16_t>(f);
        int l            = k - 1;
        while (f < freq_[l]) {
            l--;
        }
        l++;
        std::memmove(&freq_[l + 1], &freq_[l], static_cast<size_t>(k - l) * sizeof(freq_[0]));
        freq_[l] = static_cast<uint16_t>(f);
        std::memmove(&son_[l + 1], &son_[l], static_cast<size_t>(k - l) * sizeof(son_[0]));
        son_[l] = static_cast<int16_t>(i);
    }
    for (int i = 0; i < kT; i++) {
        const int k = son_[i];
        if (k >= kT) {
            prnt_[k] = static_cast<int16_t>(i);
        } else {
            prnt_[k] = prnt_[k + 1] = static_cast<int16_t>(i);
        }
    }
}

void Lzh::update(int c) {
    if (freq_[kR] == kMaxFreq) reconst();
    c = prnt_[c + kT];
    do {
        const unsigned k = ++freq_[c];
        int l            = c + 1;
        if (k > freq_[l]) {
            while (k > freq_[++l]) {
            }
            l--;
            freq_[c]    = freq_[l];
            freq_[l]    = static_cast<uint16_t>(k);
            const int i = son_[c];
            prnt_[i]    = static_cast<int16_t>(l);
            if (i < kT) prnt_[i + 1] = static_cast<int16_t>(l);
            const int j = son_[l];
            son_[l]     = static_cast<int16_t>(i);
            prnt_[j]    = static_cast<int16_t>(c);
            if (j < kT) prnt_[j + 1] = static_cast<int16_t>(c);
            son_[c] = static_cast<int16_t>(j);
            c       = l;
        }
        c = prnt_[c];
    } while (c != 0);
}

int Lzh::decode_char() {
    int c = son_[kR];
    while (c < kT) {
        c = son_[c + get_bit()];
    }
    c -= kT;
    update(c);
    return c;
}

int Lzh::decode_position() {
    int i       = get_byte();
    const int c = lzh_code(i) << 6;
    int j       = lzh_len(i) - 2;
    while (j-- > 0) {
        i = (i << 1) + get_bit();
    }
    return c | (i & 0x3f);
}

size_t Lzh::unpack(uint8_t* out, size_t cap) {
    size_t n = 0;
    start();
    while (p_ < end_) {
        const int c = decode_char();
        if (c < 256) {
            if (n >= cap) return 0;
            out[n++]     = static_cast<uint8_t>(c);
            text_[r_++]  = static_cast<uint8_t>(c);
            r_          &= kN - 1;
        } else {
            const int i = (r_ - decode_position() - 1) & (kN - 1);
            const int j = c - 255 + kThreshold;
            for (int k = 0; k < j; k++) {
                const uint8_t b = text_[(i + k) & (kN - 1)];
                if (n >= cap) return 0;
                out[n++]     = b;
                text_[r_++]  = b;
                r_          &= kN - 1;
            }
        }
    }
    return n;
}

} // namespace

// TD0 (Teledisk): заголовок 12 байт ("td" - дальше сжато LZHUF),
// комментарий при флаге 0x80, дорожки (секторов, цилиндр, головка, CRC;
// 0xFF - конец), секторы (C H R N, флаги, CRC; блок данных, если нет флагов
// 0x30 и N <= 6: длина, способ 0 - как есть, 1 - пара байтов N раз, 2 -
// блоки: 0 - литералы, k - узор 2^k байт N раз). Флаги 0x02 - ошибка CRC,
// 0x04 - метка F8.
bool DiskImage::open_td0(const uint8_t* file, size_t size, Arena& arena) {
    const uint8_t* src = file;
    size_t n           = size;
    if (file[0] == 't') {
        // Сжатое тело распаковывается в остаток arena; распаковщик (8.5 КБ)
        // лежит там же, перед ним.
        void* mem = arena.take(sizeof(Lzh));
        if (mem == nullptr) return false;
        Lzh* unpacker    = new (mem) Lzh(file + 12, size - 12);
        auto* out        = arena.base + ((arena.used + 3) & ~static_cast<size_t>(3));
        const size_t cap = arena.size - static_cast<size_t>(out - arena.base);
        if (cap < 12) return false;
        std::memcpy(out, file, 12);
        const size_t body = unpacker->unpack(out + 12, cap - 12);
        if (body == 0) return false;
        n   = 12 + body;
        src = static_cast<const uint8_t*>(arena.take(n));
    }
    size_t p = 12;
    if ((src[7] & 0x80) != 0) {
        if (p + 10 > n) return false;
        p += 10 + le16(src + 14);
    }
    int max_cyl = -1, max_head = 0;
    while (p < n) {
        const int ns = src[p];
        if (ns == 0xff) break;
        if (p + 4 > n) return false;
        const int cyl = src[p + 1], head = src[p + 2] & 1;
        p += 4;
        if (cyl >= kMaxCylinders || ns > kMaxSecPerTrk) return false;
        ImgTrack& tr = tracks_[cyl][head];
        tr.kind      = TrackKind::Sectors;
        tr.count     = 0;
        tr.sectors   = static_cast<ImgSector*>(arena.take(sizeof(ImgSector) * static_cast<size_t>(ns ? ns : 1)));
        if (tr.sectors == nullptr) return false;
        for (int s = 0; s < ns; s++) {
            if (p + 6 > n) return false;
            const uint8_t* h     = src + p;
            const uint8_t flags  = h[4];
            p                   += 6;
            ImgSector& sec       = tr.sectors[tr.count];
            sec                  = ImgSector{h[0], h[1], h[2], h[3], 0, nullptr};
            if ((flags & 0x02) != 0) sec.flags |= kSecBadDataCrc;
            if ((flags & 0x04) != 0) sec.flags |= kSecDeleted;
            if ((flags & 0x30) != 0 || h[3] > 6) {
                sec.flags |= kSecNoData;
                if ((flags & 0x40) == 0) tr.count++;
                continue;
            }
            if (p + 2 > n) return false;
            const size_t blen  = le16(src + p);
            p                 += 2;
            if (blen < 1 || p + blen > n) return false;
            const size_t sz = static_cast<size_t>(128) << (h[3] & 3);
            auto* d         = static_cast<uint8_t*>(arena.take(sz));
            if (d == nullptr) return false;
            std::memset(d, 0, sz);
            const uint8_t* b = src + p;
            const uint8_t* e = b + blen;
            const int method = *b++;
            size_t o         = 0;
            if (method == 0) {
                const size_t k = static_cast<size_t>(e - b) < sz ? static_cast<size_t>(e - b) : sz;
                std::memcpy(d, b, k);
            } else if (method == 1) {
                if (e - b < 4) return false;
                const unsigned cnt = le16(b);
                for (unsigned i = 0; i < cnt && o + 2 <= sz; i++, o += 2) {
                    d[o]     = b[2];
                    d[o + 1] = b[3];
                }
            } else if (method == 2) {
                while (b < e) {
                    const int type = *b++;
                    if (b >= e) return false;
                    const unsigned cnt = *b++;
                    if (type == 0) {
                        for (unsigned i = 0; i < cnt && b < e; i++) {
                            if (o < sz) d[o++] = *b;
                            b++;
                        }
                    } else {
                        const size_t len = static_cast<size_t>(1) << type;
                        if (static_cast<size_t>(e - b) < len) return false;
                        for (unsigned i = 0; i < cnt; i++) {
                            for (size_t k = 0; k < len; k++) {
                                if (o < sz) d[o++] = b[k];
                            }
                        }
                        b += len;
                    }
                }
            } else {
                return false;
            }
            sec.data  = d;
            p        += blen;
            if ((flags & 0x40) == 0) tr.count++;
        }
        if (cyl > max_cyl) max_cyl = cyl;
        if (head > max_head) max_head = head;
    }
    if (max_cyl < 0) return false;
    ncyl_   = max_cyl + 1;
    nsides_ = max_head + 1;
    file_   = nullptr; // в свой формат не сохраняется
    return true;
}

// HFE v1: поток ячеек от индекса, младший разряд первым, стороны
// чередуются блоками по 256 байт.
bool DiskImage::open_hfe(uint8_t* file, size_t size) {
    if (file[8] != 0) return false;
    const int cyls = file[9], heads = file[10];
    const size_t list = static_cast<size_t>(le16(file + 18)) * 512;
    if (cyls < 1 || cyls > kMaxCylinders || heads < 1 || heads > 2 || list + 4 * static_cast<size_t>(cyls) > size) {
        return false;
    }
    wprot_ = file[20] == 0;
    for (int c = 0; c < cyls; c++) {
        const size_t off = static_cast<size_t>(le16(file + list + 4 * c)) * 512;
        const size_t len = le16(file + list + 4 * c + 2);
        if (off + len > size || len / 2 == 0) return false;
        for (int h = 0; h < heads; h++) {
            ImgTrack& tr = tracks_[c][h];
            tr.kind      = TrackKind::Cells;
            tr.raw       = file + off + static_cast<size_t>(h) * 256;
            tr.raw_len   = static_cast<uint16_t>(len / 2);
        }
    }
    ncyl_   = cyls;
    nsides_ = heads;
    return true;
}

namespace {

// Ячейки с масштабом: total ячеек источника ложатся на kSlotsPerRev слотов
// ближайшим соседом.
struct CellOut {
    uint8_t* bits;
    uint32_t total;
    uint32_t i = 0;
    void put(int v) {
        if (v != 0) {
            const uint32_t s = total == kSlotsPerRev ? i : static_cast<uint32_t>((static_cast<uint64_t>(i) * kSlotsPerRev) / total);
            if (s < kSlotsPerRev) bits[s >> 3] = static_cast<uint8_t>(bits[s >> 3] | (1u << (s & 7)));
        }
        i++;
    }
};

// Промежуток GAP3, при котором дорожка помещается в оборот MFM.
int fit_gap3(const ImgSector* s, int count) {
    if (count == 0) return 54;
    int used = 146; // GAP4a, синхро, IAM, GAP1
    for (int i = 0; i < count; i++) {
        used += 12 + 4 + 4 + 2 + 22;
        if ((s[i].flags & kSecNoData) == 0) used += 12 + 4 + (128 << (s[i].n & 3)) + 2;
    }
    const int room = (static_cast<int>(kSlotsPerRev / 16) - used) / count;
    if (room >= 54) return 54;
    return room > 1 ? room : 1;
}

} // namespace

void DiskImage::track_cells(int cyl, int side, uint8_t* bits) const {
    std::memset(bits, 0, kTrackBytes);
    if (cyl < 0 || cyl >= kMaxCylinders || side < 0 || side > 1) return;
    const ImgTrack& tr = tracks_[cyl][side];
    switch (tr.kind) {
        case TrackKind::Sectors: {
            SectorSpec spec[kMaxSecPerTrk];
            for (int i = 0; i < tr.count; i++) {
                const ImgSector& s = tr.sectors[i];
                uint8_t bad        = 0;
                if ((s.flags & kSecBadIdCrc) != 0) bad |= kBadIdCrc;
                if ((s.flags & kSecBadDataCrc) != 0) bad |= kBadDataCrc;
                spec[i] = SectorSpec{s.c, s.h, s.r, s.n, (s.flags & kSecDeleted) != 0, (s.flags & kSecNoData) != 0 ? nullptr : s.data, bad};
            }
            build_mfm_track(bits, spec, tr.count, fit_gap3(tr.sectors, tr.count));
            break;
        }
        case TrackKind::RawBytes: {
            // Байт MFM - 16 ячеек; у меток A1 нет синхроимпульса бита 5, у C2 - бита 4.
            const uint8_t* bytes = tr.raw;
            const uint8_t* marks = tr.raw + tr.raw_len;
            CellOut out{bits, static_cast<uint32_t>(tr.raw_len) * 16};
            int prev = 0;
            for (uint32_t i = 0; i < tr.raw_len; i++) {
                const uint8_t b   = bytes[i];
                const bool marked = ((marks[i >> 3] >> (i & 7)) & 1) != 0;
                const int missing = !marked ? -1 : (b == 0xa1 ? 5 : (b == 0xc2 ? 4 : -1));
                for (int k = 0; k < 8; k++) {
                    const int bit = (b >> (7 - k)) & 1;
                    out.put((prev | bit) == 0 && k != missing ? 1 : 0);
                    out.put(bit);
                    prev = bit;
                }
            }
            break;
        }
        case TrackKind::Cells:
            if (fmt_ == ImageFormat::Hfe && !tr.dirty) {
                CellOut out{bits, static_cast<uint32_t>(tr.raw_len) * 8};
                for (uint32_t i = 0; i < tr.raw_len; i++) {
                    const uint8_t b = tr.raw[(i / 256) * 512 + i % 256];
                    for (int k = 0; k < 8; k++) {
                        out.put((b >> k) & 1);
                    }
                }
            } else {
                std::memcpy(bits, tr.raw, kTrackBytes);
            }
            break;
        default:
            break;
    }
}

bool DiskImage::store_cells(int cyl, int side, const uint8_t* bits, Arena& arena) {
    if (cyl < 0 || cyl >= kMaxCylinders || side < 0 || side > 1) return false;
    ImgTrack& tr = tracks_[cyl][side];
    // HFE своей длины: прямо в файл.
    if (tr.kind == TrackKind::Cells && fmt_ == ImageFormat::Hfe && !tr.dirty && tr.raw_len == kHfeSide) {
        for (uint32_t i = 0; i < kHfeSide; i++) {
            tr.raw[(i / 256) * 512 + i % 256] = bits[i];
        }
        hfe_written_ = true;
        return true;
    }
    if (tr.kind == TrackKind::Sectors) {
        // Те же заголовки с верными CRC и данные - на место секторов образа.
        // Разбор во временной части arena, после - откат.
        const size_t mark = arena.used;
        auto* found       = static_cast<FoundSector*>(arena.take(sizeof(FoundSector) * kMaxSecPerTrk));
        auto* data        = static_cast<uint8_t*>(arena.take(kTrackBytes));
        if (found == nullptr || data == nullptr) {
            arena.used = mark;
            return false;
        }
        const int nf = decode_mfm_track(bits, kSlotsPerRev, found, kMaxSecPerTrk, data, kTrackBytes);
        bool same    = nf == tr.count;
        for (int i = 0; i < tr.count && same; i++) {
            const ImgSector& s = tr.sectors[i];
            bool hit           = false;
            for (int k = 0; k < nf; k++) {
                const FoundSector& f = found[k];
                if (f.c == s.c && f.h == s.h && f.r == s.r && f.n == s.n && f.id_crc_ok && f.has_data == ((s.flags & kSecNoData) == 0) &&
                    (s.flags & kSecBadIdCrc) == 0) {
                    hit = true;
                }
            }
            same = hit;
        }
        if (same) {
            for (int i = 0; i < tr.count; i++) {
                ImgSector& s = tr.sectors[i];
                if ((s.flags & kSecNoData) != 0) continue;
                for (int k = 0; k < nf; k++) {
                    const FoundSector& f = found[k];
                    if (f.c == s.c && f.h == s.h && f.r == s.r && f.n == s.n) {
                        std::memcpy(s.data, data + f.data_off, static_cast<size_t>(128) << (s.n & 3));
                        s.flags = static_cast<uint8_t>((s.flags & ~(kSecDeleted | kSecBadDataCrc)) | (f.deleted ? kSecDeleted : 0) |
                                                       (f.data_crc_ok ? 0 : kSecBadDataCrc));
                        break;
                    }
                }
            }
            tr.dirty   = true;
            arena.used = mark;
            return true;
        }
        arena.used = mark;
    }
    // Иначе дорожка хранится потоком ячеек.
    if (tr.kind != TrackKind::Cells || (fmt_ == ImageFormat::Hfe && !tr.dirty)) {
        auto* buf = static_cast<uint8_t*>(arena.take(kTrackBytes));
        if (buf == nullptr) return false;
        tr.kind    = TrackKind::Cells;
        tr.raw     = buf;
        tr.raw_len = static_cast<uint16_t>(kTrackBytes);
    }
    std::memcpy(tr.raw, bits, kTrackBytes);
    tr.dirty    = true;
    off_layout_ = true;
    if (cyl + 1 > ncyl_) ncyl_ = cyl + 1;
    if (side + 1 > nsides_) nsides_ = side + 1;
    return true;
}

bool DiskImage::dirty() const {
    if (hfe_written_) return true;
    for (const auto& c : tracks_) {
        for (const auto& t : c) {
            if (t.dirty) return true;
        }
    }
    return false;
}

bool DiskImage::can_save() const {
    return !off_layout_ && file_ != nullptr && (fmt_ == ImageFormat::Trd || fmt_ == ImageFormat::Fdi || fmt_ == ImageFormat::Hfe);
}

} // namespace devices::bdi
