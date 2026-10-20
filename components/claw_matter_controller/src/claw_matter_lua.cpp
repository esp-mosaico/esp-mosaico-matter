/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_matter_private.h"

#include "app_lua_modules.h"
#include "cJSON.h"
extern "C" {
#include "cap_lua.h"
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdint>
#include <cstring>

namespace claw_matter
{
namespace
{

uint32_t check_u32(lua_State *lua, int argument, const char *label)
{
    const lua_Integer value = luaL_checkinteger(lua, argument);
    if (value <= 0 || static_cast<uint64_t>(value) > UINT32_MAX) {
        luaL_argerror(lua, argument, label);
    }
    return static_cast<uint32_t>(value);
}

uint16_t check_endpoint(lua_State *lua, int argument)
{
    const lua_Integer value = luaL_checkinteger(lua, argument);
    if (value <= 0 || value > UINT16_MAX) {
        luaL_argerror(lua, argument, "endpoint must be a nonzero uint16");
    }
    return static_cast<uint16_t>(value);
}

const char *check_key(lua_State *lua, int argument, size_t capacity,
                      const char *message)
{
    size_t length = 0;
    const char *value = luaL_checklstring(lua, argument, &length);
    if (length == 0 || length >= capacity || strlen(value) != length) {
        luaL_argerror(lua, argument, message);
    }
    return value;
}

int push_json(lua_State *lua, esp_err_t error, char *json)
{
    if (error != ESP_OK) {
        cJSON_free(json);
        return luaL_error(lua, "%s", esp_err_to_name(error));
    }
    lua_pushstring(lua, json);
    cJSON_free(json);
    return 1;
}

int list_devices(lua_State *lua)
{
    luaL_checktype(lua, 1, LUA_TNONE);
    char *json = nullptr;
    const esp_err_t error = claw_matter_list_devices_json(&json);
    return push_json(lua, error, json);
}

int get_device(lua_State *lua)
{
    const auto device_id = check_u32(lua, 1, "invalid device ID");
    lua_settop(lua, 1);
    char *json = nullptr;
    const esp_err_t error = claw_matter_get_device_json(device_id, &json);
    return push_json(lua, error, json);
}

int get_property(lua_State *lua)
{
    const auto device_id = check_u32(lua, 1, "invalid device ID");
    const uint16_t endpoint_id = check_endpoint(lua, 2);
    const char *cluster =
        check_key(lua, 3, FABRIC_MANAGER_CLUSTER_KEY_MAX, "invalid cluster key");
    const char *property =
        check_key(lua, 4, FABRIC_MANAGER_MEMBER_KEY_MAX, "invalid property key");
    lua_settop(lua, 4);
    char *json = nullptr;
    const esp_err_t error = claw_matter_get_property_json(
        device_id, endpoint_id, cluster, property, &json);
    return push_json(lua, error, json);
}

int invoke_action(lua_State *lua)
{
    const auto device_id = check_u32(lua, 1, "invalid device ID");
    const uint16_t endpoint_id = check_endpoint(lua, 2);
    const char *cluster =
        check_key(lua, 3, FABRIC_MANAGER_CLUSTER_KEY_MAX, "invalid cluster key");
    const char *action =
        check_key(lua, 4, FABRIC_MANAGER_MEMBER_KEY_MAX, "invalid action key");
    const char *arguments_json = "{}";
    if (!lua_isnoneornil(lua, 5)) {
        size_t length = 0;
        arguments_json = luaL_checklstring(lua, 5, &length);
        if (length == 0 || strlen(arguments_json) != length) {
            return luaL_argerror(lua, 5,
                                 "arguments must be JSON without NUL bytes");
        }
    }
    lua_settop(lua, 5);

    fabric_core_request_id_t request_id = 0;
    const esp_err_t error = claw_matter_invoke_action(
        device_id, endpoint_id, cluster, action, arguments_json, &request_id);
    if (error != ESP_OK)
        return luaL_error(lua, "%s", esp_err_to_name(error));
    lua_newtable(lua);
    lua_pushinteger(lua, request_id);
    lua_setfield(lua, -2, "request_id");
    lua_pushliteral(lua, "submitted");
    lua_setfield(lua, -2, "status");
    return 1;
}

int open_module(lua_State *lua)
{
    static const luaL_Reg functions[] = {
        {"list_devices", list_devices},
        {"get_device", get_device},
        {"get_property", get_property},
        {"invoke_action", invoke_action},
        {nullptr, nullptr},
    };
    lua_newtable(lua);
    luaL_setfuncs(lua, functions, 0);
    return 1;
}

esp_err_t register_module(const char *)
{
    return cap_lua_register_module("matter", open_module);
}

} // namespace
} // namespace claw_matter

esp_err_t claw_matter_lua_register_module(void)
{
    static bool registered;
    if (registered)
        return ESP_OK;
    const app_lua_module_external_t module = {
        .module_id = "matter",
        .display_name = "Matter Devices",
        .reg = claw_matter::register_module,
    };
    const esp_err_t error = app_lua_modules_register_external(&module);
    if (error == ESP_OK)
        registered = true;
    return error;
}
