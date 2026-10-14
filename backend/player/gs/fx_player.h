// SPDX-License-Identifier: MIT
#pragma once

// Четыре канала эффектов General Sound отдельным источником шины.
//
// Движок трекера и эффекты - разные источники. У карты эффект занимает один
// из четырёх каналов и глушит то, что там играло; здесь канал модуля глушит
// мост по active_mask(), если настройка не велит играть эффекты поверх.
// Учёт каналов как у карты: занятость, приоритеты, SeekFirst/SeekLast
// решает автомат, и по протоколу поведение неотличимо. Явное глушение (#3A)
// исполняется.
//
// Панорама жёсткая, как у карты: каналы 0 и 1 налево, 2 и 3 направо.
// Интерполяции нет - у карты её тоже нет.
//
// Запуск приходит из прерывания шины, а звук считает другая задача.
// Прерывание поэтому только оставляет заявку, разбирает её рендер: иначе
// поля голоса менялись бы под читающим.

#include <atomic>
#include <cstdint>

#include "core/audio/sound_source.h"
#include "devices/gs/gs_device.h"

namespace player::gs {

class FxPlayer {
public:
    // Заполняется здесь, а не конструктором: с конструктором объект ушёл бы
    // в статическую инициализацию, а с ненулевым начальным значением поля -
    // ещё и в .data, где на плате каждый байт на счету. Вызов идемпотентен.
    soundsinth::mixbus::SoundSource* as_sound_source() {
        iface_.self       = this;
        iface_.render_add = &FxPlayer::render_add_thunk;
        return &iface_;
    }

    // Откуда брать байты сэмплов и чья таблица их описывает. Смещения в
    // FxSample считаются от base. ready - сколько байт области уже лежит
    // по base (младшие kFxReadyBytesMask разрядов): длину сэмпла автомат
    // знает с #D2, а байты переносит основной поток позже. Запуск ждёт,
    // пока сэмпл не ляжет целиком. nullptr - байты уже на месте.
    void attach(const devices::gs::GsDevice* dev, const uint8_t* base, const std::atomic<uint32_t>* ready = nullptr) {
        dev_   = dev;
        base_  = base;
        ready_ = ready;
    }

    // Заявки с шины. Зовутся из прерывания, поэтому только пишут поля и
    // взводят бит - разбирает их рендер.
    void request_start(const devices::gs::FxStart& cmd);
    void request_stop(uint8_t mask) { stop_mask_.fetch_or(mask, std::memory_order_release); }
    // Смена ноты и громкости канала на лету, #A0-#A3 и #A8-#AB.
    void request_note(uint8_t channel, uint8_t note);
    void request_volume(uint8_t channel, uint8_t volume);

    // Какие каналы звучат. По этой маске мост глушит каналы модуля.
    uint8_t active_mask() const { return active_.load(std::memory_order_acquire); }

    // Номер доигравшего сам запуска по каналу (FxStart::seq). Читает
    // автомат: канал свободен, когда номер равен последнему выданному.
    const std::atomic<uint16_t>* ended_seqs() const { return ended_; }

    // Громкость эффектов, #00..#40. Команд, которые её ставят, две (#2B и
    // #3D), но на звук идёт одна величина - та, что записана последней.
    void set_fx_volume(uint8_t v) { fx_vol_.store(v, std::memory_order_relaxed); }

    // Всё замолчало и забыто: сброс карты, возврат области треку.
    void reset();

    // Диагностика.
    uint32_t started() const { return started_; }
    // Сколько раз запуск ждал байтов сэмпла.
    uint32_t waited() const { return waited_; }

private:
    struct Voice {
        const uint8_t* data = nullptr;
        uint32_t end        = 0; // конец проигрывания: петля или длина
        uint32_t loop_begin = 0;
        uint32_t pos        = 0; // целая часть позиции в сэмпле
        uint32_t frac       = 0; // дробная, Q32
        uint32_t step_int   = 0;
        uint32_t step_frac  = 0;
        uint16_t seq        = 0; // номер запуска - для отчёта о конце
        uint8_t volume      = 0;
        uint8_t finetune    = 0; // нужен, чтобы пересчитать шаг при смене ноты
        bool loop           = false;
        bool signed_pcm     = false;
        bool active         = false;
    };

    static void render_add_thunk(void* self, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);
    void render_add(int32_t* mix_l, int32_t* mix_r, uint32_t n_frames);
    void apply_requests();
    void apply_changes();

    soundsinth::mixbus::SoundSource iface_{};
    const devices::gs::GsDevice* dev_                      = nullptr;
    const uint8_t* base_                                   = nullptr;
    const std::atomic<uint32_t>* ready_                    = nullptr;
    std::atomic<uint16_t> ended_[devices::gs::kFxChannels] = {};

    Voice ch_[devices::gs::kFxChannels] = {};
    // Пишет рендер, читает Core1: по этой маске мост решает, какой канал
    // модуля глушить.
    std::atomic<uint8_t> active_{0};
    // Ноль, а не умолчание карты: ненулевой инициализатор поля кладёт весь
    // объект в .data. Рабочее значение ставит мост, он его и так шлёт на
    // каждом опросе - громкость меняется с шины.
    std::atomic<uint8_t> fx_vol_{0};
    uint32_t started_ = 0;
    uint32_t waited_  = 0;

    // Заявки: по слоту на канал, чтобы две подряд не потерялись.
    //
    // Маски атомарные. Заявку ставит обработчик порта
    // на Core1, принимает её рендер на Core0, и обе стороны меняют слово
    // целиком: "или" с битом против "и-не" с принятым. Чтения-изменения-
    // записи с двух ядер теряют бит, когда запись одной стороны попадает
    // между чтением и записью другой. Потерянная заявка на останов - это
    // не исполненное глушение канала, то есть канал модуля замолкает
    // навсегда. Поэтому заявка - fetch_or, приём - exchange(0): взять всё
    // и очистить одним действием.
    devices::gs::FxStart pending_[devices::gs::kFxChannels] = {};
    std::atomic<uint8_t> pending_mask_{0};
    // Счётчик записи слота: нечётный - обработчик на Core1 пишет слот прямо
    // сейчас. Рендер на Core0, застав нечётный или изменившийся счётчик,
    // оставляет заявку до следующего буфера, а не играет порванную.
    std::atomic<uint32_t> pending_seq_[devices::gs::kFxChannels] = {};
    std::atomic<uint8_t> stop_mask_{0};
    // Смена на лету: значение по каналу плюс бит "есть что применить".
    uint8_t pending_note_[devices::gs::kFxChannels] = {};
    uint8_t pending_vol_[devices::gs::kFxChannels]  = {};
    std::atomic<uint8_t> note_mask_{0};
    std::atomic<uint8_t> vol_mask_{0};
};

// Младшие разряды слова готовности - байты области; старшие - поколение
// сброса, его ведёт мост.
inline constexpr uint32_t kFxReadyBytesMask = 0x00ffffffu;

// Один на прошивку: его заводит звуковая цепочка, а кормит мост GS.
FxPlayer& fx_player();

} // namespace player::gs
