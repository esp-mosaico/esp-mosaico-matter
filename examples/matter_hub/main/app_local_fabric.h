/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "fabric_core.h"
#include "fabric_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Starts Local Fabric Core, Manager, and the CLAW semantic-device adapter. */
esp_err_t app_local_fabric_start(void);

/** Returns the started Core handle, or NULL when Local Fabric is unavailable. */
fabric_core_handle_t app_local_fabric_core(void);

/** Returns the started Manager handle, or NULL when Local Fabric is unavailable. */
fabric_manager_handle_t app_local_fabric_manager(void);

#ifdef __cplusplus
}
#endif
