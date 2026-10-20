/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_gsp.h"
#include "fabric_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void matter_home_init(fabric_manager_handle_t manager);
void matter_home_reset(void);
esp_err_t matter_home_refresh(esp_gsp_handle_t ui);
void matter_home_select_all(esp_gsp_handle_t ui);
void matter_home_select_room(esp_gsp_handle_t ui, size_t slot);
void matter_home_previous_rooms(esp_gsp_handle_t ui);
void matter_home_next_rooms(esp_gsp_handle_t ui);
void matter_home_previous_page(esp_gsp_handle_t ui);
void matter_home_next_page(esp_gsp_handle_t ui);
esp_err_t matter_home_device_at_slot(size_t slot,
                                     fabric_manager_device_id_t *out_device_id);

#ifdef __cplusplus
}
#endif
