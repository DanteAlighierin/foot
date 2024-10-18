#if !defined(_DEBUG)
 #define _DEBUG
#endif
#undef NDEBUG

#include "../log.h"

#include "../config.c"

#define ALEN(v) (sizeof(v) / sizeof((v)[0]))

/*
 * Stubs
 */

void
user_notification_add_fmt(user_notifications_t *notifications,
                          enum user_notification_kind kind,
                          const char *fmt, ...)
{
}

static void
test_invalid_key(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                 const char *key)
{
    ctx->key = key;
    ctx->value = "value for invalid key";

    if (parse_fun(ctx)) {
        BUG("[%s].%s: did not fail to parse as expected"
            "(key should be invalid)", ctx->section, ctx->key);
    }
}

static void
test_string(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, char *const *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        const char *value;
        bool invalid;
    } input[] = {
        {"a string", "a string"},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (!streq(*ptr, input[i].value)) {
                BUG("[%s].%s=%s: set value (%s) not the expected one (%s)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_c32string(struct context *ctx, bool (*parse_fun)(struct context *ctx),
               const char *key, char32_t *const *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        const char32_t *value;
        bool invalid;
    } input[] = {
        {"a string", U"a string"},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (c32cmp(*ptr, input[i].value) != 0) {
                BUG("[%s].%s=%s: set value (%ls) not the expected one (%ls)",
                    ctx->section, ctx->key, ctx->value,
                    (const wchar_t *)*ptr,
                    (const wchar_t *)input[i].value);
            }
        }
    }
}

static void
test_protocols(struct context *ctx, bool (*parse_fun)(struct context *ctx),
               const char *key, char32_t **const *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        int count;
        const char32_t *value[2];
        bool invalid;
    } input[] = {
        {""},
        {"http", 1, {U"http://"}},
        {" http", 1, {U"http://"}},
        {"http, https", 2, {U"http://", U"https://"}},
        {"longprotocolislong", 1, {U"longprotocolislong://"}},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, &ctx->value[0]);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, &ctx->value[0]);
            }
            for (int c = 0; c < input[i].count; c++) {
                if (c32cmp((*ptr)[c], input[i].value[c]) != 0) {
                    BUG("[%s].%s=%s: set value[%d] (%ls) not the expected one (%ls)",
                        ctx->section, ctx->key, &ctx->value[c], c,
                        (const wchar_t *)(*ptr)[c],
                        (const wchar_t *)input[i].value[c]);
                }
            }
        }
    }
}

static void
test_boolean(struct context *ctx, bool (*parse_fun)(struct context *ctx),
             const char *key, const bool *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        bool value;
        bool invalid;
    } input[] = {
        {"1", true}, {"0", false},
        {"on", true}, {"off", false},
        {"true", true}, {"false", false},
        {"unittest-invalid-boolean-value", false, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%s) not the expected one (%s)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr ? "true" : "false",
                    input[i].value ? "true" : "false");
            }
        }
    }
}

static void
test_uint16(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const uint16_t *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        uint16_t value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"65535", 65535}, {"65536", 0, true},
        {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%hu) not the expected one (%hu)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_uint32(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const uint32_t *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        uint32_t value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"65536", 65536}, {"4294967295", 4294967295},
         {"4294967296", 0, true}, {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%u) not the expected one (%u)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_float(struct context *ctx, bool (*parse_fun)(struct context *ctx),
            const char *key, const float *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        float value;
        bool invalid;
    } input[] = {
        {"0", 0}, {"0.1", 0.1}, {"1e10", 1e10}, {"-10.7", -10.7},
        {"abc", 0, true}, {"true", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (*ptr != input[i].value) {
                BUG("[%s].%s=%s: set value (%f) not the expected one (%f)",
                    ctx->section, ctx->key, ctx->value,
                    *ptr, input[i].value);
            }
        }
    }
}

static void
test_pt_or_px(struct context *ctx, bool (*parse_fun)(struct context *ctx),
              const char *key, const struct pt_or_px *ptr)
{
    ctx->key = key;

    static const struct {
        const char *option_string;
        struct pt_or_px value;
        bool invalid;
    } input[] = {
        {"12", {.pt = 12}}, {"12px", {.px = 12}},
        {"unittest-invalid-pt-or-px-value", {0}, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;

        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
            if (memcmp(ptr, &input[i].value, sizeof(*ptr)) != 0) {
                BUG("[%s].%s=%s: "
                    "set value (pt=%f, px=%d) not the expected one (pt=%f, px=%d)",
                    ctx->section, ctx->key, ctx->value,
                    ptr->pt, ptr->px,
                    input[i].value.pt, input[i].value.px);
            }
        }
    }
}

static void
test_spawn_template(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                    const char *key, const struct config_spawn_template *ptr)
{
    static const char *const args[] = {
        "command", "arg1", "arg2", "arg3 has spaces"};

    ctx->key = key;
    ctx->value = "command arg1 arg2 \"arg3 has spaces\"";

    if (!parse_fun(ctx))
        BUG("[%s].%s=%s: failed to parse", ctx->section, ctx->key, ctx->value);

    if (ptr->argv.args == NULL)
        BUG("[%s].%s=%s: argv is NULL", ctx->section, ctx->key, ctx->value);

    for (size_t i = 0; i < ALEN(args); i++) {
        if (ptr->argv.args[i] == NULL || !streq(ptr->argv.args[i], args[i])) {
            BUG("[%s].%s=%s: set value not the expected one: "
                "mismatch of arg #%zu: expected=\"%s\", got=\"%s\"",
                ctx->section, ctx->key, ctx->value, i,
                args[i], ptr->argv.args[i]);
        }
    }

    if (ptr->argv.args[ALEN(args)] != NULL) {
        BUG("[%s].%s=%s: set value not the expected one: "
            "expected NULL terminator at arg #%zu, got=\"%s\"",
            ctx->section, ctx->key, ctx->value,
            ALEN(args), ptr->argv.args[ALEN(args)]);
    }

    /* Trigger parse failure */
    ctx->value = "command with \"unterminated quote";
    if (parse_fun(ctx)) {
        BUG("[%s].%s=%s: did not fail to parse as expected",
            ctx->section, ctx->key, ctx->value);
    }
}

static void
test_enum(struct context *ctx, bool (*parse_fun)(struct context *ctx),
          const char *key, size_t count, const char *enum_strings[static count],
          int enum_values[static count], int *ptr)
{
    ctx->key = key;

    for (size_t i = 0; i < count; i++) {
        ctx->value = enum_strings[i];
        if (!parse_fun(ctx)) {
            BUG("[%s].%s=%s: failed to parse",
                ctx->section, ctx->key, ctx->value);
        }

        if (*ptr != enum_values[i]) {
            BUG("[%s].%s=%s: set value not the expected one: expected %d, got %d",
                ctx->section, ctx->key, ctx->value, enum_values[i], *ptr);
        }
    }

    ctx->value = "invalid-enum-value";
    if (parse_fun(ctx)) {
        BUG("[%s].%s=%s: did not fail to parse as expected",
            ctx->section, ctx->key, ctx->value);
    }
}


static void
test_color(struct context *ctx, bool (*parse_fun)(struct context *ctx),
           const char *key, bool alpha_allowed, uint32_t *ptr)
{
    ctx->key = key;

    const struct {
        const char *option_string;
        uint32_t color;
        bool invalid;
    } input[] = {
        {"000000", 0},
        {"999999", 0x999999},
        {"ffffff", 0xffffff},
        {"ffffffff", 0xffffffff, !alpha_allowed},
        {"aabbccdd", 0xaabbccdd, !alpha_allowed},
        {"00", 0, true},
        {"0000", 0, true},
        {"00000", 0, true},
        {"000000000", 0, true},
        {"unittest-invalid-color", 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;
        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
        }
    }
}

static void
test_two_colors(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                const char *key, bool alpha_allowed,
                uint32_t *ptr1, uint32_t *ptr2)
{
    ctx->key = key;

    const struct {
        const char *option_string;
        uint32_t color1;
        uint32_t color2;
        bool invalid;
    } input[] = {
        {"000000 000000", 0, 0},

        /* No alpha */
        {"999999 888888", 0x999999, 0x888888},
        {"ffffff aaaaaa", 0xffffff, 0xaaaaaa},

        /* Both colors have alpha component */
        {"ffffffff 00000000", 0xffffffff, 0x00000000, !alpha_allowed},
        {"aabbccdd, ee112233", 0xaabbccdd, 0xee112233, !alpha_allowed},

        /* Only one color has alpha component */
        {"ffffffff 112233", 0xffffffff, 0x112233, !alpha_allowed},
        {"ffffff ff112233", 0x00ffffff, 0xff112233, !alpha_allowed},

        {"unittest-invalid-color", 0, 0, true},
    };

    for (size_t i = 0; i < ALEN(input); i++) {
        ctx->value = input[i].option_string;
        if (input[i].invalid) {
            if (parse_fun(ctx)) {
                BUG("[%s].%s=%s: did not fail to parse as expected",
                    ctx->section, ctx->key, ctx->value);
            }
        } else {
            if (!parse_fun(ctx)) {
                BUG("[%s].%s=%s: failed to parse",
                    ctx->section, ctx->key, ctx->value);
            }
        }
    }
}

static void
test_section_main(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "main", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_main, "invalid-key");

    test_string(&ctx, &parse_section_main, "shell", &conf.shell);
    test_string(&ctx, &parse_section_main, "term", &conf.term);
    test_string(&ctx, &parse_section_main, "app-id", &conf.app_id);
    test_string(&ctx, &parse_section_main, "utmp-helper", &conf.utmp_helper_path);

    test_c32string(&ctx, &parse_section_main, "word-delimiters", &conf.word_delimiters);

    test_boolean(&ctx, &parse_section_main, "login-shell", &conf.login_shell);
    test_boolean(&ctx, &parse_section_main, "box-drawings-uses-font-glyphs", &conf.box_drawings_uses_font_glyphs);
    test_boolean(&ctx, &parse_section_main, "locked-title", &conf.locked_title);
    test_boolean(&ctx, &parse_section_main, "notify-focus-inhibit", &conf.desktop_notifications.inhibit_when_focused);  /* Deprecated */
    test_boolean(&ctx, &parse_section_main, "dpi-aware", &conf.dpi_aware);

    test_pt_or_px(&ctx, &parse_section_main, "font-size-adjustment", &conf.font_size_adjustment.pt_or_px);  /* TODO: test ‘N%’ values too */
    test_pt_or_px(&ctx, &parse_section_main, "line-height", &conf.line_height);
    test_pt_or_px(&ctx, &parse_section_main, "letter-spacing", &conf.letter_spacing);
    test_pt_or_px(&ctx, &parse_section_main, "horizontal-letter-offset", &conf.horizontal_letter_offset);
    test_pt_or_px(&ctx, &parse_section_main, "vertical-letter-offset", &conf.vertical_letter_offset);
    test_pt_or_px(&ctx, &parse_section_main, "underline-thickness", &conf.underline_thickness);
    test_pt_or_px(&ctx, &parse_section_main, "strikeout-thickness", &conf.strikeout_thickness);

    test_uint16(&ctx, &parse_section_main, "resize-delay-ms", &conf.resize_delay_ms);
    test_uint16(&ctx, &parse_section_main, "workers", &conf.render_worker_count);

    test_spawn_template(&ctx, &parse_section_main, "notify", &conf.desktop_notifications.command);  /* Deprecated */

    test_enum(&ctx, &parse_section_main, "selection-target",
              4,
              (const char *[]){"none", "primary", "clipboard", "both"},
              (int []){SELECTION_TARGET_NONE,
                       SELECTION_TARGET_PRIMARY,
                       SELECTION_TARGET_CLIPBOARD,
                       SELECTION_TARGET_BOTH},
              (int *)&conf.selection_target);

    test_enum(
        &ctx, &parse_section_main, "initial-window-mode",
        3,
        (const char *[]){"windowed", "maximized", "fullscreen"},
        (int []){STARTUP_WINDOWED, STARTUP_MAXIMIZED, STARTUP_FULLSCREEN},
        (int *)&conf.startup_mode);

    /* TODO: font (custom) */
    /* TODO: include (custom) */
    /* TODO: bold-text-in-bright (enum/boolean) */
    /* TODO: pad (geometry + optional string)*/
    /* TODO: initial-window-size-pixels (geometry) */
    /* TODO: initial-window-size-chars (geometry) */

    config_free(&conf);
}

static void
test_section_bell(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "bell", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_bell, "invalid-key");

    test_boolean(&ctx, &parse_section_bell, "urgent", &conf.bell.urgent);
    test_boolean(&ctx, &parse_section_bell, "notify", &conf.bell.notify);
    test_boolean(&ctx, &parse_section_bell, "command-focused",
                 &conf.bell.command_focused);
    test_spawn_template(&ctx, &parse_section_bell, "command",
                        &conf.bell.command);

    config_free(&conf);
}

static void
test_section_desktop_notifications(void)
{
    struct config conf = {0};
    struct context ctx = {.conf = &conf, .section = "desktop-notifications", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_desktop_notifications, "invalid-key");

    test_boolean(&ctx, &parse_section_desktop_notifications, "inhibit-when-focused", &conf.desktop_notifications.inhibit_when_focused);
    test_spawn_template(&ctx, &parse_section_desktop_notifications, "command", &conf.desktop_notifications.command);
    test_spawn_template(&ctx, &parse_section_desktop_notifications, "command-action-argument", &conf.desktop_notifications.command_action_arg);
    test_spawn_template(&ctx, &parse_section_desktop_notifications, "close", &conf.desktop_notifications.close);

    config_free(&conf);
}

static void
test_section_scrollback(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "scrollback", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_scrollback, "invalid-key");

    test_uint32(&ctx, &parse_section_scrollback, "lines",
                &conf.scrollback.lines);
    test_float(&ctx, parse_section_scrollback, "multiplier", &conf.scrollback.multiplier);

    test_enum(
        &ctx, &parse_section_scrollback, "indicator-position",
        3,
        (const char *[]){"none", "fixed", "relative"},
        (int []){SCROLLBACK_INDICATOR_POSITION_NONE,
            SCROLLBACK_INDICATOR_POSITION_FIXED,
            SCROLLBACK_INDICATOR_POSITION_RELATIVE},
        (int *)&conf.scrollback.indicator.position);

    /* TODO: indicator-format (enum, sort-of) */

    config_free(&conf);
}

static void
test_section_url(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "url", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_url, "invalid-key");

    test_spawn_template(&ctx, &parse_section_url, "launch", &conf.url.launch);
    test_enum(&ctx, &parse_section_url, "osc8-underline",
              2,
              (const char *[]){"url-mode", "always"},
              (int []){OSC8_UNDERLINE_URL_MODE, OSC8_UNDERLINE_ALWAYS},
              (int *)&conf.url.osc8_underline);
    test_c32string(&ctx, &parse_section_url, "label-letters", &conf.url.label_letters);
    test_protocols(&ctx, &parse_section_url, "protocols", &conf.url.protocols);

    /* TODO: uri-characters (wchar string, but sorted) */

    config_free(&conf);
}

static void
test_section_cursor(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "cursor", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_cursor, "invalid-key");

    test_enum(
        &ctx, &parse_section_cursor, "style",
        3,
        (const char *[]){"block", "beam", "underline"},
        (int []){CURSOR_BLOCK, CURSOR_BEAM, CURSOR_UNDERLINE},
        (int *)&conf.cursor.style);
    test_enum(
        &ctx, &parse_section_cursor, "unfocused-style",
        3,
        (const char *[]){"unchanged", "hollow", "none"},
        (int []){CURSOR_UNFOCUSED_UNCHANGED, CURSOR_UNFOCUSED_HOLLOW, CURSOR_UNFOCUSED_NONE},
        (int *)&conf.cursor.unfocused_style);
    test_boolean(&ctx, &parse_section_cursor, "blink", &conf.cursor.blink.enabled);
    test_uint32(&ctx, &parse_section_cursor, "blink-rate", &conf.cursor.blink.rate_ms);
    test_pt_or_px(&ctx, &parse_section_cursor, "beam-thickness",
                  &conf.cursor.beam_thickness);
    test_pt_or_px(&ctx, &parse_section_cursor, "underline-thickness",
                  &conf.cursor.underline_thickness);

    /* TODO: color (two RRGGBB values) */

    config_free(&conf);
}

static void
test_section_mouse(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_mouse, "invalid-key");

    test_boolean(&ctx, &parse_section_mouse, "hide-when-typing",
                 &conf.mouse.hide_when_typing);
    test_boolean(&ctx, &parse_section_mouse, "alternate-scroll-mode",
                 &conf.mouse.alternate_scroll_mode);

    config_free(&conf);
}

static void
test_section_touch(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "touch", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_touch, "invalid-key");

    test_uint32(&ctx, &parse_section_touch, "long-press-delay",
                &conf.touch.long_press_delay);

    config_free(&conf);
}

static void
test_section_colors(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "colors", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_colors, "invalid-key");

    test_color(&ctx, &parse_section_colors, "foreground", false, &conf.colors.fg);
    test_color(&ctx, &parse_section_colors, "background", false, &conf.colors.bg);
    test_color(&ctx, &parse_section_colors, "regular0", false, &conf.colors.table[0]);
    test_color(&ctx, &parse_section_colors, "regular1", false, &conf.colors.table[1]);
    test_color(&ctx, &parse_section_colors, "regular2", false, &conf.colors.table[2]);
    test_color(&ctx, &parse_section_colors, "regular3", false, &conf.colors.table[3]);
    test_color(&ctx, &parse_section_colors, "regular4", false, &conf.colors.table[4]);
    test_color(&ctx, &parse_section_colors, "regular5", false, &conf.colors.table[5]);
    test_color(&ctx, &parse_section_colors, "regular6", false, &conf.colors.table[6]);
    test_color(&ctx, &parse_section_colors, "regular7", false, &conf.colors.table[7]);
    test_color(&ctx, &parse_section_colors, "bright0", false, &conf.colors.table[8]);
    test_color(&ctx, &parse_section_colors, "bright1", false, &conf.colors.table[9]);
    test_color(&ctx, &parse_section_colors, "bright2", false, &conf.colors.table[10]);
    test_color(&ctx, &parse_section_colors, "bright3", false, &conf.colors.table[11]);
    test_color(&ctx, &parse_section_colors, "bright4", false, &conf.colors.table[12]);
    test_color(&ctx, &parse_section_colors, "bright5", false, &conf.colors.table[13]);
    test_color(&ctx, &parse_section_colors, "bright6", false, &conf.colors.table[14]);
    test_color(&ctx, &parse_section_colors, "bright7", false, &conf.colors.table[15]);
    test_color(&ctx, &parse_section_colors, "dim0", false, &conf.colors.dim[0]);
    test_color(&ctx, &parse_section_colors, "dim1", false, &conf.colors.dim[1]);
    test_color(&ctx, &parse_section_colors, "dim2", false, &conf.colors.dim[2]);
    test_color(&ctx, &parse_section_colors, "dim3", false, &conf.colors.dim[3]);
    test_color(&ctx, &parse_section_colors, "dim4", false, &conf.colors.dim[4]);
    test_color(&ctx, &parse_section_colors, "dim5", false, &conf.colors.dim[5]);
    test_color(&ctx, &parse_section_colors, "dim6", false, &conf.colors.dim[6]);
    test_color(&ctx, &parse_section_colors, "dim7", false, &conf.colors.dim[7]);
    test_color(&ctx, &parse_section_colors, "selection-foreground", false, &conf.colors.selection_fg);
    test_color(&ctx, &parse_section_colors, "selection-background", false, &conf.colors.selection_bg);
    test_color(&ctx, &parse_section_colors, "urls", false, &conf.colors.url);
    test_two_colors(&ctx, &parse_section_colors, "jump-labels", false,
                    &conf.colors.jump_label.fg,
                    &conf.colors.jump_label.bg);
    test_two_colors(&ctx, &parse_section_colors, "scrollback-indicator", false,
                    &conf.colors.scrollback_indicator.fg,
                    &conf.colors.scrollback_indicator.bg);
    test_two_colors(&ctx, &parse_section_colors, "search-box-no-match", false,
                    &conf.colors.search_box.no_match.fg,
                    &conf.colors.search_box.no_match.bg);
    test_two_colors(&ctx, &parse_section_colors, "search-box-match", false,
                    &conf.colors.search_box.match.fg,
                    &conf.colors.search_box.match.bg);

    for (size_t i = 0; i < 255; i++) {
        char key_name[4];
        sprintf(key_name, "%zu", i);
        test_color(&ctx, &parse_section_colors, key_name, false,
                   &conf.colors.table[i]);
    }

    test_invalid_key(&ctx, &parse_section_colors, "256");

    /* TODO: alpha (float in range 0-1, converted to uint16_t) */

    config_free(&conf);
}

static void
test_section_csd(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "csd", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_csd, "invalid-key");

    test_enum(
        &ctx, &parse_section_csd, "preferred",
        3,
        (const char *[]){"none", "client", "server"},
        (int []){CONF_CSD_PREFER_NONE,
                 CONF_CSD_PREFER_CLIENT,
                 CONF_CSD_PREFER_SERVER},
        (int *)&conf.csd.preferred);
    test_uint16(&ctx, &parse_section_csd, "size", &conf.csd.title_height);
    test_color(&ctx, &parse_section_csd, "color", true, &conf.csd.color.title);
    test_uint16(&ctx, &parse_section_csd, "border-width",
                &conf.csd.border_width_visible);
    test_color(&ctx, &parse_section_csd, "border-color", true,
               &conf.csd.color.border);
    test_uint16(&ctx, &parse_section_csd, "button-width",
                &conf.csd.button_width);
    test_color(&ctx, &parse_section_csd, "button-color", true,
               &conf.csd.color.buttons);
    test_color(&ctx, &parse_section_csd, "button-minimize-color", true,
               &conf.csd.color.minimize);
    test_color(&ctx, &parse_section_csd, "button-maximize-color", true,
               &conf.csd.color.maximize);
    test_color(&ctx, &parse_section_csd, "button-close-color", true,
               &conf.csd.color.quit);
    test_boolean(&ctx, &parse_section_csd, "hide-when-maximized",
                 &conf.csd.hide_when_maximized);
    test_boolean(&ctx, &parse_section_csd, "double-click-to-maximize",
                 &conf.csd.double_click_to_maximize);

    /* TODO: verify the ‘set’ bit is actually set for colors */
    /* TODO: font */

    config_free(&conf);
}

static bool
have_modifier(const config_modifier_list_t *mods, const char *mod)
{
    tll_foreach(*mods, it) {
        if (strcmp(it->item, mod) == 0)
            return true;
    }

    return false;
}

static void
test_key_binding(struct context *ctx, bool (*parse_fun)(struct context *ctx),
                 int action, int max_action, const char *const *map,
                 struct config_key_binding_list *bindings,
                 enum key_binding_type type)
{
    xassert(map[action] != NULL);
    xassert(bindings->count == 0);

    const char *key = map[action];

    /* “Randomize” which modifiers to enable */
    const bool ctrl = action % 2;
    const bool alt = action % 3;
    const bool shift = action % 4;
    const bool super = action % 5;
    const bool argv = action % 6;

    static const char *const args[] = {
        "command", "arg1", "arg2", "arg3 has spaces"};

    /* Generate the modifier part of the ‘value’ */
    char modifier_string[32];
    sprintf(modifier_string, "%s%s%s%s",
            ctrl ? XKB_MOD_NAME_CTRL "+" : "",
            alt ? XKB_MOD_NAME_ALT "+" : "",
            shift ? XKB_MOD_NAME_SHIFT "+" : "",
            super ? XKB_MOD_NAME_LOGO "+" : "");

    /* Use a unique symbol for this action (key bindings) */
    const xkb_keysym_t sym = XKB_KEY_a + action;

    /* Mouse button (mouse bindings) */
    const int button_idx = action % ALEN(button_map);
    const int button = button_map[button_idx].code;
    const int click_count = action % 3 + 1;

    /* Finally, generate the ‘value’ (e.g. “Control+shift+x”) */
    char value[128] = {0};

    ctx->key = key;
    ctx->value = value;

    /* First, try setting the empty string */
    if (parse_fun(ctx)) {
        BUG("[%s].%s=<empty>: did not fail to parse as expected",
            ctx->section, ctx->key);
    }

    switch (type) {
    case KEY_BINDING: {
        char sym_name[16];
        xkb_keysym_get_name(sym, sym_name, sizeof(sym_name));

        snprintf(value, sizeof(value), "%s%s%s",
                 argv ? "[command arg1 arg2 \"arg3 has spaces\"] " : "",
                 modifier_string, sym_name);
        break;
    }

    case MOUSE_BINDING: {
        const char *const button_name = button_map[button_idx].name;
        int chars = snprintf(
            value, sizeof(value), "%s%s%s",
            argv ? "[command arg1 arg2 \"arg3 has spaces\"] " : "",
            modifier_string, button_name);

        xassert(click_count > 0);
        if (click_count > 1)
            snprintf(&value[chars], sizeof(value) - chars, "-%d", click_count);
        break;
    }
    }

    if (!parse_fun(ctx)) {
        BUG("[%s].%s=%s failed to parse",
            ctx->section, ctx->key, ctx->value);
    }

    const struct config_key_binding *binding =
        &bindings->arr[bindings->count - 1];

    if (argv) {
        if (binding->aux.pipe.args == NULL) {
            BUG("[%s].%s=%s: pipe argv is NULL",
                ctx->section, ctx->key, ctx->value);
        }

        for (size_t i = 0; i < ALEN(args); i++) {
            if (binding->aux.pipe.args[i] == NULL ||
                !streq(binding->aux.pipe.args[i], args[i]))
            {
                BUG("[%s].%s=%s: pipe argv not the expected one: "
                    "mismatch of arg #%zu: expected=\"%s\", got=\"%s\"",
                    ctx->section, ctx->key, ctx->value, i,
                    args[i], binding->aux.pipe.args[i]);
            }
        }

        if (binding->aux.pipe.args[ALEN(args)] != NULL) {
            BUG("[%s].%s=%s: pipe argv not the expected one: "
                "expected NULL terminator at arg #%zu, got=\"%s\"",
                ctx->section, ctx->key, ctx->value,
                ALEN(args), binding->aux.pipe.args[ALEN(args)]);
        }
    } else {
        if (binding->aux.pipe.args != NULL) {
            BUG("[%s].%s=%s: pipe argv not NULL",
                ctx->section, ctx->key, ctx->value);
        }
    }

    if (binding->action != action) {
        BUG("[%s].%s=%s: action mismatch: %d != %d",
            ctx->section, ctx->key, ctx->value, binding->action, action);
    }

    bool have_ctrl = have_modifier(&binding->modifiers, XKB_MOD_NAME_CTRL);
    bool have_alt = have_modifier(&binding->modifiers, XKB_MOD_NAME_ALT);
    bool have_shift = have_modifier(&binding->modifiers, XKB_MOD_NAME_SHIFT);
    bool have_super = have_modifier(&binding->modifiers, XKB_MOD_NAME_LOGO);

    if (have_ctrl != ctrl || have_alt != alt ||
        have_shift != shift || have_super != super)
    {
        BUG("[%s].%s=%s: modifier mismatch:\n"
            "  have:     ctrl=%d, alt=%d, shift=%d, super=%d\n"
            "  expected: ctrl=%d, alt=%d, shift=%d, super=%d",
            ctx->section, ctx->key, ctx->value,
            have_ctrl, have_alt, have_shift, have_super,
            ctrl, alt, shift, super);
    }

    switch (type) {
    case KEY_BINDING:
        if (binding->k.sym != sym) {
            BUG("[%s].%s=%s: key symbol mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value, binding->k.sym, sym);
        }
        break;

    case MOUSE_BINDING:;
        if (binding->m.button != button) {
            BUG("[%s].%s=%s: mouse button mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value, binding->m.button, button);
        }

        if (binding->m.count != click_count) {
            BUG("[%s].%s=%s: mouse button click count mismatch: %d != %d",
                ctx->section, ctx->key, ctx->value,
                binding->m.count, click_count);
        }
        break;
    }


    free_key_binding_list(bindings);
}

enum collision_test_mode {
    FAIL_DIFFERENT_ACTION,
    FAIL_DIFFERENT_ARGV,
    FAIL_MOUSE_OVERRIDE,
    SUCCEED_SAME_ACTION_AND_ARGV,
};

static void
_test_binding_collisions(struct context *ctx,
                         int max_action, const char *const *map,
                         enum key_binding_type type,
                         enum collision_test_mode test_mode)
{
    struct config_key_binding *bindings_array =
        xcalloc(2, sizeof(bindings_array[0]));

    struct config_key_binding_list bindings = {
        .count = 2,
        .arr = bindings_array,
    };

    /* First, verify we get a collision when trying to assign the same
     * key combo to multiple actions */
    bindings.arr[0] = (struct config_key_binding){
        .action = (test_mode == FAIL_DIFFERENT_ACTION
                   ? max_action - 1 : max_action),
        .modifiers = tll_init(),
        .path = "unittest",
    };
    tll_push_back(bindings.arr[0].modifiers, xstrdup(XKB_MOD_NAME_CTRL));

    bindings.arr[1] = (struct config_key_binding){
        .action = max_action,
        .modifiers = tll_init(),
        .path = "unittest",
    };
    tll_push_back(bindings.arr[1].modifiers, xstrdup(XKB_MOD_NAME_CTRL));

    switch (type) {
    case KEY_BINDING:
        bindings.arr[0].k.sym = XKB_KEY_a;
        bindings.arr[1].k.sym = XKB_KEY_a;
        break;

    case MOUSE_BINDING:
        bindings.arr[0].m.button = BTN_LEFT;
        bindings.arr[0].m.count = 1;
        bindings.arr[1].m.button = BTN_LEFT;
        bindings.arr[1].m.count = 1;
        break;
    }

    switch (test_mode) {
    case FAIL_DIFFERENT_ACTION:
        break;

    case FAIL_MOUSE_OVERRIDE:
        tll_free_and_free(ctx->conf->mouse.selection_override_modifiers, free);
        tll_push_back(ctx->conf->mouse.selection_override_modifiers, xstrdup(XKB_MOD_NAME_CTRL));
        break;

    case FAIL_DIFFERENT_ARGV:
    case SUCCEED_SAME_ACTION_AND_ARGV:
        bindings.arr[0].aux.type = BINDING_AUX_PIPE;
        bindings.arr[0].aux.master_copy = true;
        bindings.arr[0].aux.pipe.args = xcalloc(
            4, sizeof(bindings.arr[0].aux.pipe.args[0]));
        bindings.arr[0].aux.pipe.args[0] = xstrdup("/usr/bin/foobar");
        bindings.arr[0].aux.pipe.args[1] = xstrdup("hello");
        bindings.arr[0].aux.pipe.args[2] = xstrdup("world");

        bindings.arr[1].aux.type = BINDING_AUX_PIPE;
        bindings.arr[1].aux.master_copy = true;
        bindings.arr[1].aux.pipe.args = xcalloc(
            4, sizeof(bindings.arr[1].aux.pipe.args[0]));
        bindings.arr[1].aux.pipe.args[0] = xstrdup("/usr/bin/foobar");
        bindings.arr[1].aux.pipe.args[1] = xstrdup("hello");

        if (test_mode == SUCCEED_SAME_ACTION_AND_ARGV)
            bindings.arr[1].aux.pipe.args[2] = xstrdup("world");
        break;
    }

    bool expected_result =
        test_mode == SUCCEED_SAME_ACTION_AND_ARGV ? true : false;

    if (resolve_key_binding_collisions(
            ctx->conf, ctx->section, map, &bindings, type) != expected_result)
    {
        BUG("[%s].%s vs. %s: %s",
            ctx->section, map[max_action - 1], map[max_action],
            (expected_result == true
             ? "invalid key combo collision detected"
             : "key combo collision not detected"));
    }

    if (expected_result == false) {
        if (bindings.count != 1)
            BUG("[%s]: colliding binding not removed", ctx->section);

        if (bindings.arr[0].action !=
            (test_mode == FAIL_DIFFERENT_ACTION ? max_action - 1 : max_action))
        {
            BUG("[%s]: wrong binding removed", ctx->section);
        }
    }

    free_key_binding_list(&bindings);
}

static void
test_binding_collisions(struct context *ctx,
                         int max_action, const char *const *map,
                        enum key_binding_type type)
{
    _test_binding_collisions(ctx, max_action, map, type, FAIL_DIFFERENT_ACTION);
    _test_binding_collisions(ctx, max_action, map, type, FAIL_DIFFERENT_ARGV);
    _test_binding_collisions(ctx, max_action, map, type, SUCCEED_SAME_ACTION_AND_ARGV);

    if (type == MOUSE_BINDING) {
        _test_binding_collisions(
            ctx, max_action, map, type, FAIL_MOUSE_OVERRIDE);
    }
}

static void
test_section_key_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_key_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_KEY_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_key_bindings,
            action, BIND_ACTION_KEY_COUNT - 1,
            binding_action_map, &conf.bindings.key, KEY_BINDING);
    }

    config_free(&conf);
}

static void
test_section_key_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "key-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx, BIND_ACTION_KEY_COUNT - 1, binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static void
test_section_search_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "search-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_search_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_SEARCH_COUNT; action++) {
        if (search_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_search_bindings,
            action, BIND_ACTION_SEARCH_COUNT - 1,
            search_binding_action_map, &conf.bindings.search, KEY_BINDING);
    }

    config_free(&conf);
}

static void
test_section_search_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "search-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_SEARCH_COUNT - 1, search_binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static void
test_section_url_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "rul-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_url_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_URL_COUNT; action++) {
        if (url_binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_url_bindings,
            action, BIND_ACTION_URL_COUNT - 1,
            url_binding_action_map, &conf.bindings.url, KEY_BINDING);
    }

    config_free(&conf);
}

static void
test_section_url_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "url-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_URL_COUNT - 1, url_binding_action_map, KEY_BINDING);

    config_free(&conf);
}

static void
test_section_mouse_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse-bindings", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_mouse_bindings, "invalid-key");

    for (int action = 0; action < BIND_ACTION_COUNT; action++) {
        if (binding_action_map[action] == NULL)
            continue;

        test_key_binding(
            &ctx, &parse_section_mouse_bindings,
            action, BIND_ACTION_COUNT - 1,
            binding_action_map, &conf.bindings.mouse, MOUSE_BINDING);
    }

    config_free(&conf);
}

static void
test_section_mouse_bindings_collisions(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "mouse-bindings", .path = "unittest"};

    test_binding_collisions(
        &ctx,
        BIND_ACTION_COUNT - 1, binding_action_map, MOUSE_BINDING);

    config_free(&conf);
}

static void
test_section_text_bindings(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "text-bindings", .path = "unittest"};

    ctx.key = "abcd";
    ctx.value = XKB_MOD_NAME_CTRL "+" XKB_MOD_NAME_SHIFT "+x";
    xassert(parse_section_text_bindings(&ctx));

    ctx.key = "\\x07";
    xassert(parse_section_text_bindings(&ctx));

    ctx.key = "\\x1g";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\x1";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\x";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\";
    xassert(!parse_section_text_bindings(&ctx));

    ctx.key = "\\y";
    xassert(!parse_section_text_bindings(&ctx));

#if 0
    /* Invalid modifier and key names are detected later, when a
     * layout is applied */
    ctx.key = "abcd";
    ctx.value = "InvalidMod+y";
    xassert(!parse_section_text_bindings(&ctx));
#endif
    config_free(&conf);
}

static void
test_section_environment(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "environment", .path = "unittest"};

    /* A single variable */
    ctx.key = "FOO";
    ctx.value = "bar";
    xassert(parse_section_environment(&ctx));
    xassert(tll_length(conf.env_vars) == 1);
    xassert(streq(tll_front(conf.env_vars).name, "FOO"));
    xassert(streq(tll_front(conf.env_vars).value, "bar"));

    /* Add a second variable */
    ctx.key = "BAR";
    ctx.value = "123";
    xassert(parse_section_environment(&ctx));
    xassert(tll_length(conf.env_vars) == 2);
    xassert(streq(tll_back(conf.env_vars).name, "BAR"));
    xassert(streq(tll_back(conf.env_vars).value, "123"));

    /* Replace the *value* of the first variable */
    ctx.key = "FOO";
    ctx.value = "456";
    xassert(parse_section_environment(&ctx));
    xassert(tll_length(conf.env_vars) == 2);
    xassert(streq(tll_front(conf.env_vars).name, "FOO"));
    xassert(streq(tll_front(conf.env_vars).value, "456"));
    xassert(streq(tll_back(conf.env_vars).name, "BAR"));
    xassert(streq(tll_back(conf.env_vars).value, "123"));

    config_free(&conf);
}

static void
test_section_tweak(void)
{
    struct config conf = {0};
    struct context ctx = {
        .conf = &conf, .section = "tweak", .path = "unittest"};

    test_invalid_key(&ctx, &parse_section_tweak, "invalid-key");

    test_enum(
        &ctx, &parse_section_tweak, "scaling-filter",
        5,
        (const char *[]){"none", "nearest", "bilinear", "cubic", "lanczos3"},
        (int []){FCFT_SCALING_FILTER_NONE,
                 FCFT_SCALING_FILTER_NEAREST,
                 FCFT_SCALING_FILTER_BILINEAR,
                 FCFT_SCALING_FILTER_CUBIC,
                 FCFT_SCALING_FILTER_LANCZOS3},
        (int *)&conf.tweak.fcft_filter);

    test_boolean(&ctx, &parse_section_tweak, "overflowing-glyphs",
                 &conf.tweak.overflowing_glyphs);

    test_enum(
        &ctx, &parse_section_tweak, "render-timer",
        4,
        (const char *[]){"none", "osd", "log", "both"},
        (int []){RENDER_TIMER_NONE,
                 RENDER_TIMER_OSD,
                 RENDER_TIMER_LOG,
                 RENDER_TIMER_BOTH},
        (int *)&conf.tweak.render_timer);

    test_float(&ctx, &parse_section_tweak, "box-drawing-base-thickness",
                &conf.tweak.box_drawing_base_thickness);
    test_boolean(&ctx, &parse_section_tweak, "box-drawing-solid-shades",
        &conf.tweak.box_drawing_solid_shades);

#if 0  /* Must be less than 16ms */
    test_uint32(&ctx, &parse_section_tweak, "delayed-render-lower",
                &conf.tweak.delayed_render_lower_ns);
    test_uint32(&ctx, &parse_section_tweak, "delayed-render-upper",
                &conf.tweak.delayed_render_upper_ns);
#endif
    test_boolean(&ctx, &parse_section_tweak, "damage-whole-window",
                 &conf.tweak.damage_whole_window);

#if defined(FOOT_GRAPHEME_CLUSTERING)
    test_boolean(&ctx, &parse_section_tweak, "grapheme-shaping",
                 &conf.tweak.grapheme_shaping);
#else
    /* TODO: the setting still exists, but is always forced to ‘false’. */
#endif

    test_enum(
        &ctx, &parse_section_tweak, "grapheme-width-method",
        3,
        (const char *[]){"wcswidth", "double-width", "max"},
        (int []){GRAPHEME_WIDTH_WCSWIDTH,
                 GRAPHEME_WIDTH_DOUBLE,
                 GRAPHEME_WIDTH_MAX},
        (int *)&conf.tweak.grapheme_width_method);

    test_boolean(&ctx, &parse_section_tweak, "font-monospace-warn",
                 &conf.tweak.font_monospace_warn);

    test_float(&ctx, &parse_section_tweak, "bold-text-in-bright-amount",
               &conf.bold_in_bright.amount);

#if 0 /* Must be equal to, or less than INT32_MAX */
    test_uint32(&ctx, &parse_section_tweak, "max-shm-pool-size-mb",
                &conf.tweak.max_shm_pool_size);
#endif

    config_free(&conf);
}

int
main(int argc, const char *const *argv)
{
    FcInit();
    log_init(LOG_COLORIZE_AUTO, false, 0, LOG_CLASS_ERROR);
    test_section_main();
    test_section_bell();
    test_section_desktop_notifications();
    test_section_scrollback();
    test_section_url();
    test_section_cursor();
    test_section_mouse();
    test_section_touch();
    test_section_colors();
    test_section_csd();
    test_section_key_bindings();
    test_section_key_bindings_collisions();
    test_section_search_bindings();
    test_section_search_bindings_collisions();
    test_section_url_bindings();
    test_section_url_bindings_collisions();
    test_section_mouse_bindings();
    test_section_mouse_bindings_collisions();
    test_section_text_bindings();
    test_section_environment();
    test_section_tweak();
    log_deinit();
    FcFini();
    return 0;
}
