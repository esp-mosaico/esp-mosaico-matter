/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register matter.list_devices, matter.get_device, and matter.invoke_action. */
esp_err_t hub_matter_register_tools(esp_mcp_t *mcp, void *manager);

#ifdef __cplusplus
}
#endif
