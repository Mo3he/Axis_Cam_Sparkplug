/*
 * Camera events for host writes. Each writable metric gets its own event
 * declaration (CameraApplicationPlatform/Sparkplug_Edge/MetricWritten with the
 * metric name as source), so AXIS OS action rules and Event Engine can react
 * to a specific metric. Each accepted write sends the written value.
 */
#ifndef COMMAND_EVENTS_H
#define COMMAND_EVENTS_H

#include "metrics_config.h"

void command_events_init(void);
/* Replaces the declarations. The config must stay valid until the next call
 * or command_events_cleanup(). */
void command_events_declare(const MetricsConfig *metrics);
void command_events_send(int index, const char *value);
void command_events_cleanup(void);

#endif
