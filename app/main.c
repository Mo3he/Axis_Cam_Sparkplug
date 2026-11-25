/*
 * Sparkplug Edge Node ACAP entry binary.
 *
 * Settings and the metric list are JSON files in localdata, served on
 * 127.0.0.1:2214 behind the device's authenticated reverse proxy
 * (/local/Sparkplug_Edge/api/...). Event subscriptions and the API run in the
 * GLib main loop; the MQTT client runs on its own thread.
 * The loopback port must be unique per device; see PARAM_CGI_FALLBACK.md.
 */

#include <axsdk/axparameter.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>

#include "bindings.h"
#include "cJSON.h"
#include "certs.h"
#include "command_events.h"
#include "edge_node.h"
#include "metrics_config.h"
#include "mqtt_client.h"

#define APP_NAME "Sparkplug_Edge"
#define APP_DIR "/usr/local/packages/" APP_NAME
#define LOCALDATA APP_DIR "/localdata"
#define SETTINGS_FILE LOCALDATA "/settings.json"
#define METRICS_FILE LOCALDATA "/metrics.json"
#define BDSEQ_FILE LOCALDATA "/bdseq"
#define CERTS_DIR LOCALDATA "/certs"
#define TLS_PROFILE "broker"
#define SETTINGS_HTTP_PORT 2214
#define MAX_REQUEST (256 * 1024)
#define TEXT_MAX_LEN 127

typedef struct {
    bool enabled;
    char host[256];
    int port;
    MQTT_Server alternates[MQTT_MAX_SERVERS - 1];
    int alternate_count;
    char client_id[TEXT_MAX_LEN + 1];
    char username[TEXT_MAX_LEN + 1];
    char password[TEXT_MAX_LEN + 1];
    int keepalive;
    bool tls;
    bool tls_verify;
    char group_id[EDGE_ID_MAX_LEN + 1];
    char edge_node_id[EDGE_ID_MAX_LEN + 1];
    char device_id[EDGE_ID_MAX_LEN + 1];
    char primary_host_id[EDGE_ID_MAX_LEN + 1];
    SparkplugSpec spec;
} Settings;

static GMainLoop *main_loop;
static Settings settings;
static MetricsConfig active_metrics;
static char model[64];
static char serial[32];
static char app_version[32];

/* --------------------------------------------------------------- identity */

static void read_device_parameter(AXParameter *handle, const char *name, char *out, size_t out_size) {
    gchar *value = NULL;
    out[0] = '\0';
    if (handle && ax_parameter_get(handle, name, &value, NULL) && value)
        snprintf(out, out_size, "%s", value);
    g_free(value);
}

static void load_identity(void) {
    AXParameter *handle = ax_parameter_new(APP_NAME, NULL);
    read_device_parameter(handle, "root.Brand.ProdShortName", model, sizeof(model));
    read_device_parameter(handle, "root.Properties.System.SerialNumber", serial, sizeof(serial));
    if (handle)
        ax_parameter_free(handle);

    gchar *text = NULL;
    cJSON *manifest = NULL;
    if (g_file_get_contents(APP_DIR "/manifest.json", &text, NULL, NULL))
        manifest = cJSON_Parse(text);
    const cJSON *setup = cJSON_GetObjectItem(cJSON_GetObjectItem(manifest, "acapPackageConf"), "setup");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(setup, "version"));
    snprintf(app_version, sizeof(app_version), "%s", version ? version : "");
    cJSON_Delete(manifest);
    g_free(text);
}

/* Defaults derived from the serial number, so devices never collide. */
static const char *effective_edge_node_id(const Settings *s) {
    return s->edge_node_id[0] ? s->edge_node_id : serial;
}

static void effective_client_id(const Settings *s, char *out, size_t out_size) {
    if (s->client_id[0])
        snprintf(out, out_size, "%s", s->client_id);
    else
        snprintf(out, out_size, "%s-%s", APP_NAME, serial);
}

static int effective_port(const Settings *s) {
    return s->port ? s->port : (s->tls ? 8883 : 1883);
}

/* --------------------------------------------------------------- settings */

static void default_settings(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->keepalive = 60;
    s->tls_verify = true;
    s->spec = SPEC_3_0;
}

static bool fail(char *error, size_t error_size, const char *field, const char *reason) {
    snprintf(error, error_size, "%s: %s", field, reason);
    return false;
}

static bool read_text(const cJSON *json, const char *key, char *out, size_t out_size, char *error,
                      size_t error_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!item)
        return true; /* not given: keep */
    if (!cJSON_IsString(item))
        return fail(error, error_size, key, "must be a string");
    if (strlen(item->valuestring) >= out_size) {
        char reason[64];
        snprintf(reason, sizeof(reason), "is longer than %zu characters", out_size - 1);
        return fail(error, error_size, key, reason);
    }
    for (const char *c = item->valuestring; *c; c++) {
        if ((unsigned char)*c < 0x20)
            return fail(error, error_size, key, "must not contain control characters");
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static bool read_int(const cJSON *json, const char *key, int min, int max, int *out, char *error,
                     size_t error_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!item)
        return true;
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max ||
        item->valuedouble != (double)(int)item->valuedouble) {
        char reason[64];
        snprintf(reason, sizeof(reason), "must be a whole number from %d to %d", min, max);
        return fail(error, error_size, key, reason);
    }
    *out = (int)item->valuedouble;
    return true;
}

static bool read_bool(const cJSON *json, const char *key, bool *out, char *error, size_t error_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!item)
        return true;
    if (!cJSON_IsBool(item))
        return fail(error, error_size, key, "must be true or false");
    *out = cJSON_IsTrue(item);
    return true;
}

static bool valid_host(const char *host) {
    for (const char *c = host; *c; c++) {
        if (g_ascii_isspace(*c) || *c == '/' || *c == '@')
            return false;
    }
    return true;
}

/* [{host, port}]; port 0 means the primary broker's port. */
static bool parse_alternates(const cJSON *json, Settings *s, char *error, size_t error_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "alternates");
    if (!item)
        return true;
    if (!cJSON_IsArray(item))
        return fail(error, error_size, "alternates", "must be a list of {host, port}");
    if (cJSON_GetArraySize(item) > MQTT_MAX_SERVERS - 1)
        return fail(error, error_size, "alternates", "has more than 7 brokers");
    s->alternate_count = 0;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, item) {
        MQTT_Server *server = &s->alternates[s->alternate_count];
        memset(server, 0, sizeof(*server));
        if (!read_text(entry, "host", server->host, sizeof(server->host), error, error_size))
            return false;
        if (!server->host[0] || !valid_host(server->host))
            return fail(error, error_size, "alternates.host", "must be a host name or address");
        if (!read_int(entry, "port", 0, 65535, &server->port, error, error_size))
            return false;
        s->alternate_count++;
    }
    return true;
}

/* Applies a partial update on top of base. Validates everything. */
static bool settings_from_json(const cJSON *json, const Settings *base, Settings *out, char *error,
                               size_t error_size) {
    *out = *base;
    if (!cJSON_IsObject(json))
        return fail(error, error_size, "settings", "must be a JSON object");

    const cJSON *spec = cJSON_GetObjectItemCaseSensitive(json, "spec_version");
    if (spec) {
        if (cJSON_IsString(spec) && strcmp(spec->valuestring, "3.0") == 0)
            out->spec = SPEC_3_0;
        else if (cJSON_IsString(spec) && strcmp(spec->valuestring, "2.2") == 0)
            out->spec = SPEC_2_2;
        else
            return fail(error, error_size, "spec_version", "must be 3.0 or 2.2");
    }

    if (!read_bool(json, "enabled", &out->enabled, error, error_size) ||
        !read_text(json, "host", out->host, sizeof(out->host), error, error_size) ||
        !read_int(json, "port", 0, 65535, &out->port, error, error_size) ||
        !parse_alternates(json, out, error, error_size) ||
        !read_text(json, "client_id", out->client_id, sizeof(out->client_id), error, error_size) ||
        !read_text(json, "username", out->username, sizeof(out->username), error, error_size) ||
        !read_text(json, "password", out->password, sizeof(out->password), error, error_size) ||
        !read_int(json, "keepalive", 10, 3600, &out->keepalive, error, error_size) ||
        !read_bool(json, "tls", &out->tls, error, error_size) ||
        !read_bool(json, "tls_verify", &out->tls_verify, error, error_size) ||
        !read_text(json, "group_id", out->group_id, sizeof(out->group_id), error, error_size) ||
        !read_text(json, "edge_node_id", out->edge_node_id, sizeof(out->edge_node_id), error, error_size) ||
        !read_text(json, "device_id", out->device_id, sizeof(out->device_id), error, error_size) ||
        !read_text(json, "primary_host_id", out->primary_host_id, sizeof(out->primary_host_id), error, error_size))
        return false;

    if (!valid_host(out->host))
        return fail(error, error_size, "host", "must be a host name or address");
    const char *ids[][2] = {{"group_id", out->group_id},
                            {"edge_node_id", out->edge_node_id},
                            {"device_id", out->device_id},
                            {"primary_host_id", out->primary_host_id}};
    for (size_t i = 0; i < G_N_ELEMENTS(ids); i++) {
        if (ids[i][1][0] && !edge_node_valid_id(ids[i][1]))
            return fail(error, error_size, ids[i][0], "must not contain /, + or #");
    }
    if (out->enabled && !out->host[0])
        return fail(error, error_size, "host", "is required to enable the edge node");
    if (out->enabled && !out->group_id[0])
        return fail(error, error_size, "group_id", "is required to enable the edge node");
    if (out->enabled && !edge_node_valid_id(effective_edge_node_id(out)))
        return fail(error, error_size, "edge_node_id", "is required (the serial number is not available)");
    return true;
}

static cJSON *settings_to_json(const Settings *s, bool include_password) {
    cJSON *json = cJSON_CreateObject();
    char client_id[TEXT_MAX_LEN + 1];
    cJSON_AddBoolToObject(json, "enabled", s->enabled);
    cJSON_AddStringToObject(json, "host", s->host);
    cJSON_AddNumberToObject(json, "port", s->port);
    cJSON *alternates = cJSON_AddArrayToObject(json, "alternates");
    for (int i = 0; i < s->alternate_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "host", s->alternates[i].host);
        cJSON_AddNumberToObject(entry, "port", s->alternates[i].port);
        cJSON_AddItemToArray(alternates, entry);
    }
    cJSON_AddStringToObject(json, "client_id", s->client_id);
    cJSON_AddStringToObject(json, "username", s->username);
    if (include_password)
        cJSON_AddStringToObject(json, "password", s->password);
    else
        cJSON_AddBoolToObject(json, "password_set", s->password[0] != '\0');
    cJSON_AddNumberToObject(json, "keepalive", s->keepalive);
    cJSON_AddBoolToObject(json, "tls", s->tls);
    cJSON_AddBoolToObject(json, "tls_verify", s->tls_verify);
    cJSON_AddStringToObject(json, "group_id", s->group_id);
    cJSON_AddStringToObject(json, "edge_node_id", s->edge_node_id);
    cJSON_AddStringToObject(json, "device_id", s->device_id);
    cJSON_AddStringToObject(json, "primary_host_id", s->primary_host_id);
    cJSON_AddStringToObject(json, "spec_version", s->spec == SPEC_2_2 ? "2.2" : "3.0");
    if (!include_password) {
        cJSON *effective = cJSON_AddObjectToObject(json, "effective");
        effective_client_id(s, client_id, sizeof(client_id));
        cJSON_AddStringToObject(effective, "edge_node_id", effective_edge_node_id(s));
        cJSON_AddStringToObject(effective, "client_id", client_id);
        cJSON_AddNumberToObject(effective, "port", effective_port(s));
    }
    return json;
}

/* Written owner-only: the settings file holds the broker password. */
static bool write_private_file(const char *path, cJSON *json) {
    char *text = cJSON_Print(json);
    GError *error = NULL;
    bool ok = text && g_file_set_contents(path, text, -1, &error);
    if (ok)
        g_chmod(path, 0600);
    else
        syslog(LOG_ERR, "saving %s failed: %s", path, error ? error->message : "out of memory");
    g_clear_error(&error);
    cJSON_free(text);
    return ok;
}

static void load_settings(void) {
    gchar *text = NULL;
    char error[160];
    Settings defaults;
    default_settings(&defaults);
    settings = defaults;
    if (!g_file_get_contents(SETTINGS_FILE, &text, NULL, NULL))
        return;
    cJSON *json = cJSON_Parse(text);
    g_free(text);
    if (!settings_from_json(json, &defaults, &settings, error, sizeof(error))) {
        syslog(LOG_ERR, "%s is invalid and was ignored: %s", SETTINGS_FILE, error);
        settings = defaults;
    }
    cJSON_Delete(json);
}

static void load_metrics(void) {
    gchar *text = NULL;
    char error[160];
    active_metrics.count = 0;
    if (!g_file_get_contents(METRICS_FILE, &text, NULL, NULL))
        return;
    cJSON *json = cJSON_Parse(text);
    g_free(text);
    if (!metrics_config_parse(json, &active_metrics, error, sizeof(error))) {
        syslog(LOG_ERR, "%s is invalid and was ignored: %s", METRICS_FILE, error);
        active_metrics.count = 0;
    }
    cJSON_Delete(json);
}

/* ------------------------------------------------------------------ apply */

static void mqtt_config(MQTT_Config *config) {
    memset(config, 0, sizeof(*config));
    config->enabled = settings.enabled;
    snprintf(config->host, sizeof(config->host), "%s", settings.host);
    config->port = effective_port(&settings);
    if (settings.alternate_count) {
        snprintf(config->servers[0].host, sizeof(config->servers[0].host), "%s", settings.host);
        config->servers[0].port = config->port;
        for (int i = 0; i < settings.alternate_count; i++) {
            config->servers[i + 1] = settings.alternates[i];
            if (!config->servers[i + 1].port)
                config->servers[i + 1].port = config->port;
        }
        config->server_count = settings.alternate_count + 1;
    }
    effective_client_id(&settings, config->client_id, sizeof(config->client_id));
    snprintf(config->username, sizeof(config->username), "%s", settings.username);
    snprintf(config->password, sizeof(config->password), "%s", settings.password);
    config->keepalive = settings.keepalive;
    config->use_tls = settings.tls;
    config->tls_insecure = !settings.tls_verify;
    if (settings.tls && Certs_Exists(TLS_PROFILE))
        snprintf(config->tls_profile, sizeof(config->tls_profile), "%s", TLS_PROFILE);
}

static void edge_settings(EdgeNodeSettings *out) {
    memset(out, 0, sizeof(*out));
    out->enabled = settings.enabled;
    out->spec = settings.spec;
    snprintf(out->group_id, sizeof(out->group_id), "%s", settings.group_id);
    snprintf(out->edge_node_id, sizeof(out->edge_node_id), "%s", effective_edge_node_id(&settings));
    snprintf(out->device_id, sizeof(out->device_id), "%s", settings.device_id);
    snprintf(out->primary_host_id, sizeof(out->primary_host_id), "%s", settings.primary_host_id);
}

/* Edge node identity and broker; the metrics are unchanged. */
static void apply_settings(void) {
    EdgeNodeSettings edge;
    MQTT_Config config;
    edge_settings(&edge);
    mqtt_config(&config);
    edge_node_configure(&edge, &active_metrics);
    MQTT_Reconfigure(&config);
}

/* Bindings and write events point into active_metrics: the caller stops the
 * bindings before replacing it. */
static void apply_metrics(void) {
    EdgeNodeSettings edge;
    edge_settings(&edge);
    edge_node_configure(&edge, &active_metrics);
    command_events_declare(&active_metrics);
    bindings_start(&active_metrics);
}

static void on_command(int index, const char *value) {
    if (index >= 0 && index < active_metrics.count)
        syslog(LOG_INFO, "host wrote '%s' = %s", active_metrics.metrics[index].name, value);
    command_events_send(index, value);
}

/* -------------------------------------------------------------------- API */

static cJSON *status_json(void) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", settings.enabled);
    cJSON_AddStringToObject(json, "model", model);
    cJSON_AddStringToObject(json, "version", app_version);
    cJSON_AddItemToObject(json, "mqtt", MQTT_Status());
    cJSON_AddItemToObject(json, "sparkplug", edge_node_status());
    cJSON_AddItemToObject(json, "metrics", bindings_status());
    return json;
}

static bool apply_settings_body(const char *body, char *error, size_t error_size) {
    cJSON *json = cJSON_Parse(body ? body : "");
    Settings next;
    bool ok = settings_from_json(json, &settings, &next, error, error_size);
    cJSON_Delete(json);
    if (!ok)
        return false;
    cJSON *saved = settings_to_json(&next, true);
    ok = write_private_file(SETTINGS_FILE, saved);
    cJSON_Delete(saved);
    if (!ok) {
        snprintf(error, error_size, "could not save the settings");
        return false;
    }
    settings = next;
    apply_settings();
    return true;
}

static bool apply_metrics_body(const char *body, char *error, size_t error_size) {
    cJSON *json = cJSON_Parse(body ? body : "");
    if (!json) {
        snprintf(error, error_size, "body is not valid JSON");
        return false;
    }
    MetricsConfig *parsed = g_new0(MetricsConfig, 1);
    bool ok = metrics_config_parse(json, parsed, error, error_size);
    cJSON_Delete(json);
    if (ok) {
        cJSON *saved = metrics_config_to_json(parsed);
        ok = write_private_file(METRICS_FILE, saved);
        cJSON_Delete(saved);
        if (!ok)
            snprintf(error, error_size, "could not save the metrics");
    }
    if (ok) {
        bindings_stop();
        active_metrics = *parsed;
        apply_metrics();
    }
    g_free(parsed);
    return ok;
}

static cJSON *tls_json(void) {
    cJSON *profile = Certs_Get(TLS_PROFILE);
    return profile ? profile : cJSON_CreateObject();
}

static bool apply_tls_body(const char *body, char *error, size_t error_size) {
    cJSON *json = cJSON_Parse(body ? body : "");
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        snprintf(error, error_size, "body must be a JSON object");
        return false;
    }
    /* One profile; skipping verification is the tls_verify setting instead. */
    cJSON_DeleteItemFromObject(json, "name");
    cJSON_DeleteItemFromObject(json, "insecure");
    cJSON_AddStringToObject(json, "name", TLS_PROFILE);
    bool ok = Certs_Save(json, error, error_size);
    cJSON_Delete(json);
    if (ok) {
        apply_settings();
        MQTT_TLS_Profile_Changed(TLS_PROFILE);
    }
    return ok;
}

static void send_response(GOutputStream *out, const char *status, const char *content_type, const char *body) {
    gchar *header = g_strdup_printf("HTTP/1.1 %s\r\n"
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Cache-Control: no-store\r\n"
                                    "Connection: close\r\n"
                                    "\r\n",
                                    status, content_type, strlen(body));
    g_output_stream_write_all(out, header, strlen(header), NULL, NULL, NULL);
    g_output_stream_write_all(out, body, strlen(body), NULL, NULL, NULL);
    g_free(header);
}

static void send_json(GOutputStream *out, const char *status, cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    send_response(out, status, "application/json", text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(json);
}

static void send_ok(GOutputStream *out) {
    send_response(out, "200 OK", "application/json", "{\"status\":\"ok\"}");
}

static void send_error(GOutputStream *out, const char *status, const char *message) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    send_json(out, status, json);
}

/* "GET /local/Sparkplug_Edge/api/status?x HTTP/1.1" -> "status". The reverse
 * proxy forwards the full path, so take what follows the last "/api/". */
static gchar *request_route(const char *request_line) {
    const char *path = strchr(request_line, ' ');
    if (!path)
        return g_strdup("");
    path++;
    size_t length = strcspn(path, " ?");
    gchar *route = g_strndup(path, length);
    const char *start = g_strrstr(route, "/api/");
    if (start)
        start += strlen("/api/");
    else
        for (start = route; *start == '/'; start++)
            ;
    gchar *result = g_strdup(start);
    g_free(route);
    return result;
}

static void handle_request(GOutputStream *out, const char *request_line, const char *body) {
    gboolean is_get = g_str_has_prefix(request_line, "GET ");
    gboolean is_post = g_str_has_prefix(request_line, "POST ");
    gchar *route = request_route(request_line);
    char error[256] = "";

    if (is_get && g_strcmp0(route, "status") == 0) {
        send_json(out, "200 OK", status_json());
    } else if (is_get && g_strcmp0(route, "settings") == 0) {
        send_json(out, "200 OK", settings_to_json(&settings, false));
    } else if (is_post && g_strcmp0(route, "settings") == 0) {
        if (apply_settings_body(body, error, sizeof(error)))
            send_ok(out);
        else
            send_error(out, "400 Bad Request", error);
    } else if (is_get && g_strcmp0(route, "metrics") == 0) {
        send_json(out, "200 OK", metrics_config_to_json(&active_metrics));
    } else if (is_post && g_strcmp0(route, "metrics") == 0) {
        if (apply_metrics_body(body, error, sizeof(error)))
            send_ok(out);
        else
            send_error(out, "400 Bad Request", error);
    } else if (is_get && g_strcmp0(route, "tls") == 0) {
        send_json(out, "200 OK", tls_json());
    } else if (is_post && g_strcmp0(route, "tls") == 0) {
        if (apply_tls_body(body, error, sizeof(error)))
            send_ok(out);
        else
            send_error(out, "400 Bad Request", error);
    } else if (is_post && g_strcmp0(route, "tls/delete") == 0) {
        Certs_Delete(TLS_PROFILE);
        apply_settings();
        MQTT_TLS_Profile_Changed(TLS_PROFILE);
        send_ok(out);
    } else {
        syslog(LOG_WARNING, "API: no route for '%s'", request_line);
        send_error(out, "404 Not Found", "not found");
    }
    g_free(route);
}

static gboolean on_request(GSocketService *service, GSocketConnection *connection, GObject *source,
                           gpointer user_data) {
    (void)service;
    (void)source;
    (void)user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gchar *buffer = g_malloc(MAX_REQUEST);
    gsize total = 0;
    gsize header_length = 0;
    glong content_length = -1;

    while (total < MAX_REQUEST - 1) {
        gssize got = g_input_stream_read(in, buffer + total, MAX_REQUEST - 1 - total, NULL, NULL);
        if (got <= 0)
            break;
        total += (gsize)got;
        buffer[total] = '\0';
        gchar *header_end = strstr(buffer, "\r\n\r\n");
        if (!header_end)
            continue;
        header_length = (gsize)(header_end - buffer) + 4;
        if (content_length < 0) {
            gchar *headers = g_ascii_strdown(buffer, (gssize)header_length);
            gchar *field = strstr(headers, "content-length:");
            content_length = field ? strtol(field + strlen("content-length:"), NULL, 10) : 0;
            g_free(headers);
        }
        if (total - header_length >= (gsize)content_length)
            break;
    }
    buffer[total] = '\0';

    if (header_length == 0) {
        send_error(out, "400 Bad Request", "bad request");
    } else if (content_length > 0 && total - header_length < (gsize)content_length) {
        send_error(out, "413 Payload Too Large", "request too large");
    } else {
        gchar *line_end = strstr(buffer, "\r\n");
        gchar *request_line = g_strndup(buffer, (gsize)(line_end - buffer));
        handle_request(out, request_line, buffer + header_length);
        g_free(request_line);
    }

    g_free(buffer);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
}

static gboolean start_api_server(void) {
    GError *error = NULL;
    GSocketService *service = g_socket_service_new();
    GInetAddress *address = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *socket_address = g_inet_socket_address_new(address, SETTINGS_HTTP_PORT);
    gboolean added = g_socket_listener_add_address(G_SOCKET_LISTENER(service), socket_address,
                                                   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, NULL, &error);
    g_object_unref(socket_address);
    g_object_unref(address);

    if (!added) {
        /* Usually another ACAP claimed the port, which would silently send
         * this app's API to that app. */
        syslog(LOG_ERR, "API server cannot bind 127.0.0.1:%d: %s", SETTINGS_HTTP_PORT,
               error ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(service);
        return FALSE;
    }
    g_signal_connect(service, "incoming", G_CALLBACK(on_request), NULL);
    g_socket_service_start(service);
    return TRUE;
}

/* ------------------------------------------------------------------- main */

static gboolean on_terminate(gpointer user_data) {
    (void)user_data;
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    MQTT_Config config;
    EdgeNodeSettings edge;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    load_identity();
    g_mkdir_with_parents(LOCALDATA, 0700);
    Certs_Init(CERTS_DIR);
    load_settings();
    load_metrics();

    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_terminate, NULL);
    g_unix_signal_add(SIGINT, on_terminate, NULL);

    bindings_init();
    command_events_init();
    edge_node_init(BDSEQ_FILE);
    edge_node_set_command_callback(on_command);
    /* The edge node must know its identity before the first CONNECT. */
    edge_settings(&edge);
    edge_node_configure(&edge, &active_metrics);
    command_events_declare(&active_metrics);
    mqtt_config(&config);
    MQTT_Init(&config, edge_node_on_mqtt_message, NULL);
    bindings_start(&active_metrics);
    start_api_server();
    syslog(LOG_INFO, "%s %s started with %d metrics", APP_NAME, app_version, active_metrics.count);
    g_main_loop_run(main_loop);

    bindings_stop();
    edge_node_cleanup();
    MQTT_Cleanup();
    command_events_cleanup();
    bindings_cleanup();
    Certs_Cleanup();
    g_main_loop_unref(main_loop);
    closelog();
    return EXIT_SUCCESS;
}
