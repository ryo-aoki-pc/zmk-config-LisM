/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025 4mple Lab (Simple Lab)
 *
 * 充電中の LED 表示。4mplelab/zmk-feature-charge-indicator (f0d311e) を取り込み、次の点を直したもの:
 *
 * - 充電状態のピン (STAT) の割り込みの中で k_sleep していた。GPIO のコールバックは割り込みの中で
 *   呼ばれるので眠れない (割り込まれたスレッドを止めてしまい、スケジューラが壊れる)。USB を挿すと
 *   充電が始まってこの割り込みが起き、USB でつないでいる間キーボードが操作できなくなっていた。
 *   チャタリング除去の待ちは、遅延ワーク (システムのワークキュー) で行う
 * - 保守スレッドの優先度が K_LOWEST_THREAD_PRIO (アイドルスレッドと同じ) だった。
 *   アプリのスレッドに使える最も低い K_LOWEST_APPLICATION_THREAD_PRIO にする
 */

// src/charge_indicator.c
//
// ZMK Feature: Charge Indicator (DT-driven, rgbled_adapter optional)
// - Reads charging status from a DT-defined chg_stat node (raw GPIO level: 0 = charging).
// - During charging, suppress widget output and either show a selected color or force LEDs OFF (Kconfig).
// - When not charging, keep LEDs OFF and let rgbled_widget handle all LED indications (no shortened durations).
// - Works with rgbled_adapter OR with custom DT that defines led-red/green/blue aliases; if aliases are absent, LED control is skipped safely.
// - Split-friendly: run the same code on both halves; each side reads and shows its own charging state.
// - No heap usage: uses a static thread to gently re-apply charging state only while charging.
//

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_REGISTER(charge_indicator, CONFIG_ZMK_LOG_LEVEL);


/* Devicetree: Resolve charging status input from a chg_stat node via alias.
 * DT must provide:
 *   - a node: chg_stat: chg_stat { gpios = <&gpioX PIN GPIO_ACTIVE_LOW>; status = "okay"; };
 */
#define CHG_NODE        DT_NODELABEL(chg_stat)
#if !DT_NODE_HAS_STATUS(CHG_NODE, okay)
#error "DT alias chg-stat not found or not okay. Define aliases { chg-stat = &chg_stat; } and a chg_stat node."
#endif

#define CHG_CTLR        DT_GPIO_CTLR_BY_IDX(CHG_NODE, gpios, 0)
#define CHG_PIN_NUM     DT_GPIO_PIN_BY_IDX(CHG_NODE, gpios, 0)
/* Input flags:
 * - Use pull-up to keep the line high when PMIC STAT is open-drain (not charging).
 * - Do NOT set ACTIVE_LOW for input; we read raw level and treat 0 as charging consistently.
 */
#define CHG_PIN_FLAGS   (GPIO_INPUT | GPIO_PULL_UP)

/* Resolve RGB LED aliases, either provided by rgbled_adapter or your custom DT overlay.
 * If aliases are missing, we skip LED control entirely and let rgbled_widget (or others) use LEDs freely.
 */
#define LED_RED_ALIAS    DT_ALIAS(led_red)
#define LED_GREEN_ALIAS  DT_ALIAS(led_green)
#define LED_BLUE_ALIAS   DT_ALIAS(led_blue)

#if DT_NODE_HAS_STATUS(LED_RED_ALIAS, okay) && \
    DT_NODE_HAS_STATUS(LED_GREEN_ALIAS, okay) && \
    DT_NODE_HAS_STATUS(LED_BLUE_ALIAS, okay)
  /* Aliases present: enable LED control. */
  #define LEDR_CTLR   DT_GPIO_CTLR_BY_IDX(LED_RED_ALIAS, gpios, 0)
  #define LEDR_PIN    DT_GPIO_PIN_BY_IDX(LED_RED_ALIAS, gpios, 0)
  #define LEDR_FLAGS  (DT_GPIO_FLAGS_BY_IDX(LED_RED_ALIAS, gpios, 0) | GPIO_OUTPUT)

  #define LEDG_CTLR   DT_GPIO_CTLR_BY_IDX(LED_GREEN_ALIAS, gpios, 0)
  #define LEDG_PIN    DT_GPIO_PIN_BY_IDX(LED_GREEN_ALIAS, gpios, 0)
  #define LEDG_FLAGS  (DT_GPIO_FLAGS_BY_IDX(LED_GREEN_ALIAS, gpios, 0) | GPIO_OUTPUT)

  #define LEDB_CTLR   DT_GPIO_CTLR_BY_IDX(LED_BLUE_ALIAS, gpios, 0)
  #define LEDB_PIN    DT_GPIO_PIN_BY_IDX(LED_BLUE_ALIAS, gpios, 0)
  #define LEDB_FLAGS  (DT_GPIO_FLAGS_BY_IDX(LED_BLUE_ALIAS, gpios, 0) | GPIO_OUTPUT)
#else
  /* No LED aliases: disable LED control (always delegate to widget/other features). */
  #define CHARGE_INDICATOR_DISABLE_LED 1
#endif

/* State and devices */
static atomic_t is_charging = ATOMIC_INIT(false);
static const struct device *chg_dev;
#ifndef CHARGE_INDICATOR_DISABLE_LED
static const struct device *ledr_dev, *ledg_dev, *ledb_dev;
#endif
static struct gpio_callback chg_cb;

/* Maintenance thread: reapply charging state periodically to suppress widget while charging. */
K_THREAD_STACK_DEFINE(chg_maint_stack, 512);
static struct k_thread chg_maint_thread;

/* Common-anode RGB (gpio-leds with GPIO_ACTIVE_LOW): write logical 1 to turn LED ON. */
#ifndef CHARGE_INDICATOR_DISABLE_LED
static inline void led_red(bool on)   { gpio_pin_set(ledr_dev, LEDR_PIN, on ? 1 : 0); }
static inline void led_green(bool on) { gpio_pin_set(ledg_dev, LEDG_PIN, on ? 1 : 0); }
static inline void led_blue(bool on)  { gpio_pin_set(ledb_dev, LEDB_PIN, on ? 1 : 0); }

/* Apply LED color based on color code (0-7). */
static inline void apply_color_code(int color)
{
    LOG_DBG("Applying color code: %d", color);
    switch (color) {
        case 0: /* Black(off) */             led_red(false); led_green(false); led_blue(false); break;
        case 1: /* Red */                    led_red(true);  led_green(false); led_blue(false); break;
        case 2: /* Green */                  led_red(false); led_green(true);  led_blue(false); break;
        case 3: /* Yellow(R+G) */            led_red(true);  led_green(true);  led_blue(false); break;
        case 4: /* Blue */                   led_red(false); led_green(false); led_blue(true);  break;
        case 5: /* Magenta(R+B) */           led_red(true);  led_green(false); led_blue(true);  break;
        case 6: /* Cyan(G+B) */              led_red(false); led_green(true);  led_blue(true);  break;
        case 7: /* White(R+G+B) */           led_red(true);  led_green(true);  led_blue(true);  break;
        default: /* Fallback Red */          led_red(true);  led_green(false); led_blue(false); break;
    }
}

/* Get battery level based color code. */
#if IS_ENABLED(CONFIG_CHG_BATTERY_LEVEL_BASED_COLOR)
static int get_battery_level_color(void)
{
    uint8_t battery_pct = zmk_battery_state_of_charge();
    LOG_DBG("Battery level: %d%%", battery_pct);

    if (battery_pct < 0 || battery_pct > 100) {
        return CONFIG_CHG_BATTERY_COLOR_MISSING;
    }

    if (battery_pct < CONFIG_CHG_BATTERY_LEVEL_CRITICAL) {
        return CONFIG_CHG_BATTERY_COLOR_CRITICAL;
    } else if (battery_pct < CONFIG_CHG_BATTERY_LEVEL_LOW) {
        return CONFIG_CHG_BATTERY_COLOR_LOW;
    } else if (battery_pct < CONFIG_CHG_BATTERY_LEVEL_HIGH) {
        return CONFIG_CHG_BATTERY_COLOR_MEDIUM;
    } else {
        return CONFIG_CHG_BATTERY_COLOR_HIGH;
    }
}
#endif
#endif

/* Read raw physical level:
 * - 0 = charging (STAT active low, PMIC drives low)
 * - 1 = not charging (open-drain released; internal pull-up keeps high)
 */
static inline bool read_charging(void) {
    int raw = gpio_pin_get_raw(chg_dev, CHG_PIN_NUM);
    return raw == 0;
}

/* Apply LED behavior according to charging state and policy. */
static void apply_charging_color(bool charging)
{
#ifndef CHARGE_INDICATOR_DISABLE_LED
    if (charging) {
#if IS_ENABLED(CONFIG_CHG_POLICY)
        /* Charging: force LEDs OFF, fully suppress widget output. */
        led_red(false); led_green(false); led_blue(false);
#else
        /* Charging: show color based on configuration. */
#if IS_ENABLED(CONFIG_CHG_BATTERY_LEVEL_BASED_COLOR)
        /* Charging: show battery level based color, suppress widget output. */
        int color = get_battery_level_color();
        apply_color_code(color);
#else
        /* Charging: show fixed color, suppress widget output. */
        apply_color_code(CONFIG_CHG_COLOR);
#endif
#endif
    } else {
        /* Not charging: keep LEDs OFF and fully delegate to rgbled_widget/others. */
        led_red(false); led_green(false); led_blue(false);
    }
#else
    ARG_UNUSED(charging);
    /* No LED aliases: do nothing (always delegate). */
#endif
}

/* Debounced read: read raw -> update state -> apply behavior (system work queue). */
static void chg_work_handler(struct k_work *work)
{
    bool charging = read_charging();
    atomic_set(&is_charging, charging);
    apply_charging_color(charging);
}

static K_WORK_DELAYABLE_DEFINE(chg_work, chg_work_handler);

/* IRQ handler (runs in the GPIO ISR, where sleeping is not allowed): restart the short
 * debounce delay; the read happens in chg_work_handler. */
static void chg_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    k_work_reschedule(&chg_work, K_MSEC(8));
}

/* Battery state changed event handler: update LED color if charging. */
static int battery_state_changed_listener(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }

    if (atomic_get(&is_charging)) {
        apply_charging_color(true);
    }

    return 0;
}

ZMK_LISTENER(charge_indicator, battery_state_changed_listener);
ZMK_SUBSCRIPTION(charge_indicator, zmk_battery_state_changed);

/* Maintenance thread:
 * - While charging: periodically reapply to suppress widget (prevent short blinks).
 * - Not charging: sleep and do nothing (preserve widget timing completely).
 */
static void charging_maint_task(void)
{
    while (true) {
        if (atomic_get(&is_charging)) {
            apply_charging_color(true);
            k_sleep(K_MSEC(150)); /* Tune for stronger/weaker suppression vs. power. */
        } else {
            k_sleep(K_SECONDS(1));
        }
    }
}

/* Initialization:
 * - Resolve DT devices
 * - Configure pins
 * - Stabilization wait + double-read debounce for initial state
 * - IRQ setup
 * - Start maintenance thread
 */
static int charge_indicator_init(void)
{
    if (!IS_ENABLED(CONFIG_CHARGE_INDICATOR)) {
        LOG_INF("Charge indicator disabled by Kconfig");
        return 0;
    }

    /* Input (STAT) controller device */
    chg_dev  = DEVICE_DT_GET(CHG_CTLR);
    if (!device_is_ready(chg_dev)) {
        LOG_ERR("CHG GPIO controller not ready");
        return -ENODEV;
    }

#ifndef CHARGE_INDICATOR_DISABLE_LED
    /* LED controllers */
    ledr_dev = DEVICE_DT_GET(LEDR_CTLR);
    ledg_dev = DEVICE_DT_GET(LEDG_CTLR);
    ledb_dev = DEVICE_DT_GET(LEDB_CTLR);
    if (!device_is_ready(ledr_dev) || !device_is_ready(ledg_dev) || !device_is_ready(ledb_dev)) {
        LOG_ERR("LED GPIO controller not ready");
        return -ENODEV;
    }
#endif

    /* Configure STAT input with pull-up (raw read will be used). */
    int ret = gpio_pin_configure(chg_dev, CHG_PIN_NUM, CHG_PIN_FLAGS);
    if (ret) { LOG_ERR("CHG pin cfg failed: %d", ret); return ret; }

#ifndef CHARGE_INDICATOR_DISABLE_LED
    /* Configure RGB output pins. */
    ret = gpio_pin_configure(ledr_dev, LEDR_PIN, LEDR_FLAGS);
    if (ret) { LOG_ERR("LEDR cfg failed: %d", ret); return ret; }
    ret = gpio_pin_configure(ledg_dev, LEDG_PIN, LEDG_FLAGS);
    if (ret) { LOG_ERR("LEDG cfg failed: %d", ret); return ret; }
    ret = gpio_pin_configure(ledb_dev, LEDB_PIN, LEDB_FLAGS);
    if (ret) { LOG_ERR("LEDB cfg failed: %d", ret); return ret; }
#endif

    /* Initial stabilization + double-read debounce. */
    k_sleep(K_MSEC(20));
    bool c1 = read_charging();
    k_sleep(K_MSEC(10));
    bool c2 = read_charging();
    bool charging_init = (c1 && c2);
    atomic_set(&is_charging, charging_init);
    apply_charging_color(charging_init);

    /* IRQ on both edges. */
    ret = gpio_pin_interrupt_configure(chg_dev, CHG_PIN_NUM, GPIO_INT_EDGE_BOTH);
    if (ret) { LOG_ERR("CHG int cfg failed: %d", ret); return ret; }

    gpio_init_callback(&chg_cb, chg_handler, BIT(CHG_PIN_NUM));
    ret = gpio_add_callback(chg_dev, &chg_cb);
    if (ret) { LOG_ERR("CHG add cb failed: %d", ret); return ret; }

    /* Start maintenance thread (charging-only suppression). */
    k_tid_t tid = k_thread_create(&chg_maint_thread,
                                  chg_maint_stack, K_THREAD_STACK_SIZEOF(chg_maint_stack),
                                  (k_thread_entry_t)charging_maint_task,
                                  NULL, NULL, NULL,
                                  K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(tid, "chg_maint");

    LOG_INF("Charge indicator init: pin=%d, charging=%d, tid=%p", CHG_PIN_NUM, charging_init, tid);
    return 0;
}

/* Run after widgets to make suppression predictable. */
SYS_INIT(charge_indicator_init, APPLICATION, 70);
