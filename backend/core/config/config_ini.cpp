// SPDX-License-Identifier: MIT
#include "core/config/config_ini.h"

#include "core/config/config_fields.h"

#include <cstring>

namespace soundsinth::config {
namespace {

bool printable(char c) {
    return static_cast<unsigned char>(c) > ' ' && static_cast<unsigned char>(c) != 0x7F;
}

char upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Выбросить пробелы и непечатное, привести к верхнему регистру. Возвращает
// длину; при нехватке места - ноль, такая строка нам заведомо не нужна.
uint32_t squeeze(const char* p, uint32_t n, char* out, uint32_t cap) {
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!printable(p[i])) continue;
        if (len + 1u >= cap) return 0;
        out[len++] = upper(p[i]);
    }
    out[len] = '\0';
    return len;
}

bool parse_value(const KeyDesc& k, const char* v, uint8_t& out) {
    if (*v == '\0') return false;

    const NameTable t = names_of(k.kind);
    if (t.items != nullptr) {
        for (uint32_t i = 0; i < t.count; ++i) {
            if (std::strcmp(v, t.items[i].name) == 0) {
                out = t.items[i].value;
                return true;
            }
        }
        return false;
    }

    if (k.kind == Kind::Flag) {
        if (std::strcmp(v, "1") == 0 || std::strcmp(v, "ON") == 0 || std::strcmp(v, "YES") == 0 || std::strcmp(v, "TRUE") == 0) {
            out = 1;
            return true;
        }
        if (std::strcmp(v, "0") == 0 || std::strcmp(v, "OFF") == 0 || std::strcmp(v, "NO") == 0 || std::strcmp(v, "FALSE") == 0) {
            out = 0;
            return true;
        }
        return false;
    }

    uint32_t n = 0;
    for (const char* p = v; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10u + static_cast<uint32_t>(*p - '0');
        if (n > 255u) return false;
    }
    if (n < k.min || n > k.max) return false;
    out = static_cast<uint8_t>(n);
    return true;
}

// Дописать строку в буфер. Считает нужную длину и при нехватке места
// только считает - так одна и та же функция и меряет, и пишет.
void put(const char* s, char* out, uint32_t cap, uint32_t& len) {
    for (const char* p = s; *p != '\0'; ++p) {
        if (out != nullptr && len + 1u < cap) out[len] = *p;
        ++len;
    }
}

void put_num(uint8_t v, char* out, uint32_t cap, uint32_t& len) {
    char buf[4];
    uint32_t n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char rev[4];
        uint32_t r = 0;
        for (uint8_t x = v; x != 0; x /= 10u) {
            rev[r++] = static_cast<char>('0' + x % 10u);
        }
        while (r != 0) {
            buf[n++] = rev[--r];
        }
    }
    buf[n] = '\0';
    put(buf, out, cap, len);
}

void put_value(const KeyDesc& k, uint8_t v, char* out, uint32_t cap, uint32_t& len) {
    const NameTable t = names_of(k.kind);
    if (t.items == nullptr) {
        put_num(v, out, cap, len);
        return;
    }
    for (uint32_t i = 0; i < t.count; ++i) {
        if (t.items[i].value == v) {
            put(t.items[i].name, out, cap, len);
            return;
        }
    }
    put_num(v, out, cap, len); // значение вне таблицы: пишем числом, чтобы не потерять
}

} // namespace

IniResult ini_parse(const char* text, uint32_t len, Settings& s) {
    IniResult res;
    if (text == nullptr) return res;

    uint32_t at = 0;
    while (at < len) {
        uint32_t end = at;
        while (end < len && text[end] != '\n' && text[end] != '\r') {
            ++end;
        }
        const char* line        = text + at;
        const uint32_t line_len = end - at;
        at                      = end + 1u;

        // Комментарий узнаётся по первому печатному знаку строки.
        uint32_t first = 0;
        while (first < line_len && !printable(line[first])) {
            ++first;
        }
        if (first == line_len) continue;
        if (line[first] == ';' || line[first] == '#') continue;

        uint32_t eq = first;
        while (eq < line_len && line[eq] != '=') {
            ++eq;
        }
        if (eq == line_len) continue; // строки без "=" молча пропускаем

        char key[32];
        char val[32];
        const uint32_t klen = squeeze(line + first, eq - first, key, sizeof(key));
        const uint32_t vlen = squeeze(line + eq + 1u, line_len - eq - 1u, val, sizeof(val));
        if (klen == 0 || vlen == 0) {
            ++res.bad;
            continue;
        }

        const KeyDesc* found = nullptr;
        for (const KeyDesc& k : kKeys) {
            if (std::strcmp(key, k.name) == 0) {
                found = &k;
                break;
            }
        }
        if (found == nullptr) {
            ++res.unknown;
            continue;
        }

        uint8_t parsed = 0;
        if (!parse_value(*found, val, parsed)) {
            ++res.bad;
            continue;
        }
        s.*(found->field) = parsed;
        ++res.applied;
    }
    settings_clamp(s);
    return res;
}

uint32_t ini_render(const Settings& s, char* out, uint32_t cap) {
    static const Settings kDefaults;
    uint32_t len = 0;
    put("; PI-CARD MUSICA board settings.\n", out, cap, len);
    put(";\n", out, cap, len);
    put("; Put this in the card root as set_config.txt - the board applies it\n", out, cap, len);
    put("; at power-up and renames it to set_config.done.\n", out, cap, len);
    put(";\n", out, cap, len);
    put("; To see the current settings, put an empty get_config.txt there:\n", out, cap, len);
    put("; the board writes the current settings into it and renames it to\n", out, cap, len);
    put("; get_config.done.\n", out, cap, len);

    // Распиновка - здесь, а не только в схеме: файл возвращается с платы, и
    // по нему подключают дополнительные сигналы.
    put("\n\n; --- Board pins ---\n", out, cap, len);
    put(";\n", out, cap, len);
    put("; GPIO32  log, output, 115200 8N1        - key LOG\n", out, cap, len);
    put("; GPIO33  live MIDI input, 31250 8N1     - key LIVE_MIDI=WIRE\n", out, cap, len);
    put("; GPIO34  Wi-Fi, module enable           - key WIFI\n", out, cap, len);
    put("; GPIO36  DOS_N, output, active low      - key DISKSYS=TRDOS\n", out, cap, len);
    put("; GPIO37  settings jumper to ground      - boot into the configurator\n", out, cap, len);
    put("; GPIO38  Wi-Fi, transmit                - key WIFI\n", out, cap, len);
    put("; GPIO39  Wi-Fi, receive                 - key WIFI\n", out, cap, len);

    for (const KeyDesc& k : kKeys) {
        if (k.section != nullptr) {
            put("\n\n; --- ", out, cap, len);
            put(k.section, out, cap, len);
            put(" ---\n", out, cap, len);
        }
        put("\n; ", out, cap, len);
        put(k.comment, out, cap, len);
        // Умолчание берётся из самих умолчаний, разойтись с ними не может.
        put("; default ", out, cap, len);
        put_value(k, kDefaults.*(k.field), out, cap, len);
        put("\n", out, cap, len);
        put(k.name, out, cap, len);
        put("=", out, cap, len);
        put_value(k, s.*(k.field), out, cap, len);
        put("\n", out, cap, len);
    }

    if (out != nullptr && cap != 0) out[len < cap ? len : cap - 1u] = '\0';
    return len;
}

} // namespace soundsinth::config
