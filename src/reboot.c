/*
 * Copyright (c) 2026 Mecha Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Reboot into the boot ROM for the &bootloader key.
 *
 * ZMK's reset behavior calls sys_reboot(RST_UF2) for &bootloader and
 * sys_reboot(RST_WARM) for &sys_reset. Zephyr's Cortex-M sys_arch_reboot()
 * ignores the type, so both keys only reset. This replaces it: for &bootloader
 * it first sets the reset control module to boot from the ROM at the next
 * reset, as if the BOOT pin (PTA4) were held low. Nothing is written to flash,
 * so a reset or power cycle returns to the application.
 */

#include <soc.h>

#include <zephyr/kernel.h>

#include <dt-bindings/zmk/reset.h>

void sys_arch_reboot(int type) {
    if (type == RST_UF2) {
        RCM->FM = RCM_FM_FORCEROM(1);
    }

    NVIC_SystemReset();
}
