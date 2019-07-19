#include "dcs.h"

#define LOG_MODULE "dcs"
#define LOG_ENABLE_DBG 0
#include "log.h"

void
dcs_passthrough(struct terminal *term)
{
    LOG_DBG("DCS passthrough: %.*s (%zu bytes)",
            (int)term->vt.dcs.idx, term->vt.dcs.data, term->vt.dcs.idx);
}

bool
dcs_ensure_size(struct terminal *term, size_t required_size)
{
    if (required_size <= term->vt.dcs.size)
        return true;

    size_t new_size = (required_size + 127) / 128 * 128;
    assert(new_size > 0);

    uint8_t *new_data = realloc(term->vt.dcs.data, new_size);
    if (new_data == NULL) {
        LOG_ERRNO("failed to increase size of DCS buffer");
        return false;
    }

    term->vt.dcs.data = new_data;
    term->vt.dcs.size = new_size;
    return true;
}
