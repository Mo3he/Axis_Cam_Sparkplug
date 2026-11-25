#include "metrics_config.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const struct {
    MetricType type;
    const char *name;
} type_names[] = {
    {MT_BOOLEAN, "Boolean"}, {MT_INT32, "Int32"},   {MT_INT64, "Int64"},
    {MT_FLOAT, "Float"},     {MT_DOUBLE, "Double"}, {MT_STRING, "String"},
};

const char *metric_type_name(MetricType type) {
    for (size_t i = 0; i < sizeof(type_names) / sizeof(type_names[0]); i++) {
        if (type_names[i].type == type)
            return type_names[i].name;
    }
    return "String";
}

static bool type_from_name(const char *name, MetricType *type) {
    for (size_t i = 0; name && i < sizeof(type_names) / sizeof(type_names[0]); i++) {
        if (strcmp(type_names[i].name, name) == 0) {
            *type = type_names[i].type;
            return true;
        }
    }
    return false;
}

static bool fail(char *error, size_t error_size, int index, const char *field, const char *reason) {
    if (index >= 0)
        snprintf(error, error_size, "metrics[%d].%s: %s", index, field, reason);
    else
        snprintf(error, error_size, "%s: %s", field, reason);
    return false;
}

/* Copies an optional string field; missing means empty. */
static bool copy_string(const cJSON *object, const char *key, char *out, size_t out_size, bool required,
                        char *error, size_t error_size, int index) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    out[0] = '\0';
    if (!item || cJSON_IsNull(item)) {
        if (required)
            return fail(error, error_size, index, key, "is required");
        return true;
    }
    if (!cJSON_IsString(item))
        return fail(error, error_size, index, key, "must be a string");
    if (strlen(item->valuestring) >= out_size) {
        char reason[64];
        snprintf(reason, sizeof(reason), "is longer than %zu characters", out_size - 1);
        return fail(error, error_size, index, key, reason);
    }
    if (required && item->valuestring[0] == '\0')
        return fail(error, error_size, index, key, "must not be empty");
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static bool valid_identifier(const char *text) {
    if (!*text)
        return false;
    for (const char *c = text; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-' && *c != '.')
            return false;
    }
    return true;
}

static bool parse_topic(const cJSON *topic, EventBinding *binding, char *error, size_t error_size, int index) {
    if (!cJSON_IsArray(topic) || cJSON_GetArraySize(topic) == 0)
        return fail(error, error_size, index, "event.topic", "must be a non-empty array");
    if (cJSON_GetArraySize(topic) > MAX_TOPIC_LEVELS)
        return fail(error, error_size, index, "event.topic", "has more than 6 levels");

    int level = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, topic) {
        char field[32];
        snprintf(field, sizeof(field), "event.topic[%d]", level);
        if (!cJSON_IsString(item))
            return fail(error, error_size, index, field, "must be a string");
        const char *colon = strchr(item->valuestring, ':');
        if (!colon)
            return fail(error, error_size, index, field, "must be namespace:name, e.g. tns1:Device");
        size_t ns_len = (size_t)(colon - item->valuestring);
        TopicLevel *out = &binding->topic[level];
        if (ns_len != 4 && ns_len != 7)
            return fail(error, error_size, index, field, "namespace must be tns1 or tnsaxis");
        memcpy(out->ns, item->valuestring, ns_len);
        out->ns[ns_len] = '\0';
        if (strcmp(out->ns, "tns1") != 0 && strcmp(out->ns, "tnsaxis") != 0)
            return fail(error, error_size, index, field, "namespace must be tns1 or tnsaxis");
        if (strlen(colon + 1) > TOPIC_NAME_MAX_LEN || !valid_identifier(colon + 1))
            return fail(error, error_size, index, field, "has an invalid topic name");
        snprintf(out->name, sizeof(out->name), "%s", colon + 1);
        level++;
    }
    binding->topic_count = level;
    return true;
}

static bool parse_binding(const cJSON *object, EventBinding *binding, char *error, size_t error_size, int index) {
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(object, "event");
    if (!cJSON_IsObject(event))
        return fail(error, error_size, index, "event", "is required");
    if (!parse_topic(cJSON_GetObjectItemCaseSensitive(event, "topic"), binding, error, error_size, index))
        return false;
    if (!copy_string(event, "data_key", binding->data_key, sizeof(binding->data_key), true, error, error_size,
                     index)) {
        /* copy_string reports the field as "data_key"; name its full path. */
        char reason[96];
        const char *colon = strstr(error, ": ");
        snprintf(reason, sizeof(reason), "%s", colon ? colon + 2 : "is invalid");
        return fail(error, error_size, index, "event.data_key", reason);
    }
    if (!valid_identifier(binding->data_key))
        return fail(error, error_size, index, "event.data_key", "has invalid characters");
    if (!copy_string(event, "source_key", binding->source_key, sizeof(binding->source_key), false, error,
                     error_size, index) ||
        !copy_string(event, "source_value", binding->source_value, sizeof(binding->source_value), false, error,
                     error_size, index)) {
        char reason[96];
        const char *colon = strstr(error, ": ");
        const char *field = strstr(error, "].source_key") ? "event.source_key" : "event.source_value";
        snprintf(reason, sizeof(reason), "%s", colon ? colon + 2 : "is invalid");
        return fail(error, error_size, index, field, reason);
    }
    if (binding->source_key[0] && !valid_identifier(binding->source_key))
        return fail(error, error_size, index, "event.source_key", "has invalid characters");
    if (binding->source_value[0] && !binding->source_key[0])
        return fail(error, error_size, index, "event.source_key", "is required when source_value is set");
    return true;
}

/* Sparkplug reserves these names for the node's own controls. */
static bool reserved_name(const char *name) {
    return strcmp(name, "bdSeq") == 0 || strncmp(name, "Node Control/", 13) == 0 ||
           strncmp(name, "Device Control/", 15) == 0 || strncmp(name, "Properties/", 11) == 0;
}

static bool valid_metric_name(const char *name) {
    size_t length = strlen(name);
    if (!length || isspace((unsigned char)name[0]) || isspace((unsigned char)name[length - 1]))
        return false;
    for (const char *c = name; *c; c++) {
        if (iscntrl((unsigned char)*c) || *c == '+' || *c == '#')
            return false;
    }
    return true;
}

static bool parse_metric(const cJSON *json, MetricConfig *metric, char *error, size_t error_size, int index) {
    memset(metric, 0, sizeof(*metric));
    if (!cJSON_IsObject(json))
        return fail(error, error_size, index, "", "must be an object");

    if (!copy_string(json, "name", metric->name, sizeof(metric->name), true, error, error_size, index))
        return false;
    if (!valid_metric_name(metric->name))
        return fail(error, error_size, index, "name",
                    "must not start or end with a space or contain control characters, + or #");
    if (reserved_name(metric->name))
        return fail(error, error_size, index, "name", "is reserved by Sparkplug");

    const cJSON *datatype = cJSON_GetObjectItemCaseSensitive(json, "datatype");
    if (!cJSON_IsString(datatype) || !type_from_name(datatype->valuestring, &metric->type))
        return fail(error, error_size, index, "datatype", "must be Boolean, Int32, Int64, Float, Double or String");

    const cJSON *invert = cJSON_GetObjectItemCaseSensitive(json, "invert");
    if (invert && !cJSON_IsNull(invert) && !cJSON_IsBool(invert))
        return fail(error, error_size, index, "invert", "must be true or false");
    metric->invert = cJSON_IsTrue(invert);
    if (metric->invert && metric->type != MT_BOOLEAN)
        return fail(error, error_size, index, "invert", "applies only to Boolean metrics");

    const cJSON *writable = cJSON_GetObjectItemCaseSensitive(json, "writable");
    if (writable && !cJSON_IsNull(writable) && !cJSON_IsBool(writable))
        return fail(error, error_size, index, "writable", "must be true or false");
    metric->writable = cJSON_IsTrue(writable);

    const cJSON *event = cJSON_GetObjectItemCaseSensitive(json, "event");
    if (metric->writable) {
        if (event && !cJSON_IsNull(event))
            return fail(error, error_size, index, "event", "must be empty for a writable metric");
        if (metric->invert)
            return fail(error, error_size, index, "invert", "applies only to metrics bound to an event");
        return true;
    }
    return parse_binding(json, &metric->binding, error, error_size, index);
}

bool metrics_config_parse(const cJSON *json, MetricsConfig *out, char *error, size_t error_size) {
    memset(out, 0, sizeof(*out));
    const cJSON *metrics = cJSON_IsObject(json) ? cJSON_GetObjectItemCaseSensitive(json, "metrics") : NULL;
    if (!cJSON_IsArray(metrics))
        return fail(error, error_size, -1, "metrics", "must be an array");
    if (cJSON_GetArraySize(metrics) > MAX_METRICS)
        return fail(error, error_size, -1, "metrics", "has more than 128 entries");

    const cJSON *item;
    cJSON_ArrayForEach(item, metrics) {
        int index = out->count;
        MetricConfig *metric = &out->metrics[index];
        if (!parse_metric(item, metric, error, error_size, index))
            return false;
        for (int i = 0; i < index; i++) {
            if (strcmp(out->metrics[i].name, metric->name) == 0)
                return fail(error, error_size, index, "name", "is already used by another metric");
        }
        out->count++;
    }
    return true;
}

cJSON *metrics_config_to_json(const MetricsConfig *config) {
    cJSON *root = cJSON_CreateObject();
    cJSON *metrics = cJSON_AddArrayToObject(root, "metrics");

    for (int i = 0; i < config->count; i++) {
        const MetricConfig *metric = &config->metrics[i];
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "name", metric->name);
        cJSON_AddStringToObject(json, "datatype", metric_type_name(metric->type));
        cJSON_AddBoolToObject(json, "writable", metric->writable);
        if (metric->type == MT_BOOLEAN && !metric->writable)
            cJSON_AddBoolToObject(json, "invert", metric->invert);
        if (!metric->writable) {
            cJSON *event = cJSON_AddObjectToObject(json, "event");
            cJSON *topic = cJSON_AddArrayToObject(event, "topic");
            for (int t = 0; t < metric->binding.topic_count; t++) {
                char level[sizeof(metric->binding.topic[t].ns) + TOPIC_NAME_MAX_LEN + 2];
                snprintf(level, sizeof(level), "%s:%s", metric->binding.topic[t].ns, metric->binding.topic[t].name);
                cJSON_AddItemToArray(topic, cJSON_CreateString(level));
            }
            cJSON_AddStringToObject(event, "source_key", metric->binding.source_key);
            cJSON_AddStringToObject(event, "source_value", metric->binding.source_value);
            cJSON_AddStringToObject(event, "data_key", metric->binding.data_key);
        }
        cJSON_AddItemToArray(metrics, json);
    }
    return root;
}

void binding_topic_path(const EventBinding *binding, char *out, size_t out_size) {
    size_t used = 0;
    out[0] = '\0';
    for (int t = 0; t < binding->topic_count && used < out_size; t++) {
        int written = snprintf(out + used, out_size - used, "%s%s:%s", t ? "/" : "", binding->topic[t].ns,
                               binding->topic[t].name);
        if (written < 0)
            break;
        used += (size_t)written;
    }
}

void event_value_to_string(const EventValue *value, char *out, size_t out_size) {
    switch (value->type) {
    case EV_BOOL:
        snprintf(out, out_size, "%s", value->b ? "1" : "0");
        break;
    case EV_INT:
        snprintf(out, out_size, "%lld", value->i);
        break;
    case EV_DOUBLE:
        snprintf(out, out_size, "%g", value->d);
        break;
    default:
        snprintf(out, out_size, "%s", value->s);
        break;
    }
}

static bool parse_number(const char *text, double *out) {
    char *end = NULL;
    while (isspace((unsigned char)*text))
        text++;
    if (!*text)
        return false;
    double parsed = strtod(text, &end);
    while (end && isspace((unsigned char)*end))
        end++;
    if (!end || *end || !isfinite(parsed))
        return false;
    *out = parsed;
    return true;
}

static bool event_value_to_analog(const EventValue *value, double *out) {
    switch (value->type) {
    case EV_BOOL:
        *out = value->b ? 1.0 : 0.0;
        return true;
    case EV_INT:
        *out = (double)value->i;
        return true;
    case EV_DOUBLE:
        if (!isfinite(value->d))
            return false;
        *out = value->d;
        return true;
    default:
        return parse_number(value->s, out);
    }
}

static bool event_value_to_binary(const EventValue *value, bool invert, bool *active) {
    bool result = false;
    switch (value->type) {
    case EV_BOOL:
        result = value->b;
        break;
    case EV_INT:
        result = value->i != 0;
        break;
    case EV_DOUBLE:
        result = value->d != 0.0;
        break;
    default: {
        static const char *const on[] = {"1", "true", "yes", "on", "active", "high"};
        static const char *const off[] = {"0", "false", "no", "off", "inactive", "low"};
        bool matched = false;
        for (size_t i = 0; i < sizeof(on) / sizeof(on[0]) && !matched; i++) {
            if (strcasecmp(value->s, on[i]) == 0) {
                result = true;
                matched = true;
            } else if (strcasecmp(value->s, off[i]) == 0) {
                result = false;
                matched = true;
            }
        }
        if (!matched)
            return false;
        break;
    }
    }
    *active = invert ? !result : result;
    return true;
}

static bool number_to_text(const MetricConfig *metric, double number, char *out, size_t out_size) {
    switch (metric->type) {
    case MT_INT32:
    case MT_INT64: {
        double rounded = nearbyint(number);
        double min = metric->type == MT_INT32 ? (double)INT32_MIN : -9007199254740992.0;
        double max = metric->type == MT_INT32 ? (double)INT32_MAX : 9007199254740992.0;
        if (rounded < min || rounded > max)
            return false;
        snprintf(out, out_size, "%lld", (long long)rounded);
        return true;
    }
    case MT_FLOAT:
        snprintf(out, out_size, "%.9g", number);
        return true;
    default:
        snprintf(out, out_size, "%.17g", number);
        return true;
    }
}

bool metric_value_from_event(const MetricConfig *metric, const EventValue *value, char *out, size_t out_size) {
    double number;
    bool active;
    switch (metric->type) {
    case MT_BOOLEAN:
        if (!event_value_to_binary(value, metric->invert, &active))
            return false;
        snprintf(out, out_size, "%s", active ? "true" : "false");
        return true;
    case MT_STRING:
        if (value->type == EV_BOOL)
            snprintf(out, out_size, "%s", value->b ? "true" : "false");
        else
            event_value_to_string(value, out, out_size);
        return true;
    default:
        return event_value_to_analog(value, &number) && number_to_text(metric, number, out, out_size);
    }
}

bool metric_value_from_command(const MetricConfig *metric, const char *text, char *out, size_t out_size) {
    EventValue value = {.type = EV_STRING};
    if (!text || strlen(text) > METRIC_VALUE_MAX_LEN)
        return false;
    if (metric->type == MT_STRING) {
        snprintf(out, out_size, "%s", text);
        return true;
    }
    if (strlen(text) > VALUE_MAX_LEN)
        return false;
    snprintf(value.s, sizeof(value.s), "%s", text);
    MetricConfig plain = *metric;
    plain.invert = false;
    return metric_value_from_event(&plain, &value, out, out_size);
}

bool binding_aoa_scenario(const EventBinding *binding, int *scenario) {
    int device, id, end = 0;
    if (binding->topic_count != 3 || binding->source_key[0])
        return false;
    if (strcmp(binding->topic[0].name, "CameraApplicationPlatform") != 0 ||
        strcmp(binding->topic[1].name, "ObjectAnalytics") != 0)
        return false;
    if (sscanf(binding->topic[2].name, "Device%dScenario%d%n", &device, &id, &end) != 2 ||
        binding->topic[2].name[end] != '\0' || id < 1)
        return false;
    *scenario = id;
    return true;
}

bool aoa_occupancy_value(const cJSON *response, const char *key, EventValue *out) {
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(data, key);
    if (!cJSON_IsNumber(count) || count->valuedouble < 0 || count->valuedouble != floor(count->valuedouble))
        return false;
    memset(out, 0, sizeof(*out));
    out->type = EV_INT;
    out->i = (long long)count->valuedouble;
    return true;
}
