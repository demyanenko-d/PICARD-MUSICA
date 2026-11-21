// SPDX-License-Identifier: MIT
// Сигналы машине: сброс и NMI.
//
// Обе линии импульсные, и держать их плата обязана сама: запрос только
// отмечает срок, отпускает виток. Занятым ожиданием тут нельзя - тот же
// виток обслуживает USB и подкачку.

#include "hal/host_signals.h"

#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "platform/boot_mode.h"
#include "platform/compiler.h"
#include "platform/hot_path.h"
#include "platform/log.h"
#include "platform/mono_time.h"

#include "devices/hal/host_signals.h"
#include "devices/bdi/bdi.h"
#include "devices/hid/usb_map.h"
#include "devices/storage/storage_host.h"

#include "player/config/config_service.h"
#include "player/gs/bridge.h"
#include "player/live/ay_live.h"
#include "player/live/saa_live.h"
#include "player/shared_state.h"

#include "bus.h"

namespace {

// Сброс держится заметно дольше, чем нужно процессору: за линией инвертор
// с диодом, и машина отпускает её со своей постоянной времени.
constexpr uint32_t kResetHoldUs = 50000;

// NMI защёлкивается фронтом, и по такту машины хватило бы сотен
// микросекунд: при 3.5 МГц это уже сотни тактов. Но линия монтажная, с
// подтяжкой и своей ёмкостью - как у сброса, где по той же причине стоят
// десятки миллисекунд. С 200 мкс машина ловит нажатие не всегда, по
// два-три раза на срабатывание. Пять миллисекунд взяты с запасом на
// постоянную времени линии, а не по такту процессора.
constexpr uint32_t kNmiHoldUs = 5000;

// Отсрочка перед перезапуском: строка журнала уходит задачей логгера, и
// без паузы она не успевает выйти наружу - разбирать потом будет нечего.
constexpr uint32_t kHardResetDelayUs = 200000;

uint32_t s_reset_until = 0;
uint32_t s_nmi_until   = 0;
uint32_t s_hard_at     = 0;
bool s_reset_held      = false;
bool s_nmi_held        = false;
bool s_hard_armed      = false;
// Перезапуск затеян ради конфигуратора: запрос отличает его от обычного.
bool s_hard_config = false;
// До разбора настроек - разрешён.
bool s_reset_allowed = true;

// Срок вышел? Разность беззнаковых переживает заворот счётчика.
bool expired(uint32_t deadline) {
    return static_cast<int32_t>(platform::mono_us() - deadline) >= 0;
}

// Машину сбросили: замок записи на носители снят, Beta Disk и AY сброшены,
// как линией RESET; музыка без хоста и карта GS - по настройке.
SOUNDSINTH_NOINLINE void devices_after_machine_reset() {
    shared::g_machine_resets.store(shared::g_machine_resets.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    devices::storage::storage_release_writers();
    devices::bdi::bdi_machine_reset();
    player::live::ay_live_machine_reset();
    player::live::saa_live_machine_reset();
    if (player::config::settings().stop_music_on_reset != 0) {
        shared::music_stop_request(true);
    } else {
        player::gs::bridge_machine_reset_light();
    }
}

} // namespace

void devices::hal::host_reset_request() {
    if (!s_reset_allowed) {
        debug_log("signal: machine reset disabled by settings\n");
        return;
    }
    // Повторное нажатие при удерживаемой линии только продлевает импульс.
    s_reset_until = platform::mono_us() + kResetHoldUs;
    if (s_reset_held) return;
    s_reset_held = true;
    bus::assert_host_reset();
    // Интерфейс Beta после сброса стоит в исходном, ПЗУ TR-DOS не
    // страничено: признак обязан совпасть, иначе машина стартует с чужим
    // окном памяти.
    bus::trdos_release();
    devices_after_machine_reset();
    // Уровень читается после снятия: в режиме DISKSYS=TRDOS вывод отдан
    // автомату dos_drive, и запись в регистр SIO до пада не доходит -
    // единица здесь означает, что признак действительно снят.
    debug_logf("signal: machine reset, DOS_N=%u\n", gpio_get(bus::PIN_DOS_N) ? 1u : 0u);
}

void devices::hal::host_nmi_request() {
    s_nmi_until = platform::mono_us() + kNmiHoldUs;
    if (s_nmi_held) return;
    s_nmi_held = true;
    // Линия монтажная: её тянет вниз кнопка, а мы - переводом вывода в
    // выход. Защёлка выхода стоит в нуле с загрузки, поэтому вверх линия
    // не идёт никогда и встречно кнопке не работает.
    gpio_set_dir(bus::PIN_NMI_N, GPIO_OUT);
    debug_log("signal: NMI\n");
}

void devices::hal::host_music_stop_request() {
    shared::music_stop_request(false);
}

void devices::hal::host_hard_reset_request() {
    if (s_hard_armed) return;
    s_hard_armed = true;
    s_hard_at    = platform::mono_us() + kHardResetDelayUs;
    // Машину держим с этой секунды: до перезапуска платы она иначе успеет
    // обратиться к эмуляторам, которых сейчас не станет. Запрещён сброс
    // (RESET_SIGNAL=0) - перезапускается только плата: линию сброса не
    // трогает ни этот запрос, ни загрузка платы (boot_holds_host).
    if (s_reset_allowed) {
        s_reset_held  = true;
        s_reset_until = platform::mono_us() + kHardResetDelayUs + kResetHoldUs;
        bus::assert_host_reset();
    }
    debug_log("signal: hard reset, the board is restarting\n");
}

void devices::hal::host_configurator_request() {
    if (s_hard_armed) return;
    s_hard_armed  = true;
    s_hard_at     = platform::mono_us() + kHardResetDelayUs;
    s_hard_config = true;
    // Машину держим сразу, и сброс здесь не спрашивает настройки: войти в
    // конфигуратор - явное действие пользователя, а ключ RESET_SIGNAL
    // существует для машин, которым плата не должна мешать сама по себе.
    s_reset_held  = true;
    s_reset_until = platform::mono_us() + kHardResetDelayUs + kResetHoldUs;
    bus::assert_host_reset();
    debug_log("signal: configurator requested, the board is restarting\n");
}

namespace rp2350::hal {

void host_signals_allow_reset(bool allow) {
    s_reset_allowed = allow;
}

void SOUNDSINTH_HOT_PATH(host_signals_tick)() {
    if (s_hard_armed && expired(s_hard_at)) {
        // Перезапуск с нуля: загрузка приведёт эмуляторы в исходное; машину
        // держит в сбросе, только если сброс разрешён.
        if (s_hard_config) {
            platform::BootRequest req;
            req.configurator = true;
            platform::boot_reboot(req); // не возвращается
        }
        watchdog_reboot(0, 0, 0);
        return;
    }
    if (s_reset_held && expired(s_reset_until)) {
        bus::release_host_reset();
        s_reset_held = false;
    }
    // Сброс машины по тишине на M1 (кнопка машины или свой): режим
    // клавиатуры - обычный, как после сброса с клавиатуры. ПЗУ TR-DOS уже
    // снято в bus_queues_poll.
    static uint32_t s_resets_handled = 0;
    if (const uint32_t seen = bus::machine_resets_seen(); seen != s_resets_handled) {
        s_resets_handled = seen;
        devices::hid::usb_map_set_key_mode(devices::hid::KeyMode::Normal);
        devices_after_machine_reset();
    }
    // Меню навигатора: музыка ждёт выхода из него.
    shared::g_nmi_mute.store(bus::nav_open(), std::memory_order_relaxed);
    if (s_nmi_held && expired(s_nmi_until)) {
        gpio_set_dir(bus::PIN_NMI_N, GPIO_IN);
        s_nmi_held = false;
    }
}

} // namespace rp2350::hal
