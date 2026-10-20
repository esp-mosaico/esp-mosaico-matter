/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers Lua bindings for every hot-plug accessory. Call once before
 * app_claw_start, alongside claw_matter_bridge_lua_register_module. */
esp_err_t mosaico_hotplug_bridge_register_lua(void);

/* Starts esp-mosaico module-slot hot-plug detection. On a validated PRESENT
 * event for a registered accessory board type, that accessory creates the
 * corresponding device(s) via claw_matter_bridge_add_virtual_device_json();
 * on ABSENT, it removes them. Call after claw_matter_bridge_start()
 * has returned. Chat/skill add_device rejects virtual:true so this C-only
 * method is required. To add a module type, see accessories/ and README.md. */
esp_err_t mosaico_hotplug_bridge_start(void);

#ifdef __cplusplus
}
#endif
