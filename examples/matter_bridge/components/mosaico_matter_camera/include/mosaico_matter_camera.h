/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
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

#ifdef __cplusplus
}
#endif
