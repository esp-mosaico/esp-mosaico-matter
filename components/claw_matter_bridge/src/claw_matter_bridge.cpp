/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "claw_matter_bridge_private.h"
#include "claw_matter_bridge.h"
#include <atomic>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
extern "C" {
#include "lauxlib.h"
}
#include "sdkconfig.h"
#if defined(CONFIG_SPIRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#endif

namespace claw_matter_bridge
{

bridge_context_t *g_bridge;
static const char *TAG = "claw_matter_bridge";
struct matter_wait_t {
    SemaphoreHandle_t done;
    std::atomic_int references;
    bridge_dev_mngr_request_result_t result;
};
static void wait_release(matter_wait_t *w)
{
    if (w->references.fetch_sub(1) == 1) { vSemaphoreDelete(w->done); delete w; }
}
static void completed(bridge_dev_mngr_handle_t dev_mngr,
    const bridge_dev_mngr_request_result_t *result, void *ctx)
{
    (void)dev_mngr;
    auto *w = static_cast<matter_wait_t *>(ctx);
    w->result = *result;
    xSemaphoreGive(w->done);
    wait_release(w);
}
static matter_wait_t *wait_create(void)
{
    auto *w = new (std::nothrow) matter_wait_t{};
    if (!w) return NULL;
    w->done = xSemaphoreCreateBinary();
    if (!w->done) { delete w; return NULL; }
    w->references.store(2);
    return w;
}
static esp_err_t wait_finish(matter_wait_t *w, esp_err_t err,
    bridge_dev_mngr_request_result_t *out)
{
    if (err != ESP_OK) wait_release(w);
    else if (xSemaphoreTake(w->done, pdMS_TO_TICKS(5000)) != pdTRUE) err = ESP_ERR_TIMEOUT;
    else { if (out) *out = w->result; err = w->result.error; }
    wait_release(w);
    return err;
}
esp_err_t bridge_dev_mngr_register(const bridge_dev_mngr_device_definition_t *definition,
    bridge_dev_mngr_device_info_t *out)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_register_device(g_bridge->dev_mngr, definition,
        &request_id, completed, w);
    bridge_dev_mngr_request_result_t result;
    err = wait_finish(w, err, &result);
    if (err == ESP_OK && out) *out = result.device;
    return err;
}
esp_err_t bridge_dev_mngr_activate(const char *id, bridge_dev_mngr_device_info_t *out)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_activate_device(g_bridge->dev_mngr, id, NULL,
        &request_id, completed, w);
    bridge_dev_mngr_request_result_t result;
    err = wait_finish(w, err, &result);
    if (err == ESP_OK && out) *out = result.device;
    return err;
}
esp_err_t bridge_dev_mngr_deactivate(const char *id)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_deactivate_device(g_bridge->dev_mngr, id,
        &request_id, completed, w);
    return wait_finish(w, err, NULL);
}
esp_err_t bridge_dev_mngr_remove(const char *id)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_remove_device(g_bridge->dev_mngr, id,
        &request_id, completed, w);
    return wait_finish(w, err, NULL);
}
esp_err_t bridge_dev_mngr_update(const char *id,
    const bridge_dev_mngr_device_update_t *update)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_update_device(g_bridge->dev_mngr, id, update,
        &request_id, completed, w);
    return wait_finish(w, err, NULL);
}
esp_err_t bridge_dev_mngr_read(const char *id,
    const bridge_dev_mngr_attribute_path_t *path, bridge_dev_mngr_value_t *out)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_read_attribute(g_bridge->dev_mngr, id, path,
        &request_id, completed, w);
    bridge_dev_mngr_request_result_t result;
    err = wait_finish(w, err, &result);
    if (err == ESP_OK && out) *out = result.value;
    return err;
}
esp_err_t bridge_dev_mngr_report(const char *id,
    const bridge_dev_mngr_attribute_path_t *path, const bridge_dev_mngr_value_t *value)
{
    matter_wait_t *w = wait_create();
    if (!w) return ESP_ERR_NO_MEM;
    bridge_dev_mngr_request_id_t request_id;
    esp_err_t err = bridge_dev_mngr_report_attribute(g_bridge->dev_mngr, id, path,
        value, &request_id, completed, w);
    return wait_finish(w, err, NULL);
}
bridge_entry_t *bridge_find(const char *id)
{
    if (!id) return NULL;
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
        bridge_entry_t *e = &g_bridge->entries[i];
        if (e->used && !strcmp(e->model.device.id, id)) return e;
    }
    return NULL;
}
static bool value_bool(const bridge_dev_mngr_value_t *value, bool *out)
{
    if (value->type == BRIDGE_DEV_MNGR_VALUE_BOOL) *out = value->data.boolean;
    else if (value->type == BRIDGE_DEV_MNGR_VALUE_UNSIGNED) *out = value->data.unsigned_value != 0;
    else if (value->type == BRIDGE_DEV_MNGR_VALUE_SIGNED) *out = value->data.signed_value != 0;
    else return false;
    return true;
}
static bool value_u16(const bridge_dev_mngr_value_t *value, uint16_t *out)
{
    uint64_t number;
    if (value->type == BRIDGE_DEV_MNGR_VALUE_UNSIGNED) number = value->data.unsigned_value;
    else if (value->type == BRIDGE_DEV_MNGR_VALUE_SIGNED && value->data.signed_value >= 0)
        number = (uint64_t)value->data.signed_value;
    else return false;
    if (number > UINT16_MAX) return false;
    *out = (uint16_t)number;
    return true;
}
static bool apply_attribute(bridge_entry_t *e,
    const bridge_dev_mngr_attribute_path_t *path, const bridge_dev_mngr_value_t *value)
{
    bool boolean;
    uint16_t number;
    if (path->cluster_id == 0x0006 && path->attribute_id == 0 && value_bool(value, &boolean))
        e->desired.on = boolean;
    else if (path->cluster_id == 0x0008 && path->attribute_id == 0 && value_u16(value, &number))
        e->desired.level = (uint8_t)number;
    else if (path->cluster_id == 0x0300 && path->attribute_id == 0 && value_u16(value, &number))
        e->desired.hue = (uint8_t)number;
    else if (path->cluster_id == 0x0300 && path->attribute_id == 1 && value_u16(value, &number))
        e->desired.saturation = (uint8_t)number;
    else if (path->cluster_id == 0x0300 && path->attribute_id == 3 && value_u16(value, &number))
        e->desired.x = number;
    else if (path->cluster_id == 0x0300 && path->attribute_id == 4 && value_u16(value, &number))
        e->desired.y = number;
    else if (path->cluster_id == 0x0300 && path->attribute_id == 7 && value_u16(value, &number))
        e->desired.temperature = number;
    else if (path->cluster_id == 0x0300 && path->attribute_id == 8 && value_u16(value, &number))
        e->desired.color_mode = (uint8_t)number;
    else if ((path->cluster_id == 0x0045 || path->cluster_id == 0x0406) &&
             path->attribute_id == 0 && value_bool(value, &boolean))
        e->desired.sensor = boolean;
    else return false;
    return true;
}
static void attribute_changed(bridge_dev_mngr_handle_t dev_mngr,
    const bridge_dev_mngr_attribute_change_t *change, void *ctx)
{
    (void)dev_mngr;
    auto *b = static_cast<bridge_context_t *>(ctx);
    xSemaphoreTake(b->lock, portMAX_DELAY);
    bridge_entry_t *e = bridge_find(change->device_id);
    if (e && !e->removing && !e->failed &&
        apply_attribute(e, &change->path, &change->value)) e->version++;
    xSemaphoreGive(b->lock);
}
esp_err_t bridge_save(void)
{
    g_bridge->dirty = true;
    cJSON *doc = cJSON_CreateObject(), *devices = cJSON_CreateArray();
    if (!doc || !devices) { cJSON_Delete(doc); cJSON_Delete(devices); return ESP_ERR_NO_MEM; }
    if (!cJSON_AddNumberToObject(doc, "schema", 2) || !cJSON_AddItemToObject(doc, "devices", devices)) {
        cJSON_Delete(doc); cJSON_Delete(devices); return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
        bridge_entry_t *e = &g_bridge->entries[i];
        if (!e->used) continue;
        cJSON *row = cJSON_Duplicate(e->definition, true);
        if (!row) { xSemaphoreGive(g_bridge->lock); cJSON_Delete(doc); return ESP_ERR_NO_MEM; }
        cJSON_DeleteItemFromObjectCaseSensitive(row, "id");
        cJSON_DeleteItemFromObjectCaseSensitive(row, "endpoint_id");
        cJSON_DeleteItemFromObjectCaseSensitive(row, "removing");
        cJSON_DeleteItemFromObjectCaseSensitive(row, "failed");
        cJSON_DeleteItemFromObjectCaseSensitive(row, "error");
        if (!cJSON_AddStringToObject(row, "id", e->model.device.id) ||
            !cJSON_AddBoolToObject(row, "removing", e->removing) ||
            !cJSON_AddBoolToObject(row, "failed", e->failed) ||
            !cJSON_AddStringToObject(row, "error", e->error) || !cJSON_AddItemToArray(devices, row)) {
            xSemaphoreGive(g_bridge->lock); cJSON_Delete(row); cJSON_Delete(doc); return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(g_bridge->lock);
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!text) return ESP_ERR_NO_MEM;
    esp_err_t err = strlen(text) <= BRIDGE_MAX_MANIFEST
        ? bridge_atomic_write(g_bridge->manifest, text, strlen(text)) : ESP_ERR_INVALID_SIZE;
    g_bridge->dirty = err != ESP_OK;
    cJSON_free(text);
    return err;
}
esp_err_t bridge_prepare_script(bridge_entry_t *e, const char *source, unsigned revision)
{
    if (!source || !source[0] || strlen(source) > BRIDGE_MAX_SOURCE) return ESP_ERR_INVALID_ARG;
    lua_State *L = luaL_newstate();
    if (!L) return ESP_ERR_NO_MEM;
    int status = luaL_loadbuffer(L, source, strlen(source), "matter_device_driver");
    if (status != LUA_OK) snprintf(e->error, sizeof(e->error), "Lua syntax: %.130s", lua_tostring(L, -1));
    lua_close(L);
    if (status != LUA_OK) return ESP_ERR_INVALID_ARG;
    char path[192];
    if (snprintf(path, sizeof(path), "%s/%s-%u.lua", g_bridge->root, e->model.device.id, revision) >= (int)sizeof(path))
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err = bridge_atomic_write(path, source, strlen(source));
    if (err == ESP_OK) snprintf(e->path, sizeof(e->path), "%s", path);
    return err;
}
static void job_event(const cap_lua_job_event_t *event, void *ctx)
{
    auto *b = static_cast<bridge_context_t *>(ctx);
    xSemaphoreTake(b->lock, portMAX_DELAY);
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
        bridge_entry_t *e = &b->entries[i];
        if (!e->used) continue;
        if (event->type == CAP_LUA_JOB_EVENT_CREATED && !strcmp(e->run_name, event->name))
            snprintf(e->job, sizeof(e->job), "%s", event->job_id);
        if (event->type == CAP_LUA_JOB_EVENT_TERMINAL && !strcmp(e->job, event->job_id)) {
            e->access = false;
            e->ready = false;
            if (!e->removing && !e->error[0])
                snprintf(e->error, sizeof(e->error), "Lua driver terminated (status %d)", event->status);
        }
    }
    xSemaphoreGive(b->lock);
}
esp_err_t bridge_stop_driver(bridge_entry_t *e)
{
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    if (e->quiesced && !e->job[0]) { xSemaphoreGive(g_bridge->lock); return ESP_OK; }
    e->access = false;
    e->ready = false;
    char job[CAP_LUA_JOB_ID_LEN];
    snprintf(job, sizeof(job), "%s", e->job);
    xSemaphoreGive(g_bridge->lock);
    esp_err_t err = ESP_OK;
    if (job[0]) {
        char output[192];
        err = cap_lua_stop_job(job, BRIDGE_READY_MS, output, sizeof(output));
        if (err == ESP_ERR_NOT_FOUND) err = ESP_OK;
    }
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    esp_err_t hw = bridge_hardware_close(e);
    if (err == ESP_OK) e->job[0] = 0;
    e->generation++;
    xSemaphoreGive(g_bridge->lock);
    bridge_dev_mngr_device_update_t update = {};
    update.fields = BRIDGE_DEV_MNGR_UPDATE_REACHABLE;
    update.reachable = false;
    esp_err_t matter = e->endpoint_present
        ? bridge_dev_mngr_update(e->model.device.id, &update) : ESP_OK;
    /* Once the job has stopped and native outputs are inactive, destruction
     * itself removes reachability. A broken/unregistered attribute must not
     * prevent removal of the endpoint that caused the reporting failure. */
    if (e->removing && err == ESP_OK && hw == ESP_OK) matter = ESP_OK;
    if (err == ESP_OK && hw == ESP_OK && matter == ESP_OK) e->quiesced = true;
    return err != ESP_OK ? err : hw != ESP_OK ? hw : matter;
}
esp_err_t bridge_start_driver(bridge_entry_t *e)
{
    esp_err_t err = bridge_stop_driver(e);
    if (err != ESP_OK) return err;
    if (!e->lease && (err = bridge_hardware_claim(e)) != ESP_OK) return err;
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    err = bridge_hardware_open(e);
    e->error[0] = 0;
    e->failed = false;
    e->desired.on = false;
    e->generation++;
    e->version++;
    e->heartbeat_us = esp_timer_get_time();
    snprintf(e->run_name, sizeof(e->run_name), "mb%.12s-%lu", e->model.device.id, (unsigned long)e->generation);
    xSemaphoreGive(g_bridge->lock);
    if (err != ESP_OK) return err;
    cJSON *args = cJSON_CreateObject();
    if (!args) { bridge_stop_driver(e); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(args, "device_id", e->model.device.id);
    cJSON_AddNumberToObject(args, "generation", e->generation);
    cJSON_AddStringToObject(args, "driver_path", e->path);
    char *library = strdup(g_bridge->runner);
    if (!library) { cJSON_Delete(args); bridge_stop_driver(e); return ESP_ERR_NO_MEM; }
    char *slash = strrchr(library, '/');
    if (slash) *slash = 0;
    cJSON_AddStringToObject(args, "library_dir", library);
    free(library);
    cJSON *user_args = cJSON_GetObjectItemCaseSensitive(e->definition, "driver_args");
    cJSON_AddItemToObject(args, "driver_args", user_args ? cJSON_Duplicate(user_args, true) : cJSON_CreateObject());
    char *encoded = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    if (!encoded) { bridge_stop_driver(e); return ESP_ERR_NO_MEM; }
    char output[192];
    err = cap_lua_run_script_async(g_bridge->runner, encoded, 0, e->run_name, e->run_name, false,
                                  output, sizeof(output));
    cJSON_free(encoded);
    if (err == ESP_OK) {
        int64_t deadline = esp_timer_get_time() + BRIDGE_READY_MS * INT64_C(1000);
        do {
            xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
            bool ready = e->ready, access = e->access;
            xSemaphoreGive(g_bridge->lock);
            if (ready) return ESP_OK;
            if (!access) { err = ESP_FAIL; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        } while (esp_timer_get_time() < deadline);
        if (err == ESP_OK) err = ESP_ERR_TIMEOUT;
    }
    snprintf(e->error, sizeof(e->error), "Driver startup failed: %s", esp_err_to_name(err));
    bridge_stop_driver(e);
    e->failed = true;
    return err;
}
esp_err_t bridge_remove(bridge_entry_t *e)
{
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    e->removing = true;
    xSemaphoreGive(g_bridge->lock);
    esp_err_t err = bridge_save();
    if (err != ESP_OK) {
        /* Retain identity/leases until the tombstone is durable, but still
         * make the requested device inactive if storage is unavailable. */
        bridge_stop_driver(e);
        return err;
    }
    err = bridge_stop_driver(e);
    if (err != ESP_OK) return err;
    {
        err = bridge_dev_mngr_remove(e->model.device.id);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
        e->endpoint_present = false;
    }
    /* Persist absence before releasing the name, slot and pins. On failure the
     * tombstone remains authoritative and the supervisor retries. */
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    e->used = false;
    xSemaphoreGive(g_bridge->lock);
    err = bridge_save();
    if (err != ESP_OK) {
        xSemaphoreTake(g_bridge->lock, portMAX_DELAY); e->used = true; xSemaphoreGive(g_bridge->lock);
        return err;
    }
    bridge_hardware_release(e);
    unlink(e->path);
    cJSON_Delete(e->definition);
    xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
    memset(e, 0, sizeof(*e));
    for (size_t i = 0; i < BRIDGE_MAX_PINS; i++) e->pwm_channels[i] = -1;
    xSemaphoreGive(g_bridge->lock);
    return ESP_OK;
}
static esp_err_t load(void)
{
    struct stat st;
    if (stat(g_bridge->manifest, &st) != 0) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    char *text = bridge_read_file(g_bridge->manifest, BRIDGE_MAX_MANIFEST);
    if (!text) return ESP_FAIL;
    cJSON *doc = cJSON_Parse(text);
    free(text);
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(doc, "schema");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(doc, "devices");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 2 || !cJSON_IsArray(rows) ||
        cJSON_GetArraySize(rows) > BRIDGE_DEV_MNGR_MAX_DEVICES) goto done;
    for (int i = 0; i < cJSON_GetArraySize(rows); i++) {
        const cJSON *row = cJSON_GetArrayItem(rows, i);
        bridge_entry_t *e = &g_bridge->entries[i];
        if (bridge_model_parse(row, g_bridge->allowed, &e->model, e->error, sizeof(e->error)) != ESP_OK) goto done;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        const cJSON *revision = cJSON_GetObjectItemCaseSensitive(row, "revision");
        if (!cJSON_IsString(id) || strlen(id->valuestring) != 32 || strspn(id->valuestring, "0123456789abcdef") != 32 ||
            !cJSON_IsNumber(revision) || revision->valueint < 1 || revision->valueint != revision->valuedouble) goto done;
        strcpy(e->model.device.id, id->valuestring);
        for (int j = 0; j < i; j++) {
            bridge_entry_t *other = &g_bridge->entries[j];
            if (!strcmp(other->model.device.id, e->model.device.id) ||
                !strcmp(other->model.device.name, e->model.device.name)) goto done;
        }
        e->definition = cJSON_Duplicate(row, true);
        if (!e->definition) { err = ESP_ERR_NO_MEM; goto done; }
        e->used = true;
        e->removing = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "removing"));
        e->failed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "failed"));
        char path[sizeof(e->path)];
        snprintf(path, sizeof(path), "%s/%s-%u.lua", g_bridge->root, e->model.device.id, revision->valueint);
        memcpy(e->path, path, sizeof(e->path));
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(row, "error");
        if (cJSON_IsString(message)) snprintf(e->error, sizeof(e->error), "%s", message->valuestring);
    }
    err = ESP_OK;
done:
    cJSON_Delete(doc);
    return err; /* Invalid manifests fail closed; never erase SDK identities. */
}
void bridge_supervise_once(void)
{
        if (xSemaphoreTake(g_bridge->operations, 0) != pdTRUE) return;
        for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
            bridge_entry_t *e = &g_bridge->entries[i];
            if (!e->used) continue;
            if (e->removing) { bridge_remove(e); continue; }
            xSemaphoreTake(g_bridge->lock, portMAX_DELAY);
            bool failure = !e->failed && (!e->access ||
                esp_timer_get_time() - e->heartbeat_us > BRIDGE_HEARTBEAT_MS * INT64_C(1000));
            if (failure) {
                e->failed = true;
                if (!e->error[0]) snprintf(e->error, sizeof(e->error), "Driver heartbeat expired");
            }
            bool failed = e->failed;
            xSemaphoreGive(g_bridge->lock);
            if (failed) {
                esp_err_t err = bridge_stop_driver(e);
                if (failure || err != ESP_OK) {
                    if (bridge_save() != ESP_OK) ESP_LOGE(TAG, "Failed to persist driver fault");
                }
            }
        }
        if (g_bridge->dirty && bridge_save() != ESP_OK) ESP_LOGE(TAG, "Manifest retry failed");
        xSemaphoreGive(g_bridge->operations);
}
static void supervise(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        bridge_supervise_once();
    }
}
static bool same_config(const bridge_context_t *state,
                        const claw_matter_bridge_config_t *config)
{
    return state->core == config->core && state->dev_mngr == config->dev_mngr &&
           !strcmp(state->root, config->storage_root) &&
           !strcmp(state->runner, config->runner_path);
}

static esp_err_t restore(void)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++) {
        bridge_entry_t *e = &g_bridge->entries[i];
        if (!e->used) continue;
        if (e->removing) { bridge_remove(e); continue; }
        bridge_dev_mngr_device_info_t result;
        err = bridge_dev_mngr_register(&e->model.device, &result);
        if (err == ESP_OK) err = bridge_dev_mngr_activate(e->model.device.id, &result);
        if (err == ESP_OK) {
            e->endpoint_present = true;
            e->endpoint_id = result.endpoint_id;
            err = bridge_hardware_claim(e);
        }
        char *source = bridge_read_file(e->path, BRIDGE_MAX_SOURCE);
        const cJSON *persisted = cJSON_GetObjectItemCaseSensitive(e->definition, "driver_source");
        if (!source || !cJSON_IsString(persisted) || strcmp(source, persisted->valuestring)) err = ESP_ERR_INVALID_STATE;
        free(source);
        if (err == ESP_OK && !e->failed) err = bridge_start_driver(e);
        if (err != ESP_OK) {
            e->failed = true;
            snprintf(e->error, sizeof(e->error), "Restore failed: %s", esp_err_to_name(err));
            bridge_stop_driver(e);
        }
    }
    err = bridge_save();
    xSemaphoreGive(g_bridge->operations);
    return err;
}

static esp_err_t initialize(const claw_matter_bridge_config_t *config)
{
    auto *state = static_cast<bridge_context_t *>(calloc(1, sizeof(bridge_context_t)));
    if (!state) return ESP_ERR_NO_MEM;
    state->lock = xSemaphoreCreateMutex();
    state->operations = xSemaphoreCreateMutex();
    if (!state->lock || !state->operations) {
        if (state->lock) vSemaphoreDelete(state->lock);
        if (state->operations) vSemaphoreDelete(state->operations);
        free(state);
        return ESP_ERR_NO_MEM;
    }
    snprintf(state->root, sizeof(state->root), "%s", config->storage_root);
    snprintf(state->manifest, sizeof(state->manifest), "%s/devices.json",
             config->storage_root);
    snprintf(state->runner, sizeof(state->runner), "%s", config->runner_path);
    state->core = config->core;
    state->dev_mngr = config->dev_mngr;
    state->allowed = CONFIG_CLAW_MATTER_BRIDGE_GPIO_ALLOWLIST;
    for (auto &entry : state->entries)
        for (auto &channel : entry.pwm_channels) channel = -1;
    g_bridge = state;

    if (mkdir(state->root, 0770) != 0 && errno != EEXIST) return ESP_FAIL;
    esp_err_t err = load();
    if (err != ESP_OK) return err;
    err = claw_hw_registry_init();
    if (err != ESP_OK) return err;
    bridge_dev_mngr_listener_t listener = {};
    listener.attribute_changed = attribute_changed;
    listener.user_ctx = state;
    err = bridge_dev_mngr_add_listener(state->dev_mngr, &listener, &state->listener_id);
    if (err != ESP_OK) return err;
    err = cap_lua_register_job_event_cb(job_event, state);
    if (err != ESP_OK) return err;

    err = restore();
    if (err != ESP_OK) return err;
#if defined(CONFIG_SPIRAM) && defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)
    if (xTaskCreateWithCaps(supervise, "bridge_watchdog", 6144, NULL, 3, NULL,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return ESP_ERR_NO_MEM;
#else
    if (xTaskCreate(supervise, "bridge_watchdog", 6144, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
#endif
    state->runtime_started = true;
    state->management_enabled = true;
    return ESP_OK;
}

} // namespace claw_matter_bridge

esp_err_t claw_matter_bridge_start(const claw_matter_bridge_config_t *config)
{
    using namespace claw_matter_bridge;
    if (!config || !config->core || !config->dev_mngr || !config->storage_root ||
        !config->runner_path || strlen(config->storage_root) >= sizeof(g_bridge->root) ||
        strlen(config->runner_path) >= sizeof(g_bridge->runner))
        return ESP_ERR_INVALID_ARG;
    if (!g_bridge) return initialize(config);

    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    esp_err_t err = !g_bridge->runtime_started ||
                            !same_config(g_bridge, config)
                        ? ESP_ERR_INVALID_STATE
                        : g_bridge->management_enabled
                              ? ESP_ERR_INVALID_STATE
                              : ESP_OK;
    if (err == ESP_OK) g_bridge->management_enabled = true;
    xSemaphoreGive(g_bridge->operations);
    return err;
}

esp_err_t claw_matter_bridge_stop(void)
{
    using namespace claw_matter_bridge;
    if (!g_bridge || !g_bridge->runtime_started) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    esp_err_t err = g_bridge->management_enabled ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) g_bridge->management_enabled = false;
    xSemaphoreGive(g_bridge->operations);
    return err;
}

esp_err_t claw_matter_bridge_factory_reset(void)
{
    using namespace claw_matter_bridge;
    if (!g_bridge || !g_bridge->runtime_started) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_bridge->operations, portMAX_DELAY);
    if (!g_bridge->management_enabled) {
        xSemaphoreGive(g_bridge->operations);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < BRIDGE_DEV_MNGR_MAX_DEVICES; i++)
        if (g_bridge->entries[i].used) {
            esp_err_t one = bridge_remove(&g_bridge->entries[i]);
            if (one != ESP_OK) err = one;
    }
    xSemaphoreGive(g_bridge->operations);
    return err;
}
