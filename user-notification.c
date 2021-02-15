#include "user-notification.h"
#include <stdio.h>
#include <stdarg.h>

static bool
user_notification_add_va(user_notifications_t *notifications,
                         enum user_notification_kind kind, const char *fmt,
                         va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int cnt = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);

    if (cnt < 0)
        return false;

    char *text = malloc(cnt + 1);
    vsnprintf(text, cnt + 1, fmt, ap);

    struct user_notification not = {
        .kind = kind,
        .text = text,
    };
    tll_push_back(*notifications, not);
    return true;
}

bool
user_notification_add(user_notifications_t *notifications,
                      enum user_notification_kind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ret = user_notification_add_va(notifications, kind, fmt, ap);
    va_end(ap);
    return ret;
}
