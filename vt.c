#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define LOG_MODULE "vt"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "csi.h"
#include "osc.h"
#include "grid.h"

/* https://vt100.net/emu/dec_ansi_parser */

enum state {
    STATE_SAME,       /* For state_transition */

    STATE_GROUND,
    STATE_ESCAPE,
    STATE_ESCAPE_INTERMEDIATE,

    STATE_CSI_ENTRY,
    STATE_CSI_PARAM,
    STATE_CSI_INTERMEDIATE,
    STATE_CSI_IGNORE,

    STATE_OSC_STRING,

    STATE_DCS_ENTRY,
    STATE_DCS_PARAM,
    STATE_DCS_INTERMEDIATE,
    STATE_DCS_IGNORE,
    STATE_DCS_PASSTHROUGH,

    STATE_SOS_PM_APC_STRING,

    STATE_UTF8_COLLECT,
};

enum action {
    ACTION_NONE,      /* For state_transition */

    ACTION_IGNORE,
    ACTION_CLEAR,
    ACTION_EXECUTE,
    ACTION_PRINT,
    ACTION_PARAM,
    ACTION_COLLECT,

    ACTION_ESC_DISPATCH,
    ACTION_CSI_DISPATCH,

    ACTION_OSC_START,
    ACTION_OSC_END,
    ACTION_OSC_PUT,

    ACTION_HOOK,
    ACTION_UNHOOK,
    ACTION_PUT,

    ACTION_UTF8_2_ENTRY,
    ACTION_UTF8_3_ENTRY,
    ACTION_UTF8_4_ENTRY,
    ACTION_UTF8_COLLECT,
};

static const char *const state_names[] = {
    [STATE_SAME] = "no change",
    [STATE_GROUND] = "ground",

    [STATE_ESCAPE] = "escape",
    [STATE_ESCAPE_INTERMEDIATE] = "escape intermediate",

    [STATE_CSI_ENTRY] = "CSI entry",
    [STATE_CSI_PARAM] = "CSI param",
    [STATE_CSI_INTERMEDIATE] = "CSI intermediate",
    [STATE_CSI_IGNORE] = "CSI ignore",

    [STATE_OSC_STRING] = "OSC string",

    [STATE_DCS_ENTRY] = "DCS entry",
    [STATE_DCS_PARAM] = "DCS param",
    [STATE_DCS_INTERMEDIATE] = "DCS intermediate",
    [STATE_DCS_IGNORE] = "DCS ignore",
    [STATE_DCS_PASSTHROUGH] = "DCS passthrough",

    [STATE_SOS_PM_APC_STRING] = "sos/pm/apc string",

    [STATE_UTF8_COLLECT] = "UTF-8",
};

static const char *const action_names[] __attribute__((unused)) = {
    [ACTION_NONE] = "no action",
    [ACTION_IGNORE] = "ignore",
    [ACTION_CLEAR] = "clear",
    [ACTION_EXECUTE] = "execute",
    [ACTION_PRINT] = "print",
    [ACTION_PARAM] = "param",
    [ACTION_COLLECT] = "collect",
    [ACTION_ESC_DISPATCH] = "ESC dispatch",
    [ACTION_CSI_DISPATCH] = "CSI dispatch",
    [ACTION_OSC_START] = "OSC start",
    [ACTION_OSC_END] = "OSC end",
    [ACTION_OSC_PUT] = "OSC put",
    [ACTION_HOOK] = "hook",
    [ACTION_UNHOOK] = "unhook",
    [ACTION_PUT] = "put",

    [ACTION_UTF8_2_ENTRY] = "UTF-8 (2 chars) begin",
    [ACTION_UTF8_3_ENTRY] = "UTF-8 (3 chars) begin",
    [ACTION_UTF8_4_ENTRY] = "UTF-8 (4 chars) begin",
    [ACTION_UTF8_COLLECT] = "UTF-8 collect",
};

struct state_transition {
    enum action action;
    enum state state;
};

#if 0
static const struct state_transition state_anywhere[256] = {
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};
#endif

static const struct state_transition state_ground[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x7f] = {.action = ACTION_PRINT},

    [0xc0 ... 0xdf] = {.action = ACTION_UTF8_2_ENTRY, .state = STATE_UTF8_COLLECT},
    [0xe0 ... 0xef] = {.action = ACTION_UTF8_3_ENTRY, .state = STATE_UTF8_COLLECT},
    [0xf0 ... 0xf7] = {.action = ACTION_UTF8_4_ENTRY, .state = STATE_UTF8_COLLECT},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_escape[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT,      .state = STATE_ESCAPE_INTERMEDIATE},
    [0x30 ... 0x4f] = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x50]          = {                               .state = STATE_DCS_ENTRY},
    [0x51 ... 0x57] = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x58]          = {                               .state = STATE_SOS_PM_APC_STRING},
    [0x59]          = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x5a]          = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x5b]          = {                               .state = STATE_CSI_ENTRY},
    [0x5c]          = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x5d]          = {                               .state = STATE_OSC_STRING},
    [0x5e ... 0x5f] = {                               .state = STATE_SOS_PM_APC_STRING},
    [0x60 ... 0x7e] = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_escape_intermediate[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT},
    [0x30 ... 0x7e] = {.action = ACTION_ESC_DISPATCH, .state = STATE_GROUND},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_csi_entry[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT,      .state = STATE_CSI_INTERMEDIATE},
    [0x30 ... 0x39] = {.action = ACTION_PARAM,        .state = STATE_CSI_PARAM},
    [0x3a]          = {                               .state = STATE_CSI_IGNORE},
    [0x3b]          = {.action = ACTION_PARAM,        .state = STATE_CSI_PARAM},
    [0x3c ... 0x3f] = {.action = ACTION_COLLECT,      .state = STATE_CSI_PARAM},
    [0x40 ... 0x7e] = {.action = ACTION_CSI_DISPATCH, .state = STATE_GROUND},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_csi_param[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT,      .state = STATE_CSI_INTERMEDIATE},
    [0x30 ... 0x39] = {.action = ACTION_PARAM},
    [0x3a]          = {                               .state = STATE_CSI_IGNORE},
    [0x3b]          = {.action = ACTION_PARAM},
    [0x3c ... 0x3f] = {                               .state = STATE_CSI_IGNORE},
    [0x40 ... 0x7e] = {.action = ACTION_CSI_DISPATCH, .state = STATE_GROUND},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_csi_intermediate[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT},
    [0x30 ... 0x3f] = {                               .state = STATE_CSI_IGNORE},
    [0x40 ... 0x7e] = {.action = ACTION_CSI_DISPATCH, .state = STATE_GROUND},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_csi_ignore[256] = {
    [0x00 ... 0x17] = {.action = ACTION_EXECUTE},
    [0x19]          = {.action = ACTION_EXECUTE},
    [0x1c ... 0x1f] = {.action = ACTION_EXECUTE},
    [0x20 ... 0x3f] = {.action = ACTION_IGNORE},
    [0x40 ... 0x7e] = {                          .state = STATE_GROUND},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_ocs_string[256] = {
    [0x00 ... 0x06] = {.action = ACTION_IGNORE},
    [0x07]          = {                           .state = STATE_GROUND},
    [0x08 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x1f] = {.action = ACTION_IGNORE},
    [0x20 ... 0x7f] = {.action = ACTION_OSC_PUT},
    [0x9c]          = {                           .state = STATE_GROUND},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_dcs_entry[256] = {
    [0x00 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x1f] = {.action = ACTION_IGNORE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT, .state = STATE_DCS_INTERMEDIATE},
    [0x30 ... 0x39] = {.action = ACTION_PARAM,   .state = STATE_DCS_PARAM},
    [0x3a]          = {                          .state = STATE_DCS_IGNORE},
    [0x3b]          = {.action = ACTION_PARAM,   .state = STATE_DCS_PARAM},
    [0x3c ... 0x3f] = {.action = ACTION_COLLECT, .state = STATE_DCS_PARAM},
    [0x40 ... 0x7e] = {                          .state = STATE_DCS_PASSTHROUGH},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_dcs_param[256] = {
    [0x00 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x1f] = {.action = ACTION_IGNORE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT, .state = STATE_DCS_INTERMEDIATE},
    [0x30 ... 0x39] = {.action = ACTION_PARAM},
    [0x3a]          = {                          .state = STATE_DCS_IGNORE},
    [0x3b]          = {.action = ACTION_PARAM},
    [0x3c ... 0x3f] = {                          .state = STATE_DCS_IGNORE},
    [0x40 ... 0x7e] = {                          .state = STATE_DCS_PASSTHROUGH},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_dcs_intermediate[256] = {
    [0x00 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x1f] = {.action = ACTION_IGNORE},
    [0x20 ... 0x2f] = {.action = ACTION_COLLECT},
    [0x30 ... 0x3f] = {                          .state = STATE_DCS_IGNORE},
    [0x40 ... 0x7e] = {                          .state = STATE_DCS_PASSTHROUGH},
    [0x7f]          = {.action = ACTION_IGNORE},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_dcs_ignore[256] = {
    [0x00 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x1f] = {.action = ACTION_IGNORE},
    [0x20 ... 0x7f] = {.action = ACTION_IGNORE},
    [0x9c]          = {                         .state = STATE_GROUND},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_dcs_passthrough[256] = {
    [0x00 ... 0x17] = {.action = ACTION_PUT},
    [0x19]          = {.action = ACTION_PUT},
    [0x1c ... 0x7e] = {.action = ACTION_PUT},
    [0x7f]          = {.action = ACTION_IGNORE},
    [0x9c]          = {                         .state = STATE_GROUND},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition state_sos_pm_apc_string[256] = {
    [0x00 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x7f] = {.action = ACTION_IGNORE},
    [0x9c]          = {                         .state = STATE_GROUND},

    /* Anywhere */
    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
    [0x80 ... 0x8f] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x90]          = {                          .state = STATE_DCS_ENTRY},
    [0x91 ... 0x97] = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x98]          = {                          .state = STATE_SOS_PM_APC_STRING},
    [0x99]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x9b]          = {                          .state = STATE_CSI_ENTRY},
    [0x9c]          = {                          .state = STATE_GROUND},
    [0x9d]          = {                          .state = STATE_OSC_STRING},
    [0x9e ... 0x9f] = {                          .state = STATE_SOS_PM_APC_STRING},
};

static const struct state_transition* states[] = {
    [STATE_GROUND] = state_ground,
    [STATE_ESCAPE] = state_escape,
    [STATE_ESCAPE_INTERMEDIATE] = state_escape_intermediate,
    [STATE_CSI_ENTRY] = state_csi_entry,
    [STATE_CSI_PARAM] = state_csi_param,
    [STATE_CSI_INTERMEDIATE] = state_csi_intermediate,
    [STATE_CSI_IGNORE] = state_csi_ignore,
    [STATE_OSC_STRING] = state_ocs_string,
    [STATE_DCS_ENTRY] = state_dcs_entry,
    [STATE_DCS_PARAM] = state_dcs_param,
    [STATE_DCS_INTERMEDIATE] = state_dcs_intermediate,
    [STATE_DCS_IGNORE] = state_dcs_ignore,
    [STATE_DCS_PASSTHROUGH] = state_dcs_passthrough,
    [STATE_SOS_PM_APC_STRING] = state_sos_pm_apc_string,
};

static const enum action entry_actions[] = {
    [STATE_SAME] = ACTION_NONE,
    [STATE_GROUND] = ACTION_NONE,
    [STATE_ESCAPE] = ACTION_CLEAR,
    [STATE_CSI_ENTRY] = ACTION_CLEAR,
    [STATE_CSI_PARAM] = ACTION_NONE,
    [STATE_CSI_INTERMEDIATE] = ACTION_NONE,
    [STATE_CSI_IGNORE] = ACTION_NONE,
    [STATE_OSC_STRING] = ACTION_OSC_START,
    [STATE_UTF8_COLLECT] = ACTION_NONE,
    [STATE_DCS_ENTRY] = ACTION_CLEAR,
    [STATE_DCS_PARAM] = ACTION_NONE,
    [STATE_DCS_INTERMEDIATE] = ACTION_NONE,
    [STATE_DCS_IGNORE] = ACTION_NONE,
    [STATE_DCS_PASSTHROUGH] = ACTION_HOOK,
    [STATE_SOS_PM_APC_STRING] = ACTION_NONE,
};

static const enum action exit_actions[] = {
    [STATE_SAME] = ACTION_NONE,
    [STATE_GROUND] = ACTION_NONE,
    [STATE_ESCAPE] = ACTION_NONE,
    [STATE_CSI_ENTRY] = ACTION_NONE,
    [STATE_CSI_PARAM] = ACTION_NONE,
    [STATE_CSI_INTERMEDIATE] = ACTION_NONE,
    [STATE_CSI_IGNORE] = ACTION_NONE,
    [STATE_OSC_STRING] = ACTION_OSC_END,
    [STATE_UTF8_COLLECT] = ACTION_NONE,
    [STATE_DCS_ENTRY] = ACTION_NONE,
    [STATE_DCS_PARAM] = ACTION_NONE,
    [STATE_DCS_INTERMEDIATE] = ACTION_NONE,
    [STATE_DCS_IGNORE] = ACTION_NONE,
    [STATE_DCS_PASSTHROUGH] = ACTION_UNHOOK,
    [STATE_SOS_PM_APC_STRING] = ACTION_NONE,
};

static bool
esc_dispatch(struct terminal *term, uint8_t final)
{
#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLED_DBG
    char log[1024];
    int c = snprintf(log, sizeof(log), "ESC: ");

    for (size_t i = 0; i < term->vt.intermediates.idx; i++)
        c += snprintf(&log[c], sizeof(log) - c, "%c", term->vt.intermediates.data[i]);

    c += snprintf(&log[c], sizeof(log) - c, "%c", final);
    LOG_DBG("%s", log);
#endif

    switch (final) {
    case 'B': {
        char param = term->vt.params.idx > 0 ? term->vt.params.v[0].value : '(';

        switch (param) {
        case '(':
            /* This is the default charset */
            break;

        case ')':
        case '*':
        case '+':
            LOG_ERR("unimplemented: character charset: %c", param);
            return false;

        default:
            LOG_ERR("ESC <id> B: invalid charset identifier: %c", param);
            return false;
        }
        break;
    }

    case 'M':
        /* ri - reverse index (scroll reverse) */
        term_scroll_reverse(term, 1);
        break;

    case '=':
        /* Other half of xterm's smkx */
        LOG_WARN("unimplemented: keypad mode change");
        break;

    case '>':
        /* Other half of xterm's rmkx */
        LOG_WARN("unimplemented: keypad mode change");
        break;

    default:
        LOG_ERR("ESC: unimplemented final: %c", final);
        return false;
    }

    return true;
}

static bool
action(struct terminal *term, enum action action, uint8_t c)
{
    switch (action) {
    case ACTION_NONE:
        break;

    case ACTION_IGNORE:
        break;

    case ACTION_EXECUTE:
        LOG_DBG("execute: 0x%02x", c);
        switch (c) {
        case '\n':
            /* LF - line feed */
            if (term->cursor.row == term->scroll_region.end - 1) {
                term_scroll(term, 1);
            } else
                term_cursor_down(term, 1);
            break;

        case '\r':
            /* FF - form feed */
            term_cursor_left(term, term->cursor.col);
            break;

        case '\b':
            /* backspace */
            term_cursor_left(term, 1);
            break;

        case '\x07':
            /* BEL */
            LOG_WARN("BELL");
            break;

        case '\x09': {
            /* HT - horizontal tab */
            int col = term->cursor.col;
            col = (col + 8) / 8 * 8;
            term_cursor_right(term, col - term->cursor.col);
            break;
        }

        default:
            LOG_ERR("execute: unimplemented: %c (0x%02x)", c, c);
            return false;
        }

        return true;

    case ACTION_CLEAR:
        memset(&term->vt.params, 0, sizeof(term->vt.params));
        term->vt.intermediates.idx = 0;
        term->vt.osc.idx = 0;
        term->vt.utf8.idx = 0;
        break;

    case ACTION_PRINT: {
        if (term->print_needs_wrap) {
            if (term->cursor.row == term->scroll_region.end - 1) {
                term_scroll(term, 1);
                term_cursor_to(term, term->cursor.row, 0);
            } else
                term_cursor_to(term, term->cursor.row + 1, 0);
        }

        struct cell *cell = &term->grid->cur_line[term->cursor.col];
        term_damage_update(term, term->cursor.linear, 1);

        if (term->vt.utf8.idx > 0) {
            //LOG_DBG("print: UTF8: %.*s", (int)term->vt.utf8.idx, term->vt.utf8.data);
            memcpy(cell->c, term->vt.utf8.data, term->vt.utf8.idx);
            cell->c[term->vt.utf8.idx] = '\0';
        } else {
            //LOG_DBG("print: ASCII: %c", c);
            cell->c[0] = c;
            cell->c[1] = '\0';
        }

        cell->attrs = term->vt.attrs;

        if (term->cursor.col < term->cols - 1)
            term_cursor_right(term, 1);
        else
            term->print_needs_wrap = true;

        break;
    }

    case ACTION_PARAM:{
        if (term->vt.params.idx == 0)
            term->vt.params.idx = 1;

        if (c == ';') {
            term->vt.params.idx++;
        } else if (c == ':') {
            if (term->vt.params.v[term->vt.params.idx - 1].sub.idx == 0)
                term->vt.params.v[term->vt.params.idx - 1].sub.idx = 1;
        } else {
            if (term->vt.params.v[term->vt.params.idx - 1].sub.idx > 0)
                term->vt.params.v[term->vt.params.idx - 1].sub.value[term->vt.params.v[term->vt.params.idx - 1].sub.idx] *= 10;
            else
                term->vt.params.v[term->vt.params.idx - 1].value *= 10;

            if (term->vt.params.v[term->vt.params.idx - 1].sub.idx > 0)
                term->vt.params.v[term->vt.params.idx - 1].sub.value[term->vt.params.v[term->vt.params.idx - 1].sub.idx] += c - '0';
            else
                term->vt.params.v[term->vt.params.idx - 1].value += c - '0';
        }
        break;
                }

    case ACTION_COLLECT:
        LOG_DBG("collect");
        term->vt.intermediates.data[term->vt.intermediates.idx++] = c;
        break;

        LOG_ERR("unimplemented: action ESC dispatch");
        return false;

    case ACTION_ESC_DISPATCH:
        return esc_dispatch(term, c);
        break;

    case ACTION_CSI_DISPATCH:
        return csi_dispatch(term, c);

    case ACTION_OSC_START:
        term->vt.osc.idx = 0;
        break;

    case ACTION_OSC_PUT:
        term->vt.osc.data[term->vt.osc.idx++] = c;
        break;

    case ACTION_OSC_END:
        return osc_dispatch(term);

    case ACTION_HOOK:
    case ACTION_UNHOOK:
    case ACTION_PUT:
        LOG_ERR("unimplemented: action %s", action_names[action]);
        return false;

    case ACTION_UTF8_2_ENTRY:
        term->vt.utf8.idx = 0;
        term->vt.utf8.left = 2;
        term->vt.utf8.data[term->vt.utf8.idx++] = c;
        term->vt.utf8.left--;
        break;

    case ACTION_UTF8_3_ENTRY:
        term->vt.utf8.idx = 0;
        term->vt.utf8.left = 3;
        term->vt.utf8.data[term->vt.utf8.idx++] = c;
        term->vt.utf8.left--;
        break;

    case ACTION_UTF8_4_ENTRY:
        term->vt.utf8.idx = 0;
        term->vt.utf8.left = 4;
        term->vt.utf8.data[term->vt.utf8.idx++] = c;
        term->vt.utf8.left--;
        break;

    case ACTION_UTF8_COLLECT:
        term->vt.utf8.data[term->vt.utf8.idx++] = c;
        if (--term->vt.utf8.left == 0)
            term->vt.state = STATE_GROUND;
        break;
    }

    return true;
}

void
vt_from_slave(struct terminal *term, const uint8_t *data, size_t len)
{
    //int cursor = term->grid.cursor;
    for (size_t i = 0; i < len; i++) {
        //LOG_DBG("input: 0x%02x", data[i]);
        enum state current_state = term->vt.state;

        if (current_state == STATE_UTF8_COLLECT) {
            if (!action(term, ACTION_UTF8_COLLECT, data[i]))
                abort();

            current_state = term->vt.state;
            if (current_state == STATE_UTF8_COLLECT)
                continue;

            if (!action(term, ACTION_PRINT, 0))
                abort();

            continue;
        }

        const struct state_transition *transition = &states[current_state][data[i]];
        if (transition->action == ACTION_NONE && transition->state == STATE_SAME) {
            LOG_ERR("unimplemented transition from %s: 0x%02x",
                    state_names[current_state], data[i]);
            abort();
        }

        if (transition->state != STATE_SAME) {
            enum action exit_action = exit_actions[current_state];
            if (exit_action != ACTION_NONE && !action(term, exit_action, data[i]))
                abort();
        }

        if (!action(term, transition->action, data[i]))
            abort();

        if (transition->state != STATE_SAME) {
            /*
             * LOG_DBG("transition: %s -> %s", state_names[current_state],
             *         state_names[transition->state]);
             */
            term->vt.state = transition->state;

            enum action entry_action = entry_actions[transition->state];
            if (entry_action != ACTION_NONE && !action(term, entry_action, data[i]))
                abort();
        }
    }
}
