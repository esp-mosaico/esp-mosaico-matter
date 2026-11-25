#include "frontend_ui.h"
#include "frontend_backend.h"
#include "frontend_camera.h"
#include "frontend_model.h"
#include "frontend_marquee.h"
#include "frontend_jobs.h"
#include "matter_bridge_objects.h"
#include "matter_bridge_actions.h"
#include "matter_bridge_binds.h"
#include "esp_timer.h"
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>
#ifdef ESP_PLATFORM
#include "nvs.h"
#include "esp_log.h"
#endif

#define FB(NAME) GSP_BIND_MATTER_BRIDGE_FRONTEND_##NAME
#define FK(NAME) GSP_OBJ_KEY_MATTER_BRIDGE_FRONTEND_##NAME
#define FA(NAME) GSP_ACT_ID_MATTER_BRIDGE_FRONTEND_##NAME
static frontend_model_t s_model;
static frontend_backend_snapshot_t s_snapshot;
static uint32_t s_completion;
static char s_feedback[96];
static bool s_automation_preference;
static uint32_t s_jobs_message;
enum { HOME_CARD_CAPACITY = 16, HOME_VIEW_HEIGHT = 416, HOME_CAMERA_SPACE = 243,
       HOME_GRID_TOP = 156, HOME_ROW_PITCH = 140, HOME_ROW_HEIGHT = 136 };
_Static_assert(BRIDGE_DEV_MNGR_MAX_DEVICES <= HOME_CARD_CAPACITY, "Home must fit the Matter catalog");
static char s_card_ids[HOME_CARD_CAPACITY][BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
static char s_pressed_card_ids[HOME_CARD_CAPACITY][BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
static uint32_t s_pressed_generation[HOME_CARD_CAPACITY];
static bool s_press_ids_valid;
static bool s_detail_press_valid;
static char s_detail_press_id[BRIDGE_DEV_MNGR_DEVICE_ID_MAX];
static uint32_t s_detail_press_generation;
static frontend_marquee_t s_marquee[24];
static int s_marquee_x[24];
static const frontend_backend_catalog_entry_t *selected_entry(void);
static const frontend_backend_catalog_entry_t *entry_by_id(const char *id);
static bool s_visible;
static bool s_privacy_requested;
enum { MENU_LIVE, MENU_FAMILY, MENU_ACTIVITY };
enum { PRIVACY_ALL, PRIVACY_SELECT, PRIVACY_BACKGROUND };
enum { FAMILY_CAPACITY = 3, MEMBER_NAME_BYTES = 32 };
static unsigned s_monitor_menu, s_privacy_mode;
static bool s_privacy_tray, s_faces_saved, s_member_keyboard;
static frontend_route_t s_member_origin;
static char s_family[FAMILY_CAPACITY][MEMBER_NAME_BYTES + 1];
static bool s_pointer_down, s_dragged;
static int32_t s_pointer_y;
static unsigned s_scroll_limit, s_scroll_origin;
static int s_painted_scroll, s_painted_devices;
static bool s_scroll_candidate;
typedef enum { SLIDER_NONE, SLIDER_TEMPERATURE, SLIDER_BRIGHTNESS, SLIDER_HUE, SLIDER_SATURATION } slider_mode_t;
static slider_mode_t s_slider_mode;
static float s_slider_fraction;
static bool s_slider_pending;
static int s_temperature_x, s_brightness_x, s_brightness_y;
static uint16_t s_card_glow[HOME_CARD_CAPACITY];
static int s_colour_x[2];

/* Only the UI owner touches this cache. Cache successful queue admission so a
 * full queue retries on the next tick instead of silently losing a label. */
static struct { uint16_t id; char text[96]; } s_text[112];
static struct { uint16_t id; bool visible, used; } s_visibility[256];

static void text(esp_gsp_handle_t ui, uint16_t id, const char *value)
{
    for (unsigned i = 0; i < sizeof(s_text) / sizeof(s_text[0]); ++i) {
        if (s_text[i].id == id || !s_text[i].id) {
            if (s_text[i].id == id && !strcmp(s_text[i].text, value)) return;
            if (esp_gsp_set_text(ui, id, value) == ESP_GSP_OK) {
                s_text[i].id = id;
                snprintf(s_text[i].text, sizeof(s_text[i].text), "%s", value);
            }
            return;
        }
    }
}

static void visible(esp_gsp_handle_t ui, uint16_t id, bool value)
{
    for (unsigned i = 0; i < 256; ++i) {
        if (s_visibility[i].id == id || !s_visibility[i].used) {
            if (s_visibility[i].used && s_visibility[i].id == id && s_visibility[i].visible == value) return;
            esp_gsp_err_t error;
            if(id==FB(HOME_CAMERA_CANVAS_BOTH))error=esp_gsp_component_set_visible(ui,FK(HOME_CAMERA_CANVAS_BOTH),value);
            else if(id==FB(HOME_CAMERA_CANVAS_CAMERA_ONLY))error=esp_gsp_component_set_visible(ui,FK(HOME_CAMERA_CANVAS_CAMERA_ONLY),value);
            else if(id==FB(MONITOR_CAMERA_CANVAS))error=esp_gsp_component_set_visible(ui,FK(MONITOR_CAMERA_CANVAS),value);
            else error=esp_gsp_set_visible(ui,id,value);
            if(error == ESP_GSP_OK)
                s_visibility[i].id = id, s_visibility[i].visible = value, s_visibility[i].used = true;
            return;
        }
    }
}

static void feedback(const char *value) { snprintf(s_feedback, sizeof(s_feedback), "%s", value); }
static void save_monitor_policy(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open("frontend_home", NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_u8(handle, "privacy", s_privacy_requested);
        (void)nvs_set_u8(handle, "privacy_mode", s_privacy_mode);
        (void)nvs_set_u8(handle, "monitor_on", s_model.monitor_enabled);
        (void)nvs_set_u8(handle,"automation",s_automation_preference);
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
#endif
}

static unsigned family_count(void)
{
    unsigned count = 0;
    while (count < FAMILY_CAPACITY && s_family[count][0]) ++count;
    return count;
}

static void save_family(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open("frontend_home", NVS_READWRITE, &handle) == ESP_OK) {
        (void)nvs_set_blob(handle, "family_names", s_family, sizeof(s_family));
        (void)nvs_commit(handle);
        nvs_close(handle);
    }
#endif
}

static void member_keyboard_close(esp_gsp_handle_t ui)
{
    if (!s_member_keyboard) return;
    s_member_keyboard = false;
    (void)esp_gsp_keyboard_attach_ex(ui, ESP_GSP_KEYBOARD_NONE, FB(FAMILY_MEMBER_DRAFT), 0);
    (void)esp_gsp_set_cursor(ui, ESP_GSP_NO_CURSOR);
    (void)esp_gsp_set_text(ui, FB(FAMILY_MEMBER_DRAFT), "");
}

static bool member_name(esp_gsp_handle_t ui, char out[MEMBER_NAME_BYTES + 1])
{
    if (!s_member_keyboard ||
        esp_gsp_keyboard_text(ui, out, MEMBER_NAME_BYTES + 1) != ESP_GSP_OK) return false;
    char *start = out;
    while (isspace((unsigned char)*start)) ++start;
    size_t length = strlen(start);
    while (length && isspace((unsigned char)start[length - 1])) --length;
    memmove(out, start, length);
    out[length] = '\0';
    return length != 0;
}

static void member_back(esp_gsp_handle_t ui)
{
    member_keyboard_close(ui);
    s_model.route = s_member_origin;
    feedback("");
}
static bool detail_identity_valid(void)
{
    if(!s_detail_press_valid)return true;
    const frontend_backend_catalog_entry_t *entry=selected_entry();
    return entry&&entry->available&&entry->reachable&&entry->generation==s_detail_press_generation&&
        !strcmp(entry->id,s_detail_press_id);
}
static bool command(frontend_backend_device_t device,frontend_backend_command_t kind,int value,bool manual)
{
    if(manual&&!detail_identity_valid()){feedback("Device changed; try again");return false;}
    if(device!=FRONTEND_DEVICE_LIGHT){feedback("Control is unavailable");return false;}
    const frontend_backend_catalog_entry_t *entry=selected_entry();
    if(!entry||!entry->available||!entry->reachable){feedback("Device unavailable");return false;}
    if(manual){frontend_model_manual(&s_model,1);frontend_jobs_cancel(entry->id);}
    esp_err_t error=frontend_backend_command_id(entry->id,kind,value);
    feedback(error==ESP_OK?"Sending...":error==ESP_ERR_NOT_SUPPORTED?"This device does not support that setting":
        error==ESP_ERR_INVALID_STATE?"Device unavailable or busy; try again":esp_err_to_name(error));
    return error==ESP_OK;
}
static bool toggle_id(const char *id)
{
    if((s_model.route==FRONTEND_LIGHT||s_model.route==FRONTEND_COLOUR)&&!detail_identity_valid()){feedback("Device changed; try again");return false;}
    const frontend_backend_catalog_entry_t *entry=entry_by_id(id);
    if(!entry||!entry->available||!entry->reachable){feedback("Device unavailable");return false;}
    if(!entry->supports_power){feedback("This sensor is read-only");return false;}
    if(!entry->on_known){feedback("Reading device state; try again");return false;}
    frontend_model_manual(&s_model,1);frontend_jobs_cancel(entry->id);
    esp_err_t error=frontend_backend_command_id(entry->id,FRONTEND_COMMAND_POWER,!entry->on);
    feedback(error==ESP_OK?"Sending...":"Device unavailable or busy; try again");
    return error==ESP_OK;
}

void frontend_ui_init(bridge_dev_mngr_handle_t manager)
{
#ifdef ESP_PLATFORM
    ESP_LOGI("smart_home", "Matter bridge frontend ready");
#endif
    frontend_model_init(&s_model);
    s_privacy_requested = false;
    s_privacy_tray = s_faces_saved = s_member_keyboard = false;
    s_monitor_menu = MENU_LIVE;
    s_privacy_mode = PRIVACY_ALL;
    memset(s_family, 0, sizeof(s_family));
    s_automation_preference=false;
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open("frontend_home", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value;
        if (nvs_get_u8(handle, "privacy", &value) == ESP_OK) s_privacy_requested = value != 0;
        size_t family_size = sizeof(s_family);
        if (nvs_get_blob(handle, "family_names", s_family, &family_size) != ESP_OK ||
            family_size != sizeof(s_family)) memset(s_family, 0, sizeof(s_family));
        for (unsigned i = 0; i < FAMILY_CAPACITY; ++i) s_family[i][MEMBER_NAME_BYTES] = '\0';
        if (nvs_get_u8(handle, "monitor_on", &value) == ESP_OK) s_model.monitor_enabled = value != 0;
        if(nvs_get_u8(handle,"automation",&value)==ESP_OK)s_automation_preference=value!=0;
        nvs_close(handle);
    }
#endif
    frontend_backend_init(manager);
    frontend_camera_reset();
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(s_text, 0, sizeof(s_text));
    memset(s_visibility, 0, sizeof(s_visibility));
    s_visible = false;
    s_completion=s_jobs_message=0;
    frontend_jobs_init();
    memset(s_card_ids,0,sizeof(s_card_ids));
    memset(s_marquee,0,sizeof(s_marquee));
    for(unsigned i=0;i<24;i++)s_marquee_x[i]=1;
    s_press_ids_valid=false;s_detail_press_valid=false;
    s_pointer_down = false;
    s_slider_mode = SLIDER_NONE;
    s_scroll_limit = s_scroll_origin = 0;
    s_painted_scroll = s_painted_devices = -1;
    s_scroll_candidate = s_dragged = false;
    s_slider_pending = false;
    s_temperature_x = s_brightness_x = s_brightness_y = -1;
    memset(s_card_glow, 0, sizeof(s_card_glow));
    s_colour_x[0] = s_colour_x[1] = -1;
    feedback("");
}

static const frontend_backend_catalog_entry_t *entry_by_id(const char *id)
{
    if(!id||!id[0])return NULL;
    for(size_t i=0;i<s_snapshot.catalog_count;i++)if(!strcmp(s_snapshot.catalog[i].id,id))return &s_snapshot.catalog[i];
    return NULL;
}
static bool camera_entry(const frontend_backend_catalog_entry_t *entry)
{ return entry->device_type==0x0142||!strcmp(entry->profile_id,"mosaico_camera"); }
static bool device_card(const frontend_backend_catalog_entry_t *entry)
{
    if (!entry->available || camera_entry(entry)) return false;
    return strcmp(entry->profile_id, BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR) != 0 &&
           strcmp(entry->profile_id, BRIDGE_DEV_MNGR_PROFILE_ILLUMINANCE_SENSOR) != 0;
}

static size_t display_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < s_snapshot.catalog_count; ++i) {
        if (device_card(&s_snapshot.catalog[i])) ++count;
    }
    return count;
}
static const frontend_backend_catalog_entry_t *display_entry(size_t index)
{
    for(size_t i=0;i<s_snapshot.catalog_count;i++)if (device_card(&s_snapshot.catalog[i])) {
        if (!index) return &s_snapshot.catalog[i];
        --index;
    }
    return NULL;
}
static const frontend_backend_catalog_entry_t *selected_entry(void)
{ return entry_by_id(s_snapshot.selected_id); }
static const frontend_backend_catalog_entry_t *card_entry(unsigned slot)
{ return slot < HOME_CARD_CAPACITY ? entry_by_id(s_card_ids[slot]) : NULL; }

static void update_card_ids(void)
{
    if (s_pointer_down) return;
    for (unsigned slot = 0; slot < HOME_CARD_CAPACITY; ++slot) {
        const frontend_backend_catalog_entry_t *entry = display_entry(slot);
        snprintf(s_card_ids[slot], sizeof(s_card_ids[slot]), "%s", entry ? entry->id : "");
    }
}

static void paint_home_scroll(esp_gsp_handle_t ui)
{
    int offset = (int)s_model.scroll_offset;
    if (s_painted_scroll == offset) return;
    const gsp_value_t y = {.type=GSP_VALUE_I32,.data.i32=-offset};
    if (esp_gsp_component_set_property(ui, FK(HOME_SCROLL), GSP_PROPERTY_KEY_Y, &y) == ESP_GSP_OK)
        s_painted_scroll = offset;
}

static void update_home_bounds(esp_gsp_handle_t ui, bool camera, size_t count)
{
    int devices = camera ? HOME_CAMERA_SPACE : 0;
    if (s_painted_devices != devices) {
        const gsp_value_t y = {.type=GSP_VALUE_I32,.data.i32=devices};
        if (esp_gsp_component_set_property(ui, FK(HOME_DEVICES), GSP_PROPERTY_KEY_Y, &y) == ESP_GSP_OK)
            s_painted_devices = devices;
    }
    unsigned rows = (count + 1) / 2;
    unsigned bottom = rows ? devices + HOME_GRID_TOP + (rows - 1) * HOME_ROW_PITCH + HOME_ROW_HEIGHT + 12 : 0;
    s_scroll_limit = bottom > HOME_VIEW_HEIGHT ? bottom - HOME_VIEW_HEIGHT : 0;
    if (s_model.scroll_offset > s_scroll_limit) s_model.scroll_offset = s_scroll_limit;
    paint_home_scroll(ui);
}

static void state_text(const frontend_backend_catalog_entry_t *entry,char *out,size_t size)
{
    const char *value="Reading...";
    if(!entry||!entry->available)value="Unavailable";
    else if(!entry->reachable)value="Offline";
    else if(entry->pending)value="Sending...";
    else if(entry->supports_power&&entry->on_known)value=entry->on?"ON":"OFF";
    else if(entry->occupancy_known)value=entry->occupied?"Motion":"No Motion";
    else if(entry->contact_known)value=entry->contact_closed?"Closed":"Open";
    else if(entry->illuminance_known){snprintf(out,size,"%.0f lx",entry->illuminance_lux);return;}
    snprintf(out,size,"%s",value);
}

static const struct { uint16_t binding; gsp_component_key_t component; int width,font_size; } name_frames[]={
    {FB(CAMERA_NAME_BOTH),FK(CAMERA_NAME_BOTH),280,18},
    {FB(CAMERA_NAME_CAMERA_ONLY),FK(CAMERA_NAME_CAMERA_ONLY),280,18},
    {FB(CARD_0_NAME),FK(CARD_0_NAME),160,24},
    {FB(CARD_1_NAME),FK(CARD_1_NAME),160,24},
    {FB(CARD_2_NAME),FK(CARD_2_NAME),160,24},
    {FB(CARD_3_NAME),FK(CARD_3_NAME),160,24},
    {FB(CARD_4_NAME),FK(CARD_4_NAME),160,24},
    {FB(CARD_5_NAME),FK(CARD_5_NAME),160,24},
    {FB(CARD_6_NAME),FK(CARD_6_NAME),160,24},
    {FB(CARD_7_NAME),FK(CARD_7_NAME),160,24},
    {FB(CARD_8_NAME),FK(CARD_8_NAME),160,24},
    {FB(CARD_9_NAME),FK(CARD_9_NAME),160,24},
    {FB(CARD_10_NAME),FK(CARD_10_NAME),160,24},
    {FB(CARD_11_NAME),FK(CARD_11_NAME),160,24},
    {FB(CARD_12_NAME),FK(CARD_12_NAME),160,24},
    {FB(CARD_13_NAME),FK(CARD_13_NAME),160,24},
    {FB(CARD_14_NAME),FK(CARD_14_NAME),160,24},
    {FB(CARD_15_NAME),FK(CARD_15_NAME),160,24},
    {FB(LIGHT_NAME),FK(LIGHT_NAME),286,32},
    {FB(COLOUR_NAME),FK(COLOUR_NAME),444,32},
    {FB(SENSOR_NAME),FK(SENSOR_NAME),444,32},
};
static void named(esp_gsp_handle_t ui,uint16_t binding,const char *value)
{
    text(ui,binding,value);
    for(unsigned i=0;i<sizeof(name_frames)/sizeof(name_frames[0]);i++)if(name_frames[i].binding==binding){
        int x=frontend_marquee_offset(&s_marquee[i],value,name_frames[i].width,name_frames[i].font_size,esp_timer_get_time());
        if (x != s_marquee_x[i]) {
            gsp_value_t position = {.type = GSP_VALUE_I32, .data.i32 = x};
            if (esp_gsp_component_set_property(ui, name_frames[i].component, GSP_PROPERTY_KEY_X, &position) == ESP_GSP_OK) {
                s_marquee_x[i] = x;
            }
        }
        return;
    }
}

static bool home_any_power_on(void)
{
    for (size_t i = 0; i < s_snapshot.catalog_count; ++i) {
        const frontend_backend_catalog_entry_t *entry = &s_snapshot.catalog[i];
        if (entry->supports_power && entry->available && entry->reachable && entry->on_known && entry->on)
            return true;
    }
    return false;
}

static void render_icons(esp_gsp_handle_t ui)
{
    static const struct { unsigned kind,slot;uint16_t on_binding,off_binding; } icons[]={
    {2,0,FB(ALL_INTERACTION_ONLY_ICON_ON_VISIBLE),FB(ALL_INTERACTION_ONLY_ICON_OFF_VISIBLE)},
    {3,0,FB(AUTOMATION_INTERACTION_ONLY_ICON_ON_VISIBLE),FB(AUTOMATION_INTERACTION_ONLY_ICON_OFF_VISIBLE)},
    {1,0,FB(CARD_0_ICON_ON_VISIBLE),FB(CARD_0_ICON_OFF_VISIBLE)},
    {1,1,FB(CARD_1_ICON_ON_VISIBLE),FB(CARD_1_ICON_OFF_VISIBLE)},
    {1,2,FB(CARD_2_ICON_ON_VISIBLE),FB(CARD_2_ICON_OFF_VISIBLE)},
    {1,3,FB(CARD_3_ICON_ON_VISIBLE),FB(CARD_3_ICON_OFF_VISIBLE)},
    {1,4,FB(CARD_4_ICON_ON_VISIBLE),FB(CARD_4_ICON_OFF_VISIBLE)},
    {1,5,FB(CARD_5_ICON_ON_VISIBLE),FB(CARD_5_ICON_OFF_VISIBLE)},
    {1,6,FB(CARD_6_ICON_ON_VISIBLE),FB(CARD_6_ICON_OFF_VISIBLE)},
    {1,7,FB(CARD_7_ICON_ON_VISIBLE),FB(CARD_7_ICON_OFF_VISIBLE)},
    {1,8,FB(CARD_8_ICON_ON_VISIBLE),FB(CARD_8_ICON_OFF_VISIBLE)},
    {1,9,FB(CARD_9_ICON_ON_VISIBLE),FB(CARD_9_ICON_OFF_VISIBLE)},
    {1,10,FB(CARD_10_ICON_ON_VISIBLE),FB(CARD_10_ICON_OFF_VISIBLE)},
    {1,11,FB(CARD_11_ICON_ON_VISIBLE),FB(CARD_11_ICON_OFF_VISIBLE)},
    {1,12,FB(CARD_12_ICON_ON_VISIBLE),FB(CARD_12_ICON_OFF_VISIBLE)},
    {1,13,FB(CARD_13_ICON_ON_VISIBLE),FB(CARD_13_ICON_OFF_VISIBLE)},
    {1,14,FB(CARD_14_ICON_ON_VISIBLE),FB(CARD_14_ICON_OFF_VISIBLE)},
    {1,15,FB(CARD_15_ICON_ON_VISIBLE),FB(CARD_15_ICON_OFF_VISIBLE)},
    {0,0,FB(LIGHT_POWER_ICON_ON_VISIBLE),FB(LIGHT_POWER_ICON_OFF_VISIBLE)},
};
    bool any_on = home_any_power_on();
    for(unsigned i=0;i<sizeof(icons)/sizeof(icons[0]);i++){
        const frontend_backend_catalog_entry_t *entry=icons[i].kind==1?card_entry(icons[i].slot):selected_entry();
        bool on=icons[i].kind==2?any_on:icons[i].kind==3?s_model.automation_active:
            entry&&entry->available&&entry->reachable&&entry->on_known&&entry->on;
        visible(ui,icons[i].on_binding,on);visible(ui,icons[i].off_binding,!on);
    }
}

static float clamp_fraction(float value)
{
    return fminf(1.0f, fmaxf(0.0f, value));
}

/* The stroke is rasterized from these same cubic control points. */
static void brightness_point(float t, float *x, float *y)
{
    float s = 1.0f - t;
    *x = s*s*s*25 + 3*s*s*t*130 + 3*s*t*t*336 + t*t*t*442;
    *y = 119 - 294*s*t;
}

static bool temperature_bounds(const frontend_backend_catalog_entry_t *light)
{
    return light && light->reachable && light->supports_kelvin &&
        light->kelvin_bounds_known && light->max_kelvin > light->min_kelvin;
}

static int kelvin_at(const frontend_backend_catalog_entry_t *light, float fraction)
{
    return (int)light->min_kelvin +
        (int)lroundf(fraction * (light->max_kelvin - light->min_kelvin));
}

static void render_light_controls(esp_gsp_handle_t ui, const frontend_backend_catalog_entry_t *light)
{
    if (s_model.route != FRONTEND_LIGHT) return;
    bool preview = s_slider_mode && detail_identity_valid() &&
        (s_pointer_down || (s_slider_pending && light && light->pending));
    bool bounds = temperature_bounds(light);
    bool dragging_temperature = preview && s_slider_mode == SLIDER_TEMPERATURE;
    bool temperature_known = bounds && (light->kelvin_known || dragging_temperature);
    bool dragging_brightness = preview && s_slider_mode == SLIDER_BRIGHTNESS;
    bool brightness_known = light && light->reachable && light->supports_brightness &&
        (light->brightness_known || dragging_brightness);
    char value[32];
    if (temperature_known) {
        float fraction = dragging_temperature ? s_slider_fraction :
            clamp_fraction(((float)light->kelvin - light->min_kelvin) /
                           (light->max_kelvin - light->min_kelvin));
        int kelvin = dragging_temperature ? kelvin_at(light, fraction) : (int)light->kelvin;
        snprintf(value, sizeof(value), "%d (K)", kelvin);
        int x = (int)lroundf(fraction * 396);
        if (x != s_temperature_x) {
            const gsp_component_property_update_t updates[] = {
                {FK(TEMPERATURE_THUMB), GSP_PROPERTY_KEY_X, {.type=GSP_VALUE_I32,.data.i32=x}},
                {FK(TEMPERATURE_FILL), GSP_PROPERTY_KEY_VALUE, {.type=GSP_VALUE_I32,.data.i32=x+11}},
            };
            if (esp_gsp_component_set_properties(ui, updates, 2) == ESP_GSP_OK) s_temperature_x = x;
        }
    } else {
        snprintf(value, sizeof(value), "-- (K)");
    }
    text(ui, FB(LIGHT_KELVIN), value);
    visible(ui, FB(TEMPERATURE_THUMB_VISIBLE), temperature_known);

    const uint16_t ticks[] = {FB(TEMPERATURE_TICK_0), FB(TEMPERATURE_TICK_1),
        FB(TEMPERATURE_TICK_2), FB(TEMPERATURE_TICK_3), FB(TEMPERATURE_TICK_4)};
    const int offsets[] = {0, 65, 141, 249, 396};
    for (unsigned i = 0; i < 5; ++i) {
        if (bounds) snprintf(value, sizeof(value), "%d", kelvin_at(light, offsets[i] / 396.0f));
        else value[0] = '\0';
        text(ui, ticks[i], value);
    }

    if (brightness_known) {
        float fraction = dragging_brightness ? s_slider_fraction : light->brightness_percent / 100.0f;
        int percent = (int)lroundf(fraction * 100);
        if (dragging_brightness && percent < 1) percent = 1;
        snprintf(value, sizeof(value), "%d%%", percent);
        float arc_x, arc_y;
        brightness_point(fraction, &arc_x, &arc_y);
        int x = (int)lroundf(arc_x), y = (int)lroundf(arc_y);
        if (x != s_brightness_x || y != s_brightness_y) {
            const gsp_component_property_update_t updates[] = {
                {FK(BRIGHTNESS_THUMB), GSP_PROPERTY_KEY_X, {.type=GSP_VALUE_I32,.data.i32=x-23}},
                {FK(BRIGHTNESS_THUMB), GSP_PROPERTY_KEY_Y, {.type=GSP_VALUE_I32,.data.i32=y-23}},
            };
            if (esp_gsp_component_set_properties(ui, updates, 2) == ESP_GSP_OK) {
                s_brightness_x = x;
                s_brightness_y = y;
            }
        }
    } else {
        snprintf(value, sizeof(value), "--%%");
    }
    text(ui, FB(LIGHT_BRIGHTNESS), value);
    static const uint16_t bands[] = {
        FB(BRIGHTNESS_BAND_1_VISIBLE), FB(BRIGHTNESS_BAND_2_VISIBLE), FB(BRIGHTNESS_BAND_3_VISIBLE), FB(BRIGHTNESS_BAND_4_VISIBLE),
        FB(BRIGHTNESS_BAND_5_VISIBLE), FB(BRIGHTNESS_BAND_6_VISIBLE), FB(BRIGHTNESS_BAND_7_VISIBLE), FB(BRIGHTNESS_BAND_8_VISIBLE),
        FB(BRIGHTNESS_BAND_9_VISIBLE), FB(BRIGHTNESS_BAND_10_VISIBLE), FB(BRIGHTNESS_BAND_11_VISIBLE), FB(BRIGHTNESS_BAND_12_VISIBLE),
        FB(BRIGHTNESS_BAND_13_VISIBLE), FB(BRIGHTNESS_BAND_14_VISIBLE), FB(BRIGHTNESS_BAND_15_VISIBLE), FB(BRIGHTNESS_BAND_16_VISIBLE),
        FB(BRIGHTNESS_BAND_17_VISIBLE), FB(BRIGHTNESS_BAND_18_VISIBLE), FB(BRIGHTNESS_BAND_19_VISIBLE), FB(BRIGHTNESS_BAND_20_VISIBLE),
        FB(BRIGHTNESS_BAND_21_VISIBLE), FB(BRIGHTNESS_BAND_22_VISIBLE), FB(BRIGHTNESS_BAND_23_VISIBLE), FB(BRIGHTNESS_BAND_24_VISIBLE),
        FB(BRIGHTNESS_BAND_25_VISIBLE), FB(BRIGHTNESS_BAND_26_VISIBLE), FB(BRIGHTNESS_BAND_27_VISIBLE), FB(BRIGHTNESS_BAND_28_VISIBLE)
    };
    for (unsigned i = 0; i < sizeof(bands) / sizeof(bands[0]); ++i) {
        visible(ui, bands[i], brightness_known && (int)(16 * (i+1)) < s_brightness_x);
    }
    visible(ui, FB(BRIGHTNESS_THUMB_VISIBLE), brightness_known);
}

static void render_colour_controls(esp_gsp_handle_t ui, const frontend_backend_catalog_entry_t *light)
{
    if (s_model.route != FRONTEND_COLOUR) return;
    static const struct {
        gsp_component_key_t thumb, fill;
        uint16_t visible, label;
        slider_mode_t mode;
        unsigned scale;
        const char *suffix;
    } tracks[] = {
        {FK(HUE_THUMB), FK(HUE_SLIDER), FB(HUE_THUMB_VISIBLE), FB(COLOUR_HUE), SLIDER_HUE, 360, " deg"},
        {FK(SATURATION_THUMB), FK(SATURATION_SLIDER), FB(SATURATION_THUMB_VISIBLE),
         FB(COLOUR_SATURATION), SLIDER_SATURATION, 100, "%"},
    };
    for (unsigned i = 0; i < 2; ++i) {
        bool preview = s_slider_mode == tracks[i].mode && detail_identity_valid() &&
            (s_pointer_down || (s_slider_pending && light && light->pending));
        bool known = light && light->reachable && light->supports_color &&
            (preview || (i == 0 ? light->hue_known : light->saturation_known));
        char label[32] = "--";
        if (known) {
            float fraction = preview ? s_slider_fraction : (i == 0 ? light->hue : light->saturation) / 254.0f;
            snprintf(label, sizeof(label), "%d%s", (int)lroundf(fraction * tracks[i].scale), tracks[i].suffix);
            int x = (int)lroundf(fraction * 300);
            if (x != s_colour_x[i]) {
                const gsp_component_property_update_t updates[] = {
                    {tracks[i].thumb, GSP_PROPERTY_KEY_X, {.type=GSP_VALUE_I32,.data.i32=64+x}},
                    {tracks[i].fill, GSP_PROPERTY_KEY_VALUE, {.type=GSP_VALUE_I32,.data.i32=14+x}},
                };
                if (esp_gsp_component_set_properties(ui, updates, 2) == ESP_GSP_OK) s_colour_x[i] = x;
            }
        }
        text(ui, tracks[i].label, label);
        visible(ui, tracks[i].visible, known);
    }
}

static void render_monitor_controls(esp_gsp_handle_t ui, bool live)
{
    bool on = s_model.monitor_enabled;
    bool tray = on && s_privacy_requested && s_privacy_tray;
    visible(ui, FB(MONITOR_CAMERA_UNAVAILABLE_VISIBLE), on && !live);
    visible(ui, FB(MONITOR_CAMERA_OFF_VISIBLE), !on);
    visible(ui, FB(MONITOR_PRIVACY_FAILURE_VISIBLE), false);
    visible(ui, FB(MONITOR_POWER_ON_VISIBLE), on);
    visible(ui, FB(MONITOR_POWER_OFF_VISIBLE), !on);
    visible(ui, FB(MONITOR_NORMAL_SELECTED_VISIBLE), !s_privacy_requested);
    visible(ui, FB(MONITOR_PRIVACY_SELECTED_VISIBLE), s_privacy_requested);
    visible(ui, FB(MONITOR_CONTROLS_VISIBLE), !tray);
    visible(ui, FB(MONITOR_PRIVACY_TRAY_VISIBLE), tray);
    static const uint16_t modes[] = {FB(MONITOR_PRIVACY_MODE_ALL_FACES_VISIBLE),
        FB(MONITOR_PRIVACY_MODE_SELECT_FACES_VISIBLE), FB(MONITOR_PRIVACY_MODE_BACKGROUND_VISIBLE)};
    static const uint16_t selected[] = {FB(MONITOR_TRAY_ALL_FACES_SELECTED_VISIBLE),
        FB(MONITOR_TRAY_SELECT_FACES_SELECTED_VISIBLE), FB(MONITOR_TRAY_BACKGROUND_SELECTED_VISIBLE)};
    for (unsigned i = 0; i < 3; ++i) {
        visible(ui, modes[i], on && s_privacy_requested && s_privacy_mode == i);
        visible(ui, selected[i], s_privacy_mode == i);
    }
    char stamp[24] = "";
#ifdef ESP_PLATFORM
    time_t now = time(NULL);
    struct tm local;
    if (now >= 1700000000 && localtime_r(&now, &local))
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &local);
#endif
    text(ui, FB(MONITOR_TIMESTAMP), stamp);
}

static void render_monitor_settings(esp_gsp_handle_t ui)
{
    if (s_model.route == FRONTEND_MENU) {
        visible(ui, FB(MENU_LIVE_SELECTED_VISIBLE), s_monitor_menu == MENU_LIVE);
        visible(ui, FB(MENU_FAMILY_SELECTED_VISIBLE), s_monitor_menu == MENU_FAMILY);
        visible(ui, FB(MENU_ACTIVITY_SELECTED_VISIBLE), s_monitor_menu == MENU_ACTIVITY);
    } else if (s_model.route == FRONTEND_FAMILY) {
        static const uint16_t names[] = {FB(FAMILY_NAME_0), FB(FAMILY_NAME_1), FB(FAMILY_NAME_2)};
        static const uint16_t rows[] = {FB(FAMILY_ROW_0_VISIBLE), FB(FAMILY_ROW_1_VISIBLE), FB(FAMILY_ROW_2_VISIBLE)};
        unsigned count = family_count();
        visible(ui, FB(FAMILY_EMPTY_VISIBLE), count == 0);
        visible(ui, FB(FAMILY_LIST_VISIBLE), count != 0);
        for (unsigned i = 0; i < FAMILY_CAPACITY; ++i) {
            visible(ui, rows[i], i < count);
            text(ui, names[i], s_family[i]);
        }
        char label[28];
        snprintf(label, sizeof(label), "Members: %u/%u", count, (unsigned)FAMILY_CAPACITY);
        text(ui, FB(FAMILY_COUNT), label);
    } else if (s_model.route == FRONTEND_FAMILY_MEMBER) {
        char draft[MEMBER_NAME_BYTES + 1] = "";
        bool has_name = member_name(ui, draft);
        visible(ui, FB(FAMILY_MEMBER_KEYBOARD_VISIBLE), s_member_keyboard);
        visible(ui, FB(FAMILY_MEMBER_SAVE_ENABLED_VISIBLE), has_name);
        text(ui, FB(FAMILY_MEMBER_NAME_PREVIEW), has_name ? draft : "Enter Member Name");
    } else if (s_model.route == FRONTEND_PRIVACY_FACES) {
        visible(ui, FB(PRIVACY_FACES_SCREEN_VISIBLE), !s_faces_saved);
        visible(ui, FB(PRIVACY_FACES_SAVED_VISIBLE), s_faces_saved);
    }
}

static void render_values(esp_gsp_handle_t ui)
{
    update_card_ids();
    render_icons(ui);
    static const struct { unsigned slot;uint16_t visible,name,state,on,off,glow; } card_frames[]={
    {0,FB(CARD_0_VISIBLE),FB(CARD_0_NAME),FB(CARD_0_STATE),FB(CARD_0_STATE_ON_VISIBLE),FB(CARD_0_STATE_OFF_VISIBLE),FB(CARD_0_GLOW)},
    {1,FB(CARD_1_VISIBLE),FB(CARD_1_NAME),FB(CARD_1_STATE),FB(CARD_1_STATE_ON_VISIBLE),FB(CARD_1_STATE_OFF_VISIBLE),FB(CARD_1_GLOW)},
    {2,FB(CARD_2_VISIBLE),FB(CARD_2_NAME),FB(CARD_2_STATE),FB(CARD_2_STATE_ON_VISIBLE),FB(CARD_2_STATE_OFF_VISIBLE),FB(CARD_2_GLOW)},
    {3,FB(CARD_3_VISIBLE),FB(CARD_3_NAME),FB(CARD_3_STATE),FB(CARD_3_STATE_ON_VISIBLE),FB(CARD_3_STATE_OFF_VISIBLE),FB(CARD_3_GLOW)},
    {4,FB(CARD_4_VISIBLE),FB(CARD_4_NAME),FB(CARD_4_STATE),FB(CARD_4_STATE_ON_VISIBLE),FB(CARD_4_STATE_OFF_VISIBLE),FB(CARD_4_GLOW)},
    {5,FB(CARD_5_VISIBLE),FB(CARD_5_NAME),FB(CARD_5_STATE),FB(CARD_5_STATE_ON_VISIBLE),FB(CARD_5_STATE_OFF_VISIBLE),FB(CARD_5_GLOW)},
    {6,FB(CARD_6_VISIBLE),FB(CARD_6_NAME),FB(CARD_6_STATE),FB(CARD_6_STATE_ON_VISIBLE),FB(CARD_6_STATE_OFF_VISIBLE),FB(CARD_6_GLOW)},
    {7,FB(CARD_7_VISIBLE),FB(CARD_7_NAME),FB(CARD_7_STATE),FB(CARD_7_STATE_ON_VISIBLE),FB(CARD_7_STATE_OFF_VISIBLE),FB(CARD_7_GLOW)},
    {8,FB(CARD_8_VISIBLE),FB(CARD_8_NAME),FB(CARD_8_STATE),FB(CARD_8_STATE_ON_VISIBLE),FB(CARD_8_STATE_OFF_VISIBLE),FB(CARD_8_GLOW)},
    {9,FB(CARD_9_VISIBLE),FB(CARD_9_NAME),FB(CARD_9_STATE),FB(CARD_9_STATE_ON_VISIBLE),FB(CARD_9_STATE_OFF_VISIBLE),FB(CARD_9_GLOW)},
    {10,FB(CARD_10_VISIBLE),FB(CARD_10_NAME),FB(CARD_10_STATE),FB(CARD_10_STATE_ON_VISIBLE),FB(CARD_10_STATE_OFF_VISIBLE),FB(CARD_10_GLOW)},
    {11,FB(CARD_11_VISIBLE),FB(CARD_11_NAME),FB(CARD_11_STATE),FB(CARD_11_STATE_ON_VISIBLE),FB(CARD_11_STATE_OFF_VISIBLE),FB(CARD_11_GLOW)},
    {12,FB(CARD_12_VISIBLE),FB(CARD_12_NAME),FB(CARD_12_STATE),FB(CARD_12_STATE_ON_VISIBLE),FB(CARD_12_STATE_OFF_VISIBLE),FB(CARD_12_GLOW)},
    {13,FB(CARD_13_VISIBLE),FB(CARD_13_NAME),FB(CARD_13_STATE),FB(CARD_13_STATE_ON_VISIBLE),FB(CARD_13_STATE_OFF_VISIBLE),FB(CARD_13_GLOW)},
    {14,FB(CARD_14_VISIBLE),FB(CARD_14_NAME),FB(CARD_14_STATE),FB(CARD_14_STATE_ON_VISIBLE),FB(CARD_14_STATE_OFF_VISIBLE),FB(CARD_14_GLOW)},
    {15,FB(CARD_15_VISIBLE),FB(CARD_15_NAME),FB(CARD_15_STATE),FB(CARD_15_STATE_ON_VISIBLE),FB(CARD_15_STATE_OFF_VISIBLE),FB(CARD_15_GLOW)},
};
    char value[96];
    float phase = ((esp_timer_get_time() / 100000) % 20) / 20.0f;
    float pulse = 1.0f - fabsf(2.0f * phase - 1.0f);
    unsigned red = 28 + (unsigned)(pulse * 68);
    unsigned green = 24 + (unsigned)(pulse * 12);
    unsigned blue = 25 - (unsigned)(pulse * 8);
    uint16_t glow = (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
    for(unsigned i=0;i<sizeof(card_frames)/sizeof(card_frames[0]);i++){
        const frontend_backend_catalog_entry_t *entry=card_entry(card_frames[i].slot);
        visible(ui,card_frames[i].visible,entry&&entry->available);
        named(ui,card_frames[i].name,entry?(entry->name[0]?entry->name:entry->id):"");
        state_text(entry,value,sizeof(value));
        bool on = strcmp(value, "ON") == 0;
        bool off = strcmp(value, "OFF") == 0;
        visible(ui, card_frames[i].on, on);
        visible(ui, card_frames[i].off, off);
        text(ui, card_frames[i].state, on || off ? "" : value);
        unsigned position = i;
        int row_y = (s_model.camera ? HOME_CAMERA_SPACE : 0) + HOME_GRID_TOP +
            (int)(position / 2) * HOME_ROW_PITCH - (int)s_model.scroll_offset;
        if (on && row_y < HOME_VIEW_HEIGHT && row_y + HOME_ROW_HEIGHT > 0 &&
            s_visible && s_model.route == FRONTEND_HOME &&
            s_card_glow[i] != glow && esp_gsp_set_color(ui, card_frames[i].glow, glow) == ESP_GSP_OK) {
            s_card_glow[i] = glow;
        }
    }
    const frontend_backend_catalog_entry_t *light=selected_entry();
    const char *name=light?(light->name[0]?light->name:light->id):"";
    named(ui,FB(LIGHT_NAME),name);named(ui,FB(COLOUR_NAME),name);named(ui,FB(SENSOR_NAME),name);
    state_text(light,value,sizeof(value));text(ui,FB(LIGHT_STATE),value);text(ui,FB(SENSOR_VALUE),value);
    const char *kind=light&&light->occupancy_known?"Occupancy sensor":light&&light->illuminance_known?"Illuminance sensor":
        light&&light->contact_known?"Contact sensor":"Matter device";
    text(ui,FB(SENSOR_KIND),kind);
    bool level=light&&light->supports_brightness,temperature=light&&light->supports_kelvin,color=light&&light->supports_color;
    visible(ui,FB(LIGHT_PRESETS_VISIBLE),level&&temperature);
    visible(ui,FB(LIGHT_TEMPERATURE_VISIBLE),temperature);
    visible(ui,FB(LIGHT_LEVEL_VISIBLE),level);
    visible(ui,FB(LIGHT_CONTROLS_VISIBLE),level||temperature);
    visible(ui,FB(COLOUR_LINK_VISIBLE),color);
    render_light_controls(ui, light);
    render_colour_controls(ui, light);
    if(s_snapshot.ldr_known)snprintf(value,sizeof(value),"LDR %u%%",s_snapshot.ldr_percent);else snprintf(value,sizeof(value),"LDR --");
    text(ui,FB(LDR_INTERACTION_ONLY),value);
    snprintf(value,sizeof(value),"PIR %s",!s_snapshot.pir_known?"--":s_snapshot.pir?"Motion":"No Motion");
    text(ui,FB(PIR_INTERACTION_ONLY),value);
    text(ui,FB(AUTOMATION_STATE_INTERACTION_ONLY),s_model.automation_active?"ACTIVE":"OFF");
    text(ui,FB(ALL_STATE_INTERACTION_ONLY),home_any_power_on()?"Turn All Off":"Turn All On");
    const char *camera_name="Living Room";
    for(size_t i=0;i<s_snapshot.catalog_count;i++)if(camera_entry(&s_snapshot.catalog[i])){camera_name=s_snapshot.catalog[i].name;break;}
    named(ui,FB(CAMERA_NAME_BOTH),camera_name);named(ui,FB(CAMERA_NAME_CAMERA_ONLY),camera_name);
    const uint16_t messages[]={FB(HOME_FEEDBACK),FB(LIGHT_FEEDBACK),FB(COLOUR_FEEDBACK),FB(SENSOR_FEEDBACK),
        FB(MONITOR_FEEDBACK),FB(FAMILY_FEEDBACK),FB(ACTIVITY_FEEDBACK),
        FB(FAMILY_MEMBER_FEEDBACK),FB(PRIVACY_FACES_FEEDBACK)};
    for(unsigned i=0;i<sizeof(messages)/sizeof(messages[0]);i++)text(ui,messages[i],s_feedback);
}

void frontend_ui_tick(esp_gsp_handle_t ui,bool shown,bool camera,bool interaction)
{
    s_visible=shown;
    frontend_backend_tick(&s_snapshot);
    size_t count = display_count();
    bool has_devices = interaction || count > 0 ||
        s_snapshot.pir_endpoint_id || s_snapshot.ldr_endpoint_id;
    if (s_model.camera != camera || s_model.interaction != has_devices) s_scroll_candidate = false;
    frontend_model_modules(&s_model,camera,has_devices);
    s_model.automation_active=s_automation_preference&&has_devices;
    if((s_model.route==FRONTEND_LIGHT||s_model.route==FRONTEND_COLOUR||s_model.route==FRONTEND_SENSOR)&&
       (!selected_entry()||!selected_entry()->available)){
        s_model.route=FRONTEND_HOME;feedback("Device removed");
    }
    if(s_snapshot.completion_serial!=s_completion){
        s_completion=s_snapshot.completion_serial;
        feedback(s_snapshot.completion_error==ESP_OK?"Device updated":s_snapshot.message);
    }
    unsigned automation=frontend_model_automation(&s_model,s_snapshot.pir_known,s_snapshot.pir,
        s_snapshot.ldr_known,s_snapshot.ldr_percent,esp_timer_get_time());
    if(automation&1U){
        for(size_t i=0;i<s_snapshot.catalog_count;i++){
            const frontend_backend_catalog_entry_t *entry=&s_snapshot.catalog[i];
            if(!entry->supports_power||!entry->reachable||(strcmp(entry->profile_id,BRIDGE_DEV_MNGR_PROFILE_COLOR_LIGHT)&&
               strcmp(entry->profile_id,BRIDGE_DEV_MNGR_PROFILE_DIMMABLE_LIGHT)&&strcmp(entry->profile_id,BRIDGE_DEV_MNGR_PROFILE_ON_OFF_LIGHT)))continue;
            unsigned steps=FRONTEND_JOB_POWER;
            if(entry->supports_brightness)steps|=FRONTEND_JOB_LEVEL;
            if(entry->supports_kelvin)steps|=FRONTEND_JOB_KELVIN;
            (void)frontend_jobs_queue(entry,steps,true,80,4000,true);
        }
    }
    frontend_jobs_tick(&s_snapshot);
    if(frontend_jobs_message_serial()!=s_jobs_message){
        s_jobs_message=frontend_jobs_message_serial();feedback(frontend_jobs_message());
    }
    if (!shown || s_model.route != FRONTEND_FAMILY_MEMBER) member_keyboard_close(ui);
    if (!shown || s_model.route != FRONTEND_MONITOR) s_privacy_tray = false;
    static const uint16_t roots[FRONTEND_ROUTE_COUNT]={FB(HOME_VISIBLE),FB(LIGHT_VISIBLE),
        FB(COLOUR_VISIBLE),FB(SENSOR_VISIBLE),FB(MONITOR_VISIBLE),FB(MENU_VISIBLE),FB(FAMILY_VISIBLE),FB(ACTIVITY_VISIBLE),
        FB(FAMILY_MEMBER_VISIBLE),FB(PRIVACY_FACES_VISIBLE)};
    for(unsigned i=0;i<FRONTEND_ROUTE_COUNT;i++)visible(ui,roots[i],shown&&s_model.route==i);
    visible(ui,FB(BOTH_VISIBLE),camera&&has_devices);
    visible(ui,FB(CAMERA_ONLY_VISIBLE),camera&&!has_devices);
    visible(ui,FB(HOME_DEVICES_VISIBLE),has_devices);
    visible(ui,FB(NONE_VISIBLE),!camera&&!has_devices);
    if (shown && s_model.route == FRONTEND_HOME) update_home_bounds(ui, camera, has_devices ? count : 0);
    uint16_t canvas=0;unsigned width=0,height=0;
    frontend_camera_set_privacy(s_privacy_requested);
    if(shown&&camera&&s_model.monitor_enabled){
        if(s_model.route==FRONTEND_MONITOR)canvas=FB(MONITOR_CAMERA_CANVAS),width=480,height=480;
        else if(s_model.route==FRONTEND_HOME&&s_model.scroll_offset<231){
            canvas=has_devices?FB(HOME_CAMERA_CANVAS_BOTH):FB(HOME_CAMERA_CANVAS_CAMERA_ONLY);width=454;height=147;
        }
    }
    frontend_camera_target(ui,canvas,width,height);
    bool live=canvas&&frontend_camera_tick(ui);
    visible(ui,FB(HOME_CAMERA_CANVAS_BOTH),live&&canvas==FB(HOME_CAMERA_CANVAS_BOTH));
    visible(ui,FB(HOME_CAMERA_CANVAS_CAMERA_ONLY),live&&canvas==FB(HOME_CAMERA_CANVAS_CAMERA_ONLY));
    visible(ui,FB(MONITOR_CAMERA_CANVAS),live&&canvas==FB(MONITOR_CAMERA_CANVAS));
    bool home_both_live = live && canvas == FB(HOME_CAMERA_CANVAS_BOTH);
    bool home_camera_live = live && canvas == FB(HOME_CAMERA_CANVAS_CAMERA_ONLY);
    bool monitoring_off = !s_model.monitor_enabled;
    visible(ui, FB(HOME_CAMERA_LIVE_BOTH_VISIBLE), home_both_live);
    visible(ui, FB(HOME_CAMERA_LIVE_CAMERA_ONLY_VISIBLE), home_camera_live);
    visible(ui, FB(HOME_CAMERA_OFF_BOTH_VISIBLE), monitoring_off);
    visible(ui, FB(HOME_CAMERA_OFF_CAMERA_ONLY_VISIBLE), monitoring_off);
    visible(ui, FB(HOME_CAMERA_UNAVAILABLE_BOTH_VISIBLE), !monitoring_off && !home_both_live);
    visible(ui, FB(HOME_CAMERA_UNAVAILABLE_CAMERA_ONLY_VISIBLE), !monitoring_off && !home_camera_live);
    const char *monitor = s_privacy_requested ? "Privacy" : "Unavailable";
    text(ui, FB(MONITOR_STATE_BOTH), monitoring_off || home_both_live ? "" : monitor);
    text(ui, FB(MONITOR_STATE_CAMERA_ONLY), monitoring_off || home_camera_live ? "" : monitor);
    if (shown) {
        if (s_model.route == FRONTEND_MONITOR) render_monitor_controls(ui, live);
        render_monitor_settings(ui);
        render_values(ui);
    }
}

bool frontend_ui_back(esp_gsp_handle_t ui)
{
    if (!s_visible) return false;
    feedback("");
    if (s_model.route == FRONTEND_FAMILY_MEMBER) {
        member_back(ui);
        return true;
    }
    if (s_model.route == FRONTEND_MONITOR && s_privacy_tray) {
        s_privacy_tray = false;
        return true;
    }
    return frontend_model_back(&s_model);
}

static float slider_fraction(slider_mode_t mode, int x)
{
    if (mode == SLIDER_TEMPERATURE) return clamp_fraction((x - 41) / 396.0f);

    if (mode == SLIDER_HUE || mode == SLIDER_SATURATION)
        return clamp_fraction((x - 90) / 300.0f);

    /* The Bezier is monotonic in X. Invert it at display-pixel precision. */
    if (x <= 32) return 0.0f;
    if (x >= 449) return 1.0f;
    float low = 0.0f, high = 1.0f;
    for (int i = 0; i < 11; ++i) {
        float middle = (low + high) / 2;
        float curve_x, curve_y;
        brightness_point(middle, &curve_x, &curve_y);
        if (curve_x + 7 < x) low = middle;
        else high = middle;
    }
    return (low + high) / 2;
}

static const frontend_backend_catalog_entry_t *action_entry(unsigned slot)
{
    const char *id=s_press_ids_valid?s_pressed_card_ids[slot]:s_card_ids[slot];
    const frontend_backend_catalog_entry_t *entry=entry_by_id(id);
    if(s_press_ids_valid&&entry&&entry->generation!=s_pressed_generation[slot])return NULL;
    return entry;
}
static bool open_slot(unsigned slot)
{
    const frontend_backend_catalog_entry_t *entry=action_entry(slot);
    if(!entry||!entry->available){feedback("Device removed");return false;}
    frontend_jobs_cancel(entry->id);s_detail_press_valid=false;
    if(frontend_backend_select(entry->id)!=ESP_OK){feedback("Device unavailable");return false;}
    frontend_backend_tick(&s_snapshot);
    entry=selected_entry();
    if(!entry)return false;
    s_model.route=entry->supports_power?FRONTEND_LIGHT:FRONTEND_SENSOR;
    feedback("");return true;
}

static bool monitor_action(esp_gsp_handle_t ui, uint16_t action, unsigned arg)
{
    if (!s_model.camera) return false;
    frontend_route_t route = s_model.route;
    if (action == FA(MONITOR_OPEN)) {
        s_model.route = FRONTEND_MONITOR;
#ifdef ESP_PLATFORM
        ESP_LOGI("smart_home", "Monitor opened: enabled=%d privacy=%d",
                 s_model.monitor_enabled, s_privacy_requested);
#endif
    } else if (action == FA(MENU_OPEN) && route == FRONTEND_MONITOR) {
        s_model.route = FRONTEND_MENU;
    } else if (action == FA(MONITOR_LIVE) && route == FRONTEND_MENU) {
        s_monitor_menu = MENU_LIVE;
        s_model.route = FRONTEND_MONITOR;
    } else if (action == FA(FAMILY_OPEN) && route == FRONTEND_MENU) {
        s_monitor_menu = MENU_FAMILY;
        s_model.route = FRONTEND_FAMILY;
    } else if (action == FA(ACTIVITY_OPEN) && route == FRONTEND_MENU) {
        s_monitor_menu = MENU_ACTIVITY;
        s_model.route = FRONTEND_ACTIVITY;
    } else if (action == FA(MONITOR_TOGGLE) && route == FRONTEND_MONITOR) {
        s_model.monitor_enabled = !s_model.monitor_enabled;
        s_privacy_tray = false;
        save_monitor_policy();
    } else if ((action == FA(MONITOR_NORMAL) || action == FA(MONITOR_PRIVACY)) &&
               route == FRONTEND_MONITOR && s_model.monitor_enabled) {
        s_privacy_requested = action == FA(MONITOR_PRIVACY);
        s_privacy_tray = false;
        save_monitor_policy();
    } else if (action == FA(PRIVACY_OPEN) && route == FRONTEND_MONITOR &&
               s_model.monitor_enabled && s_privacy_requested) {
        s_privacy_tray = !s_privacy_tray;
    } else if (route == FRONTEND_MONITOR && s_privacy_tray &&
               (action == FA(PRIVACY_ALL_FACES) || action == FA(PRIVACY_SELECT_FACES) ||
                action == FA(PRIVACY_BACKGROUND))) {
        if (action != FA(PRIVACY_ALL_FACES)) {
            feedback("Only all-face mosaic is available");
            return true;
        }
        s_privacy_mode = PRIVACY_ALL;
        s_privacy_tray = false;
        save_monitor_policy();
    } else if (route == FRONTEND_PRIVACY_FACES &&
               (action == FA(PRIVACY_FACES_SCREEN) || action == FA(PRIVACY_FACES_SAVED))) {
        s_faces_saved = action == FA(PRIVACY_FACES_SAVED);
    } else if (action == FA(PRIVACY_FACES_BACK) && route == FRONTEND_PRIVACY_FACES) {
        s_model.route = FRONTEND_MONITOR;
    } else if (action == FA(FAMILY_ADD) && (route == FRONTEND_FAMILY || route == FRONTEND_PRIVACY_FACES)) {
        if (family_count() == FAMILY_CAPACITY) {
            feedback("Up to 3 members");
            return true;
        }
        s_member_origin = route;
        s_model.route = FRONTEND_FAMILY_MEMBER;
    } else if (action == FA(FAMILY_MEMBER_BACK) && route == FRONTEND_FAMILY_MEMBER) {
        member_back(ui);
    } else if (action == FA(FAMILY_MEMBER_NAME) && route == FRONTEND_FAMILY_MEMBER) {
        if (!s_member_keyboard) {
            s_member_keyboard = esp_gsp_keyboard_attach_ex(ui, FA(FAMILY_MEMBER_KEYBOARD_KEY),
                FB(FAMILY_MEMBER_DRAFT), MEMBER_NAME_BYTES) == ESP_GSP_OK;
            if (!s_member_keyboard) {
                feedback("Keyboard unavailable");
                return true;
            }
            (void)esp_gsp_set_cursor(ui, FB(FAMILY_MEMBER_DRAFT));
        }
    } else if (route == FRONTEND_FAMILY_MEMBER &&
               (action == FA(FAMILY_MEMBER_SAVE) || (action == FA(FAMILY_MEMBER_KEYBOARD_KEY) && arg == 13))) {
        char draft[MEMBER_NAME_BYTES + 1] = "";
        unsigned count = family_count();
        if (count < FAMILY_CAPACITY && member_name(ui, draft)) {
            snprintf(s_family[count], sizeof(s_family[count]), "%s", draft);
            save_family();
            member_back(ui);
        }
    } else if (route == FRONTEND_FAMILY) {
        static const uint16_t deletes[] = {FA(FAMILY_DELETE_0), FA(FAMILY_DELETE_1), FA(FAMILY_DELETE_2)};
        unsigned count = family_count();
        for (unsigned i = 0; i < count; ++i) {
            if (action != deletes[i]) continue;
            if (i + 1 < count) memmove(s_family[i], s_family[i + 1], (count - i - 1) * sizeof(s_family[i]));
            memset(s_family[count - 1], 0, sizeof(s_family[0]));
            save_family();
            return true;
        }
        return false;
    } else {
        return false;
    }
    feedback("");
    return true;
}

bool frontend_ui_event(esp_gsp_handle_t ui,const mosaic_event_t *event)
{
    (void)ui;if(!s_visible)return false;
    if(event->type==MOSAIC_EVENT_POINTER){
        if(event->data.pointer.pressed&&!s_pointer_down){
            s_pointer_y=event->data.pointer.y;s_dragged=false;
            s_scroll_origin=s_model.scroll_offset;
            s_scroll_candidate=s_model.route==FRONTEND_HOME&&s_pointer_y>=64;s_slider_mode=SLIDER_NONE;s_slider_pending=false;
            memcpy(s_pressed_card_ids,s_card_ids,sizeof(s_card_ids));s_press_ids_valid=true;
            for(unsigned i=0;i<HOME_CARD_CAPACITY;i++){const frontend_backend_catalog_entry_t *item=entry_by_id(s_card_ids[i]);s_pressed_generation[i]=item?item->generation:0;}
            const frontend_backend_catalog_entry_t *entry=selected_entry();
            s_detail_press_valid=entry&&(s_model.route==FRONTEND_LIGHT||s_model.route==FRONTEND_COLOUR);
            if(s_detail_press_valid){snprintf(s_detail_press_id,sizeof(s_detail_press_id),"%s",entry->id);s_detail_press_generation=entry->generation;}
            if(s_model.route==FRONTEND_LIGHT&&entry){
                int x=event->data.pointer.x,y=event->data.pointer.y;
                if (temperature_bounds(entry) && x >= 30 && x <= 448 && y >= 268 && y <= 302)
                    s_slider_mode = SLIDER_TEMPERATURE;
                else if (entry->supports_brightness && x >= 15 && x <= 465 &&
                         y >= 354 && y <= 472 && !(y >= 416 && x >= 106 && x <= 350))
                    s_slider_mode = SLIDER_BRIGHTNESS;
            } else if (s_model.route == FRONTEND_COLOUR && entry && entry->supports_color) {
                int x = event->data.pointer.x, y = event->data.pointer.y;
                if (x >= 76 && x <= 404 && y >= 208 && y <= 252) s_slider_mode = SLIDER_HUE;
                if (x >= 76 && x <= 404 && y >= 356 && y <= 400) s_slider_mode = SLIDER_SATURATION;
            }
        } else if (s_pointer_down && s_scroll_candidate && s_model.route == FRONTEND_HOME) {
            int delta = s_pointer_y - event->data.pointer.y;
            if (delta > 8 || delta < -8) s_dragged = true;
            if (s_dragged) {
                int offset = (int)s_scroll_origin + delta;
                s_model.scroll_offset = offset < 0 ? 0 :
                    (unsigned)offset > s_scroll_limit ? s_scroll_limit : (unsigned)offset;
                paint_home_scroll(ui);
            }
        }
        bool colour_slider = s_slider_mode == SLIDER_HUE || s_slider_mode == SLIDER_SATURATION;
        bool slider_here = s_slider_mode != SLIDER_NONE &&
            s_model.route == (colour_slider ? FRONTEND_COLOUR : FRONTEND_LIGHT);
        bool released_slider = !event->data.pointer.pressed && s_pointer_down && slider_here;
        s_pointer_down = event->data.pointer.pressed;
        if (!s_pointer_down) s_scroll_candidate = false;
        if (slider_here && (s_pointer_down || released_slider) && detail_identity_valid()) {
            const frontend_backend_catalog_entry_t *entry = selected_entry();
            s_slider_fraction = slider_fraction(s_slider_mode, event->data.pointer.x);
            if (s_pointer_down) {
                if (colour_slider) render_colour_controls(ui, entry);
                else render_light_controls(ui, entry);
            } else if (released_slider && colour_slider && entry && entry->supports_color) {
                int value = (int)lroundf(s_slider_fraction * 254);
                frontend_backend_command_t kind = s_slider_mode == SLIDER_HUE ?
                    FRONTEND_COMMAND_HUE : FRONTEND_COMMAND_SATURATION;
                s_slider_pending = command(FRONTEND_DEVICE_LIGHT, kind, value, true);
            } else if (released_slider && s_slider_mode == SLIDER_TEMPERATURE && temperature_bounds(entry))
                s_slider_pending = command(FRONTEND_DEVICE_LIGHT, FRONTEND_COMMAND_KELVIN,
                                           kelvin_at(entry, s_slider_fraction), true);
            else if (released_slider && s_slider_mode == SLIDER_BRIGHTNESS) {
                int value = (int)lroundf(s_slider_fraction * 100);
                s_slider_pending = command(FRONTEND_DEVICE_LIGHT, FRONTEND_COMMAND_BRIGHTNESS,
                                           value < 1 ? 1 : value, true);
            }
        }
        return released_slider;
    }
    if(event->type!=MOSAIC_EVENT_UI_CALL)return false;
    uint16_t action=event->data.call.action_id;
    if(s_dragged){s_dragged=false;s_press_ids_valid=false;return true;}
    static const uint16_t open_actions[] = {FA(SLOT_0_OPEN), FA(SLOT_1_OPEN), FA(SLOT_2_OPEN), FA(SLOT_3_OPEN), FA(SLOT_4_OPEN), FA(SLOT_5_OPEN), FA(SLOT_6_OPEN), FA(SLOT_7_OPEN), FA(SLOT_8_OPEN), FA(SLOT_9_OPEN), FA(SLOT_10_OPEN), FA(SLOT_11_OPEN), FA(SLOT_12_OPEN), FA(SLOT_13_OPEN), FA(SLOT_14_OPEN), FA(SLOT_15_OPEN)};
    static const uint16_t toggle_actions[] = {FA(SLOT_0_TOGGLE), FA(SLOT_1_TOGGLE), FA(SLOT_2_TOGGLE), FA(SLOT_3_TOGGLE), FA(SLOT_4_TOGGLE), FA(SLOT_5_TOGGLE), FA(SLOT_6_TOGGLE), FA(SLOT_7_TOGGLE), FA(SLOT_8_TOGGLE), FA(SLOT_9_TOGGLE), FA(SLOT_10_TOGGLE), FA(SLOT_11_TOGGLE), FA(SLOT_12_TOGGLE), FA(SLOT_13_TOGGLE), FA(SLOT_14_TOGGLE), FA(SLOT_15_TOGGLE)};
    for(unsigned slot=0;slot<HOME_CARD_CAPACITY;slot++)if(action==open_actions[slot]||action==toggle_actions[slot]){
        if(action==open_actions[slot])(void)open_slot(slot);
        else {const frontend_backend_catalog_entry_t *entry=action_entry(slot);(void)toggle_id(entry?entry->id:"");}
        s_press_ids_valid=false;return true;
    }
    s_press_ids_valid=false;
    if (monitor_action(ui, action, event->data.call.arg)) return true;
    if(action==FA(BACK)){(void)frontend_ui_back(ui);}
    else if(action==FA(HOME_OPEN))s_model.route=FRONTEND_HOME;
    else if(action==FA(COLOUR_OPEN)&&s_model.route==FRONTEND_LIGHT){
        if(selected_entry()&&selected_entry()->supports_color){
            s_model.route=FRONTEND_COLOUR;
#ifdef ESP_PLATFORM
            ESP_LOGI("smart_home", "Light to Colour");
#endif
        }
    }
    else if(action==FA(COLOUR_BACK)&&s_model.route==FRONTEND_COLOUR){
        s_model.route=FRONTEND_LIGHT;
#ifdef ESP_PLATFORM
        ESP_LOGI("smart_home", "Colour to Light");
#endif
    }
    else if(action==FA(AUTOMATION_TOGGLE)){
        s_automation_preference=!s_automation_preference;
        s_model.automation_active=s_automation_preference;
        if(!s_automation_preference)frontend_jobs_cancel_automatic();
        save_monitor_policy();
        feedback(s_automation_preference?(s_snapshot.pir_known&&s_snapshot.ldr_known?"Automation active; waiting for motion":"Automation active; waiting for sensor data"):"Automation off");
    }
    else if(action==FA(LIGHT_TOGGLE))(void)toggle_id(s_snapshot.selected_id);
    else if(action==FA(ALL_TOGGLE)){
        bool turn_on = !home_any_power_on();
        bool queued = false;
        frontend_model_manual(&s_model,3);
        for(size_t i=0;i<s_snapshot.catalog_count;i++)if(s_snapshot.catalog[i].supports_power)
            queued|=frontend_jobs_queue(&s_snapshot.catalog[i],FRONTEND_JOB_POWER,turn_on,0,0,false);
        if (queued) {
            feedback("Sending...");
            frontend_jobs_tick(&s_snapshot);
        } else {
            feedback("No supported online device");
        }
    }
    else if(action==FA(LIGHT_BRIGHTNESS_MINUS)||action==FA(LIGHT_BRIGHTNESS_PLUS)){
        const frontend_backend_catalog_entry_t *entry=selected_entry();
        if(!entry||!entry->brightness_known)feedback("Brightness is unavailable");
        else{int level=entry->brightness_percent+(action==FA(LIGHT_BRIGHTNESS_PLUS)?5:-5);
            (void)command(FRONTEND_DEVICE_LIGHT,FRONTEND_COMMAND_BRIGHTNESS,level<1?1:level>100?100:level,true);}
    }
    else if(action==FA(LIGHT_PRESET_NIGHT)||action==FA(LIGHT_PRESET_RELAX)||action==FA(LIGHT_PRESET_READING)||action==FA(LIGHT_PRESET_GAMING)){
        const frontend_backend_catalog_entry_t *entry=selected_entry();
        int level=action==FA(LIGHT_PRESET_NIGHT)?10:action==FA(LIGHT_PRESET_RELAX)?40:action==FA(LIGHT_PRESET_READING)?80:65;
        int kelvin=action==FA(LIGHT_PRESET_NIGHT)?2700:action==FA(LIGHT_PRESET_RELAX)?3000:action==FA(LIGHT_PRESET_READING)?4000:4500;
        if(!detail_identity_valid()){feedback("Device changed; try again");return true;}
        if(entry&&entry->supports_brightness&&entry->supports_kelvin){
            frontend_model_manual(&s_model,1);
            (void)frontend_jobs_queue(entry,FRONTEND_JOB_LEVEL|FRONTEND_JOB_KELVIN,false,level,kelvin,false);
            frontend_jobs_tick(&s_snapshot);feedback("Sending...");
        }else feedback("This light does not support presets");
    }
    else if(action==FA(HUE_MINUS)||action==FA(HUE_PLUS)||action==FA(SATURATION_MINUS)||action==FA(SATURATION_PLUS)){
        const frontend_backend_catalog_entry_t *entry=selected_entry();
        bool hue=action==FA(HUE_MINUS)||action==FA(HUE_PLUS);
        if(!entry||(hue?!entry->hue_known:!entry->saturation_known)){feedback("Color value is unavailable");return true;}
        int value=(hue?entry->hue:entry->saturation)+((action==FA(HUE_PLUS)||action==FA(SATURATION_PLUS))?5:-5);
        if (value < 0) value = 0;
        if (value > 254) value = 254;
        (void)command(FRONTEND_DEVICE_LIGHT,hue?FRONTEND_COMMAND_HUE:FRONTEND_COMMAND_SATURATION,value,true);
    }
    else return false;
    return true;
}

void frontend_ui_stop(esp_gsp_handle_t ui)
{
    member_keyboard_close(ui);
    frontend_camera_stop(ui);frontend_jobs_init();s_visible=false;
}
