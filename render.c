#include "render.h"

#include <string.h>

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/prctl.h>

#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <presentation-time.h>

#include <fcft/fcft.h>

#define LOG_MODULE "render"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "config.h"
#include "grid.h"
#include "quirks.h"
#include "selection.h"
#include "shm.h"

#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))

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

#define shm_cookie_grid(term) ((unsigned long)((uintptr_t)term + 0))
#define shm_cookie_search(term) ((unsigned long)((uintptr_t)term + 1))
#define shm_cookie_csd(term, n) ((unsigned long)((uintptr_t)term + 2 + (n)))  /* Should be placed last */

struct renderer *
render_init(struct fdm *fdm, struct wayland *wayl)
{
    struct renderer *renderer = calloc(1, sizeof(*renderer));
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

static void __attribute__((destructor))
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
        chars += snprintf(&msg[chars], sizeof(msg) - chars,
                          "input - %lu µs -> ", diff.tv_usec);
    }

    struct timeval diff;
    timersub(&presented, commit, &diff);
    chars += snprintf(&msg[chars], sizeof(msg) - chars,
                      "commit - %lu µs -> ", diff.tv_usec);

    if (use_input) {
        assert(timercmp(&presented, input, >));
        timersub(&presented, input, &diff);
    } else {
        assert(timercmp(&presented, commit, >));
        timersub(&presented, commit, &diff);
    }

    chars += snprintf(&msg[chars], sizeof(msg) - chars,
                      "presented (total: %lu µs)", diff.tv_usec);

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

static struct font *
attrs_to_font(const struct terminal *term, const struct attributes *attrs)
{
    int idx = attrs->italic << 1 | attrs->bold;
    return term->fonts[idx];
}

static inline struct rgb
color_hex_to_rgb(uint32_t color)
{
    return (struct rgb){
        ((color >> 16) & 0xff) / 255.,
        ((color >>  8) & 0xff) / 255.,
        ((color >>  0) & 0xff) / 255.,
    };
}

static inline pixman_color_t
color_hex_to_pixman_with_alpha(uint32_t color, uint16_t alpha)
{
    if (alpha == 0)
        return (pixman_color_t){0, 0, 0, 0};

    int alpha_div = 0xffff / alpha;
    return (pixman_color_t){
        .red =   ((color >> 16 & 0xff) | (color >> 8 & 0xff00)) / alpha_div,
        .green = ((color >>  8 & 0xff) | (color >> 0 & 0xff00)) / alpha_div,
        .blue =  ((color >>  0 & 0xff) | (color << 8 & 0xff00)) / alpha_div,
        .alpha = alpha,
    };
}

static inline pixman_color_t
color_hex_to_pixman(uint32_t color)
{
    /* Count on the compiler optimizing this */
    return color_hex_to_pixman_with_alpha(color, 0xffff);
}

static inline void
color_dim(struct rgb *rgb)
{
    rgb->r /= 2.;
    rgb->g /= 2.;
    rgb->b /= 2.;
}

static inline void
pixman_color_dim(pixman_color_t *color)
{
    color->red /= 2;
    color->green /= 2;
    color->blue /= 2;
}

static inline void
pixman_color_dim_for_search(pixman_color_t *color)
{
    color->red /= 3;
    color->green /= 3;
    color->blue /= 3;
}

static inline int
font_baseline(const struct terminal *term)
{
    return term->fonts[0]->ascent;
}

static void
draw_unfocused_block(const struct terminal *term, pixman_image_t *pix,
                     const pixman_color_t *color, int x, int y, int cell_cols)
{
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color, 4,
        (pixman_rectangle16_t []){
         {x, y, cell_cols * term->cell_width, 1},                          /* top */
         {x, y, 1, term->cell_height},                                     /* left */
         {x + cell_cols * term->cell_width - 1, y, 1, term->cell_height},  /* right */
         {x, y + term->cell_height - 1, cell_cols * term->cell_width, 1},  /* bottom */
        });
}

static void
draw_bar(const struct terminal *term, pixman_image_t *pix,
         const struct font *font,
         const pixman_color_t *color, int x, int y)
{
    int baseline = y + font_baseline(term) - term->fonts[0]->ascent;
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, baseline,
            font->underline.thickness, term->fonts[0]->ascent + term->fonts[0]->descent});
}

static void
draw_underline(const struct terminal *term, pixman_image_t *pix,
               const struct font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + font_baseline(term) - font->underline.position,
            cols * term->cell_width, font->underline.thickness});
}

static void
draw_strikeout(const struct terminal *term, pixman_image_t *pix,
               const struct font *font,
               const pixman_color_t *color, int x, int y, int cols)
{
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, color,
        1, &(pixman_rectangle16_t){
            x, y + font_baseline(term) - font->strikeout.position,
            cols * term->cell_width, font->strikeout.thickness});
}

static void
draw_cursor(const struct terminal *term, const struct cell *cell,
            const struct font *font, pixman_image_t *pix, pixman_color_t *fg,
            const pixman_color_t *bg, int x, int y, int cols)
{
    pixman_color_t cursor_color;
    pixman_color_t text_color;

    bool is_selected = cell->attrs.selected;

    if (term->cursor_color.cursor >> 31) {
        cursor_color = color_hex_to_pixman(term->cursor_color.cursor);
        text_color = color_hex_to_pixman(
            term->cursor_color.text >> 31
            ? term->cursor_color.text : term->colors.bg);

        if (term->reverse ^ cell->attrs.reverse ^ is_selected) {
            pixman_color_t swap = cursor_color;
            cursor_color = text_color;
            text_color = swap;
        }

        if (term->is_searching && !is_selected) {
            pixman_color_dim_for_search(&cursor_color);
            pixman_color_dim_for_search(&text_color);
        }
    } else {
        cursor_color = *fg;
        text_color = *bg;
    }

    switch (term->cursor_style) {
    case CURSOR_BLOCK:
        if (!term->visual_focus)
            draw_unfocused_block(term, pix, &cursor_color, x, y, cols);

        else if (term->cursor_blink.state == CURSOR_BLINK_ON) {
            *fg = text_color;
            pixman_image_fill_rectangles(
                PIXMAN_OP_SRC, pix, &cursor_color, 1,
                &(pixman_rectangle16_t){x, y, cols * term->cell_width, term->cell_height});
        }
        break;

    case CURSOR_BAR:
        if (term->cursor_blink.state == CURSOR_BLINK_ON || !term->visual_focus)
            draw_bar(term, pix, font, &cursor_color, x, y);
        break;

    case CURSOR_UNDERLINE:
        if (term->cursor_blink.state == CURSOR_BLINK_ON || !term->visual_focus) {
            draw_underline(
                term, pix, attrs_to_font(term, &cell->attrs), &cursor_color,
                x, y, cols);
        }
        break;
    }
}

static int
render_cell(struct terminal *term, pixman_image_t *pix,
            struct cell *cell, int col, int row, bool has_cursor)
{
    if (cell->attrs.clean)
        return 0;

    cell->attrs.clean = 1;

    int width = term->cell_width;
    int height = term->cell_height;
    int x = term->margins.left + col * width;
    int y = term->margins.top + row * height;

    assert(cell->attrs.selected == 0 || cell->attrs.selected == 1);
    bool is_selected = cell->attrs.selected;

    uint32_t _fg = 0;
    uint32_t _bg = 0;

    /* Use cell specific color, if set, otherwise the default colors (possible reversed) */
    _fg = cell->attrs.have_fg ? cell->attrs.fg : term->colors.fg;
    _bg = cell->attrs.have_bg ? cell->attrs.bg : term->colors.bg;

    /* If *one* is set, we reverse */
    if (term->reverse ^ cell->attrs.reverse ^ is_selected) {
        uint32_t swap = _fg;
        _fg = _bg;
        _bg = swap;
    }

    if (cell->attrs.blink && term->blink.state == BLINK_OFF)
        _fg = _bg;

    pixman_color_t fg = color_hex_to_pixman(_fg);
    pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, term->colors.alpha);

    if (cell->attrs.dim)
        pixman_color_dim(&fg);

    if (term->is_searching && !is_selected) {
        pixman_color_dim_for_search(&fg);
        pixman_color_dim_for_search(&bg);
    }

    struct font *font = attrs_to_font(term, &cell->attrs);
    const struct glyph *glyph = cell->wc != 0
        ? font_glyph_for_wc(font, cell->wc, term->colors.alpha == 0xffff)
        : NULL;

    int cell_cols = glyph != NULL ? max(1, glyph->cols) : 1;

    /* Background */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, pix, &bg, 1,
        &(pixman_rectangle16_t){x, y, cell_cols * width, height});

    if (has_cursor)
        draw_cursor(term, cell, font, pix, &fg, &bg, x, y, cell_cols);

    if (cell->attrs.blink)
        term_arm_blink_timer(term);

    if (cell->wc == 0 || cell->attrs.conceal)
        return cell_cols;

    if (glyph != NULL) {
        if (unlikely(pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8)) {
            /* Glyph surface is a pre-rendered image (typically a color emoji...) */
            if (!(cell->attrs.blink && term->blink.state == BLINK_OFF)) {
                pixman_image_composite32(
                    PIXMAN_OP_OVER, glyph->pix, NULL, pix, 0, 0, 0, 0,
                    x + glyph->x, y + font_baseline(term) - glyph->y,
                    glyph->width, glyph->height);
            }
        } else {
            /* Glyph surface is an alpha mask */
            pixman_image_t *src = pixman_image_create_solid_fill(&fg);
            pixman_image_composite32(
                PIXMAN_OP_OVER, src, glyph->pix, pix, 0, 0, 0, 0,
                x + glyph->x, y + font_baseline(term) - glyph->y,
                glyph->width, glyph->height);
            pixman_image_unref(src);
        }
    }

    /* Underline */
    if (cell->attrs.underline) {
        draw_underline(term, pix, attrs_to_font(term, &cell->attrs),
                       &fg, x, y, cell_cols);
    }

    if (cell->attrs.strikethrough) {
        draw_strikeout(term, pix, attrs_to_font(term, &cell->attrs),
                       &fg, x, y, cell_cols);
    }

    return cell_cols;
}

static void
grid_render_scroll(struct terminal *term, struct buffer *buf,
                   const struct damage *dmg)
{
    int dst_y = term->margins.top + (dmg->scroll.region.start + 0) * term->cell_height;
    int src_y = term->margins.top + (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * term->cell_height;

    LOG_DBG("damage: SCROLL: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, buf->stride,
            buf->size);

    if (height > 0) {
        uint8_t *raw = buf->mmapped;
        memmove(raw + dst_y * buf->stride,
                raw + src_y * buf->stride,
                height * buf->stride);

        wl_surface_damage_buffer(
            term->window->surface, term->margins.left, dst_y, term->width - term->margins.left - term->margins.right, height);
    }
}

static void
grid_render_scroll_reverse(struct terminal *term, struct buffer *buf,
                           const struct damage *dmg)
{
    int src_y = term->margins.top + (dmg->scroll.region.start + 0) * term->cell_height;
    int dst_y = term->margins.top + (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
    int height = (dmg->scroll.region.end - dmg->scroll.region.start - dmg->scroll.lines) * term->cell_height;

    LOG_DBG("damage: SCROLL REVERSE: %d-%d by %d lines (dst-y: %d, src-y: %d, "
            "height: %d, stride: %d, mmap-size: %zu)",
            dmg->scroll.region.start, dmg->scroll.region.end,
            dmg->scroll.lines,
            dst_y, src_y, height, buf->stride,
            buf->size);

    if (height > 0) {
        uint8_t *raw = buf->mmapped;
        memmove(raw + dst_y * buf->stride,
                raw + src_y * buf->stride,
                height * buf->stride);

        wl_surface_damage_buffer(
            term->window->surface, term->margins.left, dst_y, term->width - term->margins.left - term->margins.right, height);
    }
}

static void
render_sixel(struct terminal *term, pixman_image_t *pix,
             const struct sixel *sixel)
{
    if (sixel->grid != term->grid)
        return;

    int view_end = (term->grid->view + term->rows - 1) & (term->grid->num_rows - 1);
    int first_visible_row = -1;

    for (size_t i = sixel->pos.row; i < sixel->pos.row + sixel->rows; i++) {
        int row = i & (term->grid->num_rows - 1);

        if (view_end >= term->grid->view) {
            /* Not wrapped */
            if (row >= term->grid->view && row <= view_end) {
                first_visible_row = i;
                break;
            }
        } else {
            /* Wrapped */
            if (row >= term->grid->view || row <= view_end) {
                first_visible_row = i;
                break;
            }
        }
    }

    if (first_visible_row < 0)
        return;

    /* First visible (0 based) row of the image */
    const int first_img_row = first_visible_row - sixel->pos.row;

    /* Map first visible line to current grid view */
    const int row = first_visible_row & (term->grid->num_rows - 1);
    const int view_aligned =
        (row - term->grid->view + term->grid->num_rows) & (term->grid->num_rows - 1);

    /* Translate row/column to x/y pixel values */
    const int x = term->margins.left + sixel->pos.col * term->cell_width;
    const int y = max(
        term->margins.top, term->margins.top + view_aligned * term->cell_height);

    /* Width/height, in pixels - and don't touch the window margins */
    const int width = min(sixel->width, term->width - x - term->margins.right);
    const int height = min(
        sixel->height - first_img_row * term->cell_height,
        term->height - y - term->margins.bottom);

    /* Verify we're not stepping outside the grid */
    assert(x >= term->margins.left);
    assert(y >= term->margins.top);
    assert(x + width <= term->width - term->margins.right);
    assert(y + height <= term->height - term->margins.bottom);

    pixman_image_composite(
        PIXMAN_OP_SRC,
        sixel->pix,
        NULL,
        pix,
        0, first_img_row * term->cell_height,
        0, 0,
        x, y,
        width, height);

    wl_surface_damage_buffer(term->window->surface, x, y, width, height);
}

static void
render_sixel_images(struct terminal *term, pixman_image_t *pix)
{
    tll_foreach(term->sixel_images, it)
        render_sixel(term, pix, &it->item);
}

static void
render_row(struct terminal *term, pixman_image_t *pix, struct row *row, int row_no)
{
    for (int col = term->cols - 1; col >= 0; col--)
        render_cell(term, pix, &row->cells[col], col, row_no, false);
}

int
render_worker_thread(void *_ctx)
{
    struct render_worker_context *ctx = _ctx;
    struct terminal *term = ctx->term;
    const int my_id = ctx->my_id;
    free(ctx);

    char proc_title[16];
    snprintf(proc_title, sizeof(proc_title), "foot:render:%d", my_id);

    if (prctl(PR_SET_NAME, proc_title, 0, 0, 0) < 0)
        LOG_ERRNO("render worker %d: failed to set process title", my_id);

    sem_t *start = &term->render.workers.start;
    sem_t *done = &term->render.workers.done;
    mtx_t *lock = &term->render.workers.lock;
    cnd_t *cond = &term->render.workers.cond;

    while (true) {
        sem_wait(start);

        struct buffer *buf = term->render.workers.buf;
        bool frame_done = false;

        while (!frame_done) {
            mtx_lock(lock);
            while (tll_length(term->render.workers.queue) == 0)
                cnd_wait(cond, lock);

            int row_no = tll_pop_front(term->render.workers.queue);
            mtx_unlock(lock);

            switch (row_no) {
            default:
                assert(buf != NULL);
                render_row(term, buf->pix, grid_row_in_view(term->grid, row_no), row_no);
                break;

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

struct csd_data {
    int x;
    int y;
    int width;
    int height;
};

static struct csd_data
get_csd_data(const struct terminal *term, enum csd_surface surf_idx)
{
    assert(term->window->use_csd == CSD_YES);

    /* Only title bar is rendered in maximized mode */
    const int border_width = !term->window->is_maximized
        ? term->conf->csd.border_width * term->scale : 0;

    const int title_height = !term->window->is_fullscreen
        ? term->conf->csd.title_height * term->scale : 0;

    const int button_width = !term->window->is_fullscreen
        ? term->conf->csd.button_width * term->scale : 0;

    switch (surf_idx) {
    case CSD_SURF_TITLE:  return (struct csd_data){            0,                -title_height,                    term->width,                title_height};
    case CSD_SURF_LEFT:   return (struct csd_data){-border_width,                -title_height,                   border_width, title_height + term->height};
    case CSD_SURF_RIGHT:  return (struct csd_data){  term->width,                -title_height,                   border_width, title_height + term->height};
    case CSD_SURF_TOP:    return (struct csd_data){-border_width, -title_height - border_width, term->width + 2 * border_width,                border_width};
    case CSD_SURF_BOTTOM: return (struct csd_data){-border_width,                 term->height, term->width + 2 * border_width,                border_width};

    /* Positioned relative to CSD_SURF_TITLE */
    case CSD_SURF_MINIMIZE: return (struct csd_data){term->width - 3 * button_width, 0, button_width, title_height};
    case CSD_SURF_MAXIMIZE: return (struct csd_data){term->width - 2 * button_width, 0, button_width, title_height};
    case CSD_SURF_CLOSE:    return (struct csd_data){term->width - 1 * button_width, 0, button_width, title_height};

    case CSD_SURF_COUNT:
        assert(false);
        return (struct csd_data){0};
    }

    assert(false);
    return (struct csd_data){0};
}

static void
csd_commit(struct terminal *term, struct wl_surface *surf, struct buffer *buf)
{
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
    assert(term->window->use_csd == CSD_YES);

    pixman_image_t *src = pixman_image_create_solid_fill(color);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, color, 1,
        &(pixman_rectangle16_t){0, 0, buf->width, buf->height});
    pixman_image_unref(src);
}

void
render_csd_title(struct terminal *term)
{
    if (term->window->use_csd != CSD_YES)
        return;

    struct csd_data info = get_csd_data(term, CSD_SURF_TITLE);
    struct wl_surface *surf = term->window->csd.surface[CSD_SURF_TITLE];

    assert(info.width > 0 && info.height > 0);

    unsigned long cookie = shm_cookie_csd(term, CSD_SURF_TITLE);
    struct buffer *buf = shm_get_buffer(
        term->wl->shm, info.width, info.height, cookie);

    uint32_t _color = term->colors.fg;
    uint16_t alpha = 0xffff;

    if (term->conf->csd.color.title_set) {
        _color = term->conf->csd.color.title;
        alpha = _color >> 24 | (_color >> 24 << 8);
    }

    pixman_color_t color = color_hex_to_pixman_with_alpha(_color, alpha);
    if (!term->visual_focus)
        pixman_color_dim(&color);

    render_csd_part(term, surf, buf, info.width, info.height, &color);
    csd_commit(term, surf, buf);
}

static void
render_csd_border(struct terminal *term, enum csd_surface surf_idx)
{
    assert(surf_idx >= CSD_SURF_LEFT && surf_idx <= CSD_SURF_BOTTOM);

    if (term->window->use_csd != CSD_YES)
        return;

    struct csd_data info = get_csd_data(term, surf_idx);
    struct wl_surface *surf = term->window->csd.surface[surf_idx];

    if (info.width == 0 || info.height == 0)
        return;

    unsigned long cookie = shm_cookie_csd(term, surf_idx);
    struct buffer *buf = shm_get_buffer(
        term->wl->shm, info.width, info.height, cookie);

    uint32_t _color = 0;
    uint16_t alpha = 0;

    if (term->conf->csd.color.border_set) {
        _color = term->conf->csd.color.border;
        alpha = _color >> 24 | (_color >> 24 << 8);
    }

    pixman_color_t color = color_hex_to_pixman_with_alpha(_color, alpha);
    if (!term->visual_focus)
        pixman_color_dim(&color);
    render_csd_part(term, surf, buf, info.width, info.height, &color);
    csd_commit(term, surf, buf);
}

static void
render_csd_button_minimize(struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = color_hex_to_pixman(0);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    int x_margin = (term->conf->csd.button_width * 1 / 4) * term->scale;
    int y_margin = (term->conf->csd.title_height * 4 / 6) * term->scale;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &color, 1,
        &(pixman_rectangle16_t){x_margin, y_margin, buf->width - 2 * x_margin, 1 * term->scale});

    pixman_image_unref(src);
}

static void
render_csd_button_maximize(struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = color_hex_to_pixman(0);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    int x_margin = (term->conf->csd.button_width * 1 / 4) * term->scale;
    int y_margin = (term->conf->csd.title_height * 2 / 6) * term->scale;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &color, 1,
        &(pixman_rectangle16_t){x_margin, y_margin, buf->width - 2 * x_margin, 2 * term->scale});

    pixman_image_unref(src);
}

static void
render_csd_button_close(struct terminal *term, struct buffer *buf)
{
    pixman_color_t color = color_hex_to_pixman(0);
    pixman_image_t *src = pixman_image_create_solid_fill(&color);

    int min_length = min(buf->width, buf->height);
    int length = min_length / 2;

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &color, 2,
        (pixman_rectangle16_t[]){
            {(buf->width - length) / 2, buf->height / 2, length, 1 * term->scale},
            {buf->width / 2, (buf->height - length) / 2, 1 * term->scale, length},
        });

    pixman_image_unref(src);
}

void
render_csd_button(struct terminal *term, enum csd_surface surf_idx)
{
    assert(surf_idx >= CSD_SURF_MINIMIZE);

    if (term->window->use_csd != CSD_YES)
        return;

    struct csd_data info = get_csd_data(term, surf_idx);
    struct wl_surface *surf = term->window->csd.surface[surf_idx];

    if (info.width == 0 || info.height == 0)
        return;

    unsigned long cookie = shm_cookie_csd(term, surf_idx);
    struct buffer *buf = shm_get_buffer(
        term->wl->shm, info.width, info.height, cookie);

    uint32_t _color;
    uint16_t alpha = 0xffff;
    bool is_active = false;
    const bool *is_set = NULL;
    const uint32_t *conf_color = NULL;

    switch (surf_idx) {
    case CSD_SURF_MINIMIZE:
        _color = 0xff0000ff;
        is_set = &term->conf->csd.color.minimize_set;
        conf_color = &term->conf->csd.color.minimize;
        is_active = term->active_surface == TERM_SURF_BUTTON_MINIMIZE;
        break;

    case CSD_SURF_MAXIMIZE:
        _color = 0xff00ff00;
        is_set = &term->conf->csd.color.maximize_set;
        conf_color = &term->conf->csd.color.maximize;
        is_active = term->active_surface == TERM_SURF_BUTTON_MAXIMIZE;
        break;

    case CSD_SURF_CLOSE:
        _color = 0xffff0000;
        is_set = &term->conf->csd.color.close_set;
        conf_color = &term->conf->csd.color.close;
        is_active = term->active_surface == TERM_SURF_BUTTON_CLOSE;
        break;

    default:
        assert(false);
        break;
    }

    if (is_active) {
        if (*is_set) {
            _color = *conf_color;
            alpha = _color >> 24 | (_color >> 24 << 8);
        }
    } else {
        _color = 0;
        alpha = 0;
    }

    pixman_color_t color = color_hex_to_pixman_with_alpha(_color, alpha);
    if (!term->visual_focus)
        pixman_color_dim(&color);
    render_csd_part(term, surf, buf, info.width, info.height, &color);

    switch (surf_idx) {
    case CSD_SURF_MINIMIZE: render_csd_button_minimize(term, buf); break;
    case CSD_SURF_MAXIMIZE: render_csd_button_maximize(term, buf); break;
    case CSD_SURF_CLOSE:    render_csd_button_close(term, buf); break;
        break;

    default:
        assert(false);
        break;
    }

    csd_commit(term, surf, buf);
}

void
render_csd(struct terminal *term)
{
    if (term->window->use_csd != CSD_YES)
        return;

    for (size_t i = 0; i < CSD_SURF_COUNT; i++) {
        struct csd_data info = get_csd_data(term, i);
        const int x = info.x;
        const int y = info.y;
        const int width = info.width;
        const int height = info.height;

        struct wl_surface *surf = term->window->csd.surface[i];
        struct wl_subsurface *sub = term->window->csd.sub_surface[i];

        if (width == 0 || height == 0) {
            /* CSD borders aren't rendered in maximized mode */
            assert(term->window->is_maximized || term->window->is_fullscreen);
            wl_subsurface_set_position(sub, 0, 0);
            wl_surface_attach(surf, NULL, 0, 0);
            wl_surface_commit(surf);
            continue;
        }

        wl_subsurface_set_position(sub, x / term->scale, y / term->scale);
    }

    render_csd_title(term);
    for (size_t i = CSD_SURF_LEFT; i <= CSD_SURF_BOTTOM; i++)
        render_csd_border(term, i);
    for (size_t i = CSD_SURF_MINIMIZE; i < CSD_SURF_COUNT; i++)
        render_csd_button(term, i);
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static void
grid_render(struct terminal *term)
{
    if (term->is_shutting_down)
        return;

#define TIME_FRAME_RENDERING 0

#if TIME_FRAME_RENDERING
    struct timeval start_time;
    gettimeofday(&start_time, NULL);
#endif

    assert(term->width > 0);
    assert(term->height > 0);

    unsigned long cookie = shm_cookie_grid(term);
    struct buffer *buf = shm_get_buffer(
        term->wl->shm, term->width, term->height, cookie);

    wl_surface_attach(term->window->surface, buf->wl_buf, 0, 0);

    pixman_image_t *pix = buf->pix;

    /* If we resized the window, or is flashing, or just stopped flashing */
    if (term->render.last_buf != buf ||
        term->flash.active || term->render.was_flashing ||
        term->is_searching != term->render.was_searching)
    {
        if (term->render.last_buf != NULL &&
            term->render.last_buf->width == buf->width &&
            term->render.last_buf->height == buf->height &&
            !term->flash.active &&
            !term->render.was_flashing &&
            term->is_searching == term->render.was_searching)
        {
            static bool has_warned = false;
            if (!has_warned) {
                LOG_WARN("it appears your Wayland compositor does not support buffer re-use for SHM clients; expect lower performance.");
                has_warned = true;
            }

            assert(term->render.last_buf->size == buf->size);
            memcpy(buf->mmapped, term->render.last_buf->mmapped, buf->size);
        }

        else {
            /* Fill area outside the cell grid with the default background color */
            int rmargin = term->width - term->margins.right;
            int bmargin = term->height - term->margins.bottom;

            uint32_t _bg = !term->reverse ? term->colors.bg : term->colors.fg;
            pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, term->colors.alpha);
            if (term->is_searching)
                pixman_color_dim(&bg);

            pixman_image_fill_rectangles(
                PIXMAN_OP_SRC, pix, &bg, 4,
                (pixman_rectangle16_t[]){
                {0, 0, term->width, term->margins.top},            /* Top */
                {0, 0, term->margins.left, term->height},          /* Left */
                {rmargin, 0, term->margins.right, term->height},   /* Right */
                {0, bmargin, term->width, term->margins.bottom}}); /* Bottom */

            wl_surface_damage_buffer(
                term->window->surface, 0, 0, term->width, term->margins.top);
            wl_surface_damage_buffer(
                term->window->surface, 0, 0, term->margins.left, term->height);
            wl_surface_damage_buffer(
                term->window->surface, rmargin, 0, term->margins.right, term->height);
            wl_surface_damage_buffer(
                term->window->surface, 0, bmargin, term->width, term->margins.bottom);

            /* Force a full grid refresh */
            term_damage_view(term);
        }

        term->render.last_buf = buf;
        term->render.was_flashing = term->flash.active;
        term->render.was_searching = term->is_searching;
    }

    /* Erase old cursor (if we rendered a cursor last time) */
    if (term->render.last_cursor.cell != NULL) {

        struct cell *cell = term->render.last_cursor.cell;
        struct coord at = term->render.last_cursor.in_view;
        term->render.last_cursor.cell = NULL;

        /* If cell is already dirty, it will be rendered anyway */
        if (cell->attrs.clean) {
            cell->attrs.clean = 0;
            int cols = render_cell(term, pix, cell, at.col, at.row, false);

            wl_surface_damage_buffer(
                term->window->surface,
                term->margins.left + at.col * term->cell_width,
                term->margins.top + at.row * term->cell_height,
                cols * term->cell_width, term->cell_height);
        }
    }

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

    if (term->render.workers.count > 0) {

        term->render.workers.buf = buf;
        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_post(&term->render.workers.start);

        assert(tll_length(term->render.workers.queue) == 0);

        for (int r = 0; r < term->rows; r++) {
            struct row *row = grid_row_in_view(term->grid, r);

            if (!row->dirty)
                continue;

            mtx_lock(&term->render.workers.lock);
            tll_push_back(term->render.workers.queue, r);
            cnd_signal(&term->render.workers.cond);
            mtx_unlock(&term->render.workers.lock);

            row->dirty = false;

            wl_surface_damage_buffer(
                term->window->surface,
                term->margins.left, term->margins.top + r * term->cell_height,
                term->width - term->margins.left - term->margins.right, term->cell_height);
        }

        mtx_lock(&term->render.workers.lock);
        for (size_t i = 0; i < term->render.workers.count; i++)
            tll_push_back(term->render.workers.queue, -1);
        cnd_broadcast(&term->render.workers.cond);
        mtx_unlock(&term->render.workers.lock);
    } else {
        for (int r = 0; r < term->rows; r++) {
            struct row *row = grid_row_in_view(term->grid, r);

            if (!row->dirty)
                continue;

            render_row(term, pix, row, r);
            row->dirty = false;

            wl_surface_damage_buffer(
                term->window->surface,
                term->margins.left, term->margins.top + r * term->cell_height,
                term->width - term->margins.left - term->margins.right, term->cell_height);
        }
    }

    /*
     * Determine if we need to render a cursor or not. The cursor
     * could be hidden. Or it could have been scrolled out of view.
     */
    bool cursor_is_visible = false;
    int view_end = (term->grid->view + term->rows - 1) & (term->grid->num_rows - 1);
    int cursor_row = (term->grid->offset + term->cursor.point.row) & (term->grid->num_rows - 1);
    if (view_end >= term->grid->view) {
        /* Not wrapped */
        if (cursor_row >= term->grid->view && cursor_row <= view_end)
            cursor_is_visible = true;
    } else {
        /* Wrapped */
        if (cursor_row >= term->grid->view || cursor_row <= view_end)
            cursor_is_visible = true;
    }

    /*
     * Wait for workers to finish before we render the cursor. This is
     * because the cursor cell might be dirty, in which case a worker
     * will render it (but without the cursor).
     */
    if (term->render.workers.count > 0) {
        for (size_t i = 0; i < term->render.workers.count; i++)
            sem_wait(&term->render.workers.done);
        term->render.workers.buf = NULL;
    }

    if (cursor_is_visible && !term->hide_cursor) {
        /* Remember cursor coordinates so that we can erase it next
         * time. Note that we need to re-align it against the view. */
        int view_aligned_row
            = (cursor_row - term->grid->view + term->grid->num_rows) & (term->grid->num_rows - 1);

        term->render.last_cursor.actual = term->cursor.point;
        term->render.last_cursor.in_view = (struct coord) {
            term->cursor.point.col, view_aligned_row};

        struct row *row = grid_row_in_view(term->grid, view_aligned_row);
        struct cell *cell = &row->cells[term->cursor.point.col];

        cell->attrs.clean = 0;
        term->render.last_cursor.cell = cell;
        int cols_updated = render_cell(
            term, pix, cell, term->cursor.point.col, view_aligned_row, true);

        wl_surface_damage_buffer(
            term->window->surface,
            term->margins.left + term->cursor.point.col * term->cell_width,
            term->margins.top + view_aligned_row * term->cell_height,
            cols_updated * term->cell_width, term->cell_height);
    }

    render_sixel_images(term, pix);

    if (term->flash.active) {
        /* Note: alpha is pre-computed in each color component */
        /* TODO: dim while searching */
        pixman_image_fill_rectangles(
            PIXMAN_OP_OVER, pix,
            &(pixman_color_t){.red=0x7fff, .green=0x7fff, .blue=0, .alpha=0x7fff},
            1, &(pixman_rectangle16_t){0, 0, term->width, term->height});

        wl_surface_damage_buffer(
            term->window->surface, 0, 0, term->width, term->height);
    }

    assert(term->grid->offset >= 0 && term->grid->offset < term->grid->num_rows);
    assert(term->grid->view >= 0 && term->grid->view < term->grid->num_rows);

    assert(term->window->frame_callback == NULL);
    term->window->frame_callback = wl_surface_frame(term->window->surface);
    wl_callback_add_listener(term->window->frame_callback, &frame_listener, term);

    wl_surface_set_buffer_scale(term->window->surface, term->scale);

    if (term->wl->presentation != NULL && term->render.presentation_timings) {
        struct timespec commit_time;
        clock_gettime(term->wl->presentation_clock_id, &commit_time);

        struct wp_presentation_feedback *feedback = wp_presentation_feedback(
            term->wl->presentation, term->window->surface);

        if (feedback == NULL) {
            LOG_WARN("failed to create presentation feedback");
        } else {
            struct presentation_context *ctx = malloc(sizeof(*ctx));
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

    wl_surface_commit(term->window->surface);

#if TIME_FRAME_RENDERING
    struct timeval end_time;
    gettimeofday(&end_time, NULL);

    struct timeval render_time;
    timersub(&end_time, &start_time, &render_time);
    LOG_INFO("frame rendered in %lds %ldus",
             render_time.tv_sec, render_time.tv_usec);
#endif
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct terminal *term = data;

    assert(term->window->frame_callback == wl_callback);
    wl_callback_destroy(wl_callback);
    term->window->frame_callback = NULL;

    if (term->render.pending) {
        term->render.pending = false;
        grid_render(term);
    }
}

void
render_search_box(struct terminal *term)
{
    assert(term->window->search_sub_surface != NULL);

    const size_t wanted_visible_chars = max(20, term->search.len);

    assert(term->scale >= 1);
    const int scale = term->scale;

    const size_t margin = 3 * scale;

    const size_t width = term->width - 2 * margin;
    const size_t visible_width = min(
        term->width - 2 * margin,
        2 * margin + wanted_visible_chars * term->cell_width);
    const size_t height = min(
        term->height - 2 * margin,
        2 * margin + 1 * term->cell_height);

    const size_t visible_chars = (visible_width - 2 * margin) / term->cell_width;
    size_t glyph_offset = term->render.search_glyph_offset;

    unsigned long cookie = shm_cookie_search(term);
    struct buffer *buf = shm_get_buffer(term->wl->shm, width, height, cookie);

    /* Background - yellow on empty/match, red on mismatch */
    pixman_color_t color = color_hex_to_pixman(
        term->search.match_len == term->search.len
        ? term->colors.table[3] : term->colors.table[1]);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &color,
        1, &(pixman_rectangle16_t){width - visible_width, 0, visible_width, height});

    pixman_color_t transparent = color_hex_to_pixman_with_alpha(0, 0);
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &transparent,
        1, &(pixman_rectangle16_t){0, 0, width - visible_width, height});

    struct font *font = term->fonts[0];
    int x = width - visible_width + margin;
    int y = margin;
    pixman_color_t fg = color_hex_to_pixman(term->colors.table[0]);

    if (term->search.cursor < glyph_offset ||
        term->search.cursor >= glyph_offset + visible_chars + 1)
    {
        /* Make sure cursor is always visible */
        term->render.search_glyph_offset = glyph_offset = term->search.cursor;
    }

    /* Text (what the user entered - *not* match(es)) */
    for (size_t i = glyph_offset;
         i < term->search.len && i - glyph_offset < visible_chars;
         i++)
    {
        if (i == term->search.cursor)
            draw_bar(term, buf->pix, font, &fg, x, y);

        const struct glyph *glyph = font_glyph_for_wc(font, term->search.buf[i], true);
        if (glyph == NULL)
            continue;

        pixman_image_t *src = pixman_image_create_solid_fill(&fg);
        pixman_image_composite32(
            PIXMAN_OP_OVER, src, glyph->pix, buf->pix, 0, 0, 0, 0,
            x + glyph->x, y + font_baseline(term) - glyph->y,
            glyph->width, glyph->height);
        pixman_image_unref(src);

        x += term->cell_width;
    }

    if (term->search.cursor >= term->search.len)
        draw_bar(term, buf->pix, font, &fg, x, y);

    quirk_weston_subsurface_desync_on(term->window->search_sub_surface);

    /* TODO: this is only necessary on a window resize */
    wl_subsurface_set_position(
        term->window->search_sub_surface,
        margin / scale,
        max(0, (int32_t)term->height - height - margin) / scale);

    wl_surface_attach(term->window->search_surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(term->window->search_surface, 0, 0, width, height);
    wl_surface_set_buffer_scale(term->window->search_surface, scale);

    struct wl_region *region = wl_compositor_create_region(term->wl->compositor);
    if (region != NULL) {
        wl_region_add(region, width - visible_width, 0, visible_width, height);
        wl_surface_set_opaque_region(term->window->search_surface, region);
        wl_region_destroy(region);
    }

    wl_surface_commit(term->window->search_surface);
    quirk_weston_subsurface_desync_off(term->window->search_sub_surface);
}

/* Move to terminal.c? */
static bool
maybe_resize(struct terminal *term, int width, int height, bool force)
{
    if (!term->window->is_configured)
        return false;

    if (term->cell_width == 0 && term->cell_height == 0)
        return false;

    int scale = -1;
    tll_foreach(term->window->on_outputs, it) {
        if (it->item->scale > scale)
            scale = it->item->scale;
    }

    if (scale == -1) {
        /* Haven't 'entered' an output yet? */
        scale = 1;
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
        if (term->unmaximized_width != 0 && term->unmaximized_height != 0) {
            width = term->unmaximized_width;
            height = term->unmaximized_height;
        } else {
            width = term->conf->width;
            height = term->conf->height;

            if (term->window->use_csd == CSD_YES) {
                assert(!term->window->is_fullscreen);

                /* Account for CSDs, to make actual window size match
                 * the configured size */
                if (!term->window->is_maximized) {
                    width -= 2 * term->conf->csd.border_width;
                    height -= 2 * term->conf->csd.border_width + term->conf->csd.title_height;
                } else {
                    height -= term->conf->csd.title_height;
                }
            }

            width *= scale;
            height *= scale;
        }
    }

    /* Don't shrink grid too much */
    const int min_cols = 20;
    const int min_rows = 4;

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

    selection_cancel(term);

    /* Cancel an application initiated "Synchronized Update" */
    term_disable_app_sync_updates(term);

    term->width = width;
    term->height = height;
    term->scale = scale;

    const int scrollback_lines = term->render.scrollback_lines;

    /* Screen rows/cols before resize */
    const int old_cols = term->cols;
    const int old_rows = term->rows;

    /* Screen rows/cols after resize */
    const int new_cols = (term->width - 2 * pad_x) / term->cell_width;
    const int new_rows = (term->height - 2 * pad_y) / term->cell_height;

    /* Grid rows/cols after resize */
    const int new_normal_grid_rows = 1 << (32 - __builtin_clz(new_rows + scrollback_lines - 1));
    const int new_alt_grid_rows = 1 << (32  - __builtin_clz(new_rows));

    assert(new_cols >= 1);
    assert(new_rows >= 1);

    /* Margins */
    term->margins.left = pad_x;
    term->margins.top = pad_y;
    term->margins.right = term->width - new_cols * term->cell_width - term->margins.left;
    term->margins.bottom = term->height - new_rows * term->cell_height - term->margins.top;

    assert(term->margins.left >= pad_x);
    assert(term->margins.right >= pad_x);
    assert(term->margins.top >= pad_y);
    assert(term->margins.bottom >= pad_y);

    if (new_cols == old_cols && new_rows == old_rows) {
        LOG_DBG("grid layout unaffected; skipping reflow");
        goto damage_view;
    }

    /* Reflow grids */
    int last_normal_row = grid_reflow(
        &term->normal, new_normal_grid_rows, new_cols, old_rows, new_rows);
    int last_alt_row = grid_reflow(
        &term->alt, new_alt_grid_rows, new_cols, old_rows, new_rows);

    /* Reset tab stops */
    tll_free(term->tab_stops);
    for (int c = 0; c < new_cols; c += 8)
        tll_push_back(term->tab_stops, c);

    term->cols = new_cols;
    term->rows = new_rows;

    LOG_DBG("resize: %dx%d, grid: cols=%d, rows=%d "
            "(left-margin=%d, right-margin=%d, top-margin=%d, bottom-margin=%d)",
            term->width, term->height, term->cols, term->rows,
            term->margins.left, term->margins.right, term->margins.top, term->margins.bottom);

    /* Signal TIOCSWINSZ */
    if (ioctl(term->ptmx, TIOCSWINSZ,
              &(struct winsize){
                  .ws_row = term->rows,
                  .ws_col = term->cols,
                  .ws_xpixel = term->cols * term->cell_width,
                  .ws_ypixel = term->rows * term->cell_height}) == -1)
    {
        LOG_ERRNO("TIOCSWINSZ");
    }

    if (term->scroll_region.start >= term->rows)
        term->scroll_region.start = 0;

    if (term->scroll_region.end >= old_rows)
        term->scroll_region.end = term->rows;

    /* Position cursor at the last copied row */
    /* TODO: can we do better? */
    int cursor_row = term->grid == &term->normal
        ? last_normal_row - term->normal.offset
        : last_alt_row - term->alt.offset;

    while (cursor_row < 0)
        cursor_row += term->grid->num_rows;

    assert(cursor_row >= 0);
    assert(cursor_row < term->rows);

    term_cursor_to(
        term,
        cursor_row,
        min(term->cursor.point.col, term->cols - 1));

    term->render.last_cursor.cell = NULL;

damage_view:
    if (!term->window->is_maximized && !term->window->is_fullscreen) {
        term->unmaximized_width = term->width;
        term->unmaximized_height = term->height;
    }

    xdg_toplevel_set_min_size(
        term->window->xdg_toplevel, min_width / scale, min_height / scale);

    render_csd(term);
    if (term->is_searching)
        render_search_box(term);

    tll_free(term->normal.scroll_damage);
    tll_free(term->alt.scroll_damage);

    term->render.last_buf = NULL;
    term_damage_view(term);
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

static void
render_xcursor_update(struct wayland *wayl, const struct terminal *term)
{
    /* If called from a frame callback, we may no longer have mouse focus */
    if (wayl->mouse_focus != term)
        return;

    wayl->pointer.cursor = wl_cursor_theme_get_cursor(wayl->pointer.theme, term->xcursor);
    if (wayl->pointer.cursor == NULL) {
        LOG_ERR("%s: failed to load xcursor pointer '%s'",
                wayl->pointer.theme_name, term->xcursor);
        return;
    }

    wayl->pointer.xcursor = term->xcursor;

    const int scale = term->scale;
    struct wl_cursor_image *image = wayl->pointer.cursor->images[0];

    wl_surface_attach(
        wayl->pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        wayl->pointer.pointer, wayl->pointer.serial,
        wayl->pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        wayl->pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_set_buffer_scale(wayl->pointer.surface, scale);

    assert(wayl->pointer.xcursor_callback == NULL);
    wayl->pointer.xcursor_callback = wl_surface_frame(wayl->pointer.surface);
    wl_callback_add_listener(wayl->pointer.xcursor_callback, &xcursor_listener, wayl);

    wl_surface_commit(wayl->pointer.surface);
}

static void
xcursor_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct wayland *wayl = data;

    assert(wayl->pointer.xcursor_callback == wl_callback);
    wl_callback_destroy(wl_callback);
    wayl->pointer.xcursor_callback = NULL;

    if (wayl->pointer.pending_terminal != NULL) {
        render_xcursor_update(wayl, wayl->pointer.pending_terminal);
        wayl->pointer.pending_terminal = NULL;
    }
}

static void
fdm_hook_refresh_pending_terminals(struct fdm *fdm, void *data)
{
    struct renderer *renderer = data;
    struct wayland *wayl = renderer->wayl;

    tll_foreach(renderer->wayl->terms, it) {
        struct terminal *term = it->item;

        if (!term->render.refresh_needed)
            continue;

        if (term->render.app_sync_updates.enabled)
            continue;

        assert(term->window->is_configured);
        term->render.refresh_needed = false;

        if (term->window->frame_callback == NULL)
            grid_render(term);
        else {
            /* Tells the frame callback to render again */
            term->render.pending = true;
        }
    }

    if (wayl->pointer.pending_terminal != NULL) {
        if (wayl->pointer.xcursor_callback == NULL) {
            render_xcursor_update(wayl, wayl->pointer.pending_terminal);
            wayl->pointer.pending_terminal = NULL;
        } else {
            /* Frame callback will call render_xcursor_update() */
        }
    }
}

void
render_set_title(struct terminal *term, const char *_title)
{
    /* TODO: figure out what the limit actually is */
    static const size_t max_len = 100;

    const char *title = _title;
    char *copy = NULL;

    if (strlen(title) > max_len) {
        copy = strndup(_title, max_len);
        title = copy;
    }

    xdg_toplevel_set_title(term->window->xdg_toplevel, title);
    free(copy);
}

void
render_refresh(struct terminal *term)
{
    term->render.refresh_needed = true;
}

bool
render_xcursor_set(struct terminal *term)
{
    struct wayland *wayl = term->wl;

    if (wayl->pointer.theme == NULL)
        return false;

    if (wayl->mouse_focus == NULL) {
        wayl->pointer.xcursor = NULL;
        wayl->pointer.pending_terminal = NULL;
        return true;
    }

    if (wayl->mouse_focus != term) {
        /* This terminal doesn't have mouse focus */
        return true;
    }

    if (wayl->pointer.xcursor == term->xcursor)
        return true;

    /* FDM hook takes care of actual rendering */
    wayl->pointer.pending_terminal = term;
    return true;
}
