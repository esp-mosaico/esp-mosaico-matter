/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "interact_lua.h"
#include "cap_lua.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lauxlib.h"

/* Set by interact_accessory.c whenever an interact module is opened or
 * closed on a slot. One entry per slot (LEFT/RIGHT) so both can be driven
 * concurrently. Guarded by its own lock (not g_bridge's) since it is
 * unrelated to claw_matter_bridge's own device bookkeeping. */
static led_strip_handle_t s_handle[MOSAICO_MODULE_MGR_SLOT_COUNT];
/* Created once by mosaico_interact_lua_register(), which the accessory's
 * register_lua hook (via mosaico_hotplug_bridge_register_lua()) guarantees
 * runs before app_claw_start() -- i.e. before the hot-plug task or any Lua
 * driver job can call led_set() or mosaico_interact_lua_set_handle(). No
 * lazy-init race is possible. */
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;

void mosaico_interact_lua_set_handle(mosaico_module_mgr_slot_t slot, led_strip_handle_t handle)
{
    if (slot >= MOSAICO_MODULE_MGR_SLOT_COUNT || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_handle[slot] = handle;
    xSemaphoreGive(s_lock);
}

static int led_set(lua_State *L)
{
    lua_Integer slot = luaL_checkinteger(L, 1);
    lua_Integer index = luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    bool on = lua_toboolean(L, 3);
    if (slot < 0 || slot >= MOSAICO_MODULE_MGR_SLOT_COUNT)
        return luaL_error(L, "slot out of range");
    if (index < 0 || index >= MOSAICO_INTERACT_LED_COUNT)
        return luaL_error(L, "LED index out of range");
    if (!s_lock) return luaL_error(L, "Interact module binding not ready");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    led_strip_handle_t handle = s_handle[slot];
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (handle) {
        /* White at the full interaction driver's default brightness. */
        uint8_t level = on ? 32 : 0;
        err = led_strip_set_pixel(handle, (uint32_t)index, level, level, level);
        if (err == ESP_OK) err = led_strip_refresh(handle);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return luaL_error(L, "interact led_set: %s", esp_err_to_name(err));
    return 0;
}

static int open_module(lua_State *L)
{
    const luaL_Reg functions[] = {{"led_set", led_set}, {NULL, NULL}};
    luaL_newlib(L, functions);
    return 1;
}

esp_err_t mosaico_interact_lua_register(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    return cap_lua_register_module("mosaico_interact", open_module);
}
