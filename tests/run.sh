#!/bin/sh
# Host unit tests: metric declarations, value conversion and the Sparkplug B
# protobuf codec.
set -eu
cd "$(dirname "$0")/.."
cc=${CC:-cc}
flags="-std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iapp"
"$cc" $flags -o /tmp/metrics_config_test tests/metrics_config_test.c app/metrics_config.c app/cJSON.c -lm
/tmp/metrics_config_test
"$cc" $flags -o /tmp/sparkplug_pb_test tests/sparkplug_pb_test.c app/sparkplug_pb.c
/tmp/sparkplug_pb_test
