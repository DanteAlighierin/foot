#pragma once

#include <stdbool.h>

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
