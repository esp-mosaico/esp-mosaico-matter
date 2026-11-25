#pragma once
#include "frontend_backend.h"
#include <stdbool.h>
#include <stdint.h>

enum { FRONTEND_JOB_POWER=1, FRONTEND_JOB_LEVEL=2, FRONTEND_JOB_KELVIN=4 };
void frontend_jobs_init(void);
void frontend_jobs_cancel(const char *id);
void frontend_jobs_cancel_automatic(void);
bool frontend_jobs_queue(const frontend_backend_catalog_entry_t *entry,
                          unsigned steps, bool power, int level, int kelvin, bool automatic);
/* Owner-task only. Never changes the selected detail endpoint. */
void frontend_jobs_tick(const frontend_backend_snapshot_t *snapshot);
uint32_t frontend_jobs_message_serial(void);
const char *frontend_jobs_message(void);
