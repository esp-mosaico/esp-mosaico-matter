/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "matter_commissioning.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "matter_binds.h"
#include "mosaic_app_catalog.h"
#include "mosaic_camera.h"
#include "mosaic_hub_actions.h"
#include "mosaic_loader.h"
#include "mosaic_runtime.h"

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "quirc.h"
#endif

#define MATTER_QR_WIDTH 240U
#define MATTER_QR_HEIGHT 240U
#define MATTER_QR_PERIOD_US INT64_C(100000)
#define MATTER_QR_LOG_PERIOD_US INT64_C(2000000)
#define MATTER_QR_TASK_STACK 32768U
#define MATTER_QR_TASK_PRIORITY 4U
#define MATTER_SETUP_CODE_MAX 256U

typedef struct {
    fabric_core_handle_t core;
    char progress[96];
    bool scanning;
    bool completion_pending;
    esp_err_t completion_error;
#if defined(ESP_PLATFORM)
    portMUX_TYPE event_lock;
    portMUX_TYPE code_lock;
    struct quirc *qr;
    SemaphoreHandle_t qr_lock;
    SemaphoreHandle_t qr_done;
    TaskHandle_t qr_task;
    bool qr_running;
    bool qr_frame_ready;
    int64_t qr_last_frame_us;
    int64_t qr_last_input_log_us;
    int64_t qr_last_decode_log_us;
    int64_t qr_last_failure_log_us;
    uint32_t qr_input_frames;
    uint32_t qr_decode_frames;
    uint32_t qr_queued_frames;
    char scanned_code[MATTER_SETUP_CODE_MAX];
    bool scanned_code_pending;
#endif
} matter_commissioning_state_t;

static matter_commissioning_state_t s_commission = {
#if defined(ESP_PLATFORM)
    .event_lock = portMUX_INITIALIZER_UNLOCKED,
    .code_lock = portMUX_INITIALIZER_UNLOCKED,
#endif
};

extern const mosaic_app_descriptor_t mosaic_matter_app;

static void set_completion(esp_err_t error)
{
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_commission.event_lock);
#endif
    s_commission.completion_error = error;
    s_commission.completion_pending = true;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_commission.event_lock);
#endif
}

static void matter_commission_complete(
    fabric_core_handle_t core, const fabric_core_request_result_t *result,
    void *user_ctx)
{
    (void)core;
    (void)user_ctx;
    set_completion(result->error);
}

static void matter_qr_log_failure(int64_t now, const char *message,
                                  uint32_t detail)
{
    if (now - s_commission.qr_last_failure_log_us < MATTER_QR_LOG_PERIOD_US) {
        return;
    }
    ESP_LOGW("matter_qr", "%s: detail=%" PRIu32, message, detail);
    s_commission.qr_last_failure_log_us = now;
}

static bool matter_qr_frame_is_valid(const uint8_t *pixels, size_t size,
                                     uint32_t width, uint32_t height)
{
    return s_commission.qr_running && s_commission.qr && s_commission.qr_lock && pixels &&
           width >= MATTER_QR_WIDTH && height >= MATTER_QR_HEIGHT &&
           size >= (size_t)width * height * 2U;
}

static bool matter_qr_take_frame_lock(int64_t now)
{
    if (now - s_commission.qr_last_frame_us < MATTER_QR_PERIOD_US ||
        xSemaphoreTake(s_commission.qr_lock, 0) != pdTRUE) {
        return false;
    }
    if (s_commission.qr_frame_ready) {
        xSemaphoreGive(s_commission.qr_lock);
        return false;
    }
    return true;
}

static void matter_qr_log_input(int64_t now, size_t size, uint32_t width,
                                uint32_t height, uint32_t pixel_format)
{
    ++s_commission.qr_input_frames;
    if (s_commission.qr_input_frames != 1U &&
        now - s_commission.qr_last_input_log_us < MATTER_QR_LOG_PERIOD_US) {
        return;
    }
    ESP_LOGI("matter_qr",
             "input frames=%" PRIu32 " queued=%" PRIu32
             " size=%ux%u bytes=%u format=0x%08x",
             s_commission.qr_input_frames, s_commission.qr_queued_frames,
             (unsigned)width, (unsigned)height, (unsigned)size,
             (unsigned)pixel_format);
    s_commission.qr_last_input_log_us = now;
}

static bool matter_qr_pixel_format(uint32_t pixel_format,
                                   uint32_t *luma_offset)
{
    if (pixel_format == V4L2_PIX_FMT_RGB565) {
        *luma_offset = 0;
        return true;
    }
    if (pixel_format == V4L2_PIX_FMT_UYVY) {
        *luma_offset = 1;
        return true;
    }
    if (pixel_format == V4L2_PIX_FMT_YUYV) {
        *luma_offset = 0;
        return true;
    }
    return false;
}

static uint8_t matter_qr_rgb565_luma(uint16_t pixel)
{
    const uint32_t red = ((pixel >> 11) & 0x1fU) * 255U / 31U;
    const uint32_t green = ((pixel >> 5) & 0x3fU) * 255U / 63U;
    const uint32_t blue = (pixel & 0x1fU) * 255U / 31U;
    return (uint8_t)((red * 77U + green * 150U + blue * 29U) >> 8);
}

static void matter_qr_copy_grayscale(uint8_t *target, const uint8_t *pixels,
                                     uint32_t width, uint32_t height,
                                     uint32_t pixel_format,
                                     uint32_t luma_offset)
{
    const uint32_t source_extent = width < height ? width : height;
    const uint32_t source_x = (width - source_extent) / 2U;
    const uint32_t source_y = (height - source_extent) / 2U;
    for (uint32_t y = 0; y < MATTER_QR_HEIGHT; ++y) {
        const uint32_t input_y = source_y +
            (uint32_t)((uint64_t)y * source_extent / MATTER_QR_HEIGHT);
        uint8_t *row = target + (size_t)y * MATTER_QR_WIDTH;
        for (uint32_t x = 0; x < MATTER_QR_WIDTH; ++x) {
            const uint32_t input_x = source_x +
                (uint32_t)((uint64_t)x * source_extent / MATTER_QR_WIDTH);
            const size_t input_index = (size_t)input_y * width + input_x;
            row[x] = pixel_format == V4L2_PIX_FMT_RGB565
                         ? matter_qr_rgb565_luma(
                               ((const uint16_t *)pixels)[input_index])
                         : pixels[input_index * 2U + luma_offset];
        }
    }
}

static bool matter_qr_fill_input(const uint8_t *pixels, uint32_t width,
                                 uint32_t height, uint32_t pixel_format,
                                 int64_t now)
{
    uint32_t luma_offset = 0;
    if (!matter_qr_pixel_format(pixel_format, &luma_offset)) {
        matter_qr_log_failure(now,
                              "unsupported input pixel format",
                              pixel_format);
        return false;
    }
    int qr_width = 0;
    int qr_height = 0;
    uint8_t *gray = quirc_begin(s_commission.qr, &qr_width, &qr_height);
    if (!gray || qr_width != MATTER_QR_WIDTH ||
        qr_height != MATTER_QR_HEIGHT) {
        matter_qr_log_failure(now,
                              "quirc input buffer unavailable",
                              (uint32_t)qr_width);
        return false;
    }
    matter_qr_copy_grayscale(gray, pixels, width, height, pixel_format,
                             luma_offset);
    return true;
}

static void matter_qr_frame(const uint8_t *pixels, size_t size,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_format, void *user_ctx)
{
    (void)user_ctx;
    if (!matter_qr_frame_is_valid(pixels, size, width, height)) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (!matter_qr_take_frame_lock(now)) {
        return;
    }
    matter_qr_log_input(now, size, width, height, pixel_format);
    if (matter_qr_fill_input(pixels, width, height, pixel_format, now)) {
        s_commission.qr_frame_ready = true;
        s_commission.qr_last_frame_us = now;
        ++s_commission.qr_queued_frames;
        xTaskNotifyGive(s_commission.qr_task);
    }
    xSemaphoreGive(s_commission.qr_lock);
}

static void matter_qr_log_decode(int count, int64_t now)
{
    ++s_commission.qr_decode_frames;
    if (count <= 0 && s_commission.qr_decode_frames != 1U &&
        now - s_commission.qr_last_decode_log_us < MATTER_QR_LOG_PERIOD_US) {
        return;
    }
    ESP_LOGI("matter_qr", "decode frames=%" PRIu32 " candidates=%d",
             s_commission.qr_decode_frames, count);
    s_commission.qr_last_decode_log_us = now;
}

static bool matter_qr_store_candidate(int index, int64_t now)
{
    struct quirc_code code;
    struct quirc_data data;
    quirc_extract(s_commission.qr, index, &code);
    const quirc_decode_error_t error = quirc_decode(&code, &data);
    if (error != QUIRC_SUCCESS) {
        matter_qr_log_failure(now, "candidate decode failed",
                              (uint32_t)index);
        return false;
    }
    if (data.payload_len < 3 || memcmp(data.payload, "MT:", 3) != 0) {
        matter_qr_log_failure(now, "decoded non-Matter QR",
                              (uint32_t)data.payload_len);
        return false;
    }
    ESP_LOGI("matter_qr", "Matter QR decoded: payload length=%d",
             data.payload_len);
    size_t length = data.payload_len;
    if (length >= sizeof(s_commission.scanned_code)) {
        length = sizeof(s_commission.scanned_code) - 1U;
    }
    portENTER_CRITICAL(&s_commission.code_lock);
    memcpy(s_commission.scanned_code, data.payload, length);
    s_commission.scanned_code[length] = '\0';
    s_commission.scanned_code_pending = true;
    portEXIT_CRITICAL(&s_commission.code_lock);
    return true;
}

static void matter_qr_decode_frame(void)
{
    quirc_end(s_commission.qr);
    s_commission.qr_frame_ready = false;
    const int count = quirc_count(s_commission.qr);
    const int64_t now = esp_timer_get_time();
    matter_qr_log_decode(count, now);
    for (int index = 0; index < count; ++index) {
        if (matter_qr_store_candidate(index, now)) {
            break;
        }
    }
}

static void matter_qr_task(void *arg)
{
    (void)arg;
    while (s_commission.qr_running) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (!s_commission.qr_running) {
            break;
        }
        if (xSemaphoreTake(s_commission.qr_lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_commission.qr_frame_ready) {
            matter_qr_decode_frame();
        }
        xSemaphoreGive(s_commission.qr_lock);
    }
    xSemaphoreGive(s_commission.qr_done);
    vTaskDeleteWithCaps(NULL);
}

static void matter_qr_destroy(void)
{
    if (s_commission.qr != NULL) {
        quirc_destroy(s_commission.qr);
        s_commission.qr = NULL;
    }
}

static esp_err_t matter_qr_start(void)
{
    if (s_commission.qr_running) {
        return ESP_OK;
    }
    s_commission.qr = quirc_new();
    if (!s_commission.qr || quirc_resize(s_commission.qr, MATTER_QR_WIDTH,
                                    MATTER_QR_HEIGHT) != 0) {
        matter_qr_destroy();
        return ESP_ERR_NO_MEM;
    }
    s_commission.qr_lock = xSemaphoreCreateMutex();
    s_commission.qr_done = xSemaphoreCreateBinary();
    if (!s_commission.qr_lock || !s_commission.qr_done) {
        if (s_commission.qr_done) vSemaphoreDelete(s_commission.qr_done);
        if (s_commission.qr_lock) vSemaphoreDelete(s_commission.qr_lock);
        matter_qr_destroy();
        s_commission.qr_done = NULL;
        s_commission.qr_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_commission.qr_running = true;
    s_commission.qr_frame_ready = false;
    s_commission.qr_last_frame_us = 0;
    s_commission.qr_last_input_log_us = 0;
    s_commission.qr_last_decode_log_us = 0;
    s_commission.qr_last_failure_log_us = 0;
    s_commission.qr_input_frames = 0;
    s_commission.qr_decode_frames = 0;
    s_commission.qr_queued_frames = 0;
    if (xTaskCreateWithCaps(matter_qr_task, "matter_qr", MATTER_QR_TASK_STACK,
            NULL, MATTER_QR_TASK_PRIORITY, &s_commission.qr_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_commission.qr_running = false;
        vSemaphoreDelete(s_commission.qr_done);
        vSemaphoreDelete(s_commission.qr_lock);
        matter_qr_destroy();
        s_commission.qr_done = NULL;
        s_commission.qr_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err =
        mosaic_camera_set_frame_callback(matter_qr_frame, NULL);
    if (err == ESP_OK) {
        ESP_LOGI("matter_qr", "scanner started: input=%ux%u period=%" PRIi64 " ms",
                 MATTER_QR_WIDTH, MATTER_QR_HEIGHT,
                 MATTER_QR_PERIOD_US / INT64_C(1000));
    } else {
        ESP_LOGE("matter_qr", "register frame callback failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static void matter_qr_stop(void)
{
    (void)mosaic_camera_set_frame_callback(NULL, NULL);
    if (s_commission.qr_running) {
        s_commission.qr_running = false;
        xTaskNotifyGive(s_commission.qr_task);
        (void)xSemaphoreTake(s_commission.qr_done, pdMS_TO_TICKS(2000));
    }
    if (s_commission.qr_done) vSemaphoreDelete(s_commission.qr_done);
    if (s_commission.qr_lock) vSemaphoreDelete(s_commission.qr_lock);
    matter_qr_destroy();
    s_commission.qr_done = NULL;
    s_commission.qr_lock = NULL;
    s_commission.qr_task = NULL;
    s_commission.qr_frame_ready = false;
}

static void matter_camera_start_deferred(void *user_ctx)
{
    (void)user_ctx;
    if (mosaic_loader_app() != &mosaic_matter_app ||
        !s_commission.scanning) {
        return;
    }
    esp_gsp_handle_t ui = mosaic_loader_ui();
    const mosaic_camera_view_t view = {
        .canvas_bind = GSP_BIND_MATTER_SCAN_CANVAS,
        .missing_visible_bind = GSP_BIND_MATTER_CAMERA_MISSING_VISIBLE,
    };
    esp_err_t err = matter_qr_start();
    if (err == ESP_OK) {
        err = mosaic_camera_start(ui, &view);
    }
    if (err != ESP_OK) {
        ESP_LOGE("matter_app", "start scanner failed: %s",
                 esp_err_to_name(err));
        matter_qr_stop();
        (void)esp_gsp_set_visible(ui,
            GSP_BIND_MATTER_CAMERA_MISSING_VISIBLE, true);
    } else {
        ESP_LOGI("matter_qr", "camera scan pipeline started");
    }
}

void matter_commissioning_init(fabric_core_handle_t core)
{
    s_commission.core = core;
}

void matter_commissioning_reset(void)
{
    s_commission.progress[0] = '\0';
    s_commission.scanning = false;
    s_commission.completion_pending = false;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_commission.code_lock);
    s_commission.scanned_code_pending = false;
    s_commission.scanned_code[0] = '\0';
    portEXIT_CRITICAL(&s_commission.code_lock);
#endif
}

void matter_commissioning_start_scan(esp_gsp_handle_t ui)
{
    if (!s_commission.core) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_DEVICE_COUNT,
                         "Matter controller unavailable");
        return;
    }
    s_commission.scanning = true;
    esp_gsp_set_visible(ui, GSP_BIND_MATTER_CAMERA_MISSING_VISIBLE, false);
#if defined(ESP_PLATFORM)
    if (mosaic_loader_defer(matter_camera_start_deferred, NULL) != ESP_OK) {
        s_commission.scanning = false;
        esp_gsp_set_visible(ui, GSP_BIND_MATTER_CAMERA_MISSING_VISIBLE, true);
    }
#endif
}

void matter_commissioning_stop_scan(esp_gsp_handle_t ui)
{
    s_commission.scanning = false;
#if defined(ESP_PLATFORM)
    mosaic_camera_stop(ui);
    matter_qr_stop();
#else
    (void)ui;
#endif
}

void matter_commissioning_begin(esp_gsp_handle_t ui, const char *code)
{
    if (!s_commission.core || !code || !code[0]) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_MANUAL_HINT,
                         "Enter a valid Matter setup code.");
        return;
    }
    if (s_commission.scanning) {
        matter_commissioning_stop_scan(ui);
    }
    strlcpy(s_commission.progress, "Commissioning...",
            sizeof(s_commission.progress));
    esp_gsp_set_text(ui, GSP_BIND_MATTER_PROGRESS_TEXT,
                     s_commission.progress);
    fabric_core_request_id_t request_id = 0;
    const esp_err_t error = fabric_core_start_commissioning(
        s_commission.core, code, &request_id, matter_commission_complete, NULL);
    if (error != ESP_OK) {
        snprintf(s_commission.progress, sizeof(s_commission.progress),
                 "Start failed: %s", esp_err_to_name(error));
        esp_gsp_set_text(ui, GSP_BIND_MATTER_PROGRESS_TEXT,
                         s_commission.progress);
    }
}

bool matter_commissioning_take_scanned_code(char *code, size_t capacity)
{
#if defined(ESP_PLATFORM)
    if (!code || capacity == 0) return false;
    bool scanned = false;
    portENTER_CRITICAL(&s_commission.code_lock);
    if (s_commission.scanned_code_pending) {
        strlcpy(code, s_commission.scanned_code, capacity);
        s_commission.scanned_code_pending = false;
        scanned = true;
    }
    portEXIT_CRITICAL(&s_commission.code_lock);
    return scanned;
#else
    (void)code;
    (void)capacity;
    return false;
#endif
}

bool matter_commissioning_process_events(esp_gsp_handle_t ui)
{
    bool pending = false;
    esp_err_t error = ESP_OK;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_commission.event_lock);
#endif
    pending = s_commission.completion_pending;
    error = s_commission.completion_error;
    s_commission.completion_pending = false;
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_commission.event_lock);
#endif
    if (!pending) return false;
    if (error == ESP_OK) {
        strlcpy(s_commission.progress, "Commissioned",
                sizeof(s_commission.progress));
    } else {
        snprintf(s_commission.progress, sizeof(s_commission.progress),
                 "Pairing failed (%s)", esp_err_to_name(error));
    }
    esp_gsp_set_text(ui, GSP_BIND_MATTER_PROGRESS_TEXT,
                     s_commission.progress);
    return error == ESP_OK;
}
