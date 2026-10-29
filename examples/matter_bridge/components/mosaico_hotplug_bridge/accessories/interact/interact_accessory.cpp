/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "interact_inputs.h"
#include "interact_light.h"
#include "interact_identity.h"
#include "interact_color.h"
#include "mosaico_hotplug_accessory.h"
#include "bridge_sensor_utils.h"

#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include "bsp/subboard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "led_strip.h"
#include "soc/soc_caps.h"

namespace {

constexpr size_t kLedCount = 3;
constexpr size_t kPir = 3;
constexpr size_t kLight = 4;
constexpr size_t kDeviceCount = 5;
constexpr uint8_t kStripLedCount = 6;
constexpr int64_t kPirIntervalMs = 100;
constexpr int64_t kLightIntervalMs = 1000;
const char *const kTag = "mosaico_interact";

const struct {
    const char *suffix;
    const char *name;
    const char *profile;
    uint32_t device_type;
} kDevices[kDeviceCount] = {
    {"led0", "Interact LED 1", BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT, 0x010d},
    {"led1", "Interact LED 2", BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT, 0x010d},
    {"led2", "Interact LED 3", BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT, 0x010d},
    {"pir", "Interact PIR", BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR, 0x0107},
    {"light", "Interact Light Sensor", BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR, 0x0106},
};

struct slot_state {
    led_strip_handle_t strip;
    mosaico_module_lease_t lease;
    bridge_dev_mngr_device_definition_t devices[kDeviceCount];
    bool retain_on_failure[kDeviceCount];
    bool reachable[kDeviceCount], reachable_known[kDeviceCount];
    bool active, leds_applied;
    mosaico_interact::color_state desired_leds[kLedCount];
    mosaico_interact::rgb applied_leds[kLedCount];
    uint32_t led_write_version[kLedCount];
    int64_t next_pir_ms, next_light_ms;
    bridge::sensor_report_t pir_report, light_report;
};

slot_state s_slots[MOSAICO_MODULE_MGR_SLOT_COUNT]{};
// Only device IDs and desired color states cross threads. Hardware and lifecycle
// belong to the hot-plug task; never wait for Matter while holding this lock.
portMUX_TYPE s_write_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t reachable(slot_state &slot, size_t index, bool value)
{
    if (slot.reachable_known[index] && slot.reachable[index] == value) return ESP_OK;
    esp_err_t err = mosaico_hotplug_set_reachable(slot.devices[index].id, value);
    if (err == ESP_OK) {
        slot.reachable_known[index] = true;
        slot.reachable[index] = value;
    } else slot.reachable_known[index] = false;
    return err;
}

esp_err_t initialize_led(slot_state &slot, size_t index)
{
    using namespace mosaico_interact;
    // Outputs restart off and white. Explicit HSV modes avoid the SDK's
    // default XY mode with zero coordinates producing a black first On.
    for (size_t attribute = 0; attribute < sizeof(color_paths) / sizeof(color_paths[0]); ++attribute) {
        const auto &path = color_paths[attribute];
        bridge_dev_mngr_value_t value{};
        value.type = path.cluster_id == 0x0006 ? BRIDGE_DEV_MNGR_VALUE_BOOL : BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
        if (value.type == BRIDGE_DEV_MNGR_VALUE_BOOL) value.data.boolean = false;
        else value.data.unsigned_value = initial_color_values[attribute];
        esp_err_t error = mosaico_hotplug_report(slot.devices[index].id, &path, &value);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "Initialize %s: report 0x%04" PRIx32 "/0x%04" PRIx32 ": %s",
                slot.devices[index].id, path.cluster_id, path.attribute_id, esp_err_to_name(error));
            return error;
        }
    }
    for (const auto &path : color_paths) {
        portENTER_CRITICAL(&s_write_lock);
        const uint32_t version = slot.led_write_version[index];
        portEXIT_CRITICAL(&s_write_lock);
        bridge_dev_mngr_value_t value{};
        esp_err_t error = mosaico_hotplug_read(slot.devices[index].id, &path, &value);
        if (error != ESP_OK) {
            ESP_LOGE(kTag, "Initialize %s: read 0x%04" PRIx32 "/0x%04" PRIx32 ": %s",
                slot.devices[index].id, path.cluster_id, path.attribute_id, esp_err_to_name(error));
            return error;
        }
        portENTER_CRITICAL(&s_write_lock);
        bool valid = true;
        // A newer command always wins over a read queued before it.
        if (slot.led_write_version[index] == version) {
            valid = apply_color_attribute(slot.desired_leds[index], path, value);
        }
        portEXIT_CRITICAL(&s_write_lock);
        if (!valid) {
            ESP_LOGE(kTag, "Initialize %s: invalid value for 0x%04" PRIx32 "/0x%04" PRIx32
                " (type=%d)", slot.devices[index].id, path.cluster_id, path.attribute_id, (int)value.type);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}

esp_err_t release_hardware(mosaico_module_mgr_slot_t index)
{
    auto &slot = s_slots[index];
    esp_err_t err = mosaico_interact_inputs_close(index);
    if (err != ESP_OK) return err;
    if (slot.strip) {
        err = led_strip_clear(slot.strip);
        if (err != ESP_OK) ESP_LOGW(kTag, "Clear LEDs: %s", esp_err_to_name(err));
        err = led_strip_del(slot.strip);
        if (err != ESP_OK) return err;
        slot.strip = nullptr;
    }
    if (slot.lease.id) {
        err = mosaico_module_mgr_release(&slot.lease);
        if (err != ESP_OK) return err;
        slot.lease = {};
    }
    return ESP_OK;
}

esp_err_t close_module(mosaico_module_mgr_slot_t index)
{
    auto &slot = s_slots[index];
    slot.active = false;
    esp_err_t result = ESP_OK;
    for (size_t i = 0; i < kDeviceCount; ++i) {
        if (!slot.devices[i].id[0]) continue;
        esp_err_t err = slot.retain_on_failure[i]
            ? mosaico_hotplug_deactivate_device(slot.devices[i].id)
            : mosaico_hotplug_remove_device(slot.devices[i].id);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            portENTER_CRITICAL(&s_write_lock);
            slot.devices[i] = {};
            portEXIT_CRITICAL(&s_write_lock);
        } else {
            (void)reachable(slot, i, false);
            ESP_LOGE(kTag, "Cleanup %s: %s", slot.devices[i].name, esp_err_to_name(err));
            result = err;
        }
    }
    esp_err_t err = release_hardware(index);
    return result != ESP_OK ? result : err;
}

esp_err_t open_hardware(mosaico_module_mgr_slot_t index)
{
    auto &slot = s_slots[index];
    const mosaico_module_mgr_claim_config_t claim = {
        .expected_type = MOSAICO_BOARD_TYPE_INTERACT, .slot = index, .timeout_ms = 1500,
    };
    esp_err_t err = mosaico_module_mgr_claim(&claim, &slot.lease);
    if (err != ESP_OK) return err;
    const led_strip_config_t strip = {
        .strip_gpio_num = bsp_subboard_map_gpio(static_cast<bsp_subboard_slot_t>(index), GPIO_NUM_15),
        .max_leds = kStripLedCount,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
    };
    err = led_strip_new_rmt_device(&strip, &rmt, &slot.strip);
    if (err == ESP_OK) err = led_strip_clear(slot.strip);
    if (err == ESP_OK) err = mosaico_interact_inputs_open(index);
    return err;
}

esp_err_t create_device(const mosaico_hotplug_event_t &event, size_t index,
    const bridge_dev_mngr_device_info_t *catalog, size_t count)
{
    auto &slot = s_slots[event.slot];
    bridge_dev_mngr_device_definition_t definition{};
    char legacy_name[BRIDGE_DEV_MNGR_NAME_MAX];
    snprintf(legacy_name, sizeof(legacy_name), "interact-%04x%08" PRIx32 "-%s",
        event.board_id, event.serial_number, kDevices[index].suffix);
    snprintf(definition.id, sizeof(definition.id), "%s", legacy_name);
    esp_err_t err = mosaico_interact::legacy_id(event.board_id, event.serial_number,
                                                index, definition.id, false);
    if (err != ESP_OK) return err;
    char resolved_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    memcpy(resolved_id, definition.id, sizeof(resolved_id));
    snprintf(definition.name, sizeof(definition.name), "%s", kDevices[index].name);
    snprintf(definition.profile_id, sizeof(definition.profile_id), "%s", kDevices[index].profile);
    definition.device_type = kDevices[index].device_type;
    bool existing = false;
    err = mosaico_interact::adopt_definition(definition, legacy_name, catalog, count, existing);
    if (err != ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            const auto &stored = catalog[i].definition;
            if (strcmp(stored.id, resolved_id) && strcmp(stored.name, legacy_name)) continue;
            ESP_LOGE(kTag, "Catalog conflict for %s: stored %s/0x%04" PRIx32
                ", requested %s/0x%04" PRIx32, stored.id, stored.profile_id,
                stored.device_type, kDevices[index].profile, kDevices[index].device_type);
            if (index < kLedCount && !strcmp(stored.profile_id, BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT)) {
                ESP_LOGE(kTag, "Factory reset required to replace saved on/off lights with color lights");
            }
        }
        return err;
    }
    bool rename = existing && mosaico_interact::needs_default_rename(definition, legacy_name);
    if (strcmp(definition.id, resolved_id)) {
        // Persist before renaming, so a restart can still find the old ID.
        err = mosaico_interact::legacy_id(event.board_id, event.serial_number,
                                         index, definition.id, true);
        if (err != ESP_OK) return err;
    }
    portENTER_CRITICAL(&s_write_lock);
    bool duplicate = false;
    for (int other = 0; other < MOSAICO_MODULE_MGR_SLOT_COUNT; ++other) {
        if (other == event.slot) continue;
        for (const auto &device : s_slots[other].devices)
            duplicate |= !strcmp(device.id, definition.id);
    }
    if (!duplicate) slot.devices[index] = definition;
    portEXIT_CRITICAL(&s_write_lock);
    if (duplicate) return ESP_ERR_INVALID_STATE;
    slot.retain_on_failure[index] = existing;
    bridge_dev_mngr_device_info_t info{};
    const char *stage = "register";
    err = mosaico_hotplug_register_device(&definition);
    if (err == ESP_OK) {
        stage = "activate";
        err = mosaico_hotplug_activate_device(definition.id, &info);
    }
    if (err == ESP_OK && rename) {
        stage = "rename";
        err = mosaico_hotplug_set_name(definition.id, kDevices[index].name);
        if (err == ESP_OK) {
            snprintf(definition.name, sizeof(definition.name), "%s", kDevices[index].name);
            portENTER_CRITICAL(&s_write_lock);
            slot.devices[index] = definition;
            portEXIT_CRITICAL(&s_write_lock);
        }
    }
    if (err == ESP_OK && index < kLedCount) {
        stage = "initialize";
        err = initialize_led(slot, index);
    }
    if (err != ESP_OK) ESP_LOGE(kTag, "%s %s failed: %s", stage, definition.id, esp_err_to_name(err));
    if (err == ESP_OK) ESP_LOGI(kTag, "Native %s on %s slot: endpoint %u (%s)",
        kDevices[index].suffix, mosaico_module_mgr_slot_to_name(event.slot), info.endpoint_id, definition.id);
    return err;
}

esp_err_t present(const mosaico_hotplug_event_t *event)
{
    mosaico_interact_inputs_init();
    esp_err_t err = close_module(event->slot);
    if (err != ESP_OK) return err;
    auto &slot = s_slots[event->slot];
    portENTER_CRITICAL(&s_write_lock);
    for (auto &led : slot.desired_leds) led = {};
    memset(slot.led_write_version, 0, sizeof(slot.led_write_version));
    portEXIT_CRITICAL(&s_write_lock);
    slot.leds_applied = false;
    slot.pir_report = {};
    slot.light_report = {};
    slot.next_pir_ms = slot.next_light_ms = 0;
    memset(slot.reachable_known, 0, sizeof(slot.reachable_known));
    err = open_hardware(event->slot);
    if (err == ESP_OK) {
        bridge_dev_mngr_device_info_t catalog[BRIDGE_DEV_MNGR_MAX_DEVICES]{};
        size_t count = 0;
        err = mosaico_hotplug_get_devices(catalog, BRIDGE_DEV_MNGR_MAX_DEVICES, &count);
        for (size_t i = 0; err == ESP_OK && i < kDeviceCount; ++i)
            err = create_device(*event, i, catalog, count);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Native setup failed on %s slot: %s; rolling back",
            mosaico_module_mgr_slot_to_name(event->slot), esp_err_to_name(err));
        (void)close_module(event->slot);
        return err;
    }
    for (auto &retain : slot.retain_on_failure) retain = false;
    slot.active = true;
    return ESP_OK;
}

void absent(const mosaico_hotplug_event_t *event)
{
    esp_err_t err = close_module(event->slot);
    if (err != ESP_OK) ESP_LOGW(kTag, "Native detach cleanup: %s", esp_err_to_name(err));
}

void attribute_changed(const bridge_dev_mngr_attribute_change_t *change)
{
    if (!change) return;
    portENTER_CRITICAL(&s_write_lock);
    for (auto &slot : s_slots) {
        for (size_t i = 0; i < kLedCount; ++i) {
            if (!strcmp(slot.devices[i].id, change->device_id) &&
                mosaico_interact::apply_color_attribute(slot.desired_leds[i], change->path, change->value)) {
                ++slot.led_write_version[i];
            }
        }
    }
    portEXIT_CRITICAL(&s_write_lock);
}

void report_sensor(slot_state &slot, size_t index, const bridge_dev_mngr_value_t &value,
    uint32_t cluster, bridge::sensor_report_t &last, uint16_t encoded)
{
    const bool is_null = value.type == BRIDGE_DEV_MNGR_VALUE_NULL;
    esp_err_t err = ESP_OK;
    if (last.changed(is_null, encoded)) {
        const bridge_dev_mngr_attribute_path_t path{cluster, 0};
        err = mosaico_hotplug_report(slot.devices[index].id, &path, &value);
        if (err == ESP_OK) last.remember(is_null, encoded);
        else ESP_LOGW(kTag, "Report %s: %s", slot.devices[index].name, esp_err_to_name(err));
    }
    (void)reachable(slot, index, err == ESP_OK);
}

void poll(mosaico_module_mgr_slot_t index, int64_t now_ms)
{
    auto &slot = s_slots[index];
    if (!slot.active) return;
    portENTER_CRITICAL(&s_write_lock);
    mosaico_interact::color_state desired[kLedCount];
    memcpy(desired, slot.desired_leds, sizeof(desired));
    portEXIT_CRITICAL(&s_write_lock);
    mosaico_interact::rgb frame[kLedCount];
    bool changed = !slot.leds_applied;
    for (size_t i = 0; i < kLedCount; ++i) {
        frame[i] = mosaico_interact::to_rgb(desired[i]);
        changed |= frame[i] != slot.applied_leds[i];
    }
    if (changed) {
        esp_err_t err = ESP_OK;
        for (size_t i = 0; err == ESP_OK && i < kLedCount; ++i) {
            err = led_strip_set_pixel(slot.strip, i, frame[i].red, frame[i].green, frame[i].blue);
        }
        if (err == ESP_OK) err = led_strip_refresh(slot.strip);
        if (err == ESP_OK) {
            memcpy(slot.applied_leds, frame, sizeof(frame));
            slot.leds_applied = true;
        }
        else slot.leds_applied = false;
    }
    for (size_t i = 0; i < kLedCount; ++i) (void)reachable(slot, i, slot.leds_applied);
    if (now_ms >= slot.next_pir_ms) {
        slot.next_pir_ms = now_ms + kPirIntervalMs;
        bool occupied = false;
        if (mosaico_interact_pir_read(index, &occupied) == ESP_OK) {
            bridge_dev_mngr_value_t value{};
            value.type = BRIDGE_DEV_MNGR_VALUE_BOOL;
            value.data.boolean = occupied;
            report_sensor(slot, kPir, value, 0x0406, slot.pir_report, occupied);
        } else (void)reachable(slot, kPir, false);
    }
    if (now_ms >= slot.next_light_ms) {
        slot.next_light_ms = now_ms + kLightIntervalMs;
        double millivolts = 0, lux = 0;
        uint16_t encoded = 0;
        bridge_dev_mngr_value_t value{};
        if (mosaico_interact_light_read(index, &millivolts) == ESP_OK &&
            mosaico_interact_light_estimate_lux(millivolts, MOSAICO_INTERACT_LIGHT_R10_OHMS,
                                               MOSAICO_INTERACT_LIGHT_GAMMA_X1000 / 1000.0, &lux) &&
            bridge::encode_illuminance(lux, encoded)) {
            value.type = BRIDGE_DEV_MNGR_VALUE_UNSIGNED;
            value.data.unsigned_value = encoded;
        } else value.type = BRIDGE_DEV_MNGR_VALUE_NULL;
        report_sensor(slot, kLight, value, 0x0400, slot.light_report, encoded);
    }
}

} // namespace

extern "C" const mosaico_hotplug_accessory_t mosaico_hotplug_interact_accessory = {
    .name = "interact",
    .board_type = MOSAICO_BOARD_TYPE_INTERACT,
    .on_present = present,
    .on_absent = absent,
    .poll = poll,
    .attribute_changed = attribute_changed,
};
