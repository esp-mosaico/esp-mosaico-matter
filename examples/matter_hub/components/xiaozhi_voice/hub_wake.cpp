/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hub_wake.h"

#include <stdlib.h>
#include <string.h>

#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_iface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define TAG "xz_wake"
#define WAKE_STACK_BYTES 8192
#define WAKE_READY (1 << 0)
#define WAKE_FAILED (1 << 1)

static const esp_afe_sr_iface_t *s_iface;
static esp_afe_sr_data_t *s_afe;
static srmodel_list_t *s_models;
static hub_wake_cb_t s_cb;
static void *s_ctx;
static EventGroupHandle_t s_events;
static int s_chunk;
static char s_phrase[48];
static volatile bool s_phrase_set;
static volatile bool s_feeding;
static StreamBufferHandle_t s_pcm;
static StaticStreamBuffer_t *s_pcm_ctrl;
static uint8_t *s_pcm_storage;
static uint32_t s_dropped;
static uint32_t s_drop_log_ms;

#define WAKE_RING_BYTES (16000 * sizeof(int16_t))

static bool start_feed_task(void);

static void publish_phrase(const char *phrase)
{
    size_t len = 0;
    if (phrase != nullptr) {
        len = strnlen(phrase, sizeof(s_phrase) - 1);
        memcpy(s_phrase, phrase, len);
    }
    s_phrase[len] = '\0';
    s_phrase_set = true;
}

static void copy_phrase(srmodel_list_t *models, const char *model_name)
{
    if (models == nullptr || model_name == nullptr) {
        publish_phrase(nullptr);
        return;
    }
    char *words = esp_srmodel_get_wake_words(models, const_cast<char *>(model_name));
    if (words == nullptr) {
        publish_phrase(nullptr);
        return;
    }
    if (words[0] != '\0') {
        size_t len = strcspn(words, ";");
        words[len] = '\0';
    }
    publish_phrase(words);
    free(words);
}

const char *hub_wake_phrase(void)
{
    return s_phrase_set ? s_phrase : "";
}

static char *find_wakenet(srmodel_list_t *models)
{
    if (models == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < models->num; ++i) {
        char *name = models->model_name[i];
        if (name != nullptr && strncmp(name, ESP_WN_PREFIX, strlen(ESP_WN_PREFIX)) == 0) {
            return name;
        }
    }
    return nullptr;
}

static void wake_task(void *arg)
{
    (void)arg;
    char *model_name = find_wakenet(s_models);
    afe_config_t *cfg = nullptr;
    if (model_name == nullptr) {
        ESP_LOGW(TAG, "no WakeNet model in the model partition");
        goto fail;
    }
    copy_phrase(s_models, model_name);

    cfg = afe_config_init("M", s_models, AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (cfg == nullptr) {
        ESP_LOGE(TAG, "afe_config_init failed");
        goto fail;
    }
    cfg->aec_init = false;
    cfg->vad_init = false;
    cfg->ns_init = false;
    cfg->agc_init = false;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = model_name;
    cfg->wakenet_model_name_2 = nullptr;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    /* Below the GSP render task (priority 4). A higher WakeNet priority
     * stretches the panel transfer past the TE frame and the schedule log
     * flips between slow and overrun on every frame. */
    cfg->afe_perferred_priority = 2;

    s_iface = esp_afe_handle_from_config(cfg);
    if (s_iface != nullptr) {
        s_afe = s_iface->create_from_config(cfg);
    }
    afe_config_free(cfg);
    cfg = nullptr;
    if (s_iface == nullptr || s_afe == nullptr) {
        ESP_LOGE(TAG, "WakeNet create failed");
        s_iface = nullptr;
        s_afe = nullptr;
        goto fail;
    }

    s_chunk = s_iface->get_feed_chunksize(s_afe);
    s_iface->print_pipeline(s_afe);
    ESP_LOGI(TAG, "WakeNet ready, phrase=%s, feed=%d", s_phrase, s_chunk);
    xEventGroupSetBits(s_events, WAKE_READY);

    while (true) {
        afe_fetch_result_t *result = s_iface->fetch_with_delay(s_afe, portMAX_DELAY);
        if (result == nullptr || result->wakeup_state != WAKENET_DETECTED) {
            continue;
        }
        ESP_LOGI(TAG, "wake word: %s", s_phrase);
        if (s_cb != nullptr) {
            s_cb(s_phrase, s_ctx);
        }
    }

fail:
    if (cfg != nullptr) {
        afe_config_free(cfg);
    }
    xEventGroupSetBits(s_events, WAKE_FAILED);
    vTaskDeleteWithCaps(nullptr);
}

esp_err_t hub_wake_start(srmodel_list_t *models, hub_wake_cb_t cb, void *ctx)
{
    if (s_events != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (models == nullptr || models->num <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_models = models;
    s_cb = cb;
    s_ctx = ctx;
    s_events = xEventGroupCreate();
    if (s_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_IDF_TARGET_ESP32S31
    const BaseType_t core = 1;
#else
    const BaseType_t core = tskNO_AFFINITY;
#endif
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "xz_wake", WAKE_STACK_BYTES, nullptr, 3, nullptr,
                                        core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "WakeNet task was not created, psram largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WAKE_READY | WAKE_FAILED, pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(15000));
    if ((bits & WAKE_READY) == 0) {
        ESP_LOGW(TAG, "WakeNet did not become ready");
        return ESP_FAIL;
    }
    if (!start_feed_task()) {
        ESP_LOGW(TAG, "WakeNet feed task was not created");
        return ESP_FAIL;
    }
    s_feeding = true;
    return ESP_OK;
}

static void feed_task(void *arg)
{
    (void)arg;
    const size_t chunk_bytes = static_cast<size_t>(s_chunk) * sizeof(int16_t);
    int16_t *chunk = static_cast<int16_t *>(
        heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (chunk == nullptr || s_pcm == nullptr) {
        ESP_LOGE(TAG, "WakeNet feed buffer was not allocated");
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    while (true) {
        size_t got = 0;
        auto *dst = reinterpret_cast<uint8_t *>(chunk);
        while (got < chunk_bytes) {
            got += xStreamBufferReceive(s_pcm, dst + got, chunk_bytes - got, portMAX_DELAY);
        }
        s_iface->feed(s_afe, chunk);
    }
}

static bool start_feed_task(void)
{
    if (s_chunk <= 0 || s_iface == nullptr || s_afe == nullptr) {
        return false;
    }
    s_pcm_ctrl = static_cast<StaticStreamBuffer_t *>(
        heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_pcm_storage = static_cast<uint8_t *>(
        heap_caps_malloc(WAKE_RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_pcm_ctrl == nullptr || s_pcm_storage == nullptr) {
        return false;
    }
    s_pcm = xStreamBufferCreateStatic(WAKE_RING_BYTES, sizeof(int16_t), s_pcm_storage, s_pcm_ctrl);
    if (s_pcm == nullptr) {
        return false;
    }
#if CONFIG_IDF_TARGET_ESP32S31
    const BaseType_t core = 1;
#else
    const BaseType_t core = tskNO_AFFINITY;
#endif
    return xTaskCreatePinnedToCoreWithCaps(feed_task, "xz_feed", 4096, nullptr, 3, nullptr, core,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

void hub_wake_feed(const int16_t *pcm, size_t samples)
{
    if (!s_feeding || s_pcm == nullptr || pcm == nullptr || samples == 0) {
        return;
    }
    size_t bytes = samples * sizeof(int16_t);
    size_t space = xStreamBufferSpacesAvailable(s_pcm);
    size_t take = bytes < space ? bytes : space;
    take -= take % sizeof(int16_t);
    if (take > 0) {
        xStreamBufferSend(s_pcm, pcm, take, 0);
    }
    if (take < bytes) {
        s_dropped += static_cast<uint32_t>(bytes - take);
        uint32_t now = esp_log_timestamp();
        if (now - s_drop_log_ms >= 2000) {
            ESP_LOGW(TAG, "wake queue behind, dropped %u bytes", (unsigned)s_dropped);
            s_dropped = 0;
            s_drop_log_ms = now;
        }
    }
}
