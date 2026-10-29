/*
 * Copyright (c) 2026 Mecha Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * WS2812 LED strip driver for NXP MCX C, bit-banging one GPIO pin.
 *
 * The in-tree worldsemi,ws2812-gpio driver only supports Nordic SoCs because
 * its assembly writes the nRF GPIO registers. This driver uses the same
 * devicetree binding and the same approach for MCX C: the pin is toggled
 * through the single-cycle FGPIO alias of the port, from a function that runs
 * from RAM with interrupts locked, so that flash wait states and interrupts
 * cannot change the pulse widths.
 *
 * Pulse widths, in core clock cycles (20.8 ns each at 48 MHz). H is the time
 * between the rising and the falling edge, L the time between the falling edge
 * and the next rising edge. "n" is the cost of one store to FGPIO, 1 cycle
 * for the single-cycle I/O port.
 *
 *   0 bit: H = 2 + t0h + n,  L = 6 + t0l + n   (14 + t0l + n after a byte)
 *   1 bit: H = 3 + t1h + n,  L = 4 + t1l + n   (12 + t1l + n after a byte)
 *
 * The extra cycles are the shift, test and loop instructions of the loop below.
 * The values in the devicetree node are the number of nop instructions that
 * pad each phase.
 *
 * A frame takes about 0.3 ms with interrupts locked. An interrupt-driven I2C
 * transfer that is running during that time can return wrong data (the ADS1115
 * of the pointing stick returned 0xFFFF). When the board names an I2C module in
 * the mecha,ws2812-wait-for-i2c chosen property, a frame is therefore only sent
 * while that bus is idle.
 */

#define DT_DRV_COMPAT worldsemi_ws2812_gpio

#include <soc.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/dt-bindings/led/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ws2812_gpio_mcxc, CONFIG_LED_STRIP_LOG_LEVEL);

BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == 48000000,
             "The WS2812 pulse timing is written for a 48 MHz core clock");

/* Byte offsets of the set and clear registers in a Kinetis GPIO port. */
#define FGPIO_PSOR_OFFSET 0x04
#define FGPIO_PCOR_OFFSET 0x08

/* Defaults for 48 MHz, giving about 0.4 us / 1.0 us and 0.96 us / 0.4 us. */
#define WS2812_DEFAULT_T0H 16
#define WS2812_DEFAULT_T0L 41
#define WS2812_DEFAULT_T1H 42
#define WS2812_DEFAULT_T1L 14

#ifdef CONFIG_WS2812_STRIP_GPIO_MCXC_WAIT_FOR_I2C
/* The I2C module whose bus must be idle while a frame is sent. */
#define WS2812_I2C_NODE DT_CHOSEN(mecha_ws2812_wait_for_i2c)
/* Poll the bus in steps of this many microseconds, for at most this long. */
#define WS2812_I2C_POLL_US 20
#define WS2812_I2C_WAIT_MAX_US 2000
#define WS2812_MCXC_I2C_INIT                                                                       \
    .i2c = (const I2C_Type *)DT_REG_ADDR(WS2812_I2C_NODE),                                         \
    .i2c_dev = DEVICE_DT_GET(WS2812_I2C_NODE),
#else
#define WS2812_MCXC_I2C_INIT
#endif

struct ws2812_mcxc_config {
    struct gpio_dt_spec gpio;
    /* FGPIO alias of the port that holds the data pin. */
    volatile uint32_t *fgpio;
    uint32_t pin_mask;
    const uint8_t *color_mapping;
    uint8_t num_colors;
    size_t length;
    /* On-wire frame, length * num_colors bytes. */
    uint8_t *frame;
    uint32_t reset_delay_us;
#ifdef CONFIG_WS2812_STRIP_GPIO_MCXC_WAIT_FOR_I2C
    /* I2C module whose bus must be idle while a frame is sent, and its device. */
    const I2C_Type *i2c;
    const struct device *i2c_dev;
#endif
};

struct ws2812_mcxc_data {
    struct k_mutex lock;
    /* Cycle counter value when the last frame ended. */
    uint32_t last_frame_end;
    bool sent_once;
#ifdef CONFIG_WS2812_STRIP_GPIO_MCXC_WAIT_FOR_I2C
    /* Frames that were sent while the I2C bus was still busy. */
    uint32_t i2c_timeouts;
#endif
};

/* The FGPIO alias of a GPIO port is at 0xF8000000 plus the offset of the port. */
#define WS2812_FGPIO(node_id)                                                                      \
    ((volatile uint32_t *)(0xF8000000UL | (DT_REG_ADDR(node_id) & 0xFFFUL)))

static void ws2812_mcxc_wait_reset(const struct ws2812_mcxc_config *config,
                                   struct ws2812_mcxc_data *data) {
    uint32_t elapsed_us;

    if (!data->sent_once) {
        return;
    }

    elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - data->last_frame_end);
    if (elapsed_us < config->reset_delay_us) {
        k_busy_wait(config->reset_delay_us - elapsed_us);
    }
}

/* Value of the colour channel with the given LED_COLOR_ID_* that is sent for a pixel. */
static uint8_t ws2812_mcxc_channel(const struct led_rgb *pixel, uint8_t color_id) {
    switch (color_id) {
    case LED_COLOR_ID_RED:
        return pixel->r;
    case LED_COLOR_ID_GREEN:
        return pixel->g;
    case LED_COLOR_ID_BLUE:
        return pixel->b;
    default:
        /* LED_COLOR_ID_WHITE: the LED strip API has no white channel. */
        return 0;
    }
}

#ifdef CONFIG_WS2812_STRIP_GPIO_MCXC_WAIT_FOR_I2C
/*
 * Lock interrupts at a moment when no I2C transfer is running and return the key.
 *
 * The bus busy flag is set from the START to the STOP condition, so it also covers the time
 * between the two messages of a transfer with a repeated START. With interrupts locked no thread
 * can start a transfer and no I2C interrupt can continue one, so the bus stays idle for the whole
 * frame. While a transfer is running, interrupts are on for a short time so that it can finish.
 *
 * If the bus is still busy after WS2812_I2C_WAIT_MAX_US, the frame is sent anyway so that a stuck
 * bus cannot stop the LEDs, and timed_out is set.
 */
static unsigned int ws2812_mcxc_lock_when_i2c_idle(const struct ws2812_mcxc_config *config,
                                                   bool *timed_out) {
    unsigned int key;
    unsigned int polls = 0;

    *timed_out = false;

    /*
     * No transfer can be running on an I2C module whose driver did not start, and the registers
     * of a module that is not clocked must not be read.
     */
    if (!device_is_ready(config->i2c_dev)) {
        return irq_lock();
    }

    while (true) {
        key = irq_lock();

        if ((config->i2c->S & I2C_S_BUSY_MASK) == 0) {
            return key;
        }

        if (polls >= WS2812_I2C_WAIT_MAX_US / WS2812_I2C_POLL_US) {
            *timed_out = true;
            return key;
        }

        irq_unlock(key);
        k_busy_wait(WS2812_I2C_POLL_US);
        polls++;
    }
}
#endif

static int
ws2812_mcxc_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num_pixels,
                       void (*send)(volatile uint32_t *, uint32_t, const uint8_t *, size_t)) {
    const struct ws2812_mcxc_config *config = dev->config;
    struct ws2812_mcxc_data *data = dev->data;
    uint8_t *ptr = config->frame;
    size_t len = num_pixels * config->num_colors;
    uint32_t cycles = 0;
    unsigned int key;

    if (num_pixels > config->length) {
        return -EINVAL;
    }

    if (num_pixels == 0) {
        return 0;
    }

    k_mutex_lock(&data->lock, K_FOREVER);

    /* Convert from RGB to the on-wire colour order. */
    for (size_t i = 0; i < num_pixels; i++) {
        for (uint8_t j = 0; j < config->num_colors; j++) {
            *ptr++ = ws2812_mcxc_channel(&pixels[i], config->color_mapping[j]);
        }
    }

    ws2812_mcxc_wait_reset(config, data);

#ifdef CONFIG_WS2812_STRIP_GPIO_MCXC_WAIT_FOR_I2C
    bool i2c_timed_out;

    key = ws2812_mcxc_lock_when_i2c_idle(config, &i2c_timed_out);
#else
    key = irq_lock();
#endif

    /*
     * In a debug build, measure how long the frame takes with interrupts locked. The count must
     * match the pulse widths in the comment at the top of this file: a 0 bit takes
     * 10 + t0h + t0l cycles, a 1 bit 9 + t1h + t1l cycles, plus 8 cycles per byte.
     */
    if (IS_ENABLED(CONFIG_LED_STRIP_LOG_LEVEL_DBG)) {
        cycles = k_cycle_get_32();
    }
    send(config->fgpio, config->pin_mask, config->frame, len);
    if (IS_ENABLED(CONFIG_LED_STRIP_LOG_LEVEL_DBG)) {
        cycles = k_cycle_get_32() - cycles;
    }
    irq_unlock(key);

    data->last_frame_end = k_cycle_get_32();
    data->sent_once = true;

    if (IS_ENABLED(CONFIG_LED_STRIP_LOG_LEVEL_DBG)) {
        LOG_DBG("%u bytes sent in %u cycles", (unsigned int)len, (unsigned int)cycles);
    }

#ifdef CONFIG_WS2812_STRIP_GPIO_MCXC_WAIT_FOR_I2C
    if (i2c_timed_out) {
        data->i2c_timeouts++;
        /* Log the first timeout and then every time the count doubles. */
        if (IS_POWER_OF_TWO(data->i2c_timeouts)) {
            LOG_WRN("I2C bus busy for %d us, frame sent anyway (%u times)", WS2812_I2C_WAIT_MAX_US,
                    data->i2c_timeouts);
        }
    }
#endif

    k_mutex_unlock(&data->lock);

    return 0;
}

static int ws2812_mcxc_init(const struct device *dev) {
    const struct ws2812_mcxc_config *config = dev->config;
    struct ws2812_mcxc_data *data = dev->data;

    if (!gpio_is_ready_dt(&config->gpio)) {
        LOG_ERR("GPIO port not ready");
        return -ENODEV;
    }

    if (config->gpio.dt_flags & GPIO_ACTIVE_LOW) {
        LOG_ERR("The data pin must be active high");
        return -EINVAL;
    }

    for (uint8_t i = 0; i < config->num_colors; i++) {
        switch (config->color_mapping[i]) {
        case LED_COLOR_ID_WHITE:
        case LED_COLOR_ID_RED:
        case LED_COLOR_ID_GREEN:
        case LED_COLOR_ID_BLUE:
            break;
        default:
            LOG_ERR("%s: invalid colour mapping", dev->name);
            return -EINVAL;
        }
    }

    k_mutex_init(&data->lock);

    /* Selects the GPIO function of the pin and drives it low. */
    return gpio_pin_configure_dt(&config->gpio, GPIO_OUTPUT_INACTIVE);
}

static size_t ws2812_mcxc_length(const struct device *dev) {
    const struct ws2812_mcxc_config *config = dev->config;

    return config->length;
}

/*
 * Send len bytes, most significant bit first. len must not be zero.
 *
 * Register use: %[g] FGPIO port, %[m] pin mask, %[p] data pointer, %[c] byte
 * count, %[b] current byte (kept in bits 31..24), %[n] bit counter.
 *
 * Conditional branches only reach 256 bytes, so the loops use a conditional
 * branch over an unconditional one.
 *
 * GCC emits inline assembly for Thumb-1 in divided syntax and switches back to
 * unified syntax afterwards, so the block selects unified syntax for its own
 * mnemonics and restores divided syntax at its end.
 */
#define WS2812_MCXC_SEND(idx)                                                                      \
    static __ramfunc __noinline void ws2812_mcxc_##idx##_send(                                     \
        volatile uint32_t *gpio, uint32_t mask, const uint8_t *buf, size_t len) {                  \
        uint32_t byte;                                                                             \
        uint32_t bits;                                                                             \
                                                                                                   \
        __asm volatile(".syntax unified\n"                                                         \
                       "1:\n"                                                                      \
                       "\tldrb\t%[b], [%[p]]\n"                                                    \
                       "\tadds\t%[p], %[p], #1\n"                                                  \
                       "\tlsls\t%[b], %[b], #24\n"                                                 \
                       "\tmovs\t%[n], #8\n"                                                        \
                       "2:\n"                                                                      \
                       "\tstr\t%[m], [%[g], %[set]]\n"                                             \
                       "\tlsls\t%[b], %[b], #1\n"                                                  \
                       "\tbcs\t3f\n"                                                               \
                       "\t.rept\t%c[t0h]\n"                                                        \
                       "\tnop\n"                                                                   \
                       "\t.endr\n"                                                                 \
                       "\tstr\t%[m], [%[g], %[clr]]\n"                                             \
                       "\t.rept\t%c[t0l]\n"                                                        \
                       "\tnop\n"                                                                   \
                       "\t.endr\n"                                                                 \
                       "\tb\t4f\n"                                                                 \
                       "3:\n"                                                                      \
                       "\t.rept\t%c[t1h]\n"                                                        \
                       "\tnop\n"                                                                   \
                       "\t.endr\n"                                                                 \
                       "\tstr\t%[m], [%[g], %[clr]]\n"                                             \
                       "\t.rept\t%c[t1l]\n"                                                        \
                       "\tnop\n"                                                                   \
                       "\t.endr\n"                                                                 \
                       "4:\n"                                                                      \
                       "\tsubs\t%[n], %[n], #1\n"                                                  \
                       "\tbeq\t5f\n"                                                               \
                       "\tb\t2b\n"                                                                 \
                       "5:\n"                                                                      \
                       "\tsubs\t%[c], %[c], #1\n"                                                  \
                       "\tbeq\t6f\n"                                                               \
                       "\tb\t1b\n"                                                                 \
                       "6:\n"                                                                      \
                       ".syntax divided\n"                                                         \
                       : [p] "+l"(buf), [c] "+l"(len), [b] "=&l"(byte), [n] "=&l"(bits)            \
                       : [g] "l"(gpio), [m] "l"(mask), [set] "i"(FGPIO_PSOR_OFFSET),               \
                         [clr] "i"(FGPIO_PCOR_OFFSET),                                             \
                         [t0h] "i"(DT_INST_PROP_OR(idx, delay_t0h, WS2812_DEFAULT_T0H)),           \
                         [t0l] "i"(DT_INST_PROP_OR(idx, delay_t0l, WS2812_DEFAULT_T0L)),           \
                         [t1h] "i"(DT_INST_PROP_OR(idx, delay_t1h, WS2812_DEFAULT_T1H)),           \
                         [t1l] "i"(DT_INST_PROP_OR(idx, delay_t1l, WS2812_DEFAULT_T1L))            \
                       : "cc", "memory");                                                          \
    }

#define WS2812_MCXC_DEVICE(idx)                                                                    \
    WS2812_MCXC_SEND(idx)                                                                          \
                                                                                                   \
    static const uint8_t ws2812_mcxc_##idx##_color_mapping[] = DT_INST_PROP(idx, color_mapping);   \
                                                                                                   \
    static uint8_t ws2812_mcxc_##idx##_frame[DT_INST_PROP(idx, chain_length) *                     \
                                             DT_INST_PROP_LEN(idx, color_mapping)];                \
                                                                                                   \
    static int ws2812_mcxc_##idx##_update_rgb(const struct device *dev, struct led_rgb *pixels,    \
                                              size_t num_pixels) {                                 \
        return ws2812_mcxc_update_rgb(dev, pixels, num_pixels, ws2812_mcxc_##idx##_send);          \
    }                                                                                              \
                                                                                                   \
    static DEVICE_API(led_strip, ws2812_mcxc_##idx##_api) = {                                      \
        .update_rgb = ws2812_mcxc_##idx##_update_rgb,                                              \
        .length = ws2812_mcxc_length,                                                              \
    };                                                                                             \
                                                                                                   \
    static const struct ws2812_mcxc_config ws2812_mcxc_##idx##_config = {                          \
        .gpio = GPIO_DT_SPEC_INST_GET(idx, gpios),                                                 \
        .fgpio = WS2812_FGPIO(DT_GPIO_CTLR(DT_DRV_INST(idx), gpios)),                              \
        .pin_mask = BIT(DT_INST_GPIO_PIN(idx, gpios)),                                             \
        .color_mapping = ws2812_mcxc_##idx##_color_mapping,                                        \
        .num_colors = DT_INST_PROP_LEN(idx, color_mapping),                                        \
        .length = DT_INST_PROP(idx, chain_length),                                                 \
        .frame = ws2812_mcxc_##idx##_frame,                                                        \
        .reset_delay_us = DT_INST_PROP(idx, reset_delay),                                          \
        WS2812_MCXC_I2C_INIT};                                                                     \
                                                                                                   \
    static struct ws2812_mcxc_data ws2812_mcxc_##idx##_data;                                       \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(idx, ws2812_mcxc_init, NULL, &ws2812_mcxc_##idx##_data,                  \
                          &ws2812_mcxc_##idx##_config, POST_KERNEL,                                \
                          CONFIG_LED_STRIP_INIT_PRIORITY, &ws2812_mcxc_##idx##_api);

DT_INST_FOREACH_STATUS_OKAY(WS2812_MCXC_DEVICE)
