#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/codec/pattern_reader.h"
#include <filesystem>
#include <fstream>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "FreeRTOS.h"
#include "task.h"

#ifdef _WIN32
// См. backend/ports/pc/fatfs_diskio.cpp: ff.h на MSVC сам подключает <windows.h>
// внутри своего extern "C" { ... }, что ломает C++-перегрузки из
// <windows.h>. Подключаем заранее, снаружи любого extern "C".
#include <windows.h>
#endif
#include "ff.h"

#include "platform/memory.h"
#include "pc/fatfs_disk.h"
#include "pc/sdl_sink.h"
#include "pc/wav_writer.h"
#include "sine_source.h"
#include "player/audio/buffer_pool.h"
#include "player/config.h"
#include "core/engine/sequencer.h"
#include "core/engine/song_duration.h"
#include "core/engine/tracker_engine.h"
#include "core/engine/voice.h" // voice_decode_*_count() - см. диагностику ниже
#include "core/bank/bank_reader.h"
#include "core/formats/load_stats.h"
#include "core/formats/it.h"
#include "core/formats/midi.h"
#include "player/load/sample_prefetch.h"
#include "core/formats/mod.h"
#include "core/formats/s3m.h"
#include "core/formats/xm.h"
#include "core/formats/memory_byte_source.h"
#include "player/protocol/host_protocol.h"
#include "player/load/progressive_loader.h"
#include "player/load/session_loader.h"
#include "core/memory/scratch_arena.h" // g_max_scratch_used - замер границы scratch-арены
#include "core/memory/track_memory.h"
#include "core/audio/mixbus.h"
#include "core/codec/pattern_packer.h" // g_max_dict_bytes - замер границ упаковщика
#include "player/audio/render_task.h"

namespace {

constexpr uint32_t kSampleRate = 44100;

struct AppArgs {
    const char* wav_path = nullptr;
    const char* list_image_path = nullptr;
    const char* play_file_path = nullptr; // файл MOD/S3M/XM/IT/MID
    double seconds = 3.0;
    bool seconds_explicit = false; // --seconds задан явно - иначе для --play-file длина трека вычисляется автоматически (см. compute_song_total_frames)
    const char* by_channel_dir = nullptr; // --by-channel-dir - вдобавок к обычному сведению, по одному WAV на канал (сверка с другими плеерами)
    // --psram-kb - сколько PSRAM отдать треку. 0 = столько же, сколько на
    // плате без карты. Нужно, чтобы воспроизвести условия платы: там из
    // хранилища вырезан хвост под таблицы банка с карты, и трек, который
    // влезает на ПК, на плате может не влезть. Без этого ключа разница
    // ловилась бы только на слух и только на железе.
    uint32_t psram_kb = 0;
    // --prefetch-positions N - грузить только сэмплы первых N order-позиций
    // и играть, как это делает плата (фаза C session_orchestrator.cpp), без
    // фоновой догрузки. Нужно, чтобы "начало играет не тем составом" можно
    // было воспроизвести на ПК и посмотреть глазами, а не ловить на слух на
    // железе.
    // Умолчание - строгий путь: не влез хоть один сэмпл - трек не грузится
    // и играет тестовый синус. Это выбор пользователя: неполный состав на
    // слух опознать трудно, а синус в наборе виден сразу. Плату этим путём
    // не моделируем - она играет тем, что поместилось; для такой сверки есть
    // --partial.
    uint16_t prefetch_positions = 0;
    bool progressive = false;
    // --voice-cull - сброс лишних голосов при перегрузке, как на плате.
    // На ПК он выключен нарочно (рендер должен быть воспроизводимым), и
    // из-за этого разница "на плате звучит не так" пряталась.
    bool voice_cull = false;
    // --no-limiter - выключить лимитер и мягкое насыщение даже там, где
    // загрузчик их просит: перегруз обрезается полкой. Нужно, чтобы отделить
    // их вклад от свойств материала.
    bool no_limiter = false;
    // --linear-interp - линейная интерполяция даже там, где загрузчик
    // просит эрмитову (kQuirkHermiteInterpolation, .mid): для сравнения на
    // слух и замером.
    bool linear_interp = false;
    int32_t comp_threshold = 0;   // --comp; 0 - ключа нет, ступень лимитера выключена
    int32_t comp_amount = 0;
    // --sample-report - построчно: что загрузчик просил у банка, сколько
    // это весит и чем кончилось. По сводке "не загрузилось N" не видно,
    // что конкретно грузится.
    const char* sample_report = nullptr;
    // --unused-samples - сверка "что загрузчик положил в трек" с "на что
    // есть ссылка в паттернах": платим ли мы памятью за сэмплы, которые
    // не прозвучат ни разу.
    bool unused_samples = false;
    // --load-plan - очередь фоновой догрузки против порядка игры. Плата
    // держит упреждение (lead_positions) и грузит план строго по очереди,
    // а сам план отсортирован по смещению в файле. Ключ считает, кого
    // из-за этого не окажется в памяти к первой ноте.
    bool load_plan = false;
    // --board-load - загрузка как на плате (session_orchestrator.cpp): память
    // трека того же объёма (банк с карты, то есть больше 15 МБ, отнимает
    // хвост под свои таблицы), метаданные, план, префетч первых позиций, а
    // остальное догружается по ходу рендера с вытеснением отыгравших
    // (io/progressive_loader.h). Что не успело или не влезло, молчит, как на
    // плате, и считается в сводке. --load-kbps - скорость источника в КБ/с
    // для догрузки (0 - по умолчанию: 200 у трекеров по шине, 1000 у .mid).
    bool board_load = false;
    uint32_t load_kbps = 0;
    // --soft-knee ДОЛЯ - порог мягкого насыщения в долях шкалы для сравнения
    // на слух. 0 - умолчание шины.
    double soft_knee = 0.0;
    // --through-pool - рендер в WAV через RenderTask и пул буферов, как на
    // плате, и круги создания-остановки RenderTask (render_through_pool).
    bool through_pool = false;
    // --then ФАЙЛ - смена трека как на плате: --play-file играет 5 с через
    // шину, снос (затухание, снятие источника, деструктор движка), затем
    // ФАЙЛ в ту же память и ту же шину, его рендер - в --render-wav. Он
    // обязан совпасть с рендером ФАЙЛА на свежих объектах: разница -
    // состояние, перешедшее от прошлого трека.
    const char* then_path = nullptr;
};

// Порог мягкого насыщения из --soft-knee поверх умолчания шины; без ключа
// не трогает.
void apply_soft_curve(soundsinth::mixbus::MixBus& bus, const AppArgs& args, bool verbose) {
    if (args.soft_knee <= 0.0) return;
    bus.set_soft_knee(static_cast<int32_t>(args.soft_knee * 32767.0));
    if (verbose) std::printf("мягкое насыщение: порог %.2f шкалы\n", args.soft_knee);
}

// Расширение файла -> загрузчик формата: та же логика, что в
// backend/ports/pc/tools/mass_load_scan.cpp, но без обхода каталогов - путь
// приходит явно из --play-file. Проблема с Unicode-именами, ради которой
// там заведён ends_with_ci, для одиночного явно заданного пути не
// актуальна.
enum class SongFormat { Mod, S3m, Xm, It, Midi, Unknown };

// Банк инструментов для .mid - файл .mid несёт только номера программ.
// Задаётся ключом --bank и живёт всё время работы плеера.
std::vector<uint8_t> g_bank_bytes;
soundsinth::bank::Bank g_bank;

bool ends_with_ci(const std::string& path, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (path.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(path[path.size() - n + i])) != suffix[i]) return false;
    }
    return true;
}

SongFormat song_format_from_extension(const std::string& path) {
    if (ends_with_ci(path, ".mod")) return SongFormat::Mod;
    if (ends_with_ci(path, ".s3m")) return SongFormat::S3m;
    if (ends_with_ci(path, ".xm")) return SongFormat::Xm;
    if (ends_with_ci(path, ".it")) return SongFormat::It;
    if (ends_with_ci(path, ".mid") || ends_with_ci(path, ".midi")) return SongFormat::Midi;
    return SongFormat::Unknown;
}

// file_bytes принадлежит вызывающему коду. Song::patterns/samples/
// instruments живут в mem.resident и собираются загрузчиком из file_bytes
// во время этого вызова; formats::MemoryByteSource читает file_bytes по
// указателю только здесь, после возврата он уже не нужен.
bool load_song(const char* path, std::vector<uint8_t>& file_bytes, soundsinth::memory::TrackMemory& mem,
               soundsinth::model::Song& song, const char** error_out, bool metadata_only = false) {
    const std::string path_str(path);
    const SongFormat fmt = song_format_from_extension(path_str);
    if (fmt == SongFormat::Unknown) {
        *error_out = "неизвестное расширение файла (ожидается .mod/.s3m/.xm/.it/.mid)";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error_out = "не удалось открыть файл";
        return false;
    }
    file_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    soundsinth::formats::MemoryByteSource mbs(file_bytes.data(), static_cast<uint32_t>(file_bytes.size()));
    const auto src = mbs.as_byte_source();
    switch (fmt) {
        case SongFormat::Mod: return soundsinth::formats::mod::load(src, mem, song, error_out);
        case SongFormat::S3m: return soundsinth::formats::s3m::load(src, mem, song, error_out);
        case SongFormat::Xm: return soundsinth::formats::xm::load(src, mem, song, error_out);
        case SongFormat::It: return soundsinth::formats::it::load(src, mem, song, error_out);
        case SongFormat::Midi:
            if (!g_bank.valid()) { *error_out = "для .mid нужен банк: --bank файл.ssb"; return false; }
        {
            const bool ok = soundsinth::formats::midi::load(src, static_cast<uint32_t>(file_bytes.size()), mem, g_bank,
                                                            song, error_out, metadata_only);
            // Эти две строки разбирают скрипты замеров - формат не менять. Печатаются и при отказе: мерить надо и невлезающие.
            const auto& st = soundsinth::formats::midi::last_load_stats();
            if (st.layers_known) {
                std::printf("midi: instr %u, samples %u, keymap %u, env %u -> arena ~%u B\n",
                            unsigned(st.instruments), unsigned(st.samples), unsigned(st.keymap_ranges),
                            unsigned(st.envelopes), unsigned(st.arena_bytes));
            }
            if (st.psram_known) {
                std::printf("midi: PSRAM pages free %u, samples need %u KB, patterns %u KB\n",
                            unsigned(st.free_pages), unsigned(st.samples_need_kb), unsigned(st.patterns_kb));
            }
            if (st.steals || st.off_delay_lost || st.vib_lost || st.tempo_deferred || st.tempo_dropped) {
                std::printf("midi: потери - кража %u, снятие без задержки %u, вибрато %u, темп отложен %u, "
                            "сверх карты темпа %u\n",
                            unsigned(st.steals), unsigned(st.off_delay_lost), unsigned(st.vib_lost),
                            unsigned(st.tempo_deferred), unsigned(st.tempo_dropped));
            }
            if (st.note_restacks) {
                std::printf("midi: повторов клавиши на свой канал %u\n", unsigned(st.note_restacks));
            }
            if (st.notes_over_cap || st.notes_no_zone) {
                std::printf("midi: нот без звука - сверх 511 инструментов %u, нет зоны в банке %u\n",
                            unsigned(st.notes_over_cap), unsigned(st.notes_no_zone));
            }
            return ok;
        }
        case SongFormat::Unknown: return false;
    }
    return false;
}

// Сэмплы - по плану, как на плате: план строится обходом строк трека, и
// сэмпл, которого в нём нет, на плате не зазвучит вовсе.
//
// Грузить всё подряд было бы проще, но это прячет дыры в планировщике.
// Именно так и вышло: у .mid не стало упакованных паттернов, планировщик
// начал возвращать пустой план, а рендер на ПК этого не заметил - он уже
// держал все сэмпла в памяти. На плате трек играл бы тишину.
bool load_planned_samples(SongFormat fmt, soundsinth::formats::ByteSource src, soundsinth::memory::TrackMemory& mem,
                          const soundsinth::model::Song& song, uint16_t& loaded_out,
                          uint16_t& failed_out) {
    using player::load::TrackFormat;
    TrackFormat tf = TrackFormat::None;
    switch (fmt) {
        case SongFormat::Midi: tf = TrackFormat::Midi; break;
        case SongFormat::It: tf = TrackFormat::It; break;
        case SongFormat::Xm: tf = TrackFormat::Xm; break;
        case SongFormat::S3m: tf = TrackFormat::S3m; break;
        case SongFormat::Mod: tf = TrackFormat::Mod; break;
        case SongFormat::Unknown: return false;
    }
    static uint16_t plan_indices[4096];
    static uint16_t plan_last_use[4096];
    const player::load::PlaybackPlan plan =
        player::load::plan_playback_order(song, mem.psram, plan_indices, 4096, plan_last_use);
    loaded_out = 0;
    failed_out = 0;
    for (uint16_t i = 0; i < plan.count; ++i) {
        if (player::load::load_track_sample(tf, src, mem, song, plan_indices[i])) {
            ++loaded_out;
        } else {
            ++failed_out;
        }
    }
    return true;
}

// Диагностика: смонтировать образ через FatFs и рекурсивно перечислить
// файлы с размерами - тот же путь доступа к файлам, что у formats/mod и
// остальных загрузчиков, только на PC вместо SD-карты файл-образ.
void walk_dir(const char* path) {
    DIR dir;
    if (f_opendir(&dir, path) != FR_OK) {
        std::fprintf(stderr, "не удалось открыть каталог %s\n", path);
        return;
    }
    for (;;) {
        FILINFO fno;
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        char full[512];
        std::snprintf(full, sizeof(full), "%s/%s", path, fno.fname);
        if (fno.fattrib & AM_DIR) {
            walk_dir(full);
        } else {
            std::printf("%10lu  %s\n", static_cast<unsigned long>(fno.fsize), full);
        }
    }
    f_closedir(&dir);
}

void list_image(const char* image_path) {
    if (!platform_pc::mount_disk_image(image_path)) {
        std::fprintf(stderr, "не удалось открыть файл-образ %s\n", image_path);
        return;
    }
    static FATFS fs;
    const FRESULT res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        std::fprintf(stderr, "f_mount(%s) не удался: код %d\n", image_path, static_cast<int>(res));
        return;
    }
    walk_dir("");
}

// Офлайн-рендер в WAV-файл, быстрее реального времени, в одном потоке.
// Куски строго по SOUNDSINTH_AUDIO_BUFFER_FRAMES, как у RenderTask на плате:
// движок и сглаживание громкости считают по батчу, другой размер куска дал
// бы другой WAV. Пул буферов здесь не нужен - он для передачи между
// задачей рендера и прерыванием вывода.
// per_block - перед каждым куском (--board-load: фоновая догрузка).
void render_to_wav(soundsinth::mixbus::MixBus& bus, const char* path, uint32_t total_frames,
                   void (*per_block)(void*, uint32_t) = nullptr, void* per_block_user = nullptr) {
    platform_pc::WavWriter wav;
    if (!wav.open(path, kSampleRate)) {
        std::fprintf(stderr, "не удалось открыть %s для записи\n", path);
        return;
    }

    int16_t buf[SOUNDSINTH_AUDIO_BUFFER_FRAMES * 2];
    for (uint32_t produced = 0; produced < total_frames;) {
        const uint32_t left = total_frames - produced;
        const uint32_t chunk = left < SOUNDSINTH_AUDIO_BUFFER_FRAMES ? left : SOUNDSINTH_AUDIO_BUFFER_FRAMES;
        if (per_block) per_block(per_block_user, chunk);
        bus.render(buf, chunk);
        wav.write_frames(buf, chunk);
        produced += chunk;
    }

    wav.close();
    std::printf("записано %s (%u фреймов, %.2f c)\n", path, total_frames,
                static_cast<double>(total_frames) / kSampleRate);
}

// --- Рендер путём платы (--through-pool) ---
//
// RenderTask пишет в пул, потребитель в роли прерывания I2S забирает буферы
// вызовами *_from_isr и сразу возвращает. WAV обязан совпасть с прямым
// рендером: потерянный, повторённый или переставленный буфер меняет md5.
// Затем круги "RenderTask создать - остановить" при живом потребителе, как
// на каждой смене трека: после каждого круга все буферы пула снова в
// обороте, куча FreeRTOS до и после одна.
//
// Потребитель - задача выше RenderTask по приоритету, как прерывание. Порт
// FreeRTOS для Windows в *_from_isr прерывания не маскирует, и тик может
// переключить задачи посреди операции над очередью; критическая секция
// заменяет маску прерываний платы.
constexpr uint32_t kPoolBuffers = player::audio::BufferPool::kBufferCount;
constexpr uint32_t kPoolCycles = 100;
constexpr uint32_t kPoolStopLimitMs = 3000;

struct PoolSink {
    player::audio::BufferPool* pool = nullptr;
    platform_pc::WavWriter* wav = nullptr; // nullptr - буферы только считаются
    std::atomic<uint32_t> frames_left{0};  // сколько кадров ещё писать в wav
    std::atomic<uint32_t> reads{0};
    const int16_t* seen[kPoolBuffers] = {};
    uint32_t distinct = 0;
    uint32_t foreign = 0; // указатель вне пула или седьмой разный
    std::atomic<bool> quit{false};
    platform::Semaphore* exited = nullptr;
};

void pool_sink_task(void* arg) {
    PoolSink& s = *static_cast<PoolSink*>(arg);
    const uint32_t frames = s.pool->frames_per_buffer();
    while (!s.quit.load()) {
        taskENTER_CRITICAL();
        const int16_t* buf = s.pool->try_begin_read_from_isr();
        taskEXIT_CRITICAL();
        if (buf == nullptr) {
            platform::os_task_delay_ms(1);
            continue;
        }
        const uint32_t left = s.frames_left.load();
        if (s.wav != nullptr && left > 0) {
            const uint32_t take = left < frames ? left : frames;
            s.wav->write_frames(buf, take);
            s.frames_left.store(left - take);
        }
        uint32_t i = 0;
        while (i < s.distinct && s.seen[i] != buf) ++i;
        if (i == s.distinct) {
            if (s.distinct < kPoolBuffers) s.seen[s.distinct++] = buf;
            else ++s.foreign;
        }
        taskENTER_CRITICAL();
        s.pool->end_read_from_isr(buf);
        taskEXIT_CRITICAL();
        s.reads.fetch_add(1);
    }
    platform::os_sem_post(s.exited);
    platform::os_task_delete_self();
}

void pool_stop_slow(uint32_t waited_ms) {
    std::printf("pool: остановка RenderTask ждёт %u мс\n", (unsigned)waited_ms);
    if (waited_ms >= kPoolStopLimitMs) {
        std::printf("pool: RenderTask не остановилась - ОТКАЗ\n");
        std::exit(2);
    }
}

// Один круг: потребитель и RenderTask живут, пока потребитель не прочтёт
// min_reads буферов и не допишет WAV; stop() - при живом потребителе.
// Возвращает число разных буферов, прошедших через потребителя.
uint32_t pool_cycle(soundsinth::mixbus::MixBus& bus, player::audio::BufferPool& pool,
                    platform_pc::WavWriter* wav, uint32_t frames, uint32_t min_reads, uint32_t* foreign) {
    const size_t heap_at_start = xPortGetFreeHeapSize();
    PoolSink s;
    s.pool = &pool;
    s.wav = wav;
    s.frames_left.store(frames);
    s.exited = platform::os_sem_create(0, 1);
    platform::os_task_create(&pool_sink_task, &s, "pool_sink", 256, SOUNDSINTH_RENDER_TASK_PRIORITY + 1);
    {
        player::audio::RenderTask render_task(bus, pool);
        uint32_t idle_ms = 0;
        uint32_t last_reads = 0;
        while (s.frames_left.load() > 0 || s.reads.load() < min_reads) {
            platform::os_task_delay_ms(1);
            const uint32_t r = s.reads.load();
            idle_ms = r == last_reads ? idle_ms + 1 : 0;
            last_reads = r;
            if (idle_ms >= kPoolStopLimitMs) {
                std::printf("pool: потребитель %u мс без буферов - ОТКАЗ\n", (unsigned)idle_ms);
                std::exit(2);
            }
        }
        render_task.stop(&pool_stop_slow);
    }
    s.quit.store(true);
    platform::os_sem_wait(s.exited);
    platform::os_sem_destroy(s.exited);
    pool.drain_rendered();
    *foreign += s.foreign;
    // Стеки удалённых задач освобождает задача простоя: без ожидания
    // следующий круг не находит места в куче FreeRTOS. Под нагрузкой ПК
    // простой доходит до них не сразу.
    for (uint32_t ms = 0; xPortGetFreeHeapSize() < heap_at_start && ms < 1000; ++ms) platform::os_task_delay_ms(1);
    return s.distinct;
}

void render_through_pool(soundsinth::mixbus::MixBus& bus, player::audio::BufferPool& pool, const char* path,
                         uint32_t total_frames) {
    platform_pc::WavWriter wav;
    if (!wav.open(path, kSampleRate)) {
        std::fprintf(stderr, "не удалось открыть %s для записи\n", path);
        return;
    }
    uint32_t foreign = 0;
    uint32_t bad = 0;
    if (pool_cycle(bus, pool, &wav, total_frames, 0, &foreign) != kPoolBuffers && total_frames >= kPoolBuffers * pool.frames_per_buffer()) ++bad;
    wav.close();
    std::printf("записано %s (%u фреймов, %.2f c) через пул\n", path, total_frames,
                static_cast<double>(total_frames) / kSampleRate);

    // Удалённые задачи освобождает задача простоя - дать ей пройти.
    platform::os_task_delay_ms(10);
    const size_t heap_before = xPortGetFreeHeapSize();
    for (uint32_t c = 0; c < kPoolCycles; ++c) {
        if (pool_cycle(bus, pool, nullptr, 0, 2 * kPoolBuffers, &foreign) != kPoolBuffers) ++bad;
    }
    platform::os_task_delay_ms(10);
    const size_t heap_after = xPortGetFreeHeapSize();
    std::printf("pool: кругов %u, не все %u буферов в обороте %u, чужих %u, куча FreeRTOS %s\n",
                (unsigned)(kPoolCycles + 1), (unsigned)kPoolBuffers, (unsigned)bad, (unsigned)foreign,
                heap_before == heap_after ? "та же" : "ИЗМЕНИЛАСЬ");
    if (heap_before != heap_after) {
        std::printf("pool: куча %zu -> %zu\n", heap_before, heap_after);
    }
}

// --- Загрузка как на плате (--board-load) ---
//
// Состояние между блоками рендера: план и очередь (player::load::ProgressiveLoader),
// позиция и занятые голосами сэмплы (наблюдатель тика движка, как
// publish_tick_state у платы), бюджет байтов по скорости источника.
struct BoardLoad {
    player::load::ProgressiveLoader pl;
    uint16_t plan_mem[3 * player::load::kProgressiveMaxSamples] = {};
    player::load::TrackFormat format = player::load::TrackFormat::None;
    soundsinth::memory::TrackMemory* mem = nullptr;
    soundsinth::model::Song* song = nullptr;
    soundsinth::formats::MemoryByteSource* src = nullptr;
    uint16_t order_pos = 0;
    std::atomic<uint32_t> in_use_bits[player::load::kProgressiveMaxSamples / 32] = {};
    uint32_t total_frames = 0;
    bool position_goes_back = false;
    double bytes_per_frame = 0.0;
    double budget_bytes = 0.0;
    uint16_t prefetch_count = 0;
    uint16_t prefetch_failed = 0;
    uint32_t loaded_bg = 0;       // догружено под звук
    uint32_t late = 0;            // из них - после позиции первой ноты
    uint32_t late_positions = 0;  // суммарное опоздание, в позициях order
    uint32_t done_at_frame = 0;   // когда план кончился (0 - не кончился)
    uint32_t frames = 0;
};

// Сколько байт источника стоит сэмпл: у .mid - сжатый прогон банка (так
// он лежит во флеше или на карте), у трекеров - исходник в файле.
uint32_t board_sample_cost(const BoardLoad& b, uint16_t idx) {
    const auto& sd = b.song->samples[idx];
    if (b.format == player::load::TrackFormat::Midi) {
        const uint32_t bs = sd.file_offset;
        return bs < g_bank.header->sample_count ? g_bank.samples[bs].pcm_packed_bytes : 0u;
    }
    const uint32_t bps = (sd.resident_encoding == soundsinth::model::ResidentEncoding::Raw8) ? 1u : 2u;
    return sd.source_length_samples * sd.channels * bps;
}

bool board_load_sample(void* user, uint16_t idx, const char** reason_out) {
    auto* b = static_cast<BoardLoad*>(user);
    return player::load::load_track_sample(b->format, b->src->as_byte_source(), *b->mem, *b->song, idx, reason_out);
}

void board_tick_observer(void* user, uint16_t order_pos, const uint16_t* indices, uint8_t count) {
    auto* b = static_cast<BoardLoad*>(user);
    uint32_t words[player::load::kProgressiveMaxSamples / 32] = {};
    for (uint8_t i = 0; i < count; ++i) {
        if (indices[i] < player::load::kProgressiveMaxSamples) words[indices[i] / 32u] |= 1u << (indices[i] % 32u);
    }
    for (uint32_t w = 0; w < player::load::kProgressiveMaxSamples / 32; ++w) {
        b->in_use_bits[w].store(words[w], std::memory_order_release);
    }
    b->order_pos = order_pos;
}

// Перед каждым куском рендера: сколько байт источник успел отдать за этот
// кусок, столько и догрузить. Бюджет не копится дольше секунды: пауза в
// плане не даёт права потом прочитать мегабайт мгновенно.
void board_per_block(void* user, uint32_t frames) {
    auto* b = static_cast<BoardLoad*>(user);
    b->frames += frames;
    b->budget_bytes += b->bytes_per_frame * frames;
    const double cap = b->bytes_per_frame * kSampleRate;
    if (b->budget_bytes > cap) b->budget_bytes = cap;
    const player::load::SamplesInUse in_use{b->in_use_bits, player::load::kProgressiveMaxSamples};
    b->pl.no_eviction = player::load::progressive_eviction_blocked(b->position_goes_back, b->total_frames, b->frames);
    while (b->budget_bytes > 0.0) {
        uint16_t idx = 0;
        const auto st = player::load::progressive_load_next(b->pl, *b->mem, b->order_pos, in_use,
                                                              &board_load_sample, b, &idx);
        if (st == player::load::ProgressiveStep::Waiting) break;
        if (st == player::load::ProgressiveStep::Done) {
            if (!b->done_at_frame) b->done_at_frame = b->frames;
            break;
        }
        b->budget_bytes -= board_sample_cost(*b, idx);
        if (st == player::load::ProgressiveStep::Loaded) {
            ++b->loaded_bg;
            const uint16_t first = idx < player::load::kProgressiveMaxSamples ? b->pl.plan_first_use[idx]
                                                                                 : player::load::kSampleNeverUsed;
            if (first != player::load::kSampleNeverUsed && b->order_pos > first) {
                ++b->late;
                b->late_positions += b->order_pos - first;
            }
        }
    }
}

// Фазы A-C оркестратора платы: память трека как на плате, метаданные без
// PCM (run_session_load тем же путём), план, префетч первых позиций. Фаза
// D - под звук, board_per_block.
bool board_prepare(const AppArgs* args, std::vector<uint8_t>& file_bytes, soundsinth::memory::TrackMemory& mem,
                   soundsinth::model::Song& song, const char** error_out, BoardLoad& b) {
    const SongFormat fmt = song_format_from_extension(args->play_file_path);
    std::ifstream in(args->play_file_path, std::ios::binary);
    if (!in) { *error_out = "не удалось открыть файл"; return false; }
    file_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    if (fmt == SongFormat::Midi) {
        if (!g_bank.valid()) { *error_out = "для .mid нужен банк: --bank файл.ssb"; return false; }
        player::load::session_loader_set_bank(&g_bank);
        // Больше 15 МБ во флеш платы не встаёт - такой банк лежит на карте, и
        // его таблицы отнимают у трека хвост PSRAM (main.cpp платы).
        if (g_bank_bytes.size() > 15u * 1024u * 1024u) {
            soundsinth::memory::psram_set_track_bytes(mem.psram, soundsinth::memory::kBankTableOffset);
        }
    }
    static soundsinth::formats::MemoryByteSource* src = nullptr;
    delete src;
    src = new soundsinth::formats::MemoryByteSource(file_bytes.data(), static_cast<uint32_t>(file_bytes.size()));
    b.src = src;
    b.mem = &mem;
    b.song = &song;
    player::load::SessionLoadResult load;
    if (!player::load::run_session_load(src->as_byte_source(), mem, song, load, /*metadata_only=*/true)) {
        if (error_out) *error_out = load.error;
        return false;
    }
    b.format = load.format;
    b.position_goes_back = load.duration.position_goes_back;
    b.total_frames = load.total_frames;
    b.pl.no_eviction = true; // префетч идёт до звука: отыгравших ещё нет
    player::load::progressive_attach_plan(b.pl, b.plan_mem);

    uint16_t prefetch_positions = player::load::kPrefetchOrderPositions;
    if (b.format == player::load::TrackFormat::Midi) {
        const uint16_t by_time = soundsinth::formats::midi::order_positions_in_frames(
            SOUNDSINTH_MIDI_PREFETCH_SECONDS * kSampleRate);
        if (by_time > prefetch_positions) prefetch_positions = by_time;
        b.pl.lead_positions = by_time;
    }
    const player::load::SamplesInUse none{b.in_use_bits, player::load::kProgressiveMaxSamples};
    if (song.sample_count <= player::load::kProgressiveMaxSamples) {
        const player::load::PlaybackPlan plan = player::load::plan_playback_order(
            song, mem.psram, b.pl.plan_indices, player::load::kProgressiveMaxSamples, b.pl.plan_last_use,
            player::load::LoadOrder::ByFile, prefetch_positions, b.pl.plan_first_use);
        b.pl.plan_count = plan.count;
        player::load::progressive_plan_changed(b.pl);
        b.prefetch_count = plan.prefetch_count;
        while (b.pl.plan_next < b.prefetch_count) {
            const auto st = player::load::progressive_load_next(b.pl, mem, 0, none, &board_load_sample, &b);
            if (st == player::load::ProgressiveStep::Failed) ++b.prefetch_failed;
            if (st == player::load::ProgressiveStep::Waiting || st == player::load::ProgressiveStep::Done) break;
        }
    } else {
        // Как у платы: сэмплов больше, чем помещается в план, - всё до старта.
        for (uint16_t i = 0; i < song.sample_count; ++i) board_load_sample(&b, i, nullptr);
    }
    const uint32_t kbps = args->load_kbps ? args->load_kbps : (b.format == player::load::TrackFormat::Midi ? 1000u : 200u);
    b.bytes_per_frame = double(kbps) * 1024.0 / kSampleRate;
    std::printf("board-load: память трека %u КБ, план %u сэмплов, префетч %u (%u позиций, не влезло %u), "
                "упреждение %u поз., источник %u КБ/с\n",
                (unsigned)(uint32_t(mem.psram.sample_page_count) * soundsinth::memory::kPsramPageBytes / 1024u),
                (unsigned)b.pl.plan_count, (unsigned)b.prefetch_count, (unsigned)prefetch_positions,
                (unsigned)b.prefetch_failed, (unsigned)b.pl.lead_positions, (unsigned)kbps);
    return true;
}

// Вывод на звуковую карту: платформонезависимая
// player::audio::RenderTask заполняет пул буферов, sdl_sink его
// вычерпывает задачей в роли прерывания. Платформенное здесь только
// устройство вывода (sdl_sink) и CLI-режим.
void play_live(soundsinth::mixbus::MixBus& bus, player::audio::BufferPool& pool, double seconds) {
    if (!platform_pc::sdl_sink_start(pool)) {
        std::fprintf(stderr, "не удалось открыть звуковое устройство SDL2\n");
        return;
    }

    player::audio::RenderTask render_task(bus, pool);

    std::printf("играет 440 Гц через SDL2 (%.1f c)...\n", seconds);
    platform::os_task_delay_ms(static_cast<uint32_t>(seconds * 1000.0));

    // sink должен оставаться активным (продолжать вычерпывать пул), пока
    // render_task не остановится - иначе она может зависнуть в
    // begin_write() в ожидании свободного слота, который некому освобождать.
    render_task.stop();
    platform_pc::sdl_sink_stop();
}

// Единственная прикладная FreeRTOS-задача. Аргументы разбирает main;
// задача готовит кольцо/mixbus/тестовый источник и запускает нужный
// режим.
void app_main_task(void* pv_parameters) {
    auto* args = static_cast<AppArgs*>(pv_parameters);

    // Windows-порт FreeRTOS при старте планировщика ставит процессу класс
    // REALTIME: несколько рендеров подряд забирали машину целиком. Обычный
    // класс; порядок потоков порта внутри процесса (прерывания, таймер,
    // задачи) держится их собственными приоритетами.
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    if (args->list_image_path) {
        list_image(args->list_image_path);
        std::exit(0);
    }

    player::audio::BufferPool pool;

    soundsinth::mixbus::MixBus bus;

    // Если файл трека не задан, играет диагностический синус (им с самого
    // начала проверялся пайплайн audio_sink/mixbus/sdl_sink/wav_writer, не
    // убираем).
    pc_player::SineSource sine(440.0f, kSampleRate, 8000.0f); // ~-12 дБFS, безопасная тестовая громкость
    std::vector<uint8_t> file_bytes;
    auto* mem = new soundsinth::memory::TrackMemory();
    soundsinth::memory::track_memory_create(*mem);
    if (args->psram_kb != 0) {
        soundsinth::memory::psram_set_track_bytes(mem->psram, args->psram_kb * 1024u);
        std::printf("хранилище трека: %u КБ (как на плате)\n", (unsigned)args->psram_kb);
    }
    soundsinth::model::Song song;
    soundsinth::engine::TrackerEngine* engine = nullptr;
    // Длительность рендера без --seconds - до сборки движка: у .mid
    // ревербератор занимает буфер сценариев, проход по треку затёр бы его.
    uint32_t song_frames = 0;
    soundsinth::engine::DurationStats dur;
    // --board-load: большая структура (план на 1024 сэмпла) - не на стеке задачи.
    static BoardLoad board;

    if (args->then_path && args->play_file_path) {
        const char* error = nullptr;
        if (!load_song(args->play_file_path, file_bytes, *mem, song, &error)) {
            std::fprintf(stderr, "--then: не загрузить %s: %s\n", args->play_file_path, error ? error : "?");
            std::exit(1);
        }
        auto* first = new soundsinth::engine::TrackerEngine(song, *mem);
        bus.start_track();
        bus.add_source(first->as_sound_source());
        bus.set_limiter(song.limiter_enabled && !args->no_limiter);
        bus.set_soft_clip(song.soft_clip_enabled && !args->no_limiter);
        static int16_t discard[SOUNDSINTH_AUDIO_BUFFER_FRAMES * 2];
        constexpr uint32_t kFirstTrackFrames = 5u * kSampleRate;
        for (uint32_t done = 0; done < kFirstTrackFrames; done += SOUNDSINTH_AUDIO_BUFFER_FRAMES) {
            bus.render(discard, SOUNDSINTH_AUDIO_BUFFER_FRAMES);
        }
        // Снос как у teardown_engine платы: затухание за буфер, снятие
        // источника, деструктор движка; память и Song - как у run_session_load.
        bus.fade_out();
        bus.render(discard, SOUNDSINTH_AUDIO_BUFFER_FRAMES);
        bus.remove_source(first->as_sound_source());
        delete first;
        soundsinth::memory::track_memory_reset_for_new_track(*mem);
        new (&song) soundsinth::model::Song();
        std::printf("--then: %s сыгран 5 с, дальше %s в той же памяти\n", args->play_file_path, args->then_path);
        args->play_file_path = args->then_path;
    }

    if (args->play_file_path) {
        const char* error = nullptr;
        // Метаданные без PCM, потом сэмплы по плану - тот же порядок, что на
        // плате. Иначе дыра в планировщике на ПК не видна.
        bool loaded = args->board_load ? board_prepare(args, file_bytes, *mem, song, &error, board)
                                       : load_song(args->play_file_path, file_bytes, *mem, song, &error, true);
        if (loaded && !args->board_load && !args->progressive) {
            const SongFormat fmt = song_format_from_extension(args->play_file_path);
            soundsinth::formats::MemoryByteSource ssrc(file_bytes.data(), static_cast<uint32_t>(file_bytes.size()));
            uint16_t ok_count = 0, fail_count = 0;
            loaded = load_planned_samples(fmt, ssrc.as_byte_source(), *mem, song, ok_count, fail_count);
            if (!loaded) error = "формат не поддержан загрузкой по плану";
            if (fail_count != 0) {
                std::printf("план: загружено %u сэмплов, не загрузилось %u\n", (unsigned)ok_count,
                            (unsigned)fail_count);
            }
        }
        if (loaded) {
            const bool is_midi = song_format_from_extension(args->play_file_path) == SongFormat::Midi;
            if (args->progressive && !args->board_load && !is_midi) {
                // Трекерный загрузчик уже залил все сэмплы; префетч по плану -
                // путь банка .mid. Загрузку трекера как на плате даёт --board-load.
                std::printf("--prefetch-positions/--partial/--sample-report - только для .mid, трекеры: --board-load\n");
            }
            if (args->progressive && !args->board_load && is_midi) {
                // То же, что делает плата: план по паттернам, затем префетч первых N
                // позиций, дальше играем. Фоновой догрузки здесь нет нарочно: нужен
                // снимок того, чем располагает движок в первые секунды.
                static uint16_t plan_indices[4096];
                static uint16_t plan_last_use[4096];
                const player::load::PlaybackPlan plan = player::load::plan_playback_order(
                    song, mem->psram, plan_indices, 4096, plan_last_use, player::load::LoadOrder::ByFile,
                    args->prefetch_positions);
                const uint16_t plan_count = plan.count;
                const uint16_t prefetch_count = plan.prefetch_count;
                uint16_t loaded = 0, failed = 0;
                std::FILE* rep = args->sample_report ? std::fopen(args->sample_report, "w") : nullptr;
                if (rep) std::fprintf(rep, "поз\tсэмпл\tбанк\tбайт\tпрогон\tитог\n");
                for (uint16_t i = 0; i < prefetch_count; ++i) {
                    const char* why = nullptr;
                    const uint16_t si = plan_indices[i];
                    const uint16_t bs = static_cast<uint16_t>(song.samples[si].file_offset);
                    const bool ok = soundsinth::formats::midi::load_sample_from_bank(*mem, g_bank, song, si, &why);
                    if (ok) ++loaded; else ++failed;
                    if (rep && bs < g_bank.header->sample_count) {
                        std::fprintf(rep, "%u\t%u\t%u\t%u\t%u\t%s\n", (unsigned)i, (unsigned)si, (unsigned)bs,
                                     (unsigned)g_bank.samples[bs].pcm_bytes,
                                     (unsigned)g_bank.samples[bs].pcm_offset, ok ? "загружен" : "НЕ ВЛЕЗ");
                    }
                }
                if (rep) std::fclose(rep);
                std::printf("префетч: %u из %u сэмплов плана (%u позиций), не загрузилось %u; всего в треке %u\n",
                            (unsigned)prefetch_count, (unsigned)plan_count, (unsigned)args->prefetch_positions,
                            (unsigned)failed, (unsigned)song.sample_count);
            }
            if (args->load_plan) {
                // --- Очередь догрузки против порядка игры ---
                //
                // Плата грузит план строго по очереди и держит упреждение: если у
                // головы очереди первое использование дальше, чем pos + lead, загрузка
                // встаёт целиком (session_orchestrator.cpp, load_next_planned_sample).
                // А план отсортирован по смещению в файле, со временем первой ноты это
                // не связано. Поэтому один сэмпл из далёкого будущего, лежащий в файле
                // рано, задерживает всех, кто нужен сейчас.
                //
                // Расчёт: очередь доходит до k-й записи не раньше позиции
                // max(доход[k-1], first[k] - lead). Время самого чтения не моделируем -
                // оно может только ухудшить результат.
                static uint16_t lp_idx[4096];
                static uint16_t lp_last[4096];
                static uint16_t lp_first[4096];
                uint16_t lp_prefetch = 0;
                uint16_t prefetch_positions = player::load::kPrefetchOrderPositions;
                uint16_t lead = 0;
                const bool is_mid = std::strstr(args->play_file_path, ".mid") != nullptr ||
                                    std::strstr(args->play_file_path, ".MID") != nullptr;
                if (is_mid) {
                    const uint16_t by_time = soundsinth::formats::midi::order_positions_in_frames(
                        SOUNDSINTH_MIDI_PREFETCH_SECONDS * soundsinth::engine::kSampleRateHz);
                    if (by_time > prefetch_positions) prefetch_positions = by_time;
                    lead = by_time;
                }
                for (uint8_t pass = 0; pass < 2; ++pass) {
                    const player::load::LoadOrder order =
                        pass == 0 ? player::load::LoadOrder::ByFile : player::load::LoadOrder::ByPlayback;
                    const player::load::PlaybackPlan plan = player::load::plan_playback_order(
                        song, mem->psram, lp_idx, 4096, lp_last, order, prefetch_positions, lp_first);
                    const uint16_t n = plan.count;
                    lp_prefetch = plan.prefetch_count;
                    uint16_t reach = 0;
                    unsigned late = 0, worst = 0, worst_idx = 0, worst_need = 0, worst_get = 0;
                    unsigned long delay_sum = 0;
                    for (uint16_t k = lp_prefetch; k < n; ++k) {
                        const uint16_t si = lp_idx[k];
                        const uint16_t first = lp_first[si];
                        if (first == player::load::kSampleNeverUsed) continue;
                        const uint16_t ready = first > lead ? static_cast<uint16_t>(first - lead) : 0u;
                        if (ready > reach) reach = ready;
                        if (reach > first) {
                            ++late;
                            const unsigned d = static_cast<unsigned>(reach - first);
                            delay_sum += d;
                            if (d > worst) { worst = d; worst_idx = si; worst_need = first; worst_get = reach; }
                        }
                    }
                    std::printf("план (%s): всего %u, префетч %u, упреждение %u -> опоздало %u",
                                pass == 0 ? "по файлу, как на плате" : "по порядку игры", (unsigned)n,
                                (unsigned)lp_prefetch, (unsigned)lead, late);
                    if (late) {
                        std::printf(", в среднем на %.1f позиций, худший сэмпл %u (нужен на %u, приедет на %u)",
                                    (double)delay_sum / (double)late, worst_idx, worst_need, worst_get);
                    }
                    std::printf("\n");
                }
            }
            if (args->linear_interp) song.quirks &= ~soundsinth::model::kQuirkHermiteInterpolation;
            if (args->wav_path && !args->seconds_explicit) {
                song_frames = soundsinth::engine::compute_song_total_frames(
                    song, mem->psram, soundsinth::memory::scratch_take(mem->scratch, soundsinth::memory::Scratch::DurationPass, soundsinth::memory::kDurationPassBytes), soundsinth::memory::kDurationPassBytes, &dur);
                soundsinth::memory::scratch_leave(mem->scratch, soundsinth::memory::Scratch::DurationPass);
            }
            engine = new soundsinth::engine::TrackerEngine(song, *mem);
            if (args->voice_cull) { // сброс голосов по модели загрузки, как на плате
                engine->set_voice_cull_enabled(true);
                engine->set_model_load_enabled(true);
            }
            if (args->board_load) engine->set_tick_observer(&board_tick_observer, &board);
            bus.start_track();
            bus.add_source(engine->as_sound_source());
            bus.set_limiter(song.limiter_enabled && !args->no_limiter);
            bus.set_soft_clip(song.soft_clip_enabled && !args->no_limiter);
            apply_soft_curve(bus, *args, true);
            if (args->comp_threshold > 0) {
                bus.set_limiter(true);
                bus.set_compressor(args->comp_threshold, args->comp_amount);
            }
            std::printf("играю %s (каналов=%u, паттернов=%u, тем=%u, скорость=%u)\n", args->play_file_path,
                        song.channel_count, song.pattern_count, song.default_tempo, song.default_speed);
            // Замер резидентной арены: выделено 48 КБ (kResidentMetadataBytes), на
            // плате они заняты всегда; печатается, сколько трек кладёт в неё на
            // самом деле. Прогон по всей музыкальной библиотеке даёт максимум, по
            // которому назначается размер.
            std::printf("arena: used %u of %u (сэмплов=%u, инструментов=%u)\n",
                        (unsigned)soundsinth::memory::arena_used(mem->resident),
                        (unsigned)soundsinth::memory::kResidentMetadataBytes,
                        (unsigned)song.sample_count, (unsigned)song.instrument_count);
            if (!is_midi) {
                // Потери загрузки трекерного файла: разбор и полная загрузка.
                const auto& ls = soundsinth::model::g_tracker_load_stats;
                uint32_t resident = 0;
                for (uint16_t i = 0; i < song.sample_count; ++i) {
                    if (soundsinth::memory::sample_cache_find(mem->sample_cache, i) != nullptr) ++resident;
                }
                std::printf("load: сэмплов %u, резидентно %u, не распаковываются %u, выброшено %u, отказов %u",
                            (unsigned)song.sample_count, (unsigned)resident, (unsigned)ls.samples_nonresident,
                            (unsigned)ls.samples_dropped, (unsigned)ls.samples_failed);
                if (ls.samples_failed != 0) {
                    std::printf(" (первый %u: %s)", (unsigned)ls.first_failed_sample,
                                ls.first_failure ? ls.first_failure : "?");
                }
                std::printf(", петель удержания %u, автовибрато %u, ячеек без разбора %lu\n",
                            (unsigned)ls.ignored_sustain_loops, (unsigned)ls.ignored_autovibrato,
                            (unsigned long)ls.effect_cells_dropped);
            }
            if (args->unused_samples) {
                // Проходим все паттерны и разрешаем каждую ячейку (инструмент, нота)
                // в сэмпл так же, как движок на триггере. Всё, что осталось
                // непомеченным, - память, занятая зря.
                static bool used[4096];
                std::memset(used, 0, sizeof(used));
                soundsinth::model::PatternCell cells[SOUNDSINTH_MAX_VOICES];
                for (uint16_t p = 0; p < song.pattern_count; ++p) {
                    const auto& pat = song.patterns[p];
                    if (pat.psram_offset == soundsinth::model::Pattern::kInvalidOffset ||
                        pat.channel_count == 0) continue;
                    soundsinth::patterns::PatternReader reader(
                        soundsinth::memory::psram_pattern_ptr(mem->psram, pat.psram_offset), pat.row_count,
                        pat.channel_count);
                    for (uint16_t r = 0; r < pat.row_count; ++r) {
                        reader.read_row(r, cells);
                        for (uint8_t c = 0; c < pat.channel_count; ++c) {
                            if (cells[c].instrument == 0 || cells[c].note > 119) continue;
                            const uint8_t ii = static_cast<uint8_t>(cells[c].instrument - 1);
                            if (ii >= song.instrument_count) continue;
                            const auto& ins = song.instruments[ii];
                            uint16_t si = 0xFFFF;
                            for (uint8_t q = 0; q < ins.note_to_sample_range_count; ++q) {
                                if (soundsinth::model::keymap_range_start(ins.note_to_sample_ranges[q]) > cells[c].note) break;
                                si = ins.note_to_sample_ranges[q].sample_index;
                            }
                            if (si < song.sample_count && si < 4096) used[si] = true;
                        }
                    }
                }
                // Сколько записей сэмпла делят один прогон PCM. Байты от этого не
                // удваиваются (цепочка страниц общая), но каждый дубль занимает слот
                // каталога и запись SampleDescriptor.
                uint32_t uniq = 0, dups = 0;
                for (uint16_t si = 0; si < song.sample_count; ++si) {
                    const uint16_t bs = static_cast<uint16_t>(song.samples[si].file_offset);
                    if (bs >= g_bank.header->sample_count) continue;
                    bool seen = false;
                    for (uint16_t sj = 0; sj < si; ++sj) {
                        const uint16_t bj = static_cast<uint16_t>(song.samples[sj].file_offset);
                        if (bj < g_bank.header->sample_count &&
                            g_bank.samples[bj].pcm_offset == g_bank.samples[bs].pcm_offset) { seen = true; break; }
                    }
                    if (seen) ++dups; else ++uniq;
                }
                std::printf("дубли прогона: записей %u, разных прогонов %u, дублей %u (%.1f%%), каталог %u\n",
                            (unsigned)song.sample_count, (unsigned)uniq, (unsigned)dups,
                            song.sample_count ? 100.0 * double(dups) / double(song.sample_count) : 0.0,
                            (unsigned)soundsinth::memory::kSampleCacheCatalogCapacity);
                uint32_t n_unused = 0, bytes_unused = 0, bytes_all = 0;
                for (uint16_t si = 0; si < song.sample_count; ++si) {
                    const uint16_t bs = static_cast<uint16_t>(song.samples[si].file_offset);
                    const uint32_t b = bs < g_bank.header->sample_count ? g_bank.samples[bs].pcm_bytes : 0;
                    bytes_all += b;
                    if (si >= 4096 || !used[si]) { ++n_unused; bytes_unused += b; }
                }
                std::printf("лишних сэмплов: %u из %u, %u КБ из %u (%.1f%%)\n", (unsigned)n_unused,
                            (unsigned)song.sample_count, (unsigned)(bytes_unused / 1024),
                            (unsigned)(bytes_all / 1024),
                            bytes_all ? 100.0 * double(bytes_unused) / double(bytes_all) : 0.0);
            }
            std::printf("packer: словарь %u Б (%u ячеек), строки %u Б, буфер %u, сравнений %u | scratch %u из %u\n",
                        (unsigned)soundsinth::patterns::g_max_dict_bytes,
                        (unsigned)soundsinth::patterns::g_max_dict_cells,
                        (unsigned)soundsinth::patterns::g_max_rows_bytes,
                        (unsigned)soundsinth::memory::kPatternPackBufferBytes,
                        (unsigned)soundsinth::patterns::g_dict_compares,
                        (unsigned)soundsinth::memory::g_max_scratch_used,
                        (unsigned)soundsinth::memory::kLoaderScratchBytes);
        } else {
            std::fprintf(stderr, "не удалось загрузить %s: %s — играю тестовый синус вместо этого\n",
                         args->play_file_path, error ? error : "неизвестная ошибка");
            bus.add_source(sine.as_sound_source());
        }
    } else {
        bus.add_source(sine.as_sound_source());
    }

    if (args->wav_path) {
        // Без явного --seconds и с реальным файлом рендерим весь трек
        // (см. compute_song_total_frames), а не фиксированные 3/60 секунд.
        const uint32_t total_frames =
            (!args->seconds_explicit && engine) ? song_frames : static_cast<uint32_t>(args->seconds * kSampleRate);
        if (!args->seconds_explicit && engine) {
            std::printf("duration: %.3f с, строк %u, тиков %u, стоп=%s (%u,%u)%s\n",
                        static_cast<double>(total_frames) / kSampleRate, (unsigned)dur.rows, (unsigned)dur.ticks,
                        soundsinth::engine::duration_stop_name(dur.stop), (unsigned)dur.stop_order_pos,
                        (unsigned)dur.stop_row, dur.position_goes_back ? ", переход назад" : "");
        }
        const auto t0 = std::chrono::steady_clock::now();
        if (args->through_pool && !args->board_load) {
            render_through_pool(bus, pool, args->wav_path, total_frames);
        } else {
            if (args->through_pool) std::printf("--through-pool с --board-load не сочетается, рендер прямой\n");
            render_to_wav(bus, args->wav_path, total_frames, (args->board_load && engine) ? &board_per_block : nullptr, &board);
        }
        if (args->board_load && engine) {
            // Сводка: чего трек не дождался. Сэмпл, загруженный после позиции своей
            // первой ноты, до неё молчал; не влезший молчит весь трек.
            std::printf("board-load: под звук догружено %u, опоздало %u (в среднем на %.1f позиций), "
                        "не влезло %u (из них в префетче %u), вытеснено %u, в плане осталось %u; "
                        "план догружен к %.1f с, нот без сэмпла %u\n",
                        (unsigned)board.loaded_bg, (unsigned)board.late,
                        board.late ? double(board.late_positions) / board.late : 0.0,
                        (unsigned)board.pl.plan_failed, (unsigned)board.prefetch_failed, (unsigned)board.pl.evicted,
                        (unsigned)(board.pl.plan_count - board.pl.plan_next),
                        board.done_at_frame ? double(board.done_at_frame) / kSampleRate : -1.0,
                        (unsigned)engine->triggers_without_sample());
        }
        const double render_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        // --- Нагрузка ---
        // Считается двумя мерами, потому что каждая по отдельности неточна.
        //
        // Первая - доля реального времени: сколько ушло на рендер против того,
        // сколько это играет. Она включает всё (распаковку, фильтр,
        // реверберацию, сведение), но снята на ПК: абсолютное значение к плате
        // отношения не имеет, сравнивать можно только файлы между собой.
        //
        // Вторая - пик одновременных голосов. Она переносится на плату
        // напрямую, потому что бюджет движка считается по голосам
        // (update_voice_budget, модель цены в config.h).
        if (engine && total_frames > 0) {
            const double audio_s = static_cast<double>(total_frames) / kSampleRate;
            std::printf("нагрузка: рендер %.2f с на %.2f с звука = %.4f от реального времени\n"
                        "          пик слотов в сведении %u (потолок звучащих %u), бюджет к концу %u, unlisted=%u demand_peak=%u "
                        "tails_dropped=%u очередь=%u/%u полна=%u\n",
                        render_s, audio_s, render_s / audio_s,
                        (unsigned)engine->peak_active_voice_count(), (unsigned)SOUNDSINTH_MAX_VOICES,
                        (unsigned)engine->voice_budget(), (unsigned)engine->voices_unlisted(),
                        (unsigned)engine->voice_demand_peak(), (unsigned)engine->tails_dropped(),
                        (unsigned)engine->voice_queue_peak(), (unsigned)soundsinth::engine::kVoiceQueueCapacity,
                        (unsigned)engine->voice_queue_full());
            if (engine->wave_tail_started() != 0) {
                const unsigned started = (unsigned)engine->wave_tail_started();
                const unsigned cut = (unsigned)engine->wave_tail_cut_note() + (unsigned)engine->wave_tail_cut_move();
                std::printf("волновое гашение: начато %u, оборвано %u (%.2f%%) - нотой %u, переездом NNA %u\n", started,
                            cut, 100.0 * cut / started, (unsigned)engine->wave_tail_cut_note(),
                            (unsigned)engine->wave_tail_cut_move());
            }
            std::printf("bus: на_шкале=%u полка=%u разрывов=%u макс=%u мягко=%u\n", (unsigned)bus.clipped_frames(),
                        (unsigned)bus.longest_clip_run(), (unsigned)bus.jumps(), (unsigned)bus.max_jump(),
                        (unsigned)bus.soft_clipped_frames());
        }

        // Разовая статистика "продекодировано, но выброшено до интерполяции"
        // (см. voice.h) для всего трека: total_frames уже отрендерен выше,
        // счётчики глобальные с момента старта процесса - для одиночного
        // --render-wav это и нужно.
        if (engine) {
            const soundsinth::engine::VoiceDebugCounters vdc = soundsinth::engine::voice_debug_counters();
            const uint32_t decode_calls = vdc.decode_calls;
            // step_clamps обязан быть 0: иначе шаг упёрся в потолок.
            std::printf("decode: всего=%u, из них выброшено ДО интерполяции: dpcm8=%u (%.1f%%), прямые=%u (%.1f%%),"
                        " прыжков=%u, потолок шага=%u\n",
                        decode_calls, vdc.discarded_dpcm8, decode_calls ? 100.0 * vdc.discarded_dpcm8 / decode_calls : 0.0,
                        vdc.discarded_direct, decode_calls ? 100.0 * vdc.discarded_direct / decode_calls : 0.0,
                        vdc.direct_jumps, vdc.step_clamps);
            std::printf("питч: пересчётов шага %u\n", vdc.pitch_recalcs);

        // Сколько работал лимитер. Без этой строки его вмешательство не видно:
        // он тихо снимает децибелы, и на плотном материале зажатие в 8 дБ
        // выглядит как "банк тише".
        {
            const uint32_t lf = bus.limited_frames();
            const uint32_t mg = bus.min_gain_q15();
            if (lf) {
                std::printf("лимитер: кадров под ужатием %u (%.2f%%), сильнее всего на %.1f дБ\n",
                            lf, 100.0 * double(lf) / double(total_frames ? total_frames : 1u),
                            20.0 * std::log10(double(mg) / 32768.0));
            }
        }
        }

        // Поканальный экспорт (--by-channel-dir) - сверка с OpenMPT и другими
        // плеерами по каналам отдельно, см. set_solo_channel в tracker_engine.h.
        // TrackerEngine пересоздаётся на каждый канал: состояние голосов и
        // тиков от предыдущего прогона переиспользовать нельзя - трек
        // переигрывается целиком.
        if (args->by_channel_dir && engine) {
            static int32_t discard[SOUNDSINTH_AUDIO_BUFFER_FRAMES * 2];
            std::error_code mkdir_ec;
            std::filesystem::create_directories(args->by_channel_dir, mkdir_ec);

            std::string base = args->wav_path;
            const size_t slash = base.find_last_of("/\\");
            if (slash != std::string::npos) base = base.substr(slash + 1);
            const size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base = base.substr(0, dot);

            for (uint8_t ch = 0; ch < song.channel_count && ch < SOUNDSINTH_MAX_VOICES; ++ch) {
                bus.remove_source(engine->as_sound_source());
                delete engine;
                engine = new soundsinth::engine::TrackerEngine(song, *mem);
                engine->set_solo_channel(ch, discard, SOUNDSINTH_AUDIO_BUFFER_FRAMES);
                if (args->voice_cull) { // сброс голосов по модели загрузки, как на плате
                    engine->set_voice_cull_enabled(true);
                    engine->set_model_load_enabled(true);
                }
                bus.start_track();
                bus.add_source(engine->as_sound_source());
                bus.set_limiter(song.limiter_enabled && !args->no_limiter);
                bus.set_soft_clip(song.soft_clip_enabled && !args->no_limiter);
                apply_soft_curve(bus, *args, false);
                if (args->comp_threshold > 0) {
                    bus.set_limiter(true);
                    bus.set_compressor(args->comp_threshold, args->comp_amount);
                }

                char ch_path[1024];
                std::snprintf(ch_path, sizeof(ch_path), "%s/%s_ch%02u.wav", args->by_channel_dir, base.c_str(),
                              static_cast<unsigned>(ch + 1));
                render_to_wav(bus, ch_path, total_frames);
            }
        }
    } else {
        play_live(bus, pool, args->seconds);
    }

    delete engine;
    soundsinth::memory::track_memory_destroy(*mem);
    delete mem;

    // FreeRTOS-задача не должна просто вернуться: Windows-симулятор не
    // рассчитан на штатный возврат из vTaskStartScheduler(), поэтому (как в
    // демо-программах этого порта) завершаем процесс явно.
    std::exit(0);
}

} // namespace

int main(int argc, char** argv) {
    static AppArgs args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render-wav") == 0 && i + 1 < argc) {
            args.wav_path = argv[++i];
        } else if (std::strcmp(argv[i], "--bank") == 0 && i + 1 < argc) {
            std::ifstream bf(argv[i + 1], std::ios::binary);
            if (!bf) { std::fprintf(stderr, "банк не открывается: %s\n", argv[i + 1]); return 1; }
            g_bank_bytes.assign(std::istreambuf_iterator<char>(bf), std::istreambuf_iterator<char>());
            const char* berr = nullptr;
            if (!soundsinth::bank::bank_open(g_bank_bytes.data(), static_cast<uint32_t>(g_bank_bytes.size()),
                                             g_bank, &berr)) {
                std::fprintf(stderr, "банк не читается: %s\n", berr ? berr : "?");
                return 1;
            }
            std::printf("банк: %s, инструментов %u, сэмплов %u\n", g_bank.header->name,
                        g_bank.header->instrument_count, g_bank.header->sample_count);
            // Загрузчику сэмплов по плану: без банка .mid ему взять PCM
            // неоткуда. На плате это делает open_bank при старте.
            player::load::session_loader_set_bank(&g_bank);
            ++i;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            args.seconds = std::atof(argv[++i]);
            args.seconds_explicit = true;
        } else if (std::strcmp(argv[i], "--list-image") == 0 && i + 1 < argc) {
            args.list_image_path = argv[++i];
        } else if (std::strcmp(argv[i], "--play-file") == 0 && i + 1 < argc) {
            args.play_file_path = argv[++i];
        } else if (std::strcmp(argv[i], "--by-channel-dir") == 0 && i + 1 < argc) {
            args.by_channel_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--psram-kb") == 0 && i + 1 < argc) {
            args.psram_kb = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--grid") == 0 && i + 1 < argc) {
            // "16:3" - строк на долю и тиков на строку, вместо выбора по темпу.
            const char* g = argv[++i];
            char* end = nullptr;
            const long rpb = std::strtol(g, &end, 10);
            const long tpr = (end && *end == ':') ? std::strtol(end + 1, nullptr, 10) : 0;
            if (rpb > 0 && tpr > 0)
                soundsinth::formats::midi::set_forced_grid(static_cast<uint8_t>(rpb),
                                                           static_cast<uint8_t>(tpr));
        } else if (std::strcmp(argv[i], "--keep-lead") == 0) {
            // Не срезать ведущую паузу .mid. Нужно поканальным замерам: иначе
            // каждый вырезанный канал начинается своей первой нотой и рендеры
            // расходятся между собой на десятки секунд.
            soundsinth::formats::midi::set_trim_lead_silence(false);
        } else if (std::strcmp(argv[i], "--keep-tail") == 0) {
            // Не срезать тишину после последней ноты .mid: чужой экспорт её
            // сохраняет, и для сверки длительностей она нужна.
            soundsinth::formats::midi::set_trim_tail_silence(false);
        } else if (std::strcmp(argv[i], "--comp") == 0 && i + 1 < argc) {
            // "порог%:степень" - например 90:4 это порог на 90% шкалы и
            // сжатие 4:1. Порог в процентах, а не в децибелах: у полной
            // шкалы проценты нагляднее. Компрессор - ступень лимитера, ключ
            // включает её; без ключа её не включает ни один формат.
            const char* c = argv[++i];
            char* end = nullptr;
            const double pct = std::strtod(c, &end);
            const double ratio = (end && *end == ':') ? std::strtod(end + 1, nullptr) : 2.0;
            args.comp_threshold = static_cast<int32_t>(32768.0 * pct / 100.0);
            args.comp_amount = static_cast<int32_t>(32768.0 * (1.0 - 1.0 / (ratio > 1.0 ? ratio : 1.0)));
        } else if (std::strcmp(argv[i], "--no-limiter") == 0) {
            args.no_limiter = true;
        } else if (std::strcmp(argv[i], "--linear-interp") == 0) {
            args.linear_interp = true;
        } else if (std::strcmp(argv[i], "--voice-cull") == 0) {
            args.voice_cull = true;
        } else if (std::strcmp(argv[i], "--prefetch-positions") == 0 && i + 1 < argc) {
            args.prefetch_positions = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
            args.progressive = true;
        } else if (std::strcmp(argv[i], "--unused-samples") == 0) {
            args.unused_samples = true;
        } else if (std::strcmp(argv[i], "--load-plan") == 0) {
            args.load_plan = true;
        } else if (std::strcmp(argv[i], "--sample-report") == 0 && i + 1 < argc) {
            args.sample_report = argv[++i];
            args.prefetch_positions = 0xFFFF;
            args.progressive = true;
        } else if (std::strcmp(argv[i], "--soft-knee") == 0 && i + 1 < argc) {
            args.soft_knee = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--then") == 0 && i + 1 < argc) {
            args.then_path = argv[++i];
        } else if (std::strcmp(argv[i], "--board-load") == 0) {
            args.board_load = true;
        } else if (std::strcmp(argv[i], "--through-pool") == 0) {
            args.through_pool = true;
        } else if (std::strcmp(argv[i], "--load-kbps") == 0 && i + 1 < argc) {
            args.load_kbps = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--partial") == 0) {
            // Грузить весь план, но не отменять трек из-за сэмплов, которые не
            // влезли, - как ведёт себя плата. Строгий путь (умолчание) на таком
            // файле не играет; здесь звучит то, что поместилось, и печатается,
            // сколько не влезло.
            args.prefetch_positions = 0xFFFF;
            args.progressive = true;
        }
    }

    xTaskCreate(app_main_task, "app_main", 4096, &args, tskIDLE_PRIORITY + 1, nullptr);
    vTaskStartScheduler();

    // Недостижимо при нормальной работе - app_main_task сама завершает
    // процесс через std::exit().
    std::fprintf(stderr, "vTaskStartScheduler() неожиданно вернул управление\n");
    return 1;
}
