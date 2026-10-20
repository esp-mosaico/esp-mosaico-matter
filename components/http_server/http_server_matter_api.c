/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_matter.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define MATTER_BODY_MAX 1024
#define MATTER_URI_ID_MAX 11
#define MATTER_OPERATION_MAX 8
#define MATTER_SSE_MAX_CLIENTS 3
#define MATTER_SSE_QUEUE_MAX 8
#define MATTER_SSE_PAYLOAD_MAX 768
#define MATTER_SSE_VALUE_MAX 96
#define MATTER_SSE_KEEPALIVE_US (15 * 1000 * 1000)

typedef struct {
    char payload[MATTER_SSE_PAYLOAD_MAX];
} matter_sse_event_t;

typedef struct {
    bool allocated;
    bool active;
    fabric_core_request_id_t request_id;
    esp_err_t result;
    uint32_t sequence;
} matter_operation_t;

static const char *TAG = "http_matter";

static fabric_core_handle_t s_core;
static fabric_manager_handle_t s_manager;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_init = portMUX_INITIALIZER_UNLOCKED;
static fabric_manager_listener_id_t s_manager_listener_id;
static bool s_binding_changing;
static httpd_handle_t s_httpd;
static httpd_req_t *s_sse_clients[MATTER_SSE_MAX_CLIENTS];
static esp_timer_handle_t s_sse_keepalive;
/* Manager callbacks only enqueue bounded semantic deltas. The HTTP task owns
 * socket writes; queue overflow becomes a resync instead of unbounded work. */
static EXT_RAM_BSS_ATTR matter_sse_event_t
    s_sse_queue[MATTER_SSE_QUEUE_MAX];
static size_t s_sse_queue_head;
static size_t s_sse_queue_count;
static bool s_sse_work_queued;
static bool s_sse_resync_pending;
static bool s_sse_keepalive_pending;
static uint32_t s_sse_sequence;
static matter_operation_t s_operations[MATTER_OPERATION_MAX];
static uint32_t s_operation_sequence;
static struct {
    bool active;
    fabric_core_request_id_t request_id;
    esp_err_t result;
} s_commission;

static SemaphoreHandle_t matter_lock(void)
{
    portENTER_CRITICAL(&s_lock_init);
    SemaphoreHandle_t lock = s_lock;
    portEXIT_CRITICAL(&s_lock_init);
    return lock;
}

static esp_err_t ensure_matter_lock(void)
{
    if (matter_lock()) return ESP_OK;
    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    if (!candidate) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock_init);
    if (!s_lock) {
        s_lock = candidate;
        candidate = NULL;
    }
    portEXIT_CRITICAL(&s_lock_init);
    if (candidate) vSemaphoreDelete(candidate);
    return ESP_OK;
}

static bool binding_get(fabric_core_handle_t *core, fabric_manager_handle_t *manager)
{
    SemaphoreHandle_t lock = matter_lock();
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    const bool enabled = !s_binding_changing && s_core && s_manager &&
                         s_manager_listener_id != 0;
    if (core) *core = enabled ? s_core : NULL;
    if (manager) *manager = enabled ? s_manager : NULL;
    xSemaphoreGive(lock);
    return enabled;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t error = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return error;
}

static esp_err_t send_error(httpd_req_t *req, esp_err_t error)
{
    const char *status = "500 Internal Server Error";
    if (error == ESP_ERR_INVALID_ARG) {
        status = "400 Bad Request";
    } else if (error == ESP_ERR_NOT_FOUND) {
        status = "404 Not Found";
    } else if (error == ESP_ERR_INVALID_STATE) {
        status = "409 Conflict";
    } else if (error == ESP_ERR_NOT_SUPPORTED) {
        status = "501 Not Implemented";
    }
    httpd_resp_set_status(req, status);
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddStringToObject(root, "error",
                                          esp_err_to_name(error))) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    return send_json(req, root);
}

static cJSON *receive_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MATTER_BODY_MAX) {
        return NULL;
    }
    char *body = malloc((size_t)req->content_len + 1);
    if (!body) {
        return NULL;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int count = httpd_req_recv(req, body + received,
                                         req->content_len - received);
        if (count <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

static bool sse_has_clients_locked(void)
{
    for (size_t i = 0; i < MATTER_SSE_MAX_CLIENTS; ++i) {
        if (s_sse_clients[i]) {
            return true;
        }
    }
    return false;
}

static void sse_drain_work(void *arg);

static void sse_schedule_locked(void)
{
    if (s_sse_work_queued || !s_httpd || !sse_has_clients_locked()) {
        return;
    }
    if (httpd_queue_work(s_httpd, sse_drain_work, NULL) == ESP_OK) {
        s_sse_work_queued = true;
    }
}

static void sse_request_resync(fabric_manager_handle_t manager)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_binding_changing && manager == s_manager && s_manager_listener_id &&
        sse_has_clients_locked()) {
        s_sse_resync_pending = true;
        sse_schedule_locked();
    }
    xSemaphoreGive(s_lock);
}

static void sse_enqueue(fabric_manager_handle_t manager, const char *payload)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_binding_changing || manager != s_manager || !s_manager_listener_id ||
        !sse_has_clients_locked()) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_sse_queue_count == MATTER_SSE_QUEUE_MAX) {
        s_sse_resync_pending = true;
    } else {
        const size_t tail =
            (s_sse_queue_head + s_sse_queue_count) % MATTER_SSE_QUEUE_MAX;
        strlcpy(s_sse_queue[tail].payload, payload,
                sizeof(s_sse_queue[tail].payload));
        ++s_sse_queue_count;
    }
    sse_schedule_locked();
    xSemaphoreGive(s_lock);
}

static bool append_format(char *buffer, size_t capacity, size_t *length,
                          const char *format, ...)
{
    if (*length >= capacity) {
        return false;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer + *length, capacity - *length,
                                  format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }
    *length += (size_t)written;
    return true;
}

static bool serialize_property_event(
    const fabric_manager_property_change_t *changes, size_t change_count,
    char payload[MATTER_SSE_PAYLOAD_MAX])
{
    size_t length = 0;
    if (!append_format(payload, MATTER_SSE_PAYLOAD_MAX, &length,
                       "{\"type\":\"properties_changed\",\"changes\":[")) {
        return false;
    }
    for (size_t i = 0; i < change_count; ++i) {
        const char *previous = changes[i].previous_value_json;
        const char *value = changes[i].value_json;
        if (!previous || !value || strlen(previous) >= MATTER_SSE_VALUE_MAX ||
            strlen(value) >= MATTER_SSE_VALUE_MAX ||
            !append_format(payload, MATTER_SSE_PAYLOAD_MAX, &length,
                           "%s{\"device_id\":%" PRIu32
                           ",\"endpoint\":%u,\"cluster\":\"%s\","
                           "\"property\":\"%s\",\"previous_value\":%s,"
                           "\"value\":%s}",
                           i ? "," : "", changes[i].device_id,
                           changes[i].endpoint_id, changes[i].cluster,
                           changes[i].property, previous, value)) {
            return false;
        }
    }
    return append_format(payload, MATTER_SSE_PAYLOAD_MAX, &length, "]}");
}

static void sse_send_to_clients(const char *payload, bool keepalive,
                                uint32_t sequence)
{
    char frame[MATTER_SSE_PAYLOAD_MAX + 64];
    const int frame_length = keepalive
        ? snprintf(frame, sizeof(frame), ": keepalive\n\n")
        : snprintf(frame, sizeof(frame), "id: %" PRIu32 "\ndata: %s\n\n",
                   sequence, payload);
    if (frame_length <= 0 || (size_t)frame_length >= sizeof(frame)) {
        return;
    }

    for (size_t i = 0; i < MATTER_SSE_MAX_CLIENTS; ++i) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        httpd_req_t *client = s_sse_clients[i];
        xSemaphoreGive(s_lock);
        if (!client || httpd_resp_send_chunk(client, frame,
                                              (size_t)frame_length) == ESP_OK) {
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_sse_clients[i] == client) {
            s_sse_clients[i] = NULL;
        }
        xSemaphoreGive(s_lock);
        httpd_req_async_handler_complete(client);
    }
}

static void sse_drain_work(void *arg)
{
    (void)arg;
    for (;;) {
        matter_sse_event_t event = {0};
        bool have_event = false;
        bool keepalive = false;
        uint32_t sequence = 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_sse_queue_count) {
            event = s_sse_queue[s_sse_queue_head];
            s_sse_queue_head =
                (s_sse_queue_head + 1) % MATTER_SSE_QUEUE_MAX;
            --s_sse_queue_count;
            have_event = true;
            sequence = ++s_sse_sequence;
        } else if (s_sse_resync_pending) {
            strlcpy(event.payload, "{\"type\":\"resync\"}",
                    sizeof(event.payload));
            s_sse_resync_pending = false;
            have_event = true;
            sequence = ++s_sse_sequence;
        } else if (s_sse_keepalive_pending) {
            s_sse_keepalive_pending = false;
            keepalive = true;
        } else {
            s_sse_work_queued = false;
            xSemaphoreGive(s_lock);
            return;
        }
        xSemaphoreGive(s_lock);

        sse_send_to_clients(event.payload, keepalive, sequence);
        if (!have_event && !keepalive) {
            return;
        }
    }
}

static void manager_device_set_changed(fabric_manager_handle_t manager, void *user_ctx)
{
    (void)user_ctx;
    sse_enqueue(manager, "{\"type\":\"devices_changed\"}");
}

static void manager_properties_changed(
    fabric_manager_handle_t manager, const fabric_manager_property_change_t *changes,
    size_t change_count, void *user_ctx)
{
    (void)user_ctx;
    if (!changes || !change_count) {
        return;
    }
    char payload[MATTER_SSE_PAYLOAD_MAX];
    if (serialize_property_event(changes, change_count, payload)) {
        sse_enqueue(manager, payload);
    } else {
        sse_request_resync(manager);
    }
}

static void sse_keepalive_callback(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (sse_has_clients_locked()) {
        s_sse_keepalive_pending = true;
        sse_schedule_locked();
    }
    xSemaphoreGive(s_lock);
}

static void sse_prime_work(void *arg)
{
    httpd_req_t *async = arg;
    httpd_resp_set_type(async, "text/event-stream");
    httpd_resp_set_hdr(async, "Cache-Control", "no-store");
    httpd_resp_set_hdr(async, "Connection", "keep-alive");
    static const char initial[] =
        "retry: 2000\ndata: {\"type\":\"resync\"}\n\n";
    if (httpd_resp_send_chunk(async, initial, sizeof(initial) - 1) != ESP_OK) {
        httpd_req_async_handler_complete(async);
        return;
    }

    bool added = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < MATTER_SSE_MAX_CLIENTS; ++i) {
        if (!s_sse_clients[i]) {
            s_sse_clients[i] = async;
            added = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!added) {
        ESP_LOGW(TAG, "Matter SSE client limit reached");
        httpd_req_async_handler_complete(async);
    }
}

static esp_err_t sse_handler(httpd_req_t *req)
{
    if (!binding_get(NULL, NULL) || !s_httpd) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    httpd_req_t *async = NULL;
    esp_err_t error = httpd_req_async_handler_begin(req, &async);
    if (error != ESP_OK) {
        return error;
    }
    error = httpd_queue_work(s_httpd, sse_prime_work, async);
    if (error != ESP_OK) {
        httpd_req_async_handler_complete(async);
    }
    return error;
}

static esp_err_t uri_device_id(httpd_req_t *req, const char **out_tail,
                                fabric_manager_device_id_t *out_device_id)
{
    static const char prefix[] = "/api/matter/devices/";
    if (strncmp(req->uri, prefix, sizeof(prefix) - 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *text = req->uri + sizeof(prefix) - 1;
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || value == 0 || value > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_device_id = (fabric_manager_device_id_t)value;
    *out_tail = end;
    return ESP_OK;
}

static esp_err_t send_json_owned(httpd_req_t *req, char *json)
{
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    const esp_err_t error = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return error;
}

static esp_err_t handle_list(httpd_req_t *req, fabric_manager_handle_t manager)
{
    char *json = NULL;
    esp_err_t error = fabric_manager_list_devices_json(manager, &json);
    if (error != ESP_OK) return error;
    return send_json_owned(req, json);
}

static esp_err_t handle_detail(httpd_req_t *req, fabric_manager_handle_t manager,
                               fabric_manager_device_id_t device_id)
{
    char *json = NULL;
    esp_err_t error = fabric_manager_get_device_json(manager, device_id, &json);
    if (error != ESP_OK) return error;
    return send_json_owned(req, json);
}

static esp_err_t devices_list_handler(httpd_req_t *req)
{
    fabric_manager_handle_t manager = NULL;
    return binding_get(NULL, &manager) ? handle_list(req, manager)
                                  : send_error(req, ESP_ERR_NOT_SUPPORTED);
}

static esp_err_t device_get_handler(httpd_req_t *req)
{
    fabric_manager_handle_t manager = NULL;
    if (!binding_get(NULL, &manager)) return send_error(req, ESP_ERR_NOT_SUPPORTED);
    fabric_manager_device_id_t device_id = 0;
    const char *tail = NULL;
    esp_err_t error = uri_device_id(req, &tail, &device_id);
    if (error != ESP_OK || (*tail && *tail != '?')) {
        return send_error(req, ESP_ERR_INVALID_ARG);
    }
    error = handle_detail(req, manager, device_id);
    return error == ESP_OK ? ESP_OK : send_error(req, error);
}

static matter_operation_t *operation_reserve(void)
{
    matter_operation_t *slot = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < MATTER_OPERATION_MAX; ++i) {
        if (!s_operations[i].allocated) {
            slot = &s_operations[i];
            break;
        }
        if (!s_operations[i].active &&
            (!slot || s_operations[i].sequence < slot->sequence)) {
            slot = &s_operations[i];
        }
    }
    if (slot) {
        *slot = (matter_operation_t){
            .allocated = true,
            .active = true,
            .result = ESP_OK,
            .sequence = ++s_operation_sequence,
        };
    }
    xSemaphoreGive(s_lock);
    return slot;
}

static void operation_submit(matter_operation_t *operation,
                             fabric_core_request_id_t request_id,
                             esp_err_t error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (error == ESP_OK) {
        operation->request_id = request_id;
    } else {
        operation->allocated = false;
        operation->active = false;
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t operation_uri_id(httpd_req_t *req,
                                  fabric_core_request_id_t *out_request_id)
{
    static const char prefix[] = "/api/matter/operations/";
    if (strncmp(req->uri, prefix, sizeof(prefix) - 1) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *text = req->uri + sizeof(prefix) - 1;
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || value == 0 || value > UINT32_MAX ||
        (*end && *end != '?')) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_request_id = (fabric_core_request_id_t)value;
    return ESP_OK;
}

static esp_err_t operation_status_handler(httpd_req_t *req)
{
    if (!binding_get(NULL, NULL)) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    fabric_core_request_id_t request_id = 0;
    esp_err_t error = operation_uri_id(req, &request_id);
    if (error != ESP_OK) {
        return send_error(req, error);
    }

    matter_operation_t snapshot = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < MATTER_OPERATION_MAX; ++i) {
        if (s_operations[i].allocated &&
            s_operations[i].request_id == request_id) {
            snapshot = s_operations[i];
            break;
        }
    }
    xSemaphoreGive(s_lock);
    if (!snapshot.allocated) {
        return send_error(req, ESP_ERR_NOT_FOUND);
    }

    cJSON *response = cJSON_CreateObject();
    if (!response ||
        !cJSON_AddBoolToObject(response, "active", snapshot.active) ||
        !cJSON_AddNumberToObject(response, "request_id", request_id) ||
        !cJSON_AddStringToObject(response, "result",
                                 esp_err_to_name(snapshot.result))) {
        cJSON_Delete(response);
        return send_error(req, ESP_ERR_NO_MEM);
    }
    return send_json(req, response);
}

static void operation_complete(fabric_core_handle_t core,
                               const fabric_core_request_result_t *result,
                               void *user_ctx)
{
    (void)core;
    matter_operation_t *operation = user_ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (operation->allocated && operation->active &&
        (operation->request_id == 0 ||
         operation->request_id == result->request_id)) {
        operation->request_id = result->request_id;
        operation->active = false;
        operation->result = result->error;
    }
    xSemaphoreGive(s_lock);
    if (result->error != ESP_OK) {
        ESP_LOGW(TAG, "Operation %" PRIu32 " failed: %s", result->request_id,
                 esp_err_to_name(result->error));
    }
}

static bool uri_tail_is(const char *tail, const char *path)
{
    const size_t length = strlen(path);
    return !strncmp(tail, path, length) &&
           (tail[length] == '\0' || tail[length] == '?');
}

static esp_err_t set_device_identity(fabric_manager_handle_t manager,
                                     fabric_manager_device_id_t device_id,
                                     const cJSON *body)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(body, "name");
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(body, "room");
    return cJSON_IsString(name) && cJSON_IsString(room)
        ? fabric_manager_set_device_identity(manager, device_id, name->valuestring,
                                      room->valuestring)
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t invoke_device_action(fabric_manager_handle_t manager,
                                      fabric_manager_device_id_t device_id,
                                      const cJSON *body,
                                      fabric_core_request_id_t *out_request_id)
{
    const cJSON *endpoint =
        cJSON_GetObjectItemCaseSensitive(body, "endpoint");
    const cJSON *cluster = cJSON_GetObjectItemCaseSensitive(body, "cluster");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(body, "action");
    const cJSON *arguments =
        cJSON_GetObjectItemCaseSensitive(body, "arguments");
    const bool valid_endpoint =
        cJSON_IsNumber(endpoint) && endpoint->valuedouble >= 1 &&
        endpoint->valuedouble <= UINT16_MAX &&
        floor(endpoint->valuedouble) == endpoint->valuedouble;
    if (!valid_endpoint || !cJSON_IsString(cluster) ||
        !cJSON_IsString(action) || !cJSON_IsObject(arguments)) {
        return ESP_ERR_INVALID_ARG;
    }
    char *arguments_json = cJSON_PrintUnformatted(arguments);
    if (!arguments_json) {
        return ESP_ERR_NO_MEM;
    }
    matter_operation_t *operation = operation_reserve();
    esp_err_t error = ESP_ERR_NO_MEM;
    if (operation) {
        error = fabric_manager_invoke_action(
            manager, device_id, (uint16_t)endpoint->valuedouble,
            cluster->valuestring, action->valuestring, arguments_json,
            out_request_id, operation_complete, operation);
        operation_submit(operation, *out_request_id, error);
    }
    cJSON_free(arguments_json);
    return error;
}

static esp_err_t device_post_handler(httpd_req_t *req)
{
    fabric_manager_handle_t manager = NULL;
    if (!binding_get(NULL, &manager)) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    fabric_manager_device_id_t device_id = 0;
    const char *tail = NULL;
    esp_err_t error = uri_device_id(req, &tail, &device_id);
    if (error != ESP_OK) {
        return send_error(req, error);
    }
    cJSON *body = receive_json(req);
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_error(req, ESP_ERR_INVALID_ARG);
    }

    fabric_core_request_id_t request_id = 0;
    const bool asynchronous = uri_tail_is(tail, "/actions");
    if (uri_tail_is(tail, "/identity")) {
        error = set_device_identity(manager, device_id, body);
    } else if (asynchronous) {
        error = invoke_device_action(manager, device_id, body, &request_id);
    } else {
        error = ESP_ERR_NOT_FOUND;
    }
    cJSON_Delete(body);
    if (error != ESP_OK) {
        return send_error(req, error);
    }
    cJSON *response = cJSON_CreateObject();
    if (!response || !cJSON_AddBoolToObject(response, "ok", true) ||
        (asynchronous &&
         !cJSON_AddNumberToObject(response, "request_id", request_id))) {
        cJSON_Delete(response);
        return send_error(req, ESP_ERR_NO_MEM);
    }
    return send_json(req, response);
}

static esp_err_t device_delete_handler(httpd_req_t *req)
{
    fabric_manager_handle_t manager = NULL;
    if (!binding_get(NULL, &manager)) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    fabric_manager_device_id_t device_id = 0;
    const char *tail = NULL;
    esp_err_t error = uri_device_id(req, &tail, &device_id);
    if (error != ESP_OK || (*tail && *tail != '?')) {
        return send_error(req, ESP_ERR_INVALID_ARG);
    }
    matter_operation_t *operation = operation_reserve();
    if (!operation) {
        return send_error(req, ESP_ERR_NO_MEM);
    }
    fabric_core_request_id_t request_id = 0;
    error = fabric_manager_remove_device(manager, device_id, false, &request_id,
                                  operation_complete, operation);
    operation_submit(operation, request_id, error);
    if (error != ESP_OK) {
        return send_error(req, error);
    }
    cJSON *response = cJSON_CreateObject();
    if (!response || !cJSON_AddBoolToObject(response, "ok", true) ||
            !cJSON_AddNumberToObject(response, "request_id", request_id)) {
        cJSON_Delete(response);
        return send_error(req, ESP_ERR_NO_MEM);
    }
    return send_json(req, response);
}

static void commission_complete(fabric_core_handle_t core,
                                const fabric_core_request_result_t *result,
                                void *user_ctx)
{
    (void)core;
    (void)user_ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_commission.active &&
        (s_commission.request_id == 0 ||
         s_commission.request_id == result->request_id)) {
        s_commission.request_id = result->request_id;
        s_commission.active = false;
        s_commission.result = result->error;
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t commission_start_handler(httpd_req_t *req)
{
    fabric_core_handle_t core = NULL;
    if (!binding_get(&core, NULL)) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    cJSON *body = receive_json(req);
    const cJSON *code = body
        ? cJSON_GetObjectItemCaseSensitive(body, "setup_code") : NULL;
    if (!cJSON_IsString(code) || !code->valuestring[0]) {
        cJSON_Delete(body);
        return send_error(req, ESP_ERR_INVALID_ARG);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_commission.active) {
        xSemaphoreGive(s_lock);
        cJSON_Delete(body);
        return send_error(req, ESP_ERR_INVALID_STATE);
    }
    s_commission.active = true;
    s_commission.request_id = 0;
    s_commission.result = ESP_OK;
    xSemaphoreGive(s_lock);

    fabric_core_request_id_t request_id = 0;
    esp_err_t error = fabric_core_start_commissioning(
        core, code->valuestring, &request_id, commission_complete, NULL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (error == ESP_OK) {
        s_commission.request_id = request_id;
    } else {
        s_commission.active = false;
        s_commission.result = error;
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(body);
    if (error != ESP_OK) {
        return send_error(req, error);
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "request_id", request_id);
    return send_json(req, response);
}

static esp_err_t commission_status_handler(httpd_req_t *req)
{
    if (!binding_get(NULL, NULL)) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = s_commission.active;
    const fabric_core_request_id_t request_id = s_commission.request_id;
    const esp_err_t result = s_commission.result;
    xSemaphoreGive(s_lock);
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "active", active);
    cJSON_AddNumberToObject(response, "request_id", request_id);
    cJSON_AddStringToObject(response, "result", esp_err_to_name(result));
    return send_json(req, response);
}

static esp_err_t commission_cancel_handler(httpd_req_t *req)
{
    fabric_core_handle_t core = NULL;
    if (!binding_get(&core, NULL)) {
        return send_error(req, ESP_ERR_NOT_SUPPORTED);
    }
    const esp_err_t error = fabric_core_cancel_commissioning(core);
    if (error != ESP_OK) {
        return send_error(req, error);
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    return send_json(req, response);
}

esp_err_t http_server_matter_set_local_fabric(fabric_core_handle_t core,
                                            fabric_manager_handle_t manager)
{
    if ((core == NULL) != (manager == NULL)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = ensure_matter_lock();
    if (error != ESP_OK) return error;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_binding_changing) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (core == s_core && manager == s_manager) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    bool busy = s_commission.active;
    for (size_t i = 0; i < MATTER_OPERATION_MAX; ++i) {
        busy |= s_operations[i].active;
    }
    if (busy) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_binding_changing = true;
    const fabric_core_handle_t old_core = s_core;
    const fabric_manager_handle_t old_manager = s_manager;
    const fabric_manager_listener_id_t old_id = s_manager_listener_id;
    xSemaphoreGive(s_lock);

    // Removal may wait for a callback that needs s_lock. Never hold it here.
    if (old_id) {
        error = fabric_manager_remove_listener(old_manager, old_id);
        if (error != ESP_OK) goto done;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_core = NULL;
    s_manager = NULL;
    s_manager_listener_id = 0;
    xSemaphoreGive(s_lock);

    const fabric_manager_listener_t listener = {
        .device_set_changed = manager_device_set_changed,
        .properties_changed = manager_properties_changed,
    };
    fabric_manager_listener_id_t listener_id = 0;
    if (manager) {
        error = fabric_manager_add_listener(manager, &listener, &listener_id);
        if (error != ESP_OK && old_id) {
            const esp_err_t restore_error =
                fabric_manager_add_listener(old_manager, &listener, &listener_id);
            if (restore_error == ESP_OK) {
                core = old_core;
                manager = old_manager;
            } else {
                ESP_LOGE(TAG, "Restore Matter HTTP binding: %s",
                         esp_err_to_name(restore_error));
                listener_id = 0;
            }
        }
    }
    if (listener_id) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_core = core;
        s_manager = manager;
        s_manager_listener_id = listener_id;
        xSemaphoreGive(s_lock);
    }
done:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_binding_changing = false;
    // A retry or rollback may have skipped changes while callbacks were gated.
    s_sse_queue_head = 0;
    s_sse_queue_count = 0;
    s_sse_resync_pending = sse_has_clients_locked();
    if (error == ESP_OK) {
        memset(s_operations, 0, sizeof(s_operations));
        memset(&s_commission, 0, sizeof(s_commission));
    }
    sse_schedule_locked();
    xSemaphoreGive(s_lock);
    return error;
}

bool http_server_matter_enabled(void)
{
    return binding_get(NULL, NULL);
}

esp_err_t http_server_register_matter_routes(httpd_handle_t server)
{
    esp_err_t init_error = ensure_matter_lock();
    if (init_error != ESP_OK) return init_error;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_httpd = server;
    xSemaphoreGive(s_lock);
    const httpd_uri_t routes[] = {
        {.uri = "/api/matter/devices", .method = HTTP_GET,
         .handler = devices_list_handler},
        {.uri = "/api/matter/devices/*", .method = HTTP_GET,
         .handler = device_get_handler},
        {.uri = "/api/matter/devices/*", .method = HTTP_POST,
         .handler = device_post_handler},
        {.uri = "/api/matter/devices/*", .method = HTTP_DELETE,
         .handler = device_delete_handler},
        {.uri = "/api/matter/operations/*", .method = HTTP_GET,
         .handler = operation_status_handler},
        {.uri = "/api/matter/commission", .method = HTTP_GET,
         .handler = commission_status_handler},
        {.uri = "/api/matter/commission", .method = HTTP_POST,
         .handler = commission_start_handler},
        {.uri = "/api/matter/commission/cancel", .method = HTTP_POST,
         .handler = commission_cancel_handler},
        {.uri = "/api/matter/events", .method = HTTP_GET,
         .handler = sse_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t error = httpd_register_uri_handler(server, &routes[i]);
        if (error != ESP_OK) {
            return error;
        }
    }
    if (!s_sse_keepalive) {
        const esp_timer_create_args_t timer_config = {
            .callback = sse_keepalive_callback,
            .name = "matter_sse_ka",
        };
        esp_err_t error = esp_timer_create(&timer_config, &s_sse_keepalive);
        if (error != ESP_OK) {
            return error;
        }
        error = esp_timer_start_periodic(s_sse_keepalive,
                                         MATTER_SSE_KEEPALIVE_US);
        if (error != ESP_OK) {
            esp_timer_delete(s_sse_keepalive);
            s_sse_keepalive = NULL;
            return error;
        }
    }
    return ESP_OK;
}
