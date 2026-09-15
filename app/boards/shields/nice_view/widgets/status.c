/*
 *
 * Copyright (c) 2025 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/wpm.h>

#include "bongo_frames.h"
#include "status.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define WPM_HISTORY_SAMPLE_PERIOD_MS 1000
#define BONGO_IDLE_TIMEOUT_MS (NICEVIEW_WPM_HISTORY_SIZE * WPM_HISTORY_SAMPLE_PERIOD_MS)
#define BONGO_IDLE_FRAME_PERIOD_MS 200
#define BONGO_TAP_HOLD_MS 500
#define BONGO_SLEEP_FRAME_PERIOD_MS 400
#define BONGO_SLEEP_TRANSITION_MS ((BONGO_SLEEP_FRAME_COUNT - 1) * BONGO_SLEEP_FRAME_PERIOD_MS)
#define BONGO_SLEEP_TRANSITION_START_MS (BONGO_IDLE_TIMEOUT_MS - BONGO_SLEEP_TRANSITION_MS)
#define BONGO_IDLE_FRAME_PERIOD K_MSEC(BONGO_IDLE_FRAME_PERIOD_MS)
#define BONGO_TAP_HOLD K_MSEC(BONGO_TAP_HOLD_MS)
#define BONGO_SLEEP_TRANSITION_START K_MSEC(BONGO_SLEEP_TRANSITION_START_MS)

BUILD_ASSERT(BONGO_SLEEP_TRANSITION_MS < BONGO_IDLE_TIMEOUT_MS);
#define BONGO_FRAME_Y (NICEVIEW_LOGICAL_HEIGHT - BONGO_FRAME_HEIGHT)
#define WPM_GRAPH_X 2
#define WPM_GRAPH_Y 60
#define WPM_GRAPH_WIDTH 64
#define WPM_GRAPH_HEIGHT 54

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool profiles_connected[NICEVIEW_PROFILE_COUNT];
    bool profiles_bonded[NICEVIEW_PROFILE_COUNT];
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

struct wpm_status_state {
    uint8_t wpm;
};

struct key_status_state {
    uint32_t press_count;
};

static void draw_profile_status(lv_obj_t *canvas, const struct status_state *state) {
    static const int profile_x[NICEVIEW_PROFILE_COUNT] = {7, 21, 34, 47, 61};
    const int profile_y = 50;
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_t selected_dsc;
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_t selected_label_dsc;

    init_arc_dsc(&arc_dsc, LVGL_FOREGROUND, 1);
    init_arc_dsc(&selected_dsc, LVGL_FOREGROUND, 7);
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&selected_label_dsc, LVGL_BACKGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);

    for (int i = 0; i < NICEVIEW_PROFILE_COUNT; i++) {
        const bool selected = i == state->active_profile_index;

        if (state->profiles_connected[i]) {
            lv_canvas_draw_arc(canvas, profile_x[i], profile_y, 5, 0, 360, &arc_dsc);
        } else if (state->profiles_bonded[i]) {
            const int segments = 6;
            const int gap = 24;
            for (int segment = 0; segment < segments; segment++) {
                lv_canvas_draw_arc(canvas, profile_x[i], profile_y, 5,
                                   360. / segments * segment + gap / 2.0,
                                   360. / segments * (segment + 1) - gap / 2.0, &arc_dsc);
            }
        }

        if (selected) {
            lv_canvas_draw_arc(canvas, profile_x[i], profile_y, 3, 0, 359, &selected_dsc);
        }

        char label[2];
        snprintf(label, sizeof(label), "%d", i + 1);
        lv_canvas_draw_text(canvas, profile_x[i] - 4, profile_y - 5, 8,
                            selected ? &selected_label_dsc : &label_dsc, label);
    }
}

static void draw_wpm_graph(lv_obj_t *canvas, const struct zmk_widget_status *widget) {
    lv_draw_rect_dsc_t rect_dsc;
    uint8_t scale = 60;

    init_rect_dsc(&rect_dsc, LVGL_FOREGROUND);

    for (uint8_t i = 0; i < widget->wpm_history_count; i++) {
        const uint8_t index =
            (widget->wpm_history_head + NICEVIEW_WPM_HISTORY_SIZE - widget->wpm_history_count + i) %
            NICEVIEW_WPM_HISTORY_SIZE;
        scale = MAX(scale, widget->wpm_history[index]);
    }

    lv_canvas_draw_rect(canvas, WPM_GRAPH_X, WPM_GRAPH_Y, 1, WPM_GRAPH_HEIGHT, &rect_dsc);
    lv_canvas_draw_rect(canvas, WPM_GRAPH_X, WPM_GRAPH_Y + WPM_GRAPH_HEIGHT - 1, WPM_GRAPH_WIDTH, 1,
                        &rect_dsc);

    for (uint8_t i = 0; i < widget->wpm_history_count; i++) {
        const uint8_t index =
            (widget->wpm_history_head + NICEVIEW_WPM_HISTORY_SIZE - widget->wpm_history_count + i) %
            NICEVIEW_WPM_HISTORY_SIZE;
        const uint8_t value = widget->wpm_history[index];
        if (value == 0) {
            continue;
        }

        const uint8_t bar_height =
            MIN(WPM_GRAPH_HEIGHT - 2, (value * (WPM_GRAPH_HEIGHT - 2) + scale - 1) / scale);
        const int x =
            WPM_GRAPH_X + 1 + (NICEVIEW_WPM_HISTORY_SIZE - widget->wpm_history_count + i) * 4;
        const int y = WPM_GRAPH_Y + WPM_GRAPH_HEIGHT - 1 - bar_height;
        lv_canvas_draw_rect(canvas, x, y, 3, bar_height, &rect_dsc);
    }
}

static void draw_bongo_bitmap(lv_obj_t *canvas, const uint8_t *bitmap) {
    for (uint32_t y = 0; y < BONGO_FRAME_HEIGHT; y++) {
        for (uint32_t x = 0; x < BONGO_FRAME_WIDTH; x++) {
            const uint8_t packed = bitmap[y * BONGO_FRAME_STRIDE_BYTES + x / 8];
            if ((packed & BIT(7 - (x % 8))) != 0) {
                lv_canvas_set_px(canvas, x, BONGO_FRAME_Y + y, LVGL_FOREGROUND);
            }
        }
    }
}

static void draw_status(struct zmk_widget_status *widget) {
    lv_obj_t *canvas = widget->logical_canvas;
    lv_draw_label_dsc_t layer_dsc;
    lv_draw_label_dsc_t status_dsc;
    lv_draw_label_dsc_t wpm_dsc;
    const uint8_t *frame;
    char wpm_text[8];
    char layer_text[12];
    const char *output_text = "";

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);

    draw_battery(canvas, &widget->state);

    init_label_dsc(&status_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    switch (widget->state.selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        output_text = LV_SYMBOL_USB;
        break;
    case ZMK_TRANSPORT_BLE:
        if (widget->state.active_profile_bonded) {
            output_text = widget->state.active_profile_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
        } else {
            output_text = LV_SYMBOL_SETTINGS;
        }
        break;
    }
    lv_canvas_draw_text(canvas, 36, 0, 31, &status_dsc, output_text);

    init_label_dsc(&layer_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    if (widget->state.layer_label != NULL && strlen(widget->state.layer_label) > 0) {
        snprintf(layer_text, sizeof(layer_text), "%.11s", widget->state.layer_label);
    } else {
        snprintf(layer_text, sizeof(layer_text), "LAYER %u", widget->state.layer_index);
    }
    lv_canvas_draw_text(canvas, 0, 16, NICEVIEW_LOGICAL_WIDTH, &layer_dsc, layer_text);

    init_label_dsc(&wpm_dsc, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    snprintf(wpm_text, sizeof(wpm_text), "%u WPM", widget->state.wpm[9]);
    lv_canvas_draw_text(canvas, 2, 34, 64, &wpm_dsc, wpm_text);
    draw_profile_status(canvas, &widget->state);
    draw_wpm_graph(canvas, widget);

    if (widget->sleeping || widget->falling_asleep) {
        frame = bongo_sleep_frames[widget->sleep_frame % BONGO_SLEEP_FRAME_COUNT];
    } else if (widget->show_tap_frame) {
        frame = bongo_tap_frames[widget->alternate_paw ? 1 : 0];
    } else {
        frame = bongo_idle_frames[widget->idle_frame % BONGO_IDLE_FRAME_COUNT];
    }
    draw_bongo_bitmap(canvas, frame);

    /* Match the stock nice!view widget's 90-degree rotation for the Corne mounting. */
    bool changed = !widget->rendered;
    for (uint32_t y = 0; y < NICEVIEW_LOGICAL_HEIGHT; y++) {
        for (uint32_t x = 0; x < NICEVIEW_LOGICAL_WIDTH; x++) {
            const uint32_t display_index =
                x * NICEVIEW_DISPLAY_WIDTH + (NICEVIEW_LOGICAL_HEIGHT - 1 - y);
            const lv_color_t pixel = widget->logical_cbuf[y * NICEVIEW_LOGICAL_WIDTH + x];
            if (!changed &&
                memcmp(&widget->display_cbuf[display_index], &pixel, sizeof(pixel)) != 0) {
                changed = true;
            }
            widget->display_cbuf[display_index] = pixel;
        }
    }

    widget->rendered = true;
    if (changed) {
        lv_obj_invalidate(widget->display_canvas);
    }
}

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
    bool changed = widget->state.battery != state.level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    changed = changed || widget->state.charging != state.usb_present;
    widget->state.charging = state.usb_present;
#endif
    widget->state.battery = state.level;
    if (changed) {
        draw_status(widget);
    }
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *event = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = event != NULL ? event->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    bool changed =
        !zmk_endpoint_instance_eq(widget->state.selected_endpoint, state->selected_endpoint) ||
        widget->state.active_profile_index != state->active_profile_index ||
        widget->state.active_profile_connected != state->active_profile_connected ||
        widget->state.active_profile_bonded != state->active_profile_bonded;

    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;
    for (int i = 0; i < NICEVIEW_PROFILE_COUNT; i++) {
        changed = changed || widget->state.profiles_connected[i] != state->profiles_connected[i] ||
                  widget->state.profiles_bonded[i] != state->profiles_bonded[i];
        widget->state.profiles_connected[i] = state->profiles_connected[i];
        widget->state.profiles_bonded[i] = state->profiles_bonded[i];
    }
    if (changed) {
        draw_status(widget);
    }
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *eh) {
    struct output_status_state state = {
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };

    for (int i = 0; i < MIN(NICEVIEW_PROFILE_COUNT, ZMK_BLE_PROFILE_COUNT); i++) {
        state.profiles_connected[i] = zmk_ble_profile_is_connected(i);
        state.profiles_bonded[i] = !zmk_ble_profile_is_open(i);
    }
    return state;
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    const bool label_changed = (widget->state.layer_label == NULL) != (state.label == NULL) ||
                               (widget->state.layer_label != NULL && state.label != NULL &&
                                strcmp(widget->state.layer_label, state.label) != 0);
    const bool changed = widget->state.layer_index != state.index || label_changed;

    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;
    if (changed) {
        draw_status(widget);
    }
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    const zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

static void restart_wpm_history_timer(void);

static void set_wpm_status(struct zmk_widget_status *widget, struct wpm_status_state state) {
    const uint8_t previous_wpm = widget->state.wpm[9];
    const bool changed = previous_wpm != state.wpm;

    widget->state.wpm[9] = state.wpm;
    if (changed) {
        draw_status(widget);
    }
    if (previous_wpm == 0 && state.wpm > 0 && widget->wpm_history_count == 0 && !widget->sleeping) {
        restart_wpm_history_timer();
    }
}

static void wpm_status_update_cb(struct wpm_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_wpm_status(widget, state); }
}

static struct wpm_status_state wpm_status_get_state(const zmk_event_t *eh) {
    return (struct wpm_status_state){.wpm = zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state, wpm_status_update_cb,
                            wpm_status_get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);

static void animation_work_cb(struct k_work *work) {
    bool keep_animating = false;
    int64_t next_delay_ms = BONGO_IDLE_FRAME_PERIOD_MS;
    const int64_t now = k_uptime_get();
    struct zmk_widget_status *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->sleeping || widget->falling_asleep) {
            continue;
        }

        if (widget->show_tap_frame && now < widget->tap_until) {
            next_delay_ms = MIN(next_delay_ms, widget->tap_until - now);
            keep_animating = true;
            continue;
        }

        widget->show_tap_frame = false;
        widget->idle_frame = (widget->idle_frame + 1) % BONGO_IDLE_FRAME_COUNT;
        draw_status(widget);
        keep_animating = true;
    }

    if (keep_animating) {
        k_work_reschedule_for_queue(zmk_display_work_q(),
                                    CONTAINER_OF(work, struct k_work_delayable, work),
                                    K_MSEC(MAX(1, next_delay_ms)));
    }
}

K_WORK_DELAYABLE_DEFINE(status_animation_work, animation_work_cb);

static bool wpm_history_has_bars(const struct zmk_widget_status *widget) {
    for (uint8_t i = 0; i < widget->wpm_history_count; i++) {
        if (widget->wpm_history[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool push_wpm_history(struct zmk_widget_status *widget, uint8_t wpm) {
    if (widget->wpm_history_count == 0 && wpm == 0) {
        return false;
    }

    bool changed = widget->wpm_history_count < NICEVIEW_WPM_HISTORY_SIZE;
    if (!changed) {
        for (uint8_t i = 0; i < NICEVIEW_WPM_HISTORY_SIZE; i++) {
            if (widget->wpm_history[i] != wpm) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        return false;
    }

    widget->wpm_history[widget->wpm_history_head] = wpm;
    widget->wpm_history_head = (widget->wpm_history_head + 1) % NICEVIEW_WPM_HISTORY_SIZE;
    widget->wpm_history_count = MIN(widget->wpm_history_count + 1, NICEVIEW_WPM_HISTORY_SIZE);

    if (!wpm_history_has_bars(widget)) {
        widget->wpm_history_head = 0;
        widget->wpm_history_count = 0;
    }
    return true;
}

static void wpm_history_work_cb(struct k_work *work) {
    bool keep_sampling = false;
    struct zmk_widget_status *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->sleeping) {
            continue;
        }

        if (push_wpm_history(widget, widget->state.wpm[9])) {
            draw_status(widget);
        }
        keep_sampling = keep_sampling || widget->state.wpm[9] > 0 || widget->wpm_history_count > 0;
    }

    if (keep_sampling) {
        k_work_reschedule_for_queue(zmk_display_work_q(),
                                    CONTAINER_OF(work, struct k_work_delayable, work),
                                    K_MSEC(WPM_HISTORY_SAMPLE_PERIOD_MS));
    }
}

K_WORK_DELAYABLE_DEFINE(status_wpm_history_work, wpm_history_work_cb);

static void restart_wpm_history_timer(void) {
    k_work_reschedule_for_queue(zmk_display_work_q(), &status_wpm_history_work,
                                K_MSEC(WPM_HISTORY_SAMPLE_PERIOD_MS));
}

static void idle_work_cb(struct k_work *work) {
    bool reschedule = false;
    int64_t next_run_at = INT64_MAX;
    const int64_t now = k_uptime_get();
    struct zmk_widget_status *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->sleeping) {
            continue;
        }

        const int64_t inactive_ms = now - widget->last_key_press_at;
        if (inactive_ms < BONGO_SLEEP_TRANSITION_START_MS) {
            next_run_at =
                MIN(next_run_at, widget->last_key_press_at + BONGO_SLEEP_TRANSITION_START_MS);
            reschedule = true;
            continue;
        }

        if (inactive_ms < BONGO_IDLE_TIMEOUT_MS) {
            const uint8_t sleep_frame =
                MIN((inactive_ms - BONGO_SLEEP_TRANSITION_START_MS) / BONGO_SLEEP_FRAME_PERIOD_MS,
                    BONGO_SLEEP_FRAME_COUNT - 2);
            const bool frame_changed =
                !widget->falling_asleep || widget->sleep_frame != sleep_frame;

            widget->falling_asleep = true;
            widget->show_tap_frame = false;
            widget->sleep_frame = sleep_frame;
            if (frame_changed) {
                draw_status(widget);
            }

            const int64_t next_frame_at = widget->last_key_press_at +
                                          BONGO_SLEEP_TRANSITION_START_MS +
                                          (sleep_frame + 1) * BONGO_SLEEP_FRAME_PERIOD_MS;
            next_run_at = MIN(next_run_at, next_frame_at);
            reschedule = true;
            continue;
        }

        widget->falling_asleep = false;
        widget->sleeping = true;
        widget->show_tap_frame = false;
        widget->sleep_frame = BONGO_SLEEP_FRAME_COUNT - 1;
        memset(widget->wpm_history, 0, sizeof(widget->wpm_history));
        widget->wpm_history_head = 0;
        widget->wpm_history_count = 0;
        draw_status(widget);
    }

    if (reschedule) {
        const int64_t next_delay_ms = MAX(1, next_run_at - k_uptime_get());
        k_work_reschedule_for_queue(zmk_display_work_q(),
                                    CONTAINER_OF(work, struct k_work_delayable, work),
                                    K_MSEC(next_delay_ms));
    }
}

K_WORK_DELAYABLE_DEFINE(status_idle_work, idle_work_cb);

static void restart_idle_timer(void) {
    k_work_reschedule_for_queue(zmk_display_work_q(), &status_idle_work,
                                BONGO_SLEEP_TRANSITION_START);
}

static void key_status_update_cb(struct key_status_state state) {
    bool handled_press = false;
    bool woke_from_sleep = false;
    const int64_t now = k_uptime_get();
    struct zmk_widget_status *widget;

    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        if (widget->key_press_count == state.press_count) {
            continue;
        }

        const uint32_t press_delta = state.press_count - widget->key_press_count;
        widget->key_press_count = state.press_count;
        widget->last_key_press_at = now;
        widget->tap_until = now + BONGO_TAP_HOLD_MS;
        handled_press = true;
        woke_from_sleep = woke_from_sleep || widget->sleeping;
        widget->falling_asleep = false;
        widget->sleeping = false;
        widget->show_tap_frame = true;
        if ((press_delta & 1U) != 0U) {
            widget->alternate_paw = !widget->alternate_paw;
        }
        draw_status(widget);
    }

    if (!handled_press) {
        return;
    }

    k_work_reschedule_for_queue(zmk_display_work_q(), &status_animation_work, BONGO_TAP_HOLD);
    if (woke_from_sleep) {
        restart_wpm_history_timer();
    }
    restart_idle_timer();
}

static struct key_status_state key_status_get_state(const zmk_event_t *eh) {
    static uint32_t press_count;
    static int64_t last_physical_timestamp = -1;
    static int64_t last_keycode_timestamp = -1;

    if (eh == NULL) {
        return (struct key_status_state){.press_count = press_count};
    }

    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(eh);
    const struct zmk_keycode_state_changed *keycode = as_zmk_keycode_state_changed(eh);

    /* A physical key normally also raises a keycode event with the same timestamp. Count it once,
     * while still accepting keycode-only input sources. */
    if (position != NULL && position->state && position->timestamp != last_keycode_timestamp) {
        press_count++;
        last_physical_timestamp = position->timestamp;
    } else if (keycode != NULL && keycode->state && keycode->timestamp != last_physical_timestamp) {
        press_count++;
        last_keycode_timestamp = keycode->timestamp;
    }

    return (struct key_status_state){.press_count = press_count};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_key_status, struct key_status_state, key_status_update_cb,
                            key_status_get_state)
ZMK_SUBSCRIPTION(widget_key_status, zmk_position_state_changed);
ZMK_SUBSCRIPTION(widget_key_status, zmk_keycode_state_changed);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, NICEVIEW_DISPLAY_WIDTH, NICEVIEW_DISPLAY_HEIGHT);

    widget->logical_canvas = lv_canvas_create(widget->obj);
    lv_canvas_set_buffer(widget->logical_canvas, widget->logical_cbuf, NICEVIEW_LOGICAL_WIDTH,
                         NICEVIEW_LOGICAL_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(widget->logical_canvas, LV_OBJ_FLAG_HIDDEN);

    widget->display_canvas = lv_canvas_create(widget->obj);
    lv_obj_align(widget->display_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(widget->display_canvas, widget->display_cbuf, NICEVIEW_DISPLAY_WIDTH,
                         NICEVIEW_DISPLAY_HEIGHT, LV_IMG_CF_TRUE_COLOR);

    widget->last_key_press_at = k_uptime_get();
    sys_slist_append(&widgets, &widget->node);
    draw_status(widget);
    widget_battery_status_init();
    widget_output_status_init();
    widget_layer_status_init();
    widget_wpm_status_init();
    widget_key_status_init();
    k_work_reschedule_for_queue(zmk_display_work_q(), &status_animation_work,
                                BONGO_IDLE_FRAME_PERIOD);
    restart_wpm_history_timer();
    restart_idle_timer();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
