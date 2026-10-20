/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaic_qr_canvas.h"

#include <stdlib.h>

bool mosaic_qr_render_to_canvas(esp_gsp_handle_t ui, uint16_t canvas_bind, const char *payload,
    enum qrcodegen_Ecc ecc, int quiet_modules, int size, uint16_t **pixels_cache)
{
    if (!payload || payload[0] == '\0' || !pixels_cache) {
        return false;
    }

    uint8_t *qr = malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *scratch = malloc(qrcodegen_BUFFER_LEN_MAX);
    if (qr == NULL || scratch == NULL) {
        free(qr);
        free(scratch);
        return false;
    }
    const bool ok = qrcodegen_encodeText(payload, scratch, qr, ecc, qrcodegen_VERSION_MIN,
                                          qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    free(scratch);
    if (!ok) {
        free(qr);
        return false;
    }

    if (*pixels_cache == NULL) {
        *pixels_cache = malloc((size_t)size * (size_t)size * sizeof(uint16_t));
        if (*pixels_cache == NULL) {
            free(qr);
            return false;
        }
    }
    uint16_t *pixels = *pixels_cache;
    for (int i = 0; i < size * size; ++i) {
        pixels[i] = 0xFFFF; /* white */
    }

    const int modules = qrcodegen_getSize(qr);
    const int total_modules = modules + 2 * quiet_modules;
    int scale = size / total_modules;
    if (scale < 1) {
        scale = 1;
    }
    const int used = total_modules * scale;
    const int offset = (size - used) / 2;

    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(qr, x, y)) {
                continue;
            }
            const int px0 = offset + (x + quiet_modules) * scale;
            const int py0 = offset + (y + quiet_modules) * scale;
            for (int dy = 0; dy < scale; ++dy) {
                const int py = py0 + dy;
                if (py < 0 || py >= size) {
                    continue;
                }
                for (int dx = 0; dx < scale; ++dx) {
                    const int px = px0 + dx;
                    if (px < 0 || px >= size) {
                        continue;
                    }
                    pixels[py * size + px] = 0x0000; /* black */
                }
            }
        }
    }
    const esp_gsp_err_t error = esp_gsp_canvas_push(ui, canvas_bind, pixels, size * sizeof(uint16_t), NULL, NULL);
    free(qr);
    return error == ESP_GSP_OK;
}
