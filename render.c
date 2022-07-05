#include "render.h"

#include <string.h>
#include <wctype.h>
#include <unistd.h>
#include <signal.h>

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <pthread.h>

#include "macros.h"
#if HAS_INCLUDE(<pthread_np.h>)
#include <pthread_np.h>
#define pthread_setname_np(thread, name) (pthread_set_name_np(thread, name), 0)
#elif defined(__NetBSD__)
#define pthread_setname_np(thread, name) pthread_setname_np(thread, "%s", (void *)name)
#endif

#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <presentation-time.h>

#include <fcft/fcft.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "box-drawing.h"
#include "char32.h"
#include "config.h"
#include "grid.h"
#include "hsl.h"
#include "ime.h"
#include "quirks.h"
#include "search.h"
#include "selection.h"
#include "shm.h"
#include "sixel.h"
#include "url-mode.h"
#include "util.h"
#include "xmalloc.h"

#define TIME_SCROLL_DAMAGE 0

struct renderer {
    struct fdm *fdm;
    struct wayland *wayl;
};

static struct {
    size_t total;
    size_t zero;  /* commits presented in less than one frame interval */
    size_t one;   /* commits presented in one frame interval */
    size_t two;   /* commits presented in two or more frame intervals */
} presentation_statistics = {0};

static void fdm_hook_refresh_pending_terminals(struct fdm *fdm, void *data);

struct renderer *
render_init(struct fdm *fdm, struct wayland *wayl)
{
    struct renderer *renderer = malloc(sizeof(*renderer));
    if (unlikely(renderer == NULL)) {
        LOG_ERRNO("malloc() failed");
        return NULL;
    }

    *renderer = (struct renderer) {
        .fdm = fdm,
        .wayl = wayl,
    };

    if (!fdm_hook_add(fdm, &fdm_hook_refresh_pending_terminals, renderer,
                      FDM_HOOK_PRIORITY_NORMAL))
    {
        LOG_ERR("failed to register FDM hook");
        free(renderer);
        return NULL;
    }

    return renderer;
}

void
render_destroy(struct renderer *renderer)
{
    if (renderer == NULL)
        return;

    fdm_hook_del(renderer->fdm, &fdm_hook_refresh_pending_terminals,
                 FDM_HOOK_PRIORITY_NORMAL);

    free(renderer);
}

static void DESTRUCTOR
log_presentation_statistics(void)
{
    if (presentation_statistics.total == 0)
        return;

    const size_t total = presentation_statistics.total;
    LOG_INFO("presentation statistics: zero=%f%%, one=%f%%, two=%f%%",
             100. * presentation_statistics.zero / total,
             100. * presentation_statistics.one / total,
             100. * presentation_statistics.two / total);
}

static void
sync_output(void *data,
            struct wp_presentation_feedback *wp_presentation_feedback,
            struct wl_output *output)
{
}

struct presentation_context {
    struct terminal *term;
    struct timeval input;
    struct timeval commit;
};

static void
presented(void *data,
          struct wp_presentation_feedback *wp_presentation_feedback,
          uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
          uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    struct presentation_context *ctx = data;
    struct terminal *term = ctx->term;
    const struct timeval *input = &ctx->input;
    const struct timeval *commit = &ctx->commit;

    const struct timeval presented = {
        .tv_sec = (uint64_t)tv_sec_hi << 32 | tv_sec_lo,
        .tv_usec = tv_nsec / 1000,
    };

    bool use_input = (input->tv_sec > 0 || input->tv_usec > 0) &&
        timercmp(&presented, input, >);
    char msg[1024];
    int chars = 0;

    if (use_input && timercmp(&presented, input, <))
        return;
    else if (timercmp(&presented, commit, <))
        return;

    LOG_DBG("commit: %lu s %lu µs, presented: %lu s %lu µs",
            commit->tv_sec, commit->tv_usec, presented.tv_sec, presented.tv_usec);

    if (use_input) {
        struct timeval diff;
        timersub(commit, input, &diff);
        chars += snprintf(
            &msg[chars], sizeof(msg) - chars,
            "input - %llu µs -> ", (unsigned long long)diff.tv_usec);
    }

    struct timeval diff;
    timersub(&presented, commit, &diff);
    chars += snprintf(
        &msg[chars], sizeof(msg) - chars,
        "commit - %llu µs -> ", (unsigned long long)diff.tv_usec);

    if (use_input) {
        xassert(timercmp(&presented, input, >));
        timersub(&presented, input, &diff);
    } else {
        xassert(timercmp(&presented, commit, >));
        timersub(&presented, commit, &diff);
    }

    chars += snprintf(
        &msg[chars], sizeof(msg) - chars,
        "presented (total: %llu µs)", (unsigned long long)diff.tv_usec);

    unsigned frame_count = 0;
    if (tll_length(term->window->on_outputs) > 0) {
        const struct monitor *mon = tll_front(term->window->on_outputs);
        frame_count = (diff.tv_sec * 1000000. + diff.tv_usec) / (1000000. / mon->refresh);
    }

    presentation_statistics.total++;
    if (frame_count >= 2)
        presentation_statistics.two++;
    else if (frame_count >= 1)
        presentation_statistics.one++;
    else
        presentation_statistics.zero++;

#define _log_fmt "%s (more than %u frames)"

    if (frame_count >= 2)
        LOG_ERR(_log_fmt, msg, frame_count);
    else if (frame_count >= 1)
        LOG_WARN(_log_fmt, msg, frame_count);
    else
        LOG_INFO(_log_fmt, msg, frame_count);

#undef _log_fmt

    wp_presentation_feedback_destroy(wp_presentation_feedback);
    free(ctx);
}

static void
discarded(void *data, struct wp_presentation_feedback *wp_presentation_feedback)
{
    struct presentation_context *ctx = data;
    wp_presentation_feedback_destroy(wp_presentation_feedback);
    free(ctx);
}

static const struct wp_presentation_feedback_listener presentation_feedback_listener = {
    .sync_output = &sync_output,
    .presented = &presented,
    .discarded = &discarded,
};

static struct fcft_font *
attrs_to_font(const struct terminal *term, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return term->fonts[idx];
}

static inline pixman_color_t
color_hex_to_pixman_with_alpha(uint32_t color, uint16_t alpha)
{
    return (pixman_color_t){
        .red =   ((color >> 16 & 0xff) | (color >> 8 & 0xff00)) * alpha / 0xffff,
        .green = ((color >>  8 & 0xff) | (color >> 0 & 0xff00)) * alpha / 0xffff,
        .blue =  ((color >>  0 & 0xff) | (color << 8 & 0xff00)) * alpha / 0xffff,
        .alpha = alpha,
    };
}

static inline pixman_color_t
color_hex_to_pixman(uint32_t color)
{
    /* Count on the compiler optimizing this */
    return color_hex_to_pixman_with_alpha(color, 0xffff);
}

static inline uint32_t
color_decrease_luminance(uint32_t color)
{
    uint32_t alpha = color & 0xff000000;
    int hue, sat, lum;
    rgb_to_hsl(color, &hue, &sat, &lum);
    return alpha | hsl_to_rgb(hue, sat, lum / 1.5);
}

static inline uint32_t
color_dim(const struct terminal *term, uint32_t color)
{
    const struct config *conf = term->conf;
    const uint8_t custom_dim = conf->colors.use_custom.dim;

    if (likely(custom_dim == 0))
        return color_decrease_luminance(color);

    for (size_t i = 0; i < 8; i++) {
        if (((custom_dim >> i) & 1) == 0)
            continue;

        if (term->colors.table[0 + i] == color) {
            /* “Regular” color, return the corresponding “dim” */
            return conf->colors.dim[i];
        }

        else if (term->colors.table[8 + i] == color) {
            /* “Bright” color, return the corresponding “regular” */
            return term->colors.table[i];
        }
    }

    return color_decrease_luminance(color);
}

static inline uint32_t
color_brighten(const struct terminal *term, uint32_t color)
{
    /*
     * First try to match the color against the base 8 colors. If we
     * find a match, return the corresponding bright color.
     */
    if (term->conf->bold_in_bright.palette_based) {
        for (size_t i = 0; i < 8; i++) {
            if (term->colors.table[i] == color)
                return term->colors.table[i + 8];
        }
        return color;
    }

    int hue, sat, lum;
    rgb_to_hsl(color, &hue, &sat, &lum);
    return hsl_to_rgb(hue, sat, min(100, lum * 1.3));
}

static inline int
font_baseline(const struct terminal *term)
{
    return term->font_y_ofs + term->fonts[0]->ascent;
}

static void
draw_unfocused_block(const struct terminal *term, pixman_image_t *pix,
                     const pixman_color_t *color, int x, int y, int cell_cols)
{
    const int scale = term->scale;
    const int width = min(min(scale, term->cell_width), term->cell_height);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color, 4,
        (pixman_rectangle16_t []){
         {x, y, cell_cols * term->cell_width, width},                              /* top */
         {x, y, width, term->cell_height},                                         /* left */
         {x + cell_cols * term->cell_width - width, y, width, term->cell_height},  /* right */
         {x, y + term->cell_height - width, cell_cols * term->cell_width, width},  /* bottom */
        });
}

static void
draw_beam_cursor(const struct terminal *term, pixman_image_t *pix,
                 const struct fcft_font *font,
                 const pixman_color_t *color, int x, int y)
{
    int baseline = y + font_baseline(term) - term->fonts[0]->ascent;
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, baseline,
            term_pt_or_px_as_pixels(term, &term->conf->cursor.beam_thickness),
            term->fonts[0]->ascent + term->fonts[0]->descent});
}

static int
underline_offset(const struct terminal *term, const struct fcft_font *font)
{
    return font_baseline(term) -
        (term->conf->use_custom_underline_offset
         ? -term_pt_or_px_as_pixels(term, &term->conf->underline_offset)
         : font->underline.position);
}

static void
draw_underline_cursor(const struct terminal *term, pixman_image_t *pix,
               const struct fcft_font *font,
                      const pixman_color_t *color, int x, int y, int cols)
{
    int thickness = term->conf->cursor.underline_thickness.px >= 0
        ? term_pt_or_px_as_pixels(
            term, &term->conf->cursor.underline_thickness)
        : font->underline.thickness;

    /* Make sure the line isn't positioned below the cell */
    const int y_ofs = min(underline_offset(term, font) + thickness,
                          term->cell_height - thickness);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + y_ofs, cols * term->cell_width, thickness});
}

static void
draw_underline(const struct terminal *term, pixman_image_t *pix,
               const struct fcft_font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    const int thickness = font->underline.thickness;

    /* Make sure the line isn't positioned below the cell */
    const int y_ofs = min(underline_offset(term, font),
                          term->cell_height - thickness);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + y_ofs, cols * term->cell_width, thickness});
}

static void
draw_strikeout(const struct terminal *term, pixman_image_t *pix,
               const struct fcft_font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + font_baseline(term) - font->strikeout.position,
            cols * term->cell_width, font->strikeout.thickness});
}

static void
cursor_colors_for_cell(const struct terminal *term, const struct cell *cell,
              const pixman_color_t *fg, const pixman_color_t *bg,
              pixman_color_t *cursor_color, pixman_color_t *text_color)
{
    bool is_selected = cell->attrs.selected;

    if (term->cursor_color.cursor >> 31) {
        *cursor_color = color_hex_to_pixman(term->cursor_color.cursor);
        *text_color = color_hex_to_pixman(
            term->cursor_color.text >> 31
            ? term->cursor_color.text : term->colors.bg);

        if (cell->attrs.reverse ^ is_selected) {
            pixman_color_t swap = *cursor_color;
            *cursor_color = *text_color;
            *text_color = swap;
        }
    } else {
        *cursor_color = *fg;
        *text_color = *bg;
    }
}

static void
draw_cursor(const struct terminal *term, const struct cell *cell,
            const struct fcft_font *font, pixman_image_t *pix, pixman_color_t *fg,
            const pixman_color_t *bg, int x, int y, int cols)
{
    pixman_color_t cursor_color;
    pixman_color_t text_color;
    cursor_colors_for_cell(term, cell, fg, bg, &cursor_color, &text_color);

    switch (term->cursor_style) {
    case CURSOR_BLOCK:
        if (unlikely(!term->kbd_focus))
            draw_unfocused_block(term, pix, &cursor_color, x, y, cols);

        else if (likely(term->cursor_blink.state == CURSOR_BLINK_ON)) {
            *fg = text_color;
            pixman_image_fill_rectangles(
                PIXMAN_OP_SRC, pix, &cursor_color, 1,
                &(pixman_rectangle16_t){x, y, cols * term->cell_width, term->cell_height});
        }
        break;

    case CURSOR_BEAM:
        if (likely(term->cursor_blink.state == CURSOR_BLINK_ON ||
                   !term->kbd_focus))
        {
            draw_beam_cursor(term, pix, font, &cursor_color, x, y);
        }
        break;

    case CURSOR_UNDERLINE:
        if (likely(term->cursor_blink.state == CURSOR_BLINK_ON ||
                   !term->kbd_focus))
        {
            draw_underline_cursor(term, pix, font, &cursor_color, x, y, cols);
        }
        break;
    }
}

static int
render_cell(struct terminal *term, pixman_image_t *pix,
            struct row *row, int col, int row_no, bool has_cursor)
{
    struct cell *cell = &row->cells[col];
    if (cell->attrs.clean)
        return 0;

    cell->attrs.clean = 1;
    cell->attrs.confined = true;

    int width = term->cell_width;
    int height = term->cell_height;
    const int x = term->margins.left + col * width;
    const int y = term->margins.top + row_no * height;

    bool is_selected = cell->attrs.selected;

    uint32_t _fg = 0;
    uint32_t _bg = 0;

    uint16_t alpha = 0xffff;

    if (is_selected && term->colors.use_custom_selection) {
        _fg = term->colors.selection_fg;
        _bg = term->colors.selection_bg;
    } else {
        /* Use cell specific color, if set, otherwise the default colors (possible reversed) */
        switch (cell->attrs.fg_src) {
        case COLOR_RGB:
            _fg = cell->attrs.fg;
            break;

        case COLOR_BASE16:
        case COLOR_BASE256:
            xassert(cell->attrs.fg < ALEN(term->colors.table));
            _fg = term->colors.table[cell->attrs.fg];
            break;

        case COLOR_DEFAULT:
            _fg = term->reverse ? term->colors.bg : term->colors.fg;
            break;
        }

        switch (cell->attrs.bg_src) {
        case COLOR_RGB:
            _bg = cell->attrs.bg;
            break;

        case COLOR_BASE16:
        case COLOR_BASE256:
            xassert(cell->attrs.bg < ALEN(term->colors.table));
            _bg = term->colors.table[cell->attrs.bg];
            break;

        case COLOR_DEFAULT:
            _bg = term->reverse ? term->colors.fg : term->colors.bg;
            break;
        }

        if (cell->attrs.reverse ^ is_selected) {
            uint32_t swap = _fg;
            _fg = _bg;
            _bg = swap;
        } else if (cell->attrs.bg_src == COLOR_DEFAULT)
            alpha = term->colors.alpha;
    }

    if (unlikely(is_selected && _fg == _bg)) {
        /* Invert bg when selected/highlighted text has same fg/bg */
        _bg = ~_bg;
        alpha = 0xffff;
    }

    if (cell->attrs.dim)
        _fg = color_dim(term, _fg);
    if (term->conf->bold_in_bright.enabled && cell->attrs.bold)
        _fg = color_brighten(term, _fg);

    if (cell->attrs.blink && term->blink.state == BLINK_OFF)
        _fg = color_decrease_luminance(_fg);

    pixman_color_t fg = color_hex_to_pixman(_fg);
    pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, alpha);

    struct fcft_font *font = attrs_to_font(term, &cell->attrs);
    const struct composed *composed = NULL;
    const struct fcft_grapheme *grapheme = NULL;
    const struct fcft_glyph *single = NULL;
    const struct fcft_glyph **glyphs = NULL;
    unsigned glyph_count = 0;

    char32_t base = cell->wc;
    int cell_cols = 1;

    if (base != 0) {
        if (unlikely(
                /* Classic box drawings */
                (base >= GLYPH_BOX_DRAWING_FIRST &&
                 base <= GLYPH_BOX_DRAWING_LAST) ||

                /* Braille */
                (base >= GLYPH_BRAILLE_FIRST &&
                 base <= GLYPH_BRAILLE_LAST) ||

                /*
                 * Unicode 13 "Symbols for Legacy Computing"
                 * sub-ranges below.
                 *
                 * Note, the full range is U+1FB00 - U+1FBF9
                 */
                (base >= GLYPH_LEGACY_FIRST &&
                 base <= GLYPH_LEGACY_LAST)) &&

            likely(!term->conf->box_drawings_uses_font_glyphs))
        {
            struct fcft_glyph ***arr;
            size_t count;
            size_t idx;

            if (base >= GLYPH_LEGACY_FIRST) {
                arr = &term->custom_glyphs.legacy;
                count = GLYPH_LEGACY_COUNT;
                idx = base - GLYPH_LEGACY_FIRST;
            } else if (base >= GLYPH_BRAILLE_FIRST) {
                arr = &term->custom_glyphs.braille;
                count = GLYPH_BRAILLE_COUNT;
                idx = base - GLYPH_BRAILLE_FIRST;
            } else {
                arr = &term->custom_glyphs.box_drawing;
                count = GLYPH_BOX_DRAWING_COUNT;
                idx = base - GLYPH_BOX_DRAWING_FIRST;
            }

            if (unlikely(*arr == NULL))
                *arr = xcalloc(count, sizeof((*arr)[0]));

            if (likely((*arr)[idx] != NULL))
                single = (*arr)[idx];
            else {
                mtx_lock(&term->render.workers.lock);

                /* Other thread may have instantiated it while we
                 * acquired the lock */
                single = (*arr)[idx];
                if (likely(single == NULL))
                    single = (*arr)[idx] = box_drawing(term, base);
                mtx_unlock(&term->render.workers.lock);
            }

            if (single != NULL) {
                glyph_count = 1;
                glyphs = &single;
                cell_cols = single->cols;
            }
        }

        else if (base >= CELL_COMB_CHARS_LO && base <= CELL_COMB_CHARS_HI)
        {
            composed = composed_lookup(term->composed, base - CELL_COMB_CHARS_LO);
            base = composed->chars[0];

            if (term->conf->can_shape_grapheme && term->conf->tweak.grapheme_shaping) {
                grapheme = fcft_rasterize_grapheme_utf32(
                    font, composed->count, composed->chars, term->font_subpixel);
            }

            if (grapheme != NULL) {
                cell_cols = composed->width;

                composed = NULL;
                glyphs = grapheme->glyphs;
                glyph_count = grapheme->count;
            }
        }


        if (single == NULL && grapheme == NULL) {
            xassert(base != 0);
            single = fcft_rasterize_char_utf32(font, base, term->font_subpixel);
            if (single == NULL) {
                glyph_count = 0;
                cell_cols = 1;
            } else {
                glyph_count = 1;
                glyphs = &single;
                cell_cols = single->cols;
            }
        }
    }

    assert(glyph_count == 0 || glyphs != NULL);

    const int cols_left = term->cols - col;
    cell_cols = max(1, min(cell_cols, cols_left));

    /*
     * Determine cells that will bleed into their right neighbor and remember
     * them for cleanup in the next frame.
     */
    int render_width = cell_cols * width;
    if (term->conf->tweak.overflowing_glyphs &&
        glyph_count > 0 &&
        cols_left > cell_cols)
    {
        int glyph_width = 0, advance = 0;
        for (size_t i = 0; i < glyph_count; i++) {
            glyph_width = max(glyph_width,
                              advance + glyphs[i]->x + glyphs[i]->width);
            advance += glyphs[i]->advance.x;
        }

        if (glyph_width > render_width) {
            render_width = min(glyph_width, render_width + width);

            for (int i = 0; i < cell_cols; i++)
                row->cells[col + i].attrs.confined = false;
        }
    }

    pixman_region32_t clip;
    pixman_region32_init_rect(
        &clip, x, y,
        render_width, term->cell_height);
    pixman_image_set_clip_region32(pix, &clip);
    pixman_region32_fini(&clip);

    /* Background */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, &bg, 1,
        &(pixman_rectangle16_t){x, y, cell_cols * width, height});

    if (cell->attrs.blink && term->blink.fd < 0) {
        /* TODO: use a custom lock for this? */
        mtx_lock(&term->render.workers.lock);
        term_arm_blink_timer(term);
        mtx_unlock(&term->render.workers.lock);
    }

    if (has_cursor && term->cursor_style == CURSOR_BLOCK && term->kbd_focus)
        draw_cursor(term, cell, font, pix, &fg, &bg, x, y, cell_cols);

    if (cell->wc == 0 || cell->wc >= CELL_SPACER || cell->wc == U'\t' ||
        (unlikely(cell->attrs.conceal) && !is_selected))
    {
        goto draw_cursor;
    }

    pixman_image_t *clr_pix = pixman_image_create_solid_fill(&fg);

    int pen_x = x;
    for (unsigned i = 0; i < glyph_count; i++) {
        const int letter_x_ofs = i == 0 ? term->font_x_ofs : 0;

        const struct fcft_glyph *glyph = glyphs[i];
        if (glyph == NULL)
            continue;

        int g_x = glyph->x;
        int g_y = glyph->y;

        if (i > 0 && glyph->x >= 0)
            g_x -= term->cell_width;

        if (unlikely(pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)) {
            /* Glyph surface is a pre-rendered image (typically a color emoji...) */
            if (!(cell->attrs.blink && term->blink.state == BLINK_OFF)) {
                pixman_image_composite32(
                    PIXMAN_OP_OVER, glyph->pix, NULL, pix, 0, 0, 0, 0,
                    pen_x + letter_x_ofs + g_x, y + font_baseline(term) - g_y,
                    glyph->width, glyph->height);
            }
        } else {
            pixman_image_composite32(
                PIXMAN_OP_OVER, clr_pix, glyph->pix, pix, 0, 0, 0, 0,
                pen_x + letter_x_ofs + g_x, y + font_baseline(term) - g_y,
                glyph->width, glyph->height);

            /* Combining characters */
            if (composed != NULL) {
                assert(glyph_count == 1);

                for (size_t i = 1; i < composed->count; i++) {
                    const struct fcft_glyph *g = fcft_rasterize_char_utf32(
                        font, composed->chars[i], term->font_subpixel);

                    if (g == NULL)
                        continue;

                    /*
                     * Fonts _should_ assume the pen position is now
                     * *after* the base glyph, and thus use negative
                     * offsets for combining glyphs.
                     *
                     * Not all fonts behave like this however, and we
                     * try to accommodate both variants.
                     *
                     * Since we haven't moved our pen position yet, we
                     * add a full cell width to the offset (or two, in
                     * case of double-width characters).
                     *
                     * If the font does *not* use negative offsets,
                     * we'd normally use an offset of 0. However, to
                     * somewhat deal with double-width glyphs we use
                     * an offset of *one* cell.
                     */
                    int x_ofs = g->x < 0
                        ? cell_cols * term->cell_width
                        : (cell_cols - 1) * term->cell_width;

                    pixman_image_composite32(
                        PIXMAN_OP_OVER, clr_pix, g->pix, pix, 0, 0, 0, 0,
                        /* Some fonts use a negative offset, while others use a
                         * "normal" offset */
                        pen_x + x_ofs + g->x,
                        y + font_baseline(term) - g->y,
                        g->width, g->height);
                }
            }
        }

        pen_x += glyph->advance.x;
    }

    pixman_image_unref(clr_pix);

    /* Underline */
    if (cell->attrs.underline)
        draw_underline(term, pix, font, &fg, x, y, cell_cols);

    if (cell->attrs.strikethrough)
        draw_strikeout(term, pix, font, &fg, x, y, cell_cols);

    if (unlikely(cell->attrs.url)) {
        pixman_color_t url_color = color_hex_to_pixman(
            term->conf->colors.use_custom.url
            ? term->conf->colors.url
            : term->colors.table[3]
            );
        draw_underline(term, pix, font, &url_color, x, y, cell_cols);
    }

draw_cursor:
    if (has_cursor && (term->cursor_style != CURSOR_BLOCK || !term->kbd_focus))
        draw_cursor(term, cell, font, pix, &fg, &bg, x, y, cell_cols);

    pixman_image_set_clip_region32(pix, NULL);
    return cell_cols;
}

static void
render_row(struct terminal *term, pixman_image_t *pix, struct row *row,
           int row_no, int cursor_col)
{
    for (int col = term->cols - 1; col >= 0; col--)
        render_cell(term, pix, row, col, row_no, cursor_col == col);
}

static void
render_urgency(struct terminal *term, struct buffer *buf)
{
    uint32_t red = term->colors.table[1];
    pixman_color_t bg = color_hex_to_pixman(red);

    int width = min(min(term->margins.left, term->margins.right),
                    min(term->margins.top, term->margins.bottom));

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &bg, 4,
        (pixman_rectangle16_t[]){
            /* Top */
            {0, 0, term->width, width},

            /* Bottom */
            {0, term->height - width, term->width, width},

            /* Left */
            {0, width, width, term->height - 2 * width},

            /* Right */
            {term->width - width, width, width, term->height - 2 * width},
        });
}

static void
render_margin(struct terminal *term, struct buffer *buf,
              int start_line, int end_line, bool apply_damage)
{
    /* Fill area outside the cell grid with the default background color */
    const int rmargin = term->width - term->margins.right;
    const int bmargin = term->height - term->margins.bottom;
    const int line_count = end_line - start_line;

    uint32_t _bg = !term->reverse ? term->colors.bg : term->colors.fg;
    pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, term->colors.alpha);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &bg, 4,
        (pixman_rectangle16_t[]){
            /* Top */
            {0, 0, term->width, term->margins.top},

            /* Bottom */
            {0, bmargin, term->width, term->margins.bottom},

            /* Left */
            {0, term->margins.top + start_line * term->cell_height,
             term->margins.left, line_count * term->cell_height},

            /* Right */
            {rmargin, term->margins.top + start_line * term->cell_height,
             term->margins.right, line_count * term->cell_height},
    });

    if (term->render.urgency)
        render_urgency(term, buf);

    /* Ensure the updated regions are copied to the next frame's
     * buffer when we're double buffering */
    pixman_region32_union_rect(
        &buf->dirty, &buf->dirty, 0, 0, term->width, term->margins.top);
    pixman_region32_union_rect(
        &buf->dirty, &buf->dirty, 0, bmargin, term->width, term->margins.bottom);
    pixman_region32_union_rect(
        &buf->dirty, &buf->dirty, 0, 0, term->margins.left, term->height);
    pixman_region32_union_rect(
        &buf->dirty, &buf->dirty,
        rmargin, 0, term->margins.right, term->height);

    if (apply_damage) {
        /* Top */
        wl_surface_damage_buffer(
            term->window->surface, 0, 0, term->width, term->margins.top);

        /* Bottom */
        wl_surface_damage_buffer(
            term->window->surface, 0, bmargin, term->width, term->margins.bottom);

        /* Left */
        wl_surface_damage_buffer(
            term->window->surface,
            0, term->margins.top + start_line * term->cell_height,
            term->margins.left, line_count * term->cell_height);

        /* Right */
        wl_surface_damage_buffer(
            term->window->surface,
            rmargin, term->margins.top + start_line * term->cell_height,
            term->margins.right, line_count * term->cell_height);
    }
}

static void
grid_render_scroll(struct terminal *term, struct buffer *buf,
                   const struct damage *dmg)
{
    int height = (dmg->region.end - dmg->region.start - dmg->lines) * term->cell_height;

    LOG_DBG(
        "damage: SCROLL: %d-%d by %d lines",
        dmg->region.start, dmg->region.end, dmg->lines);

    if (height <= 0)
        return;

#if TIME_SCROLL_DAMAGE
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif

    int dst_y = term->margins.top + (dmg->region.start + 0) * term->cell_height;
    int src_y = term->margins.top + (dmg->region.start + dmg->lines) * term->cell_height;

    /*
     * SHM scrolling can be *much* faster, but it depends on how many
     * lines we're scrolling, and how much repairing we need to do.
     *
     * In short, scrolling a *large* number of rows is faster with a
     * memmove, while scrolling a *small* number of lines is faster
     * with SHM scrolling.
     *
     * However, since we need to restore the scrolling regions when
     * SHM scrolling, we also need to take this into account.
     *
     * Finally, we also have to restore the window margins, and this
     * is a *huge* performance hit when scrolling a large number of
     * lines (in addition to the sloweness of SHM scrolling as
     * method).
     *
     * So, we need to figure out when to SHM scroll, and when to
     * memmove.
     *
     * For now, assume that the both methods perform roughly the same,
     * given an equal number of bytes to move/allocate, and use the
     * method that results in the least amount of bytes to touch.
     *
     * Since number of lines directly translates to bytes, we can
     * simply count lines.
     *
     * SHM scrolling needs to first "move" (punch hole + allocate)
     * dmg->lines number of lines, and then we need to restore
     * the bottom scroll region.
     *
     * If the total number of lines is less than half the screen - use
     * SHM. Otherwise use memmove.
     */
    bool try_shm_scroll =
        shm_can_scroll(buf) && (
            dmg->lines +
            dmg->region.start +
            (term->rows - dmg->region.end)) < term->rows / 2;

    bool did_shm_scroll = false;

    //try_shm_scroll = false;
    //try_shm_scroll = true;

    if (try_shm_scroll) {
        did_shm_scroll = shm_scroll(
            buf, dmg->lines * term->cell_height,
            term->margins.top, dmg->region.start * term->cell_height,
            term->margins.bottom, (term->rows - dmg->region.end) * term->cell_height);
    }

    if (did_shm_scroll) {
        /* Restore margins */
        render_margin(
            term, buf, dmg->region.end - dmg->lines, term->rows, false);
    } else {
        /* Fallback for when we either cannot do SHM scrolling, or it failed */
        uint8_t *raw = buf->data;
        memmove(raw + dst_y * buf->stride,
                raw + src_y * buf->stride,
                height * buf->stride);
    }

#if TIME_SCROLL_DAMAGE
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    struct timespec memmove_time;
    timespec_sub(&end_time, &start_time, &memmove_time);
    LOG_INFO("scrolled %dKB (%d lines) using %s in %lds %ldns",
             height * buf->stride / 1024, dmg->lines,
             did_shm_scroll ? "SHM" : try_shm_scroll ? "memmove (SHM failed)" :  "memmove",
             (long)memmove_time.tv_sec, memmove_time.tv_nsec);
#endif

    wl_surface_damage_buffer(
        term->window->surface, term->margins.left, dst_y,
        term->width - term->margins.left - term->margins.right, height);
}

static void
grid_render_scroll_reverse(struct terminal *term, struct buffer *buf,
                           const struct damage *dmg)
{
    int height = (dmg->region.end - dmg->region.start - dmg->lines) * term->cell_height;

    LOG_DBG(
        "damage: SCROLL REVERSE: %d-%d by %d lines",
        dmg->region.start, dmg->region.end, dmg->lines);

    if (height <= 0)
        return;

#if TIME_SCROLL_DAMAGE
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif

    int src_y = term->margins.top + (dmg->region.start + 0) * term->cell_height;
    int dst_y = term->margins.top + (dmg->region.start + dmg->lines) * term->cell_height;

    bool try_shm_scroll =
        shm_can_scroll(buf) && (
            dmg->lines +
            dmg->region.start +
            (term->rows - dmg->region.end)) < term->rows / 2;

    bool did_shm_scroll = false;

    if (try_shm_scroll) {
        did_shm_scroll = shm_scroll(
            buf, -dmg->lines * term->cell_height,
            term->margins.top, dmg->region.start * term->cell_height,
            term->margins.bottom, (term->rows - dmg->region.end) * term->cell_height);
    }

    if (did_shm_scroll) {
        /* Restore margins */
        render_margin(
            term, buf, dmg->region.start, dmg->region.start + dmg->lines, false);
    } else {
        /* Fallback for when we either cannot do SHM scrolling, or it failed */
        uint8_t *raw = buf->data;
        memmove(raw + dst_y * buf->stride,
                raw + src_y * buf->stride,
                height * buf->stride);
    }

#if TIME_SCROLL_DAMAGE
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    struct timespec memmove_time;
    timespec_sub(&end_time, &start_time, &memmove_time);
    LOG_INFO("scrolled REVERSE %dKB (%d lines) using %s in %lds %ldns",
             height * buf->stride / 1024, dmg->lines,
             did_shm_scroll ? "SHM" : try_shm_scroll ? "memmove (SHM failed)" :  "memmove",
             (long)memmove_time.tv_sec, memmove_time.tv_nsec);
#endif

    wl_surface_damage_buffer(
        term->window->surface, term->margins.left, dst_y,
        term->width - term->margins.left - term->margins.right, height);
}

static void
render_sixel_chunk(struct terminal *term, pixman_image_t *pix, const struct sixel *sixel,
                   int term_start_row, int img_start_row, int count)
{
    /* Translate row/column to x/y pixel values */
    const int x = term->margins.left + sixel->pos.col * term->cell_width;
    const int y = term->margins.top + term_start_row * term->cell_height;

    /* Width/height, in pixels - and don't touch the window margins */
    const int width = max(
        0,
        min(sixel->width,
            term->width - x - term->margins.right));
    const int height = max(
        0,
        min(
            min(count * term->cell_height,                          /* 'count' number of rows */
                sixel->height - img_start_row * term->cell_height), /* What remains of the sixel */
            term->height - y - term->margins.bottom));

    /* Verify we're not stepping outside the grid */
    xassert(x >= term->margins.left);
    xassert(y >= term->margins.top);
    xassert(width == 0 || x + width <= term->width - term->margins.right);
    xassert(height == 0 || y + height <= term->height - term->margins.bottom);

    //LOG_DBG("sixel chunk: %dx%d %dx%d", x, y, width, height);

    pixman_image_composite32(
        sixel->opaque ? PIXMAN_OP_SRC : PIXMAN_OP_OVER,
        sixel->pix,
        NULL,
        pix,
        0, img_start_row * term->cell_height,
        0, 0,
        x, y,
        width, height);

    wl_surface_damage_buffer(term->window->surface, x, y, width, height);
}

static void
render_sixel(struct terminal *term, pixman_image_t *pix,
             const struct coord *cursor, const struct sixel *sixel)
{
    const int view_end = (term->grid->view + term->rows - 1) & (term->grid->num_rows - 1);
    const bool last_row_needs_erase = sixel->height % term->cell_height != 0;
    const bool last_col_needs_erase = sixel->width % term->cell_width != 0;

    int chunk_img_start = -1;  /* Image-relative start row of chunk */
    int chunk_term_start = -1; /* Viewport relative start row of chunk */
    int chunk_row_count = 0;   /* Number of rows to emit */

#define maybe_emit_sixel_chunk_then_reset()                             \
    if (chunk_row_count != 0) {                                         \
        render_sixel_chunk(                                             \
            term, pix, sixel,                                           \
            chunk_term_start, chunk_img_start, chunk_row_count);        \
        chunk_term_start = chunk_img_start = -1;                        \
        chunk_row_count = 0;                                            \
    }

    /*
     * Iterate all sixel rows:
     *
     *  - ignore rows that aren't visible on-screen
     *  - ignore rows that aren't dirty (they have already been rendered)
     *  - chunk consecutive dirty rows into a 'chunk'
     *  - emit (render) chunk as soon as a row isn't visible, or is clean
     *  - emit final chunk after we've iterated all rows
     *
     * The purpose of this is to reduce the amount of pixels that
     * needs to be composited and marked as damaged for the
     * compositor.
     *
     * Since we do CPU based composition, rendering is a slow and
     * heavy task for foot, and thus it is important to not re-render
     * things unnecessarily.
     */

    for (int _abs_row_no = sixel->pos.row;
         _abs_row_no < sixel->pos.row + sixel->rows;
         _abs_row_no++)
    {
        const int abs_row_no = _abs_row_no & (term->grid->num_rows - 1);
        const int term_row_no =
            (abs_row_no - term->grid->view + term->grid->num_rows) &
            (term->grid->num_rows - 1);

        /* Check if row is in the visible viewport */
        if (view_end >= term->grid->view) {
            /* Not wrapped */
            if (!(abs_row_no >= term->grid->view && abs_row_no <= view_end)) {
                /* Not visible */
                maybe_emit_sixel_chunk_then_reset();
                continue;
            }
        } else {
            /* Wrapped */
            if (!(abs_row_no >= term->grid->view || abs_row_no <= view_end)) {
                /* Not visible */
                maybe_emit_sixel_chunk_then_reset();
                continue;
            }
        }

        /* Is the row dirty? */
        struct row *row = term->grid->rows[abs_row_no];
        xassert(row != NULL);  /* Should be visible */

        if (!row->dirty) {
            maybe_emit_sixel_chunk_then_reset();
            continue;
        }

        int cursor_col = cursor->row == term_row_no ? cursor->col : -1;

        /*
         * If image contains transparent parts, render all (dirty)
         * cells beneath it.
         *
         * If image is opaque, loop cells and set their 'clean' bit,
         * to prevent the grid rendered from overwriting the sixel
         *
         * If the last sixel row only partially covers the cell row,
         * 'erase' the cell by rendering them.
         *
         * In all cases, do *not* clear the ‘dirty’ bit on the row, to
         * ensure the regular renderer includes them in the damage
         * rect.
         */
        if (!sixel->opaque) {
            /* TODO: multithreading */
            int cursor_col = cursor->row == term_row_no ? cursor->col : -1;
            render_row(term, pix, row, term_row_no, cursor_col);
        } else {
            for (int col = sixel->pos.col;
                 col < min(sixel->pos.col + sixel->cols, term->cols);
                 col++)
            {
                struct cell *cell = &row->cells[col];

                if (!cell->attrs.clean) {
                    bool last_row = abs_row_no == sixel->pos.row + sixel->rows - 1;
                    bool last_col = col == sixel->pos.col + sixel->cols - 1;

                    if ((last_row_needs_erase && last_row) ||
                        (last_col_needs_erase && last_col))
                    {
                        render_cell(term, pix, row, col, term_row_no, cursor_col == col);
                    } else {
                        cell->attrs.clean = 1;
                        cell->attrs.confined = 1;
                    }
                }
            }
        }

        if (chunk_term_start == -1) {
            xassert(chunk_img_start == -1);
            chunk_term_start = term_row_no;
            chunk_img_start = _abs_row_no - sixel->pos.row;
            chunk_row_count = 1;
        } else
            chunk_row_count++;
    }

    maybe_emit_sixel_chunk_then_reset();
#undef maybe_emit_sixel_chunk_then_reset
}

static void
render_sixel_images(struct terminal *term, pixman_image_t *pix,
                    const struct coord *cursor)
{
    if (likely(tll_length(term->grid->sixel_images)) == 0)
        return;

    const int scrollback_end
        = (term->grid->offset + term->rows) & (term->grid->num_rows - 1);

    const int view_start
        = (term->grid->view
           - scrollback_end
           + term->grid->num_rows) & (term->grid->num_rows - 1);

    const int view_end = view_start + term->rows - 1;

    //LOG_DBG("SIXELS: %zu images, view=%d-%d",
    //        tll_length(term->grid->sixel_images), view_start, view_end);

    tll_foreach(term->grid->sixel_images, it) {
        const struct sixel *six = &it->item;
        const int start
            = (six->pos.row
               - scrollback_end
               + term->grid->num_rows) & (term->grid->num_rows - 1);
        const int end = start + six->rows - 1;

        //LOG_DBG("  sixel: %d-%d", start, end);
        if (start > view_end) {
            /* Sixel starts after view ends, no need to try to render it */
            continue;
        } else if (end < view_start) {
            /* Image ends before view starts. Since the image list is
             * sorted, we can safely stop here */
            break;
        }

        render_sixel(term, pix, cursor, &it->item);
    }
}

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
static void
render_ime_preedit_for_seat(struct terminal *term, struct seat *seat,
                            struct buffer *buf)
{
    if (likely(seat->ime.preedit.cells == NULL))
        return;

    if (unlikely(term->is_searching))
        return;

    /* Adjust cursor position to viewport */
    struct coord cursor;
    cursor = term->grid->cursor.point;
    cursor.row += term->grid->offset;
    cursor.row -= term->grid->view;
    cursor.row &= term->grid->num_rows - 1;

    if (cursor.row < 0 || cursor.row >= term->rows)
        return;

    int cells_needed = seat->ime.preedit.count;

    if (seat->ime.preedit.cursor.start == cells_needed &&
        seat->ime.preedit.cursor.end == cells_needed)
    {
        /* Cursor will be drawn *after* the pre-edit string, i.e. in
         * the cell *after*. This means we need to copy, and dirty,
         * one extra cell from the original grid, or we’ll leave
         * trailing “cursors” after us if the user deletes text while
         * pre-editing */
        cells_needed++;
    }

    int row_idx = cursor.row;
    int col_idx = cursor.col;
    int ime_ofs = 0;  /* Offset into pre-edit string to start rendering at */

    int cells_left = term->cols - cursor.col;
    int cells_used = min(cells_needed, term->cols);

    /* Adjust start of pre-edit text to the left if string doesn't fit on row */
    if (cells_left < cells_used)
        col_idx -= cells_used - cells_left;

    if (cells_needed > cells_used) {
        int start = seat->ime.preedit.cursor.start;
        int end = seat->ime.preedit.cursor.end;

        if (start == end) {
            /* Ensure *end* of pre-edit string is visible */
            ime_ofs = cells_needed - cells_used;
        } else {
            /* Ensure the *beginning* of the cursor-area is visible */
            ime_ofs = start;

            /* Display as much as possible of the pre-edit string */
            if (cells_needed - ime_ofs < cells_used)
                ime_ofs = cells_needed - cells_used;
        }

        /* Make sure we don't start in the middle of a character */
        while (ime_ofs < cells_needed &&
               seat->ime.preedit.cells[ime_ofs].wc >= CELL_SPACER)
        {
            ime_ofs++;
        }
    }

    xassert(col_idx >= 0);
    xassert(col_idx < term->cols);

    struct row *row = grid_row_in_view(term->grid, row_idx);

    /* Don't start pre-edit text in the middle of a double-width character */
    while (col_idx > 0 && row->cells[col_idx].wc >= CELL_SPACER) {
        cells_used++;
        col_idx--;
    }

    /*
     * Copy original content (render_cell() reads cell data directly
     * from grid), and mark all cells as dirty. This ensures they are
     * re-rendered when the pre-edit text is modified or removed.
     */
    struct cell *real_cells = malloc(cells_used * sizeof(real_cells[0]));
    for (int i = 0; i < cells_used; i++) {
        xassert(col_idx + i < term->cols);
        real_cells[i] = row->cells[col_idx + i];
        real_cells[i].attrs.clean = 0;
    }
    row->dirty = true;

    /* Render pre-edit text */
    xassert(seat->ime.preedit.cells[ime_ofs].wc < CELL_SPACER);
    for (int i = 0, idx = ime_ofs; idx < seat->ime.preedit.count; i++, idx++) {
        const struct cell *cell = &seat->ime.preedit.cells[idx];

        if (cell->wc >= CELL_SPACER)
            continue;

        int width = max(1, c32width(cell->wc));
        if (col_idx + i + width > term->cols)
            break;

        row->cells[col_idx + i] = *cell;
        render_cell(term, buf->pix[0], row, col_idx + i, row_idx, false);
    }

    int start = seat->ime.preedit.cursor.start - ime_ofs;
    int end = seat->ime.preedit.cursor.end - ime_ofs;

    if (!seat->ime.preedit.cursor.hidden) {
        const struct cell *start_cell = &seat->ime.preedit.cells[0];

        pixman_color_t fg = color_hex_to_pixman(term->colors.fg);
        pixman_color_t bg = color_hex_to_pixman(term->colors.bg);

        pixman_color_t cursor_color, text_color;
        cursor_colors_for_cell(
            term, start_cell, &fg, &bg, &cursor_color, &text_color);

        int x = term->margins.left + (col_idx + start) * term->cell_width;
        int y = term->margins.top + row_idx * term->cell_height;

        if (end == start) {
            /* Bar */
            if (start >= 0) {
                struct fcft_font *font = attrs_to_font(term, &start_cell->attrs);
                draw_beam_cursor(term, buf->pix[0], font, &cursor_color, x, y);
            }
            term_ime_set_cursor_rect(term, x, y, 1, term->cell_height);
        }

        else if (end > start) {
            /* Hollow cursor */
            if (start >= 0 && end <= term->cols) {
                int cols = end - start;
                draw_unfocused_block(term, buf->pix[0], &cursor_color, x, y, cols);
            }

            term_ime_set_cursor_rect(
                term, x, y, (end - start) * term->cell_width, term->cell_height);
        }
    }

    /* Restore original content (but do not render) */
    for (int i = 0; i < cells_used; i++)
        row->cells[col_idx + i] = real_cells[i];
    free(real_cells);

    wl_surface_damage_buffer(
        term->window->surface,
        term->margins.left,
        term->margins.top + row_idx * term->cell_height,
        term->width - term->margins.left - term->margins.right,
        1 * term->cell_height);
}
#endif

static void
render_ime_preedit(struct terminal *term, struct buffer *buf)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term)
            render_ime_preedit_for_seat(term, &it->item, buf);
    }
#endif
}

static void
render_overlay(struct terminal *term)
{
    struct wl_surf_subsurf *overlay = &term->window->overlay;

    const enum overlay_style style =
        term->is_searching ? OVERLAY_SEARCH :
        term->flash.active ? OVERLAY_FLASH :
        OVERLAY_NONE;

    if (likely(style == OVERLAY_NONE)) {
        if (term->render.last_overlay_style != OVERLAY_NONE) {
            /* Unmap overlay sub-surface */
            wl_surface_attach(overlay->surf, NULL, 0, 0);
            wl_surface_commit(overlay->surf);
            term->render.last_overlay_style = OVERLAY_NONE;
            term->render.last_overlay_buf = NULL;
        }
        return;
    }

    struct buffer *buf = shm_get_buffer(
        term->render.chains.overlay, term->width, term->height);

    pixman_image_set_clip_region32(buf->pix[0], NULL);

    pixman_color_t color = style == OVERLAY_SEARCH
        ? (pixman_color_t){0, 0, 0, 0x7fff}
        : (pixman_color_t){.red=0x7fff, .green=0x7fff, .blue=0, .alpha=0x7fff};

    /* Bounding rectangle of damaged areas - for wl_surface_damage_buffer() */
    pixman_box32_t damage_bounds;

    if (style == OVERLAY_SEARCH) {
        /*
         * When possible, we only update the areas that have *changed*
         * since the last frame. That means:
         *
         *  - clearing/erasing cells that are now selected, but weren’t
         *    in the last frame
         *  - dimming cells that were selected, but aren’t anymore
         *
         * To do this, we save the last frame’s selected cells as a
         * pixman region.
         *
         * Then, we calculate the corresponding region for this
         * frame’s selected cells.
         *
         * Last frame’s region minus this frame’s region gives us the
         * region that needs to be *dimmed* in this frame
         *
         * This frame’s region minus last frame’s region gives us the
         * region that needs to be *cleared* in this frame.
         *
         * Finally, the union of the two “diff” regions above, gives
         * us the total region affecte by a change, in either way. We
         * use this as the bounding box for the
         * wl_surface_damage_buffer() call.
         */
        pixman_region32_t *see_through = &term->render.last_overlay_clip;
        pixman_region32_t old_see_through;

        if (!(buf == term->render.last_overlay_buf &&
              style == term->render.last_overlay_style &&
              buf->age == 0))
        {
            /* Can’t re-use last frame’s damage - set to full window,
             * to ensure *everything* is updated */
            pixman_region32_init_rect(
                &old_see_through, 0, 0, buf->width, buf->height);
        } else {
            /* Use last frame’s saved region */
            pixman_region32_init(&old_see_through);
            pixman_region32_copy(&old_see_through, see_through);
        }

        pixman_region32_clear(see_through);

        struct search_match_iterator iter = search_matches_new_iter(term);

        for (struct range match = search_matches_next(&iter);
             match.start.row >= 0;
             match = search_matches_next(&iter))
        {
            int r = match.start.row;
            int start_col = match.start.col;
            const int end_row = match.end.row;

            while (true) {
                const int end_col =
                    r == end_row ? match.end.col : term->cols - 1;

                int x = term->margins.left + start_col * term->cell_width;
                int y = term->margins.top + r * term->cell_height;
                int width = (end_col + 1 - start_col) * term->cell_width;
                int height = 1 * term->cell_height;

                pixman_region32_union_rect(
                    see_through, see_through, x, y, width, height);

                if (++r > end_row)
                    break;

                start_col = 0;
            }
        }

        /* Current see-through, minus old see-through - aka cells that
         * need to be cleared */
        pixman_region32_t new_see_through;
        pixman_region32_init(&new_see_through);
        pixman_region32_subtract(&new_see_through, see_through, &old_see_through);
        pixman_image_set_clip_region32(buf->pix[0], &new_see_through);

        /* Old see-through, minus new see-through - aka cells that
         * needs to be dimmed */
        pixman_region32_t new_dimmed;
        pixman_region32_init(&new_dimmed);
        pixman_region32_subtract(&new_dimmed, &old_see_through, see_through);
        pixman_region32_fini(&old_see_through);

        pixman_region32_t damage;
        pixman_region32_init(&damage);
        pixman_region32_union(&damage, &new_see_through, &new_dimmed);
        damage_bounds = damage.extents;

        /* Clear cells that became selected in this frame. */
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, buf->pix[0], &(pixman_color_t){0}, 1,
            &(pixman_rectangle16_t){0, 0, term->width, term->height});

        /* Set clip region for the newly dimmed cells. The actual
         * paint call is done below */
        pixman_image_set_clip_region32(buf->pix[0], &new_dimmed);

        pixman_region32_fini(&new_see_through);
        pixman_region32_fini(&new_dimmed);
        pixman_region32_fini(&damage);
    }

    else if (buf == term->render.last_overlay_buf &&
             style == term->render.last_overlay_style)
    {
        xassert(style == OVERLAY_FLASH);
        shm_did_not_use_buf(buf);
        return;
    } else {
        pixman_image_set_clip_region32(buf->pix[0], NULL);
        damage_bounds = (pixman_box32_t){0, 0, buf->width, buf->height};
    }

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &color, 1,
        &(pixman_rectangle16_t){0, 0, term->width, term->height});

    quirk_weston_subsurface_desync_on(overlay->sub);
    wl_subsurface_set_position(overlay->sub, 0, 0);
    wl_surface_set_buffer_scale(overlay->surf, term->scale);
    wl_surface_attach(overlay->surf, buf->wl_buf, 0, 0);

    wl_surface_damage_buffer(
        overlay->surf,
        damage_bounds.x1, damage_bounds.y1,
        damage_bounds.x2 - damage_bounds.x1,
        damage_bounds.y2 - damage_bounds.y1);

    wl_surface_commit(overlay->surf);
    quirk_weston_subsurface_desync_off(overlay->sub);

    buf->age = 0;
    term->render.last_overlay_buf = buf;
    term->render.last_overlay_style = style;
}

int
render_worker_thread(void *_ctx)
{
    struct render_worker_context *ctx = _ctx;
    struct terminal *term = ctx->term;
    const int my_id = ctx->my_id;
    free(ctx);

    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    char proc_title[16];
    snprintf(proc_title, sizeof(proc_title), "foot:render:%d", my_id);

    if (pthread_setname_np(pthread_self(), proc_title) < 0)
        LOG_ERRNO("render worker %d: failed to set process title", my_id);

    sem_t *start = &term->render.workers.start;
    sem_t *done = &term->render.workers.done;
    mtx_t *lock = &term->render.workers.lock;

    while (true) {
        sem_wait(start);

        struct buffer *buf = term->render.workers.buf;
        bool frame_done = false;

        /* Translate offset-relative cursor row to view-relative */
        struct coord cursor = {-1, -1};
        if (!term->hide_cursor) {
            cursor = term->grid->cursor.point;
            cursor.row += term->grid->offset;
            cursor.row -= term->grid->view;
            cursor.row &= term->grid->num_rows - 1;
        }

        while (!frame_done) {
            mtx_lock(lock);
            xassert(tll_length(term->render.workers.queue) > 0);

            int row_no = tll_pop_front(term->render.workers.queue);
            mtx_unlock(lock);

            switch (row_no) {
            default: {
                xassert(buf != NULL);

                struct row *row = grid_row_in_view(term->grid, row_no);
                int cursor_col = cursor.row == row_no ? cursor.col : -1;

                render_row(term, buf->pix[my_id], row, row_no, cursor_col);
                break;
            }

            case -1:
                frame_done = true;
                sem_post(done);
                break;

            case -2:
                return 0;
            }
        }
    };

    return -1;
}

struct csd_data
get_csd_data(const struct terminal *term, enum csd_surface surf_idx)
{
    xassert(term->window->csd_mode == CSD_YES);

    const bool borders_visible = wayl_win_csd_borders_visible(term->window);
    const bool title_visible = wayl_win_csd_titlebar_visible(term->window);

    /* Only title bar is rendered in maximized mode */
    const int border_width = borders_visible
        ? term->conf->csd.border_width * term->scale : 0;

    const int title_height = title_visible
        ? term->conf->csd.title_height * term->scale : 0;

    const int button_width = title_visible
        ? term->conf->csd.button_width * term->scale : 0;

    const int button_close_width = term->width >= 1 * button_width
        ? button_width : 0;

    const int button_maximize_width = term->width >= 2 * button_width
        ? button_width : 0;

    const int button_minimize_width = term->width >= 3 * button_width
        ? button_width : 0;

    switch (surf_idx) {
    case CSD_SURF_TITLE:  return (struct csd_data){            0,                -title_height,                   term->width,                 title_height};
    case CSD_SURF_LEFT:   return (struct csd_data){-border_width,                -title_height,                   border_width, title_height + term->height};
    case CSD_SURF_RIGHT:  return (struct csd_data){  term->width,                -title_height,                   border_width, title_height + term->height};
    case CSD_SURF_TOP:    return (struct csd_data){-border_width, -title_height - border_width, term->width + 2 * border_width,                border_width};
    case CSD_SURF_BOTTOM: return (struct csd_data){-border_width,                 term->height, term->width + 2 * border_width,                border_width};

    /* Positioned relative to CSD_SURF_TITLE */
    case CSD_SURF_MINIMIZE: return (struct csd_data){term->width - 3 * button_width, 0, button_minimize_width, title_height};
    case CSD_SURF_MAXIMIZE: return (struct csd_data){term->width - 2 * button_width, 0, button_maximize_width, title_height};
    case CSD_SURF_CLOSE:    return (struct csd_data){term->width - 1 * button_width, 0, button_close_width,    title_height};

    case CSD_SURF_COUNT:
        break;
    }

    BUG("Invalid csd_surface type");
    return (struct csd_data){0};
}

static void
csd_commit(struct terminal *term, struct wl_surface *surf, struct buffer *buf)
{
    xassert(buf->width % term->scale == 0);
    xassert(buf->height % term->scale == 0);

    wl_surface_attach(surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, buf->width, buf->height);
    wl_surface_set_buffer_scale(surf, term->scale);
    wl_surface_commit(surf);
}

static void
render_csd_part(struct terminal *term,
                struct wl_surface *surf, struct buffer *buf,
                int width, int height, pixman_color_t *color)
{
    xassert(term->window->csd_mode == CSD_YES);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], color, 1,
        &(pixman_rectangle16_t){0, 0, buf->width, buf->height});
}

static void
render_osd(struct terminal *term,
           struct wl_surface *surf, struct wl_subsurface *sub_surf,
           struct fcft_font *font, struct buffer *buf,
           const char32_t *text, uint32_t _fg, uint32_t _bg,
           unsigned x, unsigned y)
{
    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 0, 0, buf->width, buf->height);
    pixman_image_set_clip_region32(buf->pix[0], &clip);
    pixman_region32_fini(&clip);

    uint16_t alpha = _bg >> 24 | (_bg >> 24 << 8);
    pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, alpha);
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &bg, 1,
        &(pixman_rectangle16_t){0, 0, buf->width, buf->height});

    pixman_color_t fg = color_hex_to_pixman(_fg);
    const int x_ofs = term->font_x_ofs;

    const size_t len = c32len(text);
    struct fcft_text_run *text_run = NULL;
    const struct fcft_glyph **glyphs = NULL;
    const struct fcft_glyph *_glyphs[len];
    size_t glyph_count = 0;

    if (fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING) {
        text_run = fcft_rasterize_text_run_utf32(
            font, len, (const char32_t *)text, term->font_subpixel);

        if (text_run != NULL) {
            glyphs = text_run->glyphs;
            glyph_count = text_run->count;
        }
    }

    if (glyphs == NULL) {
        for (size_t i = 0; i < len; i++) {
            const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(
                font, text[i], term->font_subpixel);

            if (glyph == NULL)
                continue;

            _glyphs[glyph_count++] = glyph;
        }

        glyphs = _glyphs;
    }

    pixman_image_t *src = pixman_image_create_solid_fill(&fg);

    for (size_t i = 0; i < glyph_count; i++) {
        const struct fcft_glyph *glyph = glyphs[i];

        if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, buf->pix[0], 0, 0, 0, 0,
                x + x_ofs + glyph->x, y + term->font_y_ofs + font->ascent - glyph->y,
                glyph->width, glyph->height);
        } else {
            pixman_image_composite32(
                PIXMAN_OP_OVER, src, glyph->pix, buf->pix[0], 0, 0, 0, 0,
                x + x_ofs + glyph->x, y + term->font_y_ofs + font->ascent - glyph->y,
                glyph->width, glyph->height);
        }

        x += glyph->advance.x;
    }

    fcft_text_run_destroy(text_run);
    pixman_image_unref(src);
    pixman_image_set_clip_region32(buf->pix[0], NULL);

    xassert(buf->width % term->scale == 0);
    xassert(buf->height % term->scale == 0);

    quirk_weston_subsurface_desync_on(sub_surf);
    wl_surface_attach(surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, buf->width, buf->height);
    wl_surface_set_buffer_scale(surf, term->scale);

    struct wl_region *region = wl_compositor_create_region(term->wl->compositor);
    if (region != NULL) {
        wl_region_add(region, 0, 0, buf->width, buf->height);
        wl_surface_set_opaque_region(surf, region);
        wl_region_destroy(region);
    }

    wl_surface_commit(surf);
    quirk_weston_subsurface_desync_off(sub_surf);
}

static void
render_csd_title(struct terminal *term, const struct csd_data *info,
                 struct buffer *buf)
{
    xassert(term->window->csd_mode == CSD_YES);

    struct wl_surf_subsurf *surf = &term->window->csd.surface[CSD_SURF_TITLE];
    if (info->width == 0 || info->height == 0)
        return;

    xassert(info->width % term->scale == 0);
    xassert(info->height % term->scale == 0);

    uint32_t bg = term->conf->csd.color.title_set
        ? term->conf->csd.color.title
        : 0xffu << 24 | term->conf->colors.fg;
    uint32_t fg = term->conf->csd.color.buttons_set
        ? term->conf->csd.color.buttons
        : term->conf->colors.bg;

    if (!term->visual_focus) {
        bg = color_dim(term, bg);
        fg = color_dim(term, fg);
    }

    char32_t *_title_text = ambstoc32(term->window_title);
    const char32_t *title_text = _title_text != NULL ? _title_text : U"";

    struct wl_window *win = term->window;

    const struct fcft_glyph *M = fcft_rasterize_char_utf32(
        win->csd.font, U'M', term->font_subpixel);

    const int margin = M != NULL ? M->advance.x : win->csd.font->max_advance.x;

    render_osd(term, surf->surf, surf->sub, win->csd.font,
               buf, title_text, fg, bg, margin,
               (buf->height - win->csd.font->height) / 2);

    csd_commit(term, surf->surf, buf);
    free(_title_text);
}

static void
render_csd_border(struct terminal *term, enum csd_surface surf_idx,
                  const struct csd_data *info, struct buffer *buf)
{
    xassert(term->window->csd_mode == CSD_YES);
    xassert(surf_idx >= CSD_SURF_LEFT && surf_idx <= CSD_SURF_BOTTOM);

    struct wl_surface *surf = term->window->csd.surface[surf_idx].surf;

    if (info->width == 0 || info->height == 0)
        return;

    xassert(info->width % term->scale == 0);
    xassert(info->height % term->scale == 0);

    {
        pixman_color_t color = color_hex_to_pixman_with_alpha(0, 0);
        render_csd_part(term, surf, buf, info->width, info->height, &color);
    }

    /*
     * The “visible” border.
     */

    int scale = term->scale;
    int bwidth = term->conf->csd.border_width * scale;
    int vwidth = term->conf->csd.border_width_visible * scale; /* Visible size */

    xassert(bwidth >= vwidth);

    if (vwidth > 0) {

        const struct config *conf = term->conf;
        int x = 0, y = 0, w = 0, h = 0;


        switch (surf_idx) {
        case CSD_SURF_TOP:
        case CSD_SURF_BOTTOM:
            x = bwidth - vwidth;
            y = surf_idx == CSD_SURF_TOP ? info->height - vwidth : 0;
            w = info->width - 2 * x;
            h = vwidth;
            break;

        case CSD_SURF_LEFT:
        case CSD_SURF_RIGHT:
            x = surf_idx == CSD_SURF_LEFT ? bwidth - vwidth : 0;
            y = 0;
            w = vwidth;
            h = info->height;
            break;

        case CSD_SURF_TITLE:
        case CSD_SURF_MINIMIZE:
        case CSD_SURF_MAXIMIZE:
        case CSD_SURF_CLOSE:
        case CSD_SURF_COUNT:
            BUG("unexpected CSD surface type");
        }

        xassert(x >= 0);
        xassert(y >= 0);
        xassert(w >= 0);
        xassert(h >= 0);

        xassert(x + w <= info->width);
        xassert(y + h <= info->height);

        uint32_t _color =
            conf->csd.color.border_set ? conf->csd.color.border :
            conf->csd.color.title_set ? conf->csd.color.title :
            0xffu << 24 | term->conf->colors.fg;
        if (!term->visual_focus)
            _color = color_dim(term, _color);

        uint16_t alpha = _color >> 24 | (_color >> 24 << 8);
        pixman_color_t color = color_hex_to_pixman_with_alpha(_color, alpha);


        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, buf->pix[0], &color, 1,
            &(pixman_rectangle16_t){x, y, w, h});
    }

    csd_commit(term, surf, buf);
}

static pixman_color_t
get_csd_button_fg_color(const struct config *conf)
{
    uint32_t _color = conf->colors.bg;
    uint16_t alpha = 0xffff;

    if (conf->csd.color.buttons_set) {
        _color = conf->csd.color.buttons;
        alpha = _color >> 24 | (_color >> 24 << 8);
    }

    return color_hex_to_pixman_with_alpha(_color, alpha);
}

static void
render_csd_button_minimize(struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = get_csd_button_fg_color(term->conf);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    const int max_height = buf->height / 2;
    const int max_width = buf->width / 2;

    int width = max_width;
    int height = max_width / 2;

    if (height > max_height) {
        height = max_height;
        width = height * 2;
    }

    xassert(width <= max_width);
    xassert(height <= max_height);

    int x_margin = (buf->width - width) / 2.;
    int y_margin = (buf->height - height) / 2.;

    pixman_triangle_t tri = {
        .p1 = {
            .x = pixman_int_to_fixed(x_margin),
            .y = pixman_int_to_fixed(y_margin),
        },
        .p2 = {
            .x = pixman_int_to_fixed(x_margin + width),
            .y = pixman_int_to_fixed(y_margin),
        },
        .p3 = {
            .x = pixman_int_to_fixed(buf->width / 2),
            .y = pixman_int_to_fixed(y_margin + height),
        },
    };

    pixman_composite_triangles(
        PIXMAN_OP_OVER, src, buf->pix[0], PIXMAN_a1,
        0, 0, 0, 0, 1, &tri);
    pixman_image_unref(src);
}

static void
render_csd_button_maximize_maximized(
    struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = get_csd_button_fg_color(term->conf);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    const int max_height = buf->height / 3;
    const int max_width = buf->width / 3;

    int width = min(max_height, max_width);
    int thick = min(width / 2, 1 * term->scale);

    const int x_margin = (buf->width - width) / 2;
    const int y_margin = (buf->height - width) / 2;

    xassert(x_margin + width - thick >= 0);
    xassert(width - 2 * thick >= 0);
    xassert(y_margin + width - thick >= 0);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &color, 4,
        (pixman_rectangle16_t[]){
            {x_margin, y_margin, width, thick},
            {x_margin, y_margin + thick, thick, width - 2 * thick},
            {x_margin + width - thick, y_margin + thick, thick, width - 2 * thick},
            {x_margin, y_margin + width - thick, width, thick}});

    pixman_image_unref(src);

}

static void
render_csd_button_maximize_window(
    struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = get_csd_button_fg_color(term->conf);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    const int max_height = buf->height / 2;
    const int max_width = buf->width / 2;

    int width = max_width;
    int height = max_width / 2;

    if (height > max_height) {
        height = max_height;
        width = height * 2;
    }

    xassert(width <= max_width);
    xassert(height <= max_height);

    int x_margin = (buf->width - width) / 2.;
    int y_margin = (buf->height - height) / 2.;

    pixman_triangle_t tri = {
        .p1 = {
            .x = pixman_int_to_fixed(buf->width / 2),
            .y = pixman_int_to_fixed(y_margin),
        },
        .p2 = {
            .x = pixman_int_to_fixed(x_margin),
            .y = pixman_int_to_fixed(y_margin + height),
        },
        .p3 = {
            .x = pixman_int_to_fixed(x_margin + width),
            .y = pixman_int_to_fixed(y_margin + height),
        },
    };

    pixman_composite_triangles(
        PIXMAN_OP_OVER, src, buf->pix[0], PIXMAN_a1,
        0, 0, 0, 0, 1, &tri);

    pixman_image_unref(src);
}

static void
render_csd_button_maximize(struct terminal *term, struct buffer *buf)
{
    if (term->window->is_maximized)
        render_csd_button_maximize_maximized(term, buf);
    else
        render_csd_button_maximize_window(term, buf);
}

static void
render_csd_button_close(struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = get_csd_button_fg_color(term->conf);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    const int max_height = buf->height / 3;
    const int max_width = buf->width / 3;

    int width = min(max_height, max_width);

    const int x_margin = (buf->width - width) / 2;
    const int y_margin = (buf->height - width) / 2;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &color, 1,
        &(pixman_rectangle16_t){x_margin, y_margin, width, width});

    pixman_image_unref(src);
}

static void
render_csd_button(struct terminal *term, enum csd_surface surf_idx,
                  const struct csd_data *info, struct buffer *buf)
{
    xassert(term->window->csd_mode == CSD_YES);
    xassert(surf_idx >= CSD_SURF_MINIMIZE && surf_idx <= CSD_SURF_CLOSE);

    struct wl_surface *surf = term->window->csd.surface[surf_idx].surf;

    if (info->width == 0 || info->height == 0)
        return;

    xassert(info->width % term->scale == 0);
    xassert(info->height % term->scale == 0);

    uint32_t _color;
    uint16_t alpha = 0xffff;
    bool is_active = false;
    bool is_set = false;
    const uint32_t *conf_color = NULL;

    switch (surf_idx) {
    case CSD_SURF_MINIMIZE:
        _color = term->conf->colors.table[4];  /* blue */
        is_set = term->conf->csd.color.minimize_set;
        conf_color = &term->conf->csd.color.minimize;
        is_active = term->active_surface == TERM_SURF_BUTTON_MINIMIZE;
        break;

    case CSD_SURF_MAXIMIZE:
        _color = term->conf->colors.table[2];  /* green */
        is_set = term->conf->csd.color.maximize_set;
        conf_color = &term->conf->csd.color.maximize;
        is_active = term->active_surface == TERM_SURF_BUTTON_MAXIMIZE;
        break;

    case CSD_SURF_CLOSE:
        _color = term->conf->colors.table[1];  /* red */
        is_set = term->conf->csd.color.close_set;
        conf_color = &term->conf->csd.color.quit;
        is_active = term->active_surface == TERM_SURF_BUTTON_CLOSE;
        break;

    default:
        BUG("unhandled surface type: %u", (unsigned)surf_idx);
        break;
    }

    if (is_active) {
        if (is_set) {
            _color = *conf_color;
            alpha = _color >> 24 | (_color >> 24 << 8);
        }
    } else {
        _color = 0;
        alpha = 0;
    }

    if (!term->visual_focus)
        _color = color_dim(term, _color);

    pixman_color_t color = color_hex_to_pixman_with_alpha(_color, alpha);
    render_csd_part(term, surf, buf, info->width, info->height, &color);

    switch (surf_idx) {
    case CSD_SURF_MINIMIZE: render_csd_button_minimize(term, buf); break;
    case CSD_SURF_MAXIMIZE: render_csd_button_maximize(term, buf); break;
    case CSD_SURF_CLOSE:    render_csd_button_close(term, buf); break;
        break;

    default:
        BUG("unhandled surface type: %u", (unsigned)surf_idx);
        break;
    }

    csd_commit(term, surf, buf);
}

static void
render_csd(struct terminal *term)
{
    xassert(term->window->csd_mode == CSD_YES);

    if (term->window->is_fullscreen)
        return;

    struct csd_data infos[CSD_SURF_COUNT];
    int widths[CSD_SURF_COUNT];
    int heights[CSD_SURF_COUNT];

    for (size_t i = 0; i < CSD_SURF_COUNT; i++) {
        infos[i] = get_csd_data(term, i);
        const int x = infos[i].x;
        const int y = infos[i].y;
        const int width = infos[i].width;
        const int height = infos[i].height;

        struct wl_surface *surf = term->window->csd.surface[i].surf;
        struct wl_subsurface *sub = term->window->csd.surface[i].sub;

        xassert(surf != NULL);
        xassert(sub != NULL);

        if (width == 0 || height == 0) {
            widths[i] = heights[i] = 0;
            wl_subsurface_set_position(sub, 0, 0);
            wl_surface_attach(surf, NULL, 0, 0);
            wl_surface_commit(surf);
            continue;
        }

        widths[i] = width;
        heights[i] = height;

        wl_subsurface_set_position(sub, x / term->scale, y / term->scale);
    }

    struct buffer *bufs[CSD_SURF_COUNT];
    shm_get_many(term->render.chains.csd, CSD_SURF_COUNT, widths, heights, bufs);

    for (size_t i = CSD_SURF_LEFT; i <= CSD_SURF_BOTTOM; i++)
        render_csd_border(term, i, &infos[i], bufs[i]);
    for (size_t i = CSD_SURF_MINIMIZE; i <= CSD_SURF_CLOSE; i++)
        render_csd_button(term, i, &infos[i], bufs[i]);
    render_csd_title(term, &infos[CSD_SURF_TITLE], bufs[CSD_SURF_TITLE]);
}

static void
render_scrollback_position(struct terminal *term)
{
    if (term->conf->scrollback.indicator.position == SCROLLBACK_INDICATOR_POSITION_NONE)
        return;

    struct wl_window *win = term->window;

    if (term->grid->view == term->grid->offset) {
        if (win->scrollback_indicator.surf != NULL)
            wayl_win_subsurface_destroy(&win->scrollback_indicator);
        return;
    }

    if (win->scrollback_indicator.surf == NULL) {
        if (!wayl_win_subsurface_new(
                win, &win->scrollback_indicator, false))
        {
            LOG_ERR("failed to create scrollback indicator surface");
            return;
        }
    }

    xassert(win->scrollback_indicator.surf != NULL);
    xassert(win->scrollback_indicator.sub != NULL);

    /* Find absolute row number of the scrollback start */
    int scrollback_start = term->grid->offset + term->rows;
    int empty_rows = 0;
    while (term->grid->rows[scrollback_start & (term->grid->num_rows - 1)] == NULL) {
        scrollback_start++;
        empty_rows++;
    }

    /* Rebase viewport against scrollback start (so that 0 is at
     * the beginning of the scrollback) */
    int rebased_view = term->grid->view - scrollback_start + term->grid->num_rows;
    rebased_view &= term->grid->num_rows - 1;

    /* How much of the scrollback is actually used? */
    int populated_rows = term->grid->num_rows - empty_rows;
    xassert(populated_rows > 0);
    xassert(populated_rows <= term->grid->num_rows);

    /*
     * How far down in the scrollback we are.
     *
     *    0% -> at the beginning of the scrollback
     *  100% -> at the bottom, i.e. where new lines are inserted
     */
    double percent =
        rebased_view + term->rows == populated_rows
        ? 1.0
        : (double)rebased_view / (populated_rows - term->rows);

    char32_t _text[64];
    const char32_t *text = _text;
    int cell_count = 0;

    /* *What* to render */
    switch (term->conf->scrollback.indicator.format) {
    case SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE: {
        char percent_str[8];
        snprintf(percent_str, sizeof(percent_str), "%u%%", (int)(100 * percent));
        mbstoc32(_text, percent_str, ALEN(_text));
        cell_count = 3;
        break;
    }

    case SCROLLBACK_INDICATOR_FORMAT_LINENO: {
        char lineno_str[64];
        snprintf(lineno_str, sizeof(lineno_str), "%d", rebased_view + 1);
        mbstoc32(_text, lineno_str, ALEN(_text));
        cell_count = ceil(log10(term->grid->num_rows));
        break;
    }

    case SCROLLBACK_INDICATOR_FORMAT_TEXT:
        text = term->conf->scrollback.indicator.text;
        cell_count = c32len(text);
        break;
    }

    const int scale = term->scale;
    const int margin = 3 * scale;

    const int width =
        (2 * margin + cell_count * term->cell_width + scale - 1) / scale * scale;
    const int height =
        (2 * margin + term->cell_height + scale - 1) / scale * scale;

    /* *Where* to render - parent relative coordinates */
    int surf_top = 0;
    switch (term->conf->scrollback.indicator.position) {
    case SCROLLBACK_INDICATOR_POSITION_NONE:
        BUG("Invalid scrollback indicator position type");
        return;

    case SCROLLBACK_INDICATOR_POSITION_FIXED:
        surf_top = term->cell_height - margin;
        break;

    case SCROLLBACK_INDICATOR_POSITION_RELATIVE: {
        int lines = term->rows - 2;  /* Avoid using first and last rows */
        if (term->is_searching) {
            /* Make sure we don't collide with the scrollback search box */
            lines--;
        }

        lines = max(lines, 0);

        int pixels = max(lines * term->cell_height - height + 2 * margin, 0);
        surf_top = term->cell_height - margin + (int)(percent * pixels);
        break;
    }
    }

    const int x = (term->width - margin - width) / scale * scale;
    const int y = (term->margins.top + surf_top) / scale * scale;

    if (y + height > term->height) {
        wl_surface_attach(win->scrollback_indicator.surf, NULL, 0, 0);
        wl_surface_commit(win->scrollback_indicator.surf);
        return;
    }

    struct buffer_chain *chain = term->render.chains.scrollback_indicator;
    struct buffer *buf = shm_get_buffer(chain, width, height);

    wl_subsurface_set_position(
        win->scrollback_indicator.sub, x / scale, y / scale);

    uint32_t fg = term->colors.table[0];
    uint32_t bg = term->colors.table[8 + 4];
    if (term->conf->colors.use_custom.scrollback_indicator) {
        fg = term->conf->colors.scrollback_indicator.fg;
        bg = term->conf->colors.scrollback_indicator.bg;
    }

    render_osd(
        term,
        win->scrollback_indicator.surf,
        win->scrollback_indicator.sub,
        term->fonts[0], buf, text,
        fg, 0xffu << 24 | bg,
        width - margin - c32len(text) * term->cell_width, margin);
}

static void
render_render_timer(struct terminal *term, struct timespec render_time)
{
    struct wl_window *win = term->window;

    char usecs_str[256];
    double usecs = render_time.tv_sec * 1000000 + render_time.tv_nsec / 1000.0;
    snprintf(usecs_str, sizeof(usecs_str), "%.2f µs", usecs);

    char32_t text[256];
    mbstoc32(text, usecs_str, ALEN(text));

    const int scale = term->scale;
    const int cell_count = c32len(text);
    const int margin = 3 * scale;
    const int width =
        (2 * margin + cell_count * term->cell_width + scale - 1) / scale * scale;
    const int height =
        (2 * margin + term->cell_height + scale - 1) / scale * scale;

    struct buffer_chain *chain = term->render.chains.render_timer;
    struct buffer *buf = shm_get_buffer(chain, width, height);

    wl_subsurface_set_position(
        win->render_timer.sub,
        margin / term->scale,
        (term->margins.top + term->cell_height - margin) / term->scale);

    render_osd(
        term,
        win->render_timer.surf,
        win->render_timer.sub,
        term->fonts[0], buf, text,
        term->colors.table[0], 0xffu << 24 | term->colors.table[8 + 1],
        margin, margin);
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static void
force_full_repaint(struct terminal *term, struct buffer *buf)
{
    tll_free(term->grid->scroll_damage);
    render_margin(term, buf, 0, term->rows, true);
    term_damage_view(term);
}

static void
reapply_old_damage(struct terminal *term, struct buffer *new, struct buffer *old)
{
    static int counter = 0;
    static bool have_warned = false;
    if (!have_warned && ++counter > 5) {
        LOG_WARN("compositor is not releasing buffers immediately; "
                 "expect lower rendering performance");
        have_warned = true;
    }

    if (new->age > 1) {
        memcpy(new->data, old->data, new->height * new->stride);
        return;
    }

    /*
     * TODO: remove this frame’s damage from the region we copy from
     * the old frame.
     *
     * - this frame’s dirty region is only valid *after* we’ve applied
     *   its scroll damage.
     * - last frame’s dirty region is only valid *before* we’ve
     *   applied this frame’s scroll damage.
     *
     * Can we transform one of the regions? It’s not trivial, since
     * scroll damage isn’t just about counting lines; there may be
     * multiple damage records, each with different scrolling regions.
     */
    pixman_region32_t dirty;
    pixman_region32_init(&dirty);

    bool full_repaint_needed = true;

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

        bool row_all_dirty = true;
        for (int c = 0; c < term->cols; c++) {
            if (row->cells[c].attrs.clean) {
                row_all_dirty = false;
                full_repaint_needed = false;
                break;
            }
        }

        if (row_all_dirty) {
            pixman_region32_union_rect(
                &dirty, &dirty,
                term->margins.left,
                term->margins.top + r * term->cell_height,
                term->width - term->margins.left - term->margins.right,
                term->cell_height);
        }
    }

    if (full_repaint_needed) {
        force_full_repaint(term, new);
        return;
    }

    for (size_t i = 0; i < old->scroll_damage_count; i++) {
        const struct damage *dmg = &old->scroll_damage[i];

        switch (dmg->type) {
        case DAMAGE_SCROLL:
            if (term->grid->view == term->grid->offset)
                grid_render_scroll(term, new, dmg);
            break;

        case DAMAGE_SCROLL_REVERSE:
            if (term->grid->view == term->grid->offset)
                grid_render_scroll_reverse(term, new, dmg);
            break;

        case DAMAGE_SCROLL_IN_VIEW:
            grid_render_scroll(term, new, dmg);
            break;

        case DAMAGE_SCROLL_REVERSE_IN_VIEW:
            grid_render_scroll_reverse(term, new, dmg);
            break;
        }
    }

    if (tll_length(term->grid->scroll_damage) == 0) {
        pixman_region32_subtract(&dirty, &old->dirty, &dirty);
        pixman_image_set_clip_region32(new->pix[0], &dirty);
    } else
        pixman_image_set_clip_region32(new->pix[0], &old->dirty);

    pixman_image_composite32(
        PIXMAN_OP_SRC, old->pix[0], NULL, new->pix[0],
        0, 0, 0, 0, 0, 0, term->width, term->height);

    pixman_image_set_clip_region32(new->pix[0], NULL);
    pixman_region32_fini(&dirty);
}

static void
dirty_old_cursor(struct terminal *term)
{
    if (term->render.last_cursor.row != NULL && !term->render.last_cursor.hidden) {
        struct row *row = term->render.last_cursor.row;
        struct cell *cell = &row->cells[term->render.last_cursor.col];
        cell->attrs.clean = 0;
        row->dirty = true;
    }

    /* Remember current cursor position, for the next frame */
    term->render.last_cursor.row = grid_row(term->grid, term->grid->cursor.point.row);
    term->render.last_cursor.col = term->grid->cursor.point.col;
    term->render.last_cursor.hidden = term->hide_cursor;
}

static void
dirty_cursor(struct terminal *term)
{
    if (term->hide_cursor)
        return;

    const struct coord *cursor = &term->grid->cursor.point;

    struct row *row = grid_row(term->grid, cursor->row);
    struct cell *cell = &row->cells[cursor->col];
    cell->attrs.clean = 0;
    row->dirty = true;
}

static void
grid_render(struct terminal *term)
{
    if (term->shutdown.in_progress)
        return;

    struct timespec start_time, start_double_buffering = {0}, stop_double_buffering = {0};

    if (term->conf->tweak.render_timer != RENDER_TIMER_NONE)
        clock_gettime(CLOCK_MONOTONIC, &start_time);

    xassert(term->width > 0);
    xassert(term->height > 0);

    struct buffer_chain *chain = term->render.chains.grid;
    struct buffer *buf = shm_get_buffer(chain, term->width, term->height);

    /* Dirty old and current cursor cell, to ensure they’re repainted */
    dirty_old_cursor(term);
    dirty_cursor(term);

    if (term->render.last_buf == NULL ||
        term->render.last_buf->width != buf->width ||
        term->render.last_buf->height != buf->height ||
        term->render.margins)
    {
        force_full_repaint(term, buf);
    }

    else if (buf->age > 0) {
        LOG_DBG("buffer age: %u (%p)", buf->age, (void *)buf);

        xassert(term->render.last_buf != NULL);
        xassert(term->render.last_buf != buf);
        xassert(term->render.last_buf->width == buf->width);
        xassert(term->render.last_buf->height == buf->height);

        clock_gettime(CLOCK_MONOTONIC, &start_double_buffering);
        reapply_old_damage(term, buf, term->render.last_buf);
        clock_gettime(CLOCK_MONOTONIC, &stop_double_buffering);
    }

    if (term->render.last_buf != NULL) {
        shm_unref(term->render.last_buf);
        term->render.last_buf = NULL;
    }

    term->render.last_buf = buf;
    shm_addref(buf);
    buf->age = 0;

    free(term->render.last_buf->scroll_damage);
    buf->scroll_damage_count = tll_length(term->grid->scroll_damage);
    buf->scroll_damage = xmalloc(
        buf->scroll_damage_count * sizeof(buf->scroll_damage[0]));

    {
        size_t i = 0;
        tll_foreach(term->grid->scroll_damage, it) {
            buf->scroll_damage[i++] = it->item;

            switch (it->item.type) {
            case DAMAGE_SCROLL:
                if (term->grid->view == term->grid->offset)
                    grid_render_scroll(term, buf, &it->item);
                break;

            case DAMAGE_SCROLL_REVERSE:
                if (term->grid->view == term->grid->offset)
                    grid_render_scroll_reverse(term, buf, &it->item);
                break;

            case DAMAGE_SCROLL_IN_VIEW:
                grid_render_scroll(term, buf, &it->item);
                break;

            case DAMAGE_SCROLL_REVERSE_IN_VIEW:
                grid_render_scroll_reverse(term, buf, &it->item);
                break;
            }

            tll_remove(term->grid->scroll_damage, it);
        }
    }

    /*
     * Ensure selected cells have their 'selected' bit set. This is
     * normally "automatically" true - the bit is set when the
     * selection is made.
     *
     * However, if the cell is updated (printed to) while the
     * selection is active, the 'selected' bit is cleared. Checking
     * for this and re-setting the bit in term_print() is too
     * expensive performance wise.
     *
     * Instead, we synchronize the selection bits here and now. This
     * makes the performance impact linear to the number of selected
     * cells rather than to the number of updated cells.
     *
     * (note that selection_dirty_cells() will not set the dirty flag
     * on cells where the 'selected' bit is already set)
     */
    selection_dirty_cells(term);

    /* Translate offset-relative row to view-relative, unless cursor
     * is hidden, then we just set it to -1 */
    struct coord cursor = {-1, -1};
    if (!term->hide_cursor) {
        cursor = term->grid->cursor.point;
        cursor.row += term->grid->offset;
        cursor.row -= term->grid->view;
        cursor.row &= term->grid->num_rows - 1;
    }

    if (term->conf->tweak.overflowing_glyphs) {
        /*
         * Pre-pass to dirty cells affected by overflowing glyphs.
         *
         * Given any two pair of cells where the first cell is
         * overflowing into the second, *both* cells must be
         * re-rendered if any one of them is dirty.
         *
         * Thus, given a string of overflowing glyphs, with a single
         * dirty cell in the middle, we need to re-render the entire
         * string.
         */
        for (int r = 0; r < term->rows; r++) {
            struct row *row = grid_row_in_view(term->grid, r);

            if (!row->dirty)
                continue;

            /* Loop row from left to right, looking for dirty cells */
            for (struct cell *cell = &row->cells[0];
                 cell < &row->cells[term->cols];
                 cell++)
            {
                if (cell->attrs.clean)
                    continue;

                /*
                 * Cell is dirty, go back and dirty previous cells, if
                 * they are overflowing.
                 *
                 * As soon as we see a non-overflowing cell we can
                 * stop, since it isn’t affecting the string of
                 * overflowing glyphs that follows it.
                 *
                 * As soon as we see a dirty cell, we can stop, since
                 * that means we’ve already handled it (remember the
                 * outer loop goes from left to right).
                 */
                for (struct cell *c = cell - 1; c >= &row->cells[0]; c--) {
                    if (c->attrs.confined)
                        break;
                    if (!c->attrs.clean)
                        break;
                    c->attrs.clean = false;
                }

                /*
                 * Now move forward, dirtying all cells until we hit a
                 * non-overflowing cell.
                 *
                 * Note that the first non-overflowing cell must be
                 * re-rendered as well, but any cell *after* that is
                 * unaffected by the string of overflowing glyphs
                 * we’re dealing with right now.
                 *
                 * For performance, this iterates the *outer* loop’s
                 * cell pointer - no point in re-checking all these
                 * glyphs again, in the outer loop.
                 */
                for (; cell < &row->cells[term->cols]; cell++) {
                    cell->attrs.clean = false;
                    if (cell->attrs.confined)
                        break;
                }
            }
        }
    }

    render_sixel_images(term, buf->pix[0], &cursor);

    if (term->render.workers.count > 0) {
        mtx_lock(&term->render.workers.lock);
        term->render.workers.buf = buf;
        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_post(&term->render.workers.start);

        xassert(tll_length(term->render.workers.queue) == 0);
    }

    int first_dirty_row = -1;
    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);

        if (!row->dirty) {
            if (first_dirty_row >= 0) {
                int x = term->margins.left;
                int y = term->margins.top + first_dirty_row * term->cell_height;
                int width = term->width - term->margins.left - term->margins.right;
                int height = (r - first_dirty_row) * term->cell_height;

                wl_surface_damage_buffer(
                    term->window->surface, x, y, width, height);
                pixman_region32_union_rect(
                    &buf->dirty, &buf->dirty, 0, y, buf->width, height);
            }
            first_dirty_row = -1;
            continue;
        }

        if (first_dirty_row < 0)
            first_dirty_row = r;

        row->dirty = false;

        if (term->render.workers.count > 0)
            tll_push_back(term->render.workers.queue, r);

        else {
            int cursor_col = cursor.row == r ? cursor.col : -1;
            render_row(term, buf->pix[0], row, r, cursor_col);
        }
    }

    if (first_dirty_row >= 0) {
        int x = term->margins.left;
        int y = term->margins.top + first_dirty_row * term->cell_height;
        int width = term->width - term->margins.left - term->margins.right;
        int height = (term->rows - first_dirty_row) * term->cell_height;

        wl_surface_damage_buffer(term->window->surface, x, y, width, height);
        pixman_region32_union_rect(&buf->dirty, &buf->dirty, 0, y, buf->width, height);
    }

    /* Signal workers the frame is done */
    if (term->render.workers.count > 0) {
        for (size_t i = 0; i < term->render.workers.count; i++)
            tll_push_back(term->render.workers.queue, -1);
        mtx_unlock(&term->render.workers.lock);

        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_wait(&term->render.workers.done);
        term->render.workers.buf = NULL;
    }

    render_overlay(term);
    render_ime_preedit(term, buf);
    render_scrollback_position(term);

    if (term->conf->tweak.render_timer != RENDER_TIMER_NONE) {
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);

        struct timespec render_time;
        timespec_sub(&end_time, &start_time, &render_time);

        struct timespec double_buffering_time;
        timespec_sub(&stop_double_buffering, &start_double_buffering, &double_buffering_time);

        switch (term->conf->tweak.render_timer) {
        case RENDER_TIMER_LOG:
        case RENDER_TIMER_BOTH:
            LOG_INFO("frame rendered in %lds %ldns "
                     "(%lds %ldns double buffering)",
                     (long)render_time.tv_sec,
                     render_time.tv_nsec,
                     (long)double_buffering_time.tv_sec,
                     double_buffering_time.tv_nsec);
            break;

        case RENDER_TIMER_OSD:
        case RENDER_TIMER_NONE:
            break;
        }

        switch (term->conf->tweak.render_timer) {
        case RENDER_TIMER_OSD:
        case RENDER_TIMER_BOTH:
            render_render_timer(term, render_time);
            break;

        case RENDER_TIMER_LOG:
        case RENDER_TIMER_NONE:
            break;
        }
    }

    xassert(term->grid->offset >= 0 && term->grid->offset < term->grid->num_rows);
    xassert(term->grid->view >= 0 && term->grid->view < term->grid->num_rows);

    xassert(term->window->frame_callback == NULL);
    term->window->frame_callback = wl_surface_frame(term->window->surface);
    wl_callback_add_listener(term->window->frame_callback, &frame_listener, term);

    wl_surface_set_buffer_scale(term->window->surface, term->scale);

    if (term->wl->presentation != NULL && term->conf->presentation_timings) {
        struct timespec commit_time;
        clock_gettime(term->wl->presentation_clock_id, &commit_time);

        struct wp_presentation_feedback *feedback = wp_presentation_feedback(
            term->wl->presentation, term->window->surface);

        if (feedback == NULL) {
            LOG_WARN("failed to create presentation feedback");
        } else {
            struct presentation_context *ctx = xmalloc(sizeof(*ctx));
            *ctx = (struct presentation_context){
                .term = term,
                .input.tv_sec = term->render.input_time.tv_sec,
                .input.tv_usec = term->render.input_time.tv_nsec / 1000,
                .commit.tv_sec = commit_time.tv_sec,
                .commit.tv_usec = commit_time.tv_nsec / 1000,
            };

            wp_presentation_feedback_add_listener(
                feedback, &presentation_feedback_listener, ctx);

            term->render.input_time.tv_sec = 0;
            term->render.input_time.tv_nsec = 0;
        }
    }

    if (term->conf->tweak.damage_whole_window) {
        wl_surface_damage_buffer(
            term->window->surface, 0, 0, INT32_MAX, INT32_MAX);
    }

    xassert(buf->width % term->scale == 0);
    xassert(buf->height % term->scale == 0);

    wl_surface_attach(term->window->surface, buf->wl_buf, 0, 0);
    wl_surface_commit(term->window->surface);
}

static void
render_search_box(struct terminal *term)
{
    xassert(term->window->search.sub != NULL);

    /*
     * We treat the search box pretty much like a row of cells. That
     * is, a glyph is either 1 or 2 (or more) “cells” wide.
     *
     * The search ‘length’, and ‘cursor’ (position) is in
     * *characters*, not cells. This means we need to translate from
     * character count to cell count when calculating the length of
     * the search box, where in the search string we should start
     * rendering etc.
     */

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    /* TODO: do we want to/need to handle multi-seat? */
    struct seat *ime_seat = NULL;
    tll_foreach(term->wl->seats, it) {
        if (it->item.kbd_focus == term) {
            ime_seat = &it->item;
            break;
        }
    }

    size_t text_len = term->search.len;
    if (ime_seat != NULL && ime_seat->ime.preedit.text != NULL)
        text_len += c32len(ime_seat->ime.preedit.text);

    char32_t *text = xmalloc((text_len + 1) *  sizeof(char32_t));
    text[0] = U'\0';

    /* Copy everything up to the cursor */
    c32ncpy(text, term->search.buf, term->search.cursor);
    text[term->search.cursor] = U'\0';

    /* Insert pre-edit text at cursor */
    if (ime_seat != NULL && ime_seat->ime.preedit.text != NULL)
        c32cat(text, ime_seat->ime.preedit.text);

    /* And finally everything after the cursor */
    c32ncat(text, &term->search.buf[term->search.cursor],
            term->search.len - term->search.cursor);
#else
    const char32_t *text = term->search.buf;
    const size_t text_len = term->search.len;
#endif

    /* Calculate the width of each character */
    int widths[text_len + 1];
    for (size_t i = 0; i < text_len; i++)
        widths[i] = max(0, c32width(text[i]));
    widths[text_len] = 0;

    const size_t total_cells = c32swidth(text, text_len);
    const size_t wanted_visible_cells = max(20, total_cells);

    xassert(term->scale >= 1);
    const int scale = term->scale;

    const size_t margin = 3 * scale;

    const size_t width = term->width - 2 * margin;
    const size_t visible_width = min(
        term->width - 2 * margin,
        (2 * margin + wanted_visible_cells * term->cell_width + scale - 1) / scale * scale);
    const size_t height = min(
        term->height - 2 * margin,
        (2 * margin + 1 * term->cell_height + scale - 1) / scale * scale);

    const size_t visible_cells = (visible_width - 2 * margin) / term->cell_width;
    size_t glyph_offset = term->render.search_glyph_offset;

    struct buffer_chain *chain = term->render.chains.search;
    struct buffer *buf = shm_get_buffer(chain, width, height);

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 0, 0, width, height);
    pixman_image_set_clip_region32(buf->pix[0], &clip);
    pixman_region32_fini(&clip);

#define WINDOW_X(x) (margin + x)
#define WINDOW_Y(y) (term->height - margin - height + y)

    /* Background - yellow on empty/match, red on mismatch */
    pixman_color_t color = color_hex_to_pixman(
        term->search.match_len == text_len
        ? term->colors.table[3] : term->colors.table[1]);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &color,
        1, &(pixman_rectangle16_t){width - visible_width, 0, visible_width, height});

    pixman_color_t transparent = color_hex_to_pixman_with_alpha(0, 0);
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &transparent,
        1, &(pixman_rectangle16_t){0, 0, width - visible_width, height});

    struct fcft_font *font = term->fonts[0];
    const int x_left = width - visible_width + margin;
    const int x_ofs = term->font_x_ofs;
    int x = x_left;
    int y = margin;
    pixman_color_t fg = color_hex_to_pixman(term->colors.table[0]);

    /* Move offset we start rendering at, to ensure the cursor is visible */
    for (size_t i = 0, cell_idx = 0; i <= term->search.cursor; cell_idx += widths[i], i++) {
        if (i != term->search.cursor)
            continue;

#if (FOOT_IME_ENABLED) && FOOT_IME_ENABLED
        if (ime_seat != NULL && ime_seat->ime.preedit.cells != NULL) {
            if (ime_seat->ime.preedit.cursor.start == ime_seat->ime.preedit.cursor.end) {
                /* All IME's I've seen so far keeps the cursor at
                 * index 0, so ensure the *end* of the pre-edit string
                 * is visible */
                cell_idx += ime_seat->ime.preedit.count;
            } else {
                /* Try to predict in which direction we'll shift the text */
                if (cell_idx + ime_seat->ime.preedit.cursor.start > glyph_offset)
                    cell_idx += ime_seat->ime.preedit.cursor.end;
                else
                    cell_idx += ime_seat->ime.preedit.cursor.start;
            }
        }
#endif

        if (cell_idx < glyph_offset) {
            /* Shift to the *left*, making *this* character the
             * *first* visible one */
            term->render.search_glyph_offset = glyph_offset = cell_idx;
        }

        else if (cell_idx > glyph_offset + visible_cells) {
            /* Shift to the *right*, making *this* character the
             * *last* visible one */
            term->render.search_glyph_offset = glyph_offset =
                cell_idx - min(cell_idx, visible_cells);
        }

        /* Adjust offset if there is free space available */
        if (total_cells - glyph_offset < visible_cells) {
            term->render.search_glyph_offset = glyph_offset =
                total_cells - min(total_cells, visible_cells);
        }

        break;
    }

    /* Ensure offset is at a character boundary */
    for (size_t i = 0, cell_idx = 0; i <= text_len; cell_idx += widths[i], i++) {
        if (cell_idx >= glyph_offset) {
            term->render.search_glyph_offset = glyph_offset = cell_idx;
            break;
        }
    }

    /*
     * Render the search string, starting at ‘glyph_offset’. Note that
     * glyph_offset is in cells, not characters
     */
    for (size_t i = 0,
             cell_idx = 0,
             width = widths[i],
             next_cell_idx = width;
         i < text_len;
         i++,
             cell_idx = next_cell_idx,
             width = widths[i],
             next_cell_idx += width)
    {
    /* Convert subsurface coordinates to window coordinates*/
        /* Render cursor */
        if (i == term->search.cursor) {
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
            bool have_preedit =
                ime_seat != NULL && ime_seat->ime.preedit.cells != NULL;
            bool hidden =
                ime_seat != NULL && ime_seat->ime.preedit.cursor.hidden;

            if (have_preedit && !hidden) {
                /* Cursor may be outside the visible area:
                 * cell_idx-glyph_offset can be negative */
                int cells_left = visible_cells - max(
                    (ssize_t)(cell_idx - glyph_offset), 0);

                /* If cursor is outside the visible area, we need to
                 * adjust our rectangle's position */
                int start = ime_seat->ime.preedit.cursor.start
                    + min((ssize_t)(cell_idx - glyph_offset), 0);
                int end = ime_seat->ime.preedit.cursor.end
                    + min((ssize_t)(cell_idx - glyph_offset), 0);

                if (start == end) {
                    int count = min(ime_seat->ime.preedit.count, cells_left);

                    /* Underline the entire (visible part of) pre-edit text */
                    draw_underline(term, buf->pix[0], font, &fg, x, y, count);

                    /* Bar-styled cursor, if in the visible area */
                    if (start >= 0 && start <= visible_cells) {
                        draw_beam_cursor(
                            term, buf->pix[0], font, &fg,
                            x + start * term->cell_width, y);
                    }

                    term_ime_set_cursor_rect(term,
                        WINDOW_X(x + start * term->cell_width), WINDOW_Y(y),
                        1, term->cell_height);
                } else {
                    /* Underline everything before and after the cursor */
                    int count1 = min(start, cells_left);
                    int count2 = max(
                        min(ime_seat->ime.preedit.count - ime_seat->ime.preedit.cursor.end,
                            cells_left - end),
                        0);
                    draw_underline(term, buf->pix[0], font, &fg, x, y, count1);
                    draw_underline(term, buf->pix[0], font, &fg, x + end * term->cell_width, y, count2);

                    /* TODO: how do we handle a partially hidden rectangle? */
                    if (start >= 0 && end <= visible_cells) {
                        draw_unfocused_block(
                            term, buf->pix[0], &fg, x + start * term->cell_width, y, end - start);
                    }
                    term_ime_set_cursor_rect(term,
                        WINDOW_X(x + start * term->cell_width), WINDOW_Y(y),
                        term->cell_width * (end - start), term->cell_height);
                }
            } else if (!have_preedit)
#endif
            {
                /* Cursor *should* be in the visible area */
                xassert(cell_idx >= glyph_offset);
                xassert(cell_idx <= glyph_offset + visible_cells);
                draw_beam_cursor(term, buf->pix[0], font, &fg, x, y);
                term_ime_set_cursor_rect(
                    term, WINDOW_X(x), WINDOW_Y(y), 1, term->cell_height);
            }
        }

        if (next_cell_idx >= glyph_offset && next_cell_idx - glyph_offset > visible_cells) {
            /* We're now beyond the visible area - nothing more to render */
            break;
        }

        if (cell_idx < glyph_offset) {
            /* We haven't yet reached the visible part of the string */
            cell_idx = next_cell_idx;
            continue;
        }

        const struct fcft_glyph *glyph = fcft_rasterize_char_utf32(
            font, text[i], term->font_subpixel);

        if (glyph == NULL) {
            cell_idx = next_cell_idx;
            continue;
        }

        if (unlikely(pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)) {
            /* Glyph surface is a pre-rendered image (typically a color emoji...) */
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, buf->pix[0], 0, 0, 0, 0,
                x + x_ofs + glyph->x, y + font_baseline(term) - glyph->y,
                glyph->width, glyph->height);
        } else {
            int combining_ofs = width == 0
                ? (glyph->x < 0
                   ? width * term->cell_width
                   : (width - 1) * term->cell_width)
                : 0;  /* Not a zero-width character - no additional offset */
            pixman_image_t *src = pixman_image_create_solid_fill(&fg);
            pixman_image_composite32(
                PIXMAN_OP_OVER, src, glyph->pix, buf->pix[0], 0, 0, 0, 0,
                x + x_ofs + combining_ofs + glyph->x,
                y + font_baseline(term) - glyph->y,
            glyph->width, glyph->height);
            pixman_image_unref(src);
        }

        x += width * term->cell_width;
        cell_idx = next_cell_idx;
    }

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
        if (ime_seat != NULL && ime_seat->ime.preedit.cells != NULL)
            /* Already rendered */;
        else
#endif
        if (term->search.cursor >= term->search.len) {
            draw_beam_cursor(term, buf->pix[0], font, &fg, x, y);
            term_ime_set_cursor_rect(
                term, WINDOW_X(x), WINDOW_Y(y), 1, term->cell_height);
        }

    quirk_weston_subsurface_desync_on(term->window->search.sub);

    /* TODO: this is only necessary on a window resize */
    wl_subsurface_set_position(
        term->window->search.sub,
        margin / scale,
        max(0, (int32_t)term->height - height - margin) / scale);

    xassert(buf->width % scale == 0);
    xassert(buf->height % scale == 0);

    wl_surface_attach(term->window->search.surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(term->window->search.surf, 0, 0, width, height);
    wl_surface_set_buffer_scale(term->window->search.surf, scale);

    struct wl_region *region = wl_compositor_create_region(term->wl->compositor);
    if (region != NULL) {
        wl_region_add(region, width - visible_width, 0, visible_width, height);
        wl_surface_set_opaque_region(term->window->search.surf, region);
        wl_region_destroy(region);
    }

    wl_surface_commit(term->window->search.surf);
    quirk_weston_subsurface_desync_off(term->window->search.sub);

#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    free(text);
#endif
#undef WINDOW_X
#undef WINDOW_Y
}

static void
render_urls(struct terminal *term)
{
    struct wl_window *win = term->window;
    xassert(tll_length(win->urls) > 0);

    const int scale = term->scale;
    const int x_margin = 2 * scale;
    const int y_margin = 1 * scale;

    /* Calculate view start, counted from the *current* scrollback start */
    const int scrollback_end
        = (term->grid->offset + term->rows) & (term->grid->num_rows - 1);
    const int view_start
        = (term->grid->view
           - scrollback_end
           + term->grid->num_rows) & (term->grid->num_rows - 1);
    const int view_end = view_start + term->rows - 1;

    const bool show_url = term->urls_show_uri_on_jump_label;

    /*
     * There can potentially be a lot of URLs.
     *
     * Since each URL is a separate sub-surface, and requires its own
     * SHM buffer, we may be allocating a lot of buffers.
     *
     * SHM buffers normally have their own, private SHM buffer
     * pool. Each pool is mmapped, and thus allocates *at least*
     * 4K. Since URL labels are typically small, we end up using an
     * excessive amount of both virtual and physical memory.
     *
     * For this reason, we instead use shm_get_many(), which uses a
     * single, shared pool for all buffers.
     *
     * To be able to use it, we need to have all the *all* the buffer
     * dimensions up front.
     *
     * Thus, the first iteration through the URLs do the heavy
     * lifting: builds the label contents and calculates both its
     * position and size. But instead of rendering the label
     * immediately, we store the calculated data, and then do a second
     * pass, where we first get all our buffers, and then render to
     * them.
     */

    /* Positioning data + label contents */
    struct {
        const struct wl_url *url;
        char32_t *text;
        int x;
        int y;
    } info[tll_length(win->urls)];

    /* For shm_get_many() */
    int widths[tll_length(win->urls)];
    int heights[tll_length(win->urls)];

    size_t render_count = 0;

    tll_foreach(win->urls, it) {
        const struct url *url = it->item.url;
        const char32_t *key = url->key;
        const size_t entered_key_len = c32len(term->url_keys);

        if (key == NULL) {
            /* TODO: if we decide to use the .text field, we cannot
             * just skip the entire jump label like this */
            continue;
        }

        struct wl_surface *surf = it->item.surf.surf;
        struct wl_subsurface *sub_surf = it->item.surf.sub;

        if (surf == NULL || sub_surf == NULL)
            continue;

        bool hide = false;
        const struct coord *pos = &url->range.start;
        const int _row
            = (pos->row
               - scrollback_end
               + term->grid->num_rows) & (term->grid->num_rows - 1);

        if (_row < view_start || _row > view_end)
            hide = true;
        if (c32len(key) <= entered_key_len)
            hide = true;
        if (c32ncasecmp(term->url_keys, key, entered_key_len) != 0)
            hide = true;

        if (hide) {
            wl_surface_attach(surf, NULL, 0, 0);
            wl_surface_commit(surf);
            continue;
        }

        int col = pos->col;
        int row = pos->row - term->grid->view;
        while (row < 0)
            row += term->grid->num_rows;
        row &= (term->grid->num_rows - 1);

        /* Position label slightly above and to the left */
        int x = col * term->cell_width - 15 * term->cell_width / 10;
        int y = row * term->cell_height - 5 * term->cell_height / 10;

        /* Don’t position it outside our window */
        if (x < -term->margins.left)
            x = -term->margins.left;
        if (y < -term->margins.top)
            y = -term->margins.top;

        /* Maximum width of label, in pixels */
        const int max_width =
            term->width - term->margins.left - term->margins.right - x;
        const int max_cols = max_width / term->cell_width;

        const size_t key_len = c32len(key);

        size_t url_len = mbstoc32(NULL, url->url, 0);
        if (url_len == (size_t)-1)
            url_len = 0;

        char32_t url_wchars[url_len + 1];
        mbstoc32(url_wchars, url->url, url_len + 1);

        /* Format label, not yet subject to any size limitations */
        size_t chars = key_len + (show_url ? (2 + url_len) : 0);
        char32_t label[chars + 1];
        label[chars] = U'\0';

        if (show_url) {
            c32cpy(label, key);
            c32cat(label, U": ");
            c32cat(label, url_wchars);
        } else
            c32ncpy(label, key, chars);

        /* Upper case the key characters */
        for (size_t i = 0; i < c32len(key); i++)
            label[i] = toc32upper(label[i]);

        /* Blank already entered key characters */
        for (size_t i = 0; i < entered_key_len; i++)
            label[i] = U' ';

        /*
         * Don’t extend outside our window
         *
         * Truncate label so that it doesn’t extend outside our
         * window.
         *
         * Do it in a way such that we don’t cut the label in the
         * middle of a double-width character.
         */

        int cols = 0;

        for (size_t i = 0; i <= c32len(label); i++) {
            int _cols = c32swidth(label, i);

            if (_cols == (size_t)-1)
                continue;

            if (_cols >= max_cols) {
                if (i > 0)
                    label[i - 1] = U'…';
                label[i] = U'\0';
                cols = max_cols;
                break;
            }
            cols = _cols;
        }

        if (cols == 0)
            continue;

        const int width =
            (2 * x_margin + cols * term->cell_width + scale - 1) / scale * scale;
        const int height =
            (2 * y_margin + term->cell_height + scale - 1) / scale * scale;

        info[render_count].url = &it->item;
        info[render_count].text = xc32dup(label);
        info[render_count].x = x;
        info[render_count].y = y;

        widths[render_count] = width;
        heights[render_count] = height;

        render_count++;
    }

    struct buffer_chain *chain = term->render.chains.url;
    struct buffer *bufs[render_count];
    shm_get_many(chain, render_count, widths, heights, bufs);

    uint32_t fg = term->conf->colors.use_custom.jump_label
        ? term->conf->colors.jump_label.fg
        : term->colors.table[0];
    uint32_t bg = term->conf->colors.use_custom.jump_label
        ? term->conf->colors.jump_label.bg
        : term->colors.table[3];

    for (size_t i = 0; i < render_count; i++) {
        struct wl_surface *surf = info[i].url->surf.surf;
        struct wl_subsurface *sub_surf = info[i].url->surf.sub;

        const char32_t *label = info[i].text;
        const int x = info[i].x;
        const int y = info[i].y;

        xassert(surf != NULL);
        xassert(sub_surf != NULL);

        wl_subsurface_set_position(
            sub_surf,
            (term->margins.left + x) / term->scale,
            (term->margins.top + y) / term->scale);

        render_osd(
            term, surf, sub_surf, term->fonts[0], bufs[i], label,
            fg, 0xffu << 24 | bg, x_margin, y_margin);

        free(info[i].text);
    }
}

static void
render_update_title(struct terminal *term)
{
    static const size_t max_len = 2048;

    const char *title = term->window_title != NULL ? term->window_title : "foot";
    char *copy = NULL;

    if (strlen(title) > max_len) {
        copy = xstrndup(title, max_len);
        title = copy;
    }

    xdg_toplevel_set_title(term->window->xdg_toplevel, title);
    free(copy);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct terminal *term = data;

    xassert(term->window->frame_callback == wl_callback);
    wl_callback_destroy(wl_callback);
    term->window->frame_callback = NULL;

    bool grid = term->render.pending.grid;
    bool csd = term->render.pending.csd;
    bool search = term->is_searching && term->render.pending.search;
    bool urls = urls_mode_is_active(term) > 0 && term->render.pending.urls;

    term->render.pending.grid = false;
    term->render.pending.csd = false;
    term->render.pending.search = false;
    term->render.pending.urls = false;

    struct grid *original_grid = term->grid;
    if (urls_mode_is_active(term)) {
        xassert(term->url_grid_snapshot != NULL);
        term->grid = term->url_grid_snapshot;
    }

    if (csd && term->window->csd_mode == CSD_YES) {
        quirk_weston_csd_on(term);
        render_csd(term);
        quirk_weston_csd_off(term);
    }

    if (search)
        render_search_box(term);

    if (urls)
        render_urls(term);

    if ((grid && !term->delayed_render_timer.is_armed) || (csd | search | urls))
        grid_render(term);

    tll_foreach(term->wl->seats, it) {
        if (it->item.ime_focus == term)
            ime_update_cursor_rect(&it->item);
    }

    term->grid = original_grid;
}

static void
tiocswinsz(struct terminal *term)
{
    if (term->ptmx >= 0) {
        if (ioctl(term->ptmx, (unsigned int)TIOCSWINSZ,
                     &(struct winsize){
                         .ws_row = term->rows,
                         .ws_col = term->cols,
                         .ws_xpixel = term->cols * term->cell_width,
                         .ws_ypixel = term->rows * term->cell_height}) < 0)
        {
            LOG_ERRNO("TIOCSWINSZ");
        }
    }
}

static bool
fdm_tiocswinsz(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    if (events & EPOLLIN)
        tiocswinsz(term);

    if (term->window->resize_timeout_fd >= 0) {
        fdm_del(fdm, term->window->resize_timeout_fd);
        term->window->resize_timeout_fd = -1;
    }
    return true;
}

static void
send_dimensions_to_client(struct terminal *term)
{
    struct wl_window *win = term->window;

    if (!win->is_resizing || term->conf->resize_delay_ms == 0) {
        /* Send new dimensions to client immediately */
        tiocswinsz(term);

        /* And make sure to reset and deallocate a lingering timer */
        if (win->resize_timeout_fd >= 0) {
            fdm_del(term->fdm, win->resize_timeout_fd);
            win->resize_timeout_fd = -1;
        }
    } else {
        /* Send new dimensions to client “in a while” */
        assert(win->is_resizing && term->conf->resize_delay_ms > 0);

        int fd = win->resize_timeout_fd;
        uint16_t delay_ms = term->conf->resize_delay_ms;
        bool successfully_scheduled = false;

        if (fd < 0) {
            /* Lazy create timer fd */
            fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            if (fd < 0)
                LOG_ERRNO("failed to create TIOCSWINSZ timer");
            else if (!fdm_add(term->fdm, fd, EPOLLIN, &fdm_tiocswinsz, term)) {
                close(fd);
                fd = -1;
            }

            win->resize_timeout_fd = fd;
        }

        if (fd >= 0) {
            /* Reset timeout */
            const struct itimerspec timeout = {
                .it_value = {.tv_sec = 0, .tv_nsec = delay_ms * 1000000},
            };

            if (timerfd_settime(fd, 0, &timeout, NULL) < 0) {
                LOG_ERRNO("failed to arm TIOCSWINSZ timer");
                fdm_del(term->fdm, fd);
                win->resize_timeout_fd = -1;
            } else
                successfully_scheduled = true;
        }

        if (!successfully_scheduled)
            tiocswinsz(term);
    }
}

/* Move to terminal.c? */
static bool
maybe_resize(struct terminal *term, int width, int height, bool force)
{
    if (term->shutdown.in_progress)
        return false;

    if (!term->window->is_configured)
        return false;

    if (term->cell_width == 0 && term->cell_height == 0)
        return false;

    int scale = -1;
    tll_foreach(term->window->on_outputs, it) {
        if (it->item->scale > scale)
            scale = it->item->scale;
    }

    if (scale < 0) {
        /* Haven't 'entered' an output yet? */
        scale = term->scale;
    }

    width *= scale;
    height *= scale;

    if (width == 0 && height == 0) {
        /*
         * The compositor is letting us choose the size
         *
         * If we have a "last" used size - use that. Otherwise, use
         * the size from the user configuration.
         */
        if (term->stashed_width != 0 && term->stashed_height != 0) {
            width = term->stashed_width;
            height = term->stashed_height;
        } else {
            switch (term->conf->size.type) {
            case CONF_SIZE_PX:
                width = term->conf->size.width;
                height = term->conf->size.height;

                /* Take CSDs into account */
                if (wayl_win_csd_titlebar_visible(term->window))
                    height -= term->conf->csd.title_height;

                if (wayl_win_csd_borders_visible(term->window)) {
                    height -= 2 * term->conf->csd.border_width_visible;
                    width -= 2 * term->conf->csd.border_width_visible;
                }

                width *= scale;
                height *= scale;
                break;

            case CONF_SIZE_CELLS:
                width = term->conf->size.width * term->cell_width;
                height = term->conf->size.height * term->cell_height;

                width += 2 * term->conf->pad_x * scale;
                height += 2 * term->conf->pad_y * scale;

                /*
                 * Ensure we can scale to logical size, and back to
                 * pixels without truncating.
                 */
                if (width % scale)
                    width += scale - width % scale;
                if (height % scale)
                    height += scale - height % scale;

                xassert(width % scale == 0);
                xassert(height % scale == 0);
                break;
            }
        }
    }

    /* Don't shrink grid too much */
    const int min_cols = 2;
    const int min_rows = 1;

    /* Minimum window size */
    const int min_width = min_cols * term->cell_width;
    const int min_height = min_rows * term->cell_height;

    width = max(width, min_width);
    height = max(height, min_height);

    /* Padding */
    const int max_pad_x = (width - min_width) / 2;
    const int max_pad_y = (height - min_height) / 2;
    const int pad_x = min(max_pad_x, scale * term->conf->pad_x);
    const int pad_y = min(max_pad_y, scale * term->conf->pad_y);

    if (!force && width == term->width && height == term->height && scale == term->scale)
        return false;

    /* Cancel an application initiated "Synchronized Update" */
    term_disable_app_sync_updates(term);

    /* Drop out of URL mode */
    urls_reset(term);

    term->width = width;
    term->height = height;
    term->scale = scale;

    const uint32_t scrollback_lines = term->render.scrollback_lines;

    /* Screen rows/cols before resize */
    const int old_cols = term->cols;
    const int old_rows = term->rows;

    /* Screen rows/cols after resize */
    const int new_cols = (term->width - 2 * pad_x) / term->cell_width;
    const int new_rows = (term->height - 2 * pad_y) / term->cell_height;

    /* Grid rows/cols after resize */
    const int new_normal_grid_rows = 1 << (32 - __builtin_clz(new_rows + scrollback_lines - 1));
    const int new_alt_grid_rows = 1 << (32  - __builtin_clz(new_rows));

    xassert(new_cols >= 1);
    xassert(new_rows >= 1);

    /* Margins */
    const int grid_width = new_cols * term->cell_width;
    const int grid_height = new_rows * term->cell_height;
    const int total_x_pad = term->width - grid_width;
    const int total_y_pad = term->height - grid_height;

    if (term->conf->center && !term->window->is_resizing) {
        term->margins.left = total_x_pad / 2;
        term->margins.top = total_y_pad / 2;
    } else {
        term->margins.left = pad_x;
        term->margins.top = pad_y;
    }
    term->margins.right = total_x_pad - term->margins.left;
    term->margins.bottom = total_y_pad - term->margins.top;

    xassert(term->margins.left >= pad_x);
    xassert(term->margins.right >= pad_x);
    xassert(term->margins.top >= pad_y);
    xassert(term->margins.bottom >= pad_y);

    if (new_cols == old_cols && new_rows == old_rows) {
        LOG_DBG("grid layout unaffected; skipping reflow");
        goto damage_view;
    }

    if (term->grid == &term->alt)
        selection_cancel(term);
    else {
        /*
         * Don’t cancel, but make sure there aren’t any ongoing
         * selections after the resize.
         */
        tll_foreach(term->wl->seats, it) {
            if (it->item.kbd_focus == term)
                selection_finalize(&it->item, term, it->item.pointer.serial);
        }
    }

    /*
     * TODO: if we remove the selection_finalize() call above (i.e. if
     * we start allowing selections to be ongoing across resizes), the
     * selection’s pivot point coordinates *must* be added to the
     * tracking points list.
     */
    struct coord *const tracking_points[] = {
        &term->selection.coords.start,
        &term->selection.coords.end,
    };

    /* Resize grids */
    grid_resize_and_reflow(
        &term->normal, new_normal_grid_rows, new_cols, old_rows, new_rows,
        term->selection.coords.end.row >= 0 ? ALEN(tracking_points) : 0,
        tracking_points);

    grid_resize_without_reflow(
        &term->alt, new_alt_grid_rows, new_cols, old_rows, new_rows);

    /* Reset tab stops */
    tll_free(term->tab_stops);
    for (int c = 0; c < new_cols; c += 8)
        tll_push_back(term->tab_stops, c);

    term->cols = new_cols;
    term->rows = new_rows;

    sixel_reflow(term);

#if defined(_DEBUG) && LOG_ENABLE_DBG
    LOG_DBG("resize: %dx%d, grid: cols=%d, rows=%d "
            "(left-margin=%d, right-margin=%d, top-margin=%d, bottom-margin=%d)",
            term->width, term->height, term->cols, term->rows,
            term->margins.left, term->margins.right, term->margins.top, term->margins.bottom);
#endif

    if (term->scroll_region.start >= term->rows)
        term->scroll_region.start = 0;

    if (term->scroll_region.end >= old_rows)
        term->scroll_region.end = term->rows;

    term->render.last_cursor.row = NULL;

damage_view:
    /* Signal TIOCSWINSZ */
    send_dimensions_to_client(term);

    if (!term->window->is_maximized &&
        !term->window->is_fullscreen &&
        !term->window->is_tiled)
    {
        /* Stash current size, to enable us to restore it when we're
         * being un-maximized/fullscreened/tiled */
        term->stashed_width = term->width;
        term->stashed_height = term->height;
    }

#if 0
    /* TODO: doesn't include CSD title bar */
    xdg_toplevel_set_min_size(
        term->window->xdg_toplevel, min_width / scale, min_height / scale);
#endif

    {
        const bool title_shown = wayl_win_csd_titlebar_visible(term->window);
        const bool border_shown = wayl_win_csd_borders_visible(term->window);

        const int title_height =
            title_shown ? term->conf->csd.title_height : 0;
        const int border_width =
            border_shown ? term->conf->csd.border_width_visible : 0;

        xdg_surface_set_window_geometry(
            term->window->xdg_surface,
            -border_width,
            -title_height - border_width,
            term->width / term->scale + 2 * border_width,
            term->height / term->scale + title_height + 2 * border_width);
    }

    tll_free(term->normal.scroll_damage);
    tll_free(term->alt.scroll_damage);

    shm_unref(term->render.last_buf);
    term->render.last_buf = NULL;
    term_damage_view(term);
    render_refresh_csd(term);
    render_refresh_search(term);
    render_refresh(term);

    return true;
}

bool
render_resize(struct terminal *term, int width, int height)
{
    return maybe_resize(term, width, height, false);
}

bool
render_resize_force(struct terminal *term, int width, int height)
{
    return maybe_resize(term, width, height, true);
}

static void xcursor_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);
static const struct wl_callback_listener xcursor_listener = {
    .done = &xcursor_callback,
};

bool
render_xcursor_is_valid(const struct seat *seat, const char *cursor)
{
    if (cursor == NULL)
        return false;
    return wl_cursor_theme_get_cursor(seat->pointer.theme, cursor) != NULL;
}

static void
render_xcursor_update(struct seat *seat)
{
    /* If called from a frame callback, we may no longer have mouse focus */
    if (!seat->mouse_focus)
        return;

    xassert(seat->pointer.xcursor != NULL);

    if (seat->pointer.xcursor == XCURSOR_HIDDEN) {
        /* Hide cursor */
        wl_surface_attach(seat->pointer.surface, NULL, 0, 0);
        wl_surface_commit(seat->pointer.surface);
        return;
    }

    xassert(seat->pointer.cursor != NULL);

    const int scale = seat->pointer.scale;
    struct wl_cursor_image *image = seat->pointer.cursor->images[0];

    wl_surface_attach(
        seat->pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        seat->wl_pointer, seat->pointer.serial,
        seat->pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        seat->pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_set_buffer_scale(seat->pointer.surface, scale);

    xassert(seat->pointer.xcursor_callback == NULL);
    seat->pointer.xcursor_callback = wl_surface_frame(seat->pointer.surface);
    wl_callback_add_listener(seat->pointer.xcursor_callback, &xcursor_listener, seat);

    wl_surface_commit(seat->pointer.surface);
}

static void
xcursor_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct seat *seat = data;

    xassert(seat->pointer.xcursor_callback == wl_callback);
    wl_callback_destroy(wl_callback);
    seat->pointer.xcursor_callback = NULL;

    if (seat->pointer.xcursor_pending) {
        render_xcursor_update(seat);
        seat->pointer.xcursor_pending = false;
    }
}

static void
fdm_hook_refresh_pending_terminals(struct fdm *fdm, void *data)
{
    struct renderer *renderer = data;
    struct wayland *wayl = renderer->wayl;

    tll_foreach(renderer->wayl->terms, it) {
        struct terminal *term = it->item;

        if (unlikely(term->shutdown.in_progress || !term->window->is_configured))
            continue;

        bool grid = term->render.refresh.grid;
        bool csd = term->render.refresh.csd;
        bool search = term->is_searching && term->render.refresh.search;
        bool urls = urls_mode_is_active(term) && term->render.refresh.urls;

        if (!(grid | csd | search | urls))
            continue;

        if (term->render.app_sync_updates.enabled && !(csd | search | urls))
            continue;

        term->render.refresh.grid = false;
        term->render.refresh.csd = false;
        term->render.refresh.search = false;
        term->render.refresh.urls = false;

        if (term->window->frame_callback == NULL) {
            struct grid *original_grid = term->grid;
            if (urls_mode_is_active(term)) {
                xassert(term->url_grid_snapshot != NULL);
                term->grid = term->url_grid_snapshot;
            }

            if (csd && term->window->csd_mode == CSD_YES) {
                quirk_weston_csd_on(term);
                render_csd(term);
                quirk_weston_csd_off(term);
            }
            if (search)
                render_search_box(term);
            if (urls)
                render_urls(term);
            if (grid | csd | search | urls)
                grid_render(term);

            tll_foreach(term->wl->seats, it) {
                if (it->item.ime_focus == term)
                    ime_update_cursor_rect(&it->item);
            }

            term->grid = original_grid;
        } else {
            /* Tells the frame callback to render again */
            term->render.pending.grid |= grid;
            term->render.pending.csd |= csd;
            term->render.pending.search |= search;
            term->render.pending.urls |= urls;
        }
    }

    tll_foreach(wayl->seats, it) {
        if (it->item.pointer.xcursor_pending) {
            if (it->item.pointer.xcursor_callback == NULL) {
                render_xcursor_update(&it->item);
                it->item.pointer.xcursor_pending = false;
            } else {
                /* Frame callback will call render_xcursor_update() */
            }
        }
    }
}

void
render_refresh_title(struct terminal *term)
{
    if (term->render.title.is_armed)
        return;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return;

    struct timespec diff;
    timespec_sub(&now, &term->render.title.last_update, &diff);

    if (diff.tv_sec == 0 && diff.tv_nsec < 8333 * 1000) {
        const struct itimerspec timeout = {
            .it_value = {.tv_nsec = 8333 * 1000 - diff.tv_nsec},
        };

        timerfd_settime(term->render.title.timer_fd, 0, &timeout, NULL);
    } else {
        term->render.title.last_update = now;
        render_update_title(term);
    }

    render_refresh_csd(term);
}

void
render_refresh(struct terminal *term)
{
    term->render.refresh.grid = true;
}

void
render_refresh_csd(struct terminal *term)
{
    if (term->window->csd_mode == CSD_YES)
        term->render.refresh.csd = true;
}

void
render_refresh_search(struct terminal *term)
{
    if (term->is_searching)
        term->render.refresh.search = true;
}

void
render_refresh_urls(struct terminal *term)
{
    if (urls_mode_is_active(term))
        term->render.refresh.urls = true;
}

bool
render_xcursor_set(struct seat *seat, struct terminal *term, const char *xcursor)
{
    if (seat->pointer.theme == NULL)
        return false;

    if (seat->mouse_focus == NULL) {
        seat->pointer.xcursor = NULL;
        return true;
    }

    if (seat->mouse_focus != term) {
        /* This terminal doesn't have mouse focus */
        return true;
    }

    if (seat->pointer.xcursor == xcursor)
        return true;

    if (xcursor != XCURSOR_HIDDEN) {
        seat->pointer.cursor = wl_cursor_theme_get_cursor(
            seat->pointer.theme, xcursor);

        if (seat->pointer.cursor == NULL) {
            seat->pointer.cursor = wl_cursor_theme_get_cursor(
                seat->pointer.theme, XCURSOR_TEXT_FALLBACK );
            if (seat->pointer.cursor == NULL) {
                LOG_ERR("failed to load xcursor pointer '%s', and fallback '%s'", xcursor, XCURSOR_TEXT_FALLBACK);
                return false;
            }
        }
    } else
        seat->pointer.cursor = NULL;

    /* FDM hook takes care of actual rendering */
    seat->pointer.xcursor = xcursor;
    seat->pointer.xcursor_pending = true;
    return true;
}
