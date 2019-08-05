#pragma once

#include <stdbool.h>
#include <wayland-client.h>

#include "terminal.h"

extern const struct wl_data_device_listener data_device_listener;
extern const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener;

void selection_start(struct terminal *term, int col, int row);
void selection_update(struct terminal *term, int col, int row);
void selection_finalize(struct terminal *term, uint32_t serial);
void selection_cancel(struct terminal *term);
void selection_mark_word(struct terminal *term, int col, int row,
                         bool spaces_only, uint32_t serial);

void selection_to_clipboard(struct terminal *term, uint32_t serial);
void selection_from_clipboard(struct terminal *term, uint32_t serial);
void selection_from_primary(struct terminal *term);
bool selection_on_row_in_view(const struct terminal *term, int row_no);

bool text_to_clipboard(struct terminal *term, char *text, uint32_t serial);
void text_from_clipboard(
    struct terminal *term, uint32_t serial,
    void (*cb)(const char *data, size_t size, void *user),
    void *user);
