# Third-party notices

The packaging and app code in this repository is licensed under the BSD 3-Clause
License; bundled upstream components retain their own licenses. The license
texts ship inside every package as `THIRD_PARTY_LICENSES.txt`
(source: [app/THIRD_PARTY_LICENSES.txt](app/THIRD_PARTY_LICENSES.txt)).

## cJSON

- Project: <https://github.com/DaveGamble/cJSON>
- Version: 1.7.18 (`app/cJSON.c`, `app/cJSON.h`).
- License: `MIT`.

## Libraries provided by AXIS OS

GLib, OpenSSL, libcurl and the ACAP SDK libraries are linked dynamically from
the device and are not distributed with this package.

## Sparkplug B payload encoding

`app/sparkplug_pb.c` encodes and decodes the Sparkplug B payload from the field
numbers in the Sparkplug specification. It contains no code or schema files
from Eclipse Tahu.

## Code shared with Event Engine

`app/mqtt_client.c`, `app/sparkplug_pb.c` and `app/certs.c` come from
[Event Engine](https://github.com/Mo3he/Event-Engine-ACAP) by the same author,
under the same BSD 3-Clause License.

## Axis ACAP Native SDK

- Project: <https://developer.axis.com/acap/>
- Used at build time to compile and package the application.

## Trademarks

Sparkplug is a trademark of the Eclipse Foundation. All other product names,
logos and brands are property of their respective owners. This is an
independent community project and is not affiliated with or endorsed by Axis
Communications or the Eclipse Foundation.
