// SPDX-License-Identifier: MIT
// Загрузка по протоколу v2: реальный HostProtocol (не старая заглушка из
// test_bus_byte_source.cpp), синхронный насос fake-хоста в стиле
// test_host_protocol.cpp (testing_host::Link и host_step из
// host_protocol_link.h), player::load::BusByteSource + run_session_load
// на файлах корпуса. Единственный способ проверить эту связку без платы.

#include "testing.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <vector>

#include "host_protocol_link.h"
#include "song_compare.h"

#include "core/engine/sequencer.h"
#include "player/load/bus_byte_source.h"
#include "player/protocol/host_protocol.h"
#include "core/formats/memory_byte_source.h"
#include "player/load/sample_prefetch.h"
#include "player/load/session_loader.h"
#include "core/memory/track_memory.h"

using namespace player::load;
using namespace player::protocol;
using soundsinth::formats::MemoryByteSource;

namespace {

// Открыть сессию так, как это делает плагин: команда HcStart с длиной
// файла, размером сектора и флагами телеметрии.
void start_session(testing_host::Link& link, uint32_t file_size, uint8_t telemetry_flags) {
    link.command(HostProtocol::kHcStart, static_cast<uint8_t>(file_size), static_cast<uint8_t>(file_size >> 8), static_cast<uint8_t>(file_size >> 16),
                 static_cast<uint8_t>(file_size >> 24),
                 /*sector=512*/ 2, telemetry_flags);
}

// "Хост" держит файл целиком у себя (как WC-хост на своём накопителе) и
// отвечает на запросы чтения: kStReadFast - всегда полные 4096 байт,
// kStReadSlow - ровно length байт, без набивки. Считает round-trip'ы -
// регрессионная проверка того, что BusByteSource кэширует, а не
// обращается к шине на каждое поле.
struct FakeHost {
    testing_host::Link link;
    const uint8_t* file_data = nullptr;
    uint32_t file_size       = 0;
    uint32_t round_trips     = 0;

    // Как на железе: основной поток крутит poll() во время ожидания файлового
    // чанка, а хост вычитывает телеметрию, то есть между запросом файла и
    // ответом по той же паре портов ходит посторонний трафик. Без этого стенд
    // проверял бы загрузку в вакууме.
    uint32_t telemetry_frames                       = 0;
    uint8_t scratch[HostProtocol::kDataBufferBytes] = {};
    // Телеметрия меняется во время загрузки, как у оркестратора: кадры
    // телеметрии чередуются с кадрами чтения.
    bool live_telemetry = false;
    uint32_t pumps      = 0;

    static void read_window(void* user, uint32_t offset, uint16_t len, uint8_t* dst) {
        auto* self = static_cast<FakeHost*>(user);
        ++self->round_trips;
        for (uint16_t i = 0; i < len; ++i) {
            const uint32_t pos = offset + i;
            dst[i]             = pos < self->file_size ? self->file_data[pos] : 0;
        }
    }

    static void pump(void* user) {
        auto* self = static_cast<FakeHost*>(user);
        if (self->live_telemetry) {
            ++self->pumps;
            self->link.p->set_position(0, static_cast<uint8_t>((self->pumps / 8u) % 60u), player::protocol::PlaybackState::Loading);
            self->link.p->set_psram_stats(100, static_cast<uint16_t>(self->pumps % 100u), 0);
        }
        self->link.p->poll(); // плата вооружает следующую команду
        const uint8_t st = self->link.poll();
        if (st >= HostProtocol::kStFileInfo && st <= HostProtocol::kStEngineLoad) ++self->telemetry_frames;
        testing_host::host_step(self->link, self, &read_window, self->scratch);
    }
};

// Порт логики плагина: stream_seek()/stream_prime() и чтение окна из
// frontend/plugin/src/main.c (раньше seek_to_aligned() +
// file_seek_and_fill() в plugin/src/bus_client.c).
//
// Зачем отдельный хост: FakeHost выше - идеальный хост с произвольным
// доступом (file_data[offset + i]). WC-хост так не умеет: у него
// последовательный поток и своя переменная позиции (s_stream_pos),
// перемотка назад - только через wc_gipagpl() в начало файла плюс
// wc_loadnone() посекторно вперёд. Этого состояния в стенде не было,
// поэтому любая ошибка в этой арифметике ловилась железом, а тестами нет.
//
// Заведено по логу 2026-08-23: на .it стабильно, на тех же индексах,
// падали отдельные сэмплы, при этом те же сэмплы из памяти
// (test_every_sample_loads_standalone) и через FakeHost распаковывались
// без ошибок. Диагностика говорила "перечитанный блок отличается", а
// повторное чтение начинается с seek(0), то есть с перемотки, которая
// исправляет рассинхрон позиции. Всё указывало сюда.
//
// Модель должна повторять плагин дословно, включая то, что выглядит
// избыточным: иначе тест зеленеет там, где железо красное.
struct SeekingHost {
    testing_host::Link link;
    const uint8_t* file_data                        = nullptr;
    uint32_t file_size                              = 0;
    uint32_t round_trips                            = 0;
    uint32_t telemetry_frames                       = 0;
    uint8_t scratch[HostProtocol::kDataBufferBytes] = {};
    // Сколько раз хост отдал не то, что у него просили. На железе такого
    // счётчика нет и быть не может (в кадре данных нет поля с фактическим
    // смещением), здесь он есть - ради него модель и заведена.
    uint32_t served_wrong_offset = 0;

    uint32_t stream_pos = 0;
    uint32_t rewinds    = 0; // прыжков назад - у WC перечитывание файла с начала

    // Байт из потока: то, что лежит по позиции потока, а не по запрошенному
    // смещению. Разница между ними и есть искомый баг.
    uint8_t stream_byte(uint32_t pos) const { return pos < file_size ? file_data[pos] : 0; }

    // Поведение WC, измеренное на железе: сразу после wc_gipagpl() поток не
    // "прогрет", и wc_loadnone() его не двигает; первое настоящее чтение
    // (wc_load512) прогревает, дальше пропуски работают.
    //
    // Это вывод из данных, а не догадка об устройстве WC: на железе МК
    // изредка получала на запрос окна W первые 4096 байт файла, байт в байт
    // (подтверждено на 6 случаях из 8, три разных .it, включая сигнатуру
    // IMPM). Портилось одно окно - первое после прыжка назад.
    //
    // Модель нарочно воспроизводит сломанное поведение: тогда отсутствие
    // прогрева в плагине сразу роняет тест, а не всплывает на плате через
    // неделю.
    bool primed = false;

    void seek_to_aligned(uint32_t aligned_offset) {
        if (aligned_offset < stream_pos) {
            ++rewinds;
            stream_pos = 0; // wc_gipagpl()
            primed     = false;
            // Порт исправления из плагина (stream_prime в frontend/plugin/src/main.c):
            // прогрев настоящим односекторным чтением вместо пропуска.
            if (aligned_offset >= 512u) {
                stream_pos = 512u;
                primed     = true;
            }
        }
        uint32_t delta_blocks = (aligned_offset - stream_pos) / 512u;
        if (delta_blocks > 32768u) delta_blocks = 0;
        while (delta_blocks > 0) {
            const uint8_t chunk = static_cast<uint8_t>(delta_blocks > 255u ? 255u : delta_blocks);
            if (primed) stream_pos += static_cast<uint32_t>(chunk) * 512u; // wc_loadnone(chunk)
            delta_blocks -= chunk;
        }
    }

    // wc_load512(dest, blocks): читает blocks*512 байт с текущей позиции
    // потока и двигает её. Возвращает начало прочитанного окна.
    uint32_t load512(uint8_t blocks) {
        const uint32_t from  = stream_pos;
        stream_pos          += static_cast<uint32_t>(blocks) * 512u;
        primed               = true;
        return from;
    }

    // Окно запроса: выровнять начало на сектор, перемотать поток как плагин,
    // прочитать load512 и отдать байты по фактической позиции потока.

    static void read_window(void* user, uint32_t offset, uint16_t len, uint8_t* dst) {
        auto* self = static_cast<SeekingHost*>(user);
        ++self->round_trips;

        const uint16_t pad     = static_cast<uint16_t>(offset % 512u);
        const uint32_t aligned = offset - pad;
        const uint16_t need    = static_cast<uint16_t>(pad + len);
        const uint8_t blocks   = static_cast<uint8_t>((need + 511u) / 512u);

        self->seek_to_aligned(aligned);
        const uint32_t from = self->load512(blocks);
        CHECK_EQ(from, aligned); // хост должен отдать запрошенное окно
        if (from != aligned) ++self->served_wrong_offset;

        for (uint16_t i = 0; i < len; ++i)
            dst[i] = self->stream_byte(from + pad + i);
    }

    static void pump(void* user) {
        auto* self = static_cast<SeekingHost*>(user);
        self->link.p->poll();
        const uint8_t st = self->link.poll();
        if (st >= HostProtocol::kStFileInfo && st <= HostProtocol::kStPsramStats) ++self->telemetry_frames;
        testing_host::host_step(self->link, self, &read_window, self->scratch);
    }
};

std::vector<uint8_t> read_whole_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Загружает файл через BusByteSource, сверяет с прямой загрузкой через
// MemoryByteSource и проверяет значения телеметрии file_info
// (minutes/seconds всегда BCD, счётчики - int16).
void check_file_loads_over_bus(const char* path) {
    const std::vector<uint8_t> file_bytes = read_whole_file(path);
    if (file_bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    const uint32_t file_size = static_cast<uint32_t>(file_bytes.size());

    // --- Эталон: прямая загрузка через MemoryByteSource ---
    soundsinth::memory::TrackMemory mem_ref;
    soundsinth::memory::track_memory_create(mem_ref);
    soundsinth::model::Song song_ref;
    {
        MemoryByteSource mbs(file_bytes.data(), file_size);
        player::load::SessionLoadResult load;
        const bool ok = player::load::run_session_load(mbs.as_byte_source(), mem_ref, song_ref, load);
        CHECK(ok);
        if (!ok) {
            std::printf("  reference load failed %s: %s\n", path, load.error ? load.error : "?");
            soundsinth::memory::track_memory_destroy(mem_ref);
            return;
        }
    }

    // --- Через шину ---
    HostProtocol protocol;
    FakeHost host;
    host.link.attach(protocol);
    host.file_data = file_bytes.data();
    host.file_size = file_size;

    // sector_size=2 (512 Б), telemetry_flags=kTelemetryFileInfo, счётчики int16 (не BCD)
    start_session(host.link, file_size, HostProtocol::kTelemetryFileInfo);

    soundsinth::memory::TrackMemory mem_bus;
    soundsinth::memory::track_memory_create(mem_bus);
    BusByteSource bus_src(protocol, file_size, &FakeHost::pump, &host);

    soundsinth::model::Song song_bus;
    player::load::SessionLoadResult load;
    const bool ok = player::load::run_session_load(bus_src.as_byte_source(), mem_bus, song_bus, load);
    CHECK(ok);
    if (!ok) {
        std::printf("  load over the bus failed %s: %s\n", path, load.error ? load.error : "?");
        soundsinth::memory::track_memory_destroy(mem_ref);
        soundsinth::memory::track_memory_destroy(mem_bus);
        return;
    }

    song_compare::check_songs_equal(song_ref, mem_ref.psram, song_bus, mem_bus.psram, &mem_ref.sample_cache, &mem_bus.sample_cache);

    // Регрессия на кэш с чтением вперёд (без него - сотни и тысячи
    // round-trip'ов).
    const uint32_t expected_upper_bound = file_size / HostProtocol::kDataBufferBytes + 50;
    CHECK(host.round_trips < expected_upper_bound);
    CHECK_EQ(host.link.unaligned, 0u);
    CHECK(host.link.slow <= 1u);
    std::printf("  %s: %u bytes, %u round trip(s) (expected < %u)\n", path, file_size, host.round_trips, expected_upper_bound);

    // --- Телеметрия file_info ---
    // Выставляет вызывающий (на плате - player::publish_file_info): минуты и
    // секунды из длительности, счётчики из Song.
    CHECK(load.total_frames > 0);
    const uint32_t total_seconds = load.total_frames / soundsinth::engine::kSampleRateHz;
    protocol.set_file_info(static_cast<uint8_t>((total_seconds / 60u) % 100u), static_cast<uint8_t>(total_seconds % 60u), song_bus.sample_count,
                           song_bus.pattern_count, song_bus.instrument_count);
    protocol.mark_session_ready();
    // В v2 телеметрия - обычная команда: крутим сторону хоста, пока плата
    // не выставит kStFileInfo, и читаем её аргументы как любые другие.
    uint32_t guard = 0;
    while (host.link.poll() != HostProtocol::kStFileInfo && guard < 1000) {
        protocol.poll();
        if (host.link.poll() == HostProtocol::kStFileInfo) break;
        testing_host::host_step(host.link, &host, &FakeHost::read_window, host.scratch);
        ++guard;
    }
    CHECK_EQ(host.link.poll(), static_cast<uint8_t>(HostProtocol::kStFileInfo));
    uint8_t info[9]; // 8 байт + контрольный
    CHECK(host.link.read_args(info, sizeof(info)));
    // minutes/seconds - всегда BCD.
    const uint8_t minutes          = static_cast<uint8_t>(((info[0] >> 4) & 0x0F) * 10 + (info[0] & 0x0F));
    const uint8_t seconds          = static_cast<uint8_t>(((info[1] >> 4) & 0x0F) * 10 + (info[1] & 0x0F));
    const uint16_t num_samples     = static_cast<uint16_t>(info[2] | (info[3] << 8));
    const uint16_t num_patterns    = static_cast<uint16_t>(info[4] | (info[5] << 8));
    const uint16_t num_instruments = static_cast<uint16_t>(info[6] | (info[7] << 8));

    CHECK_EQ(num_samples, song_bus.sample_count);
    CHECK_EQ(num_patterns, song_bus.pattern_count);
    CHECK_EQ(num_instruments, song_bus.instrument_count);
    CHECK(minutes < 60); // разумная длительность, а не 0 без вычисления
    std::printf("  %s: file_info %u:%02u, samples=%u patterns=%u instruments=%u\n", path, minutes, seconds, num_samples, num_patterns, num_instruments);

    soundsinth::memory::track_memory_destroy(mem_ref);
    soundsinth::memory::track_memory_destroy(mem_bus);
}

void test_bus_session_load_matches_direct_load() {
    std::printf("test_bus_session_load_matches_direct_load\n");
    check_file_loads_over_bus("SD/test_music/mod/ptiswap.mod");
    check_file_loads_over_bus("SD/test_music/xm/001.xm");
    check_file_loads_over_bus("SD/test_music/s3m/2nd_pm.s3m");
    check_file_loads_over_bus("SD/test_music/it/00009.it");
}

// Воспроизведение условий железа, при которых нашли отказ (2026-08-22,
// лог "session load FAILED ... сигнатура MOD" на
// SD/test_music/xm/final_fantasy.xm, который на PC из обычного файла
// грузится и играет). Условия того времени, протокол v1:
//   - telemetry_flags = 0x73 - то, что тогда слал плагин
//     (file_info+position+psram + BCD-флаги), а не только
//     kTelemetryFileInfo, как в тесте выше;
//   - насос хоста обслуживает протокол во время загрузки, как тогда
//     делал bus_pump() в backend/ports/rp2350/bus/session_orchestrator.cpp
//     (добавлено ради телеметрии во время загрузки); здесь это
//     p->poll() в FakeHost::pump;
//   - хост, как bus_start_session() плагина, не читал 0xCF при
//     телеметрийных статусах, а игнорировал их и опрашивал статус
//     дальше.

void check_loads_with_live_telemetry(const char* path) {
    const std::vector<uint8_t> file_bytes = read_whole_file(path);
    if (file_bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    const uint32_t file_size = static_cast<uint32_t>(file_bytes.size());

    HostProtocol protocol;
    FakeHost host;
    host.link.attach(protocol);
    host.file_data = file_bytes.data();
    host.file_size = file_size;

    // 0x73 - флаги, которые слал плагин на момент отказа. Текущий
    // frontend/common/bus_client.c шлёт FILE_INFO|POSITION|PSRAM|LOAD = 0x33.
    start_session(host.link, file_size, 0x73);
    host.live_telemetry = true;

    soundsinth::memory::TrackMemory mem;
    soundsinth::memory::track_memory_create(mem);
    BusByteSource bus_src(protocol, file_size, &FakeHost::pump, &host);

    soundsinth::model::Song song;
    player::load::SessionLoadResult load;
    const bool ok = player::load::run_session_load(bus_src.as_byte_source(), mem, song, load);
    CHECK(ok);
    // Кадры телеметрии действительно чередовались с чтением, без NAK и
    // переполнения приёма.
    CHECK(host.telemetry_frames >= host.round_trips / 2u);
    CHECK_EQ(protocol.nak_total(), 0u);
    CHECK_EQ(protocol.data_overflow(), 0u);
    CHECK_EQ(host.link.unaligned, 0u);
    CHECK(host.link.slow <= 1u);
    if (!ok)
        std::printf("  LOAD FAILED %s: %s\n", path, load.error ? load.error : "?");
    else
        std::printf("  %s: OK, %u round trip(s), telemetry frames %u\n", path, host.round_trips, host.telemetry_frames);
    // в v2 хост не разбирает код команды вручную, этим занят host_step.

    soundsinth::memory::track_memory_destroy(mem);
}

// Хост подтверждает окно короче, чем позиция чтения внутри него (DONE с
// длиной 100 на чтение с 300): read_fn отдаёт короткое чтение за один
// запрос, а не просит то же окно без конца.
struct ShortDoneHost {
    testing_host::Link link;
    uint32_t requests                               = 0;
    uint8_t scratch[HostProtocol::kDataBufferBytes] = {};

    static void pump(void* user) {
        auto* self = static_cast<ShortDoneHost*>(user);
        self->link.p->poll();
        const uint8_t st = self->link.poll();
        if (st != HostProtocol::kStReadFast && st != HostProtocol::kStReadSlow) {
            testing_host::host_step(self->link, self, [](void*, uint32_t, uint16_t, uint8_t*) {}, self->scratch);
            return;
        }
        uint8_t a[13];
        self->link.read_args(a, sizeof(a));
        const uint16_t length = static_cast<uint16_t>(a[4] | (a[5] << 8));
        const uint16_t sent   = st == HostProtocol::kStReadFast ? HostProtocol::kDataBufferBytes : length;
        ++self->requests;
        // После пятого запроса - честное окно, чтобы тест не висел, если
        // защита сломана.
        const uint16_t claimed = self->requests > 5 ? sent : 100;
        self->link.send_data(self->scratch, sent);
        self->link.done(claimed, testing_host::Link::data_sum(self->scratch, claimed));
    }
};

void test_bus_short_done_does_not_repeat_window() {
    std::printf("test_bus_short_done_does_not_repeat_window\n");
    HostProtocol proto;
    ShortDoneHost host;
    host.link.attach(proto);
    start_session(host.link, 10000, HostProtocol::kTelemetryFileInfo);
    BusByteSource src(proto, 10000, &ShortDoneHost::pump, &host);
    soundsinth::formats::ByteSource bs = src.as_byte_source();
    CHECK(bs.seek(bs.self, 300));
    uint8_t out[10];
    CHECK_EQ(bs.read(bs.self, out, sizeof(out)), 0u);
    CHECK_EQ(host.requests, 1u);
    CHECK_EQ(proto.short_done(), 1u);
}

void test_bus_session_load_survives_telemetry_during_load() {
    std::printf("test_bus_session_load_survives_telemetry_during_load\n");
    check_loads_with_live_telemetry("SD/test_music/xm/final_fantasy.xm");
    check_loads_with_live_telemetry("SD/test_music/xm/001.xm");
    check_loads_with_live_telemetry("SD/test_music/it/00009.it");
}

// Сброс (0x01) посреди многочанковой загрузки: BusByteSource должен
// быстро вернуть короткое чтение (не зависнуть), run_session_load -
// чисто провалиться.
void test_bus_session_load_aborts_cleanly_on_reset() {
    std::printf("test_bus_session_load_aborts_cleanly_on_reset\n");
    const std::vector<uint8_t> file_bytes = read_whole_file("SD/test_music/xm/001.xm");
    if (file_bytes.empty()) {
        std::printf("  SKIP (file not found)\n");
        return;
    }
    const uint32_t file_size = static_cast<uint32_t>(file_bytes.size());

    HostProtocol protocol;
    FakeHost host;
    host.link.attach(protocol);
    host.file_data = file_bytes.data();
    host.file_size = file_size;
    start_session(host.link, file_size, 0);

    soundsinth::memory::TrackMemory mem;
    soundsinth::memory::track_memory_create(mem);

    // Оборвать после третьего round-trip'а - "хост" присылает 0x01 вместо
    // очередного ответа на чанк.
    struct AbortAfterN {
        FakeHost* host;
        HostProtocol* protocol;
        static void pump(void* user) {
            auto* self = static_cast<AbortAfterN*>(user);
            if (self->host->round_trips >= 3) {
                uint8_t reset_cmd[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
                for (uint8_t b : reset_cmd)
                    self->protocol->on_command_byte(b);
                // В v2 команда хоста разбирается в poll(), а не в момент записи байта:
                // приём в ISR должен быть тривиальным. Без этого вызова сброс не
                // применится, и ожидание данных станет вечным.
                self->protocol->poll();
                return;
            }
            FakeHost::pump(self->host);
        }
    };
    AbortAfterN abort_pump{&host, &protocol};
    BusByteSource bus_src(protocol, file_size, &AbortAfterN::pump, &abort_pump);

    HostProtocol::Callbacks cb;
    cb.user     = &bus_src;
    cb.on_reset = [](void* user) { static_cast<BusByteSource*>(user)->on_reset(); };
    protocol.set_callbacks(cb);

    soundsinth::model::Song song;
    player::load::SessionLoadResult load;
    const bool ok = player::load::run_session_load(bus_src.as_byte_source(), mem, song, load);
    CHECK(!ok); // должно провалиться (сброс посреди загрузки), а не зависнуть
    std::printf("  failed correctly after the reset: %s\n", load.error ? load.error : "?");

    soundsinth::memory::track_memory_destroy(mem);
}

// Прогрессивная загрузка через шину. Отличие от
// test_deferred_sample_load.cpp: там источником был MemoryByteSource, и
// кэш, выравнивание и перемотки BusByteSource не участвовали.
// Прогрессивная загрузка меняет порядок доступа к файлу: сначала
// разбросанные сэмплы префетча, потом хвост, - и ошибка в кэше источника
// проявилась бы здесь, битыми сэмплами во время игры.
//
// Сверка - побайтовая, с обычной последовательной загрузкой, вместе с
// точками Dpcm8 (song_compare::collect_sample_bytes).

// Параметризован типом хоста: FakeHost (произвольный доступ) и
// SeekingHost (последовательный поток плагина) должны давать одинаковый
// результат. Расхождение = баг в позиционировании на стороне хоста.
template <typename Host>
void check_progressive_load_over_bus(const char* path) {
    const std::vector<uint8_t> file_bytes = read_whole_file(path);
    if (file_bytes.empty()) {
        std::printf("  SKIP (file not found): %s\n", path);
        return;
    }
    const uint32_t file_size = static_cast<uint32_t>(file_bytes.size());

    // --- Эталон: обычная полная загрузка (тоже через шину, чтобы
    // отличие было только в порядке, а не в источнике) ---
    soundsinth::memory::TrackMemory mem_ref;
    soundsinth::memory::track_memory_create(mem_ref);
    soundsinth::model::Song song_ref;
    HostProtocol proto_ref;
    FakeHost host_ref;
    host_ref.link.attach(proto_ref);
    host_ref.file_data = file_bytes.data();
    host_ref.file_size = file_size;
    {
        start_session(host_ref.link, file_size, HostProtocol::kTelemetryFileInfo);
        BusByteSource src(proto_ref, file_size, &FakeHost::pump, &host_ref);
        player::load::SessionLoadResult load;
        CHECK(player::load::run_session_load(src.as_byte_source(), mem_ref, song_ref, load));
    }

    // --- Прогрессивно: метаданные -> план -> сэмплы по одному ---
    soundsinth::memory::TrackMemory mem;
    soundsinth::memory::track_memory_create(mem);
    soundsinth::model::Song song;
    HostProtocol proto;
    Host host;
    host.link.attach(proto);
    host.file_data = file_bytes.data();
    host.file_size = file_size;
    start_session(host.link, file_size, HostProtocol::kTelemetryFileInfo);
    BusByteSource src(proto, file_size, &Host::pump, &host);

    player::load::SessionLoadResult load;
    CHECK(player::load::run_session_load(src.as_byte_source(), mem, song, load, /*metadata_only=*/true));
    const player::load::TrackFormat format = load.format;
    CHECK(format != player::load::TrackFormat::None);
    const uint32_t metadata_round_trips = host.round_trips;

    std::vector<uint16_t> plan(song.sample_count == 0 ? 1 : song.sample_count);
    std::vector<uint16_t> last_use(plan.size());
    const player::load::PlaybackPlan planned =
        player::load::plan_playback_order(song, mem.psram, plan.data(), static_cast<uint16_t>(plan.size()), last_use.data());
    const uint16_t count    = planned.count;
    const uint16_t prefetch = planned.prefetch_count;

    uint32_t mismatched = 0;
    for (uint16_t k = 0; k < count; ++k) {
        // Порядок оркестратора: префетч -> mark_session_ready() (звук
        // стартует, телеметрия начинает предлагаться хосту) -> фоновый хвост.
        // На хвосте по шине идёт посторонний трафик, и там на железе
        // отваливались сэмплы.
        if (k == prefetch) {
            proto.mark_session_ready();
        }
        // Между сэмплами - то же, что внешний цикл оркестратора: poll() плюс
        // вычитка предложенной телеметрии хостом. Внутри одного файлового
        // обмена телеметрия вклиниться не может (протокол их сериализует), а
        // в паузах идёт.
        for (int t = 0; t < 4; ++t)
            Host::pump(&host);
        const uint16_t idx = plan[k];
        const char* why    = "?";
        const bool loaded  = player::load::load_track_sample(format, src.as_byte_source(), mem, song, idx, &why);
        if (!loaded) std::printf("    sample %u NOT loaded: %s\n", idx, why);
        CHECK(loaded);
        const std::vector<uint8_t> expect = song_compare::collect_sample_bytes(mem_ref.psram, mem_ref.sample_cache, idx, song_ref.samples[idx]);
        const std::vector<uint8_t> got    = song_compare::collect_sample_bytes(mem.psram, mem.sample_cache, idx, song.samples[idx]);
        if (got != expect) {
            ++mismatched;
            // Печатаем первое расхождение с подробностями: по нему видно, сдвиг
            // это (кэш отдал не то окно) или отдельные байты.
            if (mismatched == 1) {
                size_t first_bad = 0;
                while (first_bad < got.size() && first_bad < expect.size() && got[first_bad] == expect[first_bad]) {
                    ++first_bad;
                }
                std::printf("    MISMATCH sample %u: got %u bytes, expected %u, first bad byte %u\n", idx, static_cast<unsigned>(got.size()),
                            static_cast<unsigned>(expect.size()), static_cast<unsigned>(first_bad));
            }
        }
    }
    CHECK_EQ(mismatched, 0u);
    // Запросы с начала сектора; медленный - только хвост файла, по разу на
    // метаданные и на сэмплы (у XM заголовки инструментов идут до конца).
    CHECK_EQ(host.link.unaligned, 0u);
    CHECK(host.link.slow <= 2u);
    if constexpr (std::is_same_v<Host, SeekingHost>) {
        CHECK_EQ(host.served_wrong_offset, 0u);
        // План по файлу: назад - после метаданных к первому сэмплу префетча
        // и когда впереди ничего не осталось.
        CHECK(host.rewinds <= 2u);
    }

    std::printf("  %s: progressively over the bus %u samples (prefetch %u), round trips %u metadata + %u samples"
                " (sequentially %u)\n",
                path, count, prefetch, metadata_round_trips, host.round_trips - metadata_round_trips, host_ref.round_trips);
    std::printf("    slow requests %u\n", host.link.slow);
    if constexpr (std::is_same_v<Host, SeekingHost>) std::printf("    rewinds %u\n", host.rewinds);
    std::printf("    telemetry frames during exchange pauses: %u\n", host.telemetry_frames);

    soundsinth::memory::track_memory_destroy(mem);
    soundsinth::memory::track_memory_destroy(mem_ref);
}

void test_progressive_load_over_bus_matches_sequential() {
    std::printf("test_progressive_load_over_bus_matches_sequential\n");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/ivi-lite__v61.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/00009.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/s3m/2nd_reality.s3m");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/xm/final_fantasy.xm");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/xm/000h_cara_mia.xm"); // 4.5 МБ, медленный старт на железе
    // На железе на этом файле два сэмпла приезжали битыми в каждом
    // прогоне - 253 и 207, и оба оказались первым 16-битным сэмплом после
    // серии 8-битных. Файл добавлен, чтобы этот переход проверялся
    // эмуляцией шины, а не только на плате.
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/bz_ult9.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/mod/star_wars.mod");
    // Файлы, на которых железо стабильно теряет по нескольку сэмплов
    // (лог 2026-08-23, размеры 723526/820189/467564/490208/328983). Провалы
    // повторяются на тех же индексах от прогона к прогону, а сами сэмплы
    // отдельно, из памяти, распаковываются без ошибок
    // (test_every_sample_loads_standalone) - значит виноват путь через шину,
    // и он должен ломаться здесь же.
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/00012 ladda upp denna.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/038djzjack_littlerock.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/deeper__v54.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/dg_pcorn__v50.it");
    check_progressive_load_over_bus<FakeHost>("SD/test_music/it/dg_reald__v55.it");
}

// Тот же прогон, но через последовательный хост, повторяющий
// позиционирование плагина (SeekingHost выше). Идеальный хост с
// произвольным доступом такие ошибки пропускает по построению.
void test_progressive_load_through_seeking_host() {
    std::printf("test_progressive_load_through_seeking_host\n");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/00012 ladda upp denna.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/038djzjack_littlerock.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/deeper__v54.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/dg_pcorn__v50.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/dg_reald__v55.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/00009.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/it/ivi-lite__v61.it");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/xm/final_fantasy.xm");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/s3m/2nd_reality.s3m");
    check_progressive_load_over_bus<SeekingHost>("SD/test_music/mod/star_wars.mod");
}

// Отказ: причина у каждого загрузчика, подписи по порядку попыток, итог -
// последняя. Банка в тестах нет - .mid отказывает по банку, а не по
// сигнатуре.
void test_attempt_errors_are_labelled() {
    std::printf("test_attempt_errors_are_labelled\n");
    const char* const names[] = {"MIDI", "IT", "XM", "S3M", "MOD"};
    for (uint8_t i = 0; i < player::load::kLoaderCount; ++i) {
        CHECK(std::strcmp(player::load::session_loader_name(i), names[i]) == 0);
    }
    CHECK(std::strcmp(player::load::session_loader_name(player::load::kLoaderCount), "?") == 0);

    std::vector<uint8_t> garbage(2000, 0x55);
    std::vector<uint8_t> it_cut(64, 0);
    std::memcpy(it_cut.data(), "IMPM", 4);
    std::vector<uint8_t> mid_no_bank(64, 0);
    std::memcpy(mid_no_bank.data(), "MThd", 4);
    const std::vector<uint8_t> one_byte(1, 0);

    soundsinth::memory::TrackMemory mem;
    soundsinth::memory::track_memory_create(mem);
    const char* it_on_garbage                 = "";
    const std::vector<uint8_t>* const files[] = {&garbage, &it_cut, &mid_no_bank, &one_byte};
    for (const std::vector<uint8_t>* file : files) {
        soundsinth::model::Song song;
        soundsinth::formats::MemoryByteSource src(file->data(), static_cast<uint32_t>(file->size()));
        player::load::SessionLoadResult load;
        CHECK(!player::load::run_session_load(src.as_byte_source(), mem, song, load));
        CHECK(load.format == player::load::TrackFormat::None);
        for (const char* e : load.attempt_errors)
            CHECK(e != nullptr);
        CHECK(load.error == load.attempt_errors[player::load::kLoaderCount - 1]);
        if (file == &mid_no_bank) CHECK(std::strstr(load.attempt_errors[0], "bank") != nullptr);
        // IMPM узнан: причина IT - не та, что на мусоре.
        if (file == &garbage) it_on_garbage = load.attempt_errors[1];
        if (file == &it_cut) CHECK(std::strcmp(load.attempt_errors[1], it_on_garbage) != 0);
    }
    soundsinth::memory::track_memory_destroy(mem);
}

} // namespace

void run_session_loader_over_bus_tests() {
    test_attempt_errors_are_labelled();
    test_progressive_load_over_bus_matches_sequential();
    test_progressive_load_through_seeking_host();
    test_bus_session_load_matches_direct_load();
    test_bus_session_load_aborts_cleanly_on_reset();
    test_bus_session_load_survives_telemetry_during_load();
    test_bus_short_done_does_not_repeat_window();
}
