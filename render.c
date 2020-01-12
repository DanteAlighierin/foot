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
#include "shm.h"
#include "grid.h"

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

static void
presented(void *data,
          struct wp_presentation_feedback *wp_presentation_feedback,
          uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
          uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    struct terminal *term = data;

    if (term->render.commit_time.tv_sec == 0 && term->render.commit_time.tv_nsec == 0)
        return;

    const struct timeval input = {
        .tv_sec = term->render.input_time.tv_sec,
        .tv_usec = term->render.input_time.tv_nsec / 1000,
    };

    const struct timeval commit = {
        .tv_sec = term->render.commit_time.tv_sec,
        .tv_usec = term->render.commit_time.tv_nsec / 1000,
    };

    const struct timeval presented = {
        .tv_sec = (uint64_t)tv_sec_hi << 32 | tv_sec_lo,
        .tv_usec = tv_nsec / 1000,
    };

    bool use_input = (input.tv_sec > 0 || input.tv_usec > 0) &&
        timercmp(&presented, &input, >);
    char msg[1024];
    int chars = 0;

    if (use_input && timercmp(&presented, &input, <))
        return;
    else if (timercmp(&presented, &commit, <))
        return;

    LOG_DBG("commit: %lu s %lu µs, presented: %lu s %lu µs",
            commit.tv_sec, commit.tv_usec, presented.tv_sec, presented.tv_usec);

    if (use_input) {
        struct timeval diff;
        timersub(&commit, &input, &diff);
        chars += snprintf(&msg[chars], sizeof(msg) - chars,
                          "input - %lu µs -> ", diff.tv_usec);
    }

    struct timeval diff;
    timersub(&presented, &commit, &diff);
    chars += snprintf(&msg[chars], sizeof(msg) - chars,
                      "commit - %lu µs -> ", diff.tv_usec);

    if (use_input) {
        assert(timercmp(&presented, &input, >));
        timersub(&presented, &input, &diff);
    } else {
        assert(timercmp(&presented, &commit, >));
        timersub(&presented, &commit, &diff);
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
    memset(&term->render.input_time, 0, sizeof(term->render.input_time));
    memset(&term->render.commit_time, 0, sizeof(term->render.commit_time));
}

static void
discarded(void *data, struct wp_presentation_feedback *wp_presentation_feedback)
{
    struct terminal *term = data;
    wp_presentation_feedback_destroy(wp_presentation_feedback);
    memset(&term->render.input_time, 0, sizeof(term->render.input_time));
    memset(&term->render.commit_time, 0, sizeof(term->render.commit_time));
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
        assert(term->cursor_color.text);

        cursor_color = color_hex_to_pixman(term->cursor_color.cursor);
        text_color = color_hex_to_pixman(term->cursor_color.text);

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
    int x = term->x_margin + col * width;
    int y = term->y_margin + row * height;

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
        ? font_glyph_for_wc(font, cell->wc) : NULL;

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
    int dst_y = term->y_margin + (dmg->scroll.region.start + 0) * term->cell_height;
    int src_y = term->y_margin + (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
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
            term->window->surface, term->x_margin, dst_y, term->width - term->x_margin, height);
    }
}

static void
grid_render_scroll_reverse(struct terminal *term, struct buffer *buf,
                           const struct damage *dmg)
{
    int src_y = term->y_margin + (dmg->scroll.region.start + 0) * term->cell_height;
    int dst_y = term->y_margin + (dmg->scroll.region.start + dmg->scroll.lines) * term->cell_height;
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
            term->window->surface, term->x_margin, dst_y, term->width - term->x_margin, height);
    }
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

    unsigned long cookie = (uintptr_t)term;
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
            int rmargin = term->x_margin + term->cols * term->cell_width;
            int bmargin = term->y_margin + term->rows * term->cell_height;
            int rmargin_width = term->width - rmargin;
            int bmargin_height = term->height - bmargin;

            uint32_t _bg = !term->reverse ? term->colors.bg : term->colors.fg;
            pixman_color_t bg = color_hex_to_pixman_with_alpha(_bg, term->colors.alpha);
            if (term->is_searching)
                pixman_color_dim(&bg);

            pixman_image_fill_rectangles(
                PIXMAN_OP_SRC, pix, &bg, 4,
                (pixman_rectangle16_t[]){
                {0, 0, term->width, term->y_margin},            /* Top */
                {0, 0, term->x_margin, term->height},           /* Left */
                {rmargin, 0, rmargin_width, term->height},      /* Right */
                {0, bmargin, term->width, bmargin_height}});    /* Bottom */

            wl_surface_damage_buffer(
                term->window->surface, 0, 0, term->width, term->y_margin);
            wl_surface_damage_buffer(
                term->window->surface, 0, 0, term->x_margin, term->height);
            wl_surface_damage_buffer(
                term->window->surface, rmargin, 0, rmargin_width, term->height);
            wl_surface_damage_buffer(
                term->window->surface, 0, bmargin, term->width, bmargin_height);

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
                term->x_margin + at.col * term->cell_width,
                term->y_margin + at.row * term->cell_height,
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
                term->x_margin, term->y_margin + r * term->cell_height,
                term->width - term->x_margin, term->cell_height);
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
                term->x_margin, term->y_margin + r * term->cell_height,
                term->width - term->x_margin, term->cell_height);
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
            term->x_margin + term->cursor.point.col * term->cell_width,
            term->y_margin + view_aligned_row * term->cell_height,
            cols_updated * term->cell_width, term->cell_height);
    }

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
        clock_gettime(term->wl->presentation_clock_id, &term->render.commit_time);

        struct wp_presentation_feedback *feedback = wp_presentation_feedback(
            term->wl->presentation, term->window->surface);

        if (feedback == NULL) {
            LOG_WARN("failed to create presentation feedback");
        } else {
            wp_presentation_feedback_add_listener(
                feedback, &presentation_feedback_listener, term);
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

    const int scale = term->scale >= 1 ? term->scale : 1;
    const size_t margin = scale * 3;

    const size_t width = min(
        term->width - 2 * margin,
        2 * margin + wanted_visible_chars * term->cell_width);
    const size_t height = min(
        term->height - 2 * margin,
        2 * margin + 1 * term->cell_height);

    const size_t visible_chars = (width - 2 * margin) / term->cell_width;
    size_t glyph_offset = term->render.search_glyph_offset;

    unsigned long cookie = (uintptr_t)term + 1;
    struct buffer *buf = shm_get_buffer(term->wl->shm, width, height, cookie);

    /* Background - yellow on empty/match, red on mismatch */
    pixman_color_t color = color_hex_to_pixman(
        term->search.match_len == term->search.len
        ? term->colors.table[3] : term->colors.table[1]);

    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &color,
        1, &(pixman_rectangle16_t){0, 0, width, height});

    struct font *font = term->fonts[0];
    int x = margin;
    int y = margin;
    pixman_color_t fg = color_hex_to_pixman(term->colors.table[0]);

    if (term->search.cursor < glyph_offset ||
        term->search.cursor >= glyph_offset + visible_chars + 2)
    {
        /* Make sure cursor is always visible */
        term->render.search_glyph_offset = glyph_offset = term->search.cursor;
    }

    /* Text (what the user entered - *not* match(es)) */
    for (size_t i = glyph_offset;
         i < term->search.len && i - glyph_offset < visible_chars + 1;
         i++)
    {
        if (i == term->search.cursor)
            draw_bar(term, buf->pix, font, &fg, x, y);

        const struct glyph *glyph = font_glyph_for_wc(font, term->search.buf[i]);
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

    wl_subsurface_set_position(
        term->window->search_sub_surface,
        max(0, (int32_t)term->width - width - margin),
        max(0, (int32_t)term->height - height - margin));

    wl_surface_damage_buffer(term->window->search_surface, 0, 0, width, height);
    wl_surface_attach(term->window->search_surface, buf->wl_buf, 0, 0);
    wl_surface_set_buffer_scale(term->window->search_surface, scale);
    wl_surface_commit(term->window->search_surface);
}

static void
reflow(struct row **new_grid, int new_cols, int new_rows,
       struct row *const *old_grid, int old_cols, int old_rows)
{
    /* TODO: actually reflow */
    for (int r = 0; r < min(new_rows, old_rows); r++) {
        size_t copy_cols = min(new_cols, old_cols);
        size_t clear_cols = new_cols - copy_cols;

        if (old_grid[r] == NULL)
            continue;

        if (new_grid[r] == NULL)
            new_grid[r] = grid_row_alloc(new_cols, false);

        struct cell *new_cells = new_grid[r]->cells;
        const struct cell *old_cells = old_grid[r]->cells;

        new_grid[r]->dirty = old_grid[r]->dirty;
        memcpy(new_cells, old_cells, copy_cols * sizeof(new_cells[0]));
        memset(&new_cells[copy_cols], 0, clear_cols * sizeof(new_cells[0]));
    }
}

/* Move to terminal.c? */
void
render_resize(struct terminal *term, int width, int height)
{
    if (width == 0 || height == 0)
        return;

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
        /* Assume we're not fully up and running yet */
        return;
    }

    if (width == term->width && height == term->height && scale == term->scale)
        return;

    /* Cancel an application initiated "Synchronized Update" */
    term_disable_app_sync_updates(term);

    term->width = width;
    term->height = height;
    term->scale = scale;

    const int scrollback_lines = term->render.scrollback_lines;

    const int old_cols = term->cols;
    const int old_rows = term->rows;
    const int old_normal_grid_rows = term->normal.num_rows;
    const int old_alt_grid_rows = term->alt.num_rows;

    const int new_cols = term->width / term->cell_width;
    const int new_rows = term->height / term->cell_height;
    const int new_normal_grid_rows = 1 << (32 - __builtin_clz(new_rows + scrollback_lines - 1));
    const int new_alt_grid_rows = 1 << (32  - __builtin_clz(new_rows));

    term->x_margin = (term->width - new_cols * term->cell_width) / 2;
    term->y_margin = (term->height - new_rows * term->cell_height) / 2;

    term->normal.offset %= new_normal_grid_rows;
    term->normal.view %= new_normal_grid_rows;

    term->alt.offset %= new_alt_grid_rows;
    term->alt.view %= new_alt_grid_rows;

    /* Allocate new 'normal' grid */
    struct row **normal = calloc(new_normal_grid_rows, sizeof(normal[0]));
    for (int r = 0; r < new_rows; r++) {
        size_t real_r = (term->normal.view + r) & (new_normal_grid_rows - 1);
        normal[real_r] = grid_row_alloc(new_cols, true);
    }

    /* Allocate new 'alt' grid */
    struct row **alt = calloc(new_alt_grid_rows, sizeof(alt[0]));
    for (int r = 0; r < new_rows; r++)  {
        int real_r = (term->alt.view + r) & (new_alt_grid_rows - 1);
        alt[real_r] = grid_row_alloc(new_cols, true);
    }

    /* Reflow content */
    reflow(normal, new_cols, new_normal_grid_rows,
           term->normal.rows, old_cols, old_normal_grid_rows);
    reflow(alt, new_cols, new_alt_grid_rows,
           term->alt.rows, old_cols, old_alt_grid_rows);

    /* Free old 'normal' grid */
    for (int r = 0; r < term->normal.num_rows; r++)
        grid_row_free(term->normal.rows[r]);
    free(term->normal.rows);

    /* Free old 'alt' grid */
    for (int r = 0; r < term->alt.num_rows; r++)
        grid_row_free(term->alt.rows[r]);
    free(term->alt.rows);

    /* Reset tab stops */
    tll_free(term->tab_stops);
    for (int c = 0; c < new_cols; c += 8)
        tll_push_back(term->tab_stops, c);

    term->cols = new_cols;
    term->rows = new_rows;

    term->normal.rows = normal;
    term->normal.num_rows = new_normal_grid_rows;
    term->normal.num_cols = new_cols;
    term->alt.rows = alt;
    term->alt.num_rows = new_alt_grid_rows;
    term->alt.num_cols = new_cols;

    LOG_INFO("resize: %dx%d, grid: cols=%d, rows=%d (x-margin=%d, y-margin=%d)",
             term->width, term->height, term->cols, term->rows,
             term->x_margin, term->y_margin);

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

    term_cursor_to(
        term,
        min(term->cursor.point.row, term->rows - 1),
        min(term->cursor.point.col, term->cols - 1));

    term->render.last_cursor.cell = NULL;

    term_damage_view(term);
    render_refresh(term);
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
