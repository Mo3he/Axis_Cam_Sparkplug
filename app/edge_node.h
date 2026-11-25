/*
 * Sparkplug B edge node session: birth and death certificates, report by
 * exception, buffering while offline, primary host STATE and host commands.
 *
 * Adapted from Event Engine's engine/sparkplug.c. The session logic is kept
 * the same so fixes can be ported between the two; event matching lives in
 * bindings.c here, and only metrics declared writable accept host writes.
 */
#ifndef EDGE_NODE_H
#define EDGE_NODE_H

#include <stdbool.h>

#include "cJSON.h"
#include "metrics_config.h"

#define EDGE_ID_MAX_LEN 64

typedef enum { SPEC_3_0, SPEC_2_2 } SparkplugSpec;

typedef struct {
    bool enabled;
    SparkplugSpec spec;
    char group_id[EDGE_ID_MAX_LEN + 1];
    char edge_node_id[EDGE_ID_MAX_LEN + 1];
    char device_id[EDGE_ID_MAX_LEN + 1];       /* optional */
    char primary_host_id[EDGE_ID_MAX_LEN + 1]; /* optional */
} EdgeNodeSettings;

/* Sparkplug ids must not contain the MQTT topic separators or wildcards. */
bool edge_node_valid_id(const char *id);

/* bdseq_file keeps bdSeq across restarts. Registers the MQTT state callback. */
void edge_node_init(const char *bdseq_file);
void edge_node_cleanup(void);

/* Applies identity and metric list. Metrics keep their last value when name
 * and type are unchanged. A changed identity or list forces a new birth. */
void edge_node_configure(const EdgeNodeSettings *settings, const MetricsConfig *metrics);

/* New value for metric index, as text from metric_value_from_event().
 * Published only when it differs from the last value (report by exception). */
void edge_node_set_value(int index, const char *text);

/* Called on the main loop for each accepted write to a writable metric. */
typedef void (*EdgeNodeCommandFn)(int index, const char *value);
void edge_node_set_command_callback(EdgeNodeCommandFn fn);

/* MQTT_Message_Callback. */
void edge_node_on_mqtt_message(const char *topic, const char *payload, int payload_len, void *user_data);

cJSON *edge_node_status(void);
/* Current value of metric index, or NULL when it has none. Caller frees. */
char *edge_node_value(int index);

#endif
