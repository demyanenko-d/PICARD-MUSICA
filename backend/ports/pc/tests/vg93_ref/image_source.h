// SPDX-License-Identifier: MIT
#pragma once

// Образ целиком в памяти (DiskImage) как источник дисковода, готов всегда;
// для ПК: тесты и стенды. Отдельным заголовком: видя единственного
// наследника источника, компилятор подставляет его вызовы в код банков BDI,
// и тот идёт во флеш.

#include <vector>

#include "beta_disk.h"
#include "disk_image.h"
#include "devices/bdi/vg93.h"
#include "mfm.h"

namespace devices::bdi {

// Потоком ячеек - для Vg93w (BetaDisk).
class ImageSource final : public DiskSource {
public:
    ImageSource() = default;
    ImageSource(DiskImage* img, Arena* arena) : img_(img), arena_(arena) {}
    bool cylinder(int cyl, uint8_t* const cells[2], unsigned mask) override {
        for (int side = 0; side < 2; side++) {
            if ((mask & (1u << side)) != 0) img_->track_cells(cyl, side, cells[side]);
        }
        return true;
    }
    bool written(int cyl, int side, const uint8_t* cells) override { return img_->store_cells(cyl, side, cells, *arena_); }
    bool write_protect() const override { return img_->write_protect(); }

private:
    DiskImage* img_ = nullptr;
    Arena* arena_   = nullptr;
};

// Дорожками байтов - для автомата Vg93: цилиндр под головкой переводится из
// ячеек, записанная сторона - обратно в образ.
class ImageTracks final : public Disk {
public:
    ImageTracks() = default;
    ImageTracks(DiskImage* img, Arena* arena) : img_(img), arena_(arena) {}
    Track* cylinder(int cyl) override {
        if (cyl != cyl_) {
            for (int side = 0; side < 2; side++) {
                img_->track_cells(cyl, side, cells_.data());
                track_from_cells(t_[side], cells_.data());
            }
            cyl_ = cyl;
        }
        return t_;
    }
    void written(int cyl, int side) override {
        track_to_cells(t_[side], cells_.data());
        if (!img_->store_cells(cyl, side, cells_.data(), *arena_)) failed_ = true;
    }
    bool write_protect() const override { return img_->write_protect(); }
    bool failed() const { return failed_; }

private:
    DiskImage* img_             = nullptr;
    Arena* arena_               = nullptr;
    int cyl_                    = -1;
    bool failed_                = false;
    Track t_[2]                 = {};
    std::vector<uint8_t> cells_ = std::vector<uint8_t>(kTrackBytes);
};

} // namespace devices::bdi
