/*
 * Frames copied from finestedm/temper-zmkstudio, branch niceview-bongo, where they were adapted
 * from SamIAm2000/zmk, branch bongo-cat-dedicated-work-queue.
 * Copyright (c) 2021 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define BONGO_FRAME_WIDTH 68
#define BONGO_FRAME_HEIGHT 36
#define BONGO_FRAME_STRIDE_BYTES ((BONGO_FRAME_WIDTH + 7) / 8)
#define BONGO_FRAME_BYTES (BONGO_FRAME_STRIDE_BYTES * BONGO_FRAME_HEIGHT)
#define BONGO_IDLE_FRAME_COUNT 5
#define BONGO_SLEEP_FRAME_COUNT 4

extern const uint8_t bongo_idle_frames[BONGO_IDLE_FRAME_COUNT][BONGO_FRAME_BYTES];
extern const uint8_t bongo_tap_frames[2][BONGO_FRAME_BYTES];
extern const uint8_t bongo_sleep_frames[BONGO_SLEEP_FRAME_COUNT][BONGO_FRAME_BYTES];
