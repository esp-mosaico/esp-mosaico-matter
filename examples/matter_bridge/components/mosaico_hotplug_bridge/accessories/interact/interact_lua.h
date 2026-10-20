/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_err.h"
#include "led_strip.h"
#include "mosaico_module_mgr.h"

/* The module has six WS2812 LEDs; Matter exposes the first three. */
#define MOSAICO_INTERACT_LED_COUNT 6U

#ifdef __cplusplus
extern "C" {
#endif

/* Registers require("mosaico_interact") -> { led_set(slot, index, on) }. */
esp_err_t mosaico_interact_lua_register(void);

/* Points the Lua binding at the currently-open interact module handle for
 * `slot`, or NULL when no module is attached to that slot. Thread-safe; the
 * previous handle is never closed by this call -- the caller owns the
 * handle's lifecycle. One handle is kept per slot (LEFT/RIGHT) so both slots
 * can drive an interact module concurrently. */
void mosaico_interact_lua_set_handle(mosaico_module_mgr_slot_t slot, led_strip_handle_t handle);

#ifdef __cplusplus
}
#endif
