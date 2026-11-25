/* Host tests for app/metrics_config.c: parsing and validation of metric
 * declarations, JSON round trip, and conversion of event and command values. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics_config.h"

static int failures = 0;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

static const char *VALID =
    "{\"metrics\":["
    "{\"name\":\"Air/CO2\",\"datatype\":\"Int32\","
    "\"event\":{\"topic\":[\"tnsaxis:AirQualityMonitor\",\"tnsaxis:Metadata\"],\"data_key\":\"CO2\"}},"
    "{\"name\":\"Door closed\",\"datatype\":\"Boolean\",\"invert\":true,"
    "\"event\":{\"topic\":[\"tns1:Device\",\"tnsaxis:IO\",\"tnsaxis:VirtualInput\"],\"source_key\":\"port\","
    "\"source_value\":\"1\",\"data_key\":\"active\"}},"
    "{\"name\":\"Commands/Play\",\"datatype\":\"Boolean\",\"writable\":true}]}";

static bool parse_text(const char *text, MetricsConfig *config, char *error, size_t size) {
    cJSON *json = cJSON_Parse(text);
    bool ok = metrics_config_parse(json, config, error, size);
    cJSON_Delete(json);
    return ok;
}

static void test_valid_and_round_trip(void) {
    MetricsConfig *config = calloc(1, sizeof(*config));
    MetricsConfig *again = calloc(1, sizeof(*again));
    char error[160] = "";
    CHECK(parse_text(VALID, config, error, sizeof(error)), "valid config rejected: %s", error);
    CHECK(config->count == 3, "count %d", config->count);
    CHECK(config->metrics[0].type == MT_INT32, "datatype");
    CHECK(config->metrics[1].invert && config->metrics[1].binding.topic_count == 3, "invert and topic");
    CHECK(config->metrics[2].writable && config->metrics[2].binding.topic_count == 0, "writable metric");

    cJSON *json = metrics_config_to_json(config);
    CHECK(metrics_config_parse(json, again, error, sizeof(error)), "round trip rejected: %s", error);
    CHECK(memcmp(config, again, sizeof(*config)) == 0, "round trip changed the config");
    cJSON_Delete(json);
    free(config);
    free(again);
}

static void expect_error(const char *metric, const char *expected) {
    MetricsConfig *config = calloc(1, sizeof(*config));
    char text[1024];
    char error[160] = "";
    snprintf(text, sizeof(text), "{\"metrics\":[%s]}", metric);
    CHECK(!parse_text(text, config, error, sizeof(error)), "accepted: %s", metric);
    CHECK(strstr(error, expected) != NULL, "error '%s' lacks '%s'", error, expected);
    free(config);
}

static void test_rejections(void) {
    const char *event = "\"event\":{\"topic\":[\"tns1:Device\"],\"data_key\":\"state\"}";
    char metric[512];

    snprintf(metric, sizeof(metric), "{\"name\":\"bdSeq\",\"datatype\":\"Int64\",%s}", event);
    expect_error(metric, "metrics[0].name: is reserved");
    snprintf(metric, sizeof(metric), "{\"name\":\"Node Control/Reboot\",\"datatype\":\"Boolean\",%s}", event);
    expect_error(metric, "is reserved");
    snprintf(metric, sizeof(metric), "{\"name\":\"a/#\",\"datatype\":\"Boolean\",%s}", event);
    expect_error(metric, "metrics[0].name");
    snprintf(metric, sizeof(metric), "{\"name\":\" padded\",\"datatype\":\"Boolean\",%s}", event);
    expect_error(metric, "metrics[0].name");
    snprintf(metric, sizeof(metric), "{\"name\":\"x\",\"datatype\":\"UInt8\",%s}", event);
    expect_error(metric, "metrics[0].datatype");
    snprintf(metric, sizeof(metric), "{\"name\":\"x\",\"datatype\":\"Int32\",\"invert\":true,%s}", event);
    expect_error(metric, "metrics[0].invert: applies only to Boolean");
    snprintf(metric, sizeof(metric), "{\"name\":\"x\",\"datatype\":\"Boolean\",\"writable\":true,%s}", event);
    expect_error(metric, "metrics[0].event: must be empty");
    expect_error("{\"name\":\"x\",\"datatype\":\"Boolean\"}", "metrics[0].event: is required");
    snprintf(metric, sizeof(metric), "{\"name\":\"x\",\"datatype\":\"Boolean\",%s},{\"name\":\"x\",\"datatype\":\"Int32\",%s}",
             event, event);
    expect_error(metric, "metrics[1].name: is already used");
}

static EventValue string_value(const char *text) {
    EventValue value = {.type = EV_STRING};
    snprintf(value.s, sizeof(value.s), "%s", text);
    return value;
}

static void test_conversions(void) {
    MetricConfig metric = {.type = MT_BOOLEAN};
    char out[64];
    EventValue b = {.type = EV_BOOL, .b = true};
    EventValue i = {.type = EV_INT, .i = 524};
    EventValue d = {.type = EV_DOUBLE, .d = 20.65};
    EventValue big = {.type = EV_DOUBLE, .d = 3e9};

    CHECK(metric_value_from_event(&metric, &b, out, sizeof(out)) && strcmp(out, "true") == 0, "bool: %s", out);
    metric.invert = true;
    CHECK(metric_value_from_event(&metric, &b, out, sizeof(out)) && strcmp(out, "false") == 0, "invert: %s", out);
    EventValue s = string_value("maybe");
    CHECK(!metric_value_from_event(&metric, &s, out, sizeof(out)), "unknown text as Boolean accepted");

    metric = (MetricConfig){.type = MT_INT32};
    CHECK(metric_value_from_event(&metric, &i, out, sizeof(out)) && strcmp(out, "524") == 0, "int: %s", out);
    CHECK(metric_value_from_event(&metric, &d, out, sizeof(out)) && strcmp(out, "21") == 0, "rounded: %s", out);
    CHECK(!metric_value_from_event(&metric, &big, out, sizeof(out)), "Int32 overflow accepted");
    s = string_value("abc");
    CHECK(!metric_value_from_event(&metric, &s, out, sizeof(out)), "text as Int32 accepted");
    metric.type = MT_INT64;
    CHECK(metric_value_from_event(&metric, &big, out, sizeof(out)) && strcmp(out, "3000000000") == 0, "Int64: %s",
          out);

    metric.type = MT_DOUBLE;
    CHECK(metric_value_from_event(&metric, &d, out, sizeof(out)) && strtod(out, NULL) == 20.65, "double: %s", out);
    metric.type = MT_FLOAT;
    CHECK(metric_value_from_event(&metric, &d, out, sizeof(out)) && strcmp(out, "20.65") == 0, "float: %s", out);
    metric.type = MT_STRING;
    CHECK(metric_value_from_event(&metric, &b, out, sizeof(out)) && strcmp(out, "true") == 0, "bool as text: %s",
          out);

    metric = (MetricConfig){.type = MT_BOOLEAN, .writable = true};
    CHECK(metric_value_from_command(&metric, "1", out, sizeof(out)) && strcmp(out, "true") == 0, "cmd bool: %s",
          out);
    CHECK(!metric_value_from_command(&metric, "please", out, sizeof(out)), "bad command bool accepted");
    metric.type = MT_INT32;
    CHECK(!metric_value_from_command(&metric, "12abc", out, sizeof(out)), "bad command int accepted");
    CHECK(metric_value_from_command(&metric, "-7", out, sizeof(out)) && strcmp(out, "-7") == 0, "cmd int: %s", out);
    metric.type = MT_STRING;
    CHECK(metric_value_from_command(&metric, "hello world", out, sizeof(out)) && strcmp(out, "hello world") == 0,
          "cmd text: %s", out);
}

static EventBinding aoa_binding(const char *level0, const char *level2) {
    EventBinding binding = {.topic_count = 3};
    snprintf(binding.topic[0].name, sizeof(binding.topic[0].name), "%s", level0);
    snprintf(binding.topic[1].name, sizeof(binding.topic[1].name), "ObjectAnalytics");
    snprintf(binding.topic[2].name, sizeof(binding.topic[2].name), "%s", level2);
    snprintf(binding.data_key, sizeof(binding.data_key), "human");
    return binding;
}

static void test_aoa_occupancy(void) {
    int scenario = 0;
    EventBinding binding = aoa_binding("CameraApplicationPlatform", "Device1Scenario2");
    CHECK(binding_aoa_scenario(&binding, &scenario) && scenario == 2, "AOA scenario not recognised");
    binding = aoa_binding("Device", "Device1Scenario2");
    CHECK(!binding_aoa_scenario(&binding, &scenario), "non-AOA topic accepted");

    cJSON *ok = cJSON_Parse("{\"data\":{\"human\":3,\"total\":4}}");
    EventValue value;
    CHECK(aoa_occupancy_value(ok, "human", &value) && value.type == EV_INT && value.i == 3, "human count");
    CHECK(!aoa_occupancy_value(ok, "car", &value), "missing class accepted");
    cJSON_Delete(ok);
}

int main(void) {
    test_valid_and_round_trip();
    test_rejections();
    test_conversions();
    test_aoa_occupancy();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("metrics_config: all tests passed\n");
    return 0;
}
