// SPDX-License-Identifier: MIT
// Режим загрузки и запрос на следующую: RP2350.
//
// Запрос - одно слово в регистре POWMAN, всегда запитанного домена.
// Сторожевой scratch[0] занят счётчиком загрузок, тем, что печатает строка
// "boot: reset".

#include "platform/boot_mode.h"

#include <cstring>

#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/powman.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico.h"
#include "hardware/irq.h"
#include "pico/multicore.h"
#include "pico/platform.h"

#include "platform/config_flash.h"

#include "platform/log.h"

#include "bus.h" // PIN_BOOT_JUMPER
#include "hal/boot_save.h"

namespace {

// Старшие три байта метки, младший - признаки.
constexpr uint32_t kMagic            = 0x43464700u; // 'CFG' и байт под признаки
constexpr uint32_t kMagicMask        = 0xffffff00u;
constexpr uint32_t kFlagConfigurator = 1u << 0;
constexpr uint32_t kFlagSaved        = 1u << 1;
constexpr uint32_t kFlagDropGui      = 1u << 2;

constexpr uint32_t kSlotFlags = 0;

uint32_t flags_of(const platform::BootRequest& req) {
    uint32_t flags = kMagic;
    if (req.configurator) flags |= kFlagConfigurator;
    if (req.saved) flags |= kFlagSaved;
    if (req.drop_open_gui) flags |= kFlagDropGui;
    return flags;
}

} // namespace

void platform::boot_reboot(const BootRequest& req) {
    powman_hw->scratch[kSlotFlags] = flags_of(req);

    // Перезагрузка без возврата; срок ненулевой, иначе сторож не успевает
    // взвестись.
    watchdog_reboot(0, 0, 1);
    for (;;) {
        tight_loop_contents();
    }
}

namespace {

// Запись ведёт Core0. PSM гасит домены по порядку, и выключение домена
// гасит все следующие за ним: FRCE_OFF у PROC0 сбрасывает и PROC1. PROC1 в
// порядке последний и выключается один. Поэтому Core1 передаёт задание
// через doorbell и ждёт, а Core0 выключает его.
struct SaveJob {
    platform::BootRequest req;
    uint32_t slot;
    const uint8_t* page;
    uint32_t bytes;
};
SaveJob s_job;
int s_bell = -1;

// Core1 и DMA исполняют и читают из флеша, а на время стирания окна XIP
// нет. FRCE_OFF сторожевой сброс не снимает, и Core1 осталось бы
// выключенным после загрузки. Поэтому бит снимается вручную, и сброс
// выдаётся следующей же записью.
[[noreturn]] void save_and_reset(SaveJob job) {
    (void)save_and_disable_interrupts();

    hw_set_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    while ((psm_hw->done & PSM_DONE_PROC1_BITS) != 0u) {
        tight_loop_contents();
    }

    for (uint32_t ch = 0; ch < NUM_DMA_CHANNELS; ++ch) {
        hw_clear_bits(&dma_hw->ch[ch].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    }
    dma_hw->abort = (1u << NUM_DMA_CHANNELS) - 1u;
    while (dma_hw->abort != 0u) {
        tight_loop_contents();
    }

    const uint8_t* const dst = platform::config_flash_view(job.slot);
    if (dst != nullptr && job.page != nullptr && job.bytes == FLASH_SECTOR_SIZE) {
        const uint32_t off = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dst) - XIP_BASE);
        flash_range_erase(off, job.bytes);
        flash_range_program(off, job.page, job.bytes);
        job.req.saved = std::memcmp(dst, job.page, job.bytes) == 0;
    }
    powman_hw->scratch[kSlotFlags] = flags_of(job.req);

    watchdog_hw->scratch[4] = 0; // без адреса входа: обычная загрузка с флеша
    hw_set_bits(&psm_hw->wdsel, PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS));
    hw_clear_bits(&psm_hw->frce_off, PSM_FRCE_OFF_PROC1_BITS);
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    for (;;) {
        tight_loop_contents();
    }
}

void bell_isr() {
    save_and_reset(s_job);
}

} // namespace

void rp2350::hal::boot_save_init() {
    s_bell = multicore_doorbell_claim_unused(1u << 0, false);
    if (s_bell < 0) return;
    const uint irq = multicore_doorbell_irq_num(static_cast<uint>(s_bell));
    irq_set_exclusive_handler(irq, bell_isr);
    // Выше порога FreeRTOS: критические секции планировщика его не держат.
    irq_set_priority(irq, 0);
    irq_set_enabled(irq, true);
}

void platform::boot_reboot_saving(BootRequest req, uint32_t slot, const uint8_t* page, uint32_t bytes) {
    s_job = SaveJob{req, slot, page, bytes};
    if (get_core_num() == 0u) save_and_reset(s_job);
    if (s_bell < 0) boot_reboot(req); // без обработчика на Core0: выход без записи
    (void)save_and_disable_interrupts();
    __dmb();
    multicore_doorbell_set_other_core(static_cast<uint>(s_bell));
    for (;;) {
        tight_loop_contents();
    }
}

bool platform::boot_request_peek(BootRequest& out) {
    const uint32_t flags = powman_hw->scratch[kSlotFlags];
    if ((flags & kMagicMask) != kMagic) return false;

    out               = BootRequest{};
    out.configurator  = (flags & kFlagConfigurator) != 0u;
    out.drop_open_gui = (flags & kFlagDropGui) != 0u;
    out.saved         = (flags & kFlagSaved) != 0u;
    return true;
}

bool platform::boot_request_take(BootRequest& out) {
    debug_logf("boot: carryover %08lx\n", static_cast<unsigned long>(powman_hw->scratch[kSlotFlags]));
    const bool have = boot_request_peek(out);
    // Гасится в любом случае: запрос одноразовый, и битый не должен
    // пережить эту загрузку.
    powman_hw->scratch[kSlotFlags] = 0;
    return have;
}

bool platform::boot_jumper_closed() {
    gpio_init(bus::PIN_BOOT_JUMPER);
    gpio_set_dir(bus::PIN_BOOT_JUMPER, GPIO_IN);
    gpio_pull_up(bus::PIN_BOOT_JUMPER);
    // Подтяжке нужно время поднять вход: ёмкость провода к перемычке на
    // порядок больше ёмкости пада.
    busy_wait_us(100);
    const bool closed = !gpio_get(bus::PIN_BOOT_JUMPER);
    return closed;
}

bool platform::config_rom_active() {
    return bus::config_rom_enabled();
}

uint8_t* platform::config_rom_page() {
    return bus::config_rom_enabled() ? bus::config_rom_page() : nullptr;
}

uint32_t platform::config_rom_page_bytes() {
    return bus::config_rom_page_bytes();
}
