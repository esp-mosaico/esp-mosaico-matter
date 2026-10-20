/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "router_defaults.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

esp_err_t matter_bridge_seed_router_rules(const char *path, const char *defaults,
                                          bool *seeded)
{
    if (!path || !defaults || !seeded) return ESP_ERR_INVALID_ARG;
    *seeded = false;
    struct stat info;
    if (stat(path, &info) == 0) return ESP_OK;
    if (errno != ENOENT) return ESP_FAIL;

    char parent[256], temp[256];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent) ||
        snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp))
        return ESP_ERR_INVALID_SIZE;
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return ESP_ERR_INVALID_ARG;
    *slash = '\0';
    if (mkdir(parent, 0755) != 0 && errno != EEXIST) return ESP_FAIL;

    FILE *source = fopen(defaults, "rb");
    if (!source) return ESP_FAIL;
    FILE *dest = fopen(temp, "wb");
    if (!dest) { fclose(source); return ESP_FAIL; }

    char buffer[512];
    size_t bytes, total = 0;
    bool ok = true;
    while ((bytes = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        total += bytes;
        if (total > 65536 || fwrite(buffer, 1, bytes, dest) != bytes) {
            ok = false;
            break;
        }
    }
    if (ferror(source) || total == 0) ok = false;
    if (fclose(source) != 0) ok = false;
    if (fflush(dest) != 0 || fsync(fileno(dest)) != 0) ok = false;
    if (fclose(dest) != 0) ok = false;
    if (ok && rename(temp, path) == 0) {
        *seeded = true;
        return ESP_OK;
    }
    unlink(temp);
    return ESP_FAIL;
}
