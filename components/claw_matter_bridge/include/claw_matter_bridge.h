/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "bridge_core.h"
#include "bridge_dev_mngr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bridge_core_handle_t core;
    bridge_dev_mngr_handle_t dev_mngr;
    const char *storage_root;
    const char *runner_path;
} claw_matter_bridge_config_t;

/** Registers the `matter_bridge` Lua module with app_claw. */
esp_err_t claw_matter_bridge_lua_register_module(void);

/**
 * Starts the runtime and enables its CLAW management plane. Starting after a
 * stop with the same configuration only re-enables management.
 */
esp_err_t
claw_matter_bridge_start(const claw_matter_bridge_config_t *config);

/**
 * Disables CLAW management without touching endpoints, drivers, reachability,
 * listeners, or the supervisor.
 */
esp_err_t claw_matter_bridge_stop(void);

esp_err_t claw_matter_bridge_factory_reset(void);

/** Returned JSON uses cJSON allocation and is released with cJSON_free(). */
esp_err_t claw_matter_bridge_get_info_json(bool open_commissioning_window,
                                           char **out_json);
esp_err_t claw_matter_bridge_list_devices_json(char **out_json);
esp_err_t claw_matter_bridge_add_device_json(const char *definition_json,
                                             char **out_json);
esp_err_t claw_matter_bridge_remove_device(const char *device_id);
esp_err_t claw_matter_bridge_restart_device_json(const char *device_id,
                                                 char **out_json);
esp_err_t claw_matter_bridge_update_driver_json(const char *device_id,
                                                const char *driver_source,
                                                const char *driver_args_json,
                                                char **out_json);

#ifdef __cplusplus
}
#endif
