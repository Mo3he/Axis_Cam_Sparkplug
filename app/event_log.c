#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

static void log_message(int priority, const char *source, const char *fmt, va_list args) {
    char message[512];
    vsnprintf(message, sizeof(message), fmt, args);
    syslog(priority, "%s: %s", source, message);
}

void EventLog_System(const char *source, const char *level, const char *fmt, ...) {
    va_list args;
    int priority = LOG_INFO;
    if (level && strcmp(level, "error") == 0)
        priority = LOG_ERR;
    else if (level && strcmp(level, "warning") == 0)
        priority = LOG_WARNING;
    va_start(args, fmt);
    log_message(priority, source, fmt, args);
    va_end(args);
}

void EventLog_Syslog(int priority, const char *source, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(priority, source, fmt, args);
    va_end(args);
}
