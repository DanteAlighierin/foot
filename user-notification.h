#pragma once

#include <tllist.h>

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
