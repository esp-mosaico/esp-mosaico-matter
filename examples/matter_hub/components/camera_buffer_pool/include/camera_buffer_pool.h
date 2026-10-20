#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once during startup, before camera users or memory-heavy services run.
 * The reservation lives for the lifetime of the application. Leases are
 * exclusive; owners must stop DMA and return borrowed frames before release. */
esp_err_t camera_buffer_pool_init(void);
esp_err_t camera_buffer_pool_capture_acquire(const void *owner, size_t bytes, size_t count);
esp_err_t camera_buffer_pool_capture_get(const void *owner, size_t index, void **buffer, size_t *capacity);
esp_err_t camera_buffer_pool_capture_release(const void *owner);
esp_err_t camera_buffer_pool_preview_acquire(const void *owner, size_t bytes, void *buffers[2]);
esp_err_t camera_buffer_pool_preview_release(const void *owner);
esp_err_t camera_buffer_pool_discard_acquire(const void *owner, size_t bytes);
esp_err_t camera_buffer_pool_discard_bind(const void *owner, void *controller);
/* Only release after the bound camera controller has been deleted. */
esp_err_t camera_buffer_pool_discard_release(const void *owner);
/* ISR-safe lookup for the active controller; other controllers get NULL. */
void *camera_buffer_pool_discard_buffer(void *controller, size_t *bytes);
bool camera_buffer_pool_is_discard(void *controller, const void *buffer);

#ifdef __cplusplus
}
#endif
