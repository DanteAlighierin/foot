#pragma once

#include <tllist.h>

struct user_notification {
    enum {
        USER_NOTIFICATION_DEPRECATED,
        USER_NOTIFICATION_WARNING,
        USER_NOTIFICATION_ERROR,
    } kind;
    char *text;
};

typedef tll(struct user_notification) user_notifications_t;
