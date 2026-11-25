#include "frontend_model.h"
#include <string.h>

static void reset_round(frontend_model_t *m)
{
    m->round_open = false;
    m->manual_mask = m->executed_mask = 0;
    m->idle_deadline_us = 0;
}

void frontend_model_init(frontend_model_t *m)
{
    memset(m, 0, sizeof(*m));
    m->monitor_enabled = true;
}

void frontend_model_modules(frontend_model_t *m, bool camera, bool interaction)
{
    if (m->camera != camera || m->interaction != interaction) m->scroll_offset = 0;
    m->camera = camera;
    m->interaction = interaction;
    if ((!camera && m->route >= FRONTEND_MONITOR) ||
        (!interaction && m->route >= FRONTEND_LIGHT && m->route <= FRONTEND_SENSOR))
        m->route = FRONTEND_HOME;
    if (!interaction) {
        m->automation_active = false;
        m->pir_known = false;
        reset_round(m);
    }
}

bool frontend_model_back(frontend_model_t *m)
{
    if (m->route == FRONTEND_HOME) {
        if (m->scroll_offset) { m->scroll_offset = 0; return true; }
        return false;
    }
    if (m->route == FRONTEND_COLOUR) { m->route=FRONTEND_LIGHT; return true; }
    if (m->route == FRONTEND_FAMILY_MEMBER) m->route = FRONTEND_FAMILY;
    else if (m->route == FRONTEND_PRIVACY_FACES) m->route = FRONTEND_MONITOR;
    else if (m->route == FRONTEND_FAMILY || m->route == FRONTEND_ACTIVITY) m->route = FRONTEND_MENU;
    else if (m->route == FRONTEND_MENU) m->route = FRONTEND_MONITOR;
    else m->route = FRONTEND_HOME;
    return true;
}

void frontend_model_manual(frontend_model_t *m, unsigned mask)
{
    if (m->round_open) m->manual_mask |= mask;
}

unsigned frontend_model_automation(frontend_model_t *m, bool known, bool pir,
                                   bool ldr_known, unsigned ldr, int64_t now)
{
    if (m->idle_deadline_us && now >= m->idle_deadline_us) reset_round(m);
    if (!known) {
        m->pir_known = false;
        m->idle_deadline_us = 0;
        return 0;
    }
    if (pir) m->idle_deadline_us = 0;
    else if (m->round_open && !m->idle_deadline_us)
        m->idle_deadline_us = now + INT64_C(300000000);
    bool rising = m->pir_known && !m->last_pir && pir;
    m->pir_known = true;
    m->last_pir = pir;
    if (!m->interaction || !m->automation_active || !rising || !ldr_known || ldr >= 70)
        return 0;
    m->round_open = true;
    unsigned actions = 3U & ~m->manual_mask & ~m->executed_mask;
    m->executed_mask |= actions;
    return actions;
}
