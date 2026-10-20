/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_matter_bridge_private.h"
#include "claw_matter_bridge.h"
extern "C" {
#include "lauxlib.h"
}
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace claw_matter_bridge
{

#define ARRAY_METATABLE "matter_bridge.array"
static char s_null;

static cJSON *from_lua(lua_State *L, int index, unsigned depth)
{
    if (depth > 16) return NULL;
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, index));
    case LUA_TSTRING: {
        size_t size;
        const char *text = lua_tolstring(L, index, &size);
        return memchr(text, 0, size) ? NULL : cJSON_CreateString(text);
    }
    case LUA_TNUMBER: {
        double value = lua_isinteger(L, index) ? (double)lua_tointeger(L, index) : lua_tonumber(L, index);
        return isfinite(value) ? cJSON_CreateNumber(value) : NULL;
    }
    case LUA_TTABLE: {
        size_t length = lua_rawlen(L, index), count = 0;
        bool array = length != 0;
        if (lua_getmetatable(L, index)) {
            luaL_getmetatable(L, ARRAY_METATABLE);
            array = array || lua_rawequal(L, -1, -2);
            lua_pop(L, 2);
        }
        cJSON *json = array ? cJSON_CreateArray() : cJSON_CreateObject();
        if (!json) return NULL;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            bool valid = array ? lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 &&
                (lua_Unsigned)lua_tointeger(L, -2) <= length : lua_type(L, -2) == LUA_TSTRING;
            if (!valid) { lua_pop(L, 2); cJSON_Delete(json); return NULL; }
            count++;
            if (!array) {
                size_t key_size;
                const char *key = lua_tolstring(L, -2, &key_size);
                cJSON *child = memchr(key, 0, key_size) ? NULL : from_lua(L, -1, depth + 1);
                if (!child || !cJSON_AddItemToObject(json, key, child)) {
                    cJSON_Delete(child); lua_pop(L, 2); cJSON_Delete(json); return NULL;
                }
            }
            lua_pop(L, 1);
        }
        if (array) {
            if (count != length) { cJSON_Delete(json); return NULL; }
            for (size_t i = 1; i <= length; i++) {
                lua_rawgeti(L, index, i);
                cJSON *child = from_lua(L, -1, depth + 1);
                lua_pop(L, 1);
                if (!child || !cJSON_AddItemToArray(json, child)) {
                    cJSON_Delete(child); cJSON_Delete(json); return NULL;
                }
            }
        }
        return json;
    }
    case LUA_TLIGHTUSERDATA:
        return lua_touserdata(L, index) == &s_null ? cJSON_CreateNull() : NULL;
    default: return NULL;
    }
}

static void push_json(lua_State *L, const cJSON *json)
{
    if (cJSON_IsNull(json)) lua_pushlightuserdata(L, &s_null);
    else if (cJSON_IsBool(json)) lua_pushboolean(L, cJSON_IsTrue(json));
    else if (cJSON_IsString(json)) lua_pushstring(L, json->valuestring);
    else if (cJSON_IsNumber(json)) {
        double number = json->valuedouble;
        if (floor(number) == number && number >= LUA_MININTEGER && number <= LUA_MAXINTEGER)
            lua_pushinteger(L, (lua_Integer)number);
        else if (floor(number) == number) {
            char *text = cJSON_PrintUnformatted(json);
            lua_pushstring(L, text); cJSON_free(text);
        } else lua_pushnumber(L, number);
    } else {
        lua_newtable(L);
        if (cJSON_IsArray(json)) {
            luaL_getmetatable(L, ARRAY_METATABLE); lua_setmetatable(L, -2);
        }
        int index = 1;
        const cJSON *child;
        cJSON_ArrayForEach(child, json) {
            push_json(L, child);
            if (cJSON_IsArray(json)) lua_rawseti(L, -2, index++);
            else lua_setfield(L, -2, child->string);
        }
    }
}

static int failure(lua_State *L, esp_err_t err)
{
    lua_pushnil(L); lua_newtable(L);
    lua_pushstring(L, esp_err_to_name(err)); lua_setfield(L, -2, "code");
    return 2;
}

static int manage(lua_State *L)
{
    const char *method = lua_tostring(L, lua_upvalueindex(1));
    if (!lua_isnoneornil(L, 1)) luaL_checktype(L, 1, LUA_TTABLE);
    if (!g_bridge || !g_bridge->runtime_started) return failure(L, ESP_ERR_INVALID_STATE);
    /* Device jobs cannot manage lifecycle: stopping their own job would wait
     * on itself, and drivers must retain their scoped hardware-only API. */
    const char *job = cap_lua_runtime_job_id(L);
    bool driver = false;
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    for (size_t i = 0; job && i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++)
        if (g_bridge->entries[i].used && !strcmp(job, g_bridge->entries[i].job)) driver = true;
    xSemaphoreGive(g_bridge->lock);
    if (driver || cap_lua_runtime_stop_requested(L)) return failure(L, ESP_ERR_INVALID_STATE);
    cJSON *args = lua_isnoneornil(L, 1) ? cJSON_CreateObject() : from_lua(L, 1, 0);
    if (!args || !cJSON_IsObject(args)) {
        cJSON_Delete(args); return luaL_error(L, "Bridge arguments must be an object with JSON-compatible values");
    }
    char *input = cJSON_PrintUnformatted(args);
    char *output = NULL;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!input) {
        cJSON_Delete(args);
        return failure(L, ESP_ERR_NO_MEM);
    }
    if (!strcmp(method, "get_info")) {
        err = claw_matter_bridge_get_info_json(cJSON_IsTrue(
            cJSON_GetObjectItemCaseSensitive(args, "open_commissioning_window")), &output);
    } else if (!strcmp(method, "list_devices")) {
        err = claw_matter_bridge_list_devices_json(&output);
    } else if (!strcmp(method, "add_device")) {
        err = claw_matter_bridge_add_device_json(input, &output);
    } else {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(args, "device_id");
        if (cJSON_IsString(id) && !strcmp(method, "remove_device")) {
            err = claw_matter_bridge_remove_device(id->valuestring);
            if (err == ESP_OK) {
                cJSON *empty = cJSON_CreateObject();
                output = cJSON_PrintUnformatted(empty);
                cJSON_Delete(empty);
                if (!output) err = ESP_ERR_NO_MEM;
            }
        } else if (cJSON_IsString(id) && !strcmp(method, "restart_device")) {
            err = claw_matter_bridge_restart_device_json(id->valuestring, &output);
        } else if (cJSON_IsString(id) && !strcmp(method, "update_driver")) {
            const cJSON *source = cJSON_GetObjectItemCaseSensitive(args, "driver_source");
            const cJSON *driver_args = cJSON_GetObjectItemCaseSensitive(args, "driver_args");
            cJSON *empty = driver_args ? NULL : cJSON_CreateObject();
            char *driver_args_json = cJSON_PrintUnformatted(driver_args ? driver_args : empty);
            cJSON_Delete(empty);
            if (cJSON_IsString(source) && driver_args_json) {
                err = claw_matter_bridge_update_driver_json(id->valuestring,
                    source->valuestring, driver_args_json, &output);
            }
            cJSON_free(driver_args_json);
        }
    }
    cJSON_Delete(args);
    cJSON_free(input);
    if (err != ESP_OK) {
        cJSON_free(output);
        return failure(L, err);
    }
    cJSON *result = cJSON_Parse(output);
    cJSON_free(output);
    if (!result) return failure(L, ESP_FAIL);
    push_json(L, result);
    cJSON_Delete(result);
    return 1;
}

static int encode_json(lua_State *L)
{
    cJSON *json = from_lua(L, 1, 0);
    if (!json) return luaL_error(L, "Unsupported JSON value or nesting exceeds 16 levels");
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return luaL_error(L, "JSON allocation failed");
    lua_pushstring(L, text); cJSON_free(text); return 1;
}

static int decode_json(lua_State *L)
{
    cJSON *json = cJSON_ParseWithOpts(luaL_checkstring(L, 1), NULL, true);
    if (!json) return luaL_error(L, "Invalid JSON");
    push_json(L, json); cJSON_Delete(json); return 1;
}

void bridge_management_lua_register(lua_State *L)
{
    luaL_newmetatable(L, ARRAY_METATABLE); lua_pop(L, 1);
    lua_pushlightuserdata(L, &s_null); lua_setfield(L, -2, "null");
    const char *methods[] = {"get_info", "list_devices", "add_device", "remove_device",
        "restart_device", "update_driver"};
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        lua_pushstring(L, methods[i]); lua_pushcclosure(L, manage, 1); lua_setfield(L, -2, methods[i]);
    }
    lua_pushcfunction(L, encode_json); lua_setfield(L, -2, "encode_json");
    lua_pushcfunction(L, decode_json); lua_setfield(L, -2, "decode_json");
}

} // namespace claw_matter_bridge
