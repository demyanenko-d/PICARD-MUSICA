#pragma once

// Сжатие потока Dpcm8 в банке без потерь: табличный ANS (tANS).
//
// Модель. Байт Dpcm8 - масштаб в старших трёх битах и знаковая величина в
// младших пяти. У волны соседние отсчёты меняются плавно, масштаб почти не
// скачет: следующий байт предсказывается старшими битами предыдущего.
// Контекст - два старших бита предыдущего байта.
//
// Распаковка байта - одна выборка из таблицы по состоянию: символ, сколько
// бит дочитать и база следующего состояния. На плате из флеша 9.9 МБ/с.
// Таблица распаковки строится из частот при загрузке трека в резидентную
// арену: в банке только частоты (2 КБ).
//
// Кодировщик (sf2bake) и декодер (плата) здесь вместе: разойдутся - банк
// молча превратится в шум.

#include <cstdint>
#include <cstring>

#include "platform/compiler.h"

#include "core/codec/dpcm8.h"

namespace soundsinth::bank {

// Два бита контекста - старшие биты масштаба.
inline constexpr uint32_t kModelContextBits = 2;
inline constexpr uint32_t kModelContexts = 1u << kModelContextBits;
static_assert(kModelContextBits <= 8 - dpcm8::kCodeScaleLsb, "контекст - только биты масштаба");

inline uint32_t model_context(uint8_t prev) { return static_cast<uint32_t>(prev) >> (8u - kModelContextBits); }

// Символ модели - байт-код Dpcm8.
inline constexpr uint32_t kModelSymbols = 256;

// Состояний на контекст; сумма частот контекста равна их числу.
inline constexpr uint32_t kModelStateBits = 9;
inline constexpr uint32_t kModelStates = 1u << kModelStateBits;
static_assert(kModelStates >= kModelSymbols, "каждому символу хватает состояния");

// Частоты: freq[ctx][s], сумма по s - kModelStates. Символ с нулевой
// частотой в контексте не встречается. Лежит в блобе банка как есть.
struct BankModel {
    uint16_t freq[kModelContexts][kModelSymbols];
};
static_assert(sizeof(BankModel) == kModelContexts * kModelSymbols * 2, "BankModel: раскладка идёт в файл");

inline bool model_valid(const BankModel& m) {
    for (uint32_t c = 0; c < kModelContexts; ++c) {
        uint32_t sum = 0;
        for (uint32_t s = 0; s < kModelSymbols; ++s) sum += m.freq[c][s];
        if (sum != kModelStates) return false;
    }
    return true;
}

// Раскладка состояний по символам: шаг, взаимно простой с kModelStates,
// разносит состояния одного символа по всей таблице. visit(ctx, s, v, i):
// состояние i декодируется в символ s, и после него значение v из
// [freq, 2*freq). Одна функция у кодировщика и декодера.
inline constexpr uint32_t kModelSpreadStep = (kModelStates >> 1) + (kModelStates >> 3) + 3;
static_assert((kModelSpreadStep & 1u) == 1u, "шаг нечётный: обходит все состояния");

template <typename Visit>
inline void model_spread(const BankModel& m, uint32_t ctx, Visit&& visit) {
    uint32_t pos = 0;
    for (uint32_t s = 0; s < kModelSymbols; ++s) {
        const uint32_t f = m.freq[ctx][s];
        for (uint32_t k = 0; k < f; ++k) {
            visit(s, f + k, pos);
            pos = (pos + kModelSpreadStep) & (kModelStates - 1u);
        }
    }
}

// Таблица распаковки: строки контекстов подряд, индекс - контекст *
// kModelStates + состояние. Запись: символ в битах 0..7, сколько бит
// дочитать - 8..11, база следующего индекса - 16..31. Контекст следующего
// байта - от этого символа, он уже вписан в базу: декодеру предыдущий байт
// не нужен.
struct BankDecodeTable {
    uint32_t entry[kModelContexts * kModelStates];
};
static_assert(kModelContexts * kModelStates <= 0x10000u, "база индекса - 16 бит");

inline void bank_build_decode_table(const BankModel& m, BankDecodeTable& t) {
    for (uint32_t c = 0; c < kModelContexts; ++c) {
        model_spread(m, c, [&](uint32_t s, uint32_t v, uint32_t i) {
            uint32_t nb = 0;
            while ((v << nb) < kModelStates) ++nb;
            const uint32_t base = model_context(static_cast<uint8_t>(s)) * kModelStates + ((v << nb) - kModelStates);
            t.entry[c * kModelStates + i] = s | (nb << 8) | (base << 16);
        });
    }
}

// Таблица кодирования - только у sf2bake.
struct BankEncodeTable {
    uint16_t freq[kModelContexts][kModelSymbols];
    uint16_t cum[kModelContexts][kModelSymbols];
    // По cum[s] + (v - freq[s]): состояние kModelStates + i.
    uint16_t next[kModelContexts][kModelStates];
};

inline void bank_build_encode_table(const BankModel& m, BankEncodeTable& t) {
    for (uint32_t c = 0; c < kModelContexts; ++c) {
        uint32_t cum = 0;
        for (uint32_t s = 0; s < kModelSymbols; ++s) {
            t.freq[c][s] = m.freq[c][s];
            t.cum[c][s] = static_cast<uint16_t>(cum);
            cum += m.freq[c][s];
        }
        model_spread(m, c, [&](uint32_t s, uint32_t v, uint32_t i) {
            t.next[c][t.cum[c][s] + (v - t.freq[c][s])] = static_cast<uint16_t>(kModelStates + i);
        });
    }
}

// Сжатый прогон: два байта начального состояния декодера, дальше биты
// младшими вперёд. Кодирование идёт с конца, биты выписываются в порядке
// чтения. scratch - src_bytes слов. Возвращает длину; 0 - символа нет в
// модели.
inline uint32_t bank_compress(const BankEncodeTable& t, const uint8_t* src, uint32_t src_bytes, uint8_t* dst,
                              uint32_t* scratch) {
    uint32_t x = kModelStates;
    for (uint32_t j = src_bytes; j-- > 0;) {
        const uint32_t c = model_context(j ? src[j - 1] : 0);
        const uint32_t s = src[j];
        const uint32_t f = t.freq[c][s];
        if (f == 0) return 0;
        uint32_t nb = 0;
        while ((x >> nb) >= 2u * f) ++nb;
        scratch[j] = (x & ((1u << nb) - 1u)) | (nb << 16);
        x = t.next[c][t.cum[c][s] + ((x >> nb) - f)];
    }
    const uint32_t start = x - kModelStates;
    dst[0] = static_cast<uint8_t>(start);
    dst[1] = static_cast<uint8_t>(start >> 8);
    uint32_t pos = 2, acc = 0, cnt = 0;
    for (uint32_t j = 0; j < src_bytes; ++j) {
        acc |= (scratch[j] & 0xffffu) << cnt;
        cnt += scratch[j] >> 16;
        while (cnt >= 8) {
            dst[pos++] = static_cast<uint8_t>(acc);
            acc >>= 8;
            cnt -= 8;
        }
    }
    if (cnt) dst[pos++] = static_cast<uint8_t>(acc);
    return pos;
}

// Декодер читает биты словом и с запасом: до семи байт за концом прогона.
// За концом зоны PCM в банке и за концом данных во входном буфере лежат
// нули.
inline constexpr uint32_t kPcmTailBytes = 8;

// Источник PCM банка с карты: байты читаются кусками по ходу распаковки
// (сжатый прогон бывает до 699 КБ). offset - от начала зоны PCM, чтение не
// дальше её конца; 0 при ненулевом запросе - отказ чтения.
struct BankPcmSource {
    uint32_t (*read)(void* user, uint32_t offset, uint8_t* dst, uint32_t bytes) = nullptr;
    void* user = nullptr;
};

// Входной буфер распаковщика - сжатые байты прогона кусками, и с карты, и
// из флеша: у декодера один быстрый путь. Плата, из флеша: через буфер
// 9.9 МБ/с, по месту 9.6; сам счёт 17.8, memcpy из флеша 41.
inline constexpr uint32_t kBankInputBytes = 1024;
// За данными в буфере - нули. У целого потока декодер учитывает байты
// самое большее до конца данных + 3 и читает словом до + 6; предел
// дочитанного прогона - конец + 9, слово с него - до + 12.
inline constexpr uint32_t kBankInputPadBytes = 16;
inline constexpr uint32_t kBankDrainedSlack = 9;
static_assert(kBankDrainedSlack + 3u < kBankInputPadBytes, "слово за пределом - в нулях");
inline constexpr uint32_t kBankInputBufferBytes = kBankInputBytes + kBankInputPadBytes;

// Байты прогона: из памяти или из источника, в буфер вызывающего на
// kBankInputBufferBytes.
class BankByteInput {
public:
    // Прогон в памяти, limit байт.
    BankByteInput(const uint8_t* mem, uint32_t limit, uint8_t* buf) : mem_(mem), left_(limit), buf_(buf) { pad(); }

    // Прогон из источника: limit байт начиная с offset.
    BankByteInput(const BankPcmSource* src, uint32_t offset, uint32_t limit, uint8_t* buf)
        : src_(src), offset_(offset), left_(limit), buf_(buf) {
        pad();
    }

    // Источник отказал: вернул 0 при ненулевом запросе. Признак липкий -
    // после него источник больше не зовётся, поток дальше не продолжится
    // поверх съеденных нулей.
    bool failed() const { return failed_; }

    // Готовые байты - [cursor, end), за end - kBankInputPadBytes нулей.
    // drained - прогон прочитан целиком, за end ничего не придёт.
    const uint8_t* cursor() const { return buf_ + pos_; }
    const uint8_t* end() const { return buf_ + filled_; }
    bool drained() const { return left_ == 0; }
    void set_cursor(const uint8_t* p) { pos_ = static_cast<uint32_t>(p - buf_); }

    // Остаток сдвигается в начало, буфер добирается из прогона.
    SOUNDSINTH_NOINLINE void refill() {
        const uint32_t keep = filled_ - pos_;
        std::memmove(buf_, buf_ + pos_, keep);
        pos_ = 0;
        filled_ = keep;
        const uint32_t room = kBankInputBytes - keep;
        const uint32_t want = left_ < room ? left_ : room;
        uint32_t got = 0;
        if (want && mem_) {
            std::memcpy(buf_ + keep, mem_ + offset_, want);
            got = want;
        } else if (want && !failed_) {
            got = src_->read(src_->user, offset_, buf_ + keep, want);
        }
        if (want && got == 0) failed_ = true;
        offset_ += got;
        left_ -= got;
        filled_ += got;
        pad();
    }

    // За концом прогона - нули.
    uint8_t next() {
        if (pos_ >= filled_ && !drained() && !failed_) refill();
        return pos_ < filled_ ? buf_[pos_++] : 0;
    }

    // Несжатый прогон: мимо буфера, прямо в dst.
    void copy(uint8_t* dst, uint32_t n) {
        const uint32_t want = n < left_ ? n : left_;
        uint32_t got = 0;
        if (mem_) {
            std::memcpy(dst, mem_ + offset_, want);
            got = want;
        } else {
            while (got < want && !failed_) {
                const uint32_t r = src_->read(src_->user, offset_ + got, dst + got, want - got);
                if (r == 0) failed_ = true;
                got += r;
            }
        }
        offset_ += got;
        left_ -= got;
        if (got < n) std::memset(dst + got, 0, n - got);
    }

private:
    void pad() { std::memset(buf_ + filled_, 0, kBankInputPadBytes); }

    const uint8_t* mem_ = nullptr;
    const BankPcmSource* src_ = nullptr;
    uint32_t offset_ = 0;
    uint32_t left_ = 0;
    uint8_t* buf_;
    uint32_t pos_ = 0;
    uint32_t filled_ = 0;
    bool failed_ = false;
};

// Состояние декодера между кусками.
struct TansState {
    uint32_t x = 0;    // индекс в BankDecodeTable::entry
    uint32_t bits = 0; // непрочитанные биты, младшие - следующие
    uint32_t cnt = 0;  // сколько их
};

// Распаковка до n байт с входом по указателю p. Байты, учтённые в
// битовом буфере, не заходят за limit; слово читается до limit + 3.
// Возвращает, сколько распаковано; p сдвигается. На плате - в SRAM.
uint32_t bank_decode_block(const BankDecodeTable& t, const uint8_t*& p, const uint8_t* limit, uint8_t* dst,
                           uint32_t n, TansState& st);

// --- Распаковка прогона ---
//
// Прогоны сжимаются независимо: подкачка идёт по одному сэмплу.
//
// Распаковщик - объект, а не функция: страницы PSRAM лежат не подряд,
// цепочка отдаётся по килобайту, поток продолжается с того же места.
// Держать в буфере весь распакованный сэмпл (бывает в сотни килобайт)
// незачем.
//
// table == nullptr - прогон не сжат, байты копируются.
class BankUnpacker {
public:
    // Прогон в памяти, packed байт; buf - на kBankInputBufferBytes.
    BankUnpacker(const BankDecodeTable* table, const uint8_t* src, uint32_t packed, uint8_t* buf)
        : table_(table), in_(src, packed, buf) {
        start();
    }

    // Тот же распаковщик, вход из источника (банк на карте).
    BankUnpacker(const BankDecodeTable* table, const BankPcmSource* src, uint32_t offset, uint32_t packed,
                 uint8_t* buf)
        : table_(table), in_(src, offset, packed, buf) {
        start();
    }

    bool failed() const { return in_.failed(); }

    void decode(uint8_t* dst, uint32_t n) {
        if (table_ == nullptr) {
            in_.copy(dst, n);
            return;
        }
        while (n) {
            const uint8_t* p = in_.cursor();
            // У дочитанного прогона нули за концом - его законный хвост.
            const uint8_t* limit = in_.drained() ? in_.end() + kBankDrainedSlack : in_.end();
            const uint32_t k = bank_decode_block(*table_, p, limit, dst, n, st_);
            in_.set_cursor(p);
            dst += k;
            n -= k;
            if (n == 0) break;
            if (in_.drained() || in_.failed()) {
                // Прогон кончился раньше распакованной длины - испорчен.
                if (k == 0) {
                    std::memset(dst, 0, n);
                    break;
                }
                continue;
            }
            in_.refill();
        }
    }

private:
    void start() {
        if (table_ == nullptr) return;
        st_.x = in_.next();
        st_.x |= static_cast<uint32_t>(in_.next()) << 8;
        // Первый байт - в контексте нуля. Испорченный прогон не выводит за
        // таблицу.
        st_.x &= kModelStates - 1u;
    }

    const BankDecodeTable* table_;
    BankByteInput in_;
    TansState st_;
};

inline void bank_decompress(const BankDecodeTable& table, const uint8_t* src, uint32_t packed, uint8_t* dst,
                            uint32_t dst_bytes) {
    uint8_t buf[kBankInputBufferBytes];
    BankUnpacker u(&table, src, packed, buf);
    u.decode(dst, dst_bytes);
}

} // namespace soundsinth::bank
