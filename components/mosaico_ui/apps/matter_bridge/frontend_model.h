#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FRONTEND_HOME, FRONTEND_LIGHT,
    FRONTEND_COLOUR, FRONTEND_SENSOR, FRONTEND_MONITOR, FRONTEND_MENU, FRONTEND_FAMILY, FRONTEND_ACTIVITY,
    FRONTEND_FAMILY_MEMBER, FRONTEND_PRIVACY_FACES, FRONTEND_ROUTE_COUNT
} frontend_route_t;

typedef struct {
    frontend_route_t route;
    bool camera, interaction, monitor_enabled, automation_active;
    unsigned scroll_offset;
    bool round_open, pir_known, last_pir;
    unsigned manual_mask, executed_mask;
    int64_t idle_deadline_us;
} frontend_model_t;

void frontend_model_init(frontend_model_t *model);
void frontend_model_modules(frontend_model_t *model, bool camera, bool interaction);
bool frontend_model_back(frontend_model_t *model);
void frontend_model_manual(frontend_model_t *model, unsigned device_mask);
/* Returns light bit 1 and AC bit 2 on the existing frontend's automation edge. */
unsigned frontend_model_automation(frontend_model_t *model, bool pir_known, bool pir,
                                   bool ldr_known, unsigned ldr_percent, int64_t now_us);
