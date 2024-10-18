#include "render.h"

#include <limits.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

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

#include <presentation-time.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>

#if defined(HAVE_XDG_TOPLEVEL_ICON)
#include <xdg-toplevel-icon-v1.h>
#endif

#include <fcft/fcft.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "box-drawing.h"
#include "char32.h"
#include "config.h"
#include "cursor-shape.h"
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
            /* "Regular" color, return the corresponding "dim" */
            return conf->colors.dim[i];
        }

        else if (term->colors.table[8 + i] == color) {
            /* "Bright" color, return the corresponding "regular" */
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

    lum = (int)roundf(lum * term->conf->bold_in_bright.amount);
    return hsl_to_rgb(hue, sat, min(lum, 100));
}

static void
draw_hollow_block(const struct terminal *term, pixman_image_t *pix,
                  const pixman_color_t *color, int x, int y, int cell_cols)
{
    const int scale = (int)roundf(term->scale);
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
    int baseline = y + term->font_baseline - term->fonts[0]->ascent;
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
    return term->font_baseline -
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
    const int thickness = term->conf->underline_thickness.px >= 0
        ? term_pt_or_px_as_pixels(
            term, &term->conf->underline_thickness)
        : font->underline.thickness;

    /* Make sure the line isn't positioned below the cell */
    const int y_ofs = min(underline_offset(term, font),
                          term->cell_height - thickness);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + y_ofs, cols * term->cell_width, thickness});
}

static void
draw_styled_underline(const struct terminal *term, pixman_image_t *pix,
                      const struct fcft_font *font,
                      const pixman_color_t *color,
                      enum underline_style style, int x, int y, int cols)
{
    xassert(style != UNDERLINE_NONE);

    if (style == UNDERLINE_SINGLE) {
        draw_underline(term, pix, font, color, x, y, cols);
        return;
    }

    const int thickness = term->conf->underline_thickness.px >= 0
        ? term_pt_or_px_as_pixels(
            term, &term->conf->underline_thickness)
        : font->underline.thickness;

    int y_ofs;

    /* Make sure the line isn't positioned below the cell */
    switch (style) {
    case UNDERLINE_DOUBLE:
    case UNDERLINE_CURLY:
        y_ofs = min(underline_offset(term, font),
                    term->cell_height - thickness * 3);
        break;

    case UNDERLINE_DASHED:
    case UNDERLINE_DOTTED:
        y_ofs = min(underline_offset(term, font),
                    term->cell_height - thickness);
        break;

    case UNDERLINE_NONE:
    case UNDERLINE_SINGLE:
    default:
        BUG("unexpected underline style: %d", (int)style);
        return;
    }

    const int ceil_w = cols * term->cell_width;

    switch (style) {
    case UNDERLINE_DOUBLE: {
        const pixman_rectangle16_t rects[] = {
            {x, y + y_ofs, ceil_w, thickness},
            {x, y + y_ofs + thickness * 2, ceil_w, thickness}};
        pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, color, 2, rects);
        break;
    }

    case UNDERLINE_DASHED: {
        const int ceil_w = cols * term->cell_width;
        const int dash_w = ceil_w / 3 + (ceil_w % 3 > 0);
        const pixman_rectangle16_t rects[] = {
            {x, y + y_ofs, dash_w, thickness},
            {x + dash_w * 2, y + y_ofs, dash_w, thickness},
        };
        pixman_image_fill_rectangles(
            PIXMAN_OP_SRC, pix, color, 2, rects);
        break;
    }

    case UNDERLINE_DOTTED: {
        /* Number of dots per cell */
        int per_cell = (term->cell_width / thickness) / 2;
        if (per_cell == 0)
            per_cell = 1;

        xassert(per_cell >= 1);

        /* Spacing between dots; start with the same width as the dots
           themselves, then widen them if necessary, to consume unused
           pixels */
        int spacing[per_cell];
        for (int i = 0; i < per_cell; i++)
            spacing[i] = thickness;

        /* Pixels remaining at the end of the cell */
        int remaining = term->cell_width - (per_cell * 2) * thickness;

        /* Spread out the left-over pixels across the spacing between
           the dots */
        for (int i = 0; remaining > 0; i = (i + 1) % per_cell, remaining--)
            spacing[i]++;

        xassert(remaining <= 0);

        pixman_rectangle16_t rects[per_cell];
        int dot_x = x;
        for (int i = 0; i < per_cell; i++) {
            rects[i] = (pixman_rectangle16_t){
                dot_x, y + y_ofs, thickness, thickness
            };

            dot_x += thickness + spacing[i];
        }

        pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, color, per_cell, rects);
        break;
    }

    case UNDERLINE_CURLY: {
        const int top = y + y_ofs;
        const int bot = top + thickness * 3;
        const int half_x = x + ceil_w / 2.0, full_x = x + ceil_w;

        const double bt_2 = (bot - top) * (bot - top);
        const double th_2 = thickness * thickness;
        const double hx_2 = ceil_w * ceil_w / 4.0;
        const int th = round(sqrt(th_2 + (th_2 * bt_2 / hx_2)) / 2.);

        #define I(x) pixman_int_to_fixed(x)
        const pixman_trapezoid_t traps[] = {
#if 0  /* characters sit within the "dips" of the curlies */
            {
                I(top), I(bot),
                {{I(x), I(top + th)}, {I(half_x), I(bot + th)}},
                {{I(x), I(top - th)}, {I(half_x), I(bot - th)}},
            },
            {
                I(top), I(bot),
                {{I(half_x), I(bot - th)}, {I(full_x), I(top - th)}},
                {{I(half_x), I(bot + th)}, {I(full_x), I(top + th)}},
            }
#else  /* characters sit on top of the curlies */
            {
                I(top), I(bot),
                {{I(x), I(bot - th)}, {I(half_x), I(top - th)}},
                {{I(x), I(bot + th)}, {I(half_x), I(top + th)}},
            },
            {
                I(top), I(bot),
                {{I(half_x), I(top + th)}, {I(full_x), I(bot + th)}},
                {{I(half_x), I(top - th)}, {I(full_x), I(bot - th)}},
            }
#endif
        };

        pixman_image_t *fill = pixman_image_create_solid_fill(color);
        pixman_composite_trapezoids(
            PIXMAN_OP_OVER, fill, pix, PIXMAN_a8, 0, 0, 0, 0,
            sizeof(traps) / sizeof(traps[0]), traps);

        pixman_image_unref(fill);
        break;
    }

    case UNDERLINE_NONE:
    case UNDERLINE_SINGLE:
        BUG("underline styles not supposed to be handled here");
        break;
    }
}

static void
draw_strikeout(const struct terminal *term, pixman_image_t *pix,
               const struct fcft_font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    const int thickness = term->conf->strikeout_thickness.px >= 0
        ? term_pt_or_px_as_pixels(
            term, &term->conf->strikeout_thickness)
        : font->strikeout.thickness;

    /* Try to center custom strikeout */
    const int position = term->conf->strikeout_thickness.px >= 0
        ? font->strikeout.position - round(font->strikeout.thickness / 2.) + round(thickness / 2.)
        : font->strikeout.position;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + term->font_baseline - position,
            cols * term->cell_width, thickness});
}

static void
cursor_colors_for_cell(const struct terminal *term, const struct cell *cell,
              const pixman_color_t *fg, const pixman_color_t *bg,
              pixman_color_t *cursor_color, pixman_color_t *text_color)
{
    if (term->colors.cursor_bg >> 31)
        *cursor_color = color_hex_to_pixman(term->colors.cursor_bg);
    else
        *cursor_color = *fg;

    if (term->colors.cursor_fg >> 31)
        *text_color = color_hex_to_pixman(term->colors.cursor_fg);
    else {
        *text_color = *bg;

        if (unlikely(text_color->alpha != 0xffff)) {
            /* The *only* color that can have transparency is the
             * default background color */
            *text_color = color_hex_to_pixman(term->colors.bg);
        }
    }

    if (text_color->red == cursor_color->red &&
        text_color->green == cursor_color->green &&
        text_color->blue == cursor_color->blue)
    {
        *text_color = color_hex_to_pixman(term->colors.bg);
        *cursor_color = color_hex_to_pixman(term->colors.fg);
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

    if (unlikely(!term->kbd_focus)) {
        switch (term->conf->cursor.unfocused_style) {
        case CURSOR_UNFOCUSED_UNCHANGED:
            break;

        case CURSOR_UNFOCUSED_HOLLOW:
            draw_hollow_block(term, pix, &cursor_color, x, y, cols);
            return;

        case CURSOR_UNFOCUSED_NONE:
            return;
        }
    }

    switch (term->cursor_style) {
    case CURSOR_BLOCK:
        if (likely(term->cursor_blink.state == CURSOR_BLINK_ON) ||
            !term->kbd_focus)
        {
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
render_cell(struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage,
            struct row *row, int row_no, int col, bool has_cursor)
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
        }

        else if (cell->attrs.bg_src == COLOR_DEFAULT) {
            if (term->window->is_fullscreen) {
                /*
                 * Note: disable transparency when fullscreened.
                 *
                 * This is because the wayland protocol mandates no
                 * screen content is shown behind the fullscreened
                 * window.
                 *
                 * The _intent_ of the specification is that a black
                 * (or other static color) should be used as
                 * background.
                 *
                 * There's a bit of gray area however, and some
                 * compositors have chosen to interpret the
                 * specification in a way that allows wallpapers to be
                 * seen through a fullscreen window.
                 *
                 * Given that a) the intent of the specification, and
                 * b) we don't know what the compositor will do, we
                 * simply disable transparency while in fullscreen.
                 *
                 * To see why, consider what happens if we keep our
                 * transparency. For example, if the background color
                 * is white, and alpha is 0.5, then the window will be
                 * drawn in a shade of gray while fullscreened.
                 *
                 * See
                 * https://gitlab.freedesktop.org/wayland/wayland-protocols/-/issues/116
                 * for a discussion on whether transparent, fullscreen
                 * windows should be allowed in some way or not.
                 *
                 * NOTE: if changing this, also update render_margin()
                 */
                xassert(alpha == 0xffff);
            } else {
                alpha = term->colors.alpha;
            }
        }
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
            if (unlikely(base >= CELL_SPACER)) {
                glyph_count = 0;
                cell_cols = 1;
            } else {
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

    if (damage != NULL) {
        pixman_region32_union_rect(
            damage, damage, x, y, render_width, term->cell_height);
    }

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

    if (unlikely(has_cursor && term->cursor_style == CURSOR_BLOCK && term->kbd_focus))
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
                    pen_x + letter_x_ofs + g_x, y + term->font_baseline - g_y,
                    glyph->width, glyph->height);
            }
        } else {
            pixman_image_composite32(
                PIXMAN_OP_OVER, clr_pix, glyph->pix, pix, 0, 0, 0, 0,
                pen_x + letter_x_ofs + g_x, y + term->font_baseline - g_y,
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
                        y + term->font_baseline - g->y,
                        g->width, g->height);
                }
            }
        }

        pen_x += glyph->advance.x;
    }

    pixman_image_unref(clr_pix);

    /* Underline */
    if (cell->attrs.underline) {
        pixman_color_t underline_color = fg;
        enum underline_style underline_style = UNDERLINE_SINGLE;

        /* Check if cell has a styled underline. This lookup is fairly
           expensive... */
        if (row->extra != NULL) {
            for (int i = 0; i < row->extra->underline_ranges.count; i++) {
                const struct row_range *range = &row->extra->underline_ranges.v[i];

                if (range->start > col)
                    break;

                if (range->start <= col && col <= range->end) {
                    switch (range->underline.color_src) {
                    case COLOR_BASE256:
                        underline_color = color_hex_to_pixman(
                            term->colors.table[range->underline.color]);
                        break;

                    case COLOR_RGB:
                        underline_color =
                            color_hex_to_pixman(range->underline.color);
                        break;

                    case COLOR_DEFAULT:
                        break;

                    case COLOR_BASE16:
                        BUG("underline color can't be base-16");
                        break;
                    }

                    underline_style = range->underline.style;
                    break;
                }
            }
        }

        draw_styled_underline(
            term, pix, font, &underline_color, underline_style, x, y, cell_cols);

    }

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
render_row(struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage,
           struct row *row, int row_no, int cursor_col)
{
    for (int col = term->cols - 1; col >= 0; col--)
        render_cell(term, pix, damage, row, row_no, col, cursor_col == col);
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

    const uint32_t _bg = !term->reverse ? term->colors.bg : term->colors.fg;
    uint16_t alpha = term->colors.alpha;

    if (term->window->is_fullscreen) {
        /* Disable alpha in fullscreen - see render_cell() for details */
        alpha = 0xffff;
    }

    pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, alpha);

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
        &buf->dirty[0], &buf->dirty[0], 0, 0, term->width, term->margins.top);
    pixman_region32_union_rect(
        &buf->dirty[0], &buf->dirty[0], 0, bmargin, term->width, term->margins.bottom);
    pixman_region32_union_rect(
        &buf->dirty[0], &buf->dirty[0], 0, 0, term->margins.left, term->height);
    pixman_region32_union_rect(
        &buf->dirty[0], &buf->dirty[0],
        rmargin, 0, term->margins.right, term->height);

    if (apply_damage) {
        /* Top */
        wl_surface_damage_buffer(
            term->window->surface.surf, 0, 0, term->width, term->margins.top);

        /* Bottom */
        wl_surface_damage_buffer(
            term->window->surface.surf, 0, bmargin, term->width, term->margins.bottom);

        /* Left */
        wl_surface_damage_buffer(
            term->window->surface.surf,
            0, term->margins.top + start_line * term->cell_height,
            term->margins.left, line_count * term->cell_height);

        /* Right */
        wl_surface_damage_buffer(
            term->window->surface.surf,
            rmargin, term->margins.top + start_line * term->cell_height,
            term->margins.right, line_count * term->cell_height);
    }
}

static void
grid_render_scroll(struct terminal *term, struct buffer *buf,
                   const struct damage *dmg)
{
    LOG_DBG(
        "damage: SCROLL: %d-%d by %d lines",
        dmg->region.start, dmg->region.end, dmg->lines);

    const int region_size = dmg->region.end - dmg->region.start;

    if (dmg->lines >= region_size) {
        /* The entire scroll region will be scrolled out (i.e. replaced) */
        return;
    }

    const int height = (region_size - dmg->lines) * term->cell_height;
    xassert(height > 0);

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
        term->window->surface.surf, term->margins.left, dst_y,
        term->width - term->margins.left - term->margins.right, height);

    /*
     * TODO: remove this if re-enabling scroll damage when re-applying
     * last frame's damage (see reapply_old_damage()
     */
    pixman_region32_union_rect(
        &buf->dirty[0], &buf->dirty[0], 0, dst_y, buf->width, height);
}

static void
grid_render_scroll_reverse(struct terminal *term, struct buffer *buf,
                           const struct damage *dmg)
{
    LOG_DBG(
        "damage: SCROLL REVERSE: %d-%d by %d lines",
        dmg->region.start, dmg->region.end, dmg->lines);

    const int region_size = dmg->region.end - dmg->region.start;

    if (dmg->lines >= region_size) {
        /* The entire scroll region will be scrolled out (i.e. replaced) */
        return;
    }

    const int height = (region_size - dmg->lines) * term->cell_height;
    xassert(height > 0);

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
        term->window->surface.surf, term->margins.left, dst_y,
        term->width - term->margins.left - term->margins.right, height);

    /*
     * TODO: remove this if re-enabling scroll damage when re-applying
     * last frame's damage (see reapply_old_damage()
     */
    pixman_region32_union_rect(
        &buf->dirty[0], &buf->dirty[0], 0, dst_y, buf->width, height);
}

static void
render_sixel_chunk(struct terminal *term, pixman_image_t *pix,
                   pixman_region32_t *damage, const struct sixel *sixel,
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

    if (damage != NULL)
        pixman_region32_union_rect(damage, damage, x, y, width, height);
}

static void
render_sixel(struct terminal *term, pixman_image_t *pix,
             pixman_region32_t *damage, const struct coord *cursor,
             const struct sixel *sixel)
{
    xassert(sixel->pix != NULL);
    xassert(sixel->width >= 0);
    xassert(sixel->height >= 0);

    const int view_end = (term->grid->view + term->rows - 1) & (term->grid->num_rows - 1);
    const bool last_row_needs_erase = sixel->height % term->cell_height != 0;
    const bool last_col_needs_erase = sixel->width % term->cell_width != 0;

    int chunk_img_start = -1;  /* Image-relative start row of chunk */
    int chunk_term_start = -1; /* Viewport relative start row of chunk */
    int chunk_row_count = 0;   /* Number of rows to emit */

#define maybe_emit_sixel_chunk_then_reset()                             \
    if (chunk_row_count != 0) {                                         \
        render_sixel_chunk(                                             \
            term, pix, damage, sixel,                                   \
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
         * In all cases, do *not* clear the 'dirty' bit on the row, to
         * ensure the regular renderer includes them in the damage
         * rect.
         */
        if (!sixel->opaque) {
            /* TODO: multithreading */
            render_row(term, pix, damage, row, term_row_no, cursor_col);
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
                        render_cell(term, pix, damage, row, term_row_no, col, cursor_col == col);
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
                    pixman_region32_t *damage,
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

        sixel_sync_cache(term, &it->item);
        render_sixel(term, pix, damage, cursor, &it->item);
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
         * one extra cell from the original grid, or we'll leave
         * trailing "cursors" after us if the user deletes text while
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
    struct cell *real_cells = xmalloc(cells_used * sizeof(real_cells[0]));
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
        render_cell(term, buf->pix[0], NULL, row, row_idx, col_idx + i, false);
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
                draw_hollow_block(term, buf->pix[0], &cursor_color, x, y, cols);
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
        term->window->surface.surf,
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
render_overlay_single_pixel(struct terminal *term, enum overlay_style style,
                            pixman_color_t color)
{
    struct wayland *wayl = term->wl;
    struct wayl_sub_surface *overlay = &term->window->overlay;
    struct wl_buffer *buf = NULL;

    /*
     * In an ideal world, we'd only update the surface (i.e. commit
     * any changes) if anything has actually changed.
     *
     * For technical reasons, we can't do that, since we can't
     * determine whether the last committed buffer is still valid
     * (i.e. does it correspond to the current overlay style, *and*
     * does last frame's size match the current size?)
     *
     * What we _can_ do is use the fact that single-pixel buffers
     * don't have a size; you have to use a viewport to "size" them.
     *
     * This means we can check if the last frame's overlay style is
     * the same as the current size. If so, then we *know* that the
     * currently attached buffer is valid, and we *don't* have to
     * create a new single-pixel buffer.
     *
     * What we do *not* know if the *size* is still valid. This means
     * we do have to do the viewport calls, and a surface commit.
     *
     * This is still better than *always* creating a new buffer.
     */

    assert(style == OVERLAY_UNICODE_MODE || style == OVERLAY_FLASH);
    assert(wayl->single_pixel_manager != NULL);
    assert(overlay->surface.viewport != NULL);

    quirk_weston_subsurface_desync_on(overlay->sub);

    if (style != term->render.last_overlay_style) {
        buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            wayl->single_pixel_manager,
            (double)color.red / 0xffff * 0xffffffff,
            (double)color.green / 0xffff * 0xffffffff,
            (double)color.blue / 0xffff * 0xffffffff,
            (double)color.alpha / 0xffff * 0xffffffff);

        wl_surface_set_buffer_scale(overlay->surface.surf, 1);
        wl_surface_attach(overlay->surface.surf, buf, 0, 0);
    }

    wp_viewport_set_destination(
        overlay->surface.viewport,
        roundf(term->width / term->scale),
        roundf(term->height / term->scale));

    wl_subsurface_set_position(overlay->sub, 0, 0);

    wl_surface_damage_buffer(
        overlay->surface.surf, 0, 0, term->width, term->height);

    wl_surface_commit(overlay->surface.surf);
    quirk_weston_subsurface_desync_off(overlay->sub);

    term->render.last_overlay_style = style;

    if (buf != NULL) {
        wl_buffer_destroy(buf);
    }
}

static void
render_overlay(struct terminal *term)
{
    struct wayl_sub_surface *overlay = &term->window->overlay;
    const bool unicode_mode_active = term->unicode_mode.active;

    const enum overlay_style style =
        term->is_searching ? OVERLAY_SEARCH :
        term->flash.active ? OVERLAY_FLASH :
        unicode_mode_active ? OVERLAY_UNICODE_MODE :
        OVERLAY_NONE;

    if (likely(style == OVERLAY_NONE)) {
        if (term->render.last_overlay_style != OVERLAY_NONE) {
            /* Unmap overlay sub-surface */
            wl_surface_attach(overlay->surface.surf, NULL, 0, 0);
            wl_surface_commit(overlay->surface.surf);
            term->render.last_overlay_style = OVERLAY_NONE;
            term->render.last_overlay_buf = NULL;

            /* Work around Sway bug - unmapping a sub-surface does not
             * damage the underlying surface */
            quirk_sway_subsurface_unmap(term);
        }
        return;
    }

    pixman_color_t color;

    switch (style) {
    case OVERLAY_SEARCH:
    case OVERLAY_UNICODE_MODE:
        color = (pixman_color_t){0, 0, 0, 0x7fff};
        break;

    case OVERLAY_FLASH:
        color = color_hex_to_pixman_with_alpha(
                term->conf->colors.flash,
                term->conf->colors.flash_alpha);
        break;

    case OVERLAY_NONE:
        xassert(false);
        break;
    }

    const bool single_pixel =
        (style == OVERLAY_UNICODE_MODE || style == OVERLAY_FLASH) &&
        term->wl->single_pixel_manager != NULL &&
        overlay->surface.viewport != NULL;

    if (single_pixel) {
        render_overlay_single_pixel(term, style, color);
        return;
    }

    struct buffer *buf = shm_get_buffer(
        term->render.chains.overlay, term->width, term->height, true);
    pixman_image_set_clip_region32(buf->pix[0], NULL);

    /* Bounding rectangle of damaged areas - for wl_surface_damage_buffer() */
    pixman_box32_t damage_bounds;

    if (style == OVERLAY_SEARCH) {
        /*
         * When possible, we only update the areas that have *changed*
         * since the last frame. That means:
         *
         *  - clearing/erasing cells that are now selected, but weren't
         *    in the last frame
         *  - dimming cells that were selected, but aren't anymore
         *
         * To do this, we save the last frame's selected cells as a
         * pixman region.
         *
         * Then, we calculate the corresponding region for this
         * frame's selected cells.
         *
         * Last frame's region minus this frame's region gives us the
         * region that needs to be *dimmed* in this frame
         *
         * This frame's region minus last frame's region gives us the
         * region that needs to be *cleared* in this frame.
         *
         * Finally, the union of the two "diff" regions above, gives
         * us the total region affected by a change, in either way. We
         * use this as the bounding box for the
         * wl_surface_damage_buffer() call.
         */
        pixman_region32_t *see_through = &term->render.last_overlay_clip;
        pixman_region32_t old_see_through;
        const bool buffer_reuse =
            buf == term->render.last_overlay_buf &&
            style == term->render.last_overlay_style &&
            buf->age == 0;

        if (!buffer_reuse) {
            /* Can't reuse last frame's damage - set to full window,
             * to ensure *everything* is updated */
            pixman_region32_init_rect(
                &old_see_through, 0, 0, buf->width, buf->height);
        } else {
            /* Use last frame's saved region */
            pixman_region32_init(&old_see_through);
            pixman_region32_copy(&old_see_through, see_through);
        }

        pixman_region32_clear(see_through);

        /* Build region consisting of all current search matches */
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

        /* Areas that need to be cleared: cells that were dimmed in
         * the last frame but is now see-through */
        pixman_region32_t new_see_through;
        pixman_region32_init(&new_see_through);

        if (buffer_reuse)
            pixman_region32_subtract(&new_see_through, see_through, &old_see_through);
        else {
            /* Buffer content is unknown - explicitly clear *all*
             * current see-through areas */
            pixman_region32_copy(&new_see_through, see_through);
        }
        pixman_image_set_clip_region32(buf->pix[0], &new_see_through);

        /* Areas that need to be dimmed: cells that were cleared in
         * the last frame but is not anymore */
        pixman_region32_t new_dimmed;
        pixman_region32_init(&new_dimmed);
        pixman_region32_subtract(&new_dimmed, &old_see_through, see_through);
        pixman_region32_fini(&old_see_through);

        /* Total affected area */
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
        xassert(style == OVERLAY_FLASH || style == OVERLAY_UNICODE_MODE);
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
    wayl_surface_scale(
        term->window, &overlay->surface, buf, term->scale);
    wl_subsurface_set_position(overlay->sub, 0, 0);
    wl_surface_attach(overlay->surface.surf, buf->wl_buf, 0, 0);

    wl_surface_damage_buffer(
        overlay->surface.surf,
        damage_bounds.x1, damage_bounds.y1,
        damage_bounds.x2 - damage_bounds.x1,
        damage_bounds.y2 - damage_bounds.y1);

    wl_surface_commit(overlay->surface.surf);
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

                render_row(term, buf->pix[my_id], &buf->dirty[my_id],
                           row, row_no, cursor_col);
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

    const float scale = term->scale;

    const int border_width = borders_visible
        ? roundf(term->conf->csd.border_width * scale) : 0;

    const int title_height = title_visible
        ? roundf(term->conf->csd.title_height * scale) : 0;

    const int button_width = title_visible
        ? roundf(term->conf->csd.button_width * scale) : 0;

    const int button_close_width = term->width >= 1 * button_width
        ? button_width : 0;

    const int button_maximize_width =
        term->width >= 2 * button_width && term->window->wm_capabilities.maximize
            ? button_width : 0;

    const int button_minimize_width =
        term->width >= 3 * button_width && term->window->wm_capabilities.minimize
            ? button_width : 0;

    /*
     * With fractional scaling, we must ensure the offset, when
     * divided by the scale (in set_position()), and the scaled back
     * (by the compositor), matches the actual pixel count made up by
     * the titlebar and the border.
     */
    const int top_offset = roundf(
        scale * (roundf(-title_height / scale) - roundf(border_width / scale)));

    const int top_bottom_width = roundf(
        scale * (roundf(term->width / scale) + 2 * roundf(border_width / scale)));

    const int left_right_height = roundf(
        scale * (roundf(title_height / scale) + roundf(term->height / scale)));

    switch (surf_idx) {
    case CSD_SURF_TITLE:  return (struct csd_data){            0, -title_height,      term->width,      title_height};
    case CSD_SURF_LEFT:   return (struct csd_data){-border_width, -title_height,     border_width, left_right_height};
    case CSD_SURF_RIGHT:  return (struct csd_data){  term->width, -title_height,     border_width, left_right_height};
    case CSD_SURF_TOP:    return (struct csd_data){-border_width,    top_offset, top_bottom_width,      border_width};
    case CSD_SURF_BOTTOM: return (struct csd_data){-border_width,  term->height, top_bottom_width,      border_width};

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
csd_commit(struct terminal *term, struct wayl_surface *surf, struct buffer *buf)
{
    wayl_surface_scale(term->window, surf, buf, term->scale);
    wl_surface_attach(surf->surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(surf->surf, 0, 0, buf->width, buf->height);
    wl_surface_commit(surf->surf);
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
render_osd(struct terminal *term, const struct wayl_sub_surface *sub_surf,
           struct fcft_font *font, struct buffer *buf,
           const char32_t *text, uint32_t _fg, uint32_t _bg,
           unsigned x)
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

    /* Calculate baseline  */
    unsigned y;
    {
        const int line_height = buf->height;
        const int font_height = max(font->height, font->ascent + font->descent);
        const int glyph_top_y = round((line_height - font_height) / 2.);
        y = term->font_y_ofs + glyph_top_y + font->ascent;
    }

    for (size_t i = 0; i < glyph_count; i++) {
        const struct fcft_glyph *glyph = glyphs[i];

        if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, buf->pix[0], 0, 0, 0, 0,
                x + x_ofs + glyph->x, y - glyph->y,
                glyph->width, glyph->height);
        } else {
            pixman_image_composite32(
                PIXMAN_OP_OVER, src, glyph->pix, buf->pix[0], 0, 0, 0, 0,
                x + x_ofs + glyph->x, y - glyph->y,
                glyph->width, glyph->height);
        }

        x += glyph->advance.x;
    }

    fcft_text_run_destroy(text_run);
    pixman_image_unref(src);
    pixman_image_set_clip_region32(buf->pix[0], NULL);

    quirk_weston_subsurface_desync_on(sub_surf->sub);
    wayl_surface_scale(term->window, &sub_surf->surface, buf, term->scale);
    wl_surface_attach(sub_surf->surface.surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(sub_surf->surface.surf, 0, 0, buf->width, buf->height);

    if (alpha == 0xffff) {
        struct wl_region *region = wl_compositor_create_region(term->wl->compositor);
        if (region != NULL) {
            wl_region_add(region, 0, 0, buf->width, buf->height);
            wl_surface_set_opaque_region(sub_surf->surface.surf, region);
            wl_region_destroy(region);
        }
    } else
        wl_surface_set_opaque_region(sub_surf->surface.surf, NULL);

    wl_surface_commit(sub_surf->surface.surf);
    quirk_weston_subsurface_desync_off(sub_surf->sub);
}

static void
render_csd_title(struct terminal *term, const struct csd_data *info,
                 struct buffer *buf)
{
    xassert(term->window->csd_mode == CSD_YES);

    struct wayl_sub_surface *surf = &term->window->csd.surface[CSD_SURF_TITLE];
    if (info->width == 0 || info->height == 0)
        return;

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

    render_osd(term, surf, win->csd.font, buf, title_text, fg, bg, margin);
    csd_commit(term, &surf->surface, buf);
    free(_title_text);
}

static void
render_csd_border(struct terminal *term, enum csd_surface surf_idx,
                  const struct csd_data *info, struct buffer *buf)
{
    xassert(term->window->csd_mode == CSD_YES);
    xassert(surf_idx >= CSD_SURF_LEFT && surf_idx <= CSD_SURF_BOTTOM);

    struct wayl_surface *surf = &term->window->csd.surface[surf_idx].surface;

    if (info->width == 0 || info->height == 0)
        return;

    {
        pixman_color_t color = color_hex_to_pixman_with_alpha(0, 0);
        render_csd_part(term, surf->surf, buf, info->width, info->height, &color);
    }

    /*
     * The "visible" border.
     */

    float scale = term->scale;
    int bwidth = (int)roundf(term->conf->csd.border_width * scale);
    int vwidth = (int)roundf(term->conf->csd.border_width_visible * scale); /* Visible size */

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
        PIXMAN_OP_SRC, buf->pix[0], &color, 1,
        (pixman_rectangle16_t[]) {
            {x_margin, y_margin + width - thick, width, thick}
    });

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
    const int shrink = 1;
    xassert(x_margin + width - thick >= 0);
    xassert(width - 2 * thick >= 0);
    xassert(y_margin + width - thick >= 0);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix[0], &color, 4,
        (pixman_rectangle16_t[]){
            {x_margin + shrink, y_margin + shrink, width - 2 * shrink, thick},
            { x_margin + shrink, y_margin + thick, thick, width - 2 * thick - shrink },
            { x_margin + width - thick - shrink, y_margin + thick, thick, width - 2 * thick - shrink },
            { x_margin + shrink, y_margin + width - thick - shrink, width - 2 * shrink, thick }});

    pixman_image_unref(src);

}

static void
render_csd_button_maximize_window(
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
        (pixman_rectangle16_t[]) {
            {x_margin, y_margin, width, thick},
            { x_margin, y_margin + thick, thick, width - 2 * thick },
            { x_margin + width - thick, y_margin + thick, thick, width - 2 * thick },
            { x_margin, y_margin + width - thick, width, thick }
    });

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
    int thick = min(width / 2, 1 * term->scale);
    const int x_margin = (buf->width - width) / 2;
    const int y_margin = (buf->height - width) / 2;

    xassert(x_margin + width - thick >= 0);
    xassert(width - 2 * thick >= 0);
    xassert(y_margin + width - thick >= 0);

    pixman_triangle_t tri[4] = {
        {
            .p1 = {
                .x = pixman_int_to_fixed(x_margin),
                .y = pixman_int_to_fixed(y_margin + thick),
            },
            .p2 = {
                .x = pixman_int_to_fixed(x_margin + width - thick),
                .y = pixman_int_to_fixed(y_margin + width),
            },
            .p3 = {
                .x = pixman_int_to_fixed(x_margin + thick),
                .y = pixman_int_to_fixed(y_margin),
            },
        },

        {
            .p1 = {
                .x = pixman_int_to_fixed(x_margin + width),
                .y = pixman_int_to_fixed(y_margin + width - thick),
            },
            .p2 = {
                .x = pixman_int_to_fixed(x_margin + thick),
                .y = pixman_int_to_fixed(y_margin),
            },
            .p3 = {
                .x = pixman_int_to_fixed(x_margin + width - thick),
                .y = pixman_int_to_fixed(y_margin + width),
            },
        },

        {
            .p1 = {
                .x = pixman_int_to_fixed(x_margin),
                .y = pixman_int_to_fixed(y_margin + width - thick),
            },
            .p2 = {
                .x = pixman_int_to_fixed(x_margin + width),
                .y = pixman_int_to_fixed(y_margin + thick),
            },
            .p3 = {
                .x = pixman_int_to_fixed(x_margin + thick),
                .y = pixman_int_to_fixed(y_margin + width),
            },
        },

        {
            .p1 = {
                .x = pixman_int_to_fixed(x_margin + width),
                .y = pixman_int_to_fixed(y_margin + thick),
            },
            .p2 = {
                .x = pixman_int_to_fixed(x_margin),
                .y = pixman_int_to_fixed(y_margin + width - thick),
            },
            .p3 = {
                .x = pixman_int_to_fixed(x_margin + width - thick),
                .y = pixman_int_to_fixed(y_margin),
            },
        },
    };

    pixman_composite_triangles(
        PIXMAN_OP_OVER, src, buf->pix[0], PIXMAN_a1,
        0, 0, 0, 0, 4, tri);

    pixman_image_unref(src);
}

static bool
any_pointer_is_on_button(const struct terminal *term, enum csd_surface csd_surface)
{
    if (unlikely(tll_length(term->wl->seats) == 0))
        return false;

    tll_foreach(term->wl->seats, it) {
        const struct seat *seat = &it->item;

        if (seat->mouse.x < 0)
            continue;
        if (seat->mouse.y < 0)
            continue;

        struct csd_data info = get_csd_data(term, csd_surface);
        if (seat->mouse.x > info.width)
            continue;

        if (seat->mouse.y > info.height)
            continue;
        return true;
    }

    return false;
}

static void
render_csd_button(struct terminal *term, enum csd_surface surf_idx,
                  const struct csd_data *info, struct buffer *buf)
{
    xassert(term->window->csd_mode == CSD_YES);
    xassert(surf_idx >= CSD_SURF_MINIMIZE && surf_idx <= CSD_SURF_CLOSE);

    struct wayl_surface *surf = &term->window->csd.surface[surf_idx].surface;

    if (info->width == 0 || info->height == 0)
        return;

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
        is_active = term->active_surface == TERM_SURF_BUTTON_MINIMIZE &&
                    any_pointer_is_on_button(term, CSD_SURF_MINIMIZE);
        break;

    case CSD_SURF_MAXIMIZE:
        _color = term->conf->colors.table[2];  /* green */
        is_set = term->conf->csd.color.maximize_set;
        conf_color = &term->conf->csd.color.maximize;
        is_active = term->active_surface == TERM_SURF_BUTTON_MAXIMIZE &&
                    any_pointer_is_on_button(term, CSD_SURF_MAXIMIZE);
        break;

    case CSD_SURF_CLOSE:
        _color = term->conf->colors.table[1];  /* red */
        is_set = term->conf->csd.color.close_set;
        conf_color = &term->conf->csd.color.quit;
        is_active = term->active_surface == TERM_SURF_BUTTON_CLOSE &&
                    any_pointer_is_on_button(term, CSD_SURF_CLOSE);
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
    render_csd_part(term, surf->surf, buf, info->width, info->height, &color);

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

    const float scale = term->scale;
    struct csd_data infos[CSD_SURF_COUNT];
    int widths[CSD_SURF_COUNT];
    int heights[CSD_SURF_COUNT];

    for (size_t i = 0; i < CSD_SURF_COUNT; i++) {
        infos[i] = get_csd_data(term, i);
        const int x = infos[i].x;
        const int y = infos[i].y;
        const int width = infos[i].width;
        const int height = infos[i].height;

        struct wl_surface *surf = term->window->csd.surface[i].surface.surf;
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
        wl_subsurface_set_position(sub, roundf(x / scale), roundf(y / scale));
    }

    struct buffer *bufs[CSD_SURF_COUNT];
    shm_get_many(term->render.chains.csd, CSD_SURF_COUNT, widths, heights, bufs, true);

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
        if (win->scrollback_indicator.surface.surf != NULL) {
            wayl_win_subsurface_destroy(&win->scrollback_indicator);

            /* Work around Sway bug - unmapping a sub-surface does not damage
             * the underlying surface */
            quirk_sway_subsurface_unmap(term);
        }
        return;
    }

    if (win->scrollback_indicator.surface.surf == NULL) {
        if (!wayl_win_subsurface_new(
                win, &win->scrollback_indicator, false))
        {
            LOG_ERR("failed to create scrollback indicator surface");
            return;
        }
    }

    xassert(win->scrollback_indicator.surface.surf != NULL);
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
        cell_count = (int)ceilf(log10f(term->grid->num_rows));
        break;
    }

    case SCROLLBACK_INDICATOR_FORMAT_TEXT:
        text = term->conf->scrollback.indicator.text;
        cell_count = c32len(text);
        break;
    }

    const float scale = term->scale;
    const int margin = (int)roundf(3. * scale);

    int width = margin + cell_count * term->cell_width + margin;
    int height = margin + term->cell_height + margin;

    width = roundf(scale * ceilf(width / scale));
    height = roundf(scale * ceilf(height / scale));

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

    int x = term->width - margin - width;
    int y = term->margins.top + surf_top;

    x = roundf(scale * ceilf(x / scale));
    y = roundf(scale * ceilf(y / scale));

    if (y + height > term->height) {
        wl_surface_attach(win->scrollback_indicator.surface.surf, NULL, 0, 0);
        wl_surface_commit(win->scrollback_indicator.surface.surf);
        return;
    }

    struct buffer_chain *chain = term->render.chains.scrollback_indicator;
    struct buffer *buf = shm_get_buffer(chain, width, height, false);

    wl_subsurface_set_position(
        win->scrollback_indicator.sub, roundf(x / scale), roundf(y / scale));

    uint32_t fg = term->colors.table[0];
    uint32_t bg = term->colors.table[8 + 4];
    if (term->conf->colors.use_custom.scrollback_indicator) {
        fg = term->conf->colors.scrollback_indicator.fg;
        bg = term->conf->colors.scrollback_indicator.bg;
    }

    render_osd(
        term,
        &win->scrollback_indicator,
        term->fonts[0], buf, text,
        fg, 0xffu << 24 | bg,
        width - margin - c32len(text) * term->cell_width);
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

    const float scale = term->scale;
    const int cell_count = c32len(text);
    const int margin = (int)roundf(3. * scale);

    int width = margin + cell_count * term->cell_width + margin;
    int height = margin + term->cell_height + margin;

    width = roundf(scale * ceilf(width / scale));
    height = roundf(scale * ceilf(height / scale));

    struct buffer_chain *chain = term->render.chains.render_timer;
    struct buffer *buf = shm_get_buffer(chain, width, height, false);

    wl_subsurface_set_position(
        win->render_timer.sub,
        roundf(margin / scale),
        roundf((term->margins.top + term->cell_height - margin) / scale));

    render_osd(
        term,
        &win->render_timer,
        term->fonts[0], buf, text,
        term->colors.table[0], 0xffu << 24 | term->colors.table[8 + 1],
        margin);
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

    pixman_region32_t dirty;
    pixman_region32_init(&dirty);

    /*
     * Figure out current frame's damage region
     *
     * If current frame doesn't have any scroll damage, we can simply
     * subtract this frame's damage from the last frame's damage. That
     * way, we don't have to copy areas from the old frame that'll
     * just get overwritten by current frame.
     *
     * Note that this is row based. A "half damaged" row is not
     * excluded. I.e. the entire row will be copied from the old frame
     * to the new, and then when actually rendering the new frame, the
     * updated cells will overwrite parts of the copied row.
     *
     * Since we're scanning the entire viewport anyway, we also track
     * whether *all* cells are to be updated. In this case, just force
     * a full re-rendering, and don't copy anything from the old
     * frame.
     */
    bool full_repaint_needed = true;

    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

        if (!row->dirty) {
            full_repaint_needed = false;
            continue;
        }

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

    /*
     * TODO: re-apply last frame's scroll damage
     *
     * We used to do this, but it turned out to be buggy. If we decide
     * to re-add it, this is where to do it. Note that we'd also have
     * to remove the updates to buf->dirty from grid_render_scroll()
     * and grid_render_scroll_reverse().
     */

    if (tll_length(term->grid->scroll_damage) == 0) {
        /*
         * We can only subtract current frame's damage from the old
         * frame's if we don't have any scroll damage.
         *
         * If we do have scroll damage, the damage region we
         * calculated above is not (yet) valid - we need to apply the
         * current frame's scroll damage *first*. This is done later,
         * when rendering the frame.
         */
        pixman_region32_subtract(&dirty, &old->dirty[0], &dirty);
        pixman_image_set_clip_region32(new->pix[0], &dirty);
    } else {
        /* Copy *all* of last frame's damaged areas */
        pixman_image_set_clip_region32(new->pix[0], &old->dirty[0]);
    }

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
    bool use_alpha = !term->window->is_fullscreen &&
                     term->colors.alpha != 0xffff;
    struct buffer *buf = shm_get_buffer(
        chain, term->width, term->height, use_alpha);

    /* Dirty old and current cursor cell, to ensure they're repainted */
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


    tll_foreach(term->grid->scroll_damage, it) {
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
                 * stop, since it isn't affecting the string of
                 * overflowing glyphs that follows it.
                 *
                 * As soon as we see a dirty cell, we can stop, since
                 * that means we've already handled it (remember the
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
                 * we're dealing with right now.
                 *
                 * For performance, this iterates the *outer* loop's
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

#if defined(_DEBUG)
    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);

        if (row->dirty) {
            bool all_clean = true;
            for (int c = 0; c < term->cols; c++) {
                if (!row->cells[c].attrs.clean) {
                    all_clean = false;
                    break;
                }
            }
            if (all_clean)
                BUG("row #%d is dirty, but all cells are marked as clean", r);
        } else {
            for (int c = 0; c < term->cols; c++) {
                if (!row->cells[c].attrs.clean)
                    BUG("row #%d is clean, but cell #%d is dirty", r, c);
            }
        }
    }
#endif

    pixman_region32_t damage;
    pixman_region32_init(&damage);

    render_sixel_images(term, buf->pix[0], &damage, &cursor);


    if (term->render.workers.count > 0) {
        mtx_lock(&term->render.workers.lock);
        term->render.workers.buf = buf;
        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_post(&term->render.workers.start);

        xassert(tll_length(term->render.workers.queue) == 0);
    }

    for (int r = 0; r < term->rows; r++) {
        struct row *row = grid_row_in_view(term->grid, r);

        if (!row->dirty)
            continue;

        row->dirty = false;

        if (term->render.workers.count > 0)
            tll_push_back(term->render.workers.queue, r);

        else {
            /* TODO: damage region */
            int cursor_col = cursor.row == r ? cursor.col : -1;
            render_row(term, buf->pix[0], &damage, row, r, cursor_col);
        }
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

    for (size_t i = 0; i < term->render.workers.count; i++)
        pixman_region32_union(&damage, &damage, &buf->dirty[i + 1]);

    pixman_region32_union(&buf->dirty[0], &buf->dirty[0], &damage);

    {
        int box_count = 0;
        pixman_box32_t *boxes = pixman_region32_rectangles(&damage, &box_count);

        for (size_t i = 0; i < box_count; i++) {
            wl_surface_damage_buffer(
                term->window->surface.surf,
                boxes[i].x1, boxes[i].y1,
                boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
        }
    }

    pixman_region32_fini(&damage);

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

        struct timespec total_render_time;
        timespec_add(&render_time, &double_buffering_time, &total_render_time);

        switch (term->conf->tweak.render_timer) {
        case RENDER_TIMER_LOG:
        case RENDER_TIMER_BOTH:
            LOG_INFO(
                "frame rendered in %lds %9ldns "
                "(%lds %9ldns rendering, %lds %9ldns double buffering)",
                (long)total_render_time.tv_sec,
                total_render_time.tv_nsec,
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
            render_render_timer(term, total_render_time);
            break;

        case RENDER_TIMER_LOG:
        case RENDER_TIMER_NONE:
            break;
        }
    }

    xassert(term->grid->offset >= 0 && term->grid->offset < term->grid->num_rows);
    xassert(term->grid->view >= 0 && term->grid->view < term->grid->num_rows);

    xassert(term->window->frame_callback == NULL);
    term->window->frame_callback = wl_surface_frame(term->window->surface.surf);
    wl_callback_add_listener(term->window->frame_callback, &frame_listener, term);

    wayl_win_scale(term->window, buf);

    if (term->wl->presentation != NULL && term->conf->presentation_timings) {
        struct timespec commit_time;
        clock_gettime(term->wl->presentation_clock_id, &commit_time);

        struct wp_presentation_feedback *feedback = wp_presentation_feedback(
            term->wl->presentation, term->window->surface.surf);

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
            term->window->surface.surf, 0, 0, INT32_MAX, INT32_MAX);
    }

    wl_surface_attach(term->window->surface.surf, buf->wl_buf, 0, 0);
    wl_surface_commit(term->window->surface.surf);
}

static void
render_search_box(struct terminal *term)
{
    xassert(term->window->search.sub != NULL);

    /*
     * We treat the search box pretty much like a row of cells. That
     * is, a glyph is either 1 or 2 (or more) "cells" wide.
     *
     * The search 'length', and 'cursor' (position) is in
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

    const float scale = term->scale;
    xassert(scale >= 1.);
    const size_t margin = (size_t)roundf(3 * scale);

    size_t width = term->width - 2 * margin;
    size_t height = min(
        term->height - 2 * margin,
        margin + 1 * term->cell_height + margin);

    width = roundf(scale * ceilf((term->width - 2 * margin) / scale));
    height = roundf(scale * ceilf(height / scale));

    size_t visible_width = min(
        term->width - 2 * margin,
        margin + wanted_visible_cells * term->cell_width + margin);

    const size_t visible_cells = (visible_width - 2 * margin) / term->cell_width;
    size_t glyph_offset = term->render.search_glyph_offset;

    struct buffer_chain *chain = term->render.chains.search;
    struct buffer *buf = shm_get_buffer(chain, width, height, true);

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 0, 0, width, height);
    pixman_image_set_clip_region32(buf->pix[0], &clip);
    pixman_region32_fini(&clip);

#define WINDOW_X(x) (margin + x)
#define WINDOW_Y(y) (term->height - margin - height + y)

    const bool is_match = term->search.match_len == text_len;
    const bool custom_colors = is_match
        ? term->conf->colors.use_custom.search_box_match
        : term->conf->colors.use_custom.search_box_no_match;

    /* Background - yellow on empty/match, red on mismatch (default) */
    const pixman_color_t color = color_hex_to_pixman(
        is_match
        ? (custom_colors
           ? term->conf->colors.search_box.match.bg
           : term->colors.table[3])
        : (custom_colors
           ? term->conf->colors.search_box.no_match.bg
           : term->colors.table[1]));

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
    pixman_color_t fg = color_hex_to_pixman(
        custom_colors
        ? (is_match
           ? term->conf->colors.search_box.match.fg
           : term->conf->colors.search_box.no_match.fg)
        : term->colors.table[0]);

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
     * Render the search string, starting at 'glyph_offset'. Note that
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
                        draw_hollow_block(
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
                x + x_ofs + glyph->x, y + term->font_baseline - glyph->y,
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
                y + term->font_baseline - glyph->y,
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
        roundf(margin / scale),
        roundf(max(0, (int32_t)term->height - height - margin) / scale));

    wayl_surface_scale(term->window, &term->window->search.surface, buf, scale);
    wl_surface_attach(term->window->search.surface.surf, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(term->window->search.surface.surf, 0, 0, width, height);

    struct wl_region *region = wl_compositor_create_region(term->wl->compositor);
    if (region != NULL) {
        wl_region_add(region, width - visible_width, 0, visible_width, height);
        wl_surface_set_opaque_region(term->window->search.surface.surf, region);
        wl_region_destroy(region);
    }

    wl_surface_commit(term->window->search.surface.surf);
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

    const float scale = term->scale;
    const int x_margin = (int)roundf(2 * scale);
    const int y_margin = (int)roundf(1 * scale);

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

        struct wl_surface *surf = it->item.surf.surface.surf;
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

        /* Don't position it outside our window */
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
         * Don't extend outside our window
         *
         * Truncate label so that it doesn't extend outside our
         * window.
         *
         * Do it in a way such that we don't cut the label in the
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

        int width = x_margin + cols * term->cell_width + x_margin;
        int height = y_margin + term->cell_height + y_margin;

        width = roundf(scale * ceilf(width / scale));
        height = roundf(scale * ceilf(height / scale));

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
    shm_get_many(chain, render_count, widths, heights, bufs, false);

    uint32_t fg = term->conf->colors.use_custom.jump_label
        ? term->conf->colors.jump_label.fg
        : term->colors.table[0];
    uint32_t bg = term->conf->colors.use_custom.jump_label
        ? term->conf->colors.jump_label.bg
        : term->colors.table[3];

    for (size_t i = 0; i < render_count; i++) {
        const struct wayl_sub_surface *sub_surf = &info[i].url->surf;

        const char32_t *label = info[i].text;
        const int x = info[i].x;
        const int y = info[i].y;

        xassert(sub_surf->surface.surf != NULL);
        xassert(sub_surf->sub != NULL);

        wl_subsurface_set_position(
            sub_surf->sub,
            roundf((term->margins.left + x) / scale),
            roundf((term->margins.top + y) / scale));

        render_osd(
            term, sub_surf, term->fonts[0], bufs[i], label,
            fg, 0xffu << 24 | bg, x_margin);

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

        term_send_size_notification(term);
    }
}

static void
delayed_reflow_of_normal_grid(struct terminal *term)
{
    if (term->interactive_resizing.grid == NULL)
        return;

    xassert(term->interactive_resizing.new_rows > 0);

    struct coord *const tracking_points[] = {
        &term->selection.coords.start,
        &term->selection.coords.end,
    };

    /* Reflow the original (since before the resize was started) grid,
     * to the *current* dimensions */
    grid_resize_and_reflow(
        term->interactive_resizing.grid,
        term->interactive_resizing.new_rows, term->normal.num_cols,
        term->interactive_resizing.old_screen_rows, term->rows,
        term->selection.coords.end.row >= 0 ? ALEN(tracking_points) : 0,
        tracking_points);

    /* Replace the current, truncated, "normal" grid with the
     * correctly reflowed one */
    grid_free(&term->normal);
    term->normal = *term->interactive_resizing.grid;
    free(term->interactive_resizing.grid);

    term->hide_cursor = term->interactive_resizing.old_hide_cursor;

    /* Reset */
    term->interactive_resizing.grid = NULL;
    term->interactive_resizing.old_screen_rows = 0;
    term->interactive_resizing.new_rows = 0;
    term->interactive_resizing.old_hide_cursor = false;

    /* Invalidate render pointers */
    shm_unref(term->render.last_buf);
    term->render.last_buf = NULL;
    term->render.last_cursor.row = NULL;

    tll_free(term->normal.scroll_damage);
    sixel_reflow_grid(term, &term->normal);

    if (term->grid == &term->normal) {
        term_damage_view(term);
        render_refresh(term);
    }

    term_ptmx_resume(term);
}

static bool
fdm_tiocswinsz(struct fdm *fdm, int fd, int events, void *data)
{
    struct terminal *term = data;

    if (events & EPOLLIN) {
        tiocswinsz(term);
        delayed_reflow_of_normal_grid(term);
    }

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
        delayed_reflow_of_normal_grid(term);

        /* And make sure to reset and deallocate a lingering timer */
        if (win->resize_timeout_fd >= 0) {
            fdm_del(term->fdm, win->resize_timeout_fd);
            win->resize_timeout_fd = -1;
        }
    } else {
        /* Send new dimensions to client "in a while" */
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
                .it_value = {
                    .tv_sec = delay_ms / 1000,
                    .tv_nsec = (delay_ms % 1000) * 1000000,
                },
            };

            if (timerfd_settime(fd, 0, &timeout, NULL) < 0) {
                LOG_ERRNO("failed to arm TIOCSWINSZ timer");
                fdm_del(term->fdm, fd);
                win->resize_timeout_fd = -1;
            } else
                successfully_scheduled = true;
        }

        if (!successfully_scheduled) {
            tiocswinsz(term);
            delayed_reflow_of_normal_grid(term);
        }
    }
}

static void
set_size_from_grid(struct terminal *term, int *width, int *height, int cols, int rows)
{
    /* Nominal grid dimensions */
    *width = cols * term->cell_width;
    *height = rows * term->cell_height;

    /* Include any configured padding */
    *width += 2 * term->conf->pad_x * term->scale;
    *height += 2 * term->conf->pad_y * term->scale;

    /* Round to multiples of scale */
    *width = round(term->scale * round(*width / term->scale));
    *height = round(term->scale * round(*height / term->scale));
}

/* Move to terminal.c? */
bool
render_resize(struct terminal *term, int width, int height, uint8_t opts)
{
    if (term->shutdown.in_progress)
        return false;

    if (!term->window->is_configured)
        return false;

    if (term->cell_width == 0 && term->cell_height == 0)
        return false;

    const bool is_floating =
        !term->window->is_maximized &&
        !term->window->is_fullscreen &&
        !term->window->is_tiled;

    /* Convert logical size to physical size */
    const float scale = term->scale;
    width = round(width * scale);
    height = round(height * scale);

    /* If the grid should be kept, the size should be overridden */
    if (is_floating && (opts & RESIZE_KEEP_GRID)) {
        set_size_from_grid(term, &width, &height, term->cols, term->rows);
    }

    if (width == 0 && height == 0) {
        /* The compositor is letting us choose the size */
        if (term->stashed_width != 0 && term->stashed_height != 0) {
            /* If a default size is requested, prefer the "last used" size */
            width = term->stashed_width;
            height = term->stashed_height;
        } else {
            /* Otherwise, use a user-configured size */
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
                set_size_from_grid(term, &width, &height,
                                   term->conf->size.width, term->conf->size.height);
                break;
            }
        }
    }

    /* Don't shrink grid too much */
    const int min_cols = 2;
    const int min_rows = 1;

    /* Minimum window size (must be divisible by the scaling factor)*/
    const int min_width = roundf(scale * ceilf((min_cols * term->cell_width) / scale));
    const int min_height = roundf(scale * ceilf((min_rows * term->cell_height) / scale));

    width = max(width, min_width);
    height = max(height, min_height);

    /* Padding */
    const int max_pad_x = (width - min_width) / 2;
    const int max_pad_y = (height - min_height) / 2;
    const int pad_x = min(max_pad_x, scale * term->conf->pad_x);
    const int pad_y = min(max_pad_y, scale * term->conf->pad_y);

    if (is_floating &&
        (opts & RESIZE_BY_CELLS) &&
        term->conf->resize_by_cells)
    {
        /* If resizing in cell increments, restrict the width and height */
        width = ((width - 2 * pad_x) / term->cell_width) * term->cell_width + 2 * pad_x;
        width = max(min_width, roundf(scale * roundf(width / scale)));

        height = ((height - 2 * pad_y) / term->cell_height) * term->cell_height + 2 * pad_y;
        height = max(min_height, roundf(scale * roundf(height / scale)));
    }

    if (!(opts & RESIZE_FORCE) &&
        width == term->width &&
        height == term->height &&
        scale == term->scale)
    {
        return false;
    }

    /* Cancel an application initiated "Synchronized Update" */
    term_disable_app_sync_updates(term);

    /* Drop out of URL mode */
    urls_reset(term);

    LOG_DBG("resized: size=%dx%d (scale=%.2f)", width, height, term->scale);
    term->width = width;
    term->height = height;

    /* Screen rows/cols before resize */
    int old_cols = term->cols;
    int old_rows = term->rows;

    /* Screen rows/cols after resize */
    const int new_cols = (term->width - 2 * pad_x) / term->cell_width;
    const int new_rows = (term->height - 2 * pad_y) / term->cell_height;

    /*
     * Requirements for scrollback:
     *
     *   a) total number of rows (visible + scrollback history) must be
     *      a power of two
     *   b) must be representable in a plain int (signed)
     *
     * This means that on a "normal" system, where ints are 32-bit,
     * the largest possible scrollback size is 1073741824 (0x40000000,
     * 1 << 30).
     *
     * The largest *signed* int is 2147483647 (0x7fffffff), which is
     * *not* a power of two.
     *
     * Note that these are theoretical limits. Most of the time,
     * you'll get a memory allocation failure when trying to allocate
     * the grid array.
     */
    const unsigned max_scrollback = (INT_MAX >> 1) + 1;
    const unsigned scrollback_lines_not_yet_power_of_two =
        min((uint64_t)term->render.scrollback_lines + new_rows - 1, max_scrollback);

    /* Grid rows/cols after resize */
    const int new_normal_grid_rows =
        min(1u << (32 - __builtin_clz(scrollback_lines_not_yet_power_of_two)),
            max_scrollback);
    const int new_alt_grid_rows =
        min(1u << (32 - __builtin_clz(new_rows)), max_scrollback);

    LOG_DBG("grid rows: %d", new_normal_grid_rows);

    xassert(new_cols >= 1);
    xassert(new_rows >= 1);

    /* Margins */
    const int grid_width = new_cols * term->cell_width;
    const int grid_height = new_rows * term->cell_height;
    const int total_x_pad = term->width - grid_width;
    const int total_y_pad = term->height - grid_height;

    const bool centered_padding = term->conf->center
                                  || term->window->is_fullscreen
                                  || term->window->is_maximized;

    if (centered_padding && !term->window->is_resizing) {
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
        term->interactive_resizing.new_rows = new_normal_grid_rows;
        goto damage_view;
    }


    /*
     * Since text reflow is slow, don't do it *while* resizing. Only
     * do it when done, or after "pausing" the resize for sufficiently
     * long. We reuse the TIOCSWINSZ timer to handle this. See
     * send_dimensions_to_client() and fdm_tiocswinsz().
     *
     * To be able to do the final reflow correctly, we need a copy of
     * the original grid, before the resize started.
     */
    if (term->window->is_resizing && term->conf->resize_delay_ms > 0) {
        if (term->interactive_resizing.grid == NULL) {
            term_ptmx_pause(term);

            /* Stash the current 'normal' grid, as-is, to be used when
             * doing the final reflow */
            term->interactive_resizing.old_screen_rows = term->rows;
            term->interactive_resizing.old_cols = term->cols;
            term->interactive_resizing.old_hide_cursor = term->hide_cursor;
            term->interactive_resizing.grid = xmalloc(sizeof(*term->interactive_resizing.grid));
            *term->interactive_resizing.grid = term->normal;

            if (term->grid == &term->normal)
                term->interactive_resizing.selection_coords = term->selection.coords;
        } else {
            /* We'll replace the current temporary grid, with a new
             * one (again based on the original grid) */
            grid_free(&term->normal);
        }

        struct grid *orig = term->interactive_resizing.grid;

        /*
         * Copy the current viewport (of the original grid) to a new
         * grid that will be used during the resize. For now, throw
         * away sixels and OSC-8 URLs. They'll be "restored" when we
         * do the final reflow.
         *
         * Note that OSC-8 URLs are perfectly ok to throw away; they
         * cannot be interacted with during the resize. And, even if
         * url.osc8-underline=always, the "underline" attribute is
         * part of the cell, not the URI struct (and thus our faked
         * grid will still render OSC-8 links underlined).
         *
         * TODO:
         *  - sixels?
         */
        struct grid g = {
            .num_rows = 1 << (32 - __builtin_clz(term->interactive_resizing.old_screen_rows)),
            .num_cols = term->interactive_resizing.old_cols,
            .offset = 0,
            .view = 0,
            .cursor = orig->cursor,
            .saved_cursor = orig->saved_cursor,
            .rows = xcalloc(g.num_rows, sizeof(g.rows[0])),
            .cur_row = NULL,
            .scroll_damage = tll_init(),
            .sixel_images = tll_init(),
            .kitty_kbd = orig->kitty_kbd,
        };

        term->selection.coords.start.row -= orig->view;
        term->selection.coords.end.row -= orig->view;

        for (size_t i = 0, j = orig->view;
             i < term->interactive_resizing.old_screen_rows;
             i++, j = (j + 1) & (orig->num_rows - 1))
        {
            g.rows[i] = grid_row_alloc(g.num_cols, false);
            memcpy(g.rows[i]->cells,
                   orig->rows[j]->cells,
                   g.num_cols * sizeof(g.rows[i]->cells[0]));

            if (orig->rows[j]->extra == NULL ||
                orig->rows[j]->extra->underline_ranges.count == 0)
            {
                continue;
            }

            /*
             * Copy underline ranges
             */

            const struct row_ranges *underline_src = &orig->rows[j]->extra->underline_ranges;

            const int count = underline_src->count;
            g.rows[i]->extra = xcalloc(1, sizeof(*g.rows[i]->extra));
            g.rows[i]->extra->underline_ranges.v = xmalloc(
                count * sizeof(g.rows[i]->extra->underline_ranges.v[0]));

            struct row_ranges *underline_dst = &g.rows[i]->extra->underline_ranges;
            underline_dst->count = underline_dst->size = count;

            for (int k = 0; k < count; k++)
                underline_dst->v[k] = underline_src->v[k];
        }

        term->normal = g;
        term->hide_cursor = true;
    }

    if (term->grid == &term->alt)
        selection_cancel(term);
    else {
        /*
         * Don't cancel, but make sure there aren't any ongoing
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
     * selection's pivot point coordinates *must* be added to the
     * tracking points list.
     */
    /* Resize grids */
    if (term->window->is_resizing && term->conf->resize_delay_ms > 0) {
        /* Simple truncating resize, *while* an interactive resize is
         * ongoing. */
        xassert(term->interactive_resizing.grid != NULL);
        xassert(new_normal_grid_rows > 0);
        term->interactive_resizing.new_rows = new_normal_grid_rows;

        grid_resize_without_reflow(
            &term->normal, new_alt_grid_rows, new_cols,
            term->interactive_resizing.old_screen_rows, new_rows);
    } else {
        /* Full text reflow */

        int old_normal_rows = old_rows;

        if (term->interactive_resizing.grid != NULL) {
            /* Throw away the current, truncated, "normal" grid, and
             * use the original grid instead (from before the resize
             * started) */
            grid_free(&term->normal);
            term->normal = *term->interactive_resizing.grid;
            free(term->interactive_resizing.grid);

            term->hide_cursor = term->interactive_resizing.old_hide_cursor;
            term->selection.coords = term->interactive_resizing.selection_coords;

            old_normal_rows = term->interactive_resizing.old_screen_rows;

            term->interactive_resizing.grid = NULL;
            term->interactive_resizing.old_screen_rows = 0;
            term->interactive_resizing.new_rows = 0;
            term->interactive_resizing.old_hide_cursor = false;
            term->interactive_resizing.selection_coords = (struct range){{-1, -1}, {-1, -1}};
            term_ptmx_resume(term);
        }

        struct coord *const tracking_points[] = {
            &term->selection.coords.start,
            &term->selection.coords.end,
        };

        grid_resize_and_reflow(
            &term->normal, new_normal_grid_rows, new_cols, old_normal_rows, new_rows,
            term->selection.coords.end.row >= 0 ? ALEN(tracking_points) : 0,
            tracking_points);
    }

    grid_resize_without_reflow(
        &term->alt, new_alt_grid_rows, new_cols, old_rows, new_rows);

    /* Reset tab stops */
    tll_free(term->tab_stops);
    for (int c = 0; c < new_cols; c += 8)
        tll_push_back(term->tab_stops, c);

    term->cols = new_cols;
    term->rows = new_rows;

    sixel_reflow(term);

    LOG_DBG("resized: grid: cols=%d, rows=%d "
            "(left-margin=%d, right-margin=%d, top-margin=%d, bottom-margin=%d)",
            term->cols, term->rows,
            term->margins.left, term->margins.right,
            term->margins.top, term->margins.bottom);

    if (term->scroll_region.start >= term->rows)
        term->scroll_region.start = 0;
    if (term->scroll_region.end > term->rows ||
        term->scroll_region.end >= old_rows)
    {
        term->scroll_region.end = term->rows;
    }

    term->render.last_cursor.row = NULL;

damage_view:
    /* Signal TIOCSWINSZ */
    send_dimensions_to_client(term);

    if (is_floating) {
        /* Stash current size, to enable us to restore it when we're
         * being un-maximized/fullscreened/tiled */
        term->stashed_width = term->width;
        term->stashed_height = term->height;
    }

    {
        const bool title_shown = wayl_win_csd_titlebar_visible(term->window);
        const bool border_shown = wayl_win_csd_borders_visible(term->window);

        const int title = title_shown
            ? roundf(term->conf->csd.title_height * scale)
            : 0;
        const int border = border_shown
            ? roundf(term->conf->csd.border_width_visible * scale)
            : 0;

        /* Must use surface logical coordinates (same calculations as
           in get_csd_data(), but with different inputs) */
        const int toplevel_min_width = roundf(border / scale) +
                                       roundf(min_width / scale) +
                                       roundf(border / scale);

        const int toplevel_min_height = roundf(border / scale) +
                                        roundf(title / scale) +
                                        roundf(min_height / scale) +
                                        roundf(border / scale);

        const int toplevel_width = roundf(border / scale) +
                                   roundf(term->width / scale) +
                                   roundf(border / scale);

        const int toplevel_height = roundf(border / scale) +
                                    roundf(title / scale) +
                                    roundf(term->height / scale) +
                                    roundf(border / scale);

        const int x = roundf(-border / scale);
        const int y = roundf(-title / scale) - roundf(border / scale);

        xdg_toplevel_set_min_size(
            term->window->xdg_toplevel,
            toplevel_min_width, toplevel_min_height);

        xdg_surface_set_window_geometry(
            term->window->xdg_surface,
            x, y, toplevel_width, toplevel_height);
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
    if (seat->pointer.theme == NULL)
        return false;
    return wl_cursor_theme_get_cursor(seat->pointer.theme, cursor) != NULL;
}

static void
render_xcursor_update(struct seat *seat)
{
    /* If called from a frame callback, we may no longer have mouse focus */
    if (!seat->mouse_focus)
        return;

    xassert(seat->pointer.shape != CURSOR_SHAPE_NONE);

    if (seat->pointer.shape == CURSOR_SHAPE_HIDDEN) {
        /* Hide cursor */
        LOG_DBG("hiding cursor using client-side NULL-surface");
        wl_surface_attach(seat->pointer.surface.surf, NULL, 0, 0);
        wl_pointer_set_cursor(
            seat->wl_pointer, seat->pointer.serial, seat->pointer.surface.surf,
            0, 0);
        wl_surface_commit(seat->pointer.surface.surf);
        return;
    }

    const enum cursor_shape shape = seat->pointer.shape;
    const char *const xcursor = seat->pointer.last_custom_xcursor;

    if (seat->pointer.shape_device != NULL) {
        xassert(shape != CURSOR_SHAPE_CUSTOM || xcursor != NULL);

        const enum wp_cursor_shape_device_v1_shape custom_shape =
            (shape == CURSOR_SHAPE_CUSTOM && xcursor != NULL
             ? cursor_string_to_server_shape(xcursor)
             : 0);

        if (shape != CURSOR_SHAPE_CUSTOM || custom_shape != 0) {
            xassert(custom_shape == 0 || shape == CURSOR_SHAPE_CUSTOM);

            const enum wp_cursor_shape_device_v1_shape wp_shape = custom_shape != 0
                ? custom_shape
                : cursor_shape_to_server_shape(shape);

            LOG_DBG("setting %scursor shape using cursor-shape-v1",
                    custom_shape != 0 ? "custom " : "");

            wp_cursor_shape_device_v1_set_shape(
                seat->pointer.shape_device,
                seat->pointer.serial,
                wp_shape);

            return;
        }
    }

    LOG_DBG("setting %scursor shape using a client-side cursor surface",
            seat->pointer.shape == CURSOR_SHAPE_CUSTOM ? "custom " : "");

    if (seat->pointer.cursor == NULL) {
        /*
         * Normally, we never get here with a NULL-cursor, because we
         * only schedule a cursor update when we succeed to load the
         * cursor image.
         *
         * However, it is possible that we did succeed to load an
         * image, and scheduled an update. But, *before* the scheduled
         * update triggers, the user mvoes the pointer, and we try to
         * load a new cursor image. This time failing.
         *
         * In this case, we have a NULL cursor, but the scheduled
         * update is still scheduled.
         */
        return;
    }

    const float scale = seat->pointer.scale;
    struct wl_cursor_image *image = seat->pointer.cursor->images[0];
    struct wl_buffer *buf = wl_cursor_image_get_buffer(image);

    wayl_surface_scale_explicit_width_height(
        seat->mouse_focus->window,
        &seat->pointer.surface, image->width, image->height, scale);

    wl_surface_attach(seat->pointer.surface.surf, buf, 0, 0);

    wl_pointer_set_cursor(
        seat->wl_pointer, seat->pointer.serial,
        seat->pointer.surface.surf,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        seat->pointer.surface.surf, 0, 0, INT32_MAX, INT32_MAX);

    xassert(seat->pointer.xcursor_callback == NULL);
    seat->pointer.xcursor_callback = wl_surface_frame(seat->pointer.surface.surf);
    wl_callback_add_listener(seat->pointer.xcursor_callback, &xcursor_listener, seat);

    wl_surface_commit(seat->pointer.surface.surf);
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
render_refresh_app_id(struct terminal *term)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return;

    struct timespec diff;
    timespec_sub(&now, &term->render.app_id.last_update, &diff);

    if (diff.tv_sec == 0 && diff.tv_nsec < 8333 * 1000) {
        const struct itimerspec timeout = {
            .it_value = {.tv_nsec = 8333 * 1000 - diff.tv_nsec},
        };

        timerfd_settime(term->render.app_id.timer_fd, 0, &timeout, NULL);
        return;
    }

    const char *app_id =
        term->app_id != NULL ? term->app_id : term->conf->app_id;

    xdg_toplevel_set_app_id(term->window->xdg_toplevel, app_id);
    term->render.app_id.last_update = now;
}

void
render_refresh_icon(struct terminal *term)
{
#if defined(HAVE_XDG_TOPLEVEL_ICON)
    if (term->wl->toplevel_icon_manager == NULL) {
        LOG_DBG("compositor does not implement xdg-toplevel-icon: "
                "ignoring request to refresh window icon");
        return;
    }

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return;

    struct timespec diff;
    timespec_sub(&now, &term->render.icon.last_update, &diff);

    if (diff.tv_sec == 0 && diff.tv_nsec < 8333 * 1000) {
        const struct itimerspec timeout = {
            .it_value = {.tv_nsec = 8333 * 1000 - diff.tv_nsec},
        };

        timerfd_settime(term->render.icon.timer_fd, 0, &timeout, NULL);
        return;
    }

    const char *icon_name = term_icon(term);
    LOG_DBG("setting toplevel icon: %s", icon_name);

    struct xdg_toplevel_icon_v1 *icon =
        xdg_toplevel_icon_manager_v1_create_icon(term->wl->toplevel_icon_manager);
    xdg_toplevel_icon_v1_set_name(icon, icon_name);
    xdg_toplevel_icon_manager_v1_set_icon(
        term->wl->toplevel_icon_manager, term->window->xdg_toplevel, icon);
    xdg_toplevel_icon_v1_destroy(icon);

    term->render.icon.last_update = now;
#endif
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
render_xcursor_set(struct seat *seat, struct terminal *term,
                   enum cursor_shape shape)
{
    if (seat->pointer.theme == NULL && seat->pointer.shape_device == NULL)
        return false;

    if (seat->mouse_focus == NULL) {
        seat->pointer.shape = CURSOR_SHAPE_NONE;
        return true;
    }

    if (seat->mouse_focus != term) {
        /* This terminal doesn't have mouse focus */
        return true;
    }

    if (seat->pointer.shape == shape &&
        !(shape == CURSOR_SHAPE_CUSTOM &&
          !streq(seat->pointer.last_custom_xcursor,
                 term->mouse_user_cursor)))
    {
        return true;
    }

    if (shape == CURSOR_SHAPE_HIDDEN) {
        seat->pointer.cursor = NULL;
        free(seat->pointer.last_custom_xcursor);
        seat->pointer.last_custom_xcursor = NULL;
    }

    else if (seat->pointer.shape_device == NULL) {
        const char *const custom_xcursors[] = {term->mouse_user_cursor, NULL};
        const char *const *xcursors = shape == CURSOR_SHAPE_CUSTOM
            ? custom_xcursors
            : cursor_shape_to_string(shape);

        xassert(xcursors[0] != NULL);

        seat->pointer.cursor = NULL;

        for (size_t i = 0; xcursors[i] != NULL; i++) {
            seat->pointer.cursor =
                wl_cursor_theme_get_cursor(seat->pointer.theme, xcursors[i]);

            if (seat->pointer.cursor != NULL) {
                LOG_DBG("loaded xcursor %s", xcursors[i]);
                break;
            }
        }

        if (seat->pointer.cursor == NULL) {
            LOG_ERR(
                "failed to load xcursor pointer '%s', and all of its fallbacks",
                xcursors[0]);
            return false;
        }
    } else {
        /* Server-side cursors - no need to load anything */
    }

    if (shape == CURSOR_SHAPE_CUSTOM) {
        free(seat->pointer.last_custom_xcursor);
        seat->pointer.last_custom_xcursor =
            xstrdup(term->mouse_user_cursor);
    }

    /* FDM hook takes care of actual rendering */
    seat->pointer.shape = shape;
    seat->pointer.xcursor_pending = true;
    return true;
}
