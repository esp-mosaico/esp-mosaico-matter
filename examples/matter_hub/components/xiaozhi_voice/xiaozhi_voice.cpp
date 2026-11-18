/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Session and transport come from espressif/esp_xiaozhi. This file owns the
 * hub state machine: Talk, wake word, Mosaic status, and when uplink runs.
 */
#include "xiaozhi_voice.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mcp_engine.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hub_audio.h"
#include "hub_matter_tools.h"
#include "hub_wake.h"
#include "model_path.h"

#define TAG "xz_voice"
#define SESSION_STACK 16384
#define LINK_STACK 8192
#define BIT_ONLINE (1 << 0)
#define BIT_OFFLINE (1 << 1)
#define BIT_REWAKE (1 << 2)
#define BIT_WAKE (1 << 3)
#define BIT_QUIET_CLOSE (1 << 4)
#define BIT_CHANNEL_CLOSED (1 << 5)
#define BIT_NET_UP (1 << 0)

extern "C" void mosaic_xiaozhi_set_status(const char *status);
extern "C" void mosaic_xiaozhi_set_notice(const char *notice);
extern "C" void mosaic_xiaozhi_set_emotion(const char *emotion);
extern "C" void mosaic_xiaozhi_set_chat(const char *role, const char *content);

static void *s_matter;
static srmodel_list_t *s_models;
static esp_xiaozhi_chat_handle_t s_chat;
static EventGroupHandle_t s_link;
static EventGroupHandle_t s_net;
static bool s_task_started;
static bool s_ready;
static bool s_listening;
static bool s_transport_up;
static bool s_transport_events_registered;
static bool s_transport_seen_up;
static bool s_audio_failed;
/* Set around open_audio_channel() so a close that arrives before open
 * returns cannot be overwritten by the success path. */
static bool s_opening;
static bool s_closed_while_opening;
static portMUX_TYPE s_listen_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_activating;
static char s_activation_code[16];
static char s_activation_line[192];
static char s_transport[16];
static char s_last_error[96];

static const char *kOfflineHint = "Server not connected";
static const char *kNoWifiHint = "Wi-Fi not connected";

static bool listening_now(void)
{
    bool value;
    portENTER_CRITICAL(&s_listen_mux);
    value = s_listening;
    portEXIT_CRITICAL(&s_listen_mux);
    return value;
}

static bool activating_now(void)
{
    bool value;
    portENTER_CRITICAL(&s_listen_mux);
    value = s_activating;
    portEXIT_CRITICAL(&s_listen_mux);
    return value;
}

static bool voice_ready_link(EventGroupHandle_t *out_link)
{
    bool ready;
    portENTER_CRITICAL(&s_listen_mux);
    ready = s_ready && s_link != nullptr;
    if (out_link != nullptr) {
        *out_link = s_link;
    }
    portEXIT_CRITICAL(&s_listen_mux);
    return ready;
}

static bool transport_up_now(void)
{
    bool value;
    portENTER_CRITICAL(&s_listen_mux);
    value = s_transport_up;
    portEXIT_CRITICAL(&s_listen_mux);
    return value;
}

static void set_transport_up(bool up)
{
    portENTER_CRITICAL(&s_listen_mux);
    s_transport_up = up;
    if (up) {
        s_transport_seen_up = true;
    }
    portEXIT_CRITICAL(&s_listen_mux);
}

static void set_last_error(const char *error)
{
    const char *next = error != nullptr ? error : "";
    portENTER_CRITICAL(&s_listen_mux);
    strlcpy(s_last_error, next, sizeof(s_last_error));
    portEXIT_CRITICAL(&s_listen_mux);
}

static void mark_listening_closed(void)
{
    portENTER_CRITICAL(&s_listen_mux);
    s_listening = false;
    if (s_opening) {
        s_closed_while_opening = true;
    }
    portEXIT_CRITICAL(&s_listen_mux);
    hub_audio_set_uplink(false);
    hub_audio_stop_playback();
}

static void begin_open(void)
{
    portENTER_CRITICAL(&s_listen_mux);
    s_closed_while_opening = false;
    s_opening = true;
    portEXIT_CRITICAL(&s_listen_mux);
}

static void cancel_open(void)
{
    portENTER_CRITICAL(&s_listen_mux);
    s_opening = false;
    portEXIT_CRITICAL(&s_listen_mux);
}

/* Publish listening only when no close landed during this open. */
static bool commit_listening(void)
{
    portENTER_CRITICAL(&s_listen_mux);
    bool closed = s_closed_while_opening;
    s_opening = false;
    if (!closed) {
        s_listening = true;
    }
    portEXIT_CRITICAL(&s_listen_mux);
    if (closed) {
        hub_audio_set_uplink(false);
        return false;
    }
    hub_audio_set_uplink(true);
    if (!listening_now()) {
        hub_audio_set_uplink(false);
        return false;
    }
    return true;
}

static void show_standby_line(const char *text)
{
    mosaic_xiaozhi_set_chat("system", text);
}

static void show_activation_hold(void)
{
    char code[sizeof(s_activation_code)];
    char line[sizeof(s_activation_line)];
    portENTER_CRITICAL(&s_listen_mux);
    strlcpy(code, s_activation_code, sizeof(code));
    strlcpy(line, s_activation_line, sizeof(line));
    portEXIT_CRITICAL(&s_listen_mux);

    mosaic_xiaozhi_set_status(code[0] != '\0' ? code : "Activation");
    mosaic_xiaozhi_set_emotion("neutral");
    show_standby_line(line[0] != '\0' ? line : code);
}

static bool sta_has_ip(void)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip = {};
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* Idle copy only. A live listening session keeps its own status. */
static void show_link_state(void)
{
    if (listening_now()) {
        return;
    }
    if (activating_now()) {
        show_activation_hold();
        return;
    }
    if (!sta_has_ip()) {
        mosaic_xiaozhi_set_status("No Wi-Fi");
        mosaic_xiaozhi_set_emotion("sad");
        show_standby_line(kNoWifiHint);
        return;
    }
    if (!transport_up_now()) {
        mosaic_xiaozhi_set_status("No server");
        mosaic_xiaozhi_set_emotion("sad");
        show_standby_line(kOfflineHint);
        return;
    }
    mosaic_xiaozhi_set_status("Ready");
    mosaic_xiaozhi_set_emotion("neutral");
    const char *phrase = hub_wake_phrase();
    if (phrase[0] == '\0') {
        show_standby_line("Say the wake word to wake me");
        return;
    }
    char line[80];
    snprintf(line, sizeof(line), "Say \"%s\" to wake me", phrase);
    show_standby_line(line);
}

static void on_wake(const char *phrase, void *ctx)
{
    (void)phrase;
    (void)ctx;
    EventGroupHandle_t link = nullptr;
    if (!voice_ready_link(&link)) {
        return;
    }
    if (listening_now()) {
        xEventGroupSetBits(link, BIT_REWAKE | BIT_WAKE);
    } else {
        xEventGroupSetBits(link, BIT_ONLINE | BIT_WAKE);
    }
}

static esp_err_t close_audio_channel(void)
{
    mark_listening_closed();
    xEventGroupClearBits(s_link, BIT_CHANNEL_CLOSED);
    esp_err_t err = esp_xiaozhi_chat_close_audio_channel(s_chat);
    if (err != ESP_OK) {
        return err;
    }
    /* esp_xiaozhi 0.1.4 posts AUDIO_CHANNEL_CLOSED before close returns.
     * Its event has no session id. Drain that notification on the event task
     * before allowing the link task to begin another open. */
    xEventGroupWaitBits(s_link, BIT_CHANNEL_CLOSED, pdTRUE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

static void link_task(void *arg)
{
    (void)arg;
    const esp_xiaozhi_chat_audio_t hello = {
        .format = "opus",
        .sample_rate = 16000,
        .channels = 1,
        .frame_duration = 60,
    };
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            s_link, BIT_ONLINE | BIT_OFFLINE | BIT_REWAKE | BIT_WAKE | BIT_QUIET_CLOSE,
            pdTRUE, pdFALSE, portMAX_DELAY);
        if (s_chat == 0) {
            continue;
        }
        if ((bits & BIT_OFFLINE) != 0) {
            esp_err_t err = close_audio_channel();
            if (err != ESP_OK) {
                set_last_error(esp_err_to_name(err));
                show_link_state();
                continue;
            }
            ESP_LOGI(TAG, "audio channel closed");
            /* WaitBits clears every bit it reports. A start that arrived in
             * the same wake must still be applied after the close. */
            if ((bits & (BIT_ONLINE | BIT_REWAKE)) == 0) {
                if ((bits & BIT_QUIET_CLOSE) == 0) {
                    show_link_state();
                }
                continue;
            }
        }
        const char *phrase = "";
        if ((bits & BIT_WAKE) != 0) {
            phrase = hub_wake_phrase();
        }
        if ((bits & BIT_REWAKE) != 0 && !listening_now()) {
            bits |= BIT_ONLINE;
        }
        if ((bits & BIT_REWAKE) != 0 && listening_now()) {
            hub_audio_stop_playback();
            mosaic_xiaozhi_set_emotion("thinking");
            esp_xiaozhi_chat_send_abort_speaking(s_chat,
                                                 ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
            if (phrase[0] != '\0') {
                esp_xiaozhi_chat_send_wake_word(s_chat, phrase);
            }
            esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);
            mosaic_xiaozhi_set_status("Listening");
            continue;
        }
        if ((bits & BIT_ONLINE) == 0) {
            continue;
        }
        if (!sta_has_ip() || !transport_up_now()) {
            ESP_LOGW(TAG, "wake ignored, link down");
            show_link_state();
            continue;
        }
        /* Show thinking before the channel opens so a greeting emotion that
         * arrives during open replaces it, instead of the reverse. */
        mosaic_xiaozhi_set_emotion("thinking");
        begin_open();
        esp_err_t err = esp_xiaozhi_chat_open_audio_channel(s_chat, &hello, nullptr, 0);
        if (err != ESP_OK) {
            cancel_open();
            set_last_error(esp_err_to_name(err));
            ESP_LOGW(TAG, "open audio channel: %s", esp_err_to_name(err));
            show_link_state();
            continue;
        }
        if (phrase[0] != '\0') {
            err = esp_xiaozhi_chat_send_wake_word(s_chat, phrase);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "send wake word: %s", esp_err_to_name(err));
            }
        }
        err = esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_REALTIME);
        if (err != ESP_OK) {
            cancel_open();
            set_last_error(esp_err_to_name(err));
            ESP_LOGW(TAG, "start listening: %s", esp_err_to_name(err));
            close_audio_channel();
            mosaic_xiaozhi_set_status("Error");
            mosaic_xiaozhi_set_emotion("sad");
            continue;
        }
        if (!commit_listening()) {
            ESP_LOGW(TAG, "audio channel closed while opening");
            show_link_state();
            continue;
        }
        set_last_error("");
        mosaic_xiaozhi_set_status("Listening");
        if (!listening_now()) {
            show_link_state();
            continue;
        }
        ESP_LOGI(TAG, "listening%s%s", phrase[0] != '\0' ? ", wake word " : "", phrase);
    }
}

static void on_chat_event(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    (void)ctx;
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        auto *text = static_cast<esp_xiaozhi_chat_text_data_t *>(event_data);
        if (text != nullptr && text->text != nullptr) {
            const char *role =
                text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER ? "user" : "assistant";
            ESP_LOGI(TAG, "chat %s: %s", role, text->text);
            mosaic_xiaozhi_set_chat(role, text->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        if (event_data != nullptr) {
            const char *emotion = static_cast<const char *>(event_data);
            ESP_LOGI(TAG, "emotion: %s", emotion);
            mosaic_xiaozhi_set_emotion(emotion);
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        auto *tts = static_cast<esp_xiaozhi_chat_tts_state_t *>(event_data);
        if (tts == nullptr) {
            break;
        }
        if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            mosaic_xiaozhi_set_status("Speaking");
            hub_audio_set_tts_active(true);
        } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            hub_audio_set_tts_active(false);
            if (listening_now()) {
                mosaic_xiaozhi_set_status("Listening");
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        mosaic_xiaozhi_set_status("Speaking");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        if (listening_now()) {
            mosaic_xiaozhi_set_status("Listening");
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        if (event_data != nullptr && strcmp(static_cast<const char *>(event_data), "reboot") == 0) {
            ESP_LOGI(TAG, "server asked for reboot");
            esp_restart();
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        auto *info = static_cast<esp_xiaozhi_chat_error_info_t *>(event_data);
        char err_buf[96];
        if (info != nullptr && info->source != nullptr && info->source[0] != '\0') {
            snprintf(err_buf, sizeof(err_buf), "%s (%s)",
                     esp_err_to_name(info->code), info->source);
        } else {
            snprintf(err_buf, sizeof(err_buf), "%s",
                     info != nullptr ? esp_err_to_name(info->code) : "unknown");
        }
        set_last_error(err_buf);
        ESP_LOGE(TAG, "chat error: %s", err_buf);
        mosaic_xiaozhi_set_status("Error");
        mosaic_xiaozhi_set_notice("Voice error");
        mosaic_xiaozhi_set_emotion("sad");
        break;
    }
    default:
        break;
    }
}

static void on_downlink(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    hub_audio_on_downlink(data, len);
}

static void on_transport(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        set_last_error("");
        set_transport_up(true);
        show_link_state();
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        set_transport_up(false);
        mark_listening_closed();
        show_link_state();
        {
            EventGroupHandle_t link = nullptr;
            if (voice_ready_link(&link)) {
                xEventGroupSetBits(link, BIT_OFFLINE);
            }
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "audio channel opened");
        set_last_error("");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED: {
        ESP_LOGI(TAG, "audio channel closed by server");
        mark_listening_closed();
        show_link_state();
        EventGroupHandle_t link = nullptr;
        voice_ready_link(&link);
        if (link != nullptr) {
            xEventGroupSetBits(link, BIT_CHANNEL_CLOSED);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "server goodbye");
        mark_listening_closed();
        mosaic_xiaozhi_set_status("Goodbye");
        mosaic_xiaozhi_set_emotion("neutral");
        {
            EventGroupHandle_t link = nullptr;
            if (voice_ready_link(&link)) {
                xEventGroupSetBits(link, BIT_OFFLINE | BIT_QUIET_CLOSE);
            }
        }
        break;
    default:
        break;
    }
}

static bool code_only_activation(const esp_xiaozhi_chat_info_t *info)
{
    // Ignore the activation challenge
    return info->has_activation_code && info->activation_code != nullptr;
}

static void show_activation(const esp_xiaozhi_chat_info_t *info)
{
    if (!code_only_activation(info)) {
        portENTER_CRITICAL(&s_listen_mux);
        s_activating = false;
        portEXIT_CRITICAL(&s_listen_mux);
        return;
    }
    char formatted_line[sizeof(s_activation_line)];
    size_t n = 0;
    const char *raw_msg = info->activation_message != nullptr
        ? info->activation_message : info->activation_code;
    while (*raw_msg != '\0' && n + 1 < sizeof(formatted_line)) {
        char c = *raw_msg++;
        if (c == '\n' || c == '\r') {
            if (n == 0 || formatted_line[n - 1] == ' ') {
                continue;
            }
            c = ' ';
        }
        formatted_line[n++] = c;
    }
    formatted_line[n] = '\0';

    bool changed = false;
    portENTER_CRITICAL(&s_listen_mux);
    if (strcmp(s_activation_code, info->activation_code) != 0) {
        strlcpy(s_activation_code, info->activation_code, sizeof(s_activation_code));
        strlcpy(s_activation_line, formatted_line, sizeof(s_activation_line));
        changed = true;
    }
    s_activating = true;
    portEXIT_CRITICAL(&s_listen_mux);

    if (changed) {
        ESP_LOGI(TAG, "activation code: %s", info->activation_code);
        if (info->activation_message != nullptr) {
            ESP_LOGI(TAG, "activation: %s", info->activation_message);
        }
    }
    show_activation_hold();
}

/* The code is entered on the Xiaozhi site. Poll until that response no
 * longer includes an activation code. A challenge is left for later. */
static void wait_for_code_activation(esp_xiaozhi_chat_info_t *info)
{
    while (code_only_activation(info)) {
        show_activation(info);
        if (!sta_has_ip()) {
            ESP_LOGI(TAG, "waiting for Wi-Fi before XiaoZhi server info");
            xEventGroupWaitBits(s_net, BIT_NET_UP, pdFALSE, pdTRUE, portMAX_DELAY);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_err_t err = esp_xiaozhi_chat_get_info(info);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "server info: %s", esp_err_to_name(err));
        }
    }
    portENTER_CRITICAL(&s_listen_mux);
    s_activating = false;
    s_activation_code[0] = '\0';
    s_activation_line[0] = '\0';
    portEXIT_CRITICAL(&s_listen_mux);
}

static esp_err_t ensure_transport_events_registered(void)
{
    if (s_transport_events_registered) {
        return ESP_OK;
    }
    esp_err_t err = esp_event_handler_register(
        ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, on_transport, nullptr);
    if (err == ESP_OK) {
        s_transport_events_registered = true;
    }
    return err;
}

static esp_err_t start_session(void)
{
    if (s_chat != 0) {
        esp_err_t err = ensure_transport_events_registered();
        if (err == ESP_OK) {
            err = esp_xiaozhi_chat_start(s_chat);
        }
        if (err != ESP_OK) {
            set_last_error(esp_err_to_name(err));
        }
        return err;
    }
    esp_xiaozhi_chat_info_t info = {};
    /* One attempt. session_task retries after Wi-Fi comes up; a fixed
     * three-try loop at boot gives up before provisioning finishes. */
    esp_err_t err = esp_xiaozhi_chat_get_info(&info);
    if (err != ESP_OK) {
        esp_xiaozhi_chat_free_info(&info);
        set_last_error(esp_err_to_name(err));
        ESP_LOGW(TAG, "server info: %s", esp_err_to_name(err));
        show_link_state();
        return err;
    }

    wait_for_code_activation(&info);
    if (!info.has_mqtt_config && !info.has_websocket_config) {
        esp_xiaozhi_chat_free_info(&info);
        set_last_error("missing server transport");
        ESP_LOGW(TAG, "server info contained neither MQTT nor WebSocket config");
        show_link_state();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "server transport mqtt=%d websocket=%d", info.has_mqtt_config,
             info.has_websocket_config);
    portENTER_CRITICAL(&s_listen_mux);
    strlcpy(s_transport,
            info.has_mqtt_config ? "mqtt" : "websocket",
            sizeof(s_transport));
    s_last_error[0] = '\0';
    portEXIT_CRITICAL(&s_listen_mux);

    esp_mcp_t *mcp = nullptr;
    err = esp_mcp_create(&mcp);
    if (err != ESP_OK) {
        esp_xiaozhi_chat_free_info(&info);
        set_last_error(esp_err_to_name(err));
        return err;
    }
    err = hub_matter_register_tools(mcp, s_matter);
    if (err != ESP_OK) {
        esp_xiaozhi_chat_free_info(&info);
        esp_mcp_destroy(mcp);
        set_last_error(esp_err_to_name(err));
        return err;
    }

    esp_xiaozhi_chat_config_t config = {};
    config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    config.audio_callback = on_downlink;
    config.event_callback = on_chat_event;
    config.mcp_engine = mcp;
    config.owns_mcp_engine = true;
    config.has_mqtt_config = info.has_mqtt_config;
    config.has_websocket_config = info.has_websocket_config;
    esp_xiaozhi_chat_free_info(&info);

    err = esp_xiaozhi_chat_init(&config, &s_chat);
    if (err != ESP_OK) {
        /* owns_mcp_engine is set before register_instance(). That failure
         * deinits the chat and destroys the engine, then returns
         * ESP_ERR_INVALID_STATE. Earlier failures have not taken ownership. */
        if (err != ESP_ERR_INVALID_STATE) {
            esp_mcp_destroy(mcp);
        }
        s_chat = 0;
        set_last_error(esp_err_to_name(err));
        return err;
    }
    err = ensure_transport_events_registered();
    if (err != ESP_OK) {
        set_last_error(esp_err_to_name(err));
        return err;
    }
    err = esp_xiaozhi_chat_start(s_chat);
    if (err != ESP_OK) {
        set_last_error(esp_err_to_name(err));
        ESP_LOGE(TAG, "chat start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "XiaoZhi session started");
    return ESP_OK;
}

static void session_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_FAIL;
    while (err != ESP_OK) {
        if (!sta_has_ip()) {
            ESP_LOGI(TAG, "waiting for Wi-Fi before XiaoZhi server info");
            xEventGroupWaitBits(s_net, BIT_NET_UP, pdFALSE, pdTRUE, portMAX_DELAY);
            continue;
        }
        /* Clear BIT_NET_UP before the attempt so a failed call waits the full
         * 10 s backoff unless a fresh IP_EVENT_STA_GOT_IP arrives meanwhile. */
        xEventGroupClearBits(s_net, BIT_NET_UP);
        err = start_session();
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "XiaoZhi session did not start: %s", esp_err_to_name(err));
        xEventGroupWaitBits(s_net, BIT_NET_UP, pdTRUE, pdTRUE, pdMS_TO_TICKS(10000));
    }

    EventGroupHandle_t link = xEventGroupCreate();
    portENTER_CRITICAL(&s_listen_mux);
    s_link = link;
    portEXIT_CRITICAL(&s_listen_mux);
    if (link == nullptr ||
        xTaskCreateWithCaps(link_task, "xz_link", LINK_STACK, nullptr, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "link task was not created");
        set_last_error(esp_err_to_name(ESP_ERR_NO_MEM));
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    err = hub_wake_start(s_models, on_wake, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wake word is off: %s", esp_err_to_name(err));
    }
    err = hub_audio_start(s_chat);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_listen_mux);
        s_audio_failed = true;
        strlcpy(s_last_error, esp_err_to_name(err), sizeof(s_last_error));
        portEXIT_CRITICAL(&s_listen_mux);
        ESP_LOGE(TAG, "audio path did not start: %s", esp_err_to_name(err));
        mosaic_xiaozhi_set_status("No audio");
        mosaic_xiaozhi_set_emotion("sad");
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    portENTER_CRITICAL(&s_listen_mux);
    s_ready = true;
    portEXIT_CRITICAL(&s_listen_mux);
    show_link_state();
    ESP_LOGI(TAG, "XiaoZhi voice ready");
    vTaskDeleteWithCaps(nullptr);
}

static void preload_speech_models(void)
{
    /* esp_partition_mmap() freezes the external cache. This caller still has
     * an internal stack. The WakeNet task later reuses the mapping. */
    const esp_partition_t *model = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (model == nullptr) {
        ESP_LOGW(TAG, "no model partition; wake word is off. Use Talk or xiaozhi_talk");
        return;
    }
    s_models = esp_srmodel_init("model");
    if (s_models == nullptr || s_models->num <= 0) {
        s_models = nullptr;
        ESP_LOGW(TAG, "model partition has no WakeNet image");
        return;
    }
    ESP_LOGI(TAG, "preloaded %d speech model(s)", s_models->num);
}

static int xiaozhi_talk_command(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xiaozhi_voice_toggle_chat();
    return 0;
}

extern "C" esp_err_t xiaozhi_voice_set_mixer_timing(uint32_t frame_ms, uint32_t ring_frames)
{
    portENTER_CRITICAL(&s_listen_mux);
    const bool started = s_task_started;
    portEXIT_CRITICAL(&s_listen_mux);
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame_ms == 0 || frame_ms > 1000 || ring_frames == 0 || ring_frames > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    hub_audio_set_mixer_timing(frame_ms, ring_frames);
    ESP_LOGI(TAG, "mixer timing %u ms x %u frames", (unsigned)frame_ms, (unsigned)ring_frames);
    return ESP_OK;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
            (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        if (s_net != nullptr) {
            xEventGroupClearBits(s_net, BIT_NET_UP);
        }
        show_link_state();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_net != nullptr) {
            xEventGroupSetBits(s_net, BIT_NET_UP);
        }
        show_link_state();
    }
}

extern "C" esp_err_t xiaozhi_voice_start(void *matter_manager)
{
    portENTER_CRITICAL(&s_listen_mux);
    if (s_task_started) {
        portEXIT_CRITICAL(&s_listen_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_task_started = true;
    portEXIT_CRITICAL(&s_listen_mux);

    s_matter = matter_manager;
    s_net = xEventGroupCreate();
    if (s_net == nullptr) {
        portENTER_CRITICAL(&s_listen_mux);
        s_task_started = false;
        portEXIT_CRITICAL(&s_listen_mux);
        return ESP_ERR_NO_MEM;
    }
    if (sta_has_ip()) {
        xEventGroupSetBits(s_net, BIT_NET_UP);
    }
    show_link_state();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               on_wifi, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               on_wifi, nullptr));
    ESP_LOGI(TAG, "voice heap: internal %u, psram %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    preload_speech_models();
    // esp_xiaozhi_chat_get_info() reads NVS. But it works normally, so this stack stays external.
    if (xTaskCreateWithCaps(session_task, "xiaozhi", SESSION_STACK, nullptr, 5, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        portENTER_CRITICAL(&s_listen_mux);
        s_task_started = false;
        portEXIT_CRITICAL(&s_listen_mux);
        ESP_LOGE(TAG, "failed to create the XiaoZhi session task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" void xiaozhi_voice_register_console(void)
{
    esp_console_cmd_t command = {};
    command.command = "xiaozhi_talk";
    command.help = "Toggle the XiaoZhi listening session";
    command.func = xiaozhi_talk_command;
    esp_err_t err = esp_console_cmd_register(&command);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "xiaozhi_talk was not registered: %s", esp_err_to_name(err));
    }
}

extern "C" void xiaozhi_voice_toggle_chat(void)
{
    EventGroupHandle_t link = nullptr;
    if (!voice_ready_link(&link)) {
        ESP_LOGW(TAG, "XiaoZhi voice is not running yet");
        return;
    }
    if (listening_now()) {
        mosaic_xiaozhi_set_emotion("neutral");
        xEventGroupSetBits(link, BIT_OFFLINE);
        ESP_LOGI(TAG, "talk: stop listening");
    } else {
        xEventGroupSetBits(link, BIT_ONLINE);
        ESP_LOGI(TAG, "talk: start listening");
    }
}

extern "C" esp_err_t xiaozhi_voice_get_status(xiaozhi_voice_status_t *out)
{
    if (out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const bool wifi_up = sta_has_ip();
    bool ready = false;
    bool audio_failed = false;
    bool chat_created = false;
    bool ever_connected = false;

    portENTER_CRITICAL(&s_listen_mux);
    out->started = s_task_started;
    out->wifi_connected = wifi_up;
    out->server_connected = s_transport_up;
    out->activating = s_activating;
    ready = s_ready;
    audio_failed = s_audio_failed;
    chat_created = (s_chat != 0);
    ever_connected = s_transport_seen_up;
    strlcpy(out->transport, s_transport, sizeof(out->transport));
    strlcpy(out->activation_code, s_activation_code, sizeof(out->activation_code));
    strlcpy(out->activation_message, s_activation_line, sizeof(out->activation_message));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
    portEXIT_CRITICAL(&s_listen_mux);

    const char *state = "starting";
    if (!out->started) {
        state = "starting";
    } else if (audio_failed) {
        state = "error";
    } else if (!wifi_up) {
        state = "no_wifi";
    } else if (out->activating) {
        state = "activating";
    } else if (out->server_connected) {
        state = "connected";
    } else if (!chat_created || !ready || !ever_connected) {
        state = "connecting";
    } else {
        state = "disconnected";
    }
    strlcpy(out->state, state, sizeof(out->state));
    return ESP_OK;
}
