// Сигналы машине (devices/hal/host_signals.h): сброс и NMI.
//
// Обе линии импульсные, и держать их плата обязана сама: запрос только
// отмечает срок, отпускает виток. Занятым ожиданием тут нельзя - тот же
// виток обслуживает USB и подкачку.

#include "devices/hal/host_signals.h"

#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "bus.h"
#include "hal/host_signals.h"
#include "platform/log.h"
#include "platform/mono_time.h"

namespace {

// Сброс держится заметно дольше, чем нужно процессору: за линией инвертор
// с диодом, и машина отпускает её со своей постоянной времени.
constexpr uint32_t kResetHoldUs = 50000;

// NMI защёлкивается фронтом, держать его дальше незачем. Сотни микросекунд
// хватает и на самый медленный такт машины.
constexpr uint32_t kNmiHoldUs = 200;

// Отсрочка перед перезапуском: строка журнала уходит задачей логгера, и
// без паузы она не успевает выйти наружу - разбирать потом будет нечего.
constexpr uint32_t kHardResetDelayUs = 200000;

uint32_t s_reset_until = 0;
uint32_t s_nmi_until = 0;
uint32_t s_hard_at = 0;
bool s_reset_held = false;
bool s_nmi_held = false;
bool s_hard_armed = false;

// Срок вышел? Разность беззнаковых переживает заворот счётчика.
bool expired(uint32_t deadline) {
    return static_cast<int32_t>(platform::mono_us() - deadline) >= 0;
}

} // namespace

void devices::hal::host_reset_request() {
    // Повторное нажатие при удерживаемой линии только продлевает импульс.
    s_reset_until = platform::mono_us() + kResetHoldUs;
    if (s_reset_held) return;
    s_reset_held = true;
    bus::assert_host_reset();
    debug_log("сигнал: сброс машины\n");
}

void devices::hal::host_nmi_request() {
    s_nmi_until = platform::mono_us() + kNmiHoldUs;
    if (s_nmi_held) return;
    s_nmi_held = true;
    // Линия монтажная: её тянет вниз кнопка, а мы - переводом вывода в
    // выход. Защёлка выхода стоит в нуле с загрузки, поэтому вверх линия
    // не идёт никогда и встречно кнопке не работает.
    gpio_set_dir(bus::PIN_NMI_N, GPIO_OUT);
    debug_log("сигнал: NMI\n");
}

void devices::hal::host_hard_reset_request() {
    if (s_hard_armed) return;
    s_hard_armed = true;
    s_hard_at = platform::mono_us() + kHardResetDelayUs;
    // Машину держим с этой секунды: до перезапуска платы она иначе успеет
    // обратиться к эмуляторам, которых сейчас не станет.
    s_reset_held = true;
    s_reset_until = platform::mono_us() + kHardResetDelayUs + kResetHoldUs;
    bus::assert_host_reset();
    debug_log("сигнал: полный сброс, плата перезапускается\n");
}

namespace rp2350::hal {

void host_signals_tick() {
    if (s_hard_armed && expired(s_hard_at)) {
        // Перезапуск с нуля: загрузка платы сама поднимет машину в сбросе,
        // приведёт в исходное эмуляторы и отпустит её, когда будет готова.
        watchdog_reboot(0, 0, 0);
        return;
    }
    if (s_reset_held && expired(s_reset_until)) {
        bus::release_host_reset();
        s_reset_held = false;
    }
    if (s_nmi_held && expired(s_nmi_until)) {
        gpio_set_dir(bus::PIN_NMI_N, GPIO_IN);
        s_nmi_held = false;
    }
}

} // namespace rp2350::hal
