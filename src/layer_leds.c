/*
 * Copyright (c) 2026 Mecha Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Layer LEDs: each LED is on while its layer is active, however the layer was
 * activated (held, toggled or one-shot), so both can be on at once.
 *
 * The layer numbers are layer IDs, the numbers used in keymap bindings such as
 * &mo 1 and &tog 2. They stay the same when layers are reordered in ZMK Studio.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct layer_led {
    struct led_dt_spec led;
    zmk_keymap_layer_id_t layer;
};

static const struct layer_led layer_leds[] = {
    {
        .led = LED_DT_SPEC_GET(DT_NODELABEL(layer1_led)),
        .layer = CONFIG_MECHA_KEYBOARD_LAYER_LED1_LAYER,
    },
    {
        .led = LED_DT_SPEC_GET(DT_NODELABEL(layer2_led)),
        .layer = CONFIG_MECHA_KEYBOARD_LAYER_LED2_LAYER,
    },
};

static int layer_leds_listener(const zmk_event_t *eh) {
    ARRAY_FOR_EACH(layer_leds, i) {
        const struct layer_led *layer_led = &layer_leds[i];

        if (!device_is_ready(layer_led->led.dev)) {
            continue;
        }

        int ret = zmk_keymap_layer_active(layer_led->layer) ? led_on_dt(&layer_led->led)
                                                            : led_off_dt(&layer_led->led);

        if (ret < 0) {
            LOG_ERR("Layer LED %zu update failed (%d)", i + 1, ret);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_leds, layer_leds_listener);
ZMK_SUBSCRIPTION(layer_leds, zmk_layer_state_changed);
