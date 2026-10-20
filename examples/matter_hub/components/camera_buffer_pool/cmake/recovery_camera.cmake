camera_pool_replace([====[#include "camera_buffer_pool.h"]====] [====[#include "camera_buffer_pool.h"
#include "mosaico_camera_recovery.h"
#include <stdatomic.h>]====] 1)
camera_pool_replace([====[    bool video_initialized;]====] [====[    bool video_initialized;
    _Atomic(uint32_t) module_epoch;
    uint32_t initialized_epoch;]====] 1)
camera_pool_replace([====[    camera->config = active;]====] [====[    camera->config = active;
    atomic_init(&camera->module_epoch, 1);
    camera->initialized_epoch = 1;]====] 1)
camera_pool_replace([====[static esp_err_t open_video_device(mosaico_camera_handle_t camera)]====] [====[void mosaico_camera_notify_hotplug(mosaico_camera_handle_t camera, bool available)
{
    if (!camera) return;
    uint32_t previous = atomic_load_explicit(&camera->module_epoch, memory_order_relaxed);
    uint32_t next;
    do {
        next = ((previous + 2U) & ~UINT32_C(1)) | (available ? 1U : 0U);
    } while (!atomic_compare_exchange_weak_explicit(
        &camera->module_epoch, &previous, next, memory_order_release, memory_order_relaxed));
}

static esp_err_t camera_refresh_after_hotplug(mosaico_camera_handle_t camera)
{
    uint32_t epoch = atomic_load_explicit(&camera->module_epoch, memory_order_acquire);
    if (!(epoch & 1U)) return ESP_ERR_NOT_FOUND;
    if (epoch == camera->initialized_epoch) return ESP_OK;
    if (count_outstanding_buffers(camera)) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Camera reinserted; reinitializing sensor and DVP");
    ESP_RETURN_ON_ERROR(close_video_device(camera), TAG, "close stale camera failed");
    if (camera->video_initialized) {
        ESP_RETURN_ON_ERROR(esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP),
                            TAG, "deinitialize stale sensor failed");
        camera->video_initialized = false;
    }
    if (camera->hardware.xclk_stabilization_ms) {
        vTaskDelay(pdMS_TO_TICKS(camera->hardware.xclk_stabilization_ms));
    }
    ESP_RETURN_ON_ERROR(initialize_video_device(camera), TAG, "reinitialize sensor failed");
    if (atomic_load_explicit(&camera->module_epoch, memory_order_acquire) != epoch) {
        return ESP_ERR_INVALID_STATE;
    }
    camera->initialized_epoch = epoch;
    return ESP_OK;
}

static esp_err_t open_video_device(mosaico_camera_handle_t camera)]====] 1)
camera_pool_replace([====[    ESP_RETURN_ON_FALSE(camera->video_initialized, ESP_ERR_INVALID_STATE, TAG, "camera video device is not initialized");]====] [====[    ESP_RETURN_ON_ERROR(camera_refresh_after_hotplug(camera), TAG, "camera recovery pending");
    ESP_RETURN_ON_FALSE(camera->video_initialized, ESP_ERR_INVALID_STATE, TAG, "camera video device is not initialized");]====] 1)
camera_pool_replace([====[    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->streaming) {]====] [====[    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (atomic_load_explicit(&camera->module_epoch, memory_order_acquire) != camera->initialized_epoch) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!camera->streaming) {]====] 1)
