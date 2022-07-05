#pragma once

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED

#include "text-input-unstable-v3.h"

extern const struct zwp_text_input_v3_listener text_input_listener;

#endif /* FOOT_IME_ENABLED */

struct seat;
struct terminal;

void ime_enable(struct seat *seat);
void ime_disable(struct seat *seat);
void ime_update_cursor_rect(struct seat *seat);

void ime_reset_pending(struct seat *seat);
void ime_reset_preedit(struct seat *seat);
