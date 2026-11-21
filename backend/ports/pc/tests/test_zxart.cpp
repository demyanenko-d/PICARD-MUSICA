// SPDX-License-Identifier: MIT
// Поставщик zxart: дерево каталогов, запросы API и разбор ответов. Сеть
// подставная - ответы в том виде, в каком их отдаёт сайт.
#include "testing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif
#include "ff.h"

#include "net/vdisk/vdisk.h"
#include "net/zxart/json_stream.h"
#include "net/hal/net.h"
#include "net/zxart/zxart.h"
#include "pc/fatfs_disk.h"

extern uint32_t g_test_time_us;

using namespace net;

namespace {

struct Reply {
    int status;
    std::string body;
};

std::map<std::string, Reply> g_replies;
std::vector<std::string> g_requests;
std::string g_pending;
bool g_has_pending = false;
uint32_t g_busy    = 0; // столько GET подряд транспорт отклонит
uint32_t g_cancels = 0;
bool g_silent      = false; // запрос принят, ответа нет
uint32_t g_piece   = 7;     // тело отдаётся кусками: разбор обязан это пережить

bool g_started = false; // код ответа отдан, тело идёт по месту у поставщика

void http_cancel() {
    ++g_cancels;
    g_has_pending = false;
    g_started     = false;
}

bool http_get(const char* path, bool /*stream*/ = false) {
    if (g_busy != 0u) {
        --g_busy;
        return false;
    }
    g_requests.push_back(path);
    g_pending     = path;
    g_has_pending = true;
    g_started     = false;
    return true;
}

uint32_t g_cuts     = 0;  // столько ответов подряд оборвётся на середине
int32_t g_cut_after = -1; // через столько целых ответов оборвётся один

// Ответ - как транспорт платы: тело не больше места у поставщика, остаток
// - следующим вызовом.
Reply g_rep;
size_t g_pos = 0, g_end = 0;
bool g_cut = false;

void serve_pending() {
    if (!g_has_pending || g_silent) return;
    if (!g_started) {
        g_started     = true;
        const auto it = g_replies.find(g_pending);
        g_rep         = it == g_replies.end() ? Reply{404, "not found"} : it->second;
        zxart::zxart_http_status(g_rep.status);
        if (g_cut_after == 0) ++g_cuts;
        if (g_cut_after >= 0) --g_cut_after;
        g_cut = g_cuts != 0u;
        g_end = g_cut ? g_rep.body.size() / 2u : g_rep.body.size();
        g_pos = 0;
        if (g_cut) --g_cuts;
    }
    while (g_has_pending && g_pos < g_end) {
        size_t n            = g_end - g_pos < g_piece ? g_end - g_pos : g_piece;
        const uint32_t room = zxart::zxart_http_room();
        if (room == 0u) return;
        if (n > room) n = room;
        zxart::zxart_http_body(reinterpret_cast<const uint8_t*>(g_rep.body.data() + g_pos), static_cast<uint32_t>(n));
        g_pos += n;
    }
    if (!g_has_pending) return; // поставщик бросил приём
    g_has_pending = false;
    g_started     = false;
    zxart::zxart_http_end(!g_cut);
}

bool reader(unsigned long lba, unsigned char* out) {
    for (int tries = 0; tries < 200; ++tries) {
        const vdisk::ReadResult r = vdisk::vdisk_read(static_cast<uint32_t>(lba), out);
        if (r == vdisk::ReadResult::Ready) return true;
        if (r == vdisk::ReadResult::Error) return false;
        g_test_time_us += 400000u; // пауза между запросами
        zxart::zxart_poll();
        serve_pending();
    }
    return false;
}

uint8_t* identity(uint8_t* p, size_t) {
    return p;
}

std::vector<uint8_t> g_mem;
std::vector<uint8_t> g_cache;
std::vector<uint8_t> g_state(zxart::kStateBytes);
FATFS g_fs;

void mount(bool cache = false) {
    g_mem.assign(vdisk::kMemoryBytes, 0);
    g_cache.assign(zxart::kCacheBytes, 0xcc);
    CHECK(vdisk::vdisk_init(g_mem.data(), static_cast<uint32_t>(g_mem.size()), &identity));
    zxart::zxart_init(&http_get, &http_cancel, g_state.data(), cache ? g_cache.data() : nullptr, &identity);
    g_busy    = 0;
    g_silent  = false;
    g_cancels = 0;
    g_requests.clear();
    g_has_pending = false;
    platform_pc::mount_sector_reader(&reader);
    CHECK_EQ(f_mount(&g_fs, "", 1), FR_OK);
}

void unmount() {
    f_mount(nullptr, "", 0);
    platform_pc::mount_sector_reader(nullptr);
    g_replies.clear();
}

std::vector<std::string> names(const char* path, FRESULT* res = nullptr) {
    std::vector<std::string> out;
    DIR d;
    FRESULT r = f_opendir(&d, path);
    FILINFO fi;
    while (r == FR_OK) {
        r = f_readdir(&d, &fi);
        if (r != FR_OK || fi.fname[0] == '\0') break;
        out.emplace_back(fi.fname);
    }
    if (res) *res = r;
    f_closedir(&d);
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v)
        s += (s.empty() ? "" : "|") + x;
    return s;
}

const std::string kApi = "/api/language:eng/";

std::string prods_path(const std::string& filter, uint32_t start = 0) {
    return kApi + "export:zxProd/start:" + std::to_string(start) + "/limit:100/order:title,asc/filter:" + filter +
           ";zxReleaseFormat=trd,scl,tap,tzx,z80,sna,dsk,mgt,img,mdr,rom,nex,szx";
}

std::string prod_json(uint32_t id, const std::string& title, uint32_t year) {
    return "{\"id\":" + std::to_string(id) + ",\"title\":\"" + title + "\",\"dateCreated\":1791879041,\"language\":[\"ru\"],\"year\":" + std::to_string(year) +
           ",\"legalStatus\":\"unknown\",\"releasesIds\":[1,2],\"authorsInfo\":[{\"id\":88732,\"authorId\":2615,\"startDate\":\"\",\"roles\":[\"graphics\"],"
           "\"type\":\"prod\"}],\"votes\":3.91,\"categoriesString\":\"Demoscene/Gift\"}";
}

std::string list_json(const char* entity, uint32_t total, const std::vector<std::string>& items) {
    std::string s = "{\"totalAmount\":" + std::to_string(total) + ",\"start\":0,\"limit\":200,\"responseData\":{\"" + entity + "\":[";
    for (size_t i = 0; i < items.size(); ++i)
        s += (i ? "," : "") + items[i];
    return s + "]},\"responseStatus\":\"success\"}";
}

void test_json_stream() {
    std::printf("test_zxart_json_stream\n");
    static std::string text;
    text.clear();
    const zxart::JsonHandler log = {
        nullptr,
        [](void*, uint32_t d, bool a) { text += (a ? "[" : "{") + std::to_string(d); },
        [](void*, uint32_t d, bool a) { text += (a ? "]" : "}") + std::to_string(d); },
        [](void*, uint32_t d, const char* k, const char* t, bool q) { text += " " + std::to_string(d) + ":" + k + "=" + (q ? "'" : "") + t + (q ? "'" : ""); },
    };
    zxart::JsonStream js;
    js.reset(&log);
    // Знаки не ASCII - UTF-8, и из UTF-8, и из \u.
    const char* doc = "{\"a\" : [1, -2.5e3, true, null, \"x\\\"y\\u0041\\u0416\"], \"b\":{}, \"c\":\"\xd0\x96\xd1\x83\xd0\xba z\xe2\x82\xac\", \"e\":[]}";
    for (const char* p = doc; *p; ++p) {
        js.feed(reinterpret_cast<const uint8_t*>(p), 1);
    }
    CHECK(js.done());
    CHECK(!js.failed());
    CHECK_EQ(text, std::string("{1[2 2:a=1 2:a=-2.5e3 2:a=true 2:a=null 2:a='x\"yA\xd0\x96']2{2}2 1:c='\xd0\x96\xd1\x83\xd0\xba z\xe2\x82\xac'[2]2}1"));
    js.reset(&log);
    const char* bad = "{\"a\" 1}";
    js.feed(reinterpret_cast<const uint8_t*>(bad), static_cast<uint32_t>(std::strlen(bad)));
    CHECK(js.failed());
    // Вложенность глубже предела пропускается, разбор идёт дальше.
    text.clear();
    js.reset(&log);
    std::string deep = "{\"x\":";
    for (int i = 0; i < 20; ++i)
        deep += "[{\"s\":\"]}\"},";
    deep += "1";
    for (int i = 0; i < 20; ++i)
        deep += "]";
    deep += ",\"y\":2}";
    js.feed(reinterpret_cast<const uint8_t*>(deep.data()), static_cast<uint32_t>(deep.size()));
    CHECK(js.done());
    CHECK(text.find(" 1:y=2}1") != std::string::npos);
    // Ключ длиннее kKeyMax режется; строка длиннее kTextMax - целыми
    // знаками, следующий ASCII ещё влезает.
    text.clear();
    js.reset(&log);
    std::string zh;
    for (int i = 0; i < 50; ++i)
        zh += "\xd0\x96";
    std::string expect_zh;
    for (int i = 0; i < 47; ++i)
        expect_zh += "\xd0\x96";
    const std::string longs = "{\"abcdefghijklmnopqrstuvwxyz0123\":\"" + zh + "x\"}";
    js.feed(reinterpret_cast<const uint8_t*>(longs.data()), static_cast<uint32_t>(longs.size()));
    CHECK(js.done());
    CHECK_EQ(text, "{1 1:abcdefghijklmnopqrstuvw='" + expect_zh + "x'}1");
    // Значение в корне - глубина 0, пустой ключ.
    text.clear();
    js.reset(&log);
    const char* root_str = "\"ab\\u0001c\"";
    js.feed(reinterpret_cast<const uint8_t*>(root_str), static_cast<uint32_t>(std::strlen(root_str)));
    CHECK(js.done());
    CHECK_EQ(text, std::string(" 0:='ab_c'"));
    text.clear();
    js.reset(&log);
    const char* root_num = "12 ";
    js.feed(reinterpret_cast<const uint8_t*>(root_num), static_cast<uint32_t>(std::strlen(root_num)));
    CHECK(js.done());
    CHECK_EQ(text, std::string(" 0:=12"));
}

void test_static_tree() {
    std::printf("test_zxart_static_tree\n");
    mount();
    CHECK_EQ(join(names("/")), std::string("authors|games|demos|system|applied|educat|press|compils|misc"));
    CHECK_EQ(join(names("/games")), std::string("names|authors|years|genres|best"));
    // Буквы - из словаря: маленькая буква - сразу каталог программ,
    // большая - её префиксы; знак в префиксе - '_'.
    const auto letters = names("/games/names");
    CHECK(letters.size() > 40u);
    CHECK(std::find(letters.begin(), letters.end(), std::string("Q")) != letters.end());
    const auto a = names("/games/names/A");
    CHECK(a.size() > 20u);
    CHECK(std::find(a.begin(), a.end(), std::string("AB")) != a.end());
    const auto e = names("/games/names/E");
    CHECK(std::find(e.begin(), e.end(), std::string("EL_")) != e.end());
    CHECK(std::find(e.begin(), e.end(), std::string("ELF")) != e.end());
    // Годы - те, где есть программы; не указан - последним.
    const auto years = names("/games/years");
    CHECK(years.size() > 40u);
    if (!years.empty()) {
        CHECK_EQ(years[0], std::string("1982"));
        CHECK_EQ(years.back(), std::string("unknown"));
    }
    CHECK_EQ(join(names("/games/years/1987")), std::string("names|authors"));
    CHECK_EQ(join(names("/games/genres")), std::string("Action|Adventure|Card and Board Games|Gambling|Interactive Fiction|Puzzle|RPG|Simulators|Strategy|"
                                                       "Undetermined"));
    CHECK_EQ(join(names("/games/genres/RPG")), std::string("names|authors|years"));
    CHECK(names("/authors").size() > 40u);
    // Ни одного запроса в сеть.
    CHECK(g_requests.empty());
    unmount();
}

void test_prods_and_files() {
    std::printf("test_zxart_prods_and_files\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=AB")] =
        Reply{200, list_json("zxProd", 4,
                             {prod_json(355321, "Abaca", 1995), prod_json(150659, "Abakus", 1990), prod_json(150660, "ABAKUS", 1991),
                              prod_json(150661, "Ab\\u0416c: \\\"q\\\"", 0)})};
    const std::string rel =
        "{\"totalAmount\":2,\"start\":0,\"limit\":1000,\"responseData\":{\"zxRelease\":["
        "{\"id\":93364,\"title\":\"Abaca\",\"file\":\"https://zxart.ee/releasefile/id:93364/Abaca.trd.zip\",\"releaseFormat\":[\"trd\"],"
        "\"releaseStructure\":[{\"id\":2643,\"md5\":\"6e\",\"parentId\":0,\"fileName\":\"Abaca.trd.zip\",\"size\":12911,\"elementId\":93364,\"type\":\"zip\","
        "\"encoding\":\"none\",\"internalType\":\"binary\",\"viewable\":false,\"items\":[{\"id\":2644,\"md5\":\"4d\",\"parentId\":2643,\"fileName\":\"ABACA."
        "TRD\","
        "\"size\":3000,\"elementId\":93364,\"type\":\"trd\",\"items\":[{\"id\":2645,\"fileName\":\"ABACA.B\",\"size\":14848,\"elementId\":93364,\"type\":"
        "\"file\","
        "\"viewable\":true}]}]}],\"prodId\":355321},"
        "{\"id\":93400,\"title\":\"Abaca\",\"releaseStructure\":[{\"id\":2700,\"fileName\":\"abaca.trd\",\"size\":1500,\"elementId\":93400,\"type\":\"trd\","
        "\"items\":[]},{\"id\":2701,\"fileName\":\"Abaca.tap\",\"size\":700,\"elementId\":93400,\"type\":\"tap\"},{\"id\":2702,\"fileName\":\"readme.txt\","
        "\"size\":70,\"elementId\":93400,\"type\":\"file\"}],\"prodId\":355321}]},\"responseStatus\":\"success\"}";
    g_replies[kApi + "export:zxRelease/limit:200/filter:zxProdId=355321"] = Reply{200, rel};
    std::string trd(3000, '\0');
    for (size_t i = 0; i < trd.size(); ++i)
        trd[i] = static_cast<char>(i * 7 + 3);
    g_replies["/zxfile/id:93364/fileId:2644/f"] = Reply{200, trd};
    mount();
    // Одинаковые названия стоят рядом и нумеруются; запрещённые в FAT
    // знаки - '_'. Длинное имя с кириллицей FatFs отдаёт в cp866 (Ж - 0x86).
    CHECK_EQ(join(names("/games/names/A/AB")), std::string("Abaca|Abakus|ABAKUS (2)|Ab\x86"
                                                           "c_ _q_"));
    CHECK_EQ(join(names("/games/names/A/AB/Abaca")), std::string("ABACA.TRD|abaca (2).trd|Abaca.tap"));
    FIL f;
    CHECK_EQ(f_open(&f, "/games/names/A/AB/Abaca/ABACA.TRD", FA_READ), FR_OK);
    std::vector<char> buf(4000);
    UINT got = 0;
    CHECK_EQ(f_read(&f, buf.data(), 4000, &got), FR_OK);
    CHECK_EQ(got, 3000u);
    CHECK(std::memcmp(buf.data(), trd.data(), 3000) == 0);
    f_close(&f);
    CHECK_EQ(zxart::zxart_stats().files, 1u);
    // Ответ с ошибкой - ошибка чтения файла, не содержимое страницы.
    g_replies["/zxfile/id:93400/fileId:2701/f"] = Reply{503, "Service Unavailable"};
    CHECK_EQ(f_open(&f, "/games/names/A/AB/Abaca/Abaca.tap", FA_READ), FR_OK);
    const FRESULT rr = f_read(&f, buf.data(), 700, &got);
    CHECK(rr != FR_OK);
    f_close(&f);
    unmount();
}

std::string author_prods_path(const std::string& filter) {
    return kApi + "export:zxProd/start:0/limit:100/order:title,asc/filter:" + filter;
}

std::string authors_path(const std::string& filter, uint32_t start = 0) {
    return kApi + "export:author/start:" + std::to_string(start) + "/limit:200/order:title,asc" + (filter.empty() ? "" : "/filter:" + filter);
}

void test_authors_paging() {
    std::printf("test_zxart_authors_paging\n");
    std::vector<std::string> page[3];
    for (uint32_t i = 0; i < 450u; ++i) {
        char t[32];
        std::snprintf(t, sizeof t, "Author %03u", static_cast<unsigned>(i));
        page[i / 200u].push_back("{\"id\":" + std::to_string(5000 + i) + ",\"title\":\"" + t + "\",\"realName\":\"X\"}");
    }
    for (uint32_t p = 0; p < 3u; ++p) {
        g_replies[authors_path("zxProdCategory=204819;authorFirstLetter=AB", p * 200u)] = Reply{200, list_json("author", 450, page[p])};
    }
    g_replies[author_prods_path("authorId=5007;zxProdCategory=204819")] = Reply{200, list_json("zxProd", 1, {prod_json(777, "Demo by seven", 1999)})};
    mount();
    const auto authors = names("/demos/authors/A/AB");
    CHECK_EQ(authors.size(), 450u);
    if (authors.size() == 450u) CHECK_EQ(authors[449], std::string("Author 449"));
    CHECK_EQ(g_requests.size(), 3u);
    CHECK_EQ(join(names("/demos/authors/A/AB/Author 007")), std::string("Demo by seven"));
    unmount();
    // Обрыв страницы: она запрашивается заново, принятое не задваивается.
    for (uint32_t p = 0; p < 3u; ++p) {
        g_replies[authors_path("zxProdCategory=204819;authorFirstLetter=AB", p * 200u)] = Reply{200, list_json("author", 450, page[p])};
    }
    mount();
    g_cut_after      = 1; // первая страница целиком, вторая оборвётся один раз
    const auto again = names("/demos/authors/A/AB");
    CHECK_EQ(again.size(), 450u);
    CHECK_EQ(std::set<std::string>(again.begin(), again.end()).size(), 450u);
    CHECK_EQ(zxart::zxart_stats().retries, 1u);
    CHECK_EQ(g_requests.size(), 4u);
    g_cut_after = -1;
    unmount();
}

// Год: буквы из словаря года, в букве - программы с фильтром года.
void test_years() {
    std::printf("test_zxart_years\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdYear=1987;zxProdFirstLetter=B")] =
        Reply{200, list_json("zxProd", 2, {prod_json(2, "Batman", 1987), prod_json(3, "Bubble", 1987)})};
    g_replies[prods_path("zxProdCategory=92177;zxProdYear=0;zxProdFirstLetter=Z")] = Reply{200, list_json("zxProd", 1, {prod_json(4, "Zub", 0)})};
    mount();
    const auto letters = names("/games/years/1987/names");
    CHECK(std::find(letters.begin(), letters.end(), std::string("B")) != letters.end());
    CHECK_EQ(join(names("/games/years/1987/names/B")), std::string("Batman|Bubble"));
    // Год не указан - фильтр zxProdYear=0.
    CHECK_EQ(join(names("/games/years/unknown/names/Z")), std::string("Zub"));
    unmount();
}

// Жанры - из словаря; небольшой список - сразу программы из сети.
void test_genres() {
    std::printf("test_zxart_genres\n");
    g_replies[prods_path("zxProdCategory=92546")] = Reply{200, list_json("zxProd", 1, {prod_json(42, "Shooter", 1988)})};
    mount();
    CHECK_EQ(join(names("/games/genres/Undetermined/names")), std::string("Shooter"));
    CHECK_EQ(g_requests.size(), 1u);
    unmount();
}

// Лучшие - сортировкой сайта по оценке, одной страницей.
void test_best() {
    std::printf("test_zxart_best\n");
    g_replies[kApi +
              "export:zxProd/limit:100/order:votes,desc/filter:zxProdCategory=92177;zxReleaseFormat=trd,scl,tap,tzx,z80,sna,dsk,mgt,img,mdr,rom,nex,szx"] =
        Reply{200, list_json("zxProd", 500, {prod_json(10, "Valley of Rains", 2020), prod_json(11, "Wonderful Dizzy", 2020)})};
    mount();
    CHECK_EQ(join(names("/games/best")), std::string("Valley of Rains|Wonderful Dizzy"));
    CHECK_EQ(g_requests.size(), 1u);
    unmount();
}

// Автор всего сайта: разделы - по категориям его программ.
void test_author_sections() {
    std::printf("test_zxart_author_sections\n");
    g_replies[authors_path("authorFirstLetter=Q")]                   = Reply{200, list_json("author", 1, {"{\"id\":321,\"title\":\"Quasar\"}"})};
    g_replies[kApi + "export:zxProd/limit:1000/filter:authorId=321"] = Reply{
        200, list_json("zxProd", 2,
                       {"{\"id\":1,\"title\":\"G\",\"connectedCategoriesIds\":[92505]}", "{\"id\":2,\"title\":\"D\",\"connectedCategoriesIds\":[92159,999]}"})};
    g_replies[author_prods_path("authorId=321;zxProdCategory=92177")] = Reply{200, list_json("zxProd", 1, {prod_json(1, "G", 1990)})};
    mount();
    CHECK_EQ(join(names("/authors/Q")), std::string("Quasar"));
    CHECK_EQ(join(names("/authors/Q/Quasar")), std::string("games|demos"));
    CHECK_EQ(join(names("/authors/Q/Quasar/games")), std::string("G"));
    unmount();
}

// Машина ничего не ждёт: релизы программ последнего списка берутся
// заранее пачкой, каталог программы потом - без сети.
void test_prefetch() {
    std::printf("test_zxart_prefetch\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=AB")] =
        Reply{200, list_json("zxProd", 3, {prod_json(11, "Abaca", 1990), prod_json(12, "Abakus", 1991), prod_json(13, "Abbey", 1992)})};
    const std::string rel = "{\"totalAmount\":3,\"responseData\":{\"zxRelease\":["
                            "{\"id\":501,\"releaseStructure\":[{\"id\":1,\"fileName\":\"ABACA.TRD\",\"size\":3000,\"elementId\":501,\"type\":\"trd\"}],"
                            "\"prodId\":11},"
                            "{\"id\":503,\"releaseStructure\":[{\"id\":3,\"fileName\":\"ABBEY.TAP\",\"size\":700,\"elementId\":503,\"type\":\"tap\"}],"
                            "\"prodId\":13},"
                            "{\"id\":502,\"releaseStructure\":[{\"id\":2,\"fileName\":\"ABACA2.SCL\",\"size\":900,\"elementId\":502,\"type\":\"scl\"}],"
                            "\"prodId\":11}]},\"responseStatus\":\"success\"}";
    // Очередь - оба списка: сначала Q, потом по кругу AB.
    g_replies[kApi + "export:zxRelease/limit:1000/filter:zxProdId=20,11,12,13"] = Reply{200, rel};
    mount(true);
    CHECK_EQ(join(names("/games/names/A/AB")), std::string("Abaca|Abakus|Abbey"));
    // Пачка ушла, ответа ещё нет - машине понадобился другой каталог:
    // пачка бросается, каталог машины идёт первым.
    g_test_time_us += 400000u;
    zxart::zxart_poll();
    CHECK(g_has_pending);
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=Q")] = Reply{200, list_json("zxProd", 1, {prod_json(20, "Quazatron", 1986)})};
    CHECK_EQ(join(names("/games/names/Q")), std::string("Quazatron"));
    CHECK_EQ(g_cancels, 1u);
    CHECK_EQ(zxart::zxart_stats().prefetches, 0u);
    CHECK_EQ(zxart::zxart_stats().queued, 4u);
    const size_t after_list = g_requests.size();
    for (int i = 0; i < 5; ++i) {
        g_test_time_us += 400000u;
        zxart::zxart_poll();
        serve_pending();
    }
    CHECK_EQ(g_requests.size(), after_list + 1u);
    CHECK_EQ(zxart::zxart_stats().prefetches, 1u);
    // Релизы одной программы вперемешку с чужими - все её файлы; программа
    // без образов - пустой каталог; запросов больше нет.
    CHECK_EQ(join(names("/games/names/A/AB/Abaca")), std::string("ABACA.TRD|ABACA2.SCL"));
    CHECK_EQ(join(names("/games/names/A/AB/Abbey")), std::string("ABBEY.TAP"));
    CHECK(names("/games/names/A/AB/Abakus").empty());
    CHECK_EQ(g_requests.size(), after_list + 1u);
    CHECK_EQ(zxart::zxart_stats().cache_hits, 3u);
    CHECK_EQ(zxart::zxart_stats().queued, 0u);
    unmount();
}

void test_failures() {
    std::printf("test_zxart_failures\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=ZZ")] = Reply{200, "{\"totalAmount\":3,\"responseData\":{\"zxProd\":[{\"id\":1,"};
    mount();
    CHECK_EQ(join(names("/games/names/Z/ZY")), std::string("ERROR")); // нет ответа - 404
    CHECK_EQ(join(names("/games/names/Z/ZZ")), std::string("ERROR")); // обрезанный JSON
    CHECK_EQ(zxart::zxart_stats().json_errors, 1u);
    CHECK_EQ(zxart::zxart_stats().failed, 2u);
    unmount();
}

std::string read_data(const char* name) {
    std::string path = std::string("backend/ports/pc/tests/data/zxart/") + name;
    std::FILE* f     = std::fopen(path.c_str(), "rb");
    std::string s;
    if (f == nullptr) return s;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) != 0u)
        s.append(buf, n);
    std::fclose(f);
    return s;
}

// Разбор настоящих ответов сайта: программы на "ab" (снято с limit:5) и
// релизы с образами внутри zip и папками (снято другими фильтрами). Здесь
// они подставлены под пути листингов - проверяется разбор формы, а не
// то, что сайт отвечает на эти пути именно ими.
void test_real_responses() {
    std::printf("test_zxart_real_responses\n");
    const std::string prods = read_data("prods_ab.json");
    const std::string trd   = read_data("releases_trd.json");
    const std::string tzx   = read_data("release_92844.json");
    CHECK(!prods.empty() && !trd.empty() && !tzx.empty());
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=AB")]    = Reply{200, prods};
    g_replies[kApi + "export:zxRelease/limit:200/filter:zxProdId=355321"] = Reply{200, trd};
    g_replies[kApi + "export:zxRelease/limit:200/filter:zxProdId=152292"] = Reply{200, tzx};
    g_piece                                                               = 1;
    mount();
    CHECK_EQ(join(names("/games/names/A/AB")), std::string("Abaca|Abacus|Abaduaber HB Gift|Abakus|Abakus (2)"));
    CHECK_EQ(join(names("/games/names/A/AB/Abaca")), std::string("AMMYTRIS.TRD|BLOCK.TRD"));
    CHECK_EQ(join(names("/games/names/A/AB/Abacus")), std::string("Abu Simbel Profanation - Medium Case - 1st Release.tzx"));
    FILINFO fi;
    CHECK_EQ(f_stat("/games/names/A/AB/Abaca/AMMYTRIS.TRD", &fi), FR_OK);
    CHECK_EQ(fi.fsize, 655360u);
    CHECK_EQ(fi.fdate, static_cast<WORD>(((1980 - 1980) << 9) | (1 << 5) | 1));
    CHECK_EQ(zxart::zxart_stats().json_errors, 0u);
    g_piece = 7;
    unmount();
}

std::string node_json(uint32_t id, uint32_t release, const char* name, uint32_t size, const char* type, const std::string& items) {
    return "{\"id\":" + std::to_string(id) + ",\"fileName\":\"" + name + "\",\"size\":" + std::to_string(size) + ",\"elementId\":" + std::to_string(release) +
           ",\"type\":\"" + type + "\",\"items\":[" + items + "]}";
}

std::string release_json(uint32_t release, const std::string& nodes) {
    return "{\"totalAmount\":1,\"responseData\":{\"zxRelease\":[{\"id\":" + std::to_string(release) + ",\"title\":\"R\",\"releaseStructure\":[" + nodes +
           "],\"prodId\":1}]},\"responseStatus\":\"success\"}";
}

// Образ глубоко в архиве, тип "file" с расширением образа, длинное имя,
// одинаковые названия не по соседству.
void test_files_and_names() {
    std::printf("test_zxart_files_and_names\n");
    const std::string deep =
        node_json(10, 500, "a.zip", 100, "zip",
                  node_json(11, 500, "d1", 0, "folder",
                            node_json(12, 500, "d2", 0, "folder", node_json(13, 500, "d3", 0, "folder", node_json(14, 500, "DEEP.TRD", 4096, "trd", ""))))) +
        "," + node_json(20, 500, "game.tap", 300, "file", "") + "," +
        node_json(21, 500, "A very long file name that goes on and on beyond the limit of a record.tzx", 200, "tzx", "") + "," +
        node_json(22, 500, "short.tap", 999, "tap", "");
    g_replies[kApi + "export:zxRelease/limit:200/filter:zxProdId=42"]  = Reply{200, release_json(500, deep)};
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=ZH")] = Reply{
        200, list_json("zxProd", 3, {prod_json(42, "Zhuk", 1990), prod_json(43, "Zebra&#039;s &amp; co &#x26; &bad;", 1991), prod_json(44, "Zhuk", 1992)})};
    g_replies["/zxfile/id:500/fileId:22/f"] = Reply{200, std::string(500, 'x')}; // короче размера из листинга
    mount();
    // HTML-сущности сайта раскрываются, незнакомые остаются как есть.
    CHECK_EQ(join(names("/games/names/Z/ZH")), std::string("Zhuk|Zebra's & co & &bad;|Zhuk (2)"));
    const auto files = names("/games/names/Z/ZH/Zhuk");
    CHECK_EQ(files.size(), 4u);
    if (files.size() == 4u) {
        CHECK_EQ(files[0], std::string("DEEP.TRD"));
        CHECK_EQ(files[1], std::string("game.tap"));
        CHECK_EQ(files[2].size(), static_cast<size_t>(vdisk::kNameMax));
        CHECK_EQ(files[2].substr(files[2].size() - 4), std::string(".tzx"));
    }
    FIL f;
    char buf[1000];
    UINT got = 0;
    CHECK_EQ(f_open(&f, "/games/names/Z/ZH/Zhuk/short.tap", FA_READ), FR_OK);
    CHECK(f_read(&f, buf, sizeof buf, &got) != FR_OK);
    f_close(&f);
    CHECK_EQ(zxart::zxart_stats().json_errors, 0u);
    // Сайт оборвал ответ: файл запрашивается заново целиком и приходит.
    g_replies["/zxfile/id:500/fileId:20/f"] = Reply{200, std::string(300, 'g')};
    g_cuts                                  = 2;
    const size_t sent                       = g_requests.size();
    CHECK_EQ(f_open(&f, "/games/names/Z/ZH/Zhuk/game.tap", FA_READ), FR_OK);
    CHECK_EQ(f_read(&f, buf, sizeof buf, &got), FR_OK);
    CHECK_EQ(got, 300u);
    CHECK(got == 300u && std::memcmp(buf, std::string(300, 'g').data(), 300) == 0);
    f_close(&f);
    CHECK_EQ(zxart::zxart_stats().retries, 2u);
    CHECK_EQ(g_requests.size(), sent + 3u);
    // Обрывы сверх kRetries - ошибка чтения.
    g_replies["/zxfile/id:500/fileId:14/f"] = Reply{200, std::string(4096, 'd')};
    g_cuts                                  = zxart::kRetries + 1u;
    CHECK_EQ(f_open(&f, "/games/names/Z/ZH/Zhuk/DEEP.TRD", FA_READ), FR_OK);
    CHECK(f_read(&f, buf, sizeof buf, &got) != FR_OK);
    f_close(&f);
    g_cuts = 0;
    unmount();
}

void test_site_refusals() {
    std::printf("test_zxart_site_refusals\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=ZA")] = Reply{200, "{\"totalAmount\":0,\"responseData\":{},\"responseStatus\":\"error\"}"};
    mount();
    FRESULT r = FR_OK;
    // Ответ с ошибкой сайта - не пустой каталог.
    CHECK_EQ(join(names("/games/names/Z/ZA")), std::string("ERROR"));
    CHECK_EQ(zxart::zxart_stats().format_errors, 1u);
    // Сразу ещё раз - без запроса; через паузу - запрос.
    const size_t sent = g_requests.size();
    CHECK_EQ(join(names("/games/names/Z/ZA")), std::string("ERROR"));
    CHECK_EQ(g_requests.size(), sent);
    CHECK(zxart::zxart_stats().throttled >= 1u);
    g_test_time_us += (zxart::kRetryMs + 1000u) * 1000u;
    names("/games/names/Z/ZA", &r);
    CHECK_EQ(g_requests.size(), sent + 1u);
    // 429 - сайт велел ждать: запросы стоят и по другим ключам.
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=ZB")] = Reply{429, "slow down"};
    names("/games/names/Z/ZB", &r);
    const size_t after_429 = g_requests.size();
    CHECK_EQ(join(names("/games/names/Z/ZD")), std::string("ERROR"));
    CHECK_EQ(g_requests.size(), after_429);
    unmount();
    // zxart_init снимает запрет 429: запрос уходит сразу.
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=ZD")] = Reply{200, list_json("zxProd", 1, {prod_json(9, "Zed", 1989)})};
    mount();
    CHECK_EQ(join(names("/games/names/Z/ZD")), std::string("Zed"));
    CHECK_EQ(g_requests.size(), 1u);
    unmount();
}

// Сущности в названии: знаки в два и три байта UTF-8.
void test_title_entities() {
    std::printf("test_zxart_title_entities\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=ZE")] =
        Reply{200, list_json("zxProd", 1, {prod_json(5, "A&#1046;&#x401;&#x2014;&#8364;z&#0;&#65536;", 1990)})};
    mount();
    // Ж и Ё в cp866 - 0x86 и 0xF0, длинное тире - '-', евро - '_'; &#0; и
    // знак вне BMP остаются текстом.
    CHECK_EQ(join(names("/games/names/Z/ZE")), std::string("A\x86\xf0-_z&#0;&#65536;"));
    unmount();
}

// Текущий год - последний каталог годов; zxart_init возвращает год сборки.
void test_year_reset() {
    std::printf("test_zxart_year_reset\n");
    mount();
    zxart::zxart_set_year(2030);
    auto years = names("/games/years");
    CHECK(years.size() >= 2u && years[years.size() - 2u] == "2030");
    unmount();
    mount();
    years = names("/games/years");
    CHECK(years.size() >= 2u && years[years.size() - 2u] == "2026");
    unmount();
}

// Пачка заранее не удалась: предзагрузка ждёт kRetryMs, потом снова.
void test_prefetch_pause() {
    std::printf("test_zxart_prefetch_pause\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=AB")] =
        Reply{200, list_json("zxProd", 2, {prod_json(11, "Abaca", 1990), prod_json(12, "Abakus", 1991)})};
    mount(true);
    CHECK_EQ(join(names("/games/names/A/AB")), std::string("Abaca|Abakus"));
    const size_t listed = g_requests.size();
    for (int i = 0; i < 5; ++i) {
        g_test_time_us += 400000u;
        zxart::zxart_poll();
        serve_pending();
    }
    CHECK_EQ(g_requests.size(), listed + 1u);
    CHECK_EQ(zxart::zxart_stats().prefetch_failed, 1u);
    g_test_time_us += zxart::kRetryMs * 1000u;
    zxart::zxart_poll();
    serve_pending();
    CHECK_EQ(g_requests.size(), listed + 2u);
    CHECK_EQ(zxart::zxart_stats().prefetch_failed, 2u);
    CHECK_EQ(zxart::zxart_stats().queued, 2u);
    unmount();
}

void test_transport_trouble() {
    std::printf("test_zxart_transport_trouble\n");
    g_replies[prods_path("zxProdCategory=92177;zxProdFirstLetter=TA")] = Reply{200, list_json("zxProd", 1, {prod_json(7, "Tank", 1985)})};
    mount();
    // Транспорт занят - запрос повторяется, гость ошибки не видит.
    g_busy = 3;
    CHECK_EQ(join(names("/games/names/T/TA")), std::string("Tank"));
    // Запрос принят, ответа нет - по сроку запрос брошен и повторён, после
    // kRetries повторов - отказ.
    g_silent = true;
    CHECK_EQ(join(names("/games/names/T/TC")), std::string("ERROR"));
    CHECK_EQ(zxart::zxart_stats().timeouts, zxart::kRetries + 1u);
    CHECK_EQ(g_cancels, zxart::kRetries + 1u);
    // Транспорт не берёт запрос дольше kQueueMs (сети нет) - отказ, а не
    // вечное ожидание.
    g_silent = false;
    g_busy   = 1000;
    CHECK_EQ(join(names("/games/names/T/TE")), std::string("ERROR"));
    CHECK(zxart::zxart_stats().busy == 1u);
    g_busy = 0;
    unmount();
}

} // namespace

void run_zxart_tests() {
    test_files_and_names();
    test_site_refusals();
    test_transport_trouble();
    test_real_responses();
    test_json_stream();
    test_static_tree();
    test_prods_and_files();
    test_authors_paging();
    test_years();
    test_genres();
    test_best();
    test_author_sections();
    test_prefetch();
    test_failures();
    test_title_entities();
    test_year_reset();
    test_prefetch_pause();
}

// Сеть платы (net/hal/net.h) на ПК - подставной транспорт этого файла.
bool net::hal::net_ready() {
    return true;
}
bool net::hal::net_http_get(const char* path, bool /*stream*/) {
    return http_get(path);
}
void net::hal::net_http_cancel() {
    http_cancel();
}
void net::hal::net_pump() {
    serve_pending();
}
