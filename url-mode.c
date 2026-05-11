#include "url-mode.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <unistd.h>
#include <regex.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "url-mode"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"
#include "grid.h"
#include "key-binding.h"
#include "quirks.h"
#include "render.h"
#include "selection.h"
#include "spawn.h"
#include "terminal.h"
#include "uri.h"
#include "util.h"
#include "xmalloc.h"

static void url_destroy(struct url *url);

static bool
execute_binding(struct seat *seat, struct terminal *term,
                const struct key_binding *binding, uint32_t serial)
{
    const enum bind_action_url action = binding->action;

    switch (action) {
    case BIND_ACTION_URL_NONE:
        return false;

    case BIND_ACTION_URL_CANCEL:
        urls_reset(term);
        return true;

    case BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL:
         term->urls_show_uri_on_jump_label = !term->urls_show_uri_on_jump_label;
        render_refresh_urls(term);
        return true;

    case BIND_ACTION_URL_COUNT:
        return false;

    }
    return true;
}

static bool
spawn_url_launcher_with_token(struct terminal *term,
                              const char *url,
                              const char *xdg_activation_token)
{
    size_t argc;
    char **argv;

    int dev_null = open("/dev/null", O_RDWR);

    if (dev_null < 0) {
        LOG_ERRNO("failed to open /dev/null");
        return false;
    }

    xassert(term->url_launch != NULL);
    bool ret = false;

    if (spawn_expand_template(
            term->url_launch, 2,
            (const char *[]){"url", "match"},
            (const char *[]){url, url},
            &argc, &argv))
    {
        ret = spawn(
            term->reaper, term->cwd, argv,
            dev_null, dev_null, dev_null, NULL, NULL, xdg_activation_token) >= 0;

        for (size_t i = 0; i < argc; i++)
            free(argv[i]);
        free(argv);
    }

    close(dev_null);
    return ret;
}

struct spawn_activation_context {
    struct terminal *term;
    char *url;
};

static void
activation_token_done(const char *token, void *data)
{
    struct spawn_activation_context *ctx = data;

    spawn_url_launcher_with_token(ctx->term, ctx->url, token);
    free(ctx->url);
    free(ctx);
}

static bool
spawn_url_launcher(struct seat *seat, struct terminal *term, const char *url,
                   uint32_t serial)
{
    xassert(term->url_launch != NULL);

    struct spawn_activation_context *ctx = xmalloc(sizeof(*ctx));
    *ctx = (struct spawn_activation_context){
        .term = term,
        .url = xstrdup(url),
    };

    if (wayl_get_activation_token(
            seat->wayl, seat, serial, term->window, &activation_token_done, ctx))
    {
        /* Context free:d by callback */
        return true;
    }

    free(ctx->url);
    free(ctx);

    return spawn_url_launcher_with_token(term, url, NULL);
}

static void
activate_url(struct seat *seat, struct terminal *term, const struct url *url,
             uint32_t serial, bool paste_url_to_self)
{
    char *url_string = NULL;

    char *scheme, *host, *path;
    if (uri_parse(url->url, strlen(url->url), &scheme, NULL, NULL,
                  &host, NULL, &path, NULL, NULL))
    {
        if (strcmp(scheme, "file") == 0 && hostname_is_localhost(host)) {
            /*
             * This is a file in *this* computer. Pass only the
             * filename to the URL-launcher.
             *
             * I.e. strip the ‘file://user@host/’ prefix.
             */
            url_string = path;
        } else
            free(path);

        free(scheme);
        free(host);
    }

    if (url_string == NULL)
        url_string = xstrdup(url->url);

    switch (url->action) {
    case URL_ACTION_COPY:
        if (paste_url_to_self) {
            if (term->bracketed_paste)
                term_to_slave(term, "\033[200~", 6);

            term_to_slave(term, url_string, strlen(url_string));

            if (term->bracketed_paste)
                term_to_slave(term, "\033[201~", 6);
        }
        if (text_to_clipboard(seat, term, url_string, seat->kbd.serial)) {
            /* Now owned by our clipboard “manager” */
            url_string = NULL;
        }
        break;

    case URL_ACTION_LAUNCH:
    case URL_ACTION_PERSISTENT: {
        spawn_url_launcher(seat, term, url_string, serial);
        break;
    }
    }

    free(url_string);
}

void
urls_input(struct seat *seat, struct terminal *term,
           const struct key_binding_set *bindings, uint32_t key,
           xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
           const xkb_keysym_t *raw_syms, size_t raw_count,
           uint32_t serial)
{
    /*
     * Key bindings
     */

    /* Match untranslated symbols */
    tll_foreach(bindings->url, it) {
        const struct key_binding *bind = &it->item;
        if (bind->mods != mods || bind->mods == 0)
            continue;

        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i]) {
                execute_binding(seat, term, bind, serial);
                seat->kbd.last_shortcut_sym = sym;
                return;
            }
        }
    }

    /* Match translated symbol */
    tll_foreach(bindings->url, it) {
        const struct key_binding *bind = &it->item;

        if (bind->k.sym == sym &&
            bind->mods == (mods & ~consumed))
        {
            execute_binding(seat, term, bind, serial);
            seat->kbd.last_shortcut_sym = sym;
            return;
        }

    }

    /* Match raw key code */
    tll_foreach(bindings->url, it) {
        const struct key_binding *bind = &it->item;
        if (bind->mods != mods || bind->mods == 0)
            continue;

        /* Match raw key code */
        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key) {
                execute_binding(seat, term, bind, serial);
                seat->kbd.last_shortcut_sym = sym;
                return;
            }
        }
    }

    size_t seq_len = c32len(term->url_keys);

    if (sym == XKB_KEY_BackSpace) {
        if (seq_len > 0) {
            term->url_keys[seq_len - 1] = U'\0';
            render_refresh_urls(term);
        }

        return;
    }

    if (mods & ~consumed)
        return;

    char32_t wc = xkb_state_key_get_utf32(seat->kbd.xkb_state, key);

    /*
     * Determine if this is a "valid" key. I.e. if there is a URL
     * label with a key combo where this key is the next in
     * sequence.
     */

    bool is_valid = false;
    const struct url *match = NULL;

    tll_foreach(term->urls, it) {
        if (it->item.key == NULL)
            continue;

        const struct url *url = &it->item;
        const size_t key_len = c32len(it->item.key);

        if (key_len >= seq_len + 1 &&
            c32ncasecmp(url->key, term->url_keys, seq_len) == 0 &&
            toc32lower(url->key[seq_len]) == toc32lower(wc))
        {
            is_valid = true;
            if (key_len == seq_len + 1) {
                match = url;
                break;
            }
        }
    }

    if (match) {
        // If the last hint character was uppercase, copy and paste
        bool insert = term->conf->uppercase_regex_insert && wc == toc32upper(wc);
        activate_url(seat, term, match, serial, insert);

        switch (match->action) {
        case URL_ACTION_COPY:
        case URL_ACTION_LAUNCH:
            urls_reset(term);
            break;

        case URL_ACTION_PERSISTENT:
            term->url_keys[0] = U'\0';
            render_refresh_urls(term);
            break;
        }
    }

    else if (is_valid) {
        xassert(seq_len + 1 <= ALEN(term->url_keys));
        term->url_keys[seq_len] = wc;
        render_refresh_urls(term);
    }
}

struct vline {
    char *utf8;
    size_t len;          /* Length of utf8[] */
    size_t sz;           /* utf8[] allocated size */
    struct coord *map;   /* Maps utf8[ofs] to grid coordinates */
};

static void
regex_detected(const struct terminal *term, enum url_action action,
               const regex_t *preg, url_list_t *urls)
{
    /*
     * Use regcomp()+regexec() to find patterns.
     *
     * Since we can't feed regexec() one character at a time, and
     * since it doesn't accept wide characters, we need to build utf8
     * strings.
     *
     * Each string represents a logical line (i.e. handle line-wrap).
     * To be able to map regex matches back to the grid, we store the
     * grid coordinates of *each* character, in the line struct as
     * well. This is offset based; utf8[ofs] has its grid coordinates
     * in map[ofs.
     */

    /* There is *at most* term->rows logical lines */
    struct vline vlines[term->rows];
    size_t vline_idx = 0;

    memset(vlines, 0, sizeof(vlines));
    struct vline *vline = &vlines[vline_idx];

    mbstate_t ps = {0};

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

        for (int c = 0; c < term->cols; c++) {
            const struct cell *cell = &row->cells[c];
            const char32_t *wc = &cell->wc;
            size_t wc_count = 1;

            /* Expand combining characters */
            if (wc[0] >= CELL_COMB_CHARS_LO && wc[0] <= CELL_COMB_CHARS_HI) {
                const struct composed *composed =
                    composed_lookup(term->composed, wc[0] - CELL_COMB_CHARS_LO);
                xassert(composed != NULL);

                wc = composed->chars;
                wc_count = composed->count;
            }

            else if (wc[0] >= CELL_SPACER)
                continue;

            /* Convert wide character to utf8 */
            for (size_t i = 0; i < wc_count; i++) {
                char buf[16];
                size_t char_len = c32rtomb(buf, wc[i], &ps);

                if (char_len == (size_t)-1)
                    continue;


                for (size_t j = 0; j < char_len; j++) {
                    const size_t requires_size = vline->len + char_len;

                    /* Need to grow? Remember to save at least one byte for terminator */
                    if (vline->sz == 0 || requires_size > vline->sz - 1) {
                        const size_t new_size = requires_size * 2;
                        vline->utf8 = xreallocarray(vline->utf8, new_size, 1);
                        vline->map = xreallocarray(vline->map, new_size, sizeof(vline->map[0]));
                        vline->sz = new_size;
                    }

                    vline->utf8[vline->len + j] =
                        (buf[j] == '\0') ? ' ' : buf[j];
                    vline->map[vline->len + j] = (struct coord){c, term->grid->view + r};
                }

                vline->len += char_len;
            }
        }

        if (row->linebreak) {
            if (vline->len > 0) {
                vline->utf8[vline->len++] = '\0';
                ps = (mbstate_t){0};

                vline_idx++;
                vline = &vlines[vline_idx];
            }
        }
    }

    /* Terminate the last line, if necessary */
    if (vline_idx < ALEN(vlines) &&
        vline->len > 0 && vline->utf8[vline->len - 1] != '\0')
    {
        vline->utf8[vline->len++] = '\0';
    }

    for (size_t i = 0; i < ALEN(vlines); i++) {
        const struct vline *v = &vlines[i];
        if (v->utf8 == NULL)
            continue;

        const char *search_string = v->utf8;
        while (true) {
            regmatch_t matches[preg->re_nsub + 1];
            int r = regexec(preg, search_string, preg->re_nsub + 1, matches, 0);

            if (r == REG_NOMATCH)
                break;

            const size_t mlen = matches[1].rm_eo - matches[1].rm_so;
            const size_t start = &search_string[matches[1].rm_so] - v->utf8;
            const size_t end = start + mlen;

            LOG_DBG(
                "regex match at row %d: %.*s (%zu bytes), row/col = %dx%d",
                matches[1].rm_so, (int)mlen, &search_string[matches[1].rm_so],
                mlen, v->map[start].row, v->map[start].col);

            tll_push_back(
                *urls,
                ((struct url){
                    .id = (uint64_t)rand() << 32 | rand(),
                    .url = xstrndup(&v->utf8[start], mlen),
                    .range = {
                        .start = v->map[start],
                        .end = v->map[end - 1], /* Inclusive */
                    },
                    .action = action,
                    .osc8 = false}));

            search_string += matches[0].rm_eo;
        }

        free(v->utf8);
        free(v->map);
    }
}

static void
osc8_uris(const struct terminal *term, enum url_action action, url_list_t *urls)
{
    bool dont_touch_url_attr = false;

    switch (term->conf->url.osc8_underline) {
    case OSC8_UNDERLINE_URL_MODE:
        dont_touch_url_attr = false;
        break;

    case OSC8_UNDERLINE_ALWAYS:
        dont_touch_url_attr = true;
        break;
    }

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);
        const struct row_data *extra = row->extra;

       if (extra == NULL)
            continue;

       for (size_t i = 0; i < extra->uri_ranges.count; i++) {
           const struct row_range *range = &extra->uri_ranges.v[i];

           struct coord start = {
               .col = range->start,
               .row = r + term->grid->view,
           };
           struct coord end = {
               .col = range->end,
               .row = r + term->grid->view,
           };
           tll_push_back(
               *urls,
               ((struct url){
                   .id = range->uri.id,
                   .url = xstrdup(range->uri.uri),
                   .range = {
                       .start = start,
                       .end = end,
                   },
                   .action = action,
                   .url_mode_dont_change_url_attr = dont_touch_url_attr,
                   .osc8 = true}));
       }
    }
}

static void
remove_overlapping(url_list_t *urls, int cols)
{
    tll_foreach(*urls, outer) {
        tll_foreach(*urls, inner) {
            if (outer == inner)
                continue;

            const struct url *out = &outer->item;
            const struct url *in = &inner->item;

            uint64_t in_start = in->range.start.row * cols + in->range.start.col;
            uint64_t in_end = in->range.end.row * cols + in->range.end.col;

            uint64_t out_start = out->range.start.row * cols + out->range.start.col;
            uint64_t out_end = out->range.end.row * cols + out->range.end.col;

            if ((in_start <= out_start && in_end >= out_start) ||
                (in_start <= out_end && in_end >= out_end) ||
                (in_start >= out_start && in_end <= out_end))
            {
                /*
                 * OSC-8 URLs can't overlap with each
                 * other.
                 *
                 * Similarly, auto-detected URLs cannot overlap with
                 * each other.
                 *
                 * But OSC-8 URLs can overlap with auto-detected ones.
                 */
                xassert(in->osc8 || out->osc8);

                if (in->osc8)
                    outer->item.duplicate = true;
                else
                    inner->item.duplicate = true;
            }
        }
    }

    tll_foreach(*urls, it) {
        if (it->item.duplicate) {
            url_destroy(&it->item);
            tll_remove(*urls, it);
        }
    }
}

void
urls_collect(const struct terminal *term, enum url_action action,
             const regex_t *preg, bool osc8, url_list_t *urls)
{
    xassert(tll_length(term->urls) == 0);
    if (osc8)
        osc8_uris(term, action, urls);
    regex_detected(term, action, preg, urls);
    remove_overlapping(urls, term->grid->num_cols);
}

static void
generate_key_combos(const struct config *conf,
                    size_t count, char32_t *combos[static count])
{
    const char32_t *alphabet = conf->url.label_letters;
    const size_t alphabet_len = c32len(alphabet);

    size_t hints_count = 1;
    char32_t **hints = xmalloc(hints_count * sizeof(hints[0]));

    hints[0] = xc32dup(U"");

    size_t offset = 0;
    do {
        const char32_t *prefix = hints[offset++];
        const size_t prefix_len = c32len(prefix);

        hints = xrealloc(hints, (hints_count + alphabet_len) * sizeof(hints[0]));

        const char32_t *wc = &alphabet[0];
        for (size_t i = 0; i < alphabet_len; i++, wc++) {
            char32_t *hint = xmalloc((prefix_len + 1 + 1) * sizeof(char32_t));
            hints[hints_count + i] = hint;

            /* Will be reversed later */
            hint[0] = *wc;
            c32cpy(&hint[1], prefix);
        }
        hints_count += alphabet_len;
    } while (hints_count - offset < count);

    xassert(hints_count - offset >= count);

    /* Copy slice of 'hints' array to the caller provided array */
    for (size_t i = 0; i < hints_count; i++) {
        if (i >= offset && i < offset + count)
            combos[i - offset] = hints[i];
        else
            free(hints[i]);
    }
    free(hints);

    /* Reverse all strings */
    for (size_t i = 0; i < count; i++) {
        const size_t len = c32len(combos[i]);
        for (size_t j = 0; j < len / 2; j++) {
            char32_t tmp = combos[i][j];
            combos[i][j] = combos[i][len - j - 1];
            combos[i][len - j - 1] = tmp;
        }
    }
}

void
urls_assign_key_combos(const struct config *conf, url_list_t *urls)
{
    const size_t count = tll_length(*urls);
    if (count == 0)
        return;

    char32_t *combos[count];
    generate_key_combos(conf, count, combos);

    size_t combo_idx = 0;

    tll_rforeach(*urls, it) {
        bool id_already_seen = false;

        /* Look for already processed URLs where both the URI and the
         * ID matches */
        tll_rforeach(*urls, it2) {
            if (&it->item == &it2->item)
                break;

            if (it->item.id == it2->item.id &&
                streq(it->item.url, it2->item.url))
            {
                id_already_seen = true;
                break;
            }
        }

        if (id_already_seen)
            continue;

        /*
         * Scan previous URLs, and check if *this* URL matches any of
         * them; if so, reuse the *same* key combo.
         */
        bool url_already_seen = false;
        tll_rforeach(*urls, it2) {
            if (&it->item == &it2->item)
                break;

            if (streq(it->item.url, it2->item.url)) {
                it->item.key = xc32dup(it2->item.key);
                url_already_seen = true;
                break;
            }
        }

        if (!url_already_seen)
            it->item.key = combos[combo_idx++];
    }

    /* Free combos we didn't use up */
    for (size_t i = combo_idx; i < count; i++)
        free(combos[i]);

#if defined(_DEBUG) && LOG_ENABLE_DBG
    tll_rforeach(*urls, it) {
        if (it->item.key == NULL)
            continue;

        char *key = ac32tombs(it->item.key);
        xassert(key != NULL);

        LOG_DBG("URL: %s (key=%s, id=%"PRIu64")", it->item.url, key, it->item.id);
        free(key);
    }
#endif
}

static void
tag_cells_for_url(struct terminal *term, const struct url *url, bool value)
{
    if (url->url_mode_dont_change_url_attr)
        return;

    struct grid *grid = term->url_grid_snapshot;
    xassert(grid != NULL);

    const struct coord *start = &url->range.start;
    const struct coord *end = &url->range.end;

    size_t end_r = end->row & (grid->num_rows - 1);

    size_t r = start->row & (grid->num_rows - 1);
    size_t c = start->col;

    struct row *row = grid->rows[r];
    row->dirty = true;

    while (true) {
        struct cell *cell = &row->cells[c];
        cell->attrs.url = value;
        cell->attrs.clean = 0;

        if (r == end_r && c == end->col)
            break;

        if (++c >= term->cols) {
            r = (r + 1) & (grid->num_rows - 1);
            c = 0;

            row = grid->rows[r];
            if (row == NULL) {
                /* Un-allocated scrollback. This most likely means a
                 * runaway OSC-8 URL. */
                break;
            }
            row->dirty = true;
        }
    }
}

void
urls_render(struct terminal *term, const struct config_spawn_template *launch)
{
    struct wl_window *win = term->window;

    if (tll_length(win->term->urls) == 0)
        return;

    /* Disable IME while in URL-mode */
    if (term_ime_is_enabled(term)) {
        term->ime_reenable_after_url_mode = true;
        term_ime_disable(term);
    }

    /* Dirty the last cursor, to ensure it is erased */
    {
        struct row *cursor_row = term->render.last_cursor.row;
        if (cursor_row != NULL) {
            struct cell *cell = &cursor_row->cells[term->render.last_cursor.col];
            cell->attrs.clean = 0;
            cursor_row->dirty = true;
        }
    }
    term->render.last_cursor.row = NULL;

    /* Clear scroll damage, to ensure we don't apply it twice (once on
     * the snapshot:ed grid, and then later again on the real grid) */
    tll_free(term->grid->scroll_damage);

    /* Damage the entire view, to ensure a full screen redraw, both
     * now, when entering URL mode, and later, when exiting it. */
    term_damage_view(term);

    /* Snapshot the current grid */
    term->url_grid_snapshot = grid_snapshot(term->grid);

    /* Remember which launcher to use */
    term->url_launch = launch;

    xassert(tll_length(win->urls) == 0);
    tll_foreach(win->term->urls, it) {
        struct wl_url url = {.url = &it->item};
        wayl_win_subsurface_new(win, &url.surf, false);

        tll_push_back(win->urls, url);
        tag_cells_for_url(term, &it->item, true);
    }

    render_refresh_urls(term);
    render_refresh(term);
}

static void
url_destroy(struct url *url)
{
    free(url->url);
    free(url->key);
}

void
urls_reset(struct terminal *term)
{
    if (likely(tll_length(term->urls) == 0)) {
        xassert(term->url_grid_snapshot == NULL);
        return;
    }

    grid_free(term->url_grid_snapshot);
    free(term->url_grid_snapshot);
    term->url_grid_snapshot = NULL;

    /*
     * Make sure "last cursor" doesn't point to a row in the just
     * free:d snapshot grid.
     *
     * Note that it will still be erased properly (if hasn't already),
     * since we marked the cell as dirty *before* taking the grid
     * snapshot.
     */
    term->render.last_cursor.row = NULL;

    if (term->window != NULL) {
        tll_foreach(term->window->urls, it) {
            wayl_win_subsurface_destroy(&it->item.surf);
            tll_remove(term->window->urls, it);
        }
    }

    tll_foreach(term->urls, it) {
        url_destroy(&it->item);
        tll_remove(term->urls, it);
    }

    term->urls_show_uri_on_jump_label = false;
    memset(term->url_keys, 0, sizeof(term->url_keys));

    /* Re-enable IME, if it was enabled before we entered URL-mode */
    if (term->ime_reenable_after_url_mode) {
        term->ime_reenable_after_url_mode = false;
        term_ime_enable(term);
    }

    render_refresh(term);
}
