#include "url-mode.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <unistd.h>

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

    bool ret = false;

    if (spawn_expand_template(
            &term->conf->url.launch, 1,
            (const char *[]){"url"},
            (const char *[]){url},
            &argc, &argv))
    {
        ret = spawn(term->reaper, term->cwd, argv,
              dev_null, dev_null, dev_null, xdg_activation_token);

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
             uint32_t serial)
{
    switch (url->action) {
    case URL_ACTION_COPY:
        text_to_clipboard(seat, term, xstrdup(url->url), seat->kbd.serial);
        break;

    case URL_ACTION_LAUNCH:
    case URL_ACTION_PERSISTENT: {
        spawn_url_launcher(seat, term, url->url, serial);
        break;
    }
    }
}

void
urls_input(struct seat *seat, struct terminal *term,
           const struct key_binding_set *bindings, uint32_t key,
           xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
           const xkb_keysym_t *raw_syms, size_t raw_count,
           uint32_t serial)
{
    /* Key bindings */
    tll_foreach(bindings->url, it) {
        const struct key_binding *bind = &it->item;

        /* Match translated symbol */
        if (bind->k.sym == sym &&
            bind->mods == (mods & ~consumed))
        {
            execute_binding(seat, term, bind, serial);
            return;
        }

        if (bind->mods != mods)
            continue;

        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i]) {
                execute_binding(seat, term, bind, serial);
                return;
            }
        }

        /* Match raw key code */
        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key) {
                execute_binding(seat, term, bind, serial);
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
        activate_url(seat, term, match, serial);

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

static int
c32cmp_single(const void *_a, const void *_b)
{
    const char32_t *a = _a;
    const char32_t *b = _b;
    return *a - *b;
}

static void
auto_detected(const struct terminal *term, enum url_action action,
              url_list_t *urls)
{
    const struct config *conf = term->conf;

    const char32_t *uri_characters = conf->url.uri_characters;
    if (uri_characters == NULL)
        return;

    const size_t uri_characters_count = c32len(uri_characters);
    if (uri_characters_count == 0)
        return;

    size_t max_prot_len = conf->url.max_prot_len;
    char32_t proto_chars[max_prot_len];
    struct coord proto_start[max_prot_len];
    size_t proto_char_count = 0;

    enum {
        STATE_PROTOCOL,
        STATE_URL,
    } state = STATE_PROTOCOL;

    struct coord start = {-1, -1};
    char32_t url[term->cols * term->rows + 1];
    size_t len = 0;

    ssize_t parenthesis = 0;
    ssize_t brackets = 0;
    ssize_t ltgts = 0;

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

        for (int c = 0; c < term->cols; c++) {
            const struct cell *cell = &row->cells[c];

            if (cell->wc >= CELL_SPACER)
                continue;

            const char32_t *wcs = NULL;
            size_t wc_count = 0;

            if (cell->wc >= CELL_COMB_CHARS_LO && cell->wc <= CELL_COMB_CHARS_HI) {
                struct composed *composed =
                    composed_lookup(term->composed, cell->wc - CELL_COMB_CHARS_LO);
                wcs = composed->chars;
                wc_count = composed->count;
            } else {
                wcs = &cell->wc;
                wc_count = 1;
            }

            for (size_t w_idx = 0; w_idx < wc_count; w_idx++) {
                char32_t wc = wcs[w_idx];

                switch (state) {
                case STATE_PROTOCOL:
                  for (size_t i = 0; i < max_prot_len - 1; i++) {
                    proto_chars[i] = proto_chars[i + 1];
                    proto_start[i] = proto_start[i + 1];
                  }

                  if (proto_char_count >= max_prot_len)
                    proto_char_count = max_prot_len - 1;

                  proto_chars[max_prot_len - 1] = wc;
                  proto_start[max_prot_len - 1] = (struct coord){c, r};
                  proto_char_count++;

                  for (size_t i = 0; i < conf->url.prot_count; i++) {
                    size_t prot_len = c32len(conf->url.protocols[i]);

                    if (proto_char_count < prot_len)
                      continue;

                    const char32_t *proto =
                        &proto_chars[max_prot_len - prot_len];

                    if (c32ncasecmp(conf->url.protocols[i], proto, prot_len) ==
                        0) {
                      state = STATE_URL;
                      start = proto_start[max_prot_len - prot_len];

                      c32ncpy(url, proto, prot_len);
                      len = prot_len;

                      parenthesis = brackets = ltgts = 0;
                      break;
                    }
                  }
                  break;

                case STATE_URL: {
                  const char32_t *match =
                      bsearch(&wc, uri_characters, uri_characters_count,
                              sizeof(uri_characters[0]), &c32cmp_single);

                  bool emit_url = false;

                  if (match == NULL) {
                    /*
                     * Character is not a valid URI character. Emit
                     * the URL we've collected so far, *without*
                     * including _this_ character.
                     */
                    emit_url = true;
                  } else {
                    xassert(*match == wc);

                    switch (wc) {
                    default:
                      url[len++] = wc;
                      break;

                    case U'(':
                      parenthesis++;
                      url[len++] = wc;
                      break;

                    case U'[':
                      brackets++;
                      url[len++] = wc;
                      break;

                    case U'<':
                      ltgts++;
                      url[len++] = wc;
                      break;

                    case U')':
                      if (--parenthesis < 0)
                        emit_url = true;
                      else
                        url[len++] = wc;
                      break;

                    case U']':
                      if (--brackets < 0)
                        emit_url = true;
                      else
                        url[len++] = wc;
                      break;

                    case U'>':
                      if (--ltgts < 0)
                        emit_url = true;
                      else
                        url[len++] = wc;
                      break;
                    }
                  }

                  if (c >= term->cols - 1 && row->linebreak) {
                    /*
                     * Endpoint is inclusive, and we'll be subtracting
                     * 1 from the column when emitting the URL.
                     */
                    c++;
                    emit_url = true;
                  }

                  if (emit_url) {
                    struct coord end = {c, r};

                    if (--end.col < 0) {
                      end.row--;
                      end.col = term->cols - 1;
                    }

                    /* Heuristic to remove trailing characters that
                     * are valid URL characters, but typically not at
                     * the end of the URL */
                    bool done = false;
                    do {
                      switch (url[len - 1]) {
                      case U'.':
                      case U',':
                      case U':':
                      case U';':
                      case U'?':
                      case U'!':
                      case U'"':
                      case U'\'':
                      case U'%':
                        len--;
                        end.col--;
                        if (end.col < 0) {
                          end.row--;
                          end.col = term->cols - 1;
                        }
                        break;

                      default:
                        done = true;
                        break;
                      }
                    } while (!done);

                    url[len] = U'\0';

                    start.row += term->grid->view;
                    end.row += term->grid->view;

                    char *url_utf8 = ac32tombs(url);
                    if (url_utf8 != NULL) {
                      tll_push_back(
                          *urls,
                          ((struct url){.id = (uint64_t)rand() << 32 | rand(),
                                        .url = url_utf8,
                                        .range =
                                            {
                                                .start = start,
                                                .end = end,
                                            },
                                        .action = action,
                                        .osc8 = false}));
                    }

                    state = STATE_PROTOCOL;
                    len = 0;
                    parenthesis = brackets = ltgts = 0;
                  }
                  break;
                }
                }
            }
        }
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
           const struct row_uri_range *range = &extra->uri_ranges.v[i];

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
                   .id = range->id,
                   .url = xstrdup(range->uri),
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
urls_collect(const struct terminal *term, enum url_action action, url_list_t *urls)
{
    xassert(tll_length(term->urls) == 0);
    osc8_uris(term, action, urls);
    auto_detected(term, action, urls);
    remove_overlapping(urls, term->grid->num_cols);
}

static int
c32cmp_qsort_wrapper(const void *_a, const void *_b)
{
    const char32_t *a = *(const char32_t **)_a;
    const char32_t *b = *(const char32_t **)_b;
    return c32cmp(a, b);
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

    /* Sorting is a kind of shuffle, since we're sorting on the
     * *reversed* strings */
    qsort(combos, count, sizeof(char32_t *), &c32cmp_qsort_wrapper);

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

    tll_foreach(*urls, it) {
        bool id_already_seen = false;

        /* Look for already processed URLs where both the URI and the
         * ID matches */
        tll_foreach(*urls, it2) {
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
        tll_foreach(*urls, it2) {
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
    tll_foreach(*urls, it) {
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
urls_render(struct terminal *term)
{
    struct wl_window *win = term->window;

    if (tll_length(win->term->urls) == 0)
        return;

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

            /* Work around Sway bug - unmapping a sub-surface does not
             * damage the underlying surface */
            quirk_sway_subsurface_unmap(term);
        }
    }

    tll_foreach(term->urls, it) {
        url_destroy(&it->item);
        tll_remove(term->urls, it);
    }

    term->urls_show_uri_on_jump_label = false;
    memset(term->url_keys, 0, sizeof(term->url_keys));

    render_refresh(term);
}
