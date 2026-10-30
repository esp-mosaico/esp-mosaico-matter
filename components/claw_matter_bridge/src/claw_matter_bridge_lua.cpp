/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_matter_bridge_private.h"
#include "claw_matter_bridge.h"
#include <cmath>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "app_lua_modules.h"
extern "C" {
#include "lauxlib.h"
}

namespace claw_matter_bridge
{

#define DEVICE_METATABLE "matter_bridge.device"
struct lua_device_t {
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    uint32_t generation, seen_version;
};

static bridge_entry_t *locked_entry(lua_State *L, lua_device_t **ud)
{
    *ud = static_cast<lua_device_t *>(luaL_checkudata(L, 1, DEVICE_METATABLE));
    const char *job = cap_lua_runtime_job_id(L);
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    bridge_entry_t *e = bridge_find((*ud)->id);
    if (!e || !job || strcmp(e->job, job) || e->generation != (*ud)->generation ||
        !e->access || e->removing || cap_lua_runtime_stop_requested(L)) {
        xSemaphoreGive(g_bridge->lock);
        luaL_error(L, "Matter device handle stopped or revoked");
        return NULL;
    }
    return e;
}
static int attach(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    lua_Integer generation = luaL_checkinteger(L, 2);
    const char *job = cap_lua_runtime_job_id(L);
    if (!job || strlen(id) != 32 || generation < 0 || (uint64_t)generation > UINT32_MAX)
        return luaL_error(L, "attach requires a bridge-owned background job");
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    bridge_entry_t *e = bridge_find(id);
    bool allowed = e && e->access && !e->removing && !strcmp(e->job, job) && e->generation == (uint32_t)generation;
    xSemaphoreGive(g_bridge->lock);
    if (!allowed) return luaL_error(L, "Device/job/generation mismatch");
    auto *d = static_cast<lua_device_t *>(lua_newuserdatauv(L, sizeof(lua_device_t), 0));
    snprintf(d->id, sizeof(d->id), "%s", id);
    d->generation = generation; d->seen_version = 0;
    luaL_setmetatable(L, DEVICE_METATABLE);
    return 1;
}
static void push_state(lua_State *L, const bridge_state_t *s)
{
    lua_newtable(L);
    lua_pushboolean(L, s->on); lua_setfield(L, -2, "on");
    lua_pushboolean(L, s->sensor); lua_setfield(L, -2, "sensor");
#define NUMBER(field) lua_pushinteger(L, s->field); lua_setfield(L, -2, #field)
    NUMBER(level); NUMBER(hue); NUMBER(saturation); NUMBER(color_mode);
    NUMBER(x); NUMBER(y); NUMBER(temperature);
#undef NUMBER
}
static int state(lua_State *L)
{
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    bridge_state_t copy = e->desired;
    bool changed = e->version != ud->seen_version;
    ud->seen_version = e->version;
    e->heartbeat_us = esp_timer_get_time();
    xSemaphoreGive(g_bridge->lock);
    push_state(L, &copy);
    lua_pushboolean(L, changed);
    return 2;
}
static int ready(lua_State *L)
{
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    bridge_dev_mngr_device_update_t update = {};
    update.fields = BRIDGE_DEV_MNGR_UPDATE_REACHABLE;
    update.reachable = true;
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    snprintf(id, sizeof(id), "%s", e->model.device.id);
    xSemaphoreGive(g_bridge->lock);
    esp_err_t err = bridge_dev_mngr_update(id, &update);
    if (err != ESP_OK) return luaL_error(L, "Matter ready: %s", esp_err_to_name(err));
    e = locked_entry(L, &ud);
    e->ready = true; e->heartbeat_us = esp_timer_get_time();
    xSemaphoreGive(g_bridge->lock);
    return 0;
}
static int report(lua_State *L)
{
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    const bool value = lua_toboolean(L, 2);
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    const bool sensor = e->model.type == BRIDGE_DEVICE_CONTACT_SENSOR ||
                        e->model.type == BRIDGE_DEVICE_OCCUPANCY_SENSOR;
    const bool changed = !e->report_initialized || (sensor ? e->desired.sensor != value : e->desired.on != value);
    bridge_dev_mngr_attribute_path_t path = {};
    path.cluster_id = static_cast<uint32_t>(sensor
        ? (e->model.type == BRIDGE_DEVICE_OCCUPANCY_SENSOR ? 0x0406 : 0x0045)
        : 0x0006);
    path.attribute_id = 0;
    bridge_dev_mngr_value_t reported = {};
    reported.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
    reported.data.boolean = value;
    char id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    snprintf(id, sizeof(id), "%s", e->model.device.id);
    xSemaphoreGive(g_bridge->lock);
    if (!changed) return 0;
    esp_err_t err = bridge_dev_mngr_report(id, &path, &reported);
    if (err != ESP_OK) return luaL_error(L, "Matter report: %s", esp_err_to_name(err));
    e = locked_entry(L, &ud);
    e->report_initialized = true;
    if (sensor) e->desired.sensor = value;
    else e->desired.on = value;
    e->version++;
    xSemaphoreGive(g_bridge->lock);
    return 0;
}
static int pin_index(bridge_entry_t *e, const char *role, bridge_pin_mode_t mode)
{
    for (size_t i = 0; i < e->model.pin_count; i++)
        if (e->model.pins[i].mode == mode && !strcmp(e->model.pins[i].role, role)) return i;
    return -1;
}
static int read_pin(lua_State *L)
{
    const char *role = luaL_checkstring(L, 2);
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    int index = pin_index(e, role, BRIDGE_INPUT);
    bool value = index >= 0 &&
                 (gpio_get_level(static_cast<gpio_num_t>(e->model.pins[index].gpio)) !=
                  e->model.pins[index].active_low);
    xSemaphoreGive(g_bridge->lock);
    if (index < 0) return luaL_error(L, "Unknown input role");
    lua_pushboolean(L, value); return 1;
}
static int write_pin(lua_State *L)
{
    const char *role = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    bool value = lua_toboolean(L, 3);
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    int index = pin_index(e, role, BRIDGE_OUTPUT);
    esp_err_t err = index < 0 ? ESP_ERR_INVALID_ARG : gpio_set_level(
        static_cast<gpio_num_t>(e->model.pins[index].gpio), value != e->model.pins[index].active_low);
    xSemaphoreGive(g_bridge->lock);
    if (err != ESP_OK) return luaL_error(L, "Digital output: %s", esp_err_to_name(err));
    return 0;
}
static int pwm(lua_State *L)
{
    const char *role = luaL_checkstring(L, 2);
    lua_Number percent = luaL_checknumber(L, 3);
    if (!std::isfinite(percent) || percent < 0 || percent > 100) return luaL_error(L, "PWM duty must be 0..100 percent");
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    int index = pin_index(e, role, BRIDGE_PWM);
    int channel = index >= 0 ? e->pwm_channels[index] : -1;
    auto ledc_channel = static_cast<ledc_channel_t>(channel);
    esp_err_t err = channel < 0 ? ESP_ERR_INVALID_ARG : ledc_set_duty(
        LEDC_LOW_SPEED_MODE, ledc_channel, (uint32_t)lround(percent * 4095 / 100));
    if (err == ESP_OK) err = ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel);
    xSemaphoreGive(g_bridge->lock);
    if (err != ESP_OK) return luaL_error(L, "PWM output: %s", esp_err_to_name(err));
    return 0;
}
static int rgb(lua_State *L)
{
    const char *role = luaL_checkstring(L, 2);
    lua_Integer red = luaL_checkinteger(L, 3), green = luaL_checkinteger(L, 4), blue = luaL_checkinteger(L, 5);
    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255)
        return luaL_error(L, "RGB channels must be 0..255");
    lua_device_t *ud;
    bridge_entry_t *e = locked_entry(L, &ud);
    int index = pin_index(e, role, BRIDGE_STRIP);
    esp_err_t err = index < 0 || !e->strips[index] ? ESP_ERR_INVALID_ARG : ESP_OK;
    for (int i = 0; err == ESP_OK && i < e->model.pins[index].led_count; i++)
        err = led_strip_set_pixel(e->strips[index], i, red, green, blue);
    if (err == ESP_OK) err = led_strip_refresh(e->strips[index]);
    xSemaphoreGive(g_bridge->lock);
    if (err != ESP_OK) return luaL_error(L, "RGB strip: %s", esp_err_to_name(err));
    return 0;
}
static int sleep_ms(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0 || ms > 200) return luaL_error(L, "sleep_ms accepts 0..200 milliseconds");
    if (cap_lua_runtime_stop_requested(L)) return luaL_error(L, "Driver stopped");
    vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
    if (cap_lua_runtime_stop_requested(L)) return luaL_error(L, "Driver stopped");
    return 0;
}
static int now_ms(lua_State *L) {
    /* The embedded Lua build uses signed 32-bit integers. */
    lua_pushinteger(L, (esp_timer_get_time() / 1000) & INT32_MAX); return 1;
}
static void cleanup(lua_State *L)
{
    const char *job = cap_lua_runtime_job_id(L);
    if (!job || !g_bridge) return;
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
        bridge_entry_t *e = &g_bridge->entries[i];
        if (e->used && !strcmp(e->job, job)) {
            const char *message = lua_tostring(L, -1);
            if (message && !e->removing) snprintf(e->error, sizeof(e->error), "Lua: %.150s", message);
            bridge_hardware_close(e);
        }
    }
    xSemaphoreGive(g_bridge->lock);
}
static int open_module(lua_State *L)
{
    const luaL_Reg methods[] = {{"state", state}, {"ready", ready}, {"report", report},
        {"read", read_pin}, {"write", write_pin}, {"pwm", pwm}, {"rgb", rgb}, {NULL, NULL}};
    luaL_newmetatable(L, DEVICE_METATABLE);
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    const luaL_Reg functions[] = {{"attach", attach}, {"sleep_ms", sleep_ms}, {"now_ms", now_ms}, {NULL, NULL}};
    luaL_newlib(L, functions);
    bridge_management_lua_register(L);
    return 1;
}
esp_err_t bridge_lua_register(const char *unused)
{
    (void)unused;
    esp_err_t err = cap_lua_register_exit_cleanup(cleanup);
    return err == ESP_OK ? cap_lua_register_module("matter_bridge", open_module) : err;
}

} // namespace claw_matter_bridge

esp_err_t claw_matter_bridge_lua_register_module(void)
{
    static bool registered;
    if (registered) return ESP_OK;
    const app_lua_module_external_t module = {
        .module_id = "matter_bridge",
        .display_name = "Matter Bridge",
        .reg = claw_matter_bridge::bridge_lua_register,
    };
    const esp_err_t err = app_lua_modules_register_external(&module);
    if (err == ESP_OK) registered = true;
    return err;
}
