// SPDX-License-Identifier: MIT
#include "i2s_sink.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "FreeRTOS.h" // configMAX_SYSCALL_INTERRUPT_PRIORITY

#include "audio_pins.h"
#include "hw_config.h"
#include "i2s_pio.pio.h"
#include "player/shared_state.h"

namespace rp2350::audio {

namespace {

using player::audio::BufferPool;

// Стерео-кадр - одно слово FIFO.
constexpr uint32_t kBlockWords = BufferPool::kFramesPerBuffer;

struct I2sSinkState {
    BufferPool* pool        = nullptr;
    uint32_t sample_rate_hz = 0;

    PIO pio                 = nullptr;
    uint sm                 = 0;
    uint pio_offset         = 0;
    volatile void* dma_dest = nullptr;
    uint dma_dreq           = 0;

    // Каналы берёт dma_init, до неё ISR не работает.
    int dma_ping = 0;
    int dma_pong = 0;

    // Буферы в каналах DMA; nullptr - канал играет тишину.
    const int16_t* volatile ping_data = nullptr;
    const int16_t* volatile pong_data = nullptr;

    volatile uint32_t underrun = 0;
    // Из них - пока Core1 догружал сэмпл фоном: признак снимается в момент
    // андеррана, а не при печати.
    volatile uint32_t underrun_bg = 0;
    // Обработчик опоздал больше чем на буфер: канал уже перезапущен цепочкой
    // и играет память за прошлым буфером, перезарядка пропущена.
    volatile uint32_t late = 0;
    // Сколько готовых буферов ждало в очереди, когда канал забирал следующий
    // во время игры (i2s_sink_ready_stats).
    volatile uint32_t ready[rp2350::audio::kReadyBins] = {};

    // true - источника звука нет (трек снят: смена трека или выход из
    // плагина). DMA гонит тишину, а не повторяет буферы
    // (i2s_sink_set_silent()). Пишет Core0, читает ISR. До старта синка
    // ISR не работает, запись просто запоминается; перед стартом её всегда
    // делает app_task.
    volatile bool silent = false;
};

// Единственный экземпляр: обработчик DMA IRQ не несёт контекст
// (irq_set_exclusive_handler принимает void(*)()). На уровне файла, без
// неконстантных инициализаторов: блок PIO ставит hw_init_i2s. Умолчания
// нулевые - объект в .bss.
I2sSinkState s_state;

// Тишина для DMA - одно слово, которое канал читает без приращения адреса.
// В SRAM и не const: const легла бы во флеш, а DMA с флеша - лишняя
// нагрузка на QMI, общий с PSRAM. Буфер из тысячи нулей для этого не нужен:
// переносов столько же, адрес один.
uint32_t s_silence_word = 0;

// Настройка канала в двух вариантах на сторону: с приращением адреса для
// музыки и без - для тишины. Заполняются в dma_init, различаются одним
// битом; chain_to у сторон разный, потому по паре на каждую.
dma_channel_config_t s_cfg_data[2];
dma_channel_config_t s_cfg_silence[2];

#ifndef SOUNDSINTH_RP2350_I2S_PHASE_TEST
#define SOUNDSINTH_RP2350_I2S_PHASE_TEST 0
#endif
#if SOUNDSINTH_RP2350_I2S_PHASE_TEST
// Узор для замера сборки кадра: вместо музыки знак меняется через два
// отсчёта (11 кГц), одинаково в обоих каналах. Смотреть двумя щупами на
// выходе, соотношение осциллограмм; какой канал где - неважно.
//   совпадают  - кадр собран из одного отсчёта, всё верно;
//   четверть периода - правое слово от следующего отсчёта;
//   противофаза - канал инвертирован, дело не в кадре.
// Смена знака каждый отсчёт для этого не годится: там сдвиг на отсчёт и
// инверсия дают одно и то же полпериода.
constexpr int16_t kPhaseTestAmp = 0x6000;
uint32_t s_phase_test[kBlockWords];
static_assert(kBlockWords % 4 == 0, "the pattern continues across the block boundary");

void fill_phase_test() {
    for (uint32_t i = 0; i < kBlockWords; ++i) {
        const int16_t v     = (i & 2u) ? static_cast<int16_t>(-kPhaseTestAmp) : kPhaseTestAmp;
        const uint32_t half = static_cast<uint16_t>(v);
        s_phase_test[i]     = half | (half << 16); // оба канала одинаковые
    }
}
#endif

// --- DMA IRQ: ping-pong с chain_to ---

// Обслужить один завершившийся канал; общий код для ping и pong.
// held - буфер, который канал играл до сих пор (ping_data/pong_data
// вызывающего), обновляется по ссылке.
//
// inline не для связи: без него GCC зовёт функцию из dma_irq_handler
// вызовом, а не встраивает (образ на 56 байт меньше, код ISR другой).
inline void __not_in_flash_func(refill_channel)(I2sSinkState& s, uint chan, const int16_t* volatile& held, uint32_t side) {
    const void* addr;
    if (!s.silent && held) {
        const uint32_t k = s.pool->rendered_count_from_isr();
        ++s.ready[k < rp2350::audio::kReadyBins ? k : rp2350::audio::kReadyBins - 1u];
    }
    if (s.silent) {
        // Играть нечего (трек снят): на шину тишина. Это не заминка, счётчик
        // не трогается, иначе он накручивался бы тысячами на каждой смене
        // трека.
        //
        // Буферы при этом прокручиваются, а не перестают забираться: иначе
        // RenderTask виснет в begin_write(), её stop() не дожидается семафора,
        // и app_task больше не соберёт следующий трек. Забрать и сразу
        // вернуть - выбросить отрендеренный буфер, сохранив круговорот пула.
        if (const int16_t* discard = s.pool->try_begin_read_from_isr()) {
            s.pool->end_read_from_isr(discard);
        }
        if (held) {
            s.pool->end_read_from_isr(held);
            held = nullptr;
        }
        addr = &s_silence_word;
    } else if (const int16_t* next = s.pool->try_begin_read_from_isr()) {
        if (held) s.pool->end_read_from_isr(held);
        held = next;
        addr = next;
    } else if (!held) {
        // Трек ещё не начался (тишина снимается до первого готового буфера):
        // тишина, это не заминка.
        addr = &s_silence_word;
    } else {
        // Настоящая заминка: рендер не успел. Канал снова играет свой прежний
        // буфер, сыгранный до играющего сейчас (порядок A B A C), - для
        // одиночной заминки мягче щелчка в ноль.
        ++s.underrun;
        if (shared::g_background_loading.load(std::memory_order_relaxed)) ++s.underrun_bg;
        addr = held ? static_cast<const void*>(held) : static_cast<const void*>(&s_silence_word);
    }
#if SOUNDSINTH_RP2350_I2S_PHASE_TEST
    addr = s_phase_test; // ключ замера: круговорот буферов выше сохранён
#endif
    // Вариант настройки по тому, что играем: у тишины адрес один и тот же,
    // приращение выключено. Канал в этот момент завершён (проверено
    // dma_channel_is_busy у вызывающего), запись в CTRL безопасна.
    const bool silence = addr == static_cast<const void*>(&s_silence_word);
    dma_channel_set_config(chan, silence ? &s_cfg_silence[side] : &s_cfg_data[side], false);
    dma_channel_set_read_addr(chan, addr, false);
    dma_channel_set_trans_count(chan, kBlockWords * 2u, false); // по полуслову на канал
}

void __not_in_flash_func(dma_irq_handler)() {
    I2sSinkState& s = s_state;

    const bool ping_done = dma_irqn_get_channel_status(bus::DMA_IRQ_INDEX_AUDIO, s.dma_ping);
    const bool pong_done = dma_irqn_get_channel_status(bus::DMA_IRQ_INDEX_AUDIO, s.dma_pong);
    if (!ping_done && !pong_done) return;

    if (ping_done) {
        dma_irqn_acknowledge_channel(bus::DMA_IRQ_INDEX_AUDIO, s.dma_ping);
        if (!dma_channel_is_busy(s.dma_ping)) {
            refill_channel(s, s.dma_ping, s.ping_data, 0);
        } else {
            ++s.late;
        }
    }
    if (pong_done) {
        dma_irqn_acknowledge_channel(bus::DMA_IRQ_INDEX_AUDIO, s.dma_pong);
        if (!dma_channel_is_busy(s.dma_pong)) {
            refill_channel(s, s.dma_pong, s.pong_data, 1);
        } else {
            ++s.late;
        }
    }
}

// --- PIO ---

void hw_init_i2s(I2sSinkState& s) {
    // База GPIO 16 уже стоит (i2s_sink_set_block_base на старте платы).
    s.pio = bus::PIO_AUDIO;

    s.pio_offset = pio_add_program(s.pio, &i2s_out_program);
    s.sm         = pio_claim_unused_sm(s.pio, true);

    pio_sm_config cfg = i2s_out_program_get_default_config(s.pio_offset);
    sm_config_set_out_pins(&cfg, kI2sData, 1);
    sm_config_set_sideset_pins(&cfg, kI2sBck); // BCK - бит 0, WS - бит 1
    // Сдвиг влево (I2S - старшим битом вперёд), autopull, порог 16: автомат
    // берёт по одному каналу за раз, и порядок слов в кадре задаёт порядок
    // полуслов в памяти, а не половины 32-битного слова.
    sm_config_set_out_shift(&cfg, false, true, 16);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);

    // 16 бит x 2 канала, 2 такта автомата на бит -> 64 такта на стерео-кадр.
    const float sys_clk = static_cast<float>(clock_get_hz(clk_sys));
    const float freq_sm = static_cast<float>(s.sample_rate_hz) * 64.0f;
    sm_config_set_clkdiv(&cfg, sys_clk / freq_sm);

    static constexpr uint kI2sPins[3] = {kI2sData, kI2sBck, kI2sWs};
    for (uint pin : kI2sPins) {
        pio_gpio_init(s.pio, pin);
    }

    // Ток и фронт заданы явно, а не по умолчанию (4 мА, медленный фронт).
    //
    // По таймингу запас десятикратный: такт автомата 354 нс, данные меняются
    // с падающим фронтом BCK (все четыре out pins,1 в i2s_pio.pio несут BCK=0
    // в side-set), приёмник читает по нарастающему - 354 нс на установку и
    // столько же на удержание при 12 нс установки и 2 нс удержания, которых
    // просит TDA1387T.
    //
    // Дело в помехоустойчивости: чем медленнее фронт, тем дольше линия в зоне
    // порога и тем легче наводке от шины Z80 (Core1 работает с ней непрерывно)
    // дать ложный перепад BCK. Лишний фронт сдвигает кадр на бит до
    // ближайшего WS: на громком отсчёте это слышимый щелчок. По набору за
    // половину шкалы заходит 0.099% отсчётов, почти все на ударах.
    for (uint pin : kI2sPins) {
        gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
        gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
    }
    pio_sm_set_consecutive_pindirs(s.pio, s.sm, kI2sBck, 2, true); // BCK+WS - выходы
    pio_sm_set_consecutive_pindirs(s.pio, s.sm, kI2sData, 1, true);

    pio_sm_init(s.pio, s.sm, s.pio_offset, &cfg);

    s.dma_dest = reinterpret_cast<volatile void*>(&s.pio->txf[s.sm]);
    s.dma_dreq = pio_get_dreq(s.pio, s.sm, true);
}

// Детерминированное стартовое состояние автомата, затем запуск. Задержки
// здесь нет: при пустом FIFO автомат встал бы на статичный BCK, а на нём
// выход хрипит; она в i2s_sink_start после dma_channel_start(), когда BCK
// уже тактирует.
void hw_start_i2s(I2sSinkState& s) {
    pio_sm_set_enabled(s.pio, s.sm, false);
    pio_sm_clear_fifos(s.pio, s.sm);
    pio_sm_restart(s.pio, s.sm);
    pio_sm_clkdiv_restart(s.pio, s.sm);
    pio_sm_exec(s.pio, s.sm, pio_encode_jmp(s.pio_offset + i2s_out_offset_entry_point));
    pio_sm_set_enabled(s.pio, s.sm, true);
}

// --- DMA ---

// Каналы заряжены тишиной и не запущены, их IRQ подтверждены; прерывание
// линии DMA_IRQ_INDEX_AUDIO включает вызывающий после этого - раньше завершений нет, и
// проверки "запущен ли синк" в ISR не нужно.
void dma_init(I2sSinkState& s) {
    s.dma_ping = dma_claim_unused_channel(true);
    s.dma_pong = dma_claim_unused_channel(true);

    for (uint i = 0; i < 2; ++i) {
        const uint ch   = (i == 0) ? s.dma_ping : s.dma_pong;
        const uint next = (i == 0) ? s.dma_pong : s.dma_ping;

        dma_channel_config_t dc = dma_channel_get_default_config(ch);
        // Обычный приоритет, выставлен явно: каналы шины высокоприоритетные, звук уступает им намеренно.
        //
        // У Z80 нет /WAIT: не отдали байт за 570-825 нс - он защёлкнул 0xFF,
        // прямая порча данных. У звука между сменами буферов целый блок запаса,
        // задержка в несколько слотов DMA ему не страшна.
        channel_config_set_high_priority(&dc, false);
        // По полуслову на канал, в порядке памяти: сначала левый, затем
        // правый. Узкая запись в регистр периферии повторяется на обе
        // половины шины, и в FIFO приходит полуслово в обеих половинах -
        // автомат с порогом 16 берёт старшую.
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
        channel_config_set_read_increment(&dc, true);
        channel_config_set_write_increment(&dc, false);
        channel_config_set_dreq(&dc, s.dma_dreq);
        channel_config_set_chain_to(&dc, next); // аппаратный автозапуск, без паузы

        // Оба варианта запоминаются здесь: в обработчике остаётся выбор из
        // двух готовых, без сборки настройки по полям.
        s_cfg_data[i]    = dc;
        s_cfg_silence[i] = dc;
        channel_config_set_read_increment(&s_cfg_silence[i], false);

        // Заряжены тишиной - значит и настройкой без приращения.
        dc = s_cfg_silence[i];
        dma_channel_configure(ch, &dc, s.dma_dest, &s_silence_word, kBlockWords * 2u, false);
        dma_irqn_acknowledge_channel(bus::DMA_IRQ_INDEX_AUDIO, ch);
        dma_irqn_set_channel_enabled(bus::DMA_IRQ_INDEX_AUDIO, ch, true);
    }

    irq_set_exclusive_handler(DMA_IRQ_NUM(bus::DMA_IRQ_INDEX_AUDIO), dma_irq_handler);
    // Наивысший приоритет, совместимый с *FromISR: SysTick/PendSV FreeRTOS
    // (0xF0) этот IRQ не вытесняют. Значение из общей карты ресурсов, рядом с
    // шинными: звук самый низкий.
    static_assert(bus::IRQ_PRIO_RELAXED == configMAX_SYSCALL_INTERRUPT_PRIORITY, "IRQ_PRIO_RELAXED must match the scheduler BASEPRI threshold");
    irq_set_priority(DMA_IRQ_NUM(bus::DMA_IRQ_INDEX_AUDIO), bus::IRQ_PRIO_RELAXED);
}

} // namespace

uint32_t i2s_sink_underrun_count() {
    return s_state.underrun;
}
uint32_t i2s_sink_underrun_bg_count() {
    return s_state.underrun_bg;
}
uint32_t i2s_sink_late_count() {
    return s_state.late;
}

ReadyStats i2s_sink_ready_stats() {
    ReadyStats r{};
    for (uint32_t k = 0; k < kReadyBins; ++k) {
        r.ready[k] = s_state.ready[k];
    }
    return r;
}

void i2s_sink_set_silent(bool silent) {
    s_state.silent = silent;
}

void i2s_sink_set_block_base() {
    // RP2350B: GPIO 44-46 > 31, нужна база GPIO 16.
    pio_set_gpio_base(bus::PIO_AUDIO, 16);
}

void i2s_sink_start(BufferPool& pool, uint32_t sample_rate_hz) {
#if SOUNDSINTH_RP2350_I2S_PHASE_TEST
    fill_phase_test();
#endif
    I2sSinkState& s  = s_state;
    s.pool           = &pool;
    s.sample_rate_hz = sample_rate_hz;

    hw_init_i2s(s);
    dma_init(s);
    hw_start_i2s(s);
    irq_set_enabled(DMA_IRQ_NUM(bus::DMA_IRQ_INDEX_AUDIO), true);
    // Буферы подхватит DMA IRQ на первом завершении блока тишины.
    dma_channel_start(s.dma_ping);
    // 2 мс после старта DMA: BCK тактирует тишиной. Без неё тон, поданный
    // сразу после старта DMA, хрипит. За 2 мс DMA первый блок не
    // заканчивает, IRQ не срабатывает.
    busy_wait_us_32(2000);
}

} // namespace rp2350::audio
