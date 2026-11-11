/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "app_asr.h"

#include <string.h>

#include "audio_hub.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mosaico_ui.h"

static const char *TAG = "app_asr";
static EXT_RAM_BSS_ATTR asr_service_handle_t s_asr;
static bool s_reload_pending = true;

static bool asr_config_changed(const app_config_t *previous, const app_config_t *config)
{
    return previous == NULL ||
           strcmp(previous->asr_provider, config->asr_provider) != 0 ||
           strcmp(previous->asr_api_key, config->asr_api_key) != 0 ||
           strcmp(previous->asr_workspace_id, config->asr_workspace_id) != 0 ||
           strcmp(previous->asr_language_hint, config->asr_language_hint) != 0 ||
           strcmp(previous->asr_model, config->asr_model) != 0 ||
           strcmp(previous->asr_endpoint, config->asr_endpoint) != 0;
}

static esp_err_t apply_config(const app_config_t *config)
{
    const bool trial = strcmp(config->asr_provider, "trial") == 0;
    ai_create_voice_status_t status = {
        .readiness = AI_CREATE_VOICE_READINESS_DISABLED,
        .error = ESP_ERR_NOT_SUPPORTED,
    };
    asr_service_handle_t replacement = NULL;
    audio_capture_handle_t capture = NULL;

    if (config->asr_provider[0] && !trial &&
            strcmp(config->asr_provider, "qwen") != 0) {
        status.readiness = AI_CREATE_VOICE_READINESS_PROVIDER_ERROR;
    } else if (!trial && !config->asr_api_key[0]) {
        /* An unconfigured provider leaves the UI available for text input. */
    } else if (audio_hub_get_capture(&capture) != ESP_OK || !capture) {
        status.readiness = AI_CREATE_VOICE_READINESS_AUDIO_ERROR;
        status.error = ESP_ERR_INVALID_STATE;
    } else {
        const asr_service_config_t asr_config = {
            .capture = capture,
            .provider = trial ? ASR_SERVICE_PROVIDER_TRIAL : ASR_SERVICE_PROVIDER_QWEN,
            .api_key = config->asr_api_key,
            .workspace_id = config->asr_workspace_id[0] ? config->asr_workspace_id : NULL,
            /* Let the service choose provider-specific defaults. */
            .endpoint = config->asr_endpoint[0] ? config->asr_endpoint : NULL,
            .model = config->asr_model[0] ? config->asr_model : NULL,
            .language_hint = config->asr_language_hint[0] ? config->asr_language_hint : NULL,
            .connect_timeout_ms = 30000,
            .send_timeout_ms = 3000,
        };
        status.error = asr_service_create(&asr_config, &replacement);
        if (status.error != ESP_OK) {
            ESP_LOGW(TAG, "ASR service unavailable: %s", esp_err_to_name(status.error));
            status.readiness = AI_CREATE_VOICE_READINESS_PROVIDER_ERROR;
            status.retryable = true;
        }
    }

    esp_err_t err = replacement
                        ? mosaico_ui_set_ai_create_asr(replacement)
                        : mosaico_ui_set_ai_create_voice_status(status);
    if (err != ESP_OK) {
        /* The UI may still own the previous instance, for example while
         * recording. Keep it alive until a later configuration retry. */
        if (replacement) asr_service_delete(replacement);
        return err;
    }

    asr_service_handle_t previous = s_asr;
    s_asr = replacement;
    if (previous) asr_service_delete(previous);
    ESP_LOGI(TAG, "ASR runtime configuration applied: enabled=%d provider=%s",
             s_asr != NULL, config->asr_provider[0] ? config->asr_provider : "qwen");
    return status.retryable ? status.error : ESP_OK;
}

esp_err_t app_asr_reload(const app_config_t *previous, const app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "ASR config is NULL");
    if (!s_reload_pending && !asr_config_changed(previous, config)) {
        return ESP_OK;
    }

    esp_err_t err = apply_config(config);
    s_reload_pending = err != ESP_OK;
    return err;
}
