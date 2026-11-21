// SPDX-License-Identifier: MIT
#include "player/config/config_tools.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace soundsinth::config {
namespace {

char ascii_or_q(uint8_t c) {
    return ascii_printable(c) ? static_cast<char>(c) : '?';
}

} // namespace

ToolId ToolScreen::tool() const {
    return static_cast<ToolId>(a_->tool);
}

bool ToolScreen::key(uint8_t& k, ToolId mine) {
    volatile ToolArea& a = *a_;
    const uint8_t ack    = a.ack;
    const uint8_t req    = a.req;
    if (ack == req) return false;
    // Инструмент и ячейку писатель кладёт раньше счётчика.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (static_cast<ToolId>(a.tool) != mine) return false;
    k = a.keys[ack % kToolKeySlots];
    // Ячейка прочитана до того, как писатель увидит её свободной.
    std::atomic_thread_fence(std::memory_order_release);
    a.ack = static_cast<uint8_t>(ack + 1u);
    return true;
}

void ToolScreen::clear() {
    std::memset(a_->text, ' ', sizeof(a_->text));
    std::memset(a_->attr, kToolInk, sizeof(a_->attr));
    a_->cur_row = kToolNoCursor;
}

void ToolScreen::put(uint32_t line, const char* s, uint8_t attr) {
    if (line >= kToolLines) return;
    char* row = a_->text[line];
    std::memset(row, ' ', kToolCols);
    for (uint32_t n = 0; s != nullptr && s[n] != 0 && n < kToolTextCols; ++n) {
        const auto c = static_cast<uint8_t>(s[n]);
        row[1u + n]  = ascii_or_q(c);
    }
    a_->attr[line] = attr;
}

void ToolScreen::vputf(uint32_t line, uint8_t attr, const char* fmt, va_list ap) {
    char buf[kToolCols + 1];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    put(line, buf, attr);
}

void ToolScreen::putf(uint32_t line, uint8_t attr, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vputf(line, attr, fmt, ap);
    va_end(ap);
}

void ToolScreen::progress(uint32_t line, uint32_t done, uint32_t total) {
    constexpr uint32_t kBar = kToolTextCols - 8u; // [ ] и " 100%"
    const uint32_t pct      = total == 0 ? 100u : static_cast<uint32_t>(static_cast<uint64_t>(done) * 100u / total);
    const uint32_t fill     = pct * kBar / 100u;
    char buf[kToolCols + 1];
    buf[0] = '[';
    for (uint32_t i = 0; i < kBar; ++i) {
        buf[1 + i] = i < fill ? '#' : '.';
    }
    std::snprintf(buf + 1 + kBar, sizeof(buf) - 1 - kBar, "] %3u%%", static_cast<unsigned>(pct));
    put(line, buf, kToolYellow);
}

void ToolScreen::cursor(uint32_t line, uint32_t col) {
    a_->cur_row = static_cast<uint8_t>(line);
    a_->cur_col = static_cast<uint8_t>(col + 1u);
}

void ToolScreen::no_cursor() {
    a_->cur_row = kToolNoCursor;
}

void ToolScreen::state(ToolState s) {
    a_->state = static_cast<uint8_t>(s);
}

ToolState ToolScreen::state() const {
    return static_cast<ToolState>(a_->state);
}

void ToolScreen::publish() {
    // Текст раньше счётчика: ПЗУ читает их в обратном порядке.
    std::atomic_thread_fence(std::memory_order_release);
    *static_cast<volatile uint8_t*>(&a_->upd) = static_cast<uint8_t>(a_->upd + 1u);
}

void tool_area_reset(uint8_t* page) {
    std::memset(page + kToolOffset, 0, sizeof(ToolArea));
    ToolScreen scr(page);
    scr.clear();
}

void tool_size_text(char* out, uint32_t cap, uint64_t bytes) {
    constexpr uint64_t kKb = 1024u;
    constexpr uint64_t kMb = kKb * 1024u;
    constexpr uint64_t kGb = kMb * 1024u;
    if (bytes >= 10u * kGb) {
        std::snprintf(out, cap, "%u GB", static_cast<unsigned>(bytes / kGb));
    } else if (bytes >= kGb) {
        const auto tenths = static_cast<unsigned>(bytes * 10u / kGb);
        std::snprintf(out, cap, "%u.%u GB", tenths / 10u, tenths % 10u);
    } else if (bytes >= kMb) {
        std::snprintf(out, cap, "%u MB", static_cast<unsigned>(bytes / kMb));
    } else if (bytes >= kKb) {
        std::snprintf(out, cap, "%u KB", static_cast<unsigned>(bytes / kKb));
    } else {
        std::snprintf(out, cap, "%u B", static_cast<unsigned>(bytes));
    }
}

namespace {

// Производители карт по полю MID: самые частые.
struct Maker {
    uint8_t id;
    const char* name;
};
constexpr Maker kMakers[] = {
    {0x01, "PANASONIC"}, {0x02, "TOSHIBA"},   {0x03, "SANDISK"},  {0x1b, "SAMSUNG"},   {0x1d, "ADATA"},   {0x27, "PHISON"},
    {0x28, "LEXAR"},     {0x31, "SIL.POWER"}, {0x41, "KINGSTON"}, {0x74, "TRANSCEND"}, {0x76, "PATRIOT"}, {0x82, "SONY"},
};

const char* maker_name(uint8_t id) {
    for (const Maker& m : kMakers) {
        if (m.id == id) return m.name;
    }
    return "";
}

} // namespace

void tool_render_card(ToolScreen& scr, const CardFacts& card, const VolumeFacts& vol) {
    scr.clear();
    if (!card.present) {
        scr.put(0, "NO CARD", kToolAlert);
        scr.put(kToolHintLine, "THE CARD DID NOT ANSWER.");
        return;
    }
    char size[16];
    tool_size_text(size, sizeof(size), static_cast<uint64_t>(card.sectors) * 512u);
    uint32_t y = 0;
    scr.putf(y++, kToolInk, "TYPE        %s", card.sdhc ? "SDHC/SDXC" : "SDSC");
    scr.putf(y++, kToolInk, "SIZE        %s", size);
    if (card.cid_valid) {
        const uint8_t* c = card.cid;
        scr.putf(y++, kToolInk, "MAKER       %02X %c%c %s", c[0], ascii_or_q(c[1]), ascii_or_q(c[2]), maker_name(c[0]));
        scr.putf(y++, kToolInk, "NAME        %c%c%c%c%c", ascii_or_q(c[3]), ascii_or_q(c[4]), ascii_or_q(c[5]), ascii_or_q(c[6]), ascii_or_q(c[7]));
        scr.putf(y++, kToolInk, "REVISION    %u.%u", c[8] >> 4, c[8] & 0x0fu);
        scr.putf(y++, kToolInk, "SERIAL      %02X%02X%02X%02X", c[9], c[10], c[11], c[12]);
        const unsigned year  = 2000u + static_cast<unsigned>(((c[13] & 0x0fu) << 4) | (c[14] >> 4));
        const unsigned month = c[14] & 0x0fu;
        scr.putf(y++, kToolInk, "MADE        %u-%02u", year, month);
    } else {
        scr.put(y++, "CID         NOT READ", kToolYellow);
    }
    ++y;
    if (!vol.valid) {
        scr.put(y++, "FILE SYSTEM NOT FAT", kToolYellow);
    } else {
        scr.putf(y++, kToolInk, "FILE SYSTEM FAT%u", vol.fat_bits);
        scr.putf(y++, kToolInk, "LABEL       %s", vol.label);
        tool_size_text(size, sizeof(size), vol.cluster_bytes);
        scr.putf(y++, kToolInk, "CLUSTER     %s", size);
        if (vol.fsinfo_free == 0xffffffffu || vol.fsinfo_free > vol.clusters) {
            scr.put(y++, "FREE        UNKNOWN", kToolYellow);
        } else {
            tool_size_text(size, sizeof(size), static_cast<uint64_t>(vol.fsinfo_free) * vol.cluster_bytes);
            scr.putf(y++, kToolInk, "FREE        %s", size);
        }
    }
    scr.put(kToolHintLine, "FREE SPACE IS FROM FSINFO;");
    scr.put(kToolHintLine + 1u, "CHECK CARD RECOUNTS IT.");
}

void tool_render_board(ToolScreen& scr, const BoardFacts& b) {
    scr.clear();
    char size[16];
    uint32_t y = 0;
    scr.putf(y++, kToolInk, "FIRMWARE    %s", b.version);
    scr.putf(y++, kToolInk, "CHIP        RP2350 A%u", b.chip_revision);
    scr.putf(y++, kToolInk, "CLOCK       %u MHZ", static_cast<unsigned>(b.sys_hz / 1000000u));
    tool_size_text(size, sizeof(size), b.flash_bytes);
    scr.putf(y++, kToolInk, "FLASH       %s", size);
    if (b.psram_bytes != 0) {
        tool_size_text(size, sizeof(size), b.psram_bytes);
        scr.putf(y++, kToolInk, "PSRAM       %s", size);
    } else {
        scr.put(y++, "PSRAM       NONE", kToolYellow);
    }
    scr.putf(y++, kToolInk, "ROM SLOTS   %u OF 3 USED", b.rom_slots_used);
    const unsigned s = static_cast<unsigned>(b.uptime_s);
    scr.putf(y++, kToolInk, "UPTIME      %u:%02u:%02u", s / 3600u, s / 60u % 60u, s % 60u);
}

void tool_render_usb(ToolScreen& scr, const platform::UsbDevice* dev, uint32_t count) {
    scr.clear();
    static const char* const kSpeed[] = {"FULL", "LOW", "HIGH"};
    // Имя роли - по номеру бита kUsbRole*.
    static const char* const kRole[] = {"KEYBOARD", "MOUSE", "GAMEPAD", "DISK", "HUB"};
    static_assert(platform::kUsbRoleKeyboard == 1u << 0 && platform::kUsbRoleMouse == 1u << 1 && platform::kUsbRoleGamepad == 1u << 2 &&
                      platform::kUsbRoleDisk == 1u << 3 && platform::kUsbRoleHub == 1u << (std::size(kRole) - 1u),
                  "kRole order differs from the kUsbRole* bits");
    constexpr uint32_t kIndent = 3; // отступ строки ролей
    if (count == 0) {
        scr.put(0, "NO USB DEVICES", kToolYellow);
    }
    uint32_t y = 0;
    for (uint32_t i = 0; i < count && y + 1u < kToolBodyLines; ++i) {
        const platform::UsbDevice& d = dev[i];
        scr.putf(y++, kToolInk, "%u  %04X:%04X  %s SPEED", d.addr, d.vid, d.pid, kSpeed[d.speed < std::size(kSpeed) ? d.speed : 0]);
        char roles[kToolTextCols + 1] = "   ";
        uint32_t n                    = kIndent;
        for (uint32_t r = 0; r < std::size(kRole); ++r) {
            if ((d.roles & (1u << r)) == 0) continue;
            n += static_cast<uint32_t>(std::snprintf(roles + n, sizeof(roles) - n, "%s%s", n > kIndent ? ", " : "", kRole[r]));
            if (n >= kToolTextCols) break;
        }
        if (d.roles == 0) std::snprintf(roles + kIndent, sizeof(roles) - kIndent, "NOT USED");
        scr.put(y++, roles, kToolCyan);
    }
    scr.put(kToolHintLine, "ADDRESS, VID:PID, SPEED AND");
    scr.put(kToolHintLine + 1u, "WHAT THE BOARD USES IT FOR.");
}

} // namespace soundsinth::config
