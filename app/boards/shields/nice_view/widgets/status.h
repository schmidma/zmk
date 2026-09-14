/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>
#include <zephyr/kernel.h>

#include "util.h"

#define NICEVIEW_LOGICAL_WIDTH 68
#define NICEVIEW_LOGICAL_HEIGHT 160
#define NICEVIEW_DISPLAY_WIDTH 160
#define NICEVIEW_DISPLAY_HEIGHT 68
#define NICEVIEW_WPM_HISTORY_SIZE 16

struct zmk_widget_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *logical_canvas;
    lv_obj_t *display_canvas;
    lv_color_t logical_cbuf[NICEVIEW_LOGICAL_WIDTH * NICEVIEW_LOGICAL_HEIGHT];
    lv_color_t display_cbuf[NICEVIEW_DISPLAY_WIDTH * NICEVIEW_DISPLAY_HEIGHT];
    struct status_state state;
    uint8_t wpm_history[NICEVIEW_WPM_HISTORY_SIZE];
    uint8_t wpm_history_head;
    uint8_t wpm_history_count;
    uint8_t idle_frame;
    uint32_t key_press_count;
    int64_t last_key_press_at;
    int64_t tap_until;
    bool alternate_paw;
    bool show_tap_frame;
    bool sleeping;
};

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget);
