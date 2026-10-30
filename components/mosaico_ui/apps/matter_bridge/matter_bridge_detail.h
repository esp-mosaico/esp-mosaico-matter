/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "bridge_dev_mngr.h"
#include "esp_gsp.h"

const char *matter_bridge_device_type(const bridge_dev_mngr_device_info_t *device);
bool matter_bridge_device_is_camera(const bridge_dev_mngr_device_info_t *device);
bool matter_bridge_device_has_controls(const bridge_dev_mngr_device_info_t *device);
void matter_bridge_detail_init(bridge_dev_mngr_handle_t manager);
/* False asks the caller to retry before changing pages or backing rows. */
bool matter_bridge_detail_close(esp_gsp_handle_t ui);
bool matter_bridge_detail_open(esp_gsp_handle_t ui,
                                const bridge_dev_mngr_device_info_t *device);
/* False means the selected endpoint has disappeared. */
bool matter_bridge_detail_tick(esp_gsp_handle_t ui);
bool matter_bridge_detail_back(void);
void matter_bridge_detail_open_entry(size_t index);
void matter_bridge_detail_control(uint16_t action, int32_t argument);
void matter_bridge_detail_pointer(bool pressed);
