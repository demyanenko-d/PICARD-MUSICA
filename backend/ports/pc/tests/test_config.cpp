// SPDX-License-Identifier: MIT
// Настройки платы: два блока во флеше и текстовый файл с карты.
//
// Проверяется то, ради чего механизм и сделан: обрыв записи не уносит
// прежние настройки, износ ложится на оба блока по очереди, а файл с
// опечаткой применяется в остальной части.

#include "testing.h"

#include <cstddef>
#include <cstring>
#include <string>

#include "core/config/config.h"
#include "core/config/config_fields.h"
#include "core/config/config_ini.h"
#include "player/config/config_menu.h"
#include "player/config/config_page.h"
#include "player/config/config_term.h"
#include "player/config/config_tools.h"
#include "player/config/tool_wifi.h"
#include "core/config/config_store.h"
#include "core/util/crc32.h"
#include "core/memory/track_memory.h"
#include "player/config/tools_service.h"

using namespace soundsinth::config;

// Часы теста (test_sd_spi_emu.cpp): экраны TOOLS перерисовываются по ним.
extern uint32_t g_test_time_us;

namespace {

// Стёртый блок флеша - одни единицы.
void erase(uint8_t* page) {
    std::memset(page, 0xFF, kPageBytes);
}

Settings other_than_default() {
    const Settings def;
    Settings s;
    s.disksys             = static_cast<uint8_t>(DiskSys::TrDos);
    s.zcontroller         = static_cast<uint8_t>(Media::Sd);
    s.usb_mouse           = static_cast<uint8_t>(!def.usb_mouse);
    s.usb_gamepad         = static_cast<uint8_t>(!def.usb_gamepad);
    s.reset_signal        = static_cast<uint8_t>(!def.reset_signal);
    s.mouse_speed         = 9;
    s.keyboard_layout     = static_cast<uint8_t>(Layout::Picard);
    s.mod_left_alt        = static_cast<uint8_t>(ModRole::SymbolShift);
    s.mouse_swap_buttons  = 1;
    s.divmmc_card1        = static_cast<uint8_t>(Media::Usb);
    s.auto_volume         = 1;
    s.joy_8bit            = 1;
    s.joy_keys[3][1]      = 0x1a; // W вторым к стрелке вверх
    s.joy_keys[5][0]      = 0x1d; // Z на второй огонь
    s.stop_music_on_reset = static_cast<uint8_t>(!def.stop_music_on_reset);
    s.divmmc_card2        = static_cast<uint8_t>(Media::Sd);
    return s;
}

bool same(const Settings& a, const Settings& b) {
    return std::memcmp(&a, &b, sizeof(Settings)) == 0;
}

void test_store_empty_flash() {
    uint8_t p0[kPageBytes], p1[kPageBytes];
    erase(p0);
    erase(p1);

    CHECK_EQ(page_inspect(p0).valid, false);
    CHECK_EQ(store_pick(p0, p1), -1);

    // Годных блоков нет: первая запись идёт в нулевой, счётчик с единицы.
    const NextWrite first = store_next(p0, p1);
    CHECK_EQ(first.slot, 0);
    CHECK_EQ(first.counter, 1u);
}

void test_store_alternates_and_survives_torn_write() {
    uint8_t p0[kPageBytes], p1[kPageBytes];
    erase(p0);
    erase(p1);

    Settings a;
    a.mouse_speed = 5;
    NextWrite w   = store_next(p0, p1);
    page_build(p0, a, w.counter);

    // Второй записью занимается соседний блок, счётчик на единицу больше.
    Settings b = other_than_default();
    w          = store_next(p0, p1);
    CHECK_EQ(w.slot, 1);
    CHECK_EQ(w.counter, 2u);
    page_build(p1, b, w.counter);

    CHECK_EQ(store_pick(p0, p1), 1);
    Settings got;
    CHECK(page_read(p1, got));
    CHECK(same(got, b));

    // Третья запись возвращается в нулевой: износ ложится на оба.
    w = store_next(p0, p1);
    CHECK_EQ(w.slot, 0);
    CHECK_EQ(w.counter, 3u);

    // Обрыв записи: свежий блок испорчен, прежний цел и берётся он.
    p1[kPayloadOffset + 2u] ^= 0x5Au;
    CHECK_EQ(page_inspect(p1).valid, false);
    CHECK_EQ(store_pick(p0, p1), 0);
    CHECK(page_read(p0, got));
    CHECK(same(got, a));
    // И следующая запись идёт в испорченный, а не в единственный целый.
    w = store_next(p0, p1);
    CHECK_EQ(w.slot, 1);
    CHECK_EQ(w.counter, 2u);
}

void test_store_rejects_foreign_format() {
    uint8_t p0[kPageBytes];
    erase(p0);
    page_build(p0, Settings{}, 7u);
    CHECK(page_inspect(p0).valid);

    // Номер формата поднялся - блок чужой, настройки из него не берутся.
    const uint16_t alien = kFormatVersion + 1u;
    std::memcpy(p0 + offsetof(PageHeader, format), &alien, sizeof(alien));
    CHECK_EQ(page_inspect(p0).valid, false);
}

void test_store_counter_wraps() {
    uint8_t p0[kPageBytes], p1[kPageBytes];
    // Счётчик перевернулся: следующий после 0xFFFFFFFF - ноль, и он свежее.
    page_build(p0, Settings{}, 0xFFFFFFFFu);
    page_build(p1, Settings{}, 0u);
    CHECK_EQ(store_pick(p0, p1), 1);
}

void test_ini_round_trip() {
    const Settings src = other_than_default();
    char text[kIniMaxBytes];
    const uint32_t len = ini_render(src, text, sizeof(text));
    CHECK(len > 0 && len < sizeof(text));

    Settings back;
    const IniResult r = ini_parse(text, len, back);
    CHECK_EQ(r.unknown, 0u);
    CHECK_EQ(r.bad, 0u);
    CHECK(same(back, src));
}

void test_ini_ignores_spaces_and_junk() {
    Settings s;
    const std::string text = "; comment\n"
                             "  DISKSYS = NONE  \n"
                             "\tmouse_speed\t=\t12\n"
                             "DIVMMC_CARD1=usb\n"
                             "MOD_RIGHT_SHIFT=symbol\n"
                             "GS=off\n";
    const IniResult r      = ini_parse(text.c_str(), static_cast<uint32_t>(text.size()), s);
    CHECK_EQ(r.applied, 5u);
    CHECK_EQ(r.unknown, 0u);
    CHECK_EQ(r.bad, 0u);
    CHECK_EQ(s.disksys, static_cast<uint8_t>(DiskSys::None));
    CHECK_EQ(s.mouse_speed, 12);
    CHECK_EQ(s.divmmc_card1, static_cast<uint8_t>(Media::Usb));
    CHECK_EQ(s.gs, 0);
    CHECK_EQ(s.mod_right_shift, static_cast<uint8_t>(ModRole::SymbolShift));
    // Чего в файле нет, осталось умолчанием.
    CHECK_EQ(s.zcontroller, Settings{}.zcontroller);
}

void test_ini_bad_lines_do_not_cancel_the_rest() {
    Settings s;
    const std::string text = "WHAT_IS_THIS=1\n"                // ключ не из таблицы
                             "MOUSE_SPEED=999\n"               // за пределами
                             "USB_MOUSE=maybe\n"               // не признак
                             "a line without an equals sign\n" // молча пропускается
                             "USB_GAMEPAD=0\n";
    const IniResult r = ini_parse(text.c_str(), static_cast<uint32_t>(text.size()), s);
    CHECK_EQ(r.applied, 1u);
    CHECK_EQ(r.unknown, 1u);
    CHECK_EQ(r.bad, 2u);
    CHECK_EQ(s.usb_gamepad, 0);
    CHECK_EQ(s.mouse_speed, kMouseSpeedUnit);
    CHECK_EQ(s.usb_mouse, Settings{}.usb_mouse);
}

// Бипер: 1BIT и 2BIT в любом регистре; прежних EAR и CLASSIC нет.
void test_ini_beeper_names() {
    Settings s;
    const std::string two = "BEEPER=2BIT\n";
    CHECK_EQ(ini_parse(two.c_str(), static_cast<uint32_t>(two.size()), s).applied, 1u);
    CHECK_EQ(s.beeper, static_cast<uint8_t>(Beeper::TwoBit));
    const std::string one = "beeper=1bit\n";
    CHECK_EQ(ini_parse(one.c_str(), static_cast<uint32_t>(one.size()), s).applied, 1u);
    CHECK_EQ(s.beeper, static_cast<uint8_t>(Beeper::OneBit));
    const std::string old = "BEEPER=CLASSIC\nBEEPER=EAR\n";
    CHECK_EQ(ini_parse(old.c_str(), static_cast<uint32_t>(old.size()), s).bad, 2u);
    CHECK_EQ(s.beeper, static_cast<uint8_t>(Beeper::OneBit));
}

void test_ini_render_measures_when_it_does_not_fit() {
    const Settings s;
    const uint32_t need = ini_render(s, nullptr, 0);
    CHECK(need > 0);
    CHECK(need < kIniMaxBytes);
    char small[8] = {};
    CHECK_EQ(ini_render(s, small, sizeof(small)), need);
}

// Настройки из блока приходят от чужой прошивки: значения приводятся к
// допустимым, а не применяются как есть.
void test_keyboard_off_is_a_layout() {
    Settings s;
    s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    CHECK(keyboard_enabled(s));
    const char text[] = "KEYBOARD_LAYOUT=none";
    const IniResult r = ini_parse(text, static_cast<uint32_t>(sizeof(text) - 1u), s);
    CHECK_EQ(r.applied, 1u);
    CHECK(!keyboard_enabled(s));
}

void test_clamp() {
    uint8_t p0[kPageBytes];
    Settings wild;
    wild.mouse_speed     = 200;
    wild.disksys         = 9;
    wild.divmmc_card1    = 5;
    wild.mod_left_ctrl   = 77;
    wild.keyboard_layout = 99;
    page_build(p0, wild, 1u);

    Settings got;
    CHECK(page_read(p0, got));
    CHECK_EQ(got.mouse_speed, kMouseSpeedMax);
    CHECK_EQ(got.disksys, static_cast<uint8_t>(DiskSys::None));
    CHECK_EQ(got.divmmc_card1, static_cast<uint8_t>(Media::None));
    CHECK_EQ(got.mod_left_ctrl, static_cast<uint8_t>(ModRole::None));
    // Чужая раскладка - наша, а не выключенная клавиатура: выключать её
    // из-за чужого номера пользователь не просил.
    CHECK_EQ(got.keyboard_layout, static_cast<uint8_t>(Layout::Picard));

    // Флаги и числа - все по таблице ключей; имя файла ПЗУ - как есть, с
    // байтами кодовой страницы карты, только ноль в конце.
    Settings s;
    s.gs_fx_separate = 7;
    s.mouse_autoswap = 9;
    s.mod_panning    = 250;
    s.mouse_speed    = 0;
    std::memset(s.rom_file[1], 'A', sizeof(s.rom_file[1]));
    s.rom_file[1][3] = static_cast<char>(0x8E);
    settings_clamp(s);
    CHECK_EQ(s.gs_fx_separate, 1);
    CHECK_EQ(s.mouse_autoswap, 1);
    CHECK_EQ(s.mod_panning, kModPanningMax);
    CHECK_EQ(s.mouse_speed, kMouseSpeedMin);
    CHECK_EQ(std::strlen(s.rom_file[1]), sizeof(s.rom_file[1]) - 1);
    CHECK_EQ(static_cast<uint8_t>(s.rom_file[1][3]), 0x8E);
}

// Умолчания - это набор для TS-Config: машина со своей памятью, своим
// диском, своей клавиатурой и мышью получает от платы только звук.
void test_defaults_are_tsconfig() {
    const Settings s;
    CHECK_EQ(s.disksys, static_cast<uint8_t>(DiskSys::None));
    CHECK_EQ(s.zcontroller, static_cast<uint8_t>(Media::None));
    CHECK_EQ(s.divmmc_card1, static_cast<uint8_t>(Media::None));
    CHECK_EQ(s.divmmc_card2, static_cast<uint8_t>(Media::None));
    CHECK_EQ(s.usb_mouse, 0);
    CHECK_EQ(s.usb_gamepad, 0);
    CHECK_EQ(s.reset_signal, 0);
    CHECK(!keyboard_enabled(s));
    CHECK_EQ(s.gs, 1);
    CHECK_EQ(s.live_midi, static_cast<uint8_t>(LiveMidi::Hook));
    CHECK_EQ(s.log, 1);
    CHECK_EQ(s.wifi, 0);
    // Эти два умолчания - требование. Эффекты General Sound отнимают канал
    // у модуля, как настоящая карта: игры на это рассчитаны. Панорама MOD
    // разведена до упора, как на Amiga.
    CHECK_EQ(s.gs_fx_separate, 0);
    CHECK_EQ(s.mod_panning, kModPanningMax);
    // Обмен кнопок мыши жестом выключен: обе кнопки разом нажимают и в
    // играх.
    CHECK_EQ(s.mouse_autoswap, 0);
}

// Блок, записанный прошивкой с меньшим числом полей, обязан остаться
// годным: иначе каждое дописанное поле стирает настройки у всех.
void test_store_reads_shorter_payload() {
    // Три последних поля плюс три байта добивки: хвост, которого в
    // старом блоке нет.
    const uint16_t kOldBytes = static_cast<uint16_t>(kLegacyBytes - 6u);
    Settings old             = other_than_default();
    old.log                  = 0; // хвост старого блока: этих байт в нём нет
    old.wifi                 = 1;
    old.mod_panning          = 50;

    uint8_t page[kPageBytes];
    page_build(page, old, 7u);
    // Укоротить payload и пересчитать сумму - ровно то, что записала бы
    // прошивка, у которой этих полей ещё не было.
    PageHeader h{};
    std::memcpy(&h, page, sizeof(h));
    h.bytes      = kOldBytes;
    uint32_t crc = soundsinth::util::kCrc32Init;
    crc          = soundsinth::util::crc32_update(crc, reinterpret_cast<const uint8_t*>(&h.format), sizeof(h.format));
    crc          = soundsinth::util::crc32_update(crc, reinterpret_cast<const uint8_t*>(&h.bytes), sizeof(h.bytes));
    crc          = soundsinth::util::crc32_update(crc, reinterpret_cast<const uint8_t*>(&h.counter), sizeof(h.counter));
    h.crc        = soundsinth::util::crc32_update(crc, page + kPayloadOffset, h.bytes);
    std::memcpy(page, &h, sizeof(h));

    Settings got;
    CHECK(page_read(page, got));
    // Что в блоке было - прочитано; чего не было - умолчание, а не мусор.
    const Settings def;
    CHECK_EQ(got.disksys, old.disksys);
    CHECK_EQ(got.mouse_speed, old.mouse_speed);
    CHECK_EQ(got.log, def.log);
    CHECK_EQ(got.wifi, def.wifi);
    CHECK_EQ(got.mod_panning, def.mod_panning);
}

// Провод MIDI и Wi-Fi делят единственный вывод приёма.
void test_wire_midi_yields_to_wifi() {
    Settings s;
    s.live_midi = static_cast<uint8_t>(LiveMidi::Wire);
    s.wifi      = 1;
    CHECK(settings_conflict(s));
    settings_clamp(s);
    CHECK_EQ(s.live_midi, static_cast<uint8_t>(LiveMidi::None));
    CHECK_EQ(s.wifi, 1);
    CHECK(!settings_conflict(s));

    // Без Wi-Fi провод остаётся проводом.
    Settings w;
    w.live_midi = static_cast<uint8_t>(LiveMidi::Wire);
    settings_clamp(w);
    CHECK_EQ(w.live_midi, static_cast<uint8_t>(LiveMidi::Wire));
}

// Кто из настроек пользуется USB. Подъёмом стека это больше не
// управляет - он безусловный, - но в журнале по этому признаку видно,
// зачем хост поднят.
void test_usb_host_used_by_every_consumer() {
    Settings off;
    off.keyboard_layout = static_cast<uint8_t>(Layout::None);
    off.usb_mouse       = 0;
    off.usb_gamepad     = 0;
    CHECK(!usb_host_used(off));

    Settings kb        = off;
    kb.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    CHECK(usb_host_used(kb));

    Settings ms  = off;
    ms.usb_mouse = 1;
    CHECK(usb_host_used(ms));

    Settings gp    = off;
    gp.usb_gamepad = 1;
    CHECK(usb_host_used(gp));

    // Флешка нужна, только если её выбрало устройство карты.
    for (uint8_t Settings::*dev : {&Settings::divmmc_card1, &Settings::divmmc_card2, &Settings::zcontroller}) {
        Settings d = off;
        d.*dev     = static_cast<uint8_t>(Media::Sd);
        CHECK(!usb_host_used(d));
        d.*dev = static_cast<uint8_t>(Media::Usb);
        CHECK(usb_host_used(d));
    }
    // DIVMMC без карт включает первую на карте.
    Settings ds = off;
    ds.disksys  = static_cast<uint8_t>(DiskSys::DivMmc);
    settings_clamp(ds);
    CHECK_EQ(ds.divmmc_card1, static_cast<uint8_t>(Media::Sd));
    // Вторая включена - первую DIVMMC не трогает.
    Settings d2     = off;
    d2.disksys      = static_cast<uint8_t>(DiskSys::DivMmc);
    d2.divmmc_card2 = static_cast<uint8_t>(Media::Usb);
    settings_clamp(d2);
    CHECK_EQ(d2.divmmc_card1, static_cast<uint8_t>(Media::None));
    // Без DIVMMC карты могут быть выключены обе.
    Settings none = off;
    none.disksys  = static_cast<uint8_t>(DiskSys::TrDos);
    settings_clamp(none);
    CHECK_EQ(none.divmmc_card1, static_cast<uint8_t>(Media::None));

    // Умолчания: раскладка не выбрана, мышь, джойстик и устройства карты
    // выключены. Машине без файла настроек USB не нужен никому.
    CHECK(!usb_host_used(Settings{}));
}

// --- Поля конфигуратора ---

// Номер поля по указателю на член: тест обязан знать, какой бит маски чей.
uint32_t field_index(uint8_t Settings::*f) {
    Settings mark;
    mark.*f = 0x5A;
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        if (config_field_value(mark, i) == 0x5A && config_field_value(Settings{}, i) != 0x5A) return i;
    }
    CHECK(false);
    return 0;
}

// Все поля по номеру переносят настройки целиком: иначе правка в меню
// теряла бы поля.
void test_fields_round_trip() {
    const Settings want = other_than_default();
    Settings got;
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        char* t = config_field_text(got, i);
        if (t != nullptr) {
            std::strcpy(t, config_field_text(want, i));
        } else {
            config_field_set(got, i, config_field_value(want, i));
        }
    }
    CHECK_EQ(std::memcmp(&got, &want, sizeof(Settings)), 0);
    std::printf("  configurator fields %u\n", static_cast<unsigned>(kPageFieldCount));
}

// Неактивные пункты: зависят от других полей, и правила считает плата.
void test_page_mask_follows_dependencies() {
    const uint32_t i_reset = field_index(&Settings::reset_signal);
    const uint32_t i_speed = field_index(&Settings::mouse_speed);

    Settings off;
    off.disksys   = static_cast<uint8_t>(DiskSys::None);
    off.usb_mouse = 0;
    settings_clamp(off);
    const uint64_t m_off = settings_active_mask(off);
    CHECK((m_off & (uint64_t{1} << i_speed)) == 0);
    // Сброс при выключенном DivMMC - на выбор пользователя.
    CHECK((m_off & (uint64_t{1} << i_reset)) != 0);

    Settings on;
    on.disksys      = static_cast<uint8_t>(DiskSys::DivMmc);
    on.usb_mouse    = 1;
    on.reset_signal = 0;
    settings_clamp(on);
    // DivMMC грузит машину со своей карты и обязан её сбросить.
    CHECK_EQ(on.reset_signal, 1);
    const uint64_t m_on = settings_active_mask(on);
    CHECK((m_on & (uint64_t{1} << i_speed)) != 0);
    CHECK((m_on & (uint64_t{1} << i_reset)) == 0);

    // Файлы ПЗУ - вместе с полем ROM, а не с Wi-Fi: при DIVMMC выбора ПЗУ
    // нет, без DivMMC он есть и при WIFI=0.
    for (uint32_t k = 0; k < kTextKeyCount; ++k) {
        const uint64_t bit = uint64_t{1} << (kTextFieldFirst + k);
        CHECK(((m_off & bit) != 0) == kTextKeys[k].file);
        CHECK((m_on & bit) == 0);
    }
}

constexpr uint32_t kExtAt = kPayloadOffset + kLegacyBytes;

// Расширение с нужными записями поверх собранного блока: заголовок с
// суммой и записи следом.
void put_records(uint8_t* page, const uint8_t* records, uint16_t bytes) {
    std::memcpy(page + kExtAt + sizeof(ExtHeader), records, bytes);
    ExtHeader e{};
    e.magic = kExtMagic;
    e.bytes = bytes;
    e.crc   = soundsinth::util::crc32_update(soundsinth::util::kCrc32Init, page + kExtAt + sizeof(ExtHeader), bytes);
    std::memcpy(page + kExtAt, &e, sizeof(e));
}

// Блок прошивки без записей: неподвижная часть читается, поля записей
// остаются умолчаниями.
void test_store_block_without_records() {
    const Settings src = other_than_default();
    uint8_t page[kPageBytes];
    page_build(page, src, 3u);
    std::memset(page + kExtAt, 0xFF, kPageBytes - kExtAt);

    Settings got;
    CHECK(page_read(page, got));
    const Settings def;
    CHECK(std::memcmp(&got, &src, kLegacyBytes) == 0);
    CHECK_EQ(got.joy_8bit, def.joy_8bit);
    CHECK(std::memcmp(got.joy_keys, def.joy_keys, sizeof(def.joy_keys)) == 0);
}

// Незнакомая запись пропускается, короткая ложится началом поверх
// умолчаний.
void test_store_records_unknown_and_short() {
    uint8_t page[kPageBytes];
    page_build(page, Settings{}, 3u);
    const uint8_t records[] = {
        99, 0, 3, 0, 0xAA, 0xBB, 0xCC, // номер 99, три байта
        2,  0, 2, 0, 0x07, 0x09,       // клавиши джойстика: только первая строка
        1,  0, 1, 0, 1,                // JOY_8BIT
    };
    put_records(page, records, sizeof(records));

    Settings got;
    CHECK(page_read(page, got));
    const Settings def;
    CHECK_EQ(got.joy_keys[0][0], 0x07);
    CHECK_EQ(got.joy_keys[0][1], 0x09);
    CHECK(std::memcmp(got.joy_keys[1], def.joy_keys[1], sizeof(def.joy_keys) - sizeof(def.joy_keys[0])) == 0);
    CHECK_EQ(got.joy_8bit, 1);
}

void test_store_records_bad_crc_keeps_fixed_part() {
    const Settings src = other_than_default();
    uint8_t page[kPageBytes];
    page_build(page, src, 3u);
    page[kExtAt + sizeof(ExtHeader) + 4u] ^= 0x5Au;

    Settings got;
    CHECK(page_read(page, got));
    CHECK(std::memcmp(&got, &src, kLegacyBytes) == 0);
    CHECK_EQ(got.joy_8bit, Settings{}.joy_8bit);
}

// Блок прежней прошивки: устройств карты в записях нет, выбор выводится из
// неподвижной части - носитель DivMMC при DISKSYS=DIVMMC, флаг и носитель
// Z-Controller. Вторая карта выключена.
void test_store_legacy_devices() {
    uint8_t page[kPageBytes];
    Settings old;
    old.disksys                  = static_cast<uint8_t>(DiskSys::DivMmc);
    old.legacy_divmmc_media      = static_cast<uint8_t>(Media::Usb);
    old.legacy_zcontroller       = 1;
    old.legacy_zcontroller_media = static_cast<uint8_t>(Media::Usb);
    page_build(page, old, 3u);
    std::memset(page + kExtAt, 0xFF, kPageBytes - kExtAt);
    Settings got;
    CHECK(page_read(page, got));
    CHECK_EQ(got.divmmc_card1, static_cast<uint8_t>(Media::Usb));
    CHECK_EQ(got.divmmc_card2, static_cast<uint8_t>(Media::None));
    CHECK_EQ(got.zcontroller, static_cast<uint8_t>(Media::Usb));

    // Без DIVMMC носитель DivMMC прежде не значил ничего: карта выключена.
    old.disksys            = static_cast<uint8_t>(DiskSys::TrDos);
    old.legacy_zcontroller = 0;
    page_build(page, old, 4u);
    std::memset(page + kExtAt, 0xFF, kPageBytes - kExtAt);
    CHECK(page_read(page, got));
    CHECK_EQ(got.divmmc_card1, static_cast<uint8_t>(Media::None));
    CHECK_EQ(got.zcontroller, static_cast<uint8_t>(Media::None));

    // Записи есть - они и действуют, старые байты не трогают.
    Settings now;
    now.legacy_zcontroller = 1;
    now.zcontroller        = static_cast<uint8_t>(Media::None);
    page_build(page, now, 5u);
    CHECK(page_read(page, got));
    CHECK_EQ(got.zcontroller, static_cast<uint8_t>(Media::None));
}

void test_ini_joy_keys() {
    const char text[] = "JOY_UP = w, kp_8\n"
                        "JOY_FIRE=ESC\n"
                        "JOY_FIRE2=A,B,C\n"
                        "JOY_LEFT=NONE\n"
                        "JOY_FIRE3=F1\n";
    Settings s;
    const IniResult r = ini_parse(text, sizeof(text) - 1u, s);
    CHECK_EQ(r.applied, 3u);
    CHECK_EQ(r.bad, 2u);
    CHECK_EQ(s.joy_keys[3][0], 0x1a);
    CHECK_EQ(s.joy_keys[3][1], 0x60);
    CHECK_EQ(s.joy_keys[1][0], 0);
    CHECK_EQ(s.joy_keys[1][1], 0);
    CHECK_EQ(s.joy_keys[6][0], 0x3a);
    // Esc и три клавиши на действие - отказ, строка прежняя.
    const Settings def;
    CHECK_EQ(s.joy_keys[4][0], def.joy_keys[4][0]);
    CHECK_EQ(s.joy_keys[5][0], def.joy_keys[5][0]);
}

// Esc и забой заняты отменой: числом из блока или со страницы они не проходят.
void test_clamp_joy_keys() {
    Settings s;
    s.joy_keys[0][0] = kKeyEsc;
    s.joy_keys[0][1] = 0x02;
    s.joy_keys[1][0] = 0xE0;
    s.joy_keys[2][1] = 0x63;
    s.joy_keys[3][1] = kKeyBackspace;
    settings_clamp(s);
    CHECK_EQ(s.joy_keys[3][1], 0);
    CHECK_EQ(s.joy_keys[0][0], 0);
    CHECK_EQ(s.joy_keys[0][1], 0);
    CHECK_EQ(s.joy_keys[1][0], 0);
    CHECK_EQ(s.joy_keys[2][1], 0x63);
}

// Привязки: поля вида клавиша за ключами таблицы, по клавише действия на
// поле. Кнопки 2..4 правятся только при восьмиразрядном Kempston, все
// привязки - только при включённой клавиатуре.
void test_joy_key_fields() {
    Settings s;
    s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    s.joy_keys[3][1]  = 0x1a; // W вторым к вверх
    CHECK_EQ(kPageFieldCount, 55u);
    const uint32_t first   = kKeyCount;
    const PageFieldInfo fi = config_field_info(first + 7u);
    CHECK(fi.page_kind == PageKind::Key);
    CHECK_EQ(std::strcmp(fi.name, "JOY_UP 2"), 0);
    CHECK_EQ(config_field_value(s, first + 7u), 0x1a);

    // Правка: Q на второй огонь, Esc на огонь - Esc отсекается.
    Settings got = s;
    config_field_set(got, first + 10u, 0x14);
    config_field_set(got, first + 8u, kKeyEsc);
    settings_clamp(got);
    CHECK_EQ(got.joy_keys[5][0], 0x14);
    CHECK_EQ(got.joy_keys[4][0], 0);

    const uint64_t narrow = settings_active_mask(s);
    CHECK((narrow & (uint64_t{1} << (first + 8u))) != 0);  // огонь
    CHECK((narrow & (uint64_t{1} << (first + 10u))) == 0); // второй огонь
    s.joy_8bit = 1;
    CHECK((settings_active_mask(s) & (uint64_t{1} << (first + 10u))) != 0);
    s.keyboard_layout = static_cast<uint8_t>(Layout::None);
    CHECK((settings_active_mask(s) & (uint64_t{1} << first)) == 0);
}

} // namespace

// Сеть Wi-Fi: строки в файле, в странице конфигуратора и во флеше.
void test_wifi_texts() {
    Settings s;
    s.wifi = 1;
    // Файл: всё после "=" без пробелов по краям, регистр и пробелы внутри
    // сохраняются; длиннее предела - отказ.
    const char* ini = "WIFI_SSID = My Home Net \nwifi_password=p@ss W0rd;#\nWIFI_SSID_X=1\n";
    IniResult r     = ini_parse(ini, static_cast<uint32_t>(std::strlen(ini)), s);
    CHECK_EQ(r.applied, 2u);
    CHECK_EQ(std::string(s.wifi_ssid), std::string("My Home Net"));
    CHECK_EQ(std::string(s.wifi_pass), std::string("p@ss W0rd;#"));
    const std::string long_ssid = "WIFI_SSID=" + std::string(33, 'x') + "\n";
    r                           = ini_parse(long_ssid.c_str(), static_cast<uint32_t>(long_ssid.size()), s);
    CHECK_EQ(r.bad, 1u);
    CHECK_EQ(std::string(s.wifi_ssid), std::string("My Home Net"));

    // Выгрузка: имя сети пишется, пароль - нет.
    static char out[16384];
    ini_render(s, out, sizeof(out));
    const std::string text(out);
    CHECK(text.find("\nWIFI_SSID=My Home Net\n") != std::string::npos);
    CHECK(text.find("p@ss") == std::string::npos);
    CHECK(text.find("; WIFI_PASSWORD= (set, not written)") != std::string::npos);

    // Флеш: строки переживают запись и чтение.
    static uint8_t block[kPageBytes];
    page_build(block, s, 1);
    Settings back;
    CHECK(page_read(block, back));
    CHECK_EQ(std::string(back.wifi_pass), std::string("p@ss W0rd;#"));

    // Поля: текст, пароль - секрет; значение - длина; без Wi-Fi поля
    // недоступны.
    const uint32_t first     = kTextFieldFirst;
    const PageFieldInfo ssid = config_field_info(first);
    const PageFieldInfo pass = config_field_info(first + 1u);
    CHECK(ssid.page_kind == PageKind::Text);
    CHECK_EQ(ssid.max, 32u);
    CHECK_EQ(ssid.min, 0u);
    CHECK_EQ(pass.min, 1u);
    CHECK_EQ(std::string(config_field_text(s, first)), std::string("My Home Net"));
    CHECK_EQ(config_field_value(s, first), 11u);
    CHECK((settings_active_mask(s) & (uint64_t{1} << first)) != 0);
    Settings off = s;
    off.wifi     = 0;
    CHECK((settings_active_mask(off) & (uint64_t{1} << first)) == 0);
    Settings got = s;
    std::strcpy(config_field_text(got, first), "Cafe");
    CHECK_EQ(config_field_value(got, first), 4u);
    CHECK_EQ(std::string(got.wifi_pass), std::string("p@ss W0rd;#"));
}

// Своё ПЗУ: имя в файле, незнакомая область - ПЗУ машины, при DivMMC поле
// неактивно и область не выбрана.
void test_custom_rom() {
    Settings s;
    const std::string text = "DISKSYS=TRDOS\nROM=SLOT2\n";
    const IniResult r      = ini_parse(text.c_str(), static_cast<uint32_t>(text.size()), s);
    CHECK_EQ(r.applied, 2u);
    CHECK_EQ(s.rom, static_cast<uint8_t>(RomSource::Slot2));
    CHECK_EQ(custom_rom_slot(s), 2u);
    const uint32_t i_rom = field_index(&Settings::rom);
    CHECK((settings_active_mask(s) & (uint64_t{1} << i_rom)) != 0);

    s.disksys = static_cast<uint8_t>(DiskSys::DivMmc);
    CHECK_EQ(custom_rom_slot(s), 0u);
    CHECK((settings_active_mask(s) & (uint64_t{1} << i_rom)) == 0);

    s.rom = 9;
    settings_clamp(s);
    CHECK_EQ(s.rom, static_cast<uint8_t>(RomSource::Board));

    // TR-DOS (Beta Disk платы): своё ПЗУ разрешено; последнее значение
    // переживает приведение, следующее за ним - уже ни одной системы. BDI -
    // не режим, а часть TRDOS.
    Settings b;
    const std::string bt = "DISKSYS=TRDOS\nROM=SLOT1\nDISKSYS=BDI\n";
    CHECK_EQ(ini_parse(bt.c_str(), static_cast<uint32_t>(bt.size()), b).applied, 2u);
    CHECK_EQ(b.disksys, static_cast<uint8_t>(DiskSys::TrDos));
    CHECK_EQ(custom_rom_slot(b), 1u);
    CHECK(!usb_medium_needed(b));
    settings_clamp(b);
    CHECK_EQ(b.disksys, static_cast<uint8_t>(DiskSys::TrDos));
    b.disksys = static_cast<uint8_t>(DiskSys::TrDos) + 1u;
    settings_clamp(b);
    CHECK_EQ(b.disksys, static_cast<uint8_t>(DiskSys::None));
    // Каждое имя таблицы разбирается в своё значение.
    for (const Named& n : kDiskSysNames) {
        Settings v;
        const std::string line = std::string("DISKSYS=") + n.name + "\n";
        CHECK_EQ(ini_parse(line.c_str(), static_cast<uint32_t>(line.size()), v).applied, 1u);
        CHECK_EQ(v.disksys, n.value);
    }
}

// Меню конфигуратора: каждое поле показано ровно одним пунктом, надписи и
// подсказки влезают в экран, переходы ведут на существующие экраны.
void test_menu_tree() {
    uint8_t shown[64]         = {};
    uint8_t tools[kToolCount] = {};
    for (uint32_t m = 0; m < kMenuCount; ++m) {
        const MenuDesc& d = kMenus[m];
        CHECK(d.count + (m == kMenuMain ? kMenuMainTop : kMenuSubTop) <= kMenuRowsMax);
        // Подменю начинается пунктом назад.
        if (m != kMenuMain) CHECK(d.items[0].type == MenuItemType::Back);
        CHECK(d.title != nullptr);
        for (uint32_t n = 0; n < d.count; ++n) {
            const MenuItemDesc& it = d.items[n];
            const uint8_t field    = it.key == nullptr ? kNoField : config_field_by_name(it.key);
            if (it.key != nullptr) CHECK(field != kNoField);
            if (it.type != MenuItemType::Blank && it.type != MenuItemType::Header) {
                CHECK(it.label != nullptr && std::strlen(it.label) <= kMenuLabelMax);
                CHECK(it.help != nullptr && std::strlen(it.help) <= kMenuHelpMax);
            }
            if (it.type == MenuItemType::Submenu) CHECK(it.menu < kMenuCount && it.menu != m);
            if (it.type == MenuItemType::Tool) {
                CHECK(it.menu != 0 && it.menu < kToolCount);
                CHECK_EQ(field, kNoField);
                if (it.menu < kToolCount) ++tools[it.menu];
            }
            if (it.type == MenuItemType::Field && field < kPageFieldCount) ++shown[field];
            if (it.type == MenuItemType::KeyRow && field + 1u < kPageFieldCount) {
                ++shown[field];
                ++shown[field + 1u];
            }
        }
    }
    for (uint32_t i = 0; i < kPageFieldCount; ++i) {
        CHECK_EQ(shown[i], 1u);
    }
    // Каждый инструмент - ровно одним пунктом.
    for (uint32_t t = 1; t < kToolCount; ++t) {
        CHECK_EQ(tools[t], 1u);
    }
    // В списке выбора ПЗУ область 2 названа своим файлом, область 1 - номером.
    Settings s;
    std::strcpy(s.rom_file[1], "QUORUM.ROM");
    CHECK_EQ(std::strcmp(config_choice_name(s, Kind::Rom, 1), "SLOT1"), 0);
    CHECK_EQ(std::strcmp(config_choice_name(s, Kind::Rom, 2), "QUORUM.ROM"), 0);
}

// Экран инструмента: кольцо нажатий, строки добиты пробелами, счётчик
// обновлений растёт.
void test_tool_screen() {
    static uint8_t page[kConfigPageBytes];
    tool_area_reset(page);
    ToolScreen scr(page);
    ToolArea& a = scr.area();
    CHECK_EQ(a.cur_row, kToolNoCursor);
    CHECK_EQ(a.text[0][0], ' ');
    uint8_t k = 0;
    CHECK(!scr.key(k, ToolId::CardInfo));
    // ПЗУ кладёт два нажатия.
    a.tool    = static_cast<uint8_t>(ToolId::CardInfo);
    a.keys[0] = kToolKeyOpen;
    a.keys[1] = kToolKeyDown;
    a.req     = 2;
    CHECK(scr.tool() == ToolId::CardInfo);
    // Чужой потребитель нажатий открытого экрана не трогает.
    CHECK(!scr.key(k, ToolId::AtTerminal));
    CHECK_EQ(a.ack, 0u);
    CHECK(scr.key(k, ToolId::CardInfo));
    CHECK_EQ(k, kToolKeyOpen);
    CHECK(scr.key(k, ToolId::CardInfo));
    CHECK_EQ(k, kToolKeyDown);
    CHECK(!scr.key(k, ToolId::CardInfo));
    CHECK_EQ(a.ack, 2u);

    const uint8_t upd = a.upd;
    scr.putf(3, kToolYellow, "FREE %u", 12u);
    scr.publish();
    CHECK_EQ(a.upd, static_cast<uint8_t>(upd + 1u));
    CHECK(std::memcmp(a.text[3], " FREE 12 ", 9) == 0);
    CHECK_EQ(a.text[3][kToolCols - 1u], ' ');
    CHECK_EQ(a.attr[3], kToolYellow);
    // Непечатное заменяется, длинное обрезается по ширине.
    const char odd[] = {'A', 1, 'B', 0};
    scr.put(4, odd);
    CHECK(std::memcmp(a.text[4], " A?B", 4) == 0);
    scr.put(5, "0123456789012345678901234567890123456789");
    CHECK_EQ(a.text[5][kToolTextCols], '7');
    CHECK_EQ(a.text[5][kToolCols - 1u], ' ');
    CHECK_EQ(a.text[6][0], ' ');
    scr.progress(7, 50, 100);
    CHECK(std::memcmp(a.text[7], " [##########..........]  50%", 28) == 0);

    char size[16];
    tool_size_text(size, sizeof(size), 7948206080ull);
    CHECK_EQ(std::string(size), std::string("7.4 GB"));
    tool_size_text(size, sizeof(size), 32768u);
    CHECK_EQ(std::string(size), std::string("32 KB"));
    // Кластер FAT12 флешки - 512 Б, не "0 KB".
    tool_size_text(size, sizeof(size), 512u);
    CHECK_EQ(std::string(size), std::string("512 B"));
    tool_size_text(size, sizeof(size), 1023u);
    CHECK_EQ(std::string(size), std::string("1023 B"));
    tool_size_text(size, sizeof(size), 1024u);
    CHECK_EQ(std::string(size), std::string("1 KB"));
}

// Терминал AT: заглавные вне кавычек, готовые команды стрелками, перенос
// длинного ответа, отправленное - строкой своим цветом.
void test_at_terminal() {
    static uint8_t page[kConfigPageBytes];
    tool_area_reset(page);
    ToolScreen scr(page);
    AtTerminal t;
    t.reset();
    for (const char c : std::string("at+cwjap=\"Home\",\"pw\"")) {
        CHECK(!t.key(static_cast<uint8_t>(c)));
    }
    CHECK(t.key(kToolKeyEnter));
    CHECK_EQ(std::string(t.command()), std::string("AT+CWJAP=\"Home\",\"pw\""));
    CHECK(!t.key(kToolKeyEnter)); // пустая строка не уходит
    CHECK(!t.key(kToolKeyDown));
    CHECK(t.key(kToolKeyEnter));
    CHECK_EQ(std::string(t.command()), std::string("AT"));
    CHECK(!t.key(kToolKeyUp));
    CHECK(!t.key(kToolKeyDel));
    CHECK(t.key(kToolKeyEnter));
    CHECK_EQ(std::string(t.command()), std::string("AT+RESTOR"));

    const char reply[] = "OK\r\n\r\n+CWLAP:(3,\"A very long network name here\",-67)\r\n";
    t.feed(reinterpret_cast<const uint8_t*>(reply), sizeof(reply) - 1u);
    t.render(scr);
    ToolArea& a    = scr.area();
    const auto row = [&](uint32_t i) { return std::string(a.text[i] + 1, kToolTextCols); };
    CHECK_EQ(row(AtTerminal::kOutLines - 1u).substr(0, 18), std::string("rk name here\",-67)"));
    CHECK_EQ(row(AtTerminal::kOutLines - 2u), std::string("+CWLAP:(3,\"A very long netwo"));
    CHECK_EQ(row(AtTerminal::kOutLines - 3u).substr(0, 3), std::string("OK "));
    CHECK_EQ(a.attr[AtTerminal::kOutLines - 4u], kToolCyan);
    CHECK_EQ(a.cur_row, AtTerminal::kInputLine);
    CHECK_EQ(a.cur_col, 3u);
}

// Раскладку страницы ПЗУ конфигуратора читает по ручной копии
// frontend/config_rom/page.inc: каждое имя оттуда сверяется с платой.
// Новое имя без строки здесь - тоже отказ.
// Экраны TOOLS на стороне Core1 без носителей: USB на ПК нет, флешки нет.
// Проверяется, когда экран публикуется и что с ним делают клавиши.
void test_tools_serve() {
    using player::config::tools_serve;
    static uint8_t page[kConfigPageBytes];
    tool_area_reset(page);
    ToolScreen scr(page);
    ToolArea& a      = scr.area();
    const auto press = [&](uint8_t k) {
        a.keys[a.req % kToolKeySlots] = k;
        a.req                         = static_cast<uint8_t>(a.req + 1u);
    };
    const auto open = [&](ToolId t) {
        a.tool = static_cast<uint8_t>(t);
        press(kToolKeyOpen);
    };
    const auto line = [&](uint32_t i) { return std::string(a.text[i] + 1, kToolTextCols); };

    // Памяти нет: на открытие - строка отказа, экран готов.
    soundsinth::memory::Arena tiny;
    static uint8_t tiny_buf[64];
    tiny.base     = tiny_buf;
    tiny.capacity = sizeof(tiny_buf);
    tiny.floor    = sizeof(tiny_buf);
    soundsinth::memory::TrackScratch no_mem{&tiny};
    open(ToolId::BoardInfo);
    uint8_t upd = a.upd;
    tools_serve(page, no_mem);
    CHECK_EQ(a.upd, static_cast<uint8_t>(upd + 1u));
    CHECK(scr.state() == ToolState::Ready);
    CHECK_EQ(line(0).substr(0, 23), std::string("NO MEMORY FOR THE TOOLS"));
    CHECK_EQ(a.attr[0], kToolAlert);
    // Без нажатий - ничего.
    upd = a.upd;
    tools_serve(page, no_mem);
    CHECK_EQ(a.upd, upd);

    static uint8_t pool[16384];
    soundsinth::memory::Arena arena;
    arena.base     = pool;
    arena.capacity = sizeof(pool);
    arena.floor    = sizeof(pool);
    soundsinth::memory::TrackScratch scratch{&arena};
    const auto serve = [&]() {
        upd = a.upd;
        tools_serve(page, scratch);
        return static_cast<uint32_t>(static_cast<uint8_t>(a.upd - upd));
    };

    // Сведения о плате: кадр на открытие, затем раз в секунду.
    open(ToolId::BoardInfo);
    CHECK_EQ(serve(), 1u);
    CHECK(scr.state() == ToolState::Ready);
    g_test_time_us += 500000u;
    CHECK_EQ(serve(), 0u);
    g_test_time_us += 500000u;
    CHECK_EQ(serve(), 1u);
    CHECK_EQ(serve(), 0u);
    // Забой закрывает экран с этой стороны: Closed и кадр.
    press(kToolKeyDel);
    CHECK_EQ(serve(), 1u);
    CHECK(scr.state() == ToolState::Closed);
    g_test_time_us += 2000000u;
    CHECK_EQ(serve(), 0u);
    // Забой и Enter без открытого экрана не делают ничего.
    press(kToolKeyDel);
    press(kToolKeyEnter);
    CHECK_EQ(serve(), 0u);
    CHECK(scr.state() == ToolState::Closed);

    // Список USB: обновляется раз в секунду, закрытие от ПЗУ кадра не даёт.
    open(ToolId::UsbInfo);
    CHECK_EQ(serve(), 1u);
    CHECK(scr.state() == ToolState::Ready);
    g_test_time_us += 1000000u;
    CHECK_EQ(serve(), 1u);
    press(kToolKeyClose);
    CHECK_EQ(serve(), 0u);
    g_test_time_us += 1000000u;
    CHECK_EQ(serve(), 0u);

    // Проверка флешки без флешки: кадр отказа, дальше тишина; Enter и ESC
    // ничего не меняют, экран остаётся открытым.
    open(ToolId::UsbCheck);
    CHECK_EQ(serve(), 1u);
    CHECK(scr.state() == ToolState::Ready);
    CHECK_EQ(line(0).substr(0, 11), std::string("NO USB DISK"));
    g_test_time_us += 300000u;
    CHECK_EQ(serve(), 0u);
    press(kToolKeyEnter);
    press(kToolKeyEsc);
    CHECK_EQ(serve(), 0u);
    CHECK(scr.state() == ToolState::Ready);
    press(kToolKeyDel);
    CHECK_EQ(serve(), 1u);
    CHECK(scr.state() == ToolState::Closed);

    // USB HID без устройств: кадр на открытие, без новых отчётов - тишина.
    open(ToolId::UsbHid);
    CHECK_EQ(serve(), 1u);
    CHECK_EQ(line(0).substr(0, 18), std::string("NO USB HID DEVICES"));
    g_test_time_us += 150000u;
    CHECK_EQ(serve(), 0u);
    press(kToolKeyClose);
    CHECK_EQ(serve(), 0u);

    // Экран терминала ведёт Core0: здесь он не обслуживается.
    open(ToolId::AtTerminal);
    CHECK_EQ(serve(), 0u);
    a.tool = static_cast<uint8_t>(ToolId::None);
    tool_area_reset(page);
}

void test_page_inc_matches_board() {
    std::printf("test_page_inc_matches_board\n");
    FILE* f = std::fopen("frontend/config_rom/page.inc", "r");
    if (f == nullptr) {
        std::printf("  SKIP (file not found): frontend/config_rom/page.inc\n");
        return;
    }
    const struct {
        const char* name;
        uint32_t value;
    } want[] = {
        {"PAGE", 0x2000},
        {"TM_UPD", offsetof(TermHeader, upd)},
        {"TM_SHOWN", offsetof(TermHeader, shown)},
        {"TM_REQ", offsetof(TermHeader, req)},
        {"TM_ACK", offsetof(TermHeader, ack)},
        {"TM_COUNT", offsetof(TermHeader, count)},
        {"TM_KEYS", offsetof(TermHeader, keys)},
        {"TM_SPANS", sizeof(TermHeader)},
        {"TM_CHARS", 0x2000u + kTermCharsAt},
        {"TM_ATTRS", 0x2000u + kTermAttrsAt},
        {"TM_STARTS", 0x2000u + kTermStartsAt},
        {"TM_KEY_SLOTS", kTermKeySlots},
        {"TM_ALL", kTermAll},
    };
    char line[160];
    uint32_t seen = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char name[48], expr[64];
        if (std::sscanf(line, " %47[A-Z0-9_] .equ %63[^;\r\n]", name, expr) != 2) continue;
        // Значение - число или "PAGE + число".
        const char* plus = std::strchr(expr, '+');
        const uint32_t v =
            plus != nullptr ? 0x2000u + static_cast<uint32_t>(std::strtoul(plus + 1, nullptr, 0)) : static_cast<uint32_t>(std::strtoul(expr, nullptr, 0));
        bool known = false;
        for (const auto& w : want) {
            if (std::strcmp(w.name, name) != 0) continue;
            known = true;
            if (v != w.value) std::printf("  %s: page.inc %u, board %u\n", name, static_cast<unsigned>(v), static_cast<unsigned>(w.value));
            CHECK_EQ(v, w.value);
        }
        if (!known) std::printf("  %s: no board value in the test\n", name);
        CHECK(known);
        ++seen;
    }
    std::fclose(f);
    CHECK_EQ(seen, static_cast<uint32_t>(sizeof(want) / sizeof(want[0])));
}

void run_config_tests() {
    std::printf("test_config\n");
    test_wifi_texts();
    test_store_empty_flash();
    test_store_alternates_and_survives_torn_write();
    test_store_rejects_foreign_format();
    test_store_counter_wraps();
    test_ini_round_trip();
    test_ini_ignores_spaces_and_junk();
    test_ini_bad_lines_do_not_cancel_the_rest();
    test_ini_render_measures_when_it_does_not_fit();
    test_ini_beeper_names();
    test_keyboard_off_is_a_layout();
    test_defaults_are_tsconfig();
    test_clamp();
    test_store_reads_shorter_payload();
    test_wire_midi_yields_to_wifi();
    test_usb_host_used_by_every_consumer();
    test_fields_round_trip();
    test_page_mask_follows_dependencies();
    test_store_block_without_records();
    test_store_records_unknown_and_short();
    test_store_records_bad_crc_keeps_fixed_part();
    test_store_legacy_devices();
    test_ini_joy_keys();
    test_clamp_joy_keys();
    test_joy_key_fields();
    test_custom_rom();
    test_menu_tree();
    test_tool_screen();
    test_at_terminal();
    test_tools_serve();
    test_page_inc_matches_board();

    char text[kIniMaxBytes];
    const uint32_t len = ini_render(Settings{}, text, sizeof(text));
    std::printf("  default settings file: %u B, keys %u\n", static_cast<unsigned>(len), static_cast<unsigned>(sizeof(Settings)));
}
