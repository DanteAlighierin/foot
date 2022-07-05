#include "key-binding.h"

#include <stdlib.h>

#define LOG_MODULE "key-binding"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "config.h"
#include "debug.h"
#include "terminal.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

struct key_set {
    struct key_binding_set public;

    const struct config *conf;
    const struct seat *seat;
    size_t conf_ref_count;
};
typedef tll(struct key_set) bind_set_list_t;

struct key_binding_manager {
    struct key_set *last_used_set;
    bind_set_list_t binding_sets;
};

static void load_keymap(struct key_set *set);
static void unload_keymap(struct key_set *set);

struct key_binding_manager *
key_binding_manager_new(void)
{
    struct key_binding_manager *mgr = xcalloc(1, sizeof(*mgr));
    return mgr;
}

void
key_binding_manager_destroy(struct key_binding_manager *mgr)
{
    xassert(tll_length(mgr->binding_sets) == 0);
    free(mgr);
}

void
key_binding_new_for_seat(struct key_binding_manager *mgr,
                         const struct seat *seat)
{
#if defined(_DEBUG)
    tll_foreach(mgr->binding_sets, it)
        xassert(it->item.seat != seat);
#endif

    tll_foreach(seat->wayl->terms, it) {
        struct key_set set = {
            .public = {
                .key = tll_init(),
                .search = tll_init(),
                .url = tll_init(),
                .mouse = tll_init(),
            },
            .conf = it->item->conf,
            .seat = seat,
            .conf_ref_count = 1,
        };

        tll_push_back(mgr->binding_sets, set);

        LOG_DBG("new (seat): set=%p, seat=%p, conf=%p, ref-count=1",
                (void *)&tll_back(mgr->binding_sets),
                (void *)set.seat, (void *)set.conf);

        load_keymap(&tll_back(mgr->binding_sets));
    }

    LOG_DBG("new (seat): total number of sets: %zu",
            tll_length(mgr->binding_sets));
}

void
key_binding_new_for_term(struct key_binding_manager *mgr,
                         const struct terminal *term)
{
    const struct config *conf = term->conf;
    const struct wayland *wayl = term->wl;

    tll_foreach(wayl->seats, it) {
        struct seat *seat = &it->item;

        struct key_set *existing =
            (struct key_set *)key_binding_for(mgr, term, seat);

        if (existing != NULL) {
            existing->conf_ref_count++;
            continue;
        }

        struct key_set set = {
            .public = {
                .key = tll_init(),
                .search = tll_init(),
                .url = tll_init(),
                .mouse = tll_init(),
            },
            .conf = conf,
            .seat = seat,
            .conf_ref_count = 1,
        };

        tll_push_back(mgr->binding_sets, set);

        load_keymap(&tll_back(mgr->binding_sets));

        /* Chances are high this set will be requested next */
        mgr->last_used_set = &tll_back(mgr->binding_sets);

        LOG_DBG("new (term): set=%p, seat=%p, conf=%p, ref-count=1",
                (void *)&tll_back(mgr->binding_sets),
                (void *)set.seat, (void *)set.conf);
    }

    LOG_DBG("new (term): total number of sets: %zu",
            tll_length(mgr->binding_sets));
}

struct key_binding_set * NOINLINE
key_binding_for(struct key_binding_manager *mgr, const struct terminal *term,
                const struct seat *seat)
{
    const struct config *conf = term->conf;

    struct key_set *last_used = mgr->last_used_set;
    if (last_used != NULL &&
        last_used->conf == conf &&
        last_used->seat == seat)
    {
        // LOG_DBG("lookup: last used");
        return &last_used->public;
    }

    tll_foreach(mgr->binding_sets, it) {
        struct key_set *set = &it->item;

        if (set->conf != conf)
            continue;
        if (set->seat != seat)
            continue;

#if 0
        LOG_DBG("lookup: set=%p, seat=%p, conf=%p, ref-count=%zu",
                (void *)set, (void *)seat, (void *)conf, set->conf_ref_count);
#endif
        mgr->last_used_set = set;
        return &set->public;
    }

    return NULL;
}

static void
key_binding_set_destroy(struct key_binding_manager *mgr,
                        struct key_set *set)
{
    unload_keymap(set);
    if (mgr->last_used_set == set)
        mgr->last_used_set = NULL;

    /* Note: caller must remove from binding_sets */
}

void
key_binding_remove_seat(struct key_binding_manager *mgr,
                        const struct seat *seat)
{
    tll_foreach(mgr->binding_sets, it) {
        struct key_set *set = &it->item;

        if (set->seat != seat)
            continue;

        key_binding_set_destroy(mgr, set);
        tll_remove(mgr->binding_sets, it);

        LOG_DBG("remove seat: set=%p, seat=%p, total number of sets: %zu",
                (void *)set, (void *)seat, tll_length(mgr->binding_sets));
    }

    LOG_DBG("remove seat: total number of sets: %zu",
            tll_length(mgr->binding_sets));
}

void
key_binding_unref_term(struct key_binding_manager *mgr,
                       const struct terminal *term)
{
    const struct config *conf = term->conf;

    tll_foreach(mgr->binding_sets, it) {
        struct key_set *set = &it->item;

        if (set->conf != conf)
            continue;

        xassert(set->conf_ref_count > 0);
        if (--set->conf_ref_count == 0) {
            LOG_DBG("unref conf: set=%p, seat=%p, conf=%p",
                    (void *)set, (void *)set->seat, (void *)conf);

            key_binding_set_destroy(mgr, set);
            tll_remove(mgr->binding_sets, it);
        }
    }

    LOG_DBG("unref conf: total number of sets: %zu",
            tll_length(mgr->binding_sets));
}

static xkb_keycode_list_t
key_codes_for_xkb_sym(struct xkb_keymap *keymap, xkb_keysym_t sym)
{
    xkb_keycode_list_t key_codes = tll_init();

    /*
     * Find all key codes that map to this symbol.
     *
     * This allows us to match bindings in other layouts
     * too.
     */
    struct xkb_state *state = xkb_state_new(keymap);

    for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap);
         code <= xkb_keymap_max_keycode(keymap);
         code++)
    {
        if (xkb_state_key_get_one_sym(state, code) == sym)
            tll_push_back(key_codes, code);
    }

    xkb_state_unref(state);
    return key_codes;
}

static xkb_keysym_t
maybe_repair_key_combo(const struct seat *seat,
                       xkb_keysym_t sym, xkb_mod_mask_t mods)
{
    /*
     * Detect combos containing a shifted symbol and the corresponding
     * modifier, and replace the shifted symbol with its unshifted
     * variant.
     *
     * For example, the combo is “Control+Shift+U”. In this case,
     * Shift is the modifier used to “shift” ‘u’ to ‘U’, after which
     * ‘Shift’ will have been “consumed”. Since we filter out consumed
     * modifiers when matching key combos, this key combo will never
     * trigger (we will never be able to match the ‘Shift’ modifier).
     *
     * There are two correct variants of the above key combo:
     *  - “Control+U”           (upper case ‘U’)
     *  - “Control+Shift+u”     (lower case ‘u’)
     *
     * What we do here is, for each key *code*, check if there are any
     * (shifted) levels where it produces ‘sym’. If there are, check
     * *which* sets of modifiers are needed to produce it, and compare
     * with ‘mods’.
     *
     * If there is at least one common modifier, it means ‘sym’ is a
     * “shifted” symbol, with the corresponding shifting modifier
     * explicitly included in the key combo. I.e. the key combo will
     * never trigger.
     *
     * We then proceed and “repair” the key combo by replacing ‘sym’
     * with the corresponding unshifted symbol.
     *
     * To reduce the noise, we ignore all key codes where the shifted
     * symbol is the same as the unshifted symbol.
     */

    for (xkb_keycode_t code = xkb_keymap_min_keycode(seat->kbd.xkb_keymap);
         code <= xkb_keymap_max_keycode(seat->kbd.xkb_keymap);
         code++)
    {
        xkb_layout_index_t layout_idx =
            xkb_state_key_get_layout(seat->kbd.xkb_state, code);

        /* Get all unshifted symbols for this key */
        const xkb_keysym_t *base_syms = NULL;
        size_t base_count = xkb_keymap_key_get_syms_by_level(
            seat->kbd.xkb_keymap, code, layout_idx, 0, &base_syms);

        if (base_count == 0 || sym == base_syms[0]) {
            /* No unshifted symbols, or unshifted symbol is same as ‘sym’ */
            continue;
        }

        /* Name of the unshifted symbol, for logging */
        char base_name[100];
        xkb_keysym_get_name(base_syms[0], base_name, sizeof(base_name));

        /* Iterate all shift levels */
        for (xkb_level_index_t level_idx = 1;
             level_idx < xkb_keymap_num_levels_for_key(
                 seat->kbd.xkb_keymap, code, layout_idx);
             level_idx++) {

            /* Get all symbols for current shift level */
            const xkb_keysym_t *shifted_syms = NULL;
            size_t shifted_count = xkb_keymap_key_get_syms_by_level(
                seat->kbd.xkb_keymap, code,
                layout_idx, level_idx, &shifted_syms);

            for (size_t i = 0; i < shifted_count; i++) {
                if (shifted_syms[i] != sym)
                    continue;

                /* Get modifier sets that produces the current shift level */
                xkb_mod_mask_t mod_masks[16];
                size_t mod_mask_count = xkb_keymap_key_get_mods_for_level(
                    seat->kbd.xkb_keymap, code, layout_idx, level_idx,
                    mod_masks, ALEN(mod_masks));

                /* Check if key combo’s modifier set intersects */
                for (size_t j = 0; j < mod_mask_count; j++) {
                    if ((mod_masks[j] & mods) != mod_masks[j])
                        continue;

                    char combo[64] = {0};

                    for (int k = 0; k < sizeof(xkb_mod_mask_t) * 8; k++) {
                        if (!(mods & (1u << k)))
                            continue;

                        const char *mod_name = xkb_keymap_mod_get_name(
                            seat->kbd.xkb_keymap, k);
                        strcat(combo, mod_name);
                        strcat(combo, "+");
                    }

                    size_t len = strlen(combo);
                    xkb_keysym_get_name(
                        sym, &combo[len], sizeof(combo) - len);

                    LOG_WARN(
                        "%s: combo with both explicit modifier and shifted symbol "
                        "(level=%d, mod-mask=0x%08x), "
                        "replacing with %s",
                        combo, level_idx, mod_masks[j], base_name);

                    /* Replace with unshifted symbol */
                    return base_syms[0];
                }
            }
        }
    }

    return sym;
}

static void NOINLINE
convert_key_binding(struct key_set *set,
                    const struct config_key_binding *conf_binding,
                    key_binding_list_t *bindings)
{
    const struct seat *seat = set->seat;

    xkb_mod_mask_t mods = conf_modifiers_to_mask(seat, &conf_binding->modifiers);
    xkb_keysym_t sym = maybe_repair_key_combo(seat, conf_binding->k.sym, mods);

    struct key_binding binding = {
        .type = KEY_BINDING,
        .action = conf_binding->action,
        .aux = &conf_binding->aux,
        .mods = mods,
        .k = {
            .sym = sym,
            .key_codes = key_codes_for_xkb_sym(seat->kbd.xkb_keymap, sym),
        },
    };
    tll_push_back(*bindings, binding);
}

static void
convert_key_bindings(struct key_set *set)
{
    const struct config *conf = set->conf;

    for (size_t i = 0; i < conf->bindings.key.count; i++) {
        const struct config_key_binding *binding = &conf->bindings.key.arr[i];
        convert_key_binding(set, binding, &set->public.key);
    }
}

static void
convert_search_bindings(struct key_set *set)
{
    const struct config *conf = set->conf;

    for (size_t i = 0; i < conf->bindings.search.count; i++) {
        const struct config_key_binding *binding = &conf->bindings.search.arr[i];
        convert_key_binding(set, binding, &set->public.search);
    }
}

static void
convert_url_bindings(struct key_set *set)
{
    const struct config *conf = set->conf;

    for (size_t i = 0; i < conf->bindings.url.count; i++) {
        const struct config_key_binding *binding = &conf->bindings.url.arr[i];
        convert_key_binding(set, binding, &set->public.url);
    }
}

static void
convert_mouse_binding(struct key_set *set,
                      const struct config_key_binding *conf_binding)
{
    struct key_binding binding = {
        .type = MOUSE_BINDING,
        .action = conf_binding->action,
        .aux = &conf_binding->aux,
        .mods = conf_modifiers_to_mask(set->seat, &conf_binding->modifiers),
        .m = {
            .button = conf_binding->m.button,
            .count = conf_binding->m.count,
        },
    };
    tll_push_back(set->public.mouse, binding);
}

static void
convert_mouse_bindings(struct key_set *set)
{
    const struct config *conf = set->conf;

    for (size_t i = 0; i < conf->bindings.mouse.count; i++) {
        const struct config_key_binding *binding =
            &conf->bindings.mouse.arr[i];
        convert_mouse_binding(set, binding);
    }
}

static void NOINLINE
load_keymap(struct key_set *set)
{
    LOG_DBG("load keymap: set=%p, seat=%p, conf=%p",
            (void *)set, (void *)set->seat, (void *)set->conf);

    if (set->seat->kbd.xkb_state == NULL ||
        set->seat->kbd.xkb_keymap == NULL)
    {
        LOG_DBG("no XKB keymap");
        return;
    }

    convert_key_bindings(set);
    convert_search_bindings(set);
    convert_url_bindings(set);
    convert_mouse_bindings(set);

    set->public.selection_overrides = conf_modifiers_to_mask(
        set->seat, &set->conf->mouse.selection_override_modifiers);
}

void
key_binding_load_keymap(struct key_binding_manager *mgr,
                        const struct seat *seat)
{
    tll_foreach(mgr->binding_sets, it) {
        struct key_set *set = &it->item;

        if (set->seat == seat)
            load_keymap(set);
    }
}

static void NOINLINE
key_bindings_destroy(key_binding_list_t *bindings)
{
    tll_foreach(*bindings, it) {
        struct key_binding *bind = &it->item;
        switch (bind->type) {
        case KEY_BINDING: tll_free(it->item.k.key_codes); break;
        case MOUSE_BINDING: break;
        }

        tll_remove(*bindings, it);
    }
}

static void NOINLINE
unload_keymap(struct key_set *set)
{
    key_bindings_destroy(&set->public.key);
    key_bindings_destroy(&set->public.search);
    key_bindings_destroy(&set->public.url);
    key_bindings_destroy(&set->public.mouse);
    set->public.selection_overrides = 0;
}

void
key_binding_unload_keymap(struct key_binding_manager *mgr,
                          const struct seat *seat)
{
    tll_foreach(mgr->binding_sets, it) {
        struct key_set *set = &it->item;
        if (set->seat != seat)
            continue;

        LOG_DBG("unload keymap: set=%p, seat=%p, conf=%p",
                (void *)set, (void *)seat, (void *)set->conf);

        unload_keymap(set);
    }
}
