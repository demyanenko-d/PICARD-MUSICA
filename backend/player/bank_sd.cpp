// Банк с карты памяти (bank_sd.h).

#include "player/bank_sd.h"

#include <cinttypes>
#include <cstdio>

#include "ff.h"

#include "platform/memory.h"
#include "core/bank/bank_format.h"
#include "core/memory/psram_store.h"
#include "devices/storage/storage.h"

namespace player {
namespace {

FATFS s_fs;
FIL   s_file;
bool  s_open = false;
uint32_t s_pcm_base = 0;   // смещение зоны PCM от начала файла

// Чтение PCM по требованию распаковщика, смещение от начала зоны PCM
// (как BankSample::pcm_offset).
//
// 0 - отказ FatFs: предел чтения - конец зоны PCM, а файл не короче
// total_bytes, законного короткого чтения нет.
uint32_t pcm_read(void*, uint32_t offset, uint8_t* dst, uint32_t bytes) {
    if (!s_open) return 0;
    const FSIZE_t want = static_cast<FSIZE_t>(s_pcm_base) + offset;
    if (f_tell(&s_file) != want && f_lseek(&s_file, want) != FR_OK) return 0;
    UINT got = 0;
    if (f_read(&s_file, dst, bytes, &got) != FR_OK) return 0;
    return got;
}

} // namespace

bool bank_sd_open(soundsinth::bank::Bank& out) {
    namespace bank = soundsinth::bank;
    namespace memory = soundsinth::memory;

    if (!devices::storage::storage_present()) return false;

    // Том монтируется сразу (второй аргумент 1): отложенное монтирование
    // перенесло бы отказ в первое чтение, а решение "флеш или карта"
    // нужно здесь.
    if (f_mount(&s_fs, "", 1) != FR_OK) {
        std::printf("boot: банк с карты — файловая система не смонтирована (FAT16/32?)\n");
        return false;
    }
    if (f_open(&s_file, SOUNDSINTH_BANK_SD_PATH, FA_READ) != FR_OK) {
        std::printf("boot: банка на карте нет (%s) — берём из флеша\n", SOUNDSINTH_BANK_SD_PATH);
        f_mount(nullptr, "", 0);
        return false;
    }
    s_open = true;

    auto give_up = [&](const char* why) {
        std::printf("boot: банк с карты не взят: %s\n", why);
        f_close(&s_file);
        f_mount(nullptr, "", 0);
        s_open = false;
        return false;
    };

    // Заголовок первым: в нём длина таблиц, от неё зависит, влезут ли они.
    bank::BankHeader head{};
    UINT got = 0;
    if (f_read(&s_file, &head, sizeof(head), &got) != FR_OK || got != sizeof(head)) {
        return give_up("заголовок не читается");
    }
    if (head.magic != bank::kMagic) return give_up("не банк: сигнатура не совпала");
    // Версия проверяется здесь, с числами в логе: bank_open() скажет только
    // "версия формата банка не та", а сигнатура SSB1 у всех версий одна, и
    // старый банк иначе не отличить от испорченного файла или сбоя чтения.
    if (head.version != bank::kVersion) {
        char m[128];
        std::snprintf(m, sizeof(m), "версия банка %u, а нужна %u — перепеките sf2bake",
                      head.version, bank::kVersion);
        return give_up(m);
    }
    if (head.pcm_offset > memory::kBankTableBytes) {
        char m[128];
        std::snprintf(m, sizeof(m), "таблицы %" PRIu32 " КБ, а места отведено %" PRIu32 " КБ (config.h)",
                      head.pcm_offset / 1024u, memory::kBankTableBytes / 1024u);
        return give_up(m);
    }
    if (f_size(&s_file) < head.total_bytes) return give_up("файл короче, чем говорит заголовок");

    // Таблицы - в конце чипа (kBankTableOffset); хранилище трека на них
    // ужимается при старте (psram_set_track_bytes), буфер GS режется уже
    // ниже, пересечения нет.
    uint8_t* tables = platform::psram_base_acquire(memory::kPsramChipBytes) + memory::kBankTableOffset;
    if (f_lseek(&s_file, 0) != FR_OK) return give_up("перемотка к началу не удалась");
    uint32_t done = 0;
    while (done < head.pcm_offset) {
        // По 32 КБ: накладные у f_read на вызов, а не на объём.
        constexpr uint32_t kTableChunkBytes = 32768u;
        const uint32_t chunk = head.pcm_offset - done < kTableChunkBytes ? head.pcm_offset - done : kTableChunkBytes;
        if (f_read(&s_file, tables + done, chunk, &got) != FR_OK || got != chunk) {
            return give_up("таблицы не дочитались");
        }
        done += chunk;
    }
    // Таблицы легли в кэшируемое окно; bank_open и движок читают тем же
    // кэшем, сброс не нужен.
    const char* err = nullptr;
    if (!bank::bank_open(tables, head.pcm_offset, out, &err, /*tables_only=*/true)) {
        return give_up(err ? err : "причина не названа");
    }
    s_pcm_base = head.pcm_offset;
    out.pcm_source = bank::BankPcmSource{pcm_read, nullptr};

    // Десятые мегабайта целыми: без плавающей точки в объекте.
    const uint32_t tenths_mb = head.total_bytes / (1048576u / 10u);
    std::printf("boot: банк с карты %s: %" PRIu32 ".%" PRIu32 " МБ, таблицы %" PRIu32
                " КБ, инструментов %u, сэмплов %u\n",
                SOUNDSINTH_BANK_SD_PATH, tenths_mb / 10u, tenths_mb % 10u, head.pcm_offset / 1024u,
                head.instrument_count, head.sample_count);
    return true;
}

} // namespace player
