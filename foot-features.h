#pragma once

#include <stdbool.h>

static inline bool feature_assertions(void)
{
#if defined(NDEBUG)
    return false;
#else
    return true;
#endif
}

static inline bool feature_ime(void)
{
#if defined(FOOT_IME_ENABLED) && FOOT_IME_ENABLED
    return true;
#else
    return false;
#endif
}

static inline bool feature_pgo(void)
{
#if defined(FOOT_PGO_ENABLED) && FOOT_PGO_ENABLED
    return true;
#else
    return false;
#endif
}

static inline bool feature_graphemes(void)
{
#if defined(FOOT_GRAPHEME_CLUSTERING) && FOOT_GRAPHEME_CLUSTERING
    return true;
#else
    return false;
#endif
}

static inline bool feature_xdg_toplevel_icon(void)
{
#if defined(HAVE_XDG_TOPLEVEL_ICON)
    return true;
#else
    return false;
#endif
}
