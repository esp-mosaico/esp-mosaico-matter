/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "bridge_dev_mngr.h"
#include "mosaico_module_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the camera hot-plug adapter to the shared device manager. */
esp_err_t mosaico_matter_camera_init(bridge_dev_mngr_handle_t dev_mngr);

/* Hot-plug hooks. They run on the dedicated Mosaico hot-plug worker. */
esp_err_t mosaico_matter_camera_present(mosaico_module_mgr_slot_t slot,
    uint16_t board_id, uint32_t serial_number);
void mosaico_matter_camera_absent(mosaico_module_mgr_slot_t slot);

/** A borrowed packed UYVY (U0 Y0 V0 Y1) frame. Copy/convert before returning.
 * Stride may exceed width*2. Runs on the capture task: never call UI APIs,
 * preview_start/stop, or any API that waits for the UI from this callback. */
typedef void (*mosaico_matter_camera_preview_cb_t)(const uint8_t *pixels,
    size_t size, uint32_t width, uint32_t height, uint32_t stride, void *ctx);

/** Subscribe to the unique Matter-owned capture pipeline. No second claim.
 * Single subscriber; returns NOT_FOUND while no camera is present. */
esp_err_t mosaico_matter_camera_preview_start(
    mosaico_matter_camera_preview_cb_t callback, void *ctx);
/** Clear the subscriber and wait for an in-flight callback to finish.
 * Must run outside the callback. Detach clears the subscription as well. */
esp_err_t mosaico_matter_camera_preview_stop(void);

/** Enable or disable on-frame face mosaic privacy processing. */
void mosaico_matter_camera_set_privacy_enabled(bool enabled);
bool mosaico_matter_camera_is_privacy_enabled(void);

/** Return true once the Matter camera capture pipeline is open and ready. */
bool mosaico_matter_camera_is_ready(void);

#ifdef __cplusplus
}
#endif
