#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_gsp.h"

/* The UI owner selects one active target; no second camera owner is created. */
void frontend_camera_reset(void);
void frontend_camera_target(esp_gsp_handle_t ui, uint16_t canvas, uint32_t width, uint32_t height);
void frontend_camera_set_privacy(bool enabled);
bool frontend_camera_tick(esp_gsp_handle_t ui);
void frontend_camera_stop(esp_gsp_handle_t ui);
