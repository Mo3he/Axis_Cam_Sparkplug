/*
 * Logging for the modules shared with Event Engine (mqtt_client.c, certs.c),
 * so they compile here unchanged. Event Engine also keeps these messages in
 * its own log; this app only writes them to the system log.
 */
#ifndef EVENT_LOG_H
#define EVENT_LOG_H

void EventLog_System(const char *source, const char *level, const char *fmt, ...)
    __attribute__((format(__printf__, 3, 4)));
void EventLog_Syslog(int priority, const char *source, const char *fmt, ...)
    __attribute__((format(__printf__, 3, 4)));

#endif
