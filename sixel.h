#pragma once

#include "terminal.h"

#define SIXEL_MAX_COLORS 1024u

void sixel_fini(struct terminal *term);

void sixel_init(struct terminal *term);
void sixel_put(struct terminal *term, uint8_t c);
void sixel_unhook(struct terminal *term);

void sixel_destroy(struct sixel *sixel);

/*
 * Deletes all sixels that are touched by the specified row(s). Used
 * when scrolling, to competely remove sixels that has either
 * completely, or partly scrolled out of history.
 *
 * Row numbers are relative to the current grid offset.
 */
void sixel_delete_in_range(struct terminal *term, int row_start, int row_end);
void sixel_delete_at_row(struct terminal *term, int row);

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
