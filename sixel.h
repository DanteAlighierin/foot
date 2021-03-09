#pragma once

#include "terminal.h"

#define SIXEL_MAX_COLORS 1024u
#define SIXEL_MAX_WIDTH 10000u
#define SIXEL_MAX_HEIGHT 10000u

void sixel_fini(struct terminal *term);

void sixel_init(struct terminal *term, int p1, int p2, int p3);
void sixel_put(struct terminal *term, uint8_t c);
void sixel_unhook(struct terminal *term);

void sixel_destroy(struct sixel *sixel);
void sixel_destroy_all(struct terminal *term);

void sixel_scroll_up(struct terminal *term, int rows);
void sixel_scroll_down(struct terminal *term, int rows);

void sixel_cell_size_changed(struct terminal *term);
void sixel_reflow(struct terminal *term);

/*
 * Remove sixel data from the specified location. Used when printing
 * or erasing characters, and when emitting new sixel images, to
 * remove sixel data that would otherwise be rendered on-top.
 *
 * Row numbers are relative to the current grid offset
 */
void sixel_overwrite_by_rectangle(
    struct terminal *term, int row, int col, int height, int width);
void sixel_overwrite_by_row(struct terminal *term, int row, int col, int width);
void sixel_overwrite_at_cursor(struct terminal *term, int width);

void sixel_colors_report_current(struct terminal *term);
void sixel_colors_reset(struct terminal *term);
void sixel_colors_set(struct terminal *term, unsigned count);
void sixel_colors_report_max(struct terminal *term);

void sixel_geometry_report_current(struct terminal *term);
void sixel_geometry_reset(struct terminal *term);
void sixel_geometry_set(struct terminal *term, unsigned width, unsigned height);
void sixel_geometry_report_max(struct terminal *term);
