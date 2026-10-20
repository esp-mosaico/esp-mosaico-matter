#include "camera_buffer_pool.h"
#include "sdkconfig.h"
#include "esp_attr.h"

#if CONFIG_CAMERA_BUFFER_POOL_ENABLE
#include <stdint.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_memory;
static void *s_discard;
static void *s_preview[2];
static size_t s_alignment;
static size_t s_capture_capacity;
static size_t s_preview_capacity;
static const void *s_capture_owner;
static const void *s_preview_owner;
static const void *s_discard_owner;
static size_t s_capture_stride;
static size_t s_capture_count;
static size_t s_discard_bytes;
static _Atomic(void *) s_controller;

static size_t align_up(size_t bytes)
{
    return (bytes + s_alignment - 1) & ~(s_alignment - 1);
}

esp_err_t camera_buffer_pool_init(void)
{
    if (s_memory != NULL) {
        return ESP_OK;
    }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    ESP_ERROR_CHECK(esp_cache_get_alignment(caps, &s_alignment));
    if (s_alignment < 64) {
        s_alignment = 64;
    }
    if ((s_alignment & (s_alignment - 1)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_capture_capacity = align_up(CONFIG_CAMERA_BUFFER_POOL_CAPTURE_BYTES);
    s_preview_capacity = align_up(CONFIG_CAMERA_BUFFER_POOL_PREVIEW_BYTES);
    const size_t total = 2 * s_capture_capacity + 2 * s_preview_capacity;
    uint8_t *memory = heap_caps_aligned_alloc(s_alignment, total, caps);
    if (memory == NULL) {
        ESP_LOGE("camera_buffer_pool", "reserve %u bytes failed: free=%u largest=%u",
                 (unsigned)total, (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_largest_free_block(caps));
        return ESP_ERR_NO_MEM;
    }
    s_discard = memory + s_capture_capacity;
    s_preview[0] = memory + 2 * s_capture_capacity;
    s_preview[1] = memory + 2 * s_capture_capacity + s_preview_capacity;
    s_memory = memory;
    ESP_LOGI("camera_buffer_pool", "Reserved %u bytes: capture=%u discard=%u preview=2x%u",
             (unsigned)total, (unsigned)s_capture_capacity,
             (unsigned)s_capture_capacity, (unsigned)s_preview_capacity);
    return ESP_OK;
}

esp_err_t camera_buffer_pool_capture_acquire(const void *owner, size_t bytes, size_t count)
{
    if (owner == NULL || bytes == 0 || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_memory == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes > s_capture_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t stride = align_up(bytes);
    if (count > s_capture_capacity / stride) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (s_capture_owner == NULL) {
        s_capture_owner = owner;
        s_capture_stride = stride;
        s_capture_count = count;
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_capture_get(const void *owner, size_t index, void **buffer, size_t *capacity)
{
    if (owner == NULL || buffer == NULL || capacity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (s_capture_owner == owner && index < s_capture_count) {
        *buffer = s_memory + index * s_capture_stride;
        *capacity = s_capture_stride;
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_capture_release(const void *owner)
{
    if (owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (s_capture_owner != NULL) {
        if (s_capture_owner != owner || atomic_load_explicit(&s_controller, memory_order_acquire) != NULL) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            s_capture_owner = NULL;
            s_capture_count = 0;
            s_capture_stride = 0;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_preview_acquire(const void *owner, size_t bytes, void *buffers[2])
{
    if (owner == NULL || bytes == 0 || buffers == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_memory == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes > s_preview_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (s_preview_owner == NULL) {
        s_preview_owner = owner;
        buffers[0] = s_preview[0];
        buffers[1] = s_preview[1];
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_preview_release(const void *owner)
{
    if (owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (s_preview_owner != NULL && s_preview_owner != owner) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_preview_owner = NULL;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_discard_acquire(const void *owner, size_t bytes)
{
    if (owner == NULL || bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_memory == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes > s_capture_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (s_discard_owner == NULL) {
        s_discard_owner = owner;
        s_discard_bytes = align_up(bytes);
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_discard_bind(const void *owner, void *controller)
{
    if (owner == NULL || controller == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if (s_discard_owner == owner && atomic_load_explicit(&s_controller, memory_order_relaxed) == NULL) {
        atomic_store_explicit(&s_controller, controller, memory_order_release);
        ret = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t camera_buffer_pool_discard_release(const void *owner)
{
    if (owner == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (s_discard_owner != NULL && s_discard_owner != owner) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        atomic_store_explicit(&s_controller, NULL, memory_order_release);
        s_discard_owner = NULL;
        s_discard_bytes = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

void *IRAM_ATTR camera_buffer_pool_discard_buffer(void *controller, size_t *bytes)
{
    if (controller != NULL && atomic_load_explicit(&s_controller, memory_order_acquire) == controller) {
        if (bytes != NULL) {
            *bytes = s_discard_bytes;
        }
        return s_discard;
    }
    if (bytes != NULL) {
        *bytes = 0;
    }
    return NULL;
}

bool IRAM_ATTR camera_buffer_pool_is_discard(void *controller, const void *buffer)
{
    return buffer != NULL && buffer == camera_buffer_pool_discard_buffer(controller, NULL);
}

#else

esp_err_t camera_buffer_pool_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_capture_acquire(const void *owner, size_t bytes, size_t count)
{ (void)owner; (void)bytes; (void)count; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_capture_get(const void *owner, size_t index, void **buffer, size_t *capacity)
{ (void)owner; (void)index; (void)buffer; (void)capacity; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_capture_release(const void *owner)
{ (void)owner; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_preview_acquire(const void *owner, size_t bytes, void *buffers[2])
{ (void)owner; (void)bytes; (void)buffers; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_preview_release(const void *owner)
{ (void)owner; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_discard_acquire(const void *owner, size_t bytes)
{ (void)owner; (void)bytes; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_discard_bind(const void *owner, void *controller)
{ (void)owner; (void)controller; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t camera_buffer_pool_discard_release(const void *owner)
{ (void)owner; return ESP_ERR_NOT_SUPPORTED; }
void *camera_buffer_pool_discard_buffer(void *controller, size_t *bytes)
{ (void)controller; if (bytes != NULL) { *bytes = 0; } return NULL; }
bool camera_buffer_pool_is_discard(void *controller, const void *buffer)
{ (void)controller; (void)buffer; return false; }

#endif
