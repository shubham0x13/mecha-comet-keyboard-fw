# Copyright (c) 2026 Mecha Systems
# SPDX-License-Identifier: BSD-3-Clause

if(CONFIG_WS2812_STRIP_GPIO_MCXC)
  zephyr_library()
  zephyr_library_sources(ws2812_gpio_mcxc.c)
endif()
