#include "frontend_camera.h"
#include "mosaico_matter_camera.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

enum { FRAME_FREE, FRAME_WRITING, FRAME_READY, FRAME_IN_FLIGHT };
enum { PREVIEW_BUFFER_COUNT = 2 };
static const char *TAG = "home_camera";
typedef struct { uint16_t *pixels; unsigned state; uint32_t sequence; } frame_t;
static struct {
    portMUX_TYPE lock;
    frame_t frames[PREVIEW_BUFFER_COUNT];
    ppa_client_handle_t ppa;
    uint16_t canvas;
    uint32_t width, height, sequence;
    uint32_t published_frames;
    int64_t last_frame_us, subscription_started_us, retry_us, allocation_retry_us;
    int64_t last_publish_us;
    bool subscribed, stop_pending;
} s_camera = {.lock = portMUX_INITIALIZER_UNLOCKED};

static uint8_t clamp_color(int value) { return value < 0 ? 0 : value > 255 ? 255 : value; }

static uint32_t align_even(uint32_t value) { return value & ~1U; }

static void convert_software(const uint8_t *source, uint32_t width, uint32_t height,
                             uint32_t stride, uint32_t crop_w, uint32_t crop_h,
                             uint32_t offset_x, uint32_t offset_y,
                             uint32_t out_w, uint32_t out_h, uint16_t *out)
{
    uint32_t source_rows[480];
    for (uint32_t x = 0; x < out_w; ++x)
        source_rows[x] = offset_x + (uint64_t)x * crop_w / out_w;
    for (uint32_t y = 0; y < out_h; ++y) {
        uint32_t source_x = width - 1 - (offset_y + (uint64_t)y * crop_h / out_h);
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint8_t *pair = source + (size_t)source_rows[x] * stride + (source_x & ~1U) * 2;
            int u = (int)pair[0] - 128, v = (int)pair[2] - 128;
            int luma = (int)pair[(source_x & 1U) ? 3 : 1] - 16;
            int r = clamp_color((298 * luma + 409 * v + 128) >> 8);
            int g = clamp_color((298 * luma - 100 * u - 208 * v + 128) >> 8);
            int b = clamp_color((298 * luma + 516 * u + 128) >> 8);
            out[(size_t)y * out_w + x] = (uint16_t)(((r & 248) << 8) | ((g & 252) << 3) | (b >> 3));
        }
    }
}

/* Match esp-mosaico-claw mosaic_camera.c camera_convert_frame():
 * even-aligned UYVY crop, 90-degree CCW rotation, and hardware PPA SRM when
 * the scale ratio is an exact 1/16 step. */
static bool convert_ppa(ppa_client_handle_t ppa, const uint8_t *source, size_t bytes,
                        uint32_t width, uint32_t height, uint32_t stride,
                        uint32_t out_w, uint32_t out_h, uint16_t *out)
{
    if (!ppa || !width || !height || !out_w || !out_h) return false;
    uint32_t bytes_per_line = width * 2U;
    if (stride >= width * 2U && (stride % 2U) == 0U) {
        bytes_per_line = stride;
    }
    if ((uint64_t)bytes_per_line * (uint64_t)height > bytes) return false;

    /* A 16:15 center crop rotates into a 450x480 window. The supported HD
     * and VGA inputs use exact PPA scales of 0.625 and 0.9375 respectively. */
    if (out_w == 480U && out_h == 480U) {
        uint32_t src_w = 0, src_h = 0;
        if (width * 15U >= height * 16U) {
            src_h = align_even(height);
            src_w = align_even((src_h * 16U) / 15U);
            if (src_w > width) src_w = align_even(width);
        } else {
            src_w = align_even(width);
            src_h = align_even((src_w * 15U) / 16U);
            if (src_h > height) src_h = align_even(height);
        }
        if (!src_w || !src_h) return false;
        const uint32_t source_x = align_even((width - src_w) / 2U);
        const uint32_t source_y = (height - src_h) / 2U;
        const float scale = (float)out_h / (float)src_w;
        const uint32_t dst_w = 450U;
        const uint32_t dst_x = (out_w - dst_w) / 2U;
        const ppa_srm_oper_config_t oper = {
            .in = {
                .buffer = source,
                .pic_w = bytes_per_line / 2U,
                .pic_h = height,
                .block_w = src_w,
                .block_h = src_h,
                .block_offset_x = source_x,
                .block_offset_y = source_y,
                .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
            .out = {
                .buffer = out,
                .buffer_size = 480U * 480U * sizeof(uint16_t),
                .pic_w = out_w,
                .pic_h = out_h,
                .block_offset_x = dst_x,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
            .scale_x = scale,
            .scale_y = scale,
            .mirror_x = false,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(ppa, &oper) != ESP_OK) return false;
        const size_t left_bytes = dst_x * sizeof(uint16_t);
        const size_t right_offset = (dst_x + dst_w) * sizeof(uint16_t);
        const size_t stride_bytes = out_w * sizeof(uint16_t);
        const size_t right_bytes = stride_bytes - right_offset;
        for (uint32_t y = 0; y < out_h; ++y) {
            uint8_t *row = (uint8_t *)out + (size_t)y * stride_bytes;
            memset(row, 0, left_bytes);
            memset(row + right_offset, 0, right_bytes);
        }
        return true;
    }

    /* For smaller widgets (e.g. 454x147 Home card), 1:1 center crop with PPA. */
    if (out_w <= height && out_h <= width) {
        const uint32_t src_w = align_even(out_h);
        const uint32_t src_h = align_even(out_w);
        if (src_w == out_h && src_h == out_w) {
            const uint32_t source_x = align_even((width - src_w) / 2U);
            const uint32_t source_y = (height - src_h) / 2U;
            const ppa_srm_oper_config_t oper = {
                .in = {
                    .buffer = source,
                    .pic_w = bytes_per_line / 2U,
                    .pic_h = height,
                    .block_w = src_w,
                    .block_h = src_h,
                    .block_offset_x = source_x,
                    .block_offset_y = source_y,
                    .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
                    .yuv_range = PPA_COLOR_RANGE_LIMIT,
                    .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                },
                .out = {
                    .buffer = out,
                    .buffer_size = 480U * 480U * sizeof(uint16_t),
                    .pic_w = out_w,
                    .pic_h = out_h,
                    .block_offset_x = 0,
                    .block_offset_y = 0,
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                },
                .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
                .scale_x = 1.0f,
                .scale_y = 1.0f,
                .mirror_x = false,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            if (ppa_do_scale_rotate_mirror(ppa, &oper) == ESP_OK) return true;
        }
    }
    return false;
}

static void capture(const uint8_t *source, size_t bytes, uint32_t width,
                    uint32_t height, uint32_t stride, void *unused)
{
    (void)unused;
    if (!source || width < 2 || height < 2 || stride < width * 2 ||
        bytes < (size_t)stride * height) return;
    unsigned slot = PREVIEW_BUFFER_COUNT;
    uint32_t out_w, out_h;
    ppa_client_handle_t ppa;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_camera.lock);
    out_w = s_camera.width; out_h = s_camera.height;
    ppa = s_camera.ppa;
    if (!out_w || !out_h || (s_camera.last_frame_us && now - s_camera.last_frame_us < INT64_C(40000))) {
        portEXIT_CRITICAL(&s_camera.lock);
        return;
    }
    /* Match esp-mosaico-claw frame_acquire_for_write(): only take FREE slots. */
    for (unsigned i = 0; i < PREVIEW_BUFFER_COUNT; ++i) {
        if (s_camera.frames[i].pixels && s_camera.frames[i].state == FRAME_FREE) {
            slot = i;
            s_camera.frames[i].state = FRAME_WRITING;
            s_camera.last_frame_us = now;
            break;
        }
    }
    portEXIT_CRITICAL(&s_camera.lock);
    if (slot == PREVIEW_BUFFER_COUNT) return;

    uint16_t *out = s_camera.frames[slot].pixels;
    if (!convert_ppa(ppa, source, bytes, width, height, stride, out_w, out_h, out)) {
        uint32_t crop_w = height, crop_h = width;
        if ((uint64_t)crop_w * out_h > (uint64_t)crop_h * out_w)
            crop_w = (uint32_t)((uint64_t)crop_h * out_w / out_h);
        else crop_h = (uint32_t)((uint64_t)crop_w * out_h / out_w);
        uint32_t offset_x = (height - crop_w) / 2, offset_y = (width - crop_h) / 2;
        convert_software(source, width, height, stride, crop_w, crop_h,
                         offset_x, offset_y, out_w, out_h, out);
    }

    portENTER_CRITICAL(&s_camera.lock);
    s_camera.frames[slot].sequence = ++s_camera.sequence;
    s_camera.frames[slot].state = FRAME_READY;
    s_camera.last_frame_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_camera.lock);
}

static void released(void *context)
{
    unsigned slot = (unsigned)(uintptr_t)context;
    if (slot >= PREVIEW_BUFFER_COUNT) return;
    portENTER_CRITICAL(&s_camera.lock);
    s_camera.frames[slot].state = FRAME_FREE;
    portEXIT_CRITICAL(&s_camera.lock);
}

void frontend_camera_reset(void)
{
    if (s_camera.subscribed) {
        (void)mosaico_matter_camera_preview_stop();
        s_camera.subscribed = false;
    }
    portENTER_CRITICAL(&s_camera.lock);
    for (unsigned i = 0; i < PREVIEW_BUFFER_COUNT; ++i)
        if (s_camera.frames[i].state != FRAME_IN_FLIGHT) s_camera.frames[i].state = FRAME_FREE;
    /* Only the renderer's release callback can return borrowed pixels.
     * Retain a failed stop so the next target change can retry it. */
    if (!s_camera.stop_pending) {
        s_camera.canvas = 0;
        s_camera.width = s_camera.height = 0;
    }
    s_camera.last_frame_us = 0;
    s_camera.last_publish_us = 0;
    s_camera.published_frames = 0;
    s_camera.subscription_started_us = 0;
    s_camera.retry_us = 0;
    portEXIT_CRITICAL(&s_camera.lock);
}

void frontend_camera_stop(esp_gsp_handle_t ui)
{
    if (s_camera.subscribed) {
        (void)mosaico_matter_camera_preview_stop();
        s_camera.subscribed = false;
    }
    if (s_camera.canvas && esp_gsp_canvas_stop(ui, s_camera.canvas) != ESP_GSP_OK) {
        /* Retain the old target and borrowed frame ownership until the render
         * queue accepts stop. Switching targets early can strand buffers. */
        s_camera.stop_pending = true;
        return;
    }
    s_camera.stop_pending = false;
    portENTER_CRITICAL(&s_camera.lock);
    for (unsigned i = 0; i < PREVIEW_BUFFER_COUNT; ++i)
        if (s_camera.frames[i].state != FRAME_IN_FLIGHT) s_camera.frames[i].state = FRAME_FREE;
    s_camera.canvas = 0;
    s_camera.width = s_camera.height = 0;
    s_camera.last_frame_us = 0;
    s_camera.last_publish_us = 0;
    s_camera.published_frames = 0;
    s_camera.subscription_started_us = 0;
    portEXIT_CRITICAL(&s_camera.lock);
}

void frontend_camera_set_privacy(bool enabled)
{
    mosaico_matter_camera_set_privacy_enabled(enabled);
}

void frontend_camera_target(esp_gsp_handle_t ui, uint16_t canvas, uint32_t width, uint32_t height)
{
    if (canvas == s_camera.canvas && width == s_camera.width && height == s_camera.height) return;
    frontend_camera_stop(ui);
    if (s_camera.canvas) return;
    if (!canvas || !width || !height || width > 480 || height > 480) return;
    if (!s_camera.frames[0].pixels && !mosaico_matter_camera_is_ready()) return;
    int64_t now = esp_timer_get_time();
    if (now < s_camera.allocation_retry_us) return;
    if (!s_camera.ppa) {
        const ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        (void)ppa_register_client(&ppa_config, &s_camera.ppa);
    }
    /* One app-lifetime pool: canvas release callbacks may be delayed until the
     * next render, so route changes must never free their borrowed pixels. */
    for (unsigned i = 0; i < PREVIEW_BUFFER_COUNT; ++i) {
        if (!s_camera.frames[i].pixels)
            s_camera.frames[i].pixels = heap_caps_aligned_calloc(64, 480U * 480U,
                sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_camera.frames[i].pixels) {
            ESP_LOGW(TAG, "Preview buffer allocation failed");
            s_camera.allocation_retry_us = now + INT64_C(2000000);
            return;
        }
    }
    portENTER_CRITICAL(&s_camera.lock);
    s_camera.canvas = canvas; s_camera.width = width; s_camera.height = height;
    s_camera.retry_us = 0;
    portEXIT_CRITICAL(&s_camera.lock);
    ESP_LOGI(TAG, "Preview target %u at %ux%u", (unsigned)canvas, (unsigned)width, (unsigned)height);
}

bool frontend_camera_tick(esp_gsp_handle_t ui)
{
    int64_t now = esp_timer_get_time();
    if (s_camera.stop_pending) { frontend_camera_stop(ui); return false; }
    if (!s_camera.canvas) return false;
    if (!s_camera.subscribed && now >= s_camera.retry_us) {
        if (!mosaico_matter_camera_is_ready()) {
            s_camera.retry_us = now + INT64_C(500000);
            return false;
        }
        /* Clear the old stream's timestamp before the new callback can run. */
        portENTER_CRITICAL(&s_camera.lock);
        s_camera.last_frame_us = 0;
        s_camera.last_publish_us = 0;
        s_camera.subscription_started_us = now;
        portEXIT_CRITICAL(&s_camera.lock);
        esp_err_t error = mosaico_matter_camera_preview_start(capture, NULL);
        s_camera.subscribed = error == ESP_OK;
        if (!s_camera.subscribed)
            ESP_LOGW(TAG, "Preview subscribe failed: %s", esp_err_to_name(error));
        s_camera.retry_us = now + INT64_C(1000000);
    }
    unsigned newest = PREVIEW_BUFFER_COUNT;
    int64_t last_frame;
    portENTER_CRITICAL(&s_camera.lock);
    for (unsigned i = 0; i < PREVIEW_BUFFER_COUNT; ++i)
        if (s_camera.frames[i].state == FRAME_READY &&
            (newest == PREVIEW_BUFFER_COUNT || s_camera.frames[i].sequence > s_camera.frames[newest].sequence)) newest = i;
    if (newest != PREVIEW_BUFFER_COUNT) {
        s_camera.frames[newest].state = FRAME_IN_FLIGHT;
        for (unsigned i = 0; i < PREVIEW_BUFFER_COUNT; ++i)
            if (i != newest && s_camera.frames[i].state == FRAME_READY) s_camera.frames[i].state = FRAME_FREE;
    }
    last_frame = s_camera.last_frame_us;
    portEXIT_CRITICAL(&s_camera.lock);
    if (newest != PREVIEW_BUFFER_COUNT) {
        if (esp_gsp_canvas_try_push(ui, s_camera.canvas, s_camera.frames[newest].pixels,
            s_camera.width * 2, released, (void *)(uintptr_t)newest) != ESP_GSP_OK) {
            /* Keep the newest frame ready for the next tick instead of discarding it. */
            portENTER_CRITICAL(&s_camera.lock);
            if (s_camera.frames[newest].state == FRAME_IN_FLIGHT)
                s_camera.frames[newest].state = FRAME_READY;
            portEXIT_CRITICAL(&s_camera.lock);
        } else {
            if (!s_camera.last_publish_us)
                ESP_LOGI(TAG, "First camera frame queued for target %u", (unsigned)s_camera.canvas);
            if (++s_camera.published_frames == 100)
                ESP_LOGI(TAG, "100 camera frames queued for target %u", (unsigned)s_camera.canvas);
            s_camera.last_publish_us = now;
        }
    }
    /* A frozen frame is not LIVE. Re-subscribe after detach/reinsert clears
     * the camera owner's old subscription. */
    int64_t activity = last_frame ? last_frame : s_camera.subscription_started_us;
    if (s_camera.subscribed && activity && now - activity > INT64_C(8000000)) {
        ESP_LOGW(TAG, "No fresh frame; retrying preview subscription");
        (void)mosaico_matter_camera_preview_stop();
        s_camera.subscribed = false;
        s_camera.retry_us = now + INT64_C(1000000);
    }
    return s_camera.last_publish_us && last_frame && now - last_frame < INT64_C(8000000);
}
