/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include "bridge_dev_mngr.h"
#include "nvs.h"

namespace mosaico_interact {

inline bool needs_default_rename(const bridge_dev_mngr_device_definition_t &definition,
                                  const char *legacy_name)
{
    return !strcmp(definition.name, legacy_name) ||
        (!strcmp(definition.profile_id, BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR) &&
         !strcmp(definition.name, "Light Sensor"));
}

// Only pre-native devices need this alias: their random ID cannot be derived
// from the EEPROM once the old hardware-based display name has been removed.
inline esp_err_t legacy_id(uint16_t board, uint32_t serial, size_t index,
                            char *id, bool save)
{
    char key[15];
    snprintf(key, sizeof(key), "%04x%08" PRIx32 "%u", board, serial, (unsigned)index);
    nvs_handle_t handle;
    esp_err_t error = nvs_open("mb_interact_ids", save ? NVS_READWRITE : NVS_READONLY, &handle);
    if (!save && error == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (error != ESP_OK) return error;
    if (save) {
        error = nvs_set_str(handle, key, id);
        if (error == ESP_OK) error = nvs_commit(handle);
    } else {
        char saved[BRIDGE_DEV_MNGR_DEVICE_ID_MAX]{};
        size_t size = sizeof(saved);
        error = nvs_get_str(handle, key, saved, &size);
        if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
        else if (error == ESP_OK) {
            if (!saved[0]) {
                error = ESP_ERR_INVALID_STATE;
            } else memcpy(id, saved, sizeof(saved));
        }
    }
    nvs_close(handle);
    return error;
}

inline esp_err_t adopt_definition(bridge_dev_mngr_device_definition_t &definition,
                                   const char *legacy_name,
                                   const bridge_dev_mngr_device_info_t *catalog,
                                   size_t count, bool &existing)
{
    existing = false;
    char requested_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
    memcpy(requested_id, definition.id, sizeof(requested_id));
    // Matching friendly names would conflate identical modules in two slots.
    for (size_t i = 0; i < count; ++i) {
        const auto &stored = catalog[i].definition;
        if (strcmp(stored.id, requested_id) && strcmp(stored.name, legacy_name)) continue;
        if (existing || stored.device_type != definition.device_type ||
            strcmp(stored.profile_id, definition.profile_id)) return ESP_ERR_INVALID_STATE;
        definition = stored;
        existing = true;
    }
    return ESP_OK;
}

} // namespace mosaico_interact
