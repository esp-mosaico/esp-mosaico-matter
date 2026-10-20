/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaico_hotplug_accessory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "claw_matter_bridge.h"

static const char *TAG = "mosaico_hotplug";

cJSON *mosaico_hotplug_fetch_devices(void)
{
    char *list_json = NULL;
    if (claw_matter_bridge_list_devices_json(&list_json) != ESP_OK) return NULL;
    cJSON *devices = cJSON_Parse(list_json);
    cJSON_free(list_json);
    return devices;
}

bool mosaico_hotplug_find_existing_device(cJSON *devices_list, const char *name,
    char *out_id, size_t out_id_len)
{
    if (!devices_list || !name || !out_id || !out_id_len) return false;
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(devices_list, "devices");
    cJSON *device;
    cJSON_ArrayForEach(device, devices) {
        cJSON *device_name_item = cJSON_GetObjectItemCaseSensitive(device, "name");
        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(device, "device_id");
        if (cJSON_IsString(device_name_item) && !strcmp(device_name_item->valuestring, name) &&
            cJSON_IsString(id_item)) {
            snprintf(out_id, out_id_len, "%s", id_item->valuestring);
            return true;
        }
    }
    return false;
}

bool mosaico_hotplug_add_virtual_device(const char *name, const char *type,
    const char *driver_source, cJSON *driver_args, char *out_id, size_t out_id_len)
{
    cJSON *request = cJSON_CreateObject();
    if (!request) {
        cJSON_Delete(driver_args);
        return false;
    }
    cJSON_AddStringToObject(request, "name", name);
    cJSON_AddStringToObject(request, "type", type);
    cJSON_AddBoolToObject(request, "virtual", true);
    cJSON_AddItemToObject(request, "pins", cJSON_CreateArray());
    cJSON_AddStringToObject(request, "driver_source", driver_source);
    cJSON_AddItemToObject(request, "driver_args", driver_args ? driver_args : cJSON_CreateObject());
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!text) return false;

    char *reply_json = NULL;
    esp_err_t err = claw_matter_bridge_add_virtual_device_json(text, &reply_json);
    cJSON_free(text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add %s: %s", name, esp_err_to_name(err));
        return false;
    }
    cJSON *reply = cJSON_Parse(reply_json);
    cJSON_free(reply_json);
    cJSON *id_item = reply ? cJSON_GetObjectItemCaseSensitive(reply, "device_id") : NULL;
    bool ok = cJSON_IsString(id_item) && out_id && out_id_len;
    if (ok) snprintf(out_id, out_id_len, "%s", id_item->valuestring);
    cJSON_Delete(reply);
    return ok;
}

void mosaico_hotplug_remove_device(const char *id)
{
    if (!id || !id[0]) return;
    esp_err_t err = claw_matter_bridge_remove_device(id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to remove device %s: %s", id, esp_err_to_name(err));
    }
}
