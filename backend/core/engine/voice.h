#pragma once

// Голос: рендерит один канал, читая резидентные страницы сэмпла в PSRAM
// (sample_cache_find -> first_page) напрямую. Нерезидентный сэмпл
// (не влез или ещё не догружен) голос не запускает.
//
// Резидентные кодеки:
//  - Raw8 (8-битные исходники) - байт на отсчёт, декодера нет: отсчёт
//    читается по индексу (страница = индекс / kPsramPageBytes, смещение -
//    остаток), ни состояния, ни контрольных точек. До 16 бит (*256)
//    раскрывается при чтении.
//  - Dpcm8 (16-битные исходники) - 8-битная линейная (не адаптивная)
//    дельта. Тоже байт на отсчёт, page/byte_offset продвигаются как у Raw8.
//  - Raw16 (16-битные исходники, если трек влезает) - два байта на
//    отсчёт, прямая адресация.
//
// Петля: позиция декодера на loop_start запоминается один раз на голос
// (Dpcm8 - при первом проходе или при смещении за loop_start, прямые кодеки -
// на первом завороте или прыжке через loop_end), витки идут от неё.
// Голос играет только прямые петли: Dpcm8 назад не читается (предиктор
// направленный). Ping-pong разворачивается в прямую при упаковке; не
// поместившаяся в память ping-pong петля играется прямой.
//
// Частотные модели (Song::frequency_model): Amiga - через таблицу
// периодов, Linear (XM, современный IT) - экспонента от смещения в
// полутонах (relative_note учтён, finetune в стиле MOD/S3M - нет).

#include <cstdint>

#include "core/codec/dpcm8.h"
#include "core/engine/engine_defs.h"
#include "core/model/instrument.h"
#include "core/model/song.h"
#include "core/memory/psram_store.h"

namespace soundsinth::engine {

struct Voice {
    bool active = false;

    // --- Позиция в резидентной странице PSRAM ---
    soundsinth::model::ResidentEncoding resident_encoding = soundsinth::model::ResidentEncoding::Raw8;
    uint16_t first_page = memory::kPageChainEnd; // начало цепочки, нужно для перезапуска на петле
    uint16_t page = memory::kPageChainEnd;
    uint16_t byte_offset = 0;
    dpcm8::Dpcm8State dpcm_state; // только при resident_encoding == Dpcm8

    // Кэша чтения на голос нет: на 64 и 256 байт он медленнее прямого чтения
    // через psram_page_ptr() (хуже распределение регистров декодера).

    // --- Позиция в исходном потоке сэмпла, в отсчётах ---
    uint32_t decoded_count = 0; // сколько исходных отсчётов уже декодировано (индекс следующего)
    // Кэш питча: период или позиция питча и c5_speed (pitch_memo_c5), по
    // которым посчитан step. Оба поля стоят там, где не сдвигают поля цикла
    // рендера.
    int32_t pitch_memo = 0;

    // --- Петля (SampleDescriptor::loop_*, только прямая) ---
    bool loop_enabled = false;
    bool hermite = false; // эрмитова интерполяция; здесь - в дырке выравнивания
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // decoded_count >= loop_end -> перемотка на loop_start (не на конец сэмпла)
    // Позиция декодера на отсчёте loop_start, ставится один раз на голос.
    bool loop_checkpoint_captured = false;
    dpcm8::Dpcm8State loop_checkpoint_dpcm_state; // только при resident_encoding == Dpcm8
    uint16_t loop_checkpoint_page = memory::kPageChainEnd;
    uint16_t loop_checkpoint_byte_offset = 0;

    // --- Интерполяция между соседними исходными отсчётами ---
    // Линейная - между prev_sample и next_sample.
    //
    // Эрмитова (hermite, kQuirkHermiteInterpolation) - по четырём точкам
    // oldest, older, prev, next между older и prev, то есть с задержкой на
    // один родной отсчёт. Упреждения нет: декодер, петли и конец сэмпла те
    // же, что у линейной, окно само проходит через стык петли. Только Raw8
    // и Dpcm8.
    int16_t prev_sample = 0;
    int16_t next_sample = 0;
    int16_t older_sample = 0; // только hermite: отсчёт до prev_sample
    int16_t oldest_sample = 0; // только hermite: отсчёт до older_sample
    uint32_t pitch_memo_c5 = 0; // c5_speed кэша питча, 0 - кэша нет
    uint32_t frac_pos = 0; // Q16.16, всегда в [0, 0x10000)
    uint32_t step = 0; // Q16.16, прирост frac_pos за выходной отсчёт (питч)
};
// 96 слотов: каждые 4 байта Voice - 384 байта SRAM.
static_assert(sizeof(Voice) == 60, "Voice: поля переставлены или добавлены - пересчитать раскладку");

// Запускает голос с нуля: позиция start_offset, первые один-два отсчёта,
// step. Amiga: step = 428 * c5_speed / output_hz / period (программный
// микшер, не симуляция Paula). Linear: 2^(полутоны/12), нота 48 (IT с
// kQuirkItLinearC5Reference - 60) без finetune -> частота == c5_speed.
// Пустой или нерезидентный сэмпл - голос не запускается.
//
// start_offset - в отсчётах исходного файла, у прореженного сэмпла делится
// на 2. За концом (у зацикленного - за loop_end) голос по умолчанию не
// запускается, с kQuirkItOffsetPastEndRestarts играет с начала, с
// kQuirkItOldEffects - с последнего отсчёта. checkpoint_first_page нужен
// только Dpcm8 при start_offset > 0, без него - линейный проход.
// set_step - считать ли шаг по ноте здесь. Движок его не просит: высоту
// всем звучащим голосам выставляет синхронизация того же тика, до рендера.
// Эрмитова интерполяция этого сэмпла: у Raw16 её нет, он играет линейно.
inline bool voice_hermite(const soundsinth::model::SampleDescriptor& sample, soundsinth::model::QuirkFlags quirks) {
    return (quirks & soundsinth::model::kQuirkHermiteInterpolation) != 0 &&
           sample.resident_encoding != soundsinth::model::ResidentEncoding::Raw16;
}

struct TriggerStart {
    bool sounds = false;
    bool loop_ok = false;
    uint32_t offset = 0; // отсчёт начала, уже с правилами формата
};
TriggerStart voice_trigger_start(const soundsinth::model::SampleDescriptor& sample, uint16_t first_page,
                                 uint32_t start_offset, soundsinth::model::QuirkFlags quirks);

// Запуск голоса по уже разобранному началу ноты: отсчёт, годность петли и
// интерполяция посчитаны вызывающим (voice_trigger_start, voice_hermite).
// Шаг не считается - его ставит высота того же тика.
void voice_trigger_prepared(Voice& voice, memory::PsramStore& psram, const soundsinth::model::SampleDescriptor& sample,
                            uint16_t first_page, uint16_t checkpoint_first_page, const TriggerStart& start,
                            bool hermite);

void voice_trigger(Voice& voice, memory::PsramStore& psram, const soundsinth::model::SampleDescriptor& sample,
                   uint16_t first_page, uint8_t note, soundsinth::model::FrequencyModel frequency_model,
                   uint32_t start_offset = 0, soundsinth::model::QuirkFlags quirks = 0,
                   uint16_t checkpoint_first_page = memory::kPageChainEnd, bool set_step = true);

// Пересчёт step по кэшу питча. Не встраиваются, лежат во флеше: зовутся,
// только когда питч сдвинулся.
// Исход запуска ноты: зазвучит ли она и с какого отсчёта. Чистая функция от
// входов voice_trigger - управляющая часть узнаёт исход, не заглядывая в
// голос. Ноты нет у пустого и нерезидентного сэмпла и у смещения за концом.
// Шаг Q16.16 по высоте: период Amiga или позиция питча Linear (1/64
// полутона) и c5_speed. Считает управляющая часть, голосу уходит готовое
// число. Считается счётчиком пересчётов питча.
uint32_t voice_step_amiga(uint16_t period, uint32_t c5_speed);
uint32_t voice_step_linear(int32_t amount_units, uint32_t c5_speed);

void voice_recompute_amiga_step(Voice& voice, uint16_t period, uint32_t c5_speed);
void voice_recompute_linear_step(Voice& voice, int32_t amount_units, uint32_t c5_speed);

// Пересчитывает Voice::step из известного Amiga-периода без ретриггера и
// без сброса позиции и фазы интерполяции. Для питч-эффектов диспетчера
// (PortaUp/PortaDown/TonePorta), двигающих период тик за тиком. Только
// модель Amiga, для Linear - voice_set_linear_pitch. На неактивном голосе
// ничего не делает.
//
// Зовётся каждый тик на каждый звучащий голос; у выдержанной ноты вход не
// меняется. Сравнение с кэшем питча встроено, в SRAM у вызывающего: иначе
// каждый голос каждый тик уходит во флеш (64-битное деление, у Linear -
// программный pow).
inline void voice_set_amiga_period(Voice& voice, uint16_t period, uint32_t c5_speed) {
    if (!voice.active) return; // нечего двигать
    if (voice.pitch_memo == period && voice.pitch_memo_c5 == c5_speed) return;
    voice_recompute_amiga_step(voice, period, c5_speed);
}

// Пересчитывает Voice::step из непрерывной позиции питча (amount_units,
// 1/64 полутона) - аналог voice_set_amiga_period для модели Linear.
// amount_units - абсолютное смещение от опорной ноты
// (kReferenceNote/kItLinearReferenceNote), заданное на Note-Trigger:
// native_hz = c5_speed * 2^(amount_units/64/12). На неактивном голосе
// ничего не делает.
inline void voice_set_linear_pitch(Voice& voice, int32_t amount_units, uint32_t c5_speed) {
    if (!voice.active) return; // как voice_set_amiga_period
    if (voice.pitch_memo == amount_units && voice.pitch_memo_c5 == c5_speed) return;
    voice_recompute_linear_step(voice, amount_units, c5_speed);
}

// Рендерит до n_frames моно-отсчётов int16 без громкости (громкость
// накладывает TrackerEngine) в out; возвращает число отрендеренных. У
// незацикленного сэмпла может быть меньше n_frames: сэмпл кончился,
// voice.active = false. У зацикленного всегда n_frames, пока голос не
// остановят извне.
uint32_t voice_render(Voice& voice, memory::PsramStore& psram, int16_t* out, uint32_t n_frames);

// Диагностические счётчики голоса с последнего сброса. Пишет только
// рендер, читает диагностика того же ядра.
struct VoiceDebugCounters {
    // Вызовы декодера: сколько раз понадобился новый исходный отсчёт (шаг
    // пересёк границу, а не каждый voice_render()). В прошивке -
    // decode_per_1k_vs, декодов на 1000 голос-отсчётов: знаменатель для
    // роста ns_per_voice_sample.
    uint32_t decode_calls;
    // Из них - посчитавшие значение, не попавшее в интерполяцию. На высоком
    // питче на выходной отсчёт приходится N декодов, используются два
    // последних (prev_sample/next_sample), остальные нужны только для
    // позиции и предиктора Dpcm8. По кодеку: у Dpcm8 выброшенный декод -
    // таблица, у прямых (Raw8, Raw16) - сдвиг или сборка двух байт.
    uint32_t discarded_dpcm8;
    uint32_t discarded_direct;
    // Прыжки прямых кодеков: обходят decode_and_advance() и в decode_calls
    // не видны.
    uint32_t direct_jumps;
    // Сколько раз step_from_double() прижала шаг к kMaxStepF. Должно быть 0.
    uint32_t step_clamps;
    // Пересчёты step по новому питчу (кэш не совпал) - сколько раз
    // voice_set_* дошли до pow и делений.
    uint32_t pitch_recalcs;
};

VoiceDebugCounters voice_debug_counters();

// Обнуляет счётчики; новый счётчик - поле выше и строка в сбросе.
// Обязательно при старте трека: их знаменатель,
// TrackerEngine::voice_sample_count_, сбрасывается в start(); без сброса
// на втором треке decode_per_1k_vs доходит до 48 миллионов.
void voice_reset_debug_counters();

} // namespace soundsinth::engine
