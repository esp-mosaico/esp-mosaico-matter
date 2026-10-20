/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gsp.h"
#include "fabric_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

void matter_detail_init(fabric_manager_handle_t manager);
void matter_detail_close(esp_gsp_handle_t ui);
esp_err_t matter_detail_open(esp_gsp_handle_t ui,
                             fabric_manager_device_id_t device_id);
esp_err_t matter_detail_refresh(esp_gsp_handle_t ui);
esp_err_t matter_detail_open_entry(esp_gsp_handle_t ui, size_t index);
/* Returns true when Back closed an entry; false when the device view should close. */
bool matter_detail_back(esp_gsp_handle_t ui);
void matter_detail_property_changed(void);
void matter_detail_process_events(esp_gsp_handle_t ui);

void matter_detail_step_mode(esp_gsp_handle_t ui, bool second, int direction);
void matter_detail_set_target(esp_gsp_handle_t ui, int32_t percent);
void matter_detail_set_speed(esp_gsp_handle_t ui, int32_t percent);
void matter_detail_pause_resume(esp_gsp_handle_t ui);
void matter_detail_go_home(esp_gsp_handle_t ui);

void matter_detail_toggle_power(esp_gsp_handle_t ui);
void matter_detail_set_level(esp_gsp_handle_t ui, int32_t percent);
void matter_detail_set_color(esp_gsp_handle_t ui, int32_t hue_degrees,
                             int32_t saturation_percent);
void matter_detail_set_hue(esp_gsp_handle_t ui, int32_t hue_degrees);
void matter_detail_set_saturation(esp_gsp_handle_t ui,
                                  int32_t saturation_percent);
void matter_detail_set_color_temperature(esp_gsp_handle_t ui,
                                         int32_t percent);

#ifdef __cplusplus
}
#endif
