#include "command_events.h"

#include <axsdk/axevent.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#define APP_TOPIC "Sparkplug_Edge"
#define EVENT_TOPIC "MetricWritten"

static AXEventHandler *handler;
static const MetricsConfig *declared_metrics;
/* By metric index in declared_metrics. */
static guint declarations[MAX_METRICS];
static bool declared[MAX_METRICS];
static char declared_names[MAX_METRICS][METRIC_NAME_MAX_LEN + 1];

void command_events_init(void) {
    handler = ax_event_handler_new();
    if (!handler)
        syslog(LOG_ERR, "could not create the event handler for host writes");
}

static bool declare(const MetricConfig *metric, guint *id) {
    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    GError *error = NULL;
    bool ok =
        ax_event_key_value_set_add_key_value(set, "topic0", "tnsaxis", "CameraApplicationPlatform",
                                             AX_VALUE_TYPE_STRING, &error) &&
        ax_event_key_value_set_add_key_value(set, "topic1", "tnsaxis", APP_TOPIC, AX_VALUE_TYPE_STRING, &error) &&
        ax_event_key_value_set_add_nice_names(set, "topic1", "tnsaxis", APP_TOPIC, "Sparkplug Edge Node", &error) &&
        ax_event_key_value_set_add_key_value(set, "topic2", "tnsaxis", EVENT_TOPIC, AX_VALUE_TYPE_STRING, &error) &&
        ax_event_key_value_set_add_nice_names(set, "topic2", "tnsaxis", EVENT_TOPIC, "Metric written by host", &error) &&
        ax_event_key_value_set_add_key_value(set, "metric", NULL, metric->name, AX_VALUE_TYPE_STRING, &error) &&
        ax_event_key_value_set_mark_as_source(set, "metric", NULL, &error) &&
        ax_event_key_value_set_add_nice_names(set, "metric", NULL, "Metric", NULL, &error) &&
        ax_event_key_value_set_add_key_value(set, "value", NULL, NULL, AX_VALUE_TYPE_STRING, &error) &&
        ax_event_key_value_set_mark_as_data(set, "value", NULL, &error) &&
        ax_event_key_value_set_add_nice_names(set, "value", NULL, "Value", NULL, &error) &&
        ax_event_handler_declare(handler, set, TRUE, id, NULL, NULL, &error);
    if (!ok)
        syslog(LOG_WARNING, "metric '%s': declaring the write event failed: %s", metric->name,
               error ? error->message : "unknown error");
    g_clear_error(&error);
    ax_event_key_value_set_free(set);
    return ok;
}

/* ax_event_handler_undeclare() blocks for about 2 s on the main loop, so
 * declarations are kept for metrics that stay writable under the same name
 * and only the others are undeclared or declared. */
void command_events_declare(const MetricsConfig *metrics) {
    guint next_ids[MAX_METRICS];
    bool next_declared[MAX_METRICS] = {false};
    if (!handler)
        return;

    for (int i = 0; i < metrics->count; i++) {
        if (!metrics->metrics[i].writable)
            continue;
        for (int old = 0; old < MAX_METRICS; old++) {
            if (declared[old] && strcmp(declared_names[old], metrics->metrics[i].name) == 0) {
                next_ids[i] = declarations[old];
                next_declared[i] = true;
                declared[old] = false;
                break;
            }
        }
    }
    for (int old = 0; old < MAX_METRICS; old++) {
        if (declared[old])
            ax_event_handler_undeclare(handler, declarations[old], NULL);
        declared[old] = false;
    }
    for (int i = 0; i < metrics->count; i++) {
        if (metrics->metrics[i].writable && !next_declared[i])
            next_declared[i] = declare(&metrics->metrics[i], &next_ids[i]);
        declared[i] = next_declared[i];
        if (declared[i]) {
            declarations[i] = next_ids[i];
            snprintf(declared_names[i], sizeof(declared_names[i]), "%s", metrics->metrics[i].name);
        }
    }
    declared_metrics = metrics;
}

void command_events_send(int index, const char *value) {
    if (!handler || !declared_metrics || index < 0 || index >= declared_metrics->count || !declared[index])
        return;
    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    GError *error = NULL;
    AXEvent *event = NULL;
    bool ok = ax_event_key_value_set_add_key_value(set, "metric", NULL, declared_metrics->metrics[index].name,
                                                   AX_VALUE_TYPE_STRING, &error) &&
              ax_event_key_value_set_add_key_value(set, "value", NULL, value, AX_VALUE_TYPE_STRING, &error);
    if (ok) {
        event = ax_event_new2(set, NULL);
        ok = event && ax_event_handler_send_event(handler, declarations[index], event, &error);
    }
    if (!ok)
        syslog(LOG_WARNING, "metric '%s': sending the write event failed: %s", declared_metrics->metrics[index].name,
               error ? error->message : "unknown error");
    if (event)
        ax_event_free(event);
    g_clear_error(&error);
    ax_event_key_value_set_free(set);
}

void command_events_cleanup(void) {
    /* Not undeclare or ax_event_handler_free(): with the main loop stopped
     * they can block until systemd kills the app (seen in Event Engine). The
     * process is exiting and the event service drops its declarations. */
    handler = NULL;
    declared_metrics = NULL;
}
