#include "url-mode.h"

#include <string.h>
#include <wctype.h>

#define LOG_MODULE "url-mode"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "grid.h"
#include "render.h"
#include "selection.h"
#include "spawn.h"
#include "terminal.h"
#include "util.h"
#include "xmalloc.h"

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

    case BIND_ACTION_URL_COUNT:
        return false;

    }
    return true;
}

static void
activate_url(struct seat *seat, struct terminal *term, const struct url *url)
{
    size_t chars = wcstombs(NULL, url->url, 0);

    if (chars != (size_t)-1) {
        char *url_utf8 = malloc(chars + 1);
        wcstombs(url_utf8, url->url, chars + 1);

        switch (url->action) {
        case URL_ACTION_COPY:
            if (text_to_clipboard(seat, term, url_utf8, seat->kbd.serial)) {
                /* Now owned by our clipboard “manager” */
                url_utf8 = NULL;
            }
            break;

        case URL_ACTION_LAUNCH: {
            size_t argc;
            char **argv;

            if (spawn_expand_template(
                    &term->conf->url_launch, 1,
                    (const char *[]){"url"},
                    (const char *[]){url_utf8},
                    &argc, &argv))
            {
                spawn(term->reaper, term->cwd, argv, -1, -1, -1);

                for (size_t i = 0; i < argc; i++)
                    free(argv[i]);
                free(argv);
            }
            break;
        }
        }

        free(url_utf8);
    }
}

void
urls_input(struct seat *seat, struct terminal *term, uint32_t key,
           xkb_keysym_t sym, xkb_mod_mask_t mods, uint32_t serial)
{
    /* Key bindings */
    tll_foreach(seat->kbd.bindings.url, it) {
        if (it->item.bind.mods != mods)
            continue;

        /* Match symbol */
        if (it->item.bind.sym == sym) {
            execute_binding(seat, term, it->item.action, serial);
            return;
        }

        /* Match raw key code */
        tll_foreach(it->item.bind.key_codes, code) {
            if (code->item == key) {
                execute_binding(seat, term, it->item.action, serial);
                return;
            }
        }
    }

    wchar_t wc = xkb_state_key_get_utf32(seat->kbd.xkb_state, key);

    /*
     * Determine if this is a “valid” key. I.e. if there is an URL
     * label with a key combo where this key is the next in
     * sequence.
     */

    size_t seq_len = wcslen(term->url_keys);

    bool is_valid = false;
    const struct url *match = NULL;

    tll_foreach(term->urls, it) {
        const struct url *url = &it->item;
        const size_t key_len = wcslen(it->item.key);

        if (key_len >= seq_len + 1 &&
            wcsncmp(url->key, term->url_keys, seq_len) == 0 &&
            url->key[seq_len] == wc)
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
    }
}

IGNORE_WARNING("-Wpedantic")

static void
auto_detected(const struct terminal *term, enum url_action action, url_list_t *urls)
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
                case L',': case L';': case L'=': case L'"':
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
                        case L'!': case L'"': case L'\'':
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

                    tll_push_back(
                        *urls,
                        ((struct url){
                            .url = xwcsdup(url),
                            .text = xwcsdup(L""),
                            .start = start,
                            .end = end,
                            .action = action}));

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

void
urls_collect(const struct terminal *term, enum url_action action, url_list_t *urls)
{
    xassert(tll_length(term->urls) == 0);
    auto_detected(term, action, urls);
}

static void url_destroy(struct url *url);

void
urls_assign_key_combos(url_list_t *urls)
{
    static const wchar_t *const combos[] = {
        L"f", L"j", L"d", L"k", L"e", L"i", L"c", L"m", L"r",
        L"u", L"s", L"l", L"w", L"o", L"x", L"a", L"q", L"p",
    };

    size_t idx = 0;
    tll_foreach(*urls, it) {
        if (idx < ALEN(combos)) {
            xassert(wcslen(combos[idx]) < ALEN(it->item.key) - 1);
            wcscpy(it->item.key, combos[idx]);
        } else {
            url_destroy(&it->item);
            tll_remove(*urls, it);
        }

        if (++idx == ALEN(combos)) {
            LOG_WARN("not enough key combos (%zu) for %zu URLs",
                     ALEN(combos), tll_length(*urls));
        }
    }

#if defined(_DEBUG) && LOG_ENABLE_DBG
    tll_foreach(*urls, it) {
        char url[1024];
        wcstombs(url, it->item.url, sizeof(url) - 1);

        char key[32];
        wcstombs(key, it->item.key, sizeof(key) - 1);

        LOG_DBG("URL: %s (%s)", url, key);
    }
#endif

}

static void
tag_cells_for_url(struct terminal *term, const struct url *url, bool value)
{
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
    struct wayland *wayl = term->wl;

    xassert(tll_length(win->urls) == 0);
    tll_foreach(win->term->urls, it) {
        struct wl_surface *surf = wl_compositor_create_surface(wayl->compositor);
        wl_surface_set_user_data(surf, win);

        struct wl_subsurface *sub_surf = NULL;

        if (surf != NULL) {
            sub_surf = wl_subcompositor_get_subsurface(
                wayl->sub_compositor, surf, win->surface);

            if (sub_surf != NULL)
                wl_subsurface_set_sync(sub_surf);
        }

        if (surf == NULL || sub_surf == NULL) {
            LOG_WARN("failed to create URL (sub)-surface");

            if (surf != NULL) {
                wl_surface_destroy(surf);
                surf = NULL;
            }

            if (sub_surf != NULL) {
                wl_subsurface_destroy(sub_surf);
                sub_surf = NULL;
            }
        }

        struct wl_url url = {
            .url = &it->item,
            .surf = surf,
            .sub_surf = sub_surf,
        };

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
    free(url->text);
}

void
urls_reset(struct terminal *term)
{
    if (likely(tll_length(term->urls) == 0))
        return;

    if (term->window != NULL) {
        tll_foreach(term->window->urls, it) {
            if (it->item.sub_surf != NULL)
                wl_subsurface_destroy(it->item.sub_surf);
            if (it->item.surf != NULL)
                wl_surface_destroy(it->item.surf);
        }
        tll_free(term->window->urls);
    }

    tll_foreach(term->urls, it) {
        tag_cells_for_url(term, &it->item, false);
        url_destroy(&it->item);
    }
    tll_free(term->urls);

    memset(term->url_keys, 0, sizeof(term->url_keys));
    render_refresh(term);
}
