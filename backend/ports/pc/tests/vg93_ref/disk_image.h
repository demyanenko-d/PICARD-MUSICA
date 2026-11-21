// SPDX-License-Identifier: MIT
#pragma once

// Образ диска для Beta Disk: разбор файла и дорожки как поток слотов.
//
// Дорожка хранится одним из трёх видов: список секторов (TRD, SCL, FDI,
// TD0), сырые байты MFM с картой меток синхронизации (UDI) или поток
// ячеек (HFE и дорожки, записанные контроллером не по разметке образа).
// Файл лежит в буфере владельца и правится на месте; таблицы и
// преобразованные данные - в области памяти Arena (на плате - PSRAM).

#include <cstddef>
#include <cstdint>

#include "devices/bdi/disk.h" // ImageFormat, Arena

#include "slot_drive.h"

namespace devices::bdi {

// Формат по содержимому, при неоднозначности - по расширению имени.
ImageFormat detect_format(const uint8_t* data, size_t size, const char* name);

inline constexpr uint8_t kSecDeleted    = 1; // метка данных F8
inline constexpr uint8_t kSecBadIdCrc   = 2;
inline constexpr uint8_t kSecBadDataCrc = 4;
inline constexpr uint8_t kSecNoData     = 8; // заголовок без поля данных

struct ImgSector {
    uint8_t c, h, r, n;
    uint8_t flags;
    uint8_t* data; // 128 << (n & 3) байт
};

enum class TrackKind : uint8_t { Empty, Sectors, RawBytes, Cells };

struct ImgTrack {
    TrackKind kind     = TrackKind::Empty;
    uint8_t count      = 0;       // секторов
    uint16_t raw_len   = 0;       // байтов MFM (RawBytes)
    ImgSector* sectors = nullptr; // Sectors
    uint8_t* raw       = nullptr; // RawBytes: байты, затем карта меток; Cells: kTrackBytes
    bool dirty         = false;   // изменена после открытия
};

class DiskImage {
public:
    // file - содержимое файла; правки записанных секторов идут в него же
    // (кроме SCL и TD0, которые разворачиваются в arena). false - файл не
    // разобран.
    bool open(ImageFormat fmt, uint8_t* file, size_t size, Arena& arena);
    void close();

    // Поток слотов дорожки (kTrackBytes байт). Неразмеченная - нули.
    void track_cells(int cyl, int side, uint8_t* bits) const;
    // Контроллер записал дорожку: сектора по разметке образа обновляются на
    // месте, иначе дорожка хранится потоком ячеек. false - нет места в arena.
    bool store_cells(int cyl, int side, const uint8_t* bits, Arena& arena);

    // Сохранение в формат файла: TRD, FDI, UDI, HFE. false - образ нельзя
    // сохранить в свой формат (SCL, TD0, дорожки не по разметке).
    bool can_save() const;
    bool dirty() const;

    ImageFormat format() const { return fmt_; }
    int cylinders() const { return ncyl_; }
    int sides() const { return nsides_; }
    bool write_protect() const { return wprot_; }
    const ImgTrack& track(int cyl, int side) const { return tracks_[cyl][side]; }

private:
    bool open_trd(uint8_t* file, size_t size, Arena& arena);
    bool open_fdi(uint8_t* file, size_t size, Arena& arena);
    bool open_udi(uint8_t* file, size_t size);
    bool open_td0(const uint8_t* file, size_t size, Arena& arena);
    bool open_hfe(uint8_t* file, size_t size);

    ImageFormat fmt_ = ImageFormat::Unknown;
    int ncyl_ = 0, nsides_ = 0;
    bool wprot_       = false;
    bool off_layout_  = false; // есть дорожки не по разметке образа
    bool hfe_written_ = false; // HFE правлен на месте
    uint8_t* file_    = nullptr;
    size_t file_size_ = 0;
    ImgTrack tracks_[kMaxCylinders][2];
};

} // namespace devices::bdi
