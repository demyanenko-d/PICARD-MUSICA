#pragma once

// Метаданные трека для хоста (код 0x1B): строки формата, чипов и тегов,
// порезанные на блоки по 16 байт, блок - кадр. Кадр собирается в момент
// выставления из файла, копии метаданных в памяти нет.

#include <cstdint>

#include "player/protocol/host_protocol.h"

namespace player::vgm {
class Player;
}
namespace soundsinth::model {
struct Song;
}

namespace player::protocol {

// Тип метаданных - байт 0 кадра (без бита последнего кадра).
inline constexpr uint8_t kMetaFormat   = 0x01; // метка 4, класс 1, версия BCD 2
inline constexpr uint8_t kMetaParams   = 0x02; // числа формата, BCD u32 каждое
inline constexpr uint8_t kMetaTagFirst = 0x10; // 0x10-0x16 - теги GD3, до 0x1F - теги
inline constexpr uint8_t kMetaProgram  = 0x17; // чем сделан трекерный файл
inline constexpr uint8_t kMetaChip     = 0x20; // 0x20-0x2F - чип 0..15

// Класс формата - байт 4 строки формата.
inline constexpr uint8_t kClassTracker = 1;
inline constexpr uint8_t kClassMidi    = 2;
inline constexpr uint8_t kClassChip    = 3;

struct TrackMeta {
    char format[4]   = {}; // метка ASCII, хвост нулями
    uint8_t klass    = 0;
    uint16_t version = 0; // цифрами BCD, как в файле
    // Чипы и теги журнала; nullptr - трек играет движок, чип "MUSICA".
    const vgm::Player* vgm = nullptr;
    // Трек движка: числа формата, название, программа; nullptr - их нет.
    const soundsinth::model::Song* song = nullptr;
};

// Поставщик для HostProtocol::set_track_meta, user - TrackMeta.
bool track_meta_frame(void* user, uint8_t frame, uint8_t (&out)[HostProtocol::kTrackMetaBytes]);

// Символ UTF-16 в CP866 плеера: ASCII как есть, управляющие - пробел,
// кириллица - раскладка ALT, прочее - '?'.
uint8_t cp866(uint16_t unit);

// Байт строки трекера (CP437) в CP866: ASCII и псевдографика 0xB0-0xDF
// совпадают, управляющие - пробел, прочее - '?'.
uint8_t cp866_from_cp437(uint8_t c);

} // namespace player::protocol
