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
#include "grid.h"
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
                enum bind_action_url action, uint32_t serial)
{
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

static void
activate_url(struct seat *seat, struct terminal *term, const struct url *url)
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
        if (text_to_clipboard(seat, term, url_string, seat->kbd.serial)) {
            /* Now owned by our clipboard “manager” */
            url_string = NULL;
        }
        break;

    case URL_ACTION_LAUNCH: {
        size_t argc;
        char **argv;

        int dev_null = open("/dev/null", O_RDWR);

        if (dev_null < 0) {
            LOG_ERRNO("failed to open /dev/null");
            break;
        }

        if (spawn_expand_template(
                &term->conf->url_launch, 1,
                (const char *[]){"url"},
                (const char *[]){url_string},
                &argc, &argv))
        {
            spawn(term->reaper, term->cwd, argv, dev_null, dev_null, dev_null);

            for (size_t i = 0; i < argc; i++)
                free(argv[i]);
            free(argv);
        }

        close(dev_null);
        break;
    }
    }

    free(url_string);
}

void
urls_input(struct seat *seat, struct terminal *term, uint32_t key,
           xkb_keysym_t sym, xkb_mod_mask_t mods, uint32_t serial)
{
    /* Key bindings */
    tll_foreach(seat->kbd.bindings.url, it) {
        if (it->item.mods != mods)
            continue;

        /* Match symbol */
        if (it->item.sym == sym) {
            execute_binding(seat, term, it->item.action, serial);
            return;
        }

        /* Match raw key code */
        tll_foreach(it->item.key_codes, code) {
            if (code->item == key) {
                execute_binding(seat, term, it->item.action, serial);
                return;
            }
        }
    }

    size_t seq_len = wcslen(term->url_keys);

    if (sym == XKB_KEY_BackSpace) {
        if (seq_len > 0) {
            term->url_keys[seq_len - 1] = L'\0';
            render_refresh_urls(term);
        }

        return;
    }

    if (mods & ~consumed)
        return;

    wchar_t wc = xkb_state_key_get_utf32(seat->kbd.xkb_state, key);

    /*
     * Determine if this is a “valid” key. I.e. if there is an URL
     * label with a key combo where this key is the next in
     * sequence.
     */

    bool is_valid = false;
    const struct url *match = NULL;

    tll_foreach(term->urls, it) {
        if (it->item.key == NULL)
            continue;

        const struct url *url = &it->item;
        const size_t key_len = wcslen(it->item.key);

        if (key_len >= seq_len + 1 &&
            wcsncasecmp(url->key, term->url_keys, seq_len) == 0 &&
            towlower(url->key[seq_len]) == towlower(wc))
        {
            is_valid = true;
            if (key_len == seq_len + 1) {
                match = url;
                break;
            }
        }
    }

    if (match) {
        activate_url(seat, term, match);
        urls_reset(term);
    }

    else if (is_valid) {
        xassert(seq_len + 1 <= ALEN(term->url_keys));
        term->url_keys[seq_len] = wc;
        render_refresh_urls(term);
    }
}

IGNORE_WARNING("-Wpedantic")

static void
auto_detected(const struct terminal *term, enum url_action action,
              url_list_t *urls)
{
    static const wchar_t *const prots[] = {
        L"http://",
        L"https://",
        L"ftp://",
        L"ftps://",
        L"file://",
        L"gemini://",
        L"gopher://",
    };

    size_t max_prot_len = 0;
    for (size_t i = 0; i < ALEN(prots); i++) {
        size_t len = wcslen(prots[i]);
        if (len > max_prot_len)
            max_prot_len = len;
    }

    wchar_t proto_chars[max_prot_len];
    struct coord proto_start[max_prot_len];
    size_t proto_char_count = 0;

    enum {
        STATE_PROTOCOL,
        STATE_URL,
    } state = STATE_PROTOCOL;

    struct coord start = {-1, -1};
    wchar_t url[term->cols * term->rows + 1];
    size_t len = 0;

    ssize_t parenthesis = 0;
    ssize_t brackets = 0;

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

        for (int c = 0; c < term->cols; c++) {
            const struct cell *cell = &row->cells[c];
            wchar_t wc = cell->wc;

            switch (state) {
            case STATE_PROTOCOL:
                for (size_t i = 0; i < max_prot_len - 1; i++) {
                    proto_chars[i] = proto_chars[i + 1];
                    proto_start[i] = proto_start[i + 1];
                }

                if (proto_char_count == max_prot_len)
                    proto_char_count--;

                proto_chars[proto_char_count] = wc;
                proto_start[proto_char_count] = (struct coord){c, r};
                proto_char_count++;

                for (size_t i = 0; i < ALEN(prots); i++) {
                    size_t prot_len = wcslen(prots[i]);

                    if (proto_char_count < prot_len)
                        continue;

                    const wchar_t *proto = &proto_chars[max_prot_len - prot_len];

                    if (wcsncasecmp(prots[i], proto, prot_len) == 0) {
                        state = STATE_URL;
                        start = proto_start[max_prot_len - prot_len];

                        wcsncpy(url, proto, prot_len);
                        len = prot_len;

                        parenthesis = brackets = 0;
                        break;
                    }
                }
                break;

            case STATE_URL: {
                // static const wchar_t allowed[] =
                //    L"abcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=";
                // static const wchar_t unwise[] = L"{}|\\^[]`";
                // static const wchar_t reserved[] = L";/?:@&=+$,";

                bool emit_url = false;
                switch (wc) {
                case L'a'...L'z':
                case L'A'...L'Z':
                case L'0'...L'9':
                case L'-': case L'.': case L'_': case L'~': case L':':
                case L'/': case L'?': case L'#': case L'@': case L'!':
                case L'$': case L'&': case L'\'': case L'*': case L'+':
                case L',': case L';': case L'=': case L'"': case L'%':
                    url[len++] = wc;
                    break;

                case L'(':
                    parenthesis++;
                    url[len++] = wc;
                    break;

                case L'[':
                    brackets++;
                    url[len++] = wc;
                    break;

                case L')':
                    if (--parenthesis < 0)
                        emit_url = true;
                    else
                        url[len++] = wc;
                    break;

                case L']':
                    if (--brackets < 0)
                        emit_url = true;
                    else
                        url[len++] = wc;
                    break;

                default:
                    emit_url = true;
                    break;
                }

                if (c >= term->cols - 1 && row->linebreak)
                    emit_url = true;

                if (emit_url) {
                    /* Heuristic to remove trailing characters that
                     * are valid URL characters, but typically not at
                     * the end of the URL */
                    bool done = false;
                    struct coord end = {c, r};

                    if (--end.col < 0) {
                        end.row--;
                        end.col = term->cols - 1;
                    }

                    do {
                        switch (url[len - 1]) {
                        case L'.': case L',': case L':': case L';': case L'?':
                        case L'!': case L'"': case L'\'': case L'%':
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

                    url[len] = L'\0';

                    start.row += term->grid->view;
                    end.row += term->grid->view;

                    size_t chars = wcstombs(NULL, url, 0);
                    if (chars != (size_t)-1) {
                        char *url_utf8 = xmalloc((chars + 1) * sizeof(wchar_t));
                        wcstombs(url_utf8, url, chars + 1);

                        tll_push_back(
                            *urls,
                            ((struct url){
                                .id = (uint64_t)rand() << 32 | rand(),
                                .url = url_utf8,
                                .start = start,
                                .end = end,
                                .action = action}));
                    }

                    state = STATE_PROTOCOL;
                    len = 0;
                    parenthesis = brackets = 0;
                }
                break;
            }
            }
        }
    }
}

UNIGNORE_WARNINGS

static void
osc8_uris(const struct terminal *term, enum url_action action, url_list_t *urls)
{
    bool dont_touch_url_attr = false;

    switch (term->conf->osc8_underline) {
    case OSC8_UNDERLINE_URL_MODE:
        dont_touch_url_attr = false;
        break;

    case OSC8_UNDERLINE_ALWAYS:
        dont_touch_url_attr = true;
        break;
    }

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

       if (row->extra == NULL)
            continue;

       tll_foreach(row->extra->uri_ranges, it) {
           struct coord start = {
               .col = it->item.start,
               .row = r + term->grid->view,
           };
           struct coord end = {
               .col = it->item.end,
               .row = r + term->grid->view,
           };
           tll_push_back(
               *urls,
               ((struct url){
                   .id = it->item.id,
                   .url = xstrdup(it->item.uri),
                   .start = start,
                   .end = end,
                   .action = action,
                   .url_mode_dont_change_url_attr = dont_touch_url_attr}));
       }
    }
}

static void
remove_duplicates(url_list_t *urls)
{
    tll_foreach(*urls, outer) {
        tll_foreach(*urls, inner) {
            if (outer == inner)
                continue;

            if (outer->item.start.row == inner->item.start.row &&
                outer->item.start.col == inner->item.start.col &&
                outer->item.end.row == inner->item.end.row &&
                outer->item.end.col == inner->item.end.col)
            {
                url_destroy(&inner->item);
                tll_remove(*urls, inner);
            }
        }
    }
}

void
urls_collect(const struct terminal *term, enum url_action action, url_list_t *urls)
{
    xassert(tll_length(term->urls) == 0);
    osc8_uris(term, action, urls);
    auto_detected(term, action, urls);
    remove_duplicates(urls);
}

static int
wcscmp_qsort_wrapper(const void *_a, const void *_b)
{
    const wchar_t *a = *(const wchar_t **)_a;
    const wchar_t *b = *(const wchar_t **)_b;
    return wcscmp(a, b);
}

static void
generate_key_combos(const struct config *conf,
                    size_t count, wchar_t *combos[static count])
{
    const wchar_t *alphabet = conf->jump_label_letters;
    const size_t alphabet_len = wcslen(alphabet);

    size_t hints_count = 1;
    wchar_t **hints = xmalloc(hints_count * sizeof(hints[0]));

    hints[0] = xwcsdup(L"");

    size_t offset = 0;
    do {
        const wchar_t *prefix = hints[offset++];
        const size_t prefix_len = wcslen(prefix);

        hints = xrealloc(hints, (hints_count + alphabet_len) * sizeof(hints[0]));

        const wchar_t *wc = &alphabet[0];
        for (size_t i = 0; i < alphabet_len; i++, wc++) {
            wchar_t *hint = xmalloc((prefix_len + 1 + 1) * sizeof(wchar_t));
            hints[hints_count + i] = hint;

            /* Will be reversed later */
            hint[0] = *wc;
            wcscpy(&hint[1], prefix);
        }
        hints_count += alphabet_len;
    } while (hints_count - offset < count);

    xassert(hints_count - offset >= count);

    /* Copy slice of ‘hints’ array to the caller provided array */
    for (size_t i = 0; i < hints_count; i++) {
        if (i >= offset && i < offset + count)
            combos[i - offset] = hints[i];
        else
            free(hints[i]);
    }
    free(hints);

    /* Sorting is a kind of shuffle, since we’re sorting on the
     * *reversed* strings */
    qsort(combos, count, sizeof(wchar_t *), &wcscmp_qsort_wrapper);

    /* Reverse all strings */
    for (size_t i = 0; i < count; i++) {
        const size_t len = wcslen(combos[i]);
        for (size_t j = 0; j < len / 2; j++) {
            wchar_t tmp = combos[i][j];
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

    uint64_t seen_ids[count];
    wchar_t *combos[count];
    generate_key_combos(conf, count, combos);

    size_t combo_idx = 0;
    size_t id_idx = 0;

    tll_foreach(*urls, it) {
        bool id_already_seen = false;

        for (size_t i = 0; i < id_idx; i++) {
            if (it->item.id == seen_ids[i]) {
                id_already_seen = true;
                break;
            }
        }

        if (id_already_seen)
            continue;
        seen_ids[id_idx++] = it->item.id;

        /*
         * Scan previous URLs, and check if *this* URL matches any of
         * them; if so, re-use the *same* key combo.
         */
        bool url_already_seen = false;
        tll_foreach(*urls, it2) {
            if (&it->item == &it2->item)
                break;

            if (strcmp(it->item.url, it2->item.url) == 0) {
                it->item.key = xwcsdup(it2->item.key);
                url_already_seen = true;
                break;
            }
        }

        if (!url_already_seen)
            it->item.key = combos[combo_idx++];
    }

    /* Free combos we didn’t use up */
    for (size_t i = combo_idx; i < count; i++)
        free(combos[i]);

#if defined(_DEBUG) && LOG_ENABLE_DBG
    tll_foreach(*urls, it) {
        if (it->item.key == NULL)
            continue;

        char key[32];
        wcstombs(key, it->item.key, sizeof(key) - 1);
        LOG_DBG("URL: %s (%s)", it->item.url, key);
    }
#endif
}

static void
tag_cells_for_url(struct terminal *term, const struct url *url, bool value)
{
    if (url->url_mode_dont_change_url_attr)
        return;

    const struct coord *start = &url->start;
    const struct coord *end = &url->end;

    size_t end_r = end->row & (term->grid->num_rows - 1);

    size_t r = start->row & (term->grid->num_rows - 1);
    size_t c = start->col;

    struct row *row = term->grid->rows[r];
    row->dirty = true;

    while (true) {
        struct cell *cell = &row->cells[c];
        cell->attrs.url = value;
        cell->attrs.clean = 0;

        if (r == end_r && c == end->col)
            break;

        if (++c >= term->cols) {
            r = (r + 1) & (term->grid->num_rows - 1);
            c = 0;

            row = term->grid->rows[r];
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

    xassert(tll_length(win->urls) == 0);
    tll_foreach(win->term->urls, it) {
        struct wl_url url = {.url = &it->item};
        wayl_win_subsurface_new(win, &url.surf);

        tll_push_back(win->urls, url);
        tag_cells_for_url(term, &it->item, true);
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

    /* Clear scroll damage, to ensure we don’t apply it twice (once on
     * the snapshot:ed grid, and then later again on the real grid) */
    tll_free(term->grid->scroll_damage);

    /* Damage the entire view, to ensure a full screen redraw, both
     * now, when entering URL mode, and later, when exiting it. */
    term_damage_view(term);

    /* Snapshot the current grid */
    term->url_grid_snapshot = grid_snapshot(term->grid);

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
     * Make sure “last cursor” doesn’t point to a row in the just
     * free:d snapshot grid.
     *
     * Note that it will still be erased properly (if hasn’t already),
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
        tag_cells_for_url(term, &it->item, false);
        url_destroy(&it->item);
        tll_remove(term->urls, it);
    }

    term->urls_show_uri_on_jump_label = false;
    memset(term->url_keys, 0, sizeof(term->url_keys));

    render_refresh(term);
}
