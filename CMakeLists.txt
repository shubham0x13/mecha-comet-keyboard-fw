# Copyright (c) 2026 Mecha Systems
# SPDX-License-Identifier: BSD-3-Clause

add_subdirectory(drivers)

# ZMK headers are private to the app target, so ZMK modules add their sources to it.
target_sources_ifdef(CONFIG_MECHA_KEYBOARD_LAYER_LEDS app PRIVATE src/layer_leds.c)

target_sources_ifdef(CONFIG_MECHA_KEYBOARD_ROM_BOOTLOADER app PRIVATE src/reboot.c)

# Zephyr's in-tree drivers/led_strip library has no sources when none of its
# drivers are enabled, which is the case when only an out-of-tree one is used.
if(TARGET drivers__led_strip)
  set_property(TARGET drivers__led_strip PROPERTY ALLOW_EMPTY TRUE)
endif()
