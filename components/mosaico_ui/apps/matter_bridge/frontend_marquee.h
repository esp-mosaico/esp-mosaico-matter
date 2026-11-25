#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char text[129];
    int64_t started_us;
    int width_px;
} frontend_marquee_t;
/* Pixel translation only; the full UTF-8 string and viewport never change. */
int frontend_marquee_offset(frontend_marquee_t *state, const char *text,
                            int frame_width, int font_size, int64_t now_us);
