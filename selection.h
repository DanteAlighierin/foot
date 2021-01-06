#pragma once

#include <stdbool.h>
#include <wayland-client.h>

#include "terminal.h"

extern const struct wl_data_device_listener data_device_listener;
extern const struct zwp_primary_selection_device_v1_listener primary_selection_device_listener;

bool selection_enabled(const struct terminal *term, struct seat *seat);
void selection_start(
    struct terminal *term, int col, int row,
    enum selection_kind new_kind, bool spaces_only);
void selection_update(struct terminal *term, int col, int row);
void selection_finalize(
    struct seat *seat, struct terminal *term, uint32_t serial);
void selection_dirty_cells(struct terminal *term);
void selection_cancel(struct terminal *term);
void selection_extend(
    struct seat *seat, struct terminal *term,
    int col, int row, enum selection_kind kind);

bool selection_on_rows(const struct terminal *term, int start, int end);

void selection_view_up(struct terminal *term, int new_view);
void selection_view_down(struct terminal *term, int new_view);

void selection_clipboard_unset(struct seat *seat);
void selection_primary_unset(struct seat *seat);

bool selection_clipboard_has_data(const struct seat *seat);
bool selection_primary_has_data(const struct seat *seat);

char *selection_to_text(const struct terminal *term);
void selection_to_clipboard(
    struct seat *seat, struct terminal *term, uint32_t serial);
void selection_from_clipboard(
    struct seat *seat, struct terminal *term, uint32_t serial);
void selection_to_primary(
    struct seat *seat, struct terminal *term, uint32_t serial);
void selection_from_primary(struct seat *seat, struct terminal *term);

/* Copy text *to* primary/clipboard */
bool text_to_clipboard(
    struct seat *seat, struct terminal *term, char *text, uint32_t serial);
bool text_to_primary(
    struct seat *seat, struct terminal *term, char *text, uint32_t serial);

/*
 * Copy text *from* primary/clipboard
 *
 * Note that these are asynchronous; they *will* return
 * immediately. The 'cb' callback will be called 0..n times with
 * clipboard data. When done (or on error), the 'done' callback is
 * called.
 *
 * As such, keep this in mind:
 *  - The 'user' context must not be stack allocated
 * - Don't expect clipboard data to have been received when these
 *   functions return (it will *never* have been received at this
 *   point).
 */
void text_from_clipboard(
    struct seat *seat, struct terminal *term,
    void (*cb)(char *data, size_t size, void *user),
    void (*done)(void *user), void *user);

void text_from_primary(
    struct seat *seat, struct terminal *term,
    void (*cb)(char *data, size_t size, void *user),
    void (*dont)(void *user), void *user);

void selection_start_scroll_timer(
    struct terminal *term, int interval_ns,
    enum selection_scroll_direction direction, int col);
void selection_stop_scroll_timer(struct terminal *term);
