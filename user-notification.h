#pragma once

#include <tllist.h>

#include "macros.h"

enum user_notification_kind {
    USER_NOTIFICATION_DEPRECATED,
    USER_NOTIFICATION_WARNING,
    USER_NOTIFICATION_ERROR,
};

struct user_notification {
    enum user_notification_kind kind;
    char *text;
};

typedef tll(struct user_notification) user_notifications_t;

static inline void
user_notifications_free(user_notifications_t *notifications)
{
    tll_foreach(*notifications, it)
        free(it->item.text);
    tll_free(*notifications);
}

static inline void
user_notification_add(user_notifications_t *notifications,
    enum user_notification_kind kind, char *text)
{
    struct user_notification notification = {
        .kind = kind,
        .text = text
    };
    tll_push_back(*notifications, notification);
}

void user_notification_add_fmt(user_notifications_t *notifications,
                           enum user_notification_kind kind,
                           const char *fmt, ...) PRINTF(3);
