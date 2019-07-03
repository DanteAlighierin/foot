#include "osc.h"

#define LOG_MODULE "osc"
#define LOG_ENABLE_DBG 0
#include "log.h"

bool
osc_dispatch(struct terminal *term)
{
    LOG_DBG("OCS: %.*s", (int)term->vt.osc.idx, term->vt.osc.data);
    return true;
}
