/*
 * Event bindings: one axevent subscription per metric. Each event that
 * matches the metric's optional source filter sets its value.
 */
#ifndef BINDINGS_H
#define BINDINGS_H

#include <stdbool.h>

#include "cJSON.h"
#include "metrics_config.h"

bool bindings_init(void);
/* Subscribes every metric that is bound to an event. The metrics config
 * must stay valid until bindings_stop(). */
void bindings_start(const MetricsConfig *metrics);
void bindings_stop(void);
/* Per-metric status for the settings page. */
cJSON *bindings_status(void);
void bindings_cleanup(void);

#endif
