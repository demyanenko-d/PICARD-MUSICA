// SPDX-License-Identifier: MIT
// Настройки платы: два блока во флеше и текстовый файл с карты.
//
// Проверяется то, ради чего механизм и сделан: обрыв записи не уносит
// прежние настройки, износ ложится на оба блока по очереди, а файл с
// опечаткой применяется в остальной части.

#include "testing.h"

#include <cstring>
#include <string>

#include "core/config/config.h"
#include "core/config/config_ini.h"
#include "core/config/config_page.h"
#include "core/config/config_store.h"
#include "core/util/crc32.h"

using namespace soundsinth::config;

namespace {

// Стёртый блок флеша - одни единицы.
void erase(uint8_t* page) {
    std::memset(page, 0xFF, kPageBytes);
}

Settings other_than_default() {
    const Settings def;
    Settings s;
    s.disksys            = static_cast<uint8_t>(DiskSys::TrDos);
    s.zcontroller        = static_cast<uint8_t>(!def.zcontroller);
    s.usb_mouse          = static_cast<uint8_t>(!def.usb_mouse);
    s.usb_gamepad        = static_cast<uint8_t>(!def.usb_gamepad);
    s.reset_signal       = static_cast<uint8_t>(!def.reset_signal);
    s.mouse_speed        = 9;
    s.keyboard_layout    = static_cast<uint8_t>(Layout::Picard);
    s.mod_left_alt       = static_cast<uint8_t>(ModRole::SymbolShift);
    s.mouse_swap_buttons = 1;
    s.disksys_media      = static_cast<uint8_t>(Media::Usb);
    s.auto_volume        = 1;
    s.joy_8bit           = 1;
    s.joy_keys[3][1]     = 0x1a; // W вторым к стрелке вверх
    s.joy_keys[5][0]     = 0x1d; // Z на второй огонь
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
                             "DISKSYS_MEDIA=usb\n"
                             "MOD_RIGHT_SHIFT=symbol\n"
                             "GS=off\n";
    const IniResult r      = ini_parse(text.c_str(), static_cast<uint32_t>(text.size()), s);
    CHECK_EQ(r.applied, 5u);
    CHECK_EQ(r.unknown, 0u);
    CHECK_EQ(r.bad, 0u);
    CHECK_EQ(s.disksys, static_cast<uint8_t>(DiskSys::None));
    CHECK_EQ(s.mouse_speed, 12);
    CHECK_EQ(s.disksys_media, static_cast<uint8_t>(Media::Usb));
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
    wild.disksys_media   = 5;
    wild.mod_left_ctrl   = 77;
    wild.keyboard_layout = 99;
    page_build(p0, wild, 1u);

    Settings got;
    CHECK(page_read(p0, got));
    CHECK_EQ(got.mouse_speed, kMouseSpeedMax);
    CHECK_EQ(got.disksys, static_cast<uint8_t>(DiskSys::None));
    CHECK_EQ(got.disksys_media, static_cast<uint8_t>(Media::Sd));
    CHECK_EQ(got.mod_left_ctrl, static_cast<uint8_t>(ModRole::None));
    // Чужая раскладка - наша, а не выключенная клавиатура: выключать её
    // из-за чужого номера пользователь не просил.
    CHECK_EQ(got.keyboard_layout, static_cast<uint8_t>(Layout::Picard));
}

// Умолчания - это набор для TS-Config: машина со своей памятью, своим
// диском, своей клавиатурой и мышью получает от платы только звук.
void test_defaults_are_tsconfig() {
    const Settings s;
    CHECK_EQ(s.disksys, static_cast<uint8_t>(DiskSys::None));
    CHECK_EQ(s.zcontroller, 0);
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
    off.keyboard_layout   = static_cast<uint8_t>(Layout::None);
    off.usb_mouse         = 0;
    off.usb_gamepad       = 0;
    off.disksys_media     = static_cast<uint8_t>(Media::Sd);
    off.zcontroller_media = static_cast<uint8_t>(Media::Sd);
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

    // Носитель USB нужен, только если его берёт включённый эмулятор.
    Settings ds      = off;
    ds.disksys_media = static_cast<uint8_t>(Media::Usb);
    ds.disksys       = static_cast<uint8_t>(DiskSys::None);
    CHECK(!usb_host_used(ds));
    ds.disksys = static_cast<uint8_t>(DiskSys::DivMmc);
    CHECK(usb_host_used(ds));

    Settings zc          = off;
    zc.zcontroller_media = static_cast<uint8_t>(Media::Usb);
    zc.zcontroller       = 0;
    CHECK(!usb_host_used(zc));
    zc.zcontroller = 1;
    CHECK(usb_host_used(zc));

    // Умолчания: раскладка не выбрана, мышь и джойстик выключены, оба
    // носителя - карта. Машине без файла настроек USB не нужен никому.
    CHECK(!usb_host_used(Settings{}));
}

// --- Страница для ПЗУ-конфигуратора ---

// Номер поля в таблице по указателю на член: тест обязан знать, какой бит
// маски чей, и узнаёт это тем же путём, что и сама маска.
uint32_t field_index(uint8_t Settings::*f) {
    Settings probe;
    for (uint32_t i = 0; i < config_page_field_count(); ++i) {
        // Пометить одно поле и посмотреть, какое значение уехало в страницу.
        Settings mark;
        mark.*f = 0x5A;
        uint8_t page[kConfigPageBytes];
        CHECK(config_page_build(mark, page, sizeof(page)) != 0);
        const auto* h       = reinterpret_cast<const ConfigPageHeader*>(page);
        const uint8_t* vals = page + h->values_offset;
        if (vals[i] == 0x5A) return i;
    }
    CHECK(false);
    return 0;
}

// Построили страницу, забрали обратно - настройки те же. Это главное
// свойство: иначе правка с экрана теряла бы поля.
void test_page_round_trip() {
    const Settings want = other_than_default();
    uint8_t page[kConfigPageBytes];
    const uint32_t used = config_page_build(want, page, sizeof(page));
    CHECK(used != 0);
    CHECK(used <= kConfigPageBytes);

    Settings got;
    CHECK(config_page_apply(page, sizeof(page), got));
    CHECK_EQ(std::memcmp(&got, &want, sizeof(Settings)), 0);

    std::printf("  configurator page: %u B of %u, fields %u\n", static_cast<unsigned>(used), static_cast<unsigned>(kConfigPageBytes),
                static_cast<unsigned>(config_page_field_count()));
}

// Страница живёт в памяти, которую пишет машина: чужую или битую плата
// обязана отвергнуть, не тронув настроек.
void test_page_rejects_foreign() {
    const Settings base = other_than_default();
    uint8_t page[kConfigPageBytes];
    CHECK(config_page_build(base, page, sizeof(page)) != 0);
    auto* h = reinterpret_cast<ConfigPageHeader*>(page);

    Settings keep         = Settings{};
    const Settings before = keep;

    const uint32_t good_magic = h->magic;
    h->magic                  = 0;
    CHECK(!config_page_apply(page, sizeof(page), keep));
    h->magic = good_magic;

    const uint16_t good_version = h->version;
    h->version                  = static_cast<uint16_t>(good_version + 1u);
    CHECK(!config_page_apply(page, sizeof(page), keep));
    h->version = good_version;

    // Сумма значений: одно значение сдвинуто, сумма прежняя.
    page[h->values_offset] = static_cast<uint8_t>(page[h->values_offset] + 1u);
    CHECK(!config_page_apply(page, sizeof(page), keep));

    CHECK_EQ(std::memcmp(&keep, &before, sizeof(Settings)), 0);
}

// Неактивные пункты: зависят от других полей, и правила считает плата.
void test_page_mask_follows_dependencies() {
    const uint32_t i_media = field_index(&Settings::disksys_media);
    const uint32_t i_reset = field_index(&Settings::reset_signal);
    const uint32_t i_zmed  = field_index(&Settings::zcontroller_media);
    const uint32_t i_speed = field_index(&Settings::mouse_speed);

    Settings off;
    off.disksys     = static_cast<uint8_t>(DiskSys::None);
    off.zcontroller = 0;
    off.usb_mouse   = 0;
    settings_clamp(off);
    const uint64_t m_off = settings_active_mask(off);
    CHECK((m_off & (uint64_t{1} << i_media)) == 0);
    CHECK((m_off & (uint64_t{1} << i_zmed)) == 0);
    CHECK((m_off & (uint64_t{1} << i_speed)) == 0);
    // Сброс при выключенном DivMMC - на выбор пользователя.
    CHECK((m_off & (uint64_t{1} << i_reset)) != 0);

    Settings on;
    on.disksys      = static_cast<uint8_t>(DiskSys::DivMmc);
    on.zcontroller  = 1;
    on.usb_mouse    = 1;
    on.reset_signal = 0;
    settings_clamp(on);
    // DivMMC грузит машину со своей карты и обязан её сбросить.
    CHECK_EQ(on.reset_signal, 1);
    const uint64_t m_on = settings_active_mask(on);
    CHECK((m_on & (uint64_t{1} << i_media)) != 0);
    CHECK((m_on & (uint64_t{1} << i_zmed)) != 0);
    CHECK((m_on & (uint64_t{1} << i_speed)) != 0);
    CHECK((m_on & (uint64_t{1} << i_reset)) == 0);
}

// Пересчёт после правки переписывает значения и маску, не трогая строк.
void test_page_refresh_keeps_descriptors() {
    uint8_t page[kConfigPageBytes];
    CHECK(config_page_build(Settings{}, page, sizeof(page)) != 0);
    const auto* h = reinterpret_cast<const ConfigPageHeader*>(page);

    uint8_t strings_before[256];
    std::memcpy(strings_before, page + h->strings_offset, sizeof(strings_before));

    Settings s;
    s.disksys = static_cast<uint8_t>(DiskSys::DivMmc);
    settings_clamp(s);
    CHECK(config_page_refresh(s, page, sizeof(page)));

    CHECK_EQ(std::memcmp(strings_before, page + h->strings_offset, sizeof(strings_before)), 0);
    Settings got;
    CHECK(config_page_apply(page, sizeof(page), got));
    CHECK_EQ(got.disksys, static_cast<uint8_t>(DiskSys::DivMmc));
    CHECK_EQ(got.reset_signal, 1);
}

// Снимок сохранённого правкой не меняется: по нему ПЗУ помечает
// изменённые поля и показывает прежнее значение. Сползи он вслед за
// правкой - пометок не было бы вовсе.
void test_page_saved_snapshot_stays() {
    const uint32_t i_disk = field_index(&Settings::disksys);
    uint8_t page[kConfigPageBytes];
    CHECK(config_page_build(Settings{}, page, sizeof(page)) != 0);
    const auto* h = reinterpret_cast<const ConfigPageHeader*>(page);

    // Сразу после сборки изменённых полей нет.
    CHECK_EQ(std::memcmp(page + h->values_offset, page + h->saved_offset, h->field_count), 0);

    Settings s;
    s.disksys = static_cast<uint8_t>(DiskSys::DivMmc);
    settings_clamp(s);
    CHECK(config_page_refresh(s, page, sizeof(page)));

    const uint8_t* values = page + h->values_offset;
    const uint8_t* saved  = page + h->saved_offset;
    CHECK_EQ(values[i_disk], static_cast<uint8_t>(DiskSys::DivMmc));
    CHECK_EQ(saved[i_disk], static_cast<uint8_t>(DiskSys::None));
    // Принуждённое поле тоже видно как изменённое.
    const uint32_t i_reset = field_index(&Settings::reset_signal);
    CHECK_EQ(values[i_reset], 1);
    CHECK_EQ(saved[i_reset], 0);
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

// Esc занят отменой: числом из блока или со страницы он не проходит.
void test_clamp_joy_keys() {
    Settings s;
    s.joy_keys[0][0] = kKeyEsc;
    s.joy_keys[0][1] = 0x02;
    s.joy_keys[1][0] = 0xE0;
    s.joy_keys[2][1] = 0x63;
    settings_clamp(s);
    CHECK_EQ(s.joy_keys[0][0], 0);
    CHECK_EQ(s.joy_keys[0][1], 0);
    CHECK_EQ(s.joy_keys[1][0], 0);
    CHECK_EQ(s.joy_keys[2][1], 0x63);
}

// Привязки на странице: поля вида "клавиша" за ключами таблицы, по клавише действия
// на поле. Кнопки 2..4 правятся только при восьмиразрядном Kempston, все
// привязки - только при включённой клавиатуре.
void test_page_joy_key_fields() {
    Settings s;
    s.keyboard_layout = static_cast<uint8_t>(Layout::Picard);
    s.joy_keys[3][1]  = 0x1a; // W вторым к вверх
    uint8_t page[kConfigPageBytes];
    CHECK(config_page_build(s, page, sizeof(page)) != 0);
    const auto* h = reinterpret_cast<const ConfigPageHeader*>(page);
    CHECK_EQ(h->field_count, 40u);

    const uint32_t first = h->field_count - kJoyActions * kJoyKeysPerAction;
    ConfigPageField f{};
    std::memcpy(&f, page + h->desc_offset + (first + 7u) * kConfigFieldBytes, sizeof(f));
    CHECK_EQ(f.kind, static_cast<uint8_t>(PageKind::Key));
    CHECK(f.choices != 0);
    CHECK_EQ(std::strcmp(reinterpret_cast<const char*>(page + f.name), "JOY_UP 2"), 0);
    CHECK_EQ(page[h->values_offset + first + 7u], 0x1a);

    // Правка с экрана: Q на второй огонь, Esc на огонь - Esc отсекается.
    page[h->values_offset + first + 10u] = 0x14;
    page[h->values_offset + first + 8u]  = kKeyEsc;
    uint8_t sum                          = 0;
    for (uint32_t i = 0; i < h->field_count; ++i) {
        sum = static_cast<uint8_t>(sum + page[h->values_offset + i]);
    }
    page[h->values_offset + h->field_count] = sum;
    Settings got                            = s;
    CHECK(config_page_apply(page, sizeof(page), got));
    CHECK_EQ(got.joy_keys[5][0], 0x14);
    CHECK_EQ(got.joy_keys[4][0], 0);

    const uint64_t narrow = settings_active_mask(s);
    CHECK((narrow & (uint64_t{1} << (first + 8u))) != 0);  // огонь
    CHECK((narrow & (uint64_t{1} << (first + 10u))) == 0); // второй огонь
    s.joy_8bit = 1;
    CHECK((settings_active_mask(s) & (uint64_t{1} << (first + 10u))) != 0);
    s.keyboard_layout = static_cast<uint8_t>(Layout::None);
    CHECK((settings_active_mask(s) & (uint64_t{1} << first)) == 0);

    // Ячейка захвата внутри страницы и после маски.
    uint8_t* cell = config_page_capture(page, sizeof(page));
    CHECK(cell != nullptr);
    CHECK_EQ(cell[0], kCaptureIdle);
    CHECK_EQ(static_cast<uint32_t>(cell - page), static_cast<uint32_t>(h->mask_offset + kConfigMaskBytes));
}

} // namespace

void run_config_tests() {
    std::printf("test_config\n");
    test_store_empty_flash();
    test_store_alternates_and_survives_torn_write();
    test_store_rejects_foreign_format();
    test_store_counter_wraps();
    test_ini_round_trip();
    test_ini_ignores_spaces_and_junk();
    test_ini_bad_lines_do_not_cancel_the_rest();
    test_ini_render_measures_when_it_does_not_fit();
    test_keyboard_off_is_a_layout();
    test_defaults_are_tsconfig();
    test_clamp();
    test_store_reads_shorter_payload();
    test_wire_midi_yields_to_wifi();
    test_usb_host_used_by_every_consumer();
    test_page_round_trip();
    test_page_rejects_foreign();
    test_page_mask_follows_dependencies();
    test_page_refresh_keeps_descriptors();
    test_page_saved_snapshot_stays();
    test_store_block_without_records();
    test_store_records_unknown_and_short();
    test_store_records_bad_crc_keeps_fixed_part();
    test_ini_joy_keys();
    test_clamp_joy_keys();
    test_page_joy_key_fields();

    char text[kIniMaxBytes];
    const uint32_t len = ini_render(Settings{}, text, sizeof(text));
    std::printf("  default settings file: %u B, keys %u\n", static_cast<unsigned>(len), static_cast<unsigned>(sizeof(Settings)));
}
