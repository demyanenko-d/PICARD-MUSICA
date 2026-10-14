// SPDX-License-Identifier: MIT
#include "player/gs/fx_player.h"

#include <cmath>
#include <cstring>

#include "core/engine/engine_defs.h"
#include "platform/hot_path.h"

namespace player::gs {
namespace {

// Высота тона. ПЗУ карты держит 16 таблиц периодов по 96 слов, и частота
// выходит как 9600000 / период, где 9600000 = 37500 x 256: частота
// прерывания микшера карты на приращение счётчика. Числа совпадают с
// амижным строем - нота 36 это C-1 Амиги, - поэтому частота считается
// формулой, а не таблицей карты: разница меньше округления таблицы.
constexpr double kRateNote0 = 9600000.0 / 18516.0; // 518.47 Гц, нота 0

// Finetune: знаковые четыре бита в 1/8 полутона, и берутся они из битов
// 4..1 байта - младший бит прошивка не смотрит. Замерено развёрткой всех
// 32 значений.
int fine_eighths(uint8_t finetune) {
    const int v = (finetune >> 1) & 0x0f;
    return v < 8 ? v : v - 16;
}

// Шаг чтения сэмпла на выходной кадр, целая часть и Q32-дробь. Считается
// на запуске, то есть редко: double здесь дешевле таблицы.
void step_for(uint8_t note, uint8_t finetune, uint32_t& step_int, uint32_t& step_frac) {
    const double semitones = static_cast<double>(note) + static_cast<double>(fine_eighths(finetune)) / 8.0;
    const double rate      = kRateNote0 * std::pow(2.0, semitones / 12.0);
    double step            = rate / static_cast<double>(soundsinth::engine::kSampleRateHz);
    if (step < 0.0) step = 0.0;
    if (step > 16.0) step = 16.0; // выше ноты 95 не бывает, это защита от мусора
    const double whole = std::floor(step);
    step_int           = static_cast<uint32_t>(whole);
    step_frac          = static_cast<uint32_t>((step - whole) * 4294967296.0);
}

// Уровень равен голосу трекера: у карты модуль и эффекты играют одними и
// теми же четырьмя каналами с одной шкалой громкости, значит сэмпл на
// громкости #40 обязан звучать как нота модуля на громкости 64.
//
// У движка это выходит так: усиление голоса - произведение громкости
// канала (единица) на sample_preamp, а MOD на четыре канала ставит его в
// 64 из 128, то есть половину. Отсчёт int16 на полной шкале даёт
// 32767 x 0.5 в масштабе шины Q24.8.
//
//   вклад = отсчёт(+-128) x громкость сэмпла x громкость эффектов x 8
//
// При обеих громкостях по #40 это 128 x 32768 - та же половина. Множитель 4
// (четыре канала ровно на шкалу) даёт эффекты вдвое тише музыки.
constexpr uint8_t kGainShift = 3;

} // namespace

// Заявки зовёт обработчик порта GS: в SRAM, путь шины без флеша.
SOUNDSINTH_HOT_PATH_ATTR("gs_fx_request_start")
void FxPlayer::request_start(const devices::gs::FxStart& cmd) {
    if (cmd.channel >= devices::gs::kFxChannels) return;
    std::atomic<uint32_t>& seq = pending_seq_[cmd.channel];
    const uint32_t s           = seq.load(std::memory_order_relaxed);
    seq.store(s + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    pending_[cmd.channel] = cmd;
    seq.store(s + 2u, std::memory_order_release);
    pending_mask_.fetch_or(static_cast<uint8_t>(1u << cmd.channel), std::memory_order_release);
}

SOUNDSINTH_HOT_PATH_ATTR("gs_fx_request_note")
void FxPlayer::request_note(uint8_t channel, uint8_t note) {
    if (channel >= devices::gs::kFxChannels) return;
    pending_note_[channel] = note;
    note_mask_.fetch_or(static_cast<uint8_t>(1u << channel), std::memory_order_release);
}

SOUNDSINTH_HOT_PATH_ATTR("gs_fx_request_volume")
void FxPlayer::request_volume(uint8_t channel, uint8_t volume) {
    if (channel >= devices::gs::kFxChannels) return;
    pending_vol_[channel] = volume;
    vol_mask_.fetch_or(static_cast<uint8_t>(1u << channel), std::memory_order_release);
}

void FxPlayer::reset() {
    for (Voice& v : ch_) {
        v = Voice{};
    }
    active_.store(0, std::memory_order_release);
    pending_mask_.store(0, std::memory_order_relaxed);
    stop_mask_.store(0, std::memory_order_relaxed);
    note_mask_.store(0, std::memory_order_relaxed);
    vol_mask_.store(0, std::memory_order_relaxed);
}

void FxPlayer::apply_requests() {
    // Взять всё и очистить одним действием: заявка, поставленная шиной
    // между чтением и записью, иначе пропадала бы.
    const uint8_t stop = stop_mask_.exchange(0, std::memory_order_acquire);
    if (stop != 0) {
        for (uint8_t c = 0; c < devices::gs::kFxChannels; ++c) {
            if ((stop & (1u << c)) != 0) ch_[c].active = false;
        }
    }
    const uint8_t start = pending_mask_.exchange(0, std::memory_order_acquire);
    if (start == 0) return;
    if (dev_ == nullptr || base_ == nullptr) return;
    for (uint8_t c = 0; c < devices::gs::kFxChannels; ++c) {
        if ((start & (1u << c)) == 0) continue;
        // Слот читается копией под счётчиком: Core1 мог начать писать
        // следующую заявку в этот же слот, пока мы читаем.
        const uint32_t s1              = pending_seq_[c].load(std::memory_order_acquire);
        const devices::gs::FxStart req = pending_[c];
        std::atomic_thread_fence(std::memory_order_acquire);
        if ((s1 & 1u) != 0u || pending_seq_[c].load(std::memory_order_relaxed) != s1) {
            pending_mask_.fetch_or(static_cast<uint8_t>(1u << c), std::memory_order_relaxed);
            continue;
        }
        if (req.length == 0) {
            // Играть нечего: канал отдаётся автомату сразу.
            ended_[c].store(req.seq, std::memory_order_relaxed);
            continue;
        }
        // Байты ещё в кольце моста: заявка ждёт следующего буфера. Иначе
        // голос играл бы то, что лежало в области до этого сэмпла.
        if (ready_ != nullptr && req.offset + req.length > (ready_->load(std::memory_order_acquire) & kFxReadyBytesMask)) {
            pending_mask_.fetch_or(static_cast<uint8_t>(1u << c), std::memory_order_relaxed);
            ++waited_;
            continue;
        }
        // Граница звука одна - конец петли (#49), с петлёй и без: отдельного
        // поля длины в заголовке карты нет, а после заливки конец петли
        // равен длине. Снимок уже ограничил её длиной и проверил петлю.
        Voice& v     = ch_[c];
        v            = Voice{};
        v.data       = base_ + req.offset;
        v.seq        = req.seq;
        v.loop       = req.loop;
        v.end        = req.end;
        v.loop_begin = req.loop_begin;
        v.volume     = req.volume > devices::gs::kModuleVolumeMax ? devices::gs::kModuleVolumeMax : req.volume;
        v.signed_pcm = req.signed_pcm;
        v.finetune   = req.finetune;
        step_for(req.note, req.finetune, v.step_int, v.step_frac);
        v.active = true;
        ++started_;
    }
}

// Смена на лету. Новый звук в канале её отменяет - параметры берутся из
// заголовка нового сэмпла: запуск переписывает голос целиком.
void FxPlayer::apply_changes() {
    const uint8_t notes = note_mask_.exchange(0, std::memory_order_acquire);
    if (notes != 0) {
        for (uint8_t c = 0; c < devices::gs::kFxChannels; ++c) {
            if ((notes & (1u << c)) == 0 || !ch_[c].active) continue;
            step_for(pending_note_[c], ch_[c].finetune, ch_[c].step_int, ch_[c].step_frac);
        }
    }
    const uint8_t vols = vol_mask_.exchange(0, std::memory_order_acquire);
    if (vols == 0) return;
    for (uint8_t c = 0; c < devices::gs::kFxChannels; ++c) {
        if ((vols & (1u << c)) == 0 || !ch_[c].active) continue;
        ch_[c].volume = pending_vol_[c] > devices::gs::kModuleVolumeMax ? devices::gs::kModuleVolumeMax : pending_vol_[c];
    }
}

void FxPlayer::render_add_thunk(void* self, int32_t* mix_l, int32_t* mix_r, uint32_t n_frames) {
    static_cast<FxPlayer*>(self)->render_add(mix_l, mix_r, n_frames);
}

void FxPlayer::render_add(int32_t* mix_l, int32_t* mix_r, uint32_t n_frames) {
    apply_requests();
    apply_changes();
    uint8_t active = 0;
    for (uint8_t c = 0; c < devices::gs::kFxChannels; ++c) {
        Voice& v = ch_[c];
        if (!v.active) continue;
        // Каналы 0 и 1 налево, 2 и 3 направо - разводка карты.
        int32_t* out = c < 2 ? mix_l : mix_r;
        // Громкости на блок: внутри цикла они не меняются.
        const int32_t gain = static_cast<int32_t>(v.volume) * static_cast<int32_t>(fx_vol_.load(std::memory_order_relaxed));
        for (uint32_t i = 0; i < n_frames; ++i) {
            if (v.pos >= v.end) {
                if (!v.loop) {
                    v.active = false;
                    ended_[c].store(v.seq, std::memory_order_relaxed);
                    break;
                }
                const uint32_t span = v.end - v.loop_begin;
                // Петля нулевой длины не крутится вечно: снимок её не
                // пропускает, но цикл ниже не должен зависеть от этого.
                if (span == 0u || span > v.end) {
                    v.active = false;
                    ended_[c].store(v.seq, std::memory_order_relaxed);
                    break;
                }
                do {
                    v.pos -= span;
                } while (v.pos >= v.end);
            }
            const uint8_t raw   = v.data[v.pos];
            const int32_t samp  = v.signed_pcm ? static_cast<int32_t>(static_cast<int8_t>(raw)) : static_cast<int32_t>(raw) - 128;
            out[i]             += (samp * gain) << kGainShift;

            const uint32_t was  = v.frac;
            v.frac             += v.step_frac;
            v.pos              += v.step_int + (v.frac < was ? 1u : 0u);
        }
        if (v.active) active = static_cast<uint8_t>(active | (1u << c));
    }
    active_.store(active, std::memory_order_release);
}

namespace {
FxPlayer s_fx; // все поля нулевые: объект лежит в .bss и не требует запуска
} // namespace

SOUNDSINTH_HOT_PATH_ATTR("gs_fx_player")
FxPlayer& fx_player() {
    return s_fx;
}

} // namespace player::gs
