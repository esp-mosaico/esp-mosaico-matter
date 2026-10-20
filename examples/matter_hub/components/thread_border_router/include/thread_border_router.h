/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THREAD_BR_UNAVAILABLE, THREAD_BR_STOPPED, THREAD_BR_STARTING,
    THREAD_BR_RUNNING, THREAD_BR_STOPPING, THREAD_BR_ERROR,
} thread_border_router_state_t;

typedef struct {
    thread_border_router_state_t state;
    bool board_present;
    bool enabled;
    bool backbone_connected;
    bool commissioning_ready;
    uint8_t channel;
    char network_name[17];
    char role[12];
    esp_err_t last_error;
} thread_border_router_status_t;

/** Initialize after Wi-Fi and NVS. Auto-start on right-slot EEPROM detection
 * unless explicitly stopped. Stock OpenThread owns fatal RCP failure handling. */
esp_err_t thread_border_router_init(void);
/** Queue the user's durable preference. Stop immediately revokes readiness. */
esp_err_t thread_border_router_set_enabled(bool enabled);
void thread_border_router_get_status(thread_border_router_status_t *status);
/** Copy credentials; never returns a pointer into the OpenThread instance. */
esp_err_t thread_border_router_get_dataset(uint8_t *buffer, size_t capacity,
                                           size_t *length, void *ctx);

#ifdef __cplusplus
}
#endif
