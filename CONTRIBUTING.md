# Contributing

## Development setup

The package is built with Docker or Podman and the Axis ACAP Native SDK image.

Build both ACAP4 architectures with:

```sh
./build.sh
```

Build one architecture with:

```sh
ARCHES=aarch64 ./build.sh
```

Install on a camera:

```sh
PW='...'
curl -s --digest -u "admin:$PW" 'http://DEVICE/axis-cgi/applications/control.cgi?action=stop&package=Sparkplug_Edge'
curl -s --digest -u "admin:$PW" -F "packfil=@releases/Sparkplug_Edge_Node_0_1_0_aarch64.eap" 'http://DEVICE/axis-cgi/applications/upload.cgi'
curl -s --digest -u "admin:$PW" 'http://DEVICE/axis-cgi/applications/control.cgi?action=start&package=Sparkplug_Edge'
```

Run the host unit tests (metric declarations and the Sparkplug B codec) with:

```sh
sh tests/run.sh
```

[tools/sparkplug_host.py](tools/sparkplug_host.py) is a minimal Sparkplug
primary host for checking a device against a broker without a SCADA system.

Do not commit API keys, camera passwords, generated `.eap` files, or local state.

## Code shared with Event Engine

`app/mqtt_client.c`, `app/sparkplug_pb.c` and `app/certs.c` (and their headers)
are copies of the same files in
[Event Engine](https://github.com/Mo3he/Event-Engine-ACAP) (`app/engine/`); only
the `cJSON.h` include path differs. Keep them identical: make a fix in both
projects. `app/edge_node.c` is adapted from Event Engine's `sparkplug.c`, so
check session-logic fixes against it too.

## Pull requests

Keep changes focused, update `CHANGELOG.md` when behavior changes, and include
the camera model and AXIS OS version when reporting hardware results. Verify the
app still works without root privileges.
