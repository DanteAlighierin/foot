#pragma once

#include <wayland-client.h>

#include "terminal.h"

extern const struct wl_data_device_listener data_device_listener;
extern const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener;

void selection_start(struct terminal *term, int col, int row);
void selection_update(struct terminal *term, int col, int row);
void selection_finalize(struct terminal *term, uint32_t serial);
void selection_cancel(struct terminal *term);
void selection_mark_word(struct terminal *term, int col, int row, uint32_t serial);

void selection_to_clipboard(struct terminal *term, uint32_t serial);
void selection_from_clipboard(struct terminal *term, uint32_t serial);
void selection_from_primary(struct terminal *term);

bool text_to_clipboard(struct terminal *term, char *text, uint32_t serial);
