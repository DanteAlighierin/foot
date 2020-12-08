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
