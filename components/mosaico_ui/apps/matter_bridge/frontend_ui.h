#pragma once
#include "bridge_dev_mngr.h"
#include "esp_gsp.h"
#include "mosaic_runtime.h"

void frontend_ui_init(bridge_dev_mngr_handle_t manager);
void frontend_ui_tick(esp_gsp_handle_t ui, bool visible, bool camera, bool interaction);
bool frontend_ui_event(esp_gsp_handle_t ui, const mosaic_event_t *event);
bool frontend_ui_back(esp_gsp_handle_t ui);
void frontend_ui_stop(esp_gsp_handle_t ui);
