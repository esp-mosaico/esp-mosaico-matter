/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "cJSON.h"
#include "bridge_dev_mngr.h"

namespace claw_matter_bridge
{

#define BRIDGE_MAX_PINS 8
#define BRIDGE_MAX_SOURCE 8192
#define BRIDGE_MAX_MANIFEST 65536
#define BRIDGE_MAX_LEDS 256
#define BRIDGE_HEARTBEAT_MS 5000
#define BRIDGE_READY_MS 2000
#define BRIDGE_PWM_HZ 2000

enum bridge_pin_mode_t { BRIDGE_INPUT, BRIDGE_OUTPUT, BRIDGE_PWM, BRIDGE_STRIP };
enum bridge_device_type_t {
    BRIDGE_DEVICE_LIGHT = 0,
    BRIDGE_DEVICE_OUTLET,
    BRIDGE_DEVICE_DIMMABLE_LIGHT,
    BRIDGE_DEVICE_COLOR_LIGHT,
    BRIDGE_DEVICE_CONTACT_SENSOR,
    BRIDGE_DEVICE_OCCUPANCY_SENSOR,
};
struct bridge_pin_t {
    char role[17];
    int gpio;
    bridge_pin_mode_t mode;
    bool active_low;
    int pull; /* -1 down, 0 none, 1 up */
    int led_count;
};
struct bridge_model_t {
    bridge_dev_mngr_device_definition_t device;
    bridge_device_type_t type;
    bridge_pin_t pins[BRIDGE_MAX_PINS];
    size_t pin_count;
};

const char *bridge_type_name(bridge_device_type_t type);
bool bridge_gpio_allowed(int gpio, uint64_t allowed_mask);
esp_err_t bridge_model_parse(const cJSON *json, uint64_t allowed_mask,
    bridge_model_t *out, char *error, size_t error_size);
/* Atomic rename, fsync before rename. Errors never replace a valid old file. */
esp_err_t bridge_atomic_write(const char *path, const void *data, size_t size);
char *bridge_read_file(const char *path, size_t max_size);

} // namespace claw_matter_bridge
