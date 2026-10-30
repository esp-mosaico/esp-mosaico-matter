/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "claw_matter_bridge_model.h"
#include "claw_hw_registry.h"
extern "C" {
#include "cap_lua.h"
}
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

namespace claw_matter_bridge
{

struct bridge_state_t {
    bool on;
    uint8_t level, hue, saturation, color_mode;
    uint16_t x, y, temperature;
    bool sensor;
};

struct bridge_entry_t {
    bool used, removing, failed, ready, access;
    bool endpoint_present, quiesced;
    uint16_t endpoint_id;
    uint32_t generation, version;
    int64_t heartbeat_us;
    bridge_model_t model;
    cJSON *definition;
    char path[192], job[CAP_LUA_JOB_ID_LEN], run_name[32], owner[64], error[160];
    claw_hw_lease_handle_t lease;
    int pwm_channels[BRIDGE_MAX_PINS];
    led_strip_handle_t strips[BRIDGE_MAX_PINS];
    bool configured[BRIDGE_MAX_PINS];
    bridge_state_t desired;
    bool report_initialized;
};

struct bridge_context_t {
    bridge_core_handle_t core;
    bridge_dev_mngr_handle_t dev_mngr;
    bridge_dev_mngr_listener_id_t listener_id;
    SemaphoreHandle_t lock; /* Hardware and entry state; never held during Matter waits. */
    SemaphoreHandle_t operations; /* Serializes management, startup and supervisor. */
    bridge_entry_t entries[BRIDGE_DEV_MNGR_MAX_DEVICES];
    char root[128], manifest[160], runner[192];
    uint64_t allowed;
    uint32_t pwm_mask;
    bool runtime_started, management_enabled, dirty;
};
extern bridge_context_t *g_bridge;

esp_err_t bridge_dev_mngr_register(const bridge_dev_mngr_device_definition_t *, bridge_dev_mngr_device_info_t *);
esp_err_t bridge_dev_mngr_activate(const char *, bridge_dev_mngr_device_info_t *);
esp_err_t bridge_dev_mngr_deactivate(const char *);
esp_err_t bridge_dev_mngr_remove(const char *);
esp_err_t bridge_dev_mngr_update(const char *, const bridge_dev_mngr_device_update_t *);
esp_err_t bridge_dev_mngr_read(const char *, const bridge_dev_mngr_attribute_path_t *, bridge_dev_mngr_value_t *);
esp_err_t bridge_dev_mngr_report(const char *, const bridge_dev_mngr_attribute_path_t *, const bridge_dev_mngr_value_t *);
esp_err_t bridge_save(void); /* Caller holds operations, not hardware lock. */
esp_err_t bridge_start_driver(bridge_entry_t *);
esp_err_t bridge_stop_driver(bridge_entry_t *);
esp_err_t bridge_remove(bridge_entry_t *);
esp_err_t bridge_hardware_claim(bridge_entry_t *);
esp_err_t bridge_hardware_open(bridge_entry_t *); /* lock held */
esp_err_t bridge_hardware_close(bridge_entry_t *); /* lock held, retains pin lease */
void bridge_hardware_release(bridge_entry_t *);
esp_err_t bridge_lua_register(const char *);
void bridge_management_lua_register(lua_State *L);
esp_err_t bridge_prepare_script(bridge_entry_t *, const char *source, unsigned revision);
/* Lookup requires lock; only operations owner can add/free entries. */
bridge_entry_t *bridge_find(const char *);
void bridge_supervise_once(void);

} // namespace claw_matter_bridge
