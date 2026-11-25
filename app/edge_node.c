#include "edge_node.h"

#include <glib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>

#include "mqtt_client.h"
#include "sparkplug_pb.h"

#define LOG(fmt, args...) syslog(LOG_INFO, "sparkplug: " fmt, ##args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "sparkplug: " fmt, ##args)

#define SPB_NAMESPACE "spBv1.0"
#define SPB_HISTORY_MAX 256
#define SPB_VALUE_LEN (METRIC_VALUE_MAX_LEN + 1)

/* Reserved by the spec; never published as user metrics */
#define SPB_METRIC_BDSEQ "bdSeq"
#define SPB_METRIC_REBIRTH "Node Control/Rebirth"

/* spBv1.0/<group>/DCMD/<node>/<device> */
#define SPB_TOPIC_LEN 320

typedef struct {
    MetricConfig config;
    int datatype;
    uint64_t alias;
    char value[SPB_VALUE_LEN];
    int has_value;
    int pending; /* changed, not yet published */
    uint64_t value_ts;
} MetricDef;

typedef struct {
    int def_idx;
    char value[SPB_VALUE_LEN];
    uint64_t ts;
} Sample;

static pthread_mutex_t spb_lock = PTHREAD_MUTEX_INITIALIZER;

static EdgeNodeSettings settings;
static MetricDef defs[MAX_METRICS];
static int def_count = 0;

/* Ring buffer of samples collected while the broker was unreachable */
static Sample history[SPB_HISTORY_MAX];
static int hist_head = 0;
static int hist_count = 0;

static char bdseq_path[256];
static uint64_t bdseq = 0;
/* Identity the current session's will was registered for (at PRECONNECT). */
static EdgeNodeSettings session;
static uint8_t seq = 0;
static int birth_sent = 0;
static int initialized = 0;
static uint64_t last_birth_ms = 0;
static uint64_t data_published = 0;
static uint64_t commands_received = 0;

/* When a primary host is configured, its STATE tells us whether anyone is
 * listening. Data collected while it is offline is buffered instead of being
 * published into the void, then replayed when it returns. */
static int host_online = 1;

static char cmd_filter_node[SPB_TOPIC_LEN] = "";
static char cmd_filter_dev[SPB_TOPIC_LEN] = "";
static char state_filter[SPB_TOPIC_LEN] = "";
static char cmd_topic_node[SPB_TOPIC_LEN] = "";
static char cmd_topic_dev[SPB_TOPIC_LEN] = "";

static EdgeNodeCommandFn command_fn = NULL;

/* =====================================================
 * Helpers
 * ===================================================== */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

bool edge_node_valid_id(const char *s) {
    if (!s || !s[0] || strlen(s) > EDGE_ID_MAX_LEN)
        return false;
    for (; *s; s++)
        if (*s == '/' || *s == '+' || *s == '#' || (unsigned char)*s < 0x20)
            return false;
    return true;
}

static int node_ready(void) {
    return settings.enabled && edge_node_valid_id(settings.group_id) && edge_node_valid_id(settings.edge_node_id);
}

static void build_topic(char *out, size_t n, const char *msgtype) {
    if (settings.device_id[0] && msgtype[0] == 'D')
        snprintf(out, n, SPB_NAMESPACE "/%s/%s/%s/%s", settings.group_id, msgtype, settings.edge_node_id,
                 settings.device_id);
    else
        snprintf(out, n, SPB_NAMESPACE "/%s/%s/%s", settings.group_id, msgtype, settings.edge_node_id);
}

static int datatype_of(MetricType type) {
    switch (type) {
    case MT_BOOLEAN:
        return SPB_DT_BOOLEAN;
    case MT_INT32:
        return SPB_DT_INT32;
    case MT_INT64:
        return SPB_DT_INT64;
    case MT_FLOAT:
        return SPB_DT_FLOAT;
    case MT_DOUBLE:
        return SPB_DT_DOUBLE;
    default:
        return SPB_DT_STRING;
    }
}

/* =====================================================
 * Publishing
 * ===================================================== */
static int publish_payload(const char *msgtype, SPB_Metric *metrics, int count, int has_seq, uint64_t use_seq,
                           int qos) {
    char topic[SPB_TOPIC_LEN];
    build_topic(topic, sizeof(topic), msgtype);

    SPB_Buf buf;
    if (!SPB_Encode(&buf, now_ms(), has_seq, use_seq, metrics, count)) {
        LOG_WARN("failed to encode %s payload", msgtype);
        return 0;
    }
    int ok = MQTT_Publish_Binary(topic, buf.data, buf.len, 0, qos);
    SPB_Buf_Free(&buf);
    if (!ok)
        LOG_WARN("failed to publish %s", msgtype);
    return ok;
}

static uint64_t take_seq(void) {
    uint64_t s = seq;
    seq = (uint8_t)((seq + 1) & 0xFF);
    return s;
}

/* bdSeq must differ between MQTT sessions, including across app restarts, or a
 * late NDEATH from a previous session would look like it kills the current one. */
static void load_bdseq(void) {
    gchar *text = NULL;
    if (g_file_get_contents(bdseq_path, &text, NULL, NULL))
        bdseq = g_ascii_strtoull(text, NULL, 10) & 0xFF;
    g_free(text);
}

static void save_bdseq(void) {
    char text[32];
    snprintf(text, sizeof(text), "%llu\n", (unsigned long long)bdseq);
    if (!g_file_set_contents(bdseq_path, text, -1, NULL))
        LOG_WARN("could not save bdSeq to %s", bdseq_path);
}

/* NDEATH carries only the bdSeq that identifies the session it kills. */
static int register_will(void) {
    SPB_Metric m;
    memset(&m, 0, sizeof(m));
    m.name = SPB_METRIC_BDSEQ;
    m.datatype = SPB_DT_UINT64;
    m.i64 = (int64_t)bdseq;
    m.timestamp_ms = now_ms();

    SPB_Buf buf;
    if (!SPB_Encode(&buf, m.timestamp_ms, 0, 0, &m, 1))
        return 0;

    char topic[SPB_TOPIC_LEN];
    build_topic(topic, sizeof(topic), "NDEATH");
    int ok = MQTT_Set_Will(topic, buf.data, buf.len, 1, 0);
    SPB_Buf_Free(&buf);
    return ok;
}

static void fill_birth_metric(SPB_Metric *m, const MetricDef *def, uint64_t ts) {
    m->name = def->config.name;
    m->alias = def->alias;
    m->has_alias = 1;
    m->datatype = def->datatype;
    m->timestamp_ms = def->has_value ? def->value_ts : ts;
    if (def->has_value)
        SPB_Metric_Set_Value(m, def->value);
    else
        m->is_null = 1;
}

/* Caller must hold spb_lock. */
static void publish_birth(void) {
    if (!node_ready())
        return;

    static SPB_Metric m[MAX_METRICS + 2];
    memset(m, 0, sizeof(m));
    uint64_t ts = now_ms();
    int n = 0;

    m[n].name = SPB_METRIC_BDSEQ;
    m[n].datatype = SPB_DT_UINT64;
    m[n].i64 = (int64_t)bdseq;
    m[n].timestamp_ms = ts;
    n++;

    m[n].name = SPB_METRIC_REBIRTH;
    m[n].datatype = SPB_DT_BOOLEAN;
    m[n].i64 = 0;
    m[n].timestamp_ms = ts;
    n++;

    /* When a device id is set the metrics live under the device, so NBIRTH
     * declares only the node controls and DBIRTH carries the metric list. */
    int node_carries_metrics = settings.device_id[0] ? 0 : 1;
    if (node_carries_metrics)
        for (int i = 0; i < def_count; i++)
            fill_birth_metric(&m[n++], &defs[i], ts);

    seq = 0;
    if (!publish_payload("NBIRTH", m, n, 1, take_seq(), 0))
        return;

    if (!node_carries_metrics) {
        memset(m, 0, sizeof(m));
        for (int i = 0; i < def_count; i++)
            fill_birth_metric(&m[i], &defs[i], ts);
        if (!publish_payload("DBIRTH", m, def_count, 1, take_seq(), 0))
            return;
    }

    birth_sent = 1;
    last_birth_ms = ts;
    LOG("birth published (bdSeq=%llu, %d metrics)", (unsigned long long)bdseq, def_count);
}

/* Caller must hold spb_lock. Replays samples captured while offline. */
static void flush_history(void) {
    if (!hist_count)
        return;

    static SPB_Metric m[SPB_HISTORY_MAX];
    memset(m, 0, sizeof(m));
    int n = 0;

    for (int k = 0; k < hist_count && n < SPB_HISTORY_MAX; k++) {
        Sample *s = &history[(hist_head + k) % SPB_HISTORY_MAX];
        if (s->def_idx < 0 || s->def_idx >= def_count)
            continue;
        m[n].alias = defs[s->def_idx].alias;
        m[n].has_alias = 1;
        m[n].datatype = defs[s->def_idx].datatype;
        m[n].timestamp_ms = s->ts;
        m[n].is_historical = 1;
        SPB_Metric_Set_Value(&m[n], s->value);
        n++;
    }

    if (!n) {
        hist_head = hist_count = 0;
        return;
    }

    /* Only drop the buffer once the replay is actually on the wire, otherwise a
     * send failure would silently destroy the very data we buffered. */
    if (!publish_payload(settings.device_id[0] ? "DDATA" : "NDATA", m, n, 1, take_seq(), 0)) {
        LOG_WARN("replay of %d buffered samples failed, keeping them buffered", n);
        return;
    }
    hist_head = hist_count = 0;
    LOG("replayed %d buffered metric samples", n);
}

/* Caller must hold spb_lock. */
static void buffer_sample(int def_idx, const char *value, uint64_t ts) {
    int slot;
    if (hist_count < SPB_HISTORY_MAX) {
        slot = (hist_head + hist_count) % SPB_HISTORY_MAX;
        hist_count++;
    } else {
        slot = hist_head; /* full: drop the oldest */
        hist_head = (hist_head + 1) % SPB_HISTORY_MAX;
    }
    history[slot].def_idx = def_idx;
    history[slot].ts = ts;
    snprintf(history[slot].value, sizeof(history[slot].value), "%s", value);
}

static int can_publish(void) {
    return birth_sent && host_online && MQTT_Is_Connected();
}

static guint flush_source = 0;

/* Changes from one camera event arrive as one callback per metric. Publish
 * everything that changed in one DATA message once they have all run. */
static gboolean flush_pending(gpointer user_data) {
    (void)user_data;
    static SPB_Metric m[MAX_METRICS];
    int n = 0;

    pthread_mutex_lock(&spb_lock);
    flush_source = 0;
    int publish = node_ready() && can_publish();
    memset(m, 0, sizeof(m));
    for (int i = 0; i < def_count; i++) {
        if (!defs[i].pending)
            continue;
        defs[i].pending = 0;
        if (!publish) {
            buffer_sample(i, defs[i].value, defs[i].value_ts);
            continue;
        }
        m[n].alias = defs[i].alias;
        m[n].has_alias = 1;
        m[n].datatype = defs[i].datatype;
        m[n].timestamp_ms = defs[i].value_ts;
        SPB_Metric_Set_Value(&m[n], defs[i].value);
        n++;
    }
    if (n && publish_payload(settings.device_id[0] ? "DDATA" : "NDATA", m, n, 1, take_seq(), 0))
        data_published++;
    pthread_mutex_unlock(&spb_lock);
    return G_SOURCE_REMOVE;
}

/* Caller must hold spb_lock. Records a new value and queues it for
 * publishing, or buffers it for replay when the session is not up. */
static void update_value(int i, const char *text, int force) {
    if (!force && defs[i].has_value && strcmp(defs[i].value, text) == 0)
        return;

    snprintf(defs[i].value, sizeof(defs[i].value), "%s", text);
    defs[i].has_value = 1;
    defs[i].value_ts = now_ms();
    if (!node_ready())
        return;

    if (!can_publish()) {
        buffer_sample(i, text, defs[i].value_ts);
        return;
    }
    defs[i].pending = 1;
    if (!flush_source)
        flush_source = g_idle_add(flush_pending, NULL);
}

/* =====================================================
 * Subscriptions
 * ===================================================== */
/* True when topic is exactly prefix, or a deeper level below it. */
static int topic_matches(const char *topic, const char *prefix) {
    if (!prefix[0])
        return 0;
    size_t n = strlen(prefix);
    if (strncmp(topic, prefix, n) != 0)
        return 0;
    return topic[n] == '\0' || topic[n] == '/';
}

/* Caller must hold spb_lock. */
static void update_command_subscriptions(void) {
    char node[SPB_TOPIC_LEN], dev[SPB_TOPIC_LEN], state[SPB_TOPIC_LEN];
    node[0] = dev[0] = state[0] = '\0';
    cmd_topic_node[0] = cmd_topic_dev[0] = '\0';

    if (node_ready()) {
        snprintf(cmd_topic_node, sizeof(cmd_topic_node), SPB_NAMESPACE "/%s/NCMD/%s", settings.group_id,
                 settings.edge_node_id);
        snprintf(node, sizeof(node), "%s/#", cmd_topic_node);
        if (settings.device_id[0]) {
            snprintf(cmd_topic_dev, sizeof(cmd_topic_dev), SPB_NAMESPACE "/%s/DCMD/%s/%s", settings.group_id,
                     settings.edge_node_id, settings.device_id);
            snprintf(dev, sizeof(dev), "%s", cmd_topic_dev);
        }
        if (settings.primary_host_id[0]) {
            /* Sparkplug 3.0 moved STATE under the spBv1.0 namespace */
            if (settings.spec == SPEC_3_0)
                snprintf(state, sizeof(state), SPB_NAMESPACE "/STATE/%s", settings.primary_host_id);
            else
                snprintf(state, sizeof(state), "STATE/%s", settings.primary_host_id);
        }
    }

    struct {
        char *cur;
        size_t cur_sz;
        const char *next;
    } subs[] = {
        {cmd_filter_node, sizeof(cmd_filter_node), node},
        {cmd_filter_dev, sizeof(cmd_filter_dev), dev},
        {state_filter, sizeof(state_filter), state},
    };

    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        if (strcmp(subs[i].cur, subs[i].next) == 0)
            continue;
        if (subs[i].cur[0])
            MQTT_Unsubscribe(subs[i].cur);
        snprintf(subs[i].cur, subs[i].cur_sz, "%s", subs[i].next);
        if (subs[i].cur[0])
            MQTT_Subscribe(subs[i].cur);
    }
}

/* =====================================================
 * MQTT lifecycle (called on the MQTT worker thread)
 * ===================================================== */
static void mqtt_state_cb(int state, void *user_data) {
    (void)user_data;

    if (state == MQTT_STATE_PRECONNECT) {
        pthread_mutex_lock(&spb_lock);
        /* The previous session's view of the host does not carry over: a new
         * broker may hold no STATE at all, and staying OFFLINE would mute the
         * node forever. A retained OFFLINE will arrive right after SUBSCRIBE. */
        host_online = 1;
        if (node_ready()) {
            /* Sparkplug 3.0: 0 to 255, wrapping to 0. */
            bdseq = (bdseq + 1) & 0xFF;
            save_bdseq();
            birth_sent = 0;
            session = settings;
            register_will();
        } else {
            MQTT_Set_Will(NULL, NULL, 0, 0, 0);
        }
        pthread_mutex_unlock(&spb_lock);
        return;
    }

    if (state == MQTT_STATE_CONNECTED) {
        pthread_mutex_lock(&spb_lock);
        /* The identity changed during the handshake: the broker holds a will
         * for the old one, so start a session that matches instead. */
        if (node_ready() && (strcmp(session.group_id, settings.group_id) != 0 ||
                             strcmp(session.edge_node_id, settings.edge_node_id) != 0 ||
                             strcmp(session.device_id, settings.device_id) != 0)) {
            pthread_mutex_unlock(&spb_lock);
            LOG("identity changed while connecting, reconnecting");
            MQTT_Force_Reconnect();
            return;
        }
        if (node_ready()) {
            publish_birth();
            if (birth_sent)
                flush_history();
        }
        pthread_mutex_unlock(&spb_lock);
        return;
    }

    if (state == MQTT_STATE_DISCONNECTED) {
        pthread_mutex_lock(&spb_lock);
        birth_sent = 0;
        pthread_mutex_unlock(&spb_lock);
    }
}

/* =====================================================
 * Inbound commands (called on the main loop)
 * ===================================================== */
typedef struct {
    int rebirth;
    int index[MAX_METRICS];
    char vals[MAX_METRICS][SPB_VALUE_LEN];
    int cmd_count;
} CommandBatch;

static void command_metric_handler(const char *name, uint64_t alias, int has_alias, int datatype,
                                   const char *value_str, void *user) {
    (void)datatype;
    CommandBatch *batch = user;

    if (name && (strcmp(name, SPB_METRIC_REBIRTH) == 0 || strcmp(name, "Device Control/Rebirth") == 0)) {
        if (value_str && strcasecmp(value_str, "true") == 0)
            batch->rebirth = 1;
        return;
    }

    /* Hosts may address a metric by alias instead of name */
    int found = -1;
    for (int i = 0; i < def_count && found < 0; i++) {
        if ((name && name[0]) ? strcmp(defs[i].config.name, name) == 0 : (has_alias && defs[i].alias == alias))
            found = i;
    }
    if (found < 0) {
        LOG_WARN("write to unknown metric '%s' ignored", name && name[0] ? name : "(alias)");
        return;
    }
    if (!defs[found].config.writable) {
        LOG_WARN("write to read-only metric '%s' ignored", defs[found].config.name);
        return;
    }
    char value[SPB_VALUE_LEN];
    if (!metric_value_from_command(&defs[found].config, value_str ? value_str : "", value, sizeof(value))) {
        LOG_WARN("write to '%s' ignored: '%s' is not a %s value", defs[found].config.name, value_str ? value_str : "",
                 metric_type_name(defs[found].config.type));
        return;
    }
    if (batch->cmd_count >= MAX_METRICS)
        return;
    batch->index[batch->cmd_count] = found;
    snprintf(batch->vals[batch->cmd_count], SPB_VALUE_LEN, "%s", value);
    batch->cmd_count++;
}

static void handle_state(const char *payload, int payload_len) {
    /* 2.2 sends the bare word ONLINE/OFFLINE, 3.0 sends {"online":true,...} */
    char text[256];
    int n = payload_len < (int)sizeof(text) - 1 ? payload_len : (int)sizeof(text) - 1;
    memcpy(text, payload ? payload : "", n > 0 ? (size_t)n : 0);
    text[n > 0 ? n : 0] = '\0';
    int online = strstr(text, "\"online\":true") || strstr(text, "\"online\": true") ||
                 (strstr(text, "ONLINE") && !strstr(text, "OFFLINE"));

    pthread_mutex_lock(&spb_lock);
    int was_online = host_online;
    host_online = online;
    if (online && !was_online) {
        LOG("primary host '%s' online, rebirthing", settings.primary_host_id);
        publish_birth();
        if (birth_sent)
            flush_history();
    } else if (online) {
        publish_birth();
    } else if (was_online) {
        LOG("primary host '%s' offline, buffering data until it returns", settings.primary_host_id);
    }
    pthread_mutex_unlock(&spb_lock);
}

void edge_node_on_mqtt_message(const char *topic, const char *payload, int payload_len, void *user_data) {
    (void)user_data;
    if (!topic)
        return;

    pthread_mutex_lock(&spb_lock);
    int ready = node_ready();
    int is_cmd = topic_matches(topic, cmd_topic_node) || topic_matches(topic, cmd_topic_dev);
    int is_state = state_filter[0] && strcmp(topic, state_filter) == 0;
    pthread_mutex_unlock(&spb_lock);
    if (!ready)
        return;

    if (is_state) {
        handle_state(payload, payload_len);
        return;
    }
    if (!is_cmd || !payload || payload_len <= 0)
        return;

    static CommandBatch batch;
    memset(&batch, 0, sizeof(batch));

    pthread_mutex_lock(&spb_lock);
    if (SPB_Decode((const uint8_t *)payload, payload_len, command_metric_handler, &batch) < 0) {
        LOG_WARN("malformed command payload on %s", topic);
        pthread_mutex_unlock(&spb_lock);
        return;
    }
    if (batch.rebirth) {
        LOG("rebirth requested by host");
        publish_birth();
    }
    /* Report the written value back, as Sparkplug hosts expect. */
    for (int i = 0; i < batch.cmd_count; i++) {
        commands_received++;
        update_value(batch.index[i], batch.vals[i], 1);
    }
    pthread_mutex_unlock(&spb_lock);

    if (command_fn)
        for (int i = 0; i < batch.cmd_count; i++)
            command_fn(batch.index[i], batch.vals[i]);
}

/* =====================================================
 * Values and configuration (called on the main loop)
 * ===================================================== */
void edge_node_set_value(int index, const char *text) {
    pthread_mutex_lock(&spb_lock);
    if (index >= 0 && index < def_count && text)
        update_value(index, text, 0);
    pthread_mutex_unlock(&spb_lock);
}

void edge_node_configure(const EdgeNodeSettings *next, const MetricsConfig *metrics) {
    static MetricDef previous[MAX_METRICS];

    pthread_mutex_lock(&spb_lock);
    int identity_changed = next->enabled != settings.enabled || strcmp(next->group_id, settings.group_id) != 0 ||
                           strcmp(next->edge_node_id, settings.edge_node_id) != 0 ||
                           strcmp(next->device_id, settings.device_id) != 0 ||
                           strcmp(next->primary_host_id, settings.primary_host_id) != 0 || next->spec != settings.spec;
    int list_changed = metrics->count != def_count;
    for (int i = 0; !list_changed && i < def_count; i++)
        list_changed = memcmp(&metrics->metrics[i], &defs[i].config, sizeof(MetricConfig)) != 0;

    /* A different (or no) primary host means the old host's ONLINE/OFFLINE
     * state no longer applies. Assume online until the new host says otherwise,
     * so telemetry is never suppressed by a host that never reports. */
    if (!next->primary_host_id[0] || strcmp(next->primary_host_id, settings.primary_host_id) != 0)
        host_online = 1;
    /* Nothing may be published under the new identity before its birth. */
    if (identity_changed)
        birth_sent = 0;
    settings = *next;

    /* Aliases are positional and stable for the life of a session; any change
     * to the list forces a new birth. Values carry over by name and type. */
    int previous_count = def_count;
    memcpy(previous, defs, sizeof(MetricDef) * (size_t)previous_count);
    memset(defs, 0, sizeof(defs));
    def_count = metrics->count;
    for (int i = 0; i < def_count; i++) {
        defs[i].config = metrics->metrics[i];
        defs[i].datatype = datatype_of(defs[i].config.type);
        defs[i].alias = (uint64_t)i + 1; /* alias 0 is reserved */
        for (int p = 0; p < previous_count; p++) {
            if (previous[p].has_value && previous[p].config.type == defs[i].config.type &&
                strcmp(previous[p].config.name, defs[i].config.name) == 0) {
                snprintf(defs[i].value, sizeof(defs[i].value), "%s", previous[p].value);
                defs[i].has_value = 1;
                defs[i].value_ts = previous[p].value_ts;
                break;
            }
        }
    }
    if (list_changed) {
        /* Buffered samples refer to the old aliases. */
        hist_head = hist_count = 0;
    }
    update_command_subscriptions();
    pthread_mutex_unlock(&spb_lock);

    if (!settings.enabled) {
        MQTT_Set_Will(NULL, NULL, 0, 0, 0);
        pthread_mutex_lock(&spb_lock);
        birth_sent = 0;
        pthread_mutex_unlock(&spb_lock);
        return;
    }

    /* A new identity also needs a new will, which only a reconnect registers.
     * A new metric list only needs a new birth. */
    if (identity_changed && MQTT_Is_Connected()) {
        LOG("identity changed, reconnecting to rebirth");
        MQTT_Force_Reconnect();
    } else if (list_changed && MQTT_Is_Connected()) {
        pthread_mutex_lock(&spb_lock);
        publish_birth();
        pthread_mutex_unlock(&spb_lock);
    }
}

/* =====================================================
 * Lifecycle and status
 * ===================================================== */
void edge_node_init(const char *bdseq_file) {
    if (initialized)
        return;
    snprintf(bdseq_path, sizeof(bdseq_path), "%s", bdseq_file);
    load_bdseq();
    MQTT_Set_State_Callback(mqtt_state_cb, NULL);
    initialized = 1;
}

void edge_node_cleanup(void) {
    if (!initialized)
        return;
    MQTT_Set_State_Callback(NULL, NULL);

    pthread_mutex_lock(&spb_lock);
    /* A graceful MQTT DISCONNECT suppresses the will, so send NDEATH here. */
    if (node_ready() && birth_sent && MQTT_Is_Connected()) {
        SPB_Metric m;
        memset(&m, 0, sizeof(m));
        m.name = SPB_METRIC_BDSEQ;
        m.datatype = SPB_DT_UINT64;
        m.i64 = (int64_t)bdseq;
        m.timestamp_ms = now_ms();
        if (publish_payload("NDEATH", &m, 1, 0, 0, 1))
            LOG("NDEATH published (bdSeq=%llu)", (unsigned long long)bdseq);
    }
    birth_sent = 0;
    def_count = 0;
    settings.enabled = false;
    pthread_mutex_unlock(&spb_lock);
    initialized = 0;
}

void edge_node_set_command_callback(EdgeNodeCommandFn fn) {
    command_fn = fn;
}

cJSON *edge_node_status(void) {
    cJSON *s = cJSON_CreateObject();
    pthread_mutex_lock(&spb_lock);
    cJSON_AddBoolToObject(s, "online", node_ready() && birth_sent && MQTT_Is_Connected());
    cJSON_AddStringToObject(s, "group_id", settings.group_id);
    cJSON_AddStringToObject(s, "edge_node_id", settings.edge_node_id);
    cJSON_AddStringToObject(s, "device_id", settings.device_id);
    cJSON_AddStringToObject(s, "primary_host_id", settings.primary_host_id);
    cJSON_AddBoolToObject(s, "host_online", host_online);
    cJSON_AddNumberToObject(s, "bdseq", (double)bdseq);
    cJSON_AddNumberToObject(s, "seq", (double)seq);
    cJSON_AddNumberToObject(s, "metric_count", def_count);
    cJSON_AddNumberToObject(s, "buffered", hist_count);
    cJSON_AddNumberToObject(s, "data_published", (double)data_published);
    cJSON_AddNumberToObject(s, "commands_received", (double)commands_received);
    cJSON_AddNumberToObject(s, "last_birth", (double)(last_birth_ms / 1000));

    char topic[SPB_TOPIC_LEN] = "";
    if (node_ready())
        build_topic(topic, sizeof(topic), settings.device_id[0] ? "DDATA" : "NDATA");
    cJSON_AddStringToObject(s, "data_topic", topic);
    pthread_mutex_unlock(&spb_lock);
    return s;
}

char *edge_node_value(int index) {
    char *value = NULL;
    pthread_mutex_lock(&spb_lock);
    if (index >= 0 && index < def_count && defs[index].has_value)
        value = g_strdup(defs[index].value);
    pthread_mutex_unlock(&spb_lock);
    return value;
}
