PROG = Sparkplug_Edge
SRCS = main.c edge_node.c bindings.c command_events.c metrics_config.c event_log.c cJSON.c
# Copied unchanged from Event Engine (app/engine/), so fixes port both ways.
SHARED = mqtt_client.c sparkplug_pb.c certs.c
PKGS = axparameter axevent glib-2.0 gio-2.0 libcurl

CFLAGS += -Wall -Wextra -Werror $(shell pkg-config --cflags $(PKGS))
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,-z,noexecstack -Wl,--gc-sections
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lssl -lcrypto -lpthread -lm

SHARED_OBJS = $(SHARED:.c=.o)

all: $(PROG)

$(SHARED_OBJS): %.o: %.c
	$(CC) $(CFLAGS) -Wno-unused-function -c -o $@ $<

$(PROG): $(SRCS) $(SHARED_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	$(STRIP) $@ 2>/dev/null || true

clean:
	rm -f $(PROG) $(SHARED_OBJS)
