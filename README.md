# Sparkplug B Edge Node ACAP for Axis Devices

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis_Cam_Sparkplug?style=flat&include_prereleases)](https://github.com/Mo3he/Axis_Cam_Sparkplug/releases)
[![Build](https://github.com/Mo3he/Axis_Cam_Sparkplug/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_Sparkplug/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/Mo3he/Axis_Cam_Sparkplug?style=flat)](LICENSE)
[![Super-Linter](https://github.com/Mo3he/Axis_Cam_Sparkplug/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_Sparkplug/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

Makes an Axis camera or sensor an MQTT Sparkplug B edge node, so SCADA and
industrial IoT platforms (Ignition, Honeywell EBI, Cirrus Link and other
Sparkplug hosts) discover its metrics and receive changes without a gateway in
between.

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB. Use at your own risk.

> **Beta:** feedback from anyone running it against a Sparkplug host is very
> welcome in [Issues](https://github.com/Mo3he/Axis_Cam_Sparkplug/issues).

## Overview

- Each metric follows one field of a device event: for example the CO2 level
  of an AXIS D6310 Air Quality Sensor, a people count from AXIS Object
  Analytics, or a virtual input.
- Metrics are declared on the app's settings page, which lists the events the
  device actually declares.
- The app publishes a birth certificate (NBIRTH, or DBIRTH under a device ID)
  with every metric, then only the metrics that changed (report by exception).
- A host can write metrics declared writable. Each write sends a device event,
  so AXIS OS action rules or [Event Engine](https://github.com/Mo3he/Event-Engine-ACAP)
  can react to it, for example by playing an audio clip.
- Data collected while the broker or the primary host is offline is buffered
  and replayed as historical data when it returns.
- TLS, a private CA, client certificates and broker credentials are supported.

## Compatibility

| Package | AXIS OS | Architecture | Status |
|---|---|---|---|
| ACAP 4 | 11.11 - 13 | aarch64 | Beta, tested on AXIS OS 12.11 |
| ACAP 4 | 11.11 - 13 | armv7hf | Beta, built but not yet tested on a device |

The app runs as the unprivileged `sdk` user. It does not require root access.

## Installation

Download the EAP matching the device architecture from the
[Releases page](https://github.com/Mo3he/Axis_Cam_Sparkplug/releases), install
it from **Apps -> Add app** in the device web interface and start it. Then open
**Apps -> Sparkplug Edge Node -> Open**, set the broker and group ID, add
metrics and enable the edge node. The edge node is disabled after installation,
so nothing is published until it is turned on.

## Configuration

### Edge node

| Setting | Default | Description |
|---|---|---|
| Enable | off | Connects to the broker and publishes. |
| Group ID | none | Required. The Sparkplug group, e.g. `Building1`. |
| Edge node ID | serial number | Unique within the group. |
| Device ID | none | Optional. Publishes the metrics under a Sparkplug device (DBIRTH/DDATA) instead of the node. |
| Primary host ID | none | Optional. While this host reports itself offline, data is buffered; when it returns the node publishes a new birth and replays the buffer. |
| Specification | 3.0 | Sparkplug 3.0 or B 2.2. Only the host `STATE` topic and payload differ. |

IDs must not contain `/`, `+` or `#`.

### MQTT broker

| Setting | Default | Description |
|---|---|---|
| Host, Port | none, 1883 (8883 with TLS) | The broker. |
| Alternate brokers | none | Up to 7 more brokers (`host:port`), tried in order when the broker cannot be reached. |
| Client ID | `Sparkplug_Edge-<serial>` | |
| Username, Password | none | The password is never shown again after saving. |
| Keepalive | 60 s | |
| Use TLS | off | |
| Verify the broker's certificate | on | Checks the certificate chain and that it matches the host name or IP address. |

**TLS certificates:** optionally paste a CA certificate (for a broker with a
private CA) and a client certificate and key (for brokers that require mutual
TLS). An encrypted key needs its passphrase; it is stored decrypted, readable
only by the app. Without a CA certificate the device's trusted CAs are used.

### Metrics

| Field | Description |
|---|---|
| Name | Unique, up to 127 characters. Use `/` to group metrics, e.g. `Air/CO2`. Names reserved by Sparkplug (`bdSeq`, `Node Control/...`, `Device Control/...`, `Properties/...`) are rejected. |
| Data type | Boolean, Int32, Int64, Float, Double or String. |
| Device event | The event, an optional source filter (e.g. `port` equals `1`), and the data field that carries the value. |
| Invert | Boolean only: publish true as false. |
| Writable | No event: the host writes the value (see below). |

How event values are converted:

| Data type | From | Rule |
|---|---|---|
| Boolean | boolean, number or text | True, non-zero, `on`, `active`, `yes` or `high` is true. |
| Int32, Int64 | number, boolean or numeric text | Rounded to the nearest integer; values out of range are rejected. |
| Float, Double | number, boolean or numeric text | The number; true is 1. |
| String | any | The value as text. |

A value that cannot be converted is not published and is shown on the
settings page. Metrics bound to a stateful event get their value as soon as
they are saved; others get it with the next event. Metrics bound to an AXIS
Object Analytics occupancy scenario read the scenario's current count when
they start.

The metric list can also be read and written as JSON at
`/local/Sparkplug_Edge/api/metrics`:

```json
{
  "metrics": [
    {
      "name": "IO/Virtual input 1",
      "datatype": "Boolean",
      "event": {
        "topic": ["tns1:Device", "tnsaxis:IO", "tnsaxis:VirtualInput"],
        "source_key": "port",
        "source_value": "1",
        "data_key": "active"
      }
    },
    { "name": "Commands/Message", "datatype": "String", "writable": true }
  ]
}
```

### Host writes

A host writes a metric with an NCMD (or DCMD under a device ID). The app
accepts writes only to metrics declared writable, and only values that fit the
metric's data type; other writes are ignored and logged. An accepted write is
published back as the metric's new value, as Sparkplug hosts expect, and sends
the device event **Sparkplug Edge Node / Metric written by host**
(`tnsaxis:CameraApplicationPlatform/Sparkplug_Edge/MetricWritten`) with the
metric name as its source and the written `value` as data. Pick the metric in
an action rule's event conditions to react to it. `Node Control/Rebirth`
always works.

### Example: AXIS D6310 Air Quality Sensor

[examples/axis-d6310-air-quality.json](examples/axis-d6310-air-quality.json)
publishes the sensor's measurements and detections as 14 metrics: temperature,
humidity, CO2, PM1.0, PM2.5, PM4.0, PM10, VOC index, NOx index, AQI, heat
index and humidex under `Air/`, and vaping and masking under `Detection/`. The
measurements come from the sensor's metadata event, about once a second, and
are published when they change. Load the file with:

```sh
curl --digest -u USER:PASS -H 'Content-Type: application/json' \
  --data @examples/axis-d6310-air-quality.json \
  http://SENSOR/local/Sparkplug_Edge/api/metrics
```

## Ports & security

- The app opens no listening network port. It connects out to the configured
  MQTT brokers.
- The settings and status API binds `127.0.0.1:2214` and is reachable only
  through the device's authenticated reverse proxy, restricted to `admin`.
- The broker password and the client key are stored in the app's data
  directory, readable only by the app. The API never returns them.
- Without TLS, MQTT traffic and the broker password cross the network in clear
  text. Use TLS on untrusted networks.
- Hosts can only write metrics declared writable, and a write only sends a
  device event: what happens next is decided by the device's own rules.
- To read the current count of AXIS Object Analytics scenarios the app uses
  the device's VAPIX service account, through D-Bus
  (`com.axis.HTTPConf1.VAPIXServiceAccounts1.GetCredentials`).
- The edge node is disabled by default.

## How it works

Each metric subscribes to its device event through the ACAP event API, and
the GLib main loop converts the value and queues it. Changes from one event are
published together in one DATA message. The MQTT client runs on its own
thread. `bdSeq` is kept across restarts, so a late NDEATH from an earlier
session never ends the current one; NDEATH is both registered as the MQTT will
and sent explicitly on a clean shutdown. Changing the metric list publishes a
new birth; changing the node's identity reconnects so the will matches.

## Testing

Tested on an AXIS P3288-LV (AXIS OS 12.11.77) with Mosquitto 2.1 and
[tools/sparkplug_host.py](tools/sparkplug_host.py), which checks birth before
data, seq order, aliases and that NDEATH matches its birth:

- NBIRTH with aliases, then NDATA by exception, for virtual inputs, an Object
  Analytics occupancy scenario and the recording status;
- writes to writable metrics published back and sent as a device event (read
  back through a metric bound to that event); writes to read-only metrics and
  values of the wrong type ignored;
- Rebirth; restart and disable with a matching NDEATH and a new `bdSeq`;
- broker outage and primary host offline: changes buffered, then a new birth
  and replay marked historical;
- username and password, including recovery right after a wrong password is
  corrected.

## Limitations

- Metrics carry no Sparkplug properties (such as engineering units) and no
  templates or datasets.
- Writable metrics only send a device event; the app does not control the
  device itself.
- A writable metric has no value until a host writes it, and loses it when the
  app restarts.
- Removing a writable metric takes about 2 seconds per metric to save, because
  its event declaration is removed synchronously.
- The buffer holds the latest 256 changes; older ones are dropped.
- MQTT 3.1.1 only, QoS 0 for data and QoS 1 for death certificates, with clean
  sessions as Sparkplug requires.

## Relationship to Event Engine

[Event Engine](https://github.com/Mo3he/Event-Engine-ACAP) also has a Sparkplug
B edge node, as one feature of a rules engine. This app is the same edge node
as a single-purpose app: the MQTT client, Sparkplug codec and certificate store
are shared with Event Engine, and the session logic is adapted from it. Use
Event Engine when metrics should come from rules (conditions, schedules,
templates); use this app when metrics simply follow device events.

## Build from source

Docker or Podman is required; the Axis ACAP Native SDK image does the build.

```sh
./build.sh                 # aarch64 and armv7hf into ./releases
ARCHES=aarch64 ./build.sh  # a single architecture
```

### Tests

`tests/run.sh` builds and runs the host unit tests for the metric declarations,
value conversion and the Sparkplug B codec, with AddressSanitizer and UBSan.

### CI

Every push runs the checks and lint. A release is built by running the Build
workflow manually with a version, which creates a draft release with unsigned
packages; signed packages are added before it is published.

## Links

- Releases: <https://github.com/Mo3he/Axis_Cam_Sparkplug/releases>
- Issues: <https://github.com/Mo3he/Axis_Cam_Sparkplug/issues>
- Sparkplug specification: <https://sparkplug.eclipse.org/>
- ACAP documentation: <https://developer.axis.com/acap/>
- Axis Communications: <https://www.axis.com/>

## License

The packaging and app code in this repository is licensed under BSD 3-Clause
(see [LICENSE](LICENSE)). Bundled upstream components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
