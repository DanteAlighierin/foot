#include "vt.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define LOG_MODULE "vt"
#define LOG_ENABLE_DBG 0
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
    ACTION_UTF8_PRINT,
};

#if 0
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
#endif
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
    [ACTION_UTF8_PRINT] = "UTF-8 print",
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
    [0x3a ... 0x3b] = {                               .state = STATE_CSI_PARAM},
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
    [0x3a ... 0x3b] = {.action = ACTION_PARAM},
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

static const struct state_transition state_osc_string[256] = {
    [0x00 ... 0x06] = {.action = ACTION_IGNORE},
    [0x07]          = {                           .state = STATE_GROUND},
    [0x08 ... 0x17] = {.action = ACTION_IGNORE},
    [0x19]          = {.action = ACTION_IGNORE},
    [0x1c ... 0x1f] = {.action = ACTION_IGNORE},

    [0x20 ... 0xff] = {.action = ACTION_OSC_PUT},

    [0x18]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1a]          = {.action = ACTION_EXECUTE, .state = STATE_GROUND},
    [0x1b]          = {                          .state = STATE_ESCAPE},
#if 0
    [0x20 ... 0x7f] = {.action = ACTION_OSC_PUT},
    [0x9c]          = {                           .state = STATE_GROUND},

    [0xc0 ... 0xdf] = {.action = ACTION_OSC_PUT},
    [0xe0 ... 0xef] = {.action = ACTION_OSC_PUT},
    [0xf0 ... 0xf7] = {.action = ACTION_OSC_PUT},

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
#endif
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
    [STATE_OSC_STRING] = state_osc_string,
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

static const char *
esc_as_string(struct terminal *term, uint8_t final)
{
    static char msg[1024];
    int c = snprintf(msg, sizeof(msg), "\\E");

    if (term->vt.private != 0)
        c += snprintf(&msg[c], sizeof(msg) - c, "%c", term->vt.private);

    assert(term->vt.params.idx == 0);

    c += snprintf(&msg[c], sizeof(msg) - c, "%c", final);
    return msg;

}

static void
esc_dispatch(struct terminal *term, uint8_t final)
{
    LOG_DBG("ESC: %s", esc_as_string(term, final));

    switch (final) {
    case '7':
        term->saved_cursor = term->cursor;
        term->vt.saved_attrs = term->vt.attrs;
        break;

    case '8':
        term->cursor = term->saved_cursor;
        term->vt.attrs = term->vt.saved_attrs;
        break;

    case 'c':
        LOG_WARN("unimplemented: reset to initial state");
        break;

    case 'B': {
        /* Configure G0-G3 to use ASCII */
        char param = term->vt.private != 0 ? term->vt.private : '(';

        switch (param) {
        case '(': term->charset[0] = CHARSET_ASCII; break;
        case ')': term->charset[1] = CHARSET_ASCII; break;
        case '*': term->charset[2] = CHARSET_ASCII; break;
        case '+': term->charset[3] = CHARSET_ASCII; break;

        default:
            LOG_ERR("%s: invalid charset identifier", esc_as_string(term, final));
            abort();
            break;
        }
        break;
    }

    case 'D':
        term_linefeed(term);
        break;

    case 'E':
        term_linefeed(term);
        term_cursor_left(term, term->cursor.col);
        break;

    case 'M':
        term_reverse_index(term);
        break;

    case 'N':
        /* SS2 - Single Shift 2 */
        term->selected_charset = 2; /* G2 */
        break;

    case 'O':
        /* SS3 - Single Shift 3 */
        term->selected_charset = 3; /* G3 */
        break;

    case '\\':
        /* ST - String Terminator */
        break;

    case '0': {
        /* Configure G0-G3 to use special chars + line drawing */
        char param = term->vt.private != 0 ? term->vt.private : '(';

        switch (param) {
        case '(': term->charset[0] = CHARSET_GRAPHIC; break;
        case ')': term->charset[1] = CHARSET_GRAPHIC; break;
        case '*': term->charset[2] = CHARSET_GRAPHIC; break;
        case '+': term->charset[3] = CHARSET_GRAPHIC; break;

        default:
            LOG_ERR("%s: invalid charset identifier", esc_as_string(term, final));
            abort();
            break;
        }
        break;
    }

    case '=':
        term->keypad_keys_mode = KEYPAD_APPLICATION;
        break;

    case '>':
        term->keypad_keys_mode = KEYPAD_NUMERICAL;
        break;

    default:
        LOG_ERR("unimplemented: ESC: %s", esc_as_string(term, final));
        abort();
        break;
    }
}

static inline void
pre_print(struct terminal *term)
{
    if (unlikely(term->print_needs_wrap) && term->auto_margin) {
        if (term->cursor.row == term->scroll_region.end - 1) {
            term_scroll(term, 1);
            term_cursor_to(term, term->cursor.row, 0);
        } else
            term_cursor_to(term, term->cursor.row + 1, 0);
    }
}

static inline void
post_print(struct terminal *term)
{
    term->grid->cur_row->dirty = true;
    if (term->cursor.col < term->cols - 1)
        term_cursor_right(term, 1);
    else
        term->print_needs_wrap = true;
}

static inline void
print_insert(struct terminal *term)
{
   if (unlikely(term->insert_mode)) {
        assert(false && "untested");

        struct row *row = term->grid->cur_row;
        memmove(
            &row[term->cursor.col + 1],
            &row[term->cursor.col],
            term->cols - term->cursor.col - 1);

#if 0
        term_damage_update(
            term, term->cursor.linear + 1, term->cols - term->cursor.col - 1);
#else
        row->dirty = true;
#endif
    }
}

static void
action_print_utf8(struct terminal *term)
{
    pre_print(term);

    struct row *row = term->grid->cur_row;
    struct cell *cell = &row->cells[term->cursor.col];
#if 0
    term_damage_update(term, term->cursor.linear, 1);
#else
    row->dirty = true;
#endif

    print_insert(term);

    //LOG_DBG("print: UTF8: %.*s", (int)term->vt.utf8.idx, term->vt.utf8.data);
    memcpy(cell->c, term->vt.utf8.data, term->vt.utf8.idx);
    cell->c[term->vt.utf8.idx] = '\0';
    term->vt.utf8.idx = 0;

    cell->attrs = term->vt.attrs;
    post_print(term);
}

static void
action_print(struct terminal *term, uint8_t c)
{
    pre_print(term);

    struct row *row = term->grid->cur_row;
    struct cell *cell = &row->cells[term->cursor.col];
#if 0
    term_damage_update(term, term->cursor.linear, 1);
#else
    row->dirty = true;
#endif

    print_insert(term);

    static const char *const vt100_0[62] = { /* 0x41 - 0x7e */
        "↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
        0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
        0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
        0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
        "◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
        "␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
        "⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
        "│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
    };

    if (unlikely(term->charset[term->selected_charset] == CHARSET_GRAPHIC) &&
        c >= 0x41 && c <= 0x7e)
    {
        const char *glyph = vt100_0[c - 0x41];
        if (glyph != NULL)
            strncpy(cell->c, glyph, sizeof(cell->c));
    } else {
        //LOG_DBG("print: ASCII: %c", c);
        cell->c[0] = c;
        cell->c[1] = '\0';
    }

    cell->attrs = term->vt.attrs;

    post_print(term);
}

static void
action(struct terminal *term, enum action _action, uint8_t c)
{
    switch (_action) {
    case ACTION_NONE:
        break;

    case ACTION_IGNORE:
        break;

    case ACTION_EXECUTE:
        LOG_DBG("execute: 0x%02x", c);
        switch (c) {
        case '\0':
            break;

        case '\n':
            /* LF - line feed */
            term_linefeed(term);
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

        case '\016':
            /* SO - shift out */
            term->selected_charset = 1; /* G1 */
            break;

        case '\017':
            /* SI - shift in */
            term->selected_charset = 0; /* G0 */
            break;

        default:
            LOG_WARN("execute: ignoring: %c (0x%02x)", c, c);
            break;
        }

        break;

    case ACTION_CLEAR:
        memset(&term->vt.params, 0, sizeof(term->vt.params));
        term->vt.private = 0;
        term->vt.osc.idx = 0;
        term->vt.utf8.idx = 0;
        break;

    case ACTION_PRINT:
        action_print(term, c);
        break;

    case ACTION_UTF8_PRINT:
        action_print_utf8(term);
        break;

    case ACTION_PARAM:
        if (term->vt.params.idx == 0)
            term->vt.params.idx = 1;

        if (c == ';') {
            term->vt.params.idx++;
        } else if (c == ':') {
            term->vt.params.v[term->vt.params.idx - 1].sub.idx++;
        } else {
            assert(term->vt.params.idx >= 0);
            struct vt_param *param = &term->vt.params.v[term->vt.params.idx - 1];

            unsigned *value = param->sub.idx > 0
                ? &param->sub.value[param->sub.idx - 1]
                : &param->value;

            *value *= 10;
            *value += c - '0';
        }
        break;

    case ACTION_COLLECT:
        LOG_DBG("collect");
        if (term->vt.private != 0) {
            LOG_ERR("only one private/intermediate characters supported");
            abort();
        }
        term->vt.private = c;
        break;

    case ACTION_ESC_DISPATCH:
        esc_dispatch(term, c);
        break;

    case ACTION_CSI_DISPATCH:
        csi_dispatch(term, c);
        break;

    case ACTION_OSC_START:
        term->vt.osc.idx = 0;
        break;

    case ACTION_OSC_PUT:
        if (term->vt.osc.idx < (int)sizeof(term->vt.osc.data) - 1)
            term->vt.osc.data[term->vt.osc.idx++] = c;
        break;

    case ACTION_OSC_END:
        assert(term->vt.osc.idx < sizeof(term->vt.osc.data));
        term->vt.osc.data[term->vt.osc.idx] = '\0';
        osc_dispatch(term);
        break;

    case ACTION_HOOK:
    case ACTION_UNHOOK:
    case ACTION_PUT:
        LOG_ERR("unimplemented: action %s", action_names[_action]);
        abort();

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
        if (--term->vt.utf8.left == 0) {
            term->vt.state = STATE_GROUND;
            action_print_utf8(term);
        }
        break;
    }
}

void
vt_from_slave(struct terminal *term, const uint8_t *data, size_t len)
{
    //LOG_DBG("input: 0x%02x", data[i]);
    enum state current_state = term->vt.state;

    for (size_t i = 0; i < len; i++) {

        if (current_state == STATE_UTF8_COLLECT) {
            action(term, ACTION_UTF8_COLLECT, data[i]);

            current_state = term->vt.state;
            continue;
        }

        const struct state_transition *transition = &states[current_state][data[i]];
        assert(transition->action != ACTION_NONE || transition->state != STATE_SAME);

        if (transition->state != STATE_SAME) {
            enum action exit_action = exit_actions[current_state];
            action(term, exit_action, data[i]);
        }

        action(term, transition->action, data[i]);

        if (transition->state != STATE_SAME) {
            /*
             * LOG_DBG("transition: %s -> %s", state_names[current_state],
             *         state_names[transition->state]);
             */
            term->vt.state = transition->state;
            current_state = transition->state;

            enum action entry_action = entry_actions[transition->state];
            action(term, entry_action, data[i]);
        }
    }
}

bool
vt_to_slave(struct terminal *term, const void *_data, size_t len)
{
    const uint8_t *data = _data;
    size_t left = len;

    while (left > 0) {
        ssize_t ret = write(term->ptmx, data, left);
        if (ret < 0) {
            LOG_ERRNO("failed to write to client");
            return false;
        }

        data += ret;
        left -= ret;
    }

    return true;
}
