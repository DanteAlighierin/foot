#include "kbd.h"

#include <xkbcommon/xkbcommon-compose.h>

void
kbd_destroy(struct kbd *kbd)
{
    if (kbd->xkb_compose_state != NULL)
        xkb_compose_state_unref(kbd->xkb_compose_state);
    if (kbd->xkb_compose_table != NULL)
        xkb_compose_table_unref(kbd->xkb_compose_table);
    if (kbd->xkb_keymap != NULL)
        xkb_keymap_unref(kbd->xkb_keymap);
    if (kbd->xkb_state != NULL)
        xkb_state_unref(kbd->xkb_state);
    if (kbd->xkb != NULL)
        xkb_context_unref(kbd->xkb);
}
