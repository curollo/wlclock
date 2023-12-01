BINS = wlclock

PREFIX ?= /usr/local
CFLAGS += -Wall -Wextra -Wno-unused-parameter -g

all: $(BINS) $(MANS)

clean:
	$(RM) $(BINS) $(addsuffix .o,$(BINS))

install: all
	install -D -t $(PREFIX)/bin $(BINS)
	install -Dm644 -t $(PREFIX)/share/man/man1 $(MANS)

WAYLAND_PROTOCOLS=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(shell pkg-config --variable=wayland_scanner wayland-scanner)

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.o: xdg-shell-protocol.h

wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/wlr-layer-shell-unstable-v1.xml $@

wlr-layer-shell-unstable-v1-protocol.o: wlr-layer-shell-unstable-v1-protocol.h

wlclock.o: utf8.h xdg-shell-protocol.h wlr-layer-shell-unstable-v1-protocol.h

# Protocol dependencies
wlclock: xdg-shell-protocol.o wlr-layer-shell-unstable-v1-protocol.o

# Library dependencies
wlclock: CFLAGS+=$(shell pkg-config --cflags wayland-client fcft pixman-1)
wlclock: LDLIBS+=$(shell pkg-config --libs wayland-client fcft pixman-1) -lrt

.PHONY: all clean install
