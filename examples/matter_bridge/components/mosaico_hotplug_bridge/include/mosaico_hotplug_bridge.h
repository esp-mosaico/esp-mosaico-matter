/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "bridge_dev_mngr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts native accessory detection and polling. Call after the Matter core
 * starts and legacy Lua virtual definitions have been handed to the catalog.
 * Fixed accessories never create Lua jobs. */
esp_err_t mosaico_hotplug_bridge_start(bridge_dev_mngr_handle_t dev_mngr);

#ifdef __cplusplus
}
#endif
