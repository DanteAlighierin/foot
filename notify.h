#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include <tllist.h>

struct terminal;

enum notify_when {
    /* First, so that it can be left out of initializer and still be
       the default */
    NOTIFY_ALWAYS,

    NOTIFY_UNFOCUSED,
    NOTIFY_INVISIBLE
};

enum notify_urgency {
    /* First, so that it can be left out of initializer and still be
       the default */
    NOTIFY_URGENCY_NORMAL,

    NOTIFY_URGENCY_LOW,
    NOTIFY_URGENCY_CRITICAL,
};

struct notification {
    /*
     * Set by caller of notify_notify()
     */
    char *id;      /* Internal notification ID */

    char *app_id;  /* Custom app-id, overrides the terminal's app-id if set */
    char *title;   /* Required */
    char *body;
    char *category;

    enum notify_when when;
    enum notify_urgency urgency;
    int32_t expire_time;

    tll(char *) actions;

    char *icon_cache_id;
    char *icon_symbolic_name;
    uint8_t *icon_data;
    size_t icon_data_sz;

    bool focus;    /* Focus the foot window when notification is activated */
    bool may_be_programatically_closed; /* OSC-99: notification may be programmatically closed by the client */
    bool report_activated;  /* OSC-99: report notification activation to client */
    bool report_closed;     /* OSC-99: report notification closed to client */

    bool muted;            /* Explicitly mute the notification */
    char *sound_name;      /* Should be set to NULL if muted == true */

    /*
     * Used internally by notify
     */

    uint32_t external_id;  /* Daemon assigned notification ID */
    bool activated;        /* User 'activated' the notification */
    uint32_t button_count; /* Number of buttons (custom actions) in notification */
    uint32_t activated_button; /* User activated one of the custom actions */
    char *xdg_token;       /* XDG activation token, from daemon */

    pid_t pid;             /* Notifier command PID */
    int stdout_fd;         /* Notifier command's stdout */

    char *stdout_data;     /* Data we've reado from command's stdout */
    size_t stdout_sz;

    /* Used when notification provides raw icon data, and it's
       bypassing the icon cache */
    char *icon_path;
    int icon_fd;
};

struct notification_icon {
    char *id;
    char *symbolic_name;
    char *tmp_file_name;
    int tmp_file_fd;
};

bool notify_notify(struct terminal *term, struct notification *notif);
void notify_close(struct terminal *term, const char *id);
void notify_free(struct terminal *term, struct notification *notif);

void notify_icon_add(struct terminal *term, const char *id,
                     const char *symbolic_name, const uint8_t *data,
                     size_t data_sz);
void notify_icon_del(struct terminal *term, const char *id);
void notify_icon_free(struct notification_icon *icon);
