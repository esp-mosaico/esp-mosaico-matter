/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_gsp.h"
#include "fabric_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void matter_commissioning_init(fabric_core_handle_t core);
void matter_commissioning_reset(void);
void matter_commissioning_start_scan(esp_gsp_handle_t ui);
void matter_commissioning_stop_scan(esp_gsp_handle_t ui);
void matter_commissioning_begin(esp_gsp_handle_t ui, const char *code);
bool matter_commissioning_take_scanned_code(char *code, size_t capacity);
bool matter_commissioning_process_events(esp_gsp_handle_t ui);

#ifdef __cplusplus
}
#endif
