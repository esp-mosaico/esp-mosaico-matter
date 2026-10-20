/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gsp.h"
#include "qrcodegen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rasterizes `payload` as a QR code into a `size` x `size` RGB565 canvas
 * (white background, black modules, centered with `quiet_modules` of margin
 * on each side) and pushes it via esp_gsp_canvas_push under `canvas_bind`.
 *
 * *pixels_cache is the canvas pixel buffer, owned by the caller's static
 * storage and initialized to NULL before the first call; it is allocated
 * once here and reused on every later call (the buffer size depends only on
 * `size`, which callers pass as a compile-time constant). qrcodegen's own
 * scratch buffers are heap-allocated and freed within this call -- too large
 * for a small task's stack, per a stack-overflow crash the retired
 * esp-mosaico-matter-bridge repo's apps/matter/matter_app.c hit and worked
 * around the same way.
 *
 * Returns false (canvas left unpushed) on an empty payload, an allocation
 * failure, or a qrcodegen encode failure. */
bool mosaic_qr_render_to_canvas(esp_gsp_handle_t ui, uint16_t canvas_bind, const char *payload,
    enum qrcodegen_Ecc ecc, int quiet_modules, int size, uint16_t **pixels_cache);

#ifdef __cplusplus
}
#endif
