/*
 * SPDX-License-Identifier: MIT
 */

/*
 * QMK の Bootmagic と同じように、決めたキーを押したまま USB ケーブルを挿すと、
 * その半分をブートローダ (UF2) で再起動する。
 *
 * キーマップの &bootloader はセントラルが解釈してペリフェラルに指示するため、
 * 左手側 (ペリフェラル) を切り替えるには右手側の電源と接続が要る。ここではキーの押下と
 * USB 給電の検出をその半分の中だけで判定するので、もう片側が無くても切り替えられる。
 *
 * キーは overlay の row / column (その半分の kscan 上の位置) で指定する。キー位置は、
 * 選択中の物理レイアウトの transform でそのつど求める (col-offset も反映される)。
 *
 * 発動するのは次のどちらか:
 *   1. USB 給電が無し → 有りになったとき、キーが押されている (電池で動いているときに挿した場合)
 *   2. 起動直後、USB 給電を検出した少し後にキーが押された (電源 OFF の状態で挿して起動した場合。
 *      起動時は、キーの押下と USB の検出のどちらが先に届くか決まらない)
 *
 * 押したキーはふだんどおりセントラル (セントラルなら PC) にも送られる。再起動の前に、
 * キーを離したことにしてから再起動する。そうしないと、切断にセントラルが気付くまでの
 * 約 4 秒間、PC でキーリピートが続く。
 */

#define DT_DRV_COMPAT zmk_usb_bootmagic

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/reset.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/physical_layouts.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Only one zmk,usb-bootmagic node is supported");

#define BOOTMAGIC_ROW DT_INST_PROP(0, row)
#define BOOTMAGIC_COLUMN DT_INST_PROP(0, column)

// 起動時: USB 給電を検出してから、この時間までにキーが押されたら発動する
#define BOOT_KEY_WINDOW_MS 500
// 起動時の判定は、起動からこの時間までに限る (挿してすぐキーを打っただけでは発動しない)
#define BOOT_PERIOD_MS 3000
// これより長く押したままのキーでは発動しない (離したイベントを取りこぼして押下が残った場合の保険)
#define MAX_HOLD_MS 30000
// キーを離したことをセントラル / PC に伝えてから再起動するまでの時間
#define REBOOT_DELAY_MS 200

static bool key_held;
static int64_t key_pressed_at;

static bool usb_powered;
static int64_t usb_powered_at;

static bool triggered;
static uint32_t triggered_position;

// キー位置。キーが選択中の物理レイアウトに無いときなどは負の値
static int32_t bootmagic_position(void) {
    struct zmk_physical_layout const *const *layouts;
    size_t len = zmk_physical_layouts_get_list(&layouts);
    int selected = zmk_physical_layouts_get_selected();

    if (selected < 0 || (size_t)selected >= len) {
        return -ENODEV;
    }

    return zmk_matrix_transform_row_column_to_position(layouts[selected]->matrix_transform,
                                                       BOOTMAGIC_ROW, BOOTMAGIC_COLUMN);
}

static void reboot_to_bootloader(struct k_work *work) {
    // &bootloader (behavior_reset.c) と同じ
    sys_reboot(RST_UF2);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_to_bootloader);

static void release_key(struct k_work *work) {
    raise_zmk_position_state_changed(
        (struct zmk_position_state_changed){.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
                                            .state = false,
                                            .position = triggered_position,
                                            .timestamp = k_uptime_get()});

    k_work_schedule(&reboot_work, K_MSEC(REBOOT_DELAY_MS));
}

// リスナーの中で離したイベントを出すと、押したイベントより先に他のリスナーへ届いてしまうので、
// ワークアイテムで出す
static K_WORK_DEFINE(release_work, release_key);

static void trigger(uint32_t position) {
    if (triggered) {
        return;
    }

    LOG_INF("USB bootmagic: rebooting into the bootloader");
    triggered = true;
    triggered_position = position;
    k_work_submit(&release_work);
}

static void on_position_state_changed(const struct zmk_position_state_changed *ev) {
    // もう片側から届いたキーは見ない
    if (ev->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        return;
    }

    int32_t position = bootmagic_position();
    if (position < 0 || ev->position != (uint32_t)position) {
        return;
    }

    key_held = ev->state;
    if (!key_held) {
        return;
    }

    int64_t now = k_uptime_get();
    key_pressed_at = now;

    // 起動時に USB の検出がキーの押下より先に届いた場合
    if (usb_powered && now < BOOT_PERIOD_MS && now - usb_powered_at <= BOOT_KEY_WINDOW_MS) {
        trigger(position);
    }
}

static void on_usb_conn_state_changed(const struct zmk_usb_conn_state_changed *ev) {
    // 通知はまとめられることがあり、NONE → HID が直接来たり、同じ状態が続けて来たりする。
    // 給電の有無が変わったときだけを見る (POWERED と HID の間の変化は無視する)
    bool powered = ev->conn_state != ZMK_USB_CONN_NONE;
    if (powered == usb_powered) {
        return;
    }

    usb_powered = powered;
    if (!powered) {
        return;
    }

    int64_t now = k_uptime_get();
    usb_powered_at = now;

    if (key_held && now - key_pressed_at <= MAX_HOLD_MS) {
        int32_t position = bootmagic_position();
        if (position >= 0) {
            trigger(position);
        }
    }
}

static int usb_bootmagic_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev != NULL) {
        on_position_state_changed(pos_ev);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_usb_conn_state_changed *usb_ev = as_zmk_usb_conn_state_changed(eh);
    if (usb_ev != NULL) {
        on_usb_conn_state_changed(usb_ev);
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (as_zmk_physical_layout_selection_changed(eh) != NULL) {
        // キー位置が変わるので、押していた状態は分からなくなる
        key_held = false;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_bootmagic, usb_bootmagic_listener);
ZMK_SUBSCRIPTION(usb_bootmagic, zmk_position_state_changed);
ZMK_SUBSCRIPTION(usb_bootmagic, zmk_usb_conn_state_changed);
ZMK_SUBSCRIPTION(usb_bootmagic, zmk_physical_layout_selection_changed);
