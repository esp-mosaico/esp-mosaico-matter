/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_gsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "xiaozhi_actions.h"
#include "xiaozhi_binds.h"
#include "xiaozhi_objects.h"

#define XIAOZHI_TEXT_MAX 96
#define XIAOZHI_MSG_MAX 32
#define XIAOZHI_EMOTION_MAX 32

typedef enum {
    XIAOZHI_ROLE_NONE = 0,
    XIAOZHI_ROLE_USER,
    XIAOZHI_ROLE_ASSISTANT,
    XIAOZHI_ROLE_SYSTEM,
} xiaozhi_role_t;

typedef struct {
    uint32_t seq;
    uint32_t revision;
    xiaozhi_role_t role;
    char text[XIAOZHI_TEXT_MAX];
} xiaozhi_msg_t;

typedef struct {
    char status[48];
    char notice[XIAOZHI_TEXT_MAX];
} xiaozhi_chrome_t;

static const char *TAG = "xiaozhi_ui";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_chrome_t s_chrome = {
    .status = "Standby",
};
/* Names match xiaozhi-esp32 noto_emoji and the xiaozhi_chat display table. */
static const struct {
    const char *name;
    const char *glyph;
} s_emotions[] = {
    {"neutral", "😶"},
    {"happy", "🙂"},
    {"laughing", "😆"},
    {"funny", "😂"},
    {"sad", "😔"},
    {"angry", "😠"},
    {"crying", "😭"},
    {"loving", "😍"},
    {"embarrassed", "😳"},
    {"surprised", "😯"},
    {"shocked", "😱"},
    {"thinking", "🤔"},
    {"winking", "😉"},
    {"cool", "😎"},
    {"relaxed", "😌"},
    {"delicious", "🤤"},
    {"kissy", "😘"},
    {"confident", "😏"},
    {"sleepy", "😴"},
    {"silly", "😜"},
    {"confused", "🙄"},
};
static char s_emotion[XIAOZHI_EMOTION_MAX] = "neutral";
/* Render task only. Skips a repeated esp_gsp_set_text of the same face. */
static char s_published_emotion[XIAOZHI_EMOTION_MAX];
static xiaozhi_msg_t s_stage[XIAOZHI_MSG_MAX] = {
    {
        .seq = 1,
        .revision = 1,
        .role = XIAOZHI_ROLE_SYSTEM,
        .text = "Wi-Fi not connected",
    },
};
static int s_stage_count = 1;
static uint32_t s_stage_gen = 1;
static uint32_t s_next_seq = 2;
/* Published only from the render task. message_list callbacks read it there. */
static xiaozhi_msg_t s_live[XIAOZHI_MSG_MAX];
static int s_live_count;
static uint32_t s_published_gen;
static esp_gsp_list_t s_messages = ESP_GSP_LIST_NONE;
static esp_gsp_handle_t s_prepared_ui;

extern void xiaozhi_voice_toggle_chat(void) __attribute__((weak));

static void copy_field(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) {
        src = "";
    }
    strlcpy(dst, src, dst_size);
}

void mosaic_xiaozhi_set_status(const char *status)
{
    portENTER_CRITICAL(&s_mux);
    copy_field(s_chrome.status, sizeof(s_chrome.status), status);
    /* A notice is a one-shot banner. The next state replaces it, otherwise
     * "Connected" stays on screen and Talk never appears to change the line. */
    s_chrome.notice[0] = '\0';
    portEXIT_CRITICAL(&s_mux);
}

static const char *emotion_glyph(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') {
        name = "neutral";
    }
    for (i = 0; i < sizeof(s_emotions) / sizeof(s_emotions[0]); i++) {
        if (strcmp(s_emotions[i].name, name) == 0) {
            return s_emotions[i].glyph;
        }
    }
    return NULL;
}

void mosaic_xiaozhi_set_emotion(const char *emotion)
{
    if (emotion == NULL || emotion[0] == '\0') {
        emotion = "neutral";
    }
    portENTER_CRITICAL(&s_mux);
    copy_field(s_emotion, sizeof(s_emotion), emotion);
    portEXIT_CRITICAL(&s_mux);
}

void mosaic_xiaozhi_set_notice(const char *notice)
{
    portENTER_CRITICAL(&s_mux);
    copy_field(s_chrome.notice, sizeof(s_chrome.notice), notice);
    portEXIT_CRITICAL(&s_mux);
}

static xiaozhi_role_t role_from(const char *role)
{
    if (role != NULL && strcmp(role, "user") == 0) {
        return XIAOZHI_ROLE_USER;
    }
    if (role != NULL && strcmp(role, "assistant") == 0) {
        return XIAOZHI_ROLE_ASSISTANT;
    }
    return XIAOZHI_ROLE_SYSTEM;
}

static void push_message(xiaozhi_role_t kind, const char *content)
{
    if (s_stage_count == XIAOZHI_MSG_MAX) {
        memmove(&s_stage[0], &s_stage[1],
                sizeof(s_stage[0]) * (XIAOZHI_MSG_MAX - 1));
        s_stage_count--;
    }
    xiaozhi_msg_t *slot = &s_stage[s_stage_count++];
    memset(slot, 0, sizeof(*slot));
    slot->seq = s_next_seq++;
    slot->revision = 1;
    slot->role = kind;
    copy_field(slot->text, sizeof(slot->text), content);
}

void mosaic_xiaozhi_set_chat(const char *role, const char *content)
{
    const xiaozhi_role_t kind = role_from(role);
    if (content == NULL) {
        content = "";
    }

    portENTER_CRITICAL(&s_mux);
    if (content[0] == '\0') {
        if (kind == XIAOZHI_ROLE_SYSTEM && s_stage_count > 0 &&
                s_stage[s_stage_count - 1].role == XIAOZHI_ROLE_SYSTEM) {
            s_stage_count--;
            memset(&s_stage[s_stage_count], 0, sizeof(s_stage[0]));
            s_stage_gen++;
        }
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    if (kind == XIAOZHI_ROLE_SYSTEM && s_stage_count > 0 &&
            s_stage[s_stage_count - 1].role == XIAOZHI_ROLE_SYSTEM) {
        /* A new id makes the message list drop the previous bubble.
         * Rewriting the text under the same id leaves the old sentence
         * on screen. */
        xiaozhi_msg_t *slot = &s_stage[s_stage_count - 1];
        memset(slot, 0, sizeof(*slot));
        slot->seq = s_next_seq++;
        slot->revision = 1;
        slot->role = XIAOZHI_ROLE_SYSTEM;
        copy_field(slot->text, sizeof(slot->text), content);
        s_stage_gen++;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    push_message(kind, content);
    s_stage_gen++;
    portEXIT_CRITICAL(&s_mux);
}

void mosaic_xiaozhi_clear_chat(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_stage, 0, sizeof(s_stage));
    s_stage_count = 0;
    s_chrome.notice[0] = '\0';
    s_stage_gen++;
    portEXIT_CRITICAL(&s_mux);
}

static uint32_t message_count(void *user_ctx)
{
    (void)user_ctx;
    return (uint32_t)s_live_count;
}

static bool message_get(void *user_ctx, uint32_t index, esp_gsp_message_t *out_message)
{
    (void)user_ctx;
    if (out_message == NULL || index >= (uint32_t)s_live_count) {
        return false;
    }
    const xiaozhi_msg_t *message = &s_live[index];
    *out_message = (esp_gsp_message_t) {
        .text = message->text,
        .id = message->seq,
        .revision = message->revision,
        .direction = message->role == XIAOZHI_ROLE_USER
            ? ESP_GSP_MESSAGE_OUTGOING : ESP_GSP_MESSAGE_INCOMING,
    };
    return true;
}

static bool publish_messages(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_stage_gen == s_published_gen) {
        portEXIT_CRITICAL(&s_mux);
        return false;
    }
    memcpy(s_live, s_stage, sizeof(s_live));
    s_live_count = s_stage_count;
    s_published_gen = s_stage_gen;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static void publish_emotion(esp_gsp_handle_t ui, const char *name)
{
    const char *glyph;

    if (strcmp(name, s_published_emotion) == 0) {
        return;
    }
    glyph = emotion_glyph(name);
    if (glyph == NULL) {
        ESP_LOGW(TAG, "unknown emotion: %s", name);
        copy_field(s_published_emotion, sizeof(s_published_emotion), name);
        return;
    }
    esp_gsp_set_text(ui, GSP_BIND_XIAOZHI_EMOTION, glyph);
    copy_field(s_published_emotion, sizeof(s_published_emotion), name);
}

static void render(esp_gsp_handle_t ui, void *ctx)
{
    xiaozhi_chrome_t chrome;
    char emotion[XIAOZHI_EMOTION_MAX];
    (void)ctx;
    if (publish_messages() && s_messages != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_message_list_changed(ui, s_messages, 0);
    }
    portENTER_CRITICAL(&s_mux);
    chrome = s_chrome;
    memcpy(emotion, s_emotion, sizeof(emotion));
    portEXIT_CRITICAL(&s_mux);

    const char *status = chrome.notice[0] != '\0' ? chrome.notice :
                         (chrome.status[0] != '\0' ? chrome.status : "Standby");
    esp_gsp_set_text(ui, GSP_BIND_XIAOZHI_STATUS, status);
    publish_emotion(ui, emotion);
}

static void stopping(esp_gsp_handle_t ui)
{
    if (s_prepared_ui == ui) {
        s_prepared_ui = NULL;
    }
    s_messages = ESP_GSP_LIST_NONE;
}

static void started(esp_gsp_handle_t ui)
{
    /* prepare_app_ui runs this before the first frame. START runs it again
     * and must not bind a second list or repaint the face. */
    if (s_prepared_ui == ui) {
        return;
    }
    s_prepared_ui = ui;
    const esp_gsp_message_source_t source = {
        .struct_size = sizeof(source),
        .count = message_count,
        .get = message_get,
    };
    publish_messages();
    s_messages = esp_gsp_message_list_bind_component(
        ui, GSP_OBJ_KEY_XIAOZHI_MESSAGES, &source);
    if (s_messages == ESP_GSP_LIST_NONE) {
        ESP_LOGW(TAG, "chat list was not bound");
    }
    s_published_emotion[0] = '\0';
    render(ui, NULL);
    (void)esp_gsp_timer_create(ui, 250, render, NULL);
}

static void on_event(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    (void)ui;
    if (event == NULL || event->type != MOSAIC_EVENT_UI_CALL) {
        return;
    }
    if (event->data.call.action_id == GSP_ACT_ID_XIAOZHI_TALK &&
            xiaozhi_voice_toggle_chat != NULL) {
        xiaozhi_voice_toggle_chat();
    }
}

const mosaic_app_descriptor_t mosaic_xiaozhi_app = {
    .id = 22,
    .launch_action = GSP_ACT_ID_APP_XIAOZHI,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "xiaozhi",
    .title = "XiaoZhi",
    .directory = &gsp_obj_directory_xiaozhi,
    .root_header_in_stack = true,
    .disable_swipe = true,
    .on_started = started,
    .on_stopping = stopping,
    .on_event = on_event,
};
