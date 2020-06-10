#pragma once

#include "terminal.h"

#define SIXEL_MAX_COLORS 1024u

void sixel_fini(struct terminal *term);

void sixel_init(struct terminal *term);
void sixel_put(struct terminal *term, uint8_t c);
void sixel_unhook(struct terminal *term);

void sixel_destroy(struct sixel *sixel);

void sixel_delete_in_range(struct terminal *term, int start, int end);
void sixel_delete_at_row(struct terminal *term, int _row);
void sixel_delete_at_cursor(struct terminal *term);

void sixel_colors_report_current(struct terminal *term);
void sixel_colors_reset(struct terminal *term);
void sixel_colors_set(struct terminal *term, unsigned count);
void sixel_colors_report_max(struct terminal *term);

void sixel_geometry_report_current(struct terminal *term);
void sixel_geometry_reset(struct terminal *term);
void sixel_geometry_set(struct terminal *term, unsigned width, unsigned height);
void sixel_geometry_report_max(struct terminal *term);
