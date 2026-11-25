/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "esp_gsp.h"
#include "esp_log.h"
#include "mosaic_app_catalog.h"
#include "mosaic_qr_canvas.h"
#include "mosaic_runtime.h"
#include "bridge_core.h"
#include "bridge_dev_mngr.h"
#include "mosaic_matter_bridge.h"
#include "mosaic_settings.h"
#include "mosaico_module_mgr.h"
#include "frontend_ui.h"
#include "matter_bridge_actions.h"
#include "matter_bridge_binds.h"
#include "matter_bridge_objects.h"
#include "mosaic_hub_actions.h"
#define GSP_ACT_ID_APP_MATTER_BRIDGE GSP_ACT_ID_APP_MATTER
#define QR_CANVAS_SIZE 191
#define QR_QUIET_MODULES 4
#define QR_PAYLOAD_MAX 200
#define MANUAL_CODE_MAX 32
static bridge_core_handle_t s_core;
static bridge_dev_mngr_handle_t s_dev_mngr;
static bool s_commissioned,s_qr_ready,s_qr_on_network;
static esp_err_t s_pairing_log_err=ESP_OK;
static uint16_t *s_qr_pixels;
static const char *TAG="smart_home";
typedef enum { ROOT_HOME,ROOT_BOARD,ROOT_PAIRING } root_view_t;
static root_view_t s_view;
void mosaic_matter_bridge_set_handles(bridge_core_handle_t core,bridge_dev_mngr_handle_t manager)
{s_core=core;s_dev_mngr=manager;}

static void render_pairing(esp_gsp_handle_t ui)
{
    mosaic_settings_network_t network = {0};
    bool on_network = mosaic_settings_get_wifi(&network) == ESP_OK && network.connected;
    if (s_qr_ready && s_qr_on_network == on_network) return;
    s_qr_ready = false;
    gsp_matter_bridge_matter_bridge_qr_canvas_set_visible(ui, false);
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_MANUAL_CODE, "");
    if (!s_core) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Waiting for pairing window");
        return;
    }
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Preparing pairing code");
    esp_err_t error = on_network ? bridge_core_open_on_network_commissioning_window(s_core) :
                                   bridge_core_open_commissioning_window(s_core);
    if (error != ESP_OK) {
        if (s_pairing_log_err != error) {
            ESP_LOGW(TAG, "Open pairing window: %s", esp_err_to_name(error));
            s_pairing_log_err = error;
        }
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT,
                         s_commissioned ? "Already paired; share from your home app" :
                         "Pairing busy; retry after setup finishes");
        return;
    }
    s_pairing_log_err = ESP_OK;
    char payload[QR_PAYLOAD_MAX] = {0};
    error = bridge_core_get_qr_code(s_core, payload, sizeof(payload));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Pairing payload: %s", esp_err_to_name(error));
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Pairing code unavailable; retry");
        return;
    }
    if (!mosaic_qr_render_to_canvas(ui, GSP_BIND_MATTER_BRIDGE_QR_CANVAS, payload, qrcodegen_Ecc_MEDIUM,
            QR_QUIET_MODULES, QR_CANVAS_SIZE, &s_qr_pixels)) {
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT, "Unable to draw pairing code; retry");
        return;
    }
    char manual_code[MANUAL_CODE_MAX] = {0};
    if (bridge_core_get_manual_pairing_code(s_core, manual_code, sizeof(manual_code)) == ESP_OK && manual_code[0]) {
        char text[64];
        snprintf(text, sizeof(text), "Manual code: %s", manual_code);
        esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_MANUAL_CODE, text);
    }
    esp_gsp_set_text(ui, GSP_BIND_MATTER_BRIDGE_PAIRING_HINT,
                     on_network ? "On-network; scan within 180 seconds" :
                                  "Bluetooth; scan within 180 seconds");
    if (gsp_matter_bridge_matter_bridge_qr_canvas_set_visible(ui, true) != ESP_GSP_OK) return;
    s_qr_on_network = on_network;
    s_qr_ready = true;
}

static bool module_board_present(uint8_t board_type)
{
    for (int slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
         slot < MOSAICO_MODULE_MGR_SLOT_COUNT; ++slot) {
        mosaico_module_mgr_info_t info = {0};
        if (mosaico_module_mgr_get_info((mosaico_module_mgr_slot_t)slot, &info) == ESP_OK &&
            info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
            info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
            info.eeprom.board_type == board_type) {
            return true;
        }
    }
    return false;
}


static void render(esp_gsp_handle_t ui)
{
    bool commissioned=false;
    if(s_core&&bridge_core_is_commissioned(s_core,&commissioned)==ESP_OK){
        if(!s_commissioned&&commissioned&&s_view==ROOT_PAIRING)s_view=ROOT_HOME;
        s_commissioned=commissioned;
    }
    bool camera=module_board_present(MOSAICO_BOARD_TYPE_CAMERA);
    bool interaction=module_board_present(MOSAICO_BOARD_TYPE_INTERACT);
    const gsp_component_update_t updates[]={
        {GSP_OBJ_KEY_MATTER_BRIDGE_QR_GROUP,GSP_COMPONENT_PROP_VISIBLE,{.type=GSP_VALUE_BOOL,.data.boolean=s_view==ROOT_PAIRING}},
        {GSP_OBJ_KEY_MATTER_BRIDGE_BOARD_INFO,GSP_COMPONENT_PROP_VISIBLE,{.type=GSP_VALUE_BOOL,.data.boolean=s_view==ROOT_BOARD}},
    };
    (void)esp_gsp_component_set_many(ui,updates,sizeof(updates)/sizeof(updates[0]));
    frontend_ui_tick(ui,s_view==ROOT_HOME,camera,interaction);
    if(s_view==ROOT_BOARD){
        (void)esp_gsp_set_text(ui,GSP_BIND_MATTER_BRIDGE_CAMERA_STATUS,camera?"Connected":"Not Detected");
        (void)esp_gsp_set_text(ui,GSP_BIND_MATTER_BRIDGE_INTERACT_STATUS,interaction?"Connected":"Not Detected");
    }
    if(s_view==ROOT_PAIRING)render_pairing(ui);
}
static void started(esp_gsp_handle_t ui)
{
    s_qr_ready=false;s_qr_on_network=false;s_pairing_log_err=ESP_OK;s_commissioned=false;
    if(s_core)(void)bridge_core_is_commissioned(s_core,&s_commissioned);
    s_view=s_commissioned?ROOT_HOME:ROOT_PAIRING;
    frontend_ui_init(s_dev_mngr);render(ui);
}
static void stopped(esp_gsp_handle_t ui){frontend_ui_stop(ui);}
static bool back(esp_gsp_handle_t ui,int64_t timestamp_us)
{
    (void)timestamp_us;
    if(s_view==ROOT_PAIRING){s_view=ROOT_BOARD;render(ui);return true;}
    if(s_view==ROOT_BOARD){s_view=ROOT_HOME;render(ui);return true;}
    if(frontend_ui_back(ui)){render(ui);return true;}
    return false;
}
static void event(esp_gsp_handle_t ui,const mosaic_event_t *event)
{
    if(event->type==MOSAIC_EVENT_TIMER){render(ui);return;}
    if(s_view==ROOT_HOME&&frontend_ui_event(ui,event)){render(ui);return;}
    if(event->type!=MOSAIC_EVENT_UI_CALL)return;
    uint16_t action=event->data.call.action_id;
    if(action==GSP_ACT_ID_MATTER_BRIDGE_FRONTEND_HOME_OPEN){s_view=ROOT_HOME;render(ui);}
    else if(action==GSP_ACT_ID_MATTER_BRIDGE_FRONTEND_BOARD_INFO||
            action==GSP_ACT_ID_MATTER_BRIDGE_FRONTEND_MONITOR_INFO||
            action==GSP_ACT_ID_MATTER_BRIDGE_BOARD_INFO){s_view=ROOT_BOARD;render(ui);}
    else if(action==GSP_ACT_ID_MATTER_BRIDGE_BOARD_BACK){(void)back(ui,event->timestamp_us);}
    else if(action==GSP_ACT_ID_MATTER_BRIDGE_MATTER_SETUP){s_view=ROOT_PAIRING;s_qr_ready=false;s_pairing_log_err=ESP_OK;render(ui);}
}
const mosaic_app_descriptor_t mosaic_matter_bridge_app={
    .id=21,.launch_action=GSP_ACT_ID_APP_MATTER_BRIDGE,
    .back_action=MOSAIC_APP_SHELL_BACK_ACTION,.back_exits_app=true,.root_header_in_stack=true,
    .name="matter_bridge",.title="Smart Home",.directory=&gsp_obj_directory_matter_bridge,
    .instance_slots=12,.image_cache_bytes=512U*1024U,.disable_swipe=true,
    .on_started=started,.on_stopping=stopped,.on_event=event,.on_back=back,
};
