// SPDX-License-Identifier: MIT
#include "player/config/tools_view.h"

#include "core/config/config_fields.h"

namespace player::config {

using namespace devices;
namespace {

using soundsinth::config::kSdSpeedCount;
using soundsinth::config::kSdSpeedNames;
using soundsinth::config::kToolAlert;
using soundsinth::config::kToolBodyLines;
using soundsinth::config::kToolCyan;
using soundsinth::config::kToolGreen;
using soundsinth::config::kToolInk;
using soundsinth::config::kToolYellow;
using soundsinth::config::tool_size_text;
using soundsinth::config::ToolScreen;

constexpr uint32_t kHint = kToolBodyLines;

void put_size(ToolScreen& scr, uint32_t line, uint8_t attr, const char* label, uint64_t bytes, const char* tail = "") {
    char size[16];
    tool_size_text(size, sizeof(size), bytes);
    scr.putf(line, attr, "%-12s%s%s", label, size, tail);
}

} // namespace

void tool_render_check(ToolScreen& scr, const CheckView& k) {
    const char* const what = k.usb ? "USB DISK" : "CARD";
    scr.clear();
    if (k.phase == CheckPhase::Failed) {
        if (!k.present) {
            scr.putf(0, kToolAlert, "NO %s", what);
        } else {
            scr.putf(0, kToolAlert, "NO FAT ON THE %s", what);
        }
        return;
    }
    const storage::FatVolume& v = k.vol;
    scr.putf(0, kToolInk, "VOLUME      FAT%u %s", v.fat_bits, v.label);
    put_size(scr, 1, kToolInk, "SIZE", static_cast<uint64_t>(v.clusters) * v.cluster_bytes);
    put_size(scr, 2, kToolInk, "CLUSTER", v.cluster_bytes);
    if (v.fat_bits == 12) {
        scr.put(4, "FAT12 IS NOT CHECKED", kToolYellow);
        return;
    }
    if (k.phase == CheckPhase::Scan) {
        scr.put(4, "COUNTING FREE SPACE...");
        scr.progress(5, k.c.sector, storage::fat_check_total(v));
        scr.put(kHint, "READING THE FAT, NOTHING IS");
        scr.put(kHint + 1u, "WRITTEN. ESC STOPS.");
        return;
    }
    const storage::FatCheck& c = k.c;
    uint32_t y                 = 4;
    put_size(scr, y++, kToolInk, "FREE", static_cast<uint64_t>(c.free) * v.cluster_bytes);
    if (c.first_free != storage::kFatNoValue) scr.putf(y++, kToolInk, "FIRST FREE  %u", static_cast<unsigned>(c.first_free));
    const bool wrong      = storage::fat_fsinfo_wrong(v, c);
    const bool free_wrong = v.fsinfo_free != c.free;
    if (v.fsinfo_lba == 0) {
        scr.put(y++, "FSINFO      NONE");
    } else if (!wrong) {
        scr.put(y++, k.fixed ? "FSINFO      FIXED" : "FSINFO      RIGHT", kToolGreen);
    } else if (!free_wrong) {
        put_size(scr, y++, kToolInk, "FSINFO", static_cast<uint64_t>(v.fsinfo_free) * v.cluster_bytes);
    } else if (v.fsinfo_free > v.clusters) {
        scr.put(y++, "FSINFO      UNKNOWN, WRONG", kToolYellow);
    } else {
        put_size(scr, y++, kToolYellow, "FSINFO", static_cast<uint64_t>(v.fsinfo_free) * v.cluster_bytes, ", WRONG");
    }
    if (v.fsinfo_lba != 0) {
        const bool next_wrong = storage::fat_fsinfo_next_wrong(v, c);
        if (v.fsinfo_next == storage::kFatNoValue) {
            scr.put(y++, next_wrong ? "NEXT FREE   NONE, WRONG" : "NEXT FREE   NONE", next_wrong ? kToolYellow : kToolInk);
        } else {
            scr.putf(y++, next_wrong ? kToolYellow : kToolInk, next_wrong ? "NEXT FREE   %u, WRONG" : "NEXT FREE   %u", static_cast<unsigned>(v.fsinfo_next));
        }
    }
    if (v.fat_count < 2) {
        scr.put(y++, "FAT COPIES  ONE");
    } else if (c.copies_differ == 0) {
        scr.put(y++, "FAT COPIES  SAME", kToolGreen);
    } else {
        scr.putf(y++, kToolYellow, "FAT COPIES  DIFFER IN %u", static_cast<unsigned>(c.copies_differ));
    }
    scr.putf(y++, c.bad != 0 ? kToolYellow : kToolInk, "BAD         %u CLUSTERS", static_cast<unsigned>(c.bad));
    scr.putf(y++, c.invalid != 0 ? kToolYellow : kToolInk, "BAD LINKS   %u", static_cast<unsigned>(c.invalid));
    scr.put(y++, c.dirty ? "STATE       NOT CLOSED" : "STATE       CLOSED", c.dirty ? kToolYellow : kToolInk);
    scr.putf(y++, kToolInk, "TIME        %u.%u S", static_cast<unsigned>(k.took_ms / 1000u), static_cast<unsigned>(k.took_ms / 100u % 10u));
    if (c.io_error) scr.putf(y++, kToolAlert, "READ ERROR, FAT SECTOR %u", static_cast<unsigned>(c.sector));
    if (k.fix_failed) {
        scr.put(kHint, "WRITING FSINFO FAILED.", kToolAlert);
    } else if (wrong && !c.io_error) {
        scr.put(kHint, "ENTER - WRITE THE COUNTED");
        scr.put(kHint + 1u, "FREE SPACE AND FIRST FREE");
        scr.put(kHint + 2u, "CLUSTER TO FSINFO.");
    } else {
        scr.put(kHint, "ONLY FSINFO IS FIXED HERE;");
        scr.put(kHint + 1u, "IT IS RIGHT.");
    }
}

void tool_render_hid(ToolScreen& scr, const HidView& v) {
    scr.clear();
    if (v.count == 0) scr.put(0, "NO USB HID DEVICES", kToolYellow);
    for (uint32_t i = 0; i < v.count; ++i) {
        const HidRow& r  = v.rows[i];
        const uint32_t y = 2u * i;
        const char* role = r.role == platform::kUsbRoleMouse      ? "MOUSE"
                           : r.role == platform::kUsbRoleKeyboard ? "KEYB"
                           : r.role == platform::kUsbRoleGamepad  ? "PAD"
                                                                  : "NONE";
        scr.putf(y, r.role != 0 ? kToolCyan : kToolYellow, "%u.%u %04X:%04X %s %s", r.addr, r.idx, r.vid, r.pid, role, r.boot ? "BOOT" : "REPORT");
        if (r.role == 0) {
            scr.putf(y + 1u, kToolInk, "  USAGE %04X:%04X", r.top_page, r.top_usage);
        } else if (r.last_len == 0) {
            scr.put(y + 1u, "  NO REPORT YET");
        } else if (!r.decoded) {
            scr.putf(y + 1u, kToolYellow, "  %u B, NOT DECODED", r.last_len);
        } else if (r.role == platform::kUsbRoleKeyboard) {
            scr.putf(y + 1u, kToolInk, "  %u B MOD %02X KEY %02X", r.last_len, r.keys[0], r.keys[1]);
        } else {
            scr.putf(y + 1u, kToolInk, "  %uB X%d Y%d B%04X", r.last_len, static_cast<int>(r.x), static_cast<int>(r.y), r.buttons);
        }
    }
    if (v.save == HidSave::Saved) {
        scr.put(kHint, "SAVED: USB_HID.TXT ON CARD.", kToolGreen);
    } else if (v.save == HidSave::Failed) {
        scr.put(kHint, "SAVING TO THE CARD FAILED.", kToolAlert);
    } else {
        scr.put(kHint, "ENTER - SAVE USB_HID.TXT");
        scr.put(kHint + 1u, "TO THE CARD. HOLD A BUTTON");
        scr.put(kHint + 2u, "OR LEAVE THE DEVICE IDLE.");
    }
}

void tool_render_speed(ToolScreen& scr, const SpeedView& v) {
    scr.clear();
    if (!v.present) {
        scr.put(0, "NO CARD", kToolAlert);
        return;
    }
    scr.put(0, "MHZ    RESULT          KB/S", kToolCyan);
    uint32_t best = kSdSpeedCount;
    for (uint32_t i = 0; i < kSdSpeedCount; ++i) {
        const SpeedRow& r = v.rows[i];
        const char* mhz   = kSdSpeedNames[i].name;
        const uint32_t y  = 1u + i;
        switch (r.state) {
            case SpeedState::Wait:
                scr.putf(y, kToolInk, "%-7s-", mhz);
                break;
            case SpeedState::Run:
                scr.putf(y, kToolInk, "%-7sTESTING...", mhz);
                break;
            case SpeedState::NoHighSpeed:
                scr.putf(y, kToolYellow, "%-7sNO HIGH SPEED", mhz);
                break;
            case SpeedState::Done:
                if (r.differs) {
                    scr.putf(y, kToolAlert, "%-7sDATA DIFFERS    %u", mhz, static_cast<unsigned>(r.kbps));
                } else if (r.errors != 0) {
                    scr.putf(y, kToolYellow, "%-7s%u ERRORS", mhz, static_cast<unsigned>(r.errors));
                } else {
                    scr.putf(y, kToolGreen, "%-7s%-16s%u", mhz, "NO ERRORS", static_cast<unsigned>(r.kbps));
                    best = i;
                }
                break;
        }
    }
    scr.putf(7, kToolInk, "IN SETUP    %s MHZ", kSdSpeedNames[v.setting].name);
    if (v.running) {
        scr.progress(9, v.done, v.total);
        scr.put(kHint, "ONLY READS, NOTHING IS");
        scr.put(kHint + 1u, "WRITTEN. ESC STOPS.");
        return;
    }
    if (v.stopped) {
        scr.put(9, "STOPPED", kToolYellow);
    } else if (best < kSdSpeedCount) {
        scr.putf(9, kToolGreen, "FASTEST CLEAN %s MHZ", kSdSpeedNames[best].name);
    } else {
        scr.put(9, "NO CLOCK WITHOUT ERRORS", kToolAlert);
    }
    scr.put(kHint, "SET THE CLOCK IN DISKS AND");
    scr.put(kHint + 1u, "DRIVES - SD CARD MHZ.");
}

} // namespace player::config
