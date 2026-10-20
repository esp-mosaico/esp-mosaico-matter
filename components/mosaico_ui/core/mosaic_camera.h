/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gsp.h"

typedef struct {
    uint16_t canvas_bind;
    uint16_t missing_visible_bind;
} mosaic_camera_view_t;

/** The frame storage is borrowed and valid only for the callback duration. */
typedef void (*mosaic_camera_frame_cb_t)(const uint8_t *pixels,
                                         size_t size,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t pixel_format,
                                         void *user_ctx);

esp_err_t mosaic_camera_start(esp_gsp_handle_t ui,
                              const mosaic_camera_view_t *view);
esp_err_t mosaic_camera_set_frame_callback(mosaic_camera_frame_cb_t callback,
                                           void *user_ctx);
esp_err_t mosaic_camera_capture_photo(const char *path, bool use_flash);
esp_err_t mosaic_camera_set_flash_enabled(bool enabled);
esp_err_t mosaic_camera_toggle_flip(bool *out_enabled);
void mosaic_camera_tick(esp_gsp_handle_t ui);
void mosaic_camera_stop(esp_gsp_handle_t ui);
