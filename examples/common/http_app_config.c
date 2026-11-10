/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_app_config.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

/*
 * Field catalogue
 *
 * Every NVS-backed config field is described here along with the group it
 * belongs to. The WebUI addresses fields by `name`; the C side stores them
 * in `app_config_t` at the given offset. Keeping the table in one place
 * lets both the GET (partial read) and POST (partial write) handlers share
 * the same serialisation logic.
 */

#define CONFIG_FIELD(group, field) { \
    #field, (group), \
    offsetof(app_config_t, field), \
    sizeof(((app_config_t *)0)->field) \
}

typedef struct {
    const char *name;
    const char *group;
    size_t offset;
    size_t size;
} config_field_def_t;

static const config_field_def_t CONFIG_FIELDS[] = {
    CONFIG_FIELD("wifi",         wifi_ssid),
    CONFIG_FIELD("wifi",         wifi_password),
    CONFIG_FIELD("wifi",         ap_ssid),
    CONFIG_FIELD("wifi",         ap_password),
    CONFIG_FIELD("wifi",         ap_behavior),

    CONFIG_FIELD("llm",          llm_api_key),
    CONFIG_FIELD("llm",          llm_backend_type),
    CONFIG_FIELD("llm",          llm_model),
    CONFIG_FIELD("llm",          llm_base_url),
    CONFIG_FIELD("llm",          llm_auth_type),
    CONFIG_FIELD("llm",          llm_timeout_ms),
    CONFIG_FIELD("llm",          llm_max_tokens),
    CONFIG_FIELD("llm",          llm_default_image_max_bytes),
    CONFIG_FIELD("llm",          llm_max_tokens_field),
    CONFIG_FIELD("llm",          llm_reasoning_effort),
    CONFIG_FIELD("llm",          llm_supports_tools),
    CONFIG_FIELD("llm",          llm_supports_vision),
    CONFIG_FIELD("llm",          llm_image_remote_url_only),
    CONFIG_FIELD("asr",          asr_provider),
    CONFIG_FIELD("asr",          asr_api_key),
    CONFIG_FIELD("asr",          asr_workspace_id),
    CONFIG_FIELD("asr",          asr_language_hint),
    CONFIG_FIELD("asr",          asr_model),
    CONFIG_FIELD("asr",          asr_endpoint),

    CONFIG_FIELD("im",           qq_app_id),
    CONFIG_FIELD("im",           qq_app_secret),
    CONFIG_FIELD("im",           qq_msg_type),
    CONFIG_FIELD("im",           feishu_app_id),
    CONFIG_FIELD("im",           feishu_app_secret),
    CONFIG_FIELD("im",           tg_bot_token),
    CONFIG_FIELD("im",           wechat_token),
    CONFIG_FIELD("im",           wechat_base_url),
    CONFIG_FIELD("im",           wechat_cdn_base_url),
    CONFIG_FIELD("im",           wechat_account_id),

    CONFIG_FIELD("search",       search_brave_key),
    CONFIG_FIELD("search",       search_tavily_key),
    CONFIG_FIELD("search",       search_http_allowlist),

    CONFIG_FIELD("capabilities", enabled_cap_groups),
    CONFIG_FIELD("capabilities", llm_visible_cap_groups),

    CONFIG_FIELD("skills",       enabled_lua_modules),

    CONFIG_FIELD("time",         time_timezone),
};

static const size_t CONFIG_FIELD_COUNT = sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);

static http_app_config_services_t s_services;

void http_app_config_init(const http_app_config_services_t *services)
{
    s_services = *services;
}

static bool field_is_exposed(const config_field_def_t *field)
{
    if (strcmp(field->group, "wifi") == 0) {
        return strncmp(field->name, "ap_", 3) != 0 || s_services.ap_config_supported;
    }
    if (strcmp(field->group, "time") == 0) return true;
#if CONFIG_HTTP_SERVER_CLAW_ENABLE
    if (strcmp(field->group, "asr") == 0) return s_services.asr_supported;
#if !CONFIG_APP_CLAW_CAP_IM_QQ
    if (strncmp(field->name, "qq_", 3) == 0) return false;
#endif
#if !CONFIG_APP_CLAW_CAP_IM_FEISHU
    if (strncmp(field->name, "feishu_", 7) == 0) return false;
#endif
#if !CONFIG_APP_CLAW_CAP_IM_TG
    if (strncmp(field->name, "tg_", 3) == 0) return false;
#endif
#if !CONFIG_APP_CLAW_CAP_IM_WECHAT
    if (strncmp(field->name, "wechat_", 7) == 0) return false;
#endif
#if !CONFIG_APP_CLAW_CAP_WEB_SEARCH
    if (strcmp(field->name, "search_brave_key") == 0 ||
        strcmp(field->name, "search_tavily_key") == 0) return false;
#endif
#if !CONFIG_APP_CLAW_CAP_HTTP_REQUEST
    if (strcmp(field->name, "search_http_allowlist") == 0) return false;
#endif
#if !CONFIG_APP_CLAW_CAP_LUA
    if (strcmp(field->group, "skills") == 0) return false;
#endif
    return true;
#else
    return false;
#endif
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static bool csv_contains(const char *csv, const char *token)
{
    if (!csv || !token) {
        return false;
    }
    size_t token_len = strlen(token);
    const char *cursor = csv;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == ',') {
            cursor++;
        }
        if (!*cursor) {
            break;
        }
        const char *comma = strchr(cursor, ',');
        size_t seg_len = comma ? (size_t)(comma - cursor) : strlen(cursor);
        while (seg_len > 0 && cursor[seg_len - 1] == ' ') {
            seg_len--;
        }
        if (seg_len == token_len && strncmp(cursor, token, token_len) == 0) {
            return true;
        }
        cursor += seg_len;
        if (comma) {
            cursor = comma + 1;
        }
    }
    return false;
}

static bool field_matches_filter(const config_field_def_t *field,
                                 const char *groups_csv,
                                 const char *fields_csv)
{
    if (!groups_csv && !fields_csv) {
        return true;
    }
    if (groups_csv && csv_contains(groups_csv, field->group)) {
        return true;
    }
    if (fields_csv && csv_contains(fields_csv, field->name)) {
        return true;
    }
    return false;
}

static const char *field_value(const app_config_t *config, const config_field_def_t *field)
{
    return ((const char *)config) + field->offset;
}

static char *field_mutable(app_config_t *config, const config_field_def_t *field)
{
    return ((char *)config) + field->offset;
}

static bool is_positive_decimal_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    if (!cursor || !cursor[0]) {
        return false;
    }
    if (cursor[0] == '0') {
        return false;
    }

    while (*cursor) {
        if (!isdigit(*cursor)) {
            return false;
        }
        cursor++;
    }

    return true;
}

static bool is_boolean_string(const char *value)
{
    return value &&
           (strcmp(value, "true") == 0 ||
            strcmp(value, "false") == 0 ||
            strcmp(value, "1") == 0 ||
            strcmp(value, "0") == 0);
}

static bool is_reasoning_effort(const char *value)
{
    return value &&
           (strcmp(value, "none") == 0 ||
            strcmp(value, "low") == 0 ||
            strcmp(value, "medium") == 0 ||
            strcmp(value, "high") == 0 ||
            strcmp(value, "xhigh") == 0 ||
            strcmp(value, "max") == 0);
}

static bool is_sensitive_config_field(const char *name)
{
    static const char *SENSITIVE_FIELDS[] = {
        "wifi_password",
        "ap_password",
        "llm_api_key",
        "asr_api_key",
        "qq_app_secret",
        "feishu_app_secret",
        "tg_bot_token",
        "wechat_token",
        "search_brave_key",
        "search_tavily_key",
        NULL,
    };

    for (const char **cursor = SENSITIVE_FIELDS; *cursor != NULL; cursor++) {
        if (strcmp(name, *cursor) == 0) {
            return true;
        }
    }
    return false;
}

/* Mask secrets for GET responses: reveal prefix/suffix, replace the middle
 * with equal-length asterisks. Empty strings are returned unchanged. */
static void mask_secret_value(const char *input, char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return;
    }
    if (!input) {
        output[0] = '\0';
        return;
    }

    size_t len = strlen(input);
    if (len == 0) {
        output[0] = '\0';
        return;
    }

    if (len <= 2) {
        strlcpy(output, input, output_size);
        return;
    }

    size_t prefix_len;
    size_t suffix_len;
    if (len > 20) {
        prefix_len = 6;
        suffix_len = 5;
    } else if (len > 10) {
        prefix_len = 2;
        suffix_len = 2;
    } else {
        prefix_len = 1;
        suffix_len = 1;
    }

    if (prefix_len + suffix_len >= len) {
        strlcpy(output, input, output_size);
        return;
    }

    size_t masked_len = len;
    if (masked_len + 1 > output_size) {
        strlcpy(output, input, output_size);
        return;
    }

    memcpy(output, input, prefix_len);
    memset(output + prefix_len, '*', len - prefix_len - suffix_len);
    memcpy(output + prefix_len + (len - prefix_len - suffix_len),
           input + len - suffix_len,
           suffix_len);
    output[masked_len] = '\0';
}

static esp_err_t add_meta_field(cJSON *groups, cJSON *fields, const config_field_def_t *field)
{
    cJSON *entry = cJSON_CreateObject();
    if (!entry) return ESP_ERR_NO_MEM;
    if (!cJSON_AddItemToArray(fields, entry)) {
        cJSON_Delete(entry);
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddStringToObject(entry, "name", field->name) ||
        !cJSON_AddStringToObject(entry, "group", field->group)) return ESP_ERR_NO_MEM;
    cJSON *existing;
    cJSON_ArrayForEach(existing, groups) {
        if (strcmp(existing->valuestring, field->group) == 0) return ESP_OK;
    }
    cJSON *name = cJSON_CreateString(field->group);
    if (!name || !cJSON_AddItemToArray(groups, name)) {
        cJSON_Delete(name);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t http_app_config_read(const http_server_config_query_t *query, cJSON **out)
{
    *out = NULL;
    app_config_t *config = calloc(1, sizeof(*config));
    if (!config) return ESP_ERR_NO_MEM;
    esp_err_t err = s_services.load(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *meta = query->include_meta && root ? cJSON_AddObjectToObject(root, "_meta") : NULL;
    cJSON *groups = meta ? cJSON_AddArrayToObject(meta, "groups") : NULL;
    cJSON *fields = meta ? cJSON_AddArrayToObject(meta, "fields") : NULL;
    if (!root || (query->include_meta && (!meta || !groups || !fields))) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    for (size_t i = 0; i < CONFIG_FIELD_COUNT; ++i) {
        const config_field_def_t *field = &CONFIG_FIELDS[i];
        if (!field_is_exposed(field)) continue;
        if (meta && (err = add_meta_field(groups, fields, field)) != ESP_OK) goto done;
        if (!field_matches_filter(field, query->groups_csv, query->fields_csv)) continue;
        const char *value = field_value(config, field);
        char masked[APP_CONFIG_STR_LEN];
        if (is_sensitive_config_field(field->name)) {
            mask_secret_value(value, masked, sizeof(masked));
            value = masked;
        }
        if (!cJSON_AddStringToObject(root, field->name, value)) {
            err = ESP_ERR_NO_MEM;
            goto done;
        }
    }
    *out = root;
    root = NULL;
done:
    free(config);
    cJSON_Delete(root);
    return err;
}

static esp_err_t apply_patch_fields(app_config_t *config, const cJSON *patch,
                                    size_t *count, const char **message)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, patch) {
        const config_field_def_t *field = NULL;
        for (size_t i = 0; i < CONFIG_FIELD_COUNT; ++i) {
            if (strcmp(CONFIG_FIELDS[i].name, item->string) == 0) {
                field = &CONFIG_FIELDS[i];
                break;
            }
        }
        if (!field) continue;
        if (!field_is_exposed(field)) {
            *message = "Configuration field is unavailable in this web interface";
            return ESP_ERR_INVALID_ARG;
        }
        if (!cJSON_IsString(item) || strlen(item->valuestring) >= field->size) {
            *message = "Configuration fields must be strings within their size limits";
            return ESP_ERR_INVALID_ARG;
        }
        if ((strcmp(field->name, "llm_max_tokens") == 0 ||
             strcmp(field->name, "llm_default_image_max_bytes") == 0) &&
            !is_positive_decimal_string(item->valuestring)) {
            *message = "LLM token/image limits must be positive integers";
            return ESP_ERR_INVALID_ARG;
        }
        if ((strcmp(field->name, "llm_supports_tools") == 0 ||
             strcmp(field->name, "llm_supports_vision") == 0 ||
             strcmp(field->name, "llm_image_remote_url_only") == 0) &&
            !is_boolean_string(item->valuestring)) {
            *message = "LLM boolean fields must be true/false";
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(field->name, "llm_reasoning_effort") == 0 &&
            !is_reasoning_effort(item->valuestring)) {
            *message = "llm_reasoning_effort must be none/low/medium/high/xhigh/max";
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(field_mutable(config, field), item->valuestring, field->size);
        ++*count;
    }
    return ESP_OK;
}

esp_err_t http_app_config_patch(const cJSON *patch, cJSON **out, const char **message)
{
    *out = NULL;
    *message = NULL;
    if (!cJSON_IsObject(patch)) {
        *message = "Configuration patch must be an object";
        return ESP_ERR_INVALID_ARG;
    }
    app_config_t *config = calloc(1, sizeof(*config));
    if (!config) return ESP_ERR_NO_MEM;
    esp_err_t err = s_services.load(config);
    size_t count = 0;
    if (err == ESP_OK) err = apply_patch_fields(config, patch, &count, message);
    if (err == ESP_OK && count == 0) {
        *message = "Request did not contain any recognised fields";
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) err = app_config_validate_wifi(config, message);
    cJSON *response = NULL;
    if (err == ESP_OK) {
        response = cJSON_CreateObject();
        if (!response || !cJSON_AddBoolToObject(response, "ok", true) ||
            !cJSON_AddNumberToObject(response, "applied", (double)count) ||
            !cJSON_AddStringToObject(response, "message",
                                    "Saved. Restart the device to apply changes.")) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) err = s_services.save(config);
    free(config);
    if (err != ESP_OK) {
        cJSON_Delete(response);
        return err;
    }
    *out = response;
    return ESP_OK;
}
