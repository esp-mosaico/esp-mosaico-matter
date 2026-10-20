/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_matter_bridge_private.h"
#include "claw_matter_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_random.h"

namespace claw_matter_bridge
{

static esp_err_t emit(cJSON *json, char *out, size_t size)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = strlen(text) < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) snprintf(out, size, "%s", text);
    cJSON_free(text); return err;
}
static esp_err_t failure(esp_err_t err, const char *message, char *out, size_t size)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", esp_err_to_name(err));
    cJSON_AddStringToObject(j, "message", message ? message : "Bridge operation failed");
    emit(j, out, size);
    return err;
}
static cJSON *entry_json(const bridge_entry_t *e)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "device_id", e->model.device.id);
    cJSON_AddStringToObject(j, "name", e->model.device.name);
    cJSON_AddStringToObject(j, "type", bridge_type_name(e->model.type));
    cJSON_AddNumberToObject(j, "endpoint_id", e->endpoint_id);
    cJSON_AddStringToObject(j, "status", e->removing ? "removing" : e->failed ? "failed" : e->ready ? "ready" : "starting");
    cJSON_AddStringToObject(j, "error", e->error);
    cJSON_AddBoolToObject(j, "reachable", e->ready);
    cJSON_AddNumberToObject(j, "generation", e->generation);
    cJSON_AddItemToObject(j, "pins", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(e->definition, "pins"), true));
    return j;
}
static esp_err_t list_devices(const char *input, char *out, size_t size)
{
    (void)input;
    if (!g_bridge || !g_bridge->runtime_started) return failure(ESP_ERR_INVALID_STATE, "Bridge not started", out, size);
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    if (!g_bridge->management_enabled) {
        xSemaphoreGive(g_bridge->operations);
        return failure(ESP_ERR_INVALID_STATE, "Bridge management is stopped", out, size);
    }
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateObject(), *devices = cJSON_CreateArray();
    cJSON_AddItemToObject(j, "devices", devices);
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++)
        if (g_bridge->entries[i].used) cJSON_AddItemToArray(devices, entry_json(&g_bridge->entries[i]));
    xSemaphoreGive(g_bridge->lock);
    xSemaphoreGive(g_bridge->operations);
    return emit(j, out, size);
}
static esp_err_t info(const char *input, char *out, size_t size)
{
    if (!g_bridge || !g_bridge->runtime_started) return failure(ESP_ERR_INVALID_STATE, "Bridge not started", out, size);
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    if (!g_bridge->management_enabled) {
        xSemaphoreGive(g_bridge->operations);
        return failure(ESP_ERR_INVALID_STATE, "Bridge management is stopped", out, size);
    }
    cJSON *j = cJSON_Parse(input);
    bool open = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "open_commissioning_window"));
    cJSON_Delete(j);
    esp_err_t err = open ? bridge_core_open_commissioning_window(g_bridge->core) : ESP_OK;
    bool commissioned = false;
    if (err == ESP_OK) err = bridge_core_is_commissioned(g_bridge->core, &commissioned);
    if (err != ESP_OK) {
        xSemaphoreGive(g_bridge->operations);
        return failure(err, "Matter information/window operation failed", out, size);
    }
    j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "commissioned", commissioned);
    cJSON_AddNumberToObject(j, "max_devices", BRIDGE_DEV_MNGR_MAX_DEVICES);
    cJSON_AddNumberToObject(j, "pwm_frequency_hz", BRIDGE_PWM_HZ);
    cJSON_AddNumberToObject(j, "max_strip_leds", BRIDGE_MAX_LEDS);
    cJSON *pins = cJSON_CreateArray(); cJSON_AddItemToObject(j, "allowed_gpios", pins);
    for (int i = 0; i < 64; i++) if (bridge_gpio_allowed(i, g_bridge->allowed)) cJSON_AddItemToArray(pins, cJSON_CreateNumber(i));
    cJSON_AddStringToObject(j, "pairing_codes", "See serial console; opening a window prints onboarding codes.");
    err = emit(j, out, size);
    xSemaphoreGive(g_bridge->operations);
    return err;
}
static bool driver_valid(const cJSON *j)
{
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(j, "driver_source");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(j, "driver_args");
    return cJSON_IsString(source) && source->valuestring[0] && strlen(source->valuestring) <= BRIDGE_MAX_SOURCE &&
        (!args || cJSON_IsObject(args));
}
static esp_err_t add_device(const char *input, char *out, size_t size, bool allow_virtual)
{
    if (!g_bridge || !g_bridge->runtime_started) return failure(ESP_ERR_INVALID_STATE, "Bridge not started", out, size);
    cJSON *json = cJSON_Parse(input);
    auto *model = static_cast<bridge_model_t *>(calloc(1, sizeof(bridge_model_t)));
    if (!model) { cJSON_Delete(json); return ESP_ERR_NO_MEM; }
    char message[160] = "driver_source and object driver_args required";
    esp_err_t err = driver_valid(json) ? bridge_model_parse(json, g_bridge->allowed, model, message, sizeof(message)) : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK && model->virtual_device != allow_virtual) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(message, sizeof(message), "%s", allow_virtual
            ? "add_virtual_device requires virtual:true"
            : "virtual devices cannot be created through add_device");
    }
    /* Accessory-specific driver_args (slot, led_index, ...) are owned by
     * mosaico_hotplug_bridge adapters, not this GPIO model. */
    if (err == ESP_OK && allow_virtual &&
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(json, "driver_args"))) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(message, sizeof(message), "virtual devices require object driver_args");
    }
    if (err != ESP_OK) { cJSON_Delete(json); free(model); return failure(err, message, out, size); }
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    if (!g_bridge->management_enabled) {
        xSemaphoreGive(g_bridge->operations);
        cJSON_Delete(json);
        free(model);
        return failure(ESP_ERR_INVALID_STATE, "Bridge management is stopped", out, size);
    }
    bridge_entry_t *e = NULL;
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
        bridge_entry_t *candidate = &g_bridge->entries[i];
        if (candidate->used && !strcmp(candidate->model.device.name, model->device.name)) {
            err = ESP_ERR_INVALID_STATE; snprintf(message, sizeof(message), "Device name already exists; list devices first"); goto done;
        }
        if (!candidate->used && !e) e = candidate;
    }
    if (!e) {
        err = ESP_ERR_NO_MEM;
        snprintf(message, sizeof(message), "%d device slots are occupied",
            (int)BRIDGE_DEV_MNGR_MAX_DEVICES);
        goto done;
    }
    uint8_t random[16]; esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); i++) snprintf(model->device.id + i * 2, 3, "%02x", random[i]);
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    e->model = *model;
    e->definition = json; json = NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(e->definition, "revision");
    if (!cJSON_AddNumberToObject(e->definition, "revision", 1)) {
        json = e->definition; e->definition = NULL;
        xSemaphoreGive(g_bridge->lock); err = ESP_ERR_NO_MEM; goto done;
    }
    e->used = true;
    xSemaphoreGive(g_bridge->lock);
    err = bridge_prepare_script(e, cJSON_GetObjectItemCaseSensitive(e->definition, "driver_source")->valuestring, 1);
    if (err == ESP_OK) err = bridge_hardware_claim(e);
    if (err == ESP_OK) err = bridge_save(); /* Durable CLAW creation intent. */
    if (err == ESP_OK) {
        bridge_dev_mngr_device_info_t result = {};
        err = bridge_dev_mngr_register(&e->model.device, &result);
        if (err == ESP_OK) err = bridge_dev_mngr_activate(e->model.device.id, &result);
        if (err == ESP_OK) {
            e->endpoint_id = result.endpoint_id;
            e->endpoint_present = true;
        }
    }
    if (err == ESP_OK) err = bridge_save();
    if (err == ESP_OK) err = bridge_start_driver(e);
    if (err == ESP_OK) err = bridge_save();
    if (err != ESP_OK) {
        snprintf(message, sizeof(message), "Creation failed: %s; %.90s", esp_err_to_name(err), e->error);
        esp_err_t rollback = bridge_remove(e);
        if (rollback != ESP_OK) snprintf(message, sizeof(message), "Creation failed; cleanup pending: %s", esp_err_to_name(rollback));
    } else {
        xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
        cJSON *reply = entry_json(e); cJSON_AddBoolToObject(reply, "ok", true);
        xSemaphoreGive(g_bridge->lock);
        err = emit(reply, out, size);
    }
done:
    xSemaphoreGive(g_bridge->operations);
    cJSON_Delete(json); free(model);
    return err == ESP_OK ? err : failure(err, message, out, size);
}
enum action_t { ACTION_REMOVE, ACTION_RESTART, ACTION_UPDATE };
static esp_err_t device_action(action_t action, const char *input, char *out, size_t size)
{
    if (!g_bridge || !g_bridge->runtime_started) return failure(ESP_ERR_INVALID_STATE, "Bridge not started", out, size);
    cJSON *json = cJSON_Parse(input);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "device_id");
    if (!cJSON_IsString(id) || (action == ACTION_UPDATE && !driver_valid(json))) {
        cJSON_Delete(json); return failure(ESP_ERR_INVALID_ARG, "Invalid device_id or Lua driver", out, size);
    }
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    if (!g_bridge->management_enabled) {
        xSemaphoreGive(g_bridge->operations);
        cJSON_Delete(json);
        return failure(ESP_ERR_INVALID_STATE, "Bridge management is stopped", out, size);
    }
    bridge_entry_t *e = bridge_find(id->valuestring);
    esp_err_t err = e ? ESP_OK : ESP_ERR_NOT_FOUND;
    if (e && action != ACTION_REMOVE && (e->removing || !e->endpoint_present)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && action == ACTION_REMOVE) err = bridge_remove(e);
    if (err == ESP_OK && action == ACTION_RESTART) {
        err = bridge_start_driver(e);
        if (err != ESP_OK) e->failed = true;
        esp_err_t saved = bridge_save();
        if (saved != ESP_OK) err = saved;
    }
    if (err == ESP_OK && action == ACTION_UPDATE) {
        unsigned revision = cJSON_GetObjectItemCaseSensitive(e->definition, "revision")->valueint + 1U;
        auto *staged = static_cast<bridge_entry_t *>(calloc(1, sizeof(bridge_entry_t)));
        cJSON *definition = cJSON_Duplicate(e->definition, true);
        if (!staged || !definition || revision > INT32_MAX) err = ESP_ERR_NO_MEM;
        else {
            staged->model = e->model;
            const cJSON *source = cJSON_GetObjectItemCaseSensitive(json, "driver_source");
            const cJSON *args = cJSON_GetObjectItemCaseSensitive(json, "driver_args");
            cJSON *new_source = cJSON_CreateString(source->valuestring);
            cJSON *new_revision = cJSON_CreateNumber(revision);
            cJSON *new_args = args ? cJSON_Duplicate(args, true) : cJSON_CreateObject();
            if (!new_source || !new_revision || !new_args) {
                cJSON_Delete(new_source); cJSON_Delete(new_revision); cJSON_Delete(new_args); err = ESP_ERR_NO_MEM;
            } else {
                cJSON_DeleteItemFromObjectCaseSensitive(definition, "driver_source");
                cJSON_DeleteItemFromObjectCaseSensitive(definition, "revision");
                cJSON_DeleteItemFromObjectCaseSensitive(definition, "driver_args");
                if (!cJSON_AddItemToObject(definition, "driver_source", new_source)) { cJSON_Delete(new_source); err = ESP_ERR_NO_MEM; }
                if (!cJSON_AddItemToObject(definition, "revision", new_revision)) { cJSON_Delete(new_revision); err = ESP_ERR_NO_MEM; }
                if (!cJSON_AddItemToObject(definition, "driver_args", new_args)) { cJSON_Delete(new_args); err = ESP_ERR_NO_MEM; }
            }
            if (err == ESP_OK) err = bridge_prepare_script(staged, source->valuestring, revision);
            if (err == ESP_OK) err = bridge_stop_driver(e);
            if (err == ESP_OK) {
                char old_path[sizeof(e->path)];
                snprintf(old_path, sizeof(old_path), "%s", e->path);
                xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
                cJSON *old = e->definition;
                e->definition = definition; definition = old;
                snprintf(e->path, sizeof(e->path), "%s", staged->path);
                e->failed = false;
                xSemaphoreGive(g_bridge->lock);
                err = bridge_save();
                if (err != ESP_OK) {
                    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
                    cJSON *swap = e->definition; e->definition = definition; definition = swap;
                    snprintf(e->path, sizeof(e->path), "%s", old_path);
                    e->failed = true;
                    xSemaphoreGive(g_bridge->lock);
                } else {
                    unlink(old_path);
                    err = bridge_start_driver(e);
                    if (err != ESP_OK) e->failed = true;
                    esp_err_t saved = bridge_save();
                    if (saved != ESP_OK) err = saved;
                }
            }
            if (strcmp(staged->path, e->path)) unlink(staged->path);
        }
        cJSON_Delete(definition); free(staged);
    }
    cJSON *reply = NULL;
    if (err == ESP_OK) {
        reply = action == ACTION_REMOVE ? cJSON_CreateObject() : entry_json(e);
        cJSON_AddBoolToObject(reply, "ok", true);
    }
    xSemaphoreGive(g_bridge->operations);
    cJSON_Delete(json);
    return err == ESP_OK ? emit(reply, out, size) : failure(err,
        action == ACTION_REMOVE ? "Removal incomplete; retry or inspect pending cleanup" : "Driver repair failed; list devices for diagnostics", out, size);
}
using operation_t = esp_err_t (*)(const char *, char *, size_t);

static esp_err_t call_allocated(operation_t operation, const char *input,
                                char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;
    *out_json = nullptr;
    auto *output = static_cast<char *>(cJSON_malloc(BRIDGE_MAX_MANIFEST + 1));
    if (!output) return ESP_ERR_NO_MEM;
    output[0] = '\0';
    esp_err_t err = operation(input, output, BRIDGE_MAX_MANIFEST + 1);
    if (err != ESP_OK) {
        cJSON_free(output);
        return err;
    }
    *out_json = output;
    return ESP_OK;
}

static esp_err_t add_physical(const char *input, char *output, size_t size)
{
    return add_device(input, output, size, false);
}

static esp_err_t add_virtual(const char *input, char *output, size_t size)
{
    return add_device(input, output, size, true);
}

static esp_err_t remove(const char *input, char *output, size_t size)
{
    return device_action(ACTION_REMOVE, input, output, size);
}

static esp_err_t restart(const char *input, char *output, size_t size)
{
    return device_action(ACTION_RESTART, input, output, size);
}

static esp_err_t update(const char *input, char *output, size_t size)
{
    return device_action(ACTION_UPDATE, input, output, size);
}

static char *object_with_id(const char *device_id)
{
    if (!device_id || !device_id[0]) return nullptr;
    cJSON *object = cJSON_CreateObject();
    if (!object || !cJSON_AddStringToObject(object, "device_id", device_id)) {
        cJSON_Delete(object);
        return nullptr;
    }
    char *json = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    return json;
}

} // namespace claw_matter_bridge

esp_err_t claw_matter_bridge_get_info_json(bool open_commissioning_window,
                                           char **out_json)
{
    using namespace claw_matter_bridge;
    return call_allocated(info, open_commissioning_window
                                          ? "{\"open_commissioning_window\":true}"
                                          : "{\"open_commissioning_window\":false}",
                          out_json);
}

esp_err_t claw_matter_bridge_list_devices_json(char **out_json)
{
    using namespace claw_matter_bridge;
    return call_allocated(list_devices, "{}", out_json);
}

esp_err_t claw_matter_bridge_add_device_json(const char *definition_json,
                                             char **out_json)
{
    using namespace claw_matter_bridge;
    if (!out_json) return ESP_ERR_INVALID_ARG;
    *out_json = nullptr;
    if (!definition_json) return ESP_ERR_INVALID_ARG;
    return call_allocated(add_physical, definition_json, out_json);
}

esp_err_t
claw_matter_bridge_add_virtual_device_json(const char *definition_json,
                                           char **out_json)
{
    using namespace claw_matter_bridge;
    if (!out_json) return ESP_ERR_INVALID_ARG;
    *out_json = nullptr;
    if (!definition_json) return ESP_ERR_INVALID_ARG;
    return call_allocated(add_virtual, definition_json, out_json);
}

esp_err_t claw_matter_bridge_remove_device(const char *device_id)
{
    using namespace claw_matter_bridge;
    char *input = object_with_id(device_id);
    if (!input) return ESP_ERR_INVALID_ARG;
    char *output = nullptr;
    esp_err_t err = call_allocated(remove, input, &output);
    cJSON_free(input);
    cJSON_free(output);
    return err;
}

esp_err_t claw_matter_bridge_restart_device_json(const char *device_id,
                                                 char **out_json)
{
    using namespace claw_matter_bridge;
    if (!out_json) return ESP_ERR_INVALID_ARG;
    *out_json = nullptr;
    char *input = object_with_id(device_id);
    if (!input) return ESP_ERR_INVALID_ARG;
    esp_err_t err = call_allocated(restart, input, out_json);
    cJSON_free(input);
    return err;
}

esp_err_t claw_matter_bridge_update_driver_json(const char *device_id,
                                                const char *driver_source,
                                                const char *driver_args_json,
                                                char **out_json)
{
    using namespace claw_matter_bridge;
    if (!out_json) return ESP_ERR_INVALID_ARG;
    *out_json = nullptr;
    if (!device_id || !device_id[0] || !driver_source || !driver_source[0])
        return ESP_ERR_INVALID_ARG;
    cJSON *args = driver_args_json
                      ? cJSON_ParseWithOpts(driver_args_json, nullptr, true)
                      : cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    if (!args || !cJSON_IsObject(args) || !request ||
        !cJSON_AddStringToObject(request, "device_id", device_id) ||
        !cJSON_AddStringToObject(request, "driver_source", driver_source) ||
        !cJSON_AddItemToObject(request, "driver_args", args)) {
        cJSON_Delete(args);
        cJSON_Delete(request);
        return ESP_ERR_INVALID_ARG;
    }
    char *input = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!input) return ESP_ERR_NO_MEM;
    esp_err_t err = call_allocated(update, input, out_json);
    cJSON_free(input);
    return err;
}
