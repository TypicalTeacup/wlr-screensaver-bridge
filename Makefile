PREFIX ?= /usr/local
CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
IDLE_INHIBIT_XML := $(WAYLAND_PROTOCOLS)/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml

wlr-screensaver-bridge: main.c idle-inhibit-protocol.c idle-inhibit-unstable-v1-client-protocol.h
	$(CC) -O2 -Wall -Wextra -o $@ main.c idle-inhibit-protocol.c \
		$(shell $(PKG_CONFIG) --cflags --libs libsystemd wayland-client)

idle-inhibit-unstable-v1-client-protocol.h:
	wayland-scanner client-header $(IDLE_INHIBIT_XML) $@

idle-inhibit-protocol.c:
	wayland-scanner private-code $(IDLE_INHIBIT_XML) $@

install: wlr-screensaver-bridge
	install -Dm755 wlr-screensaver-bridge -t $(DESTDIR)$(PREFIX)/bin
	install -Dm644 wlr-screensaver-bridge.service -t $(DESTDIR)$(PREFIX)/lib/systemd/user

clean:
	rm -f wlr-screensaver-bridge idle-inhibit-protocol.c idle-inhibit-unstable-v1-client-protocol.h

.PHONY: install clean
