#include "user-notification.h"
#include <stdarg.h>
#include "xmalloc.h"

void
user_notification_add_fmt(user_notifications_t *notifications,
                      enum user_notification_kind kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *text = xvasprintf(fmt, ap);
    va_end(ap);
    user_notification_add(notifications, kind, text);
}
