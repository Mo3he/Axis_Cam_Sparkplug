/*
 * Sparkplug metric declarations: parsing, validation, and the conversion of
 * camera event values and host commands to metric values. No AXIS SDK or MQTT
 * dependencies, so it is unit-tested on the host (tests/).
 */
#ifndef METRICS_CONFIG_H
#define METRICS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

#define MAX_METRICS 128
#define MAX_TOPIC_LEVELS 6
#define METRIC_NAME_MAX_LEN 127
#define TOPIC_NAME_MAX_LEN 64
#define KEY_MAX_LEN 64
#define VALUE_MAX_LEN 64
#define METRIC_VALUE_MAX_LEN 255

typedef enum { MT_BOOLEAN, MT_INT32, MT_INT64, MT_FLOAT, MT_DOUBLE, MT_STRING } MetricType;

typedef struct {
    char ns[16]; /* "tns1" or "tnsaxis" */
    char name[TOPIC_NAME_MAX_LEN + 1];
} TopicLevel;

typedef struct {
    TopicLevel topic[MAX_TOPIC_LEVELS];
    int topic_count;
    char source_key[KEY_MAX_LEN + 1]; /* optional filter, e.g. "port" */
    char source_value[VALUE_MAX_LEN + 1];
    char data_key[KEY_MAX_LEN + 1]; /* event field that carries the value */
} EventBinding;

typedef struct {
    char name[METRIC_NAME_MAX_LEN + 1];
    MetricType type;
    bool invert;   /* Boolean metrics bound to an event */
    bool writable; /* written by the host; no event binding */
    EventBinding binding;
} MetricConfig;

typedef struct {
    MetricConfig metrics[MAX_METRICS];
    int count;
} MetricsConfig;

/* Parses and validates {"metrics":[...]}. On failure returns false and
 * writes "metrics[i].field: reason" to error. */
bool metrics_config_parse(const cJSON *json, MetricsConfig *out, char *error, size_t error_size);

/* Serializes to the same JSON shape metrics_config_parse accepts. */
cJSON *metrics_config_to_json(const MetricsConfig *config);

/* Sparkplug data type name, e.g. "Int32", as used by sparkplug_pb. */
const char *metric_type_name(MetricType type);

/* Topic path as shown to users, e.g. "tns1:Device/tnsaxis:IO/VirtualInput". */
void binding_topic_path(const EventBinding *binding, char *out, size_t out_size);

/* An event field value, converted from the camera's typed event data. */
typedef enum { EV_BOOL, EV_INT, EV_DOUBLE, EV_STRING } EventValueType;

typedef struct {
    EventValueType type;
    bool b;
    long long i;
    double d;
    char s[VALUE_MAX_LEN + 1];
} EventValue;

/* Text form used for source filters and for the status page. */
void event_value_to_string(const EventValue *value, char *out, size_t out_size);

/* The metric value as text in the form sparkplug_pb parses for the metric's
 * type ("true"/"false", integers, numbers). False when the event value does
 * not fit the type, e.g. text in an Int32 metric. */
bool metric_value_from_event(const MetricConfig *metric, const EventValue *value, char *out, size_t out_size);

/* Same for a value written by the host (decoded command payload). */
bool metric_value_from_command(const MetricConfig *metric, const char *text, char *out, size_t out_size);

/* AXIS Object Analytics occupancy events are only sent when something
 * changes, so a metric bound to one would wait for its first value. These
 * let the app read the current count once instead. True when the binding is
 * an AOA scenario event without a source filter; sets the scenario ID. */
bool binding_aoa_scenario(const EventBinding *binding, int *scenario);
/* Reads data.<key> from a getOccupancy response as an integer value. */
bool aoa_occupancy_value(const cJSON *response, const char *key, EventValue *out);

#endif
