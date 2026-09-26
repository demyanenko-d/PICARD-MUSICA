// Банк инструментов .mid (player/banks.h).

#include "player/banks.h"

#include <cinttypes>

#include "player/bank_sd.h"
#include "platform/log.h"
#include "player/shared_state.h"
#include "core/bank/bank_reader.h"
#include "player/load/session_loader.h"
#include "core/memory/psram_store.h"

namespace player {

void open_banks(const uint8_t* flash_blob, uint32_t flash_bytes) {
    const char* bank_err = nullptr;
    // Флеш-банк - всегда: живой MIDI подгружает сэмплы только из него.
    // bank_open проверяет сигнатуру, версию и CRC таблиц, PCM - нет.
    if (soundsinth::bank::bank_open(flash_blob, flash_bytes, shared::g_flash_bank, &bank_err)) {
        // Контрольные суммы - чтобы отличить залитый банк от собранного на ПК:
        // имя и счётчики у пересобранного те же.
        // Двумя вызовами, одной строкой: имя банка до 28 байт, а буфер
        // debug_logf короткий.
        debug_logf("boot: банк во флеше %s, инструментов %u, сэмплов %u", shared::g_flash_bank.header->name,
                   static_cast<unsigned>(shared::g_flash_bank.header->instrument_count),
                   static_cast<unsigned>(shared::g_flash_bank.header->sample_count));
        debug_logf(", crc таблиц %08" PRIX32 ", PCM %08" PRIX32 "\n", shared::g_flash_bank.header->table_crc32,
                   shared::g_flash_bank.header->pcm_crc32);
    } else {
        debug_logf("boot: банка во флеше нет (%s)\n", bank_err ? bank_err : "причина не названа");
    }
    bank_err = nullptr;
    if (bank_sd_open(shared::g_bank)) {
        player::load::session_loader_set_bank(&shared::g_bank);
        // Таблицы банка лежат в конце чипа (kBankTableOffset): хранилище
        // трека ужимается ниже них. Пока трек не загружен, это безопасно:
        // список свободных страниц пересобирается, выделять из него ещё
        // некому.
        soundsinth::memory::psram_set_track_bytes(shared::g_track_memory.psram, soundsinth::memory::kBankTableOffset);
        debug_logf("boot: хранилище трека ужато до %" PRIu32 " КБ под таблицы банка\n",
                   soundsinth::memory::kBankTableOffset / 1024u);
    } else if (soundsinth::bank::bank_open(flash_blob, flash_bytes, shared::g_bank, &bank_err)) {
        player::load::session_loader_set_bank(&shared::g_bank);
        debug_logf("boot: банк %s, инструментов %u, сэмплов %u\n", shared::g_bank.header->name,
                   static_cast<unsigned>(shared::g_bank.header->instrument_count),
                   static_cast<unsigned>(shared::g_bank.header->sample_count));
    } else {
        debug_logf("boot: банка нет (%s) - .mid недоступен, остальное работает\n",
                   bank_err ? bank_err : "причина не названа");
    }
}

} // namespace player
