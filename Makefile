# nautilusctl Makefile
#
# Three executables share the same modules:
#   nautilusctld     -> background service, sole owner of the LCD (systemd --user)
#   nautilusctl      -> command line (service client)
#   nautilusctl-gui  -> GTK 4 graphical interface (service client)
#
# pkg-config finds the flags (-I, -l) of the installed libraries.
# giflib ships no .pc file, so it is linked directly with -lgif.
#
# CFLAGS/LDFLAGS are left to the user (e.g. `make CFLAGS="-O2 -Werror"`);
# the flags the build needs live in ALL_CFLAGS/ALL_LDLIBS so that overriding
# CFLAGS on the command line cannot drop them.

CC       ?= cc
PKGS     := libturbojpeg libpng glib-2.0 gio-2.0 gio-unix-2.0
PANGO_CF := $(shell pkg-config --cflags pangocairo)
PANGO_LIBS := $(shell pkg-config --libs pangocairo)
CFLAGS   ?= -O2 -g
WARN     := -Wall -Wextra
ALL_CFLAGS := -std=c11 -D_DEFAULT_SOURCE $(WARN) $(shell pkg-config --cflags $(PKGS)) $(CFLAGS)
ALL_LDLIBS := $(shell pkg-config --libs $(PKGS)) -lgif -lm $(LDLIBS)
GTK_CF   := $(shell pkg-config --cflags gtk4)
GTK_LIBS := $(shell pkg-config --libs gtk4)

IMAGE    := build/image.o
DEVICE   := build/hid.o build/nautilus.o
PREFIX   ?= $(HOME)/.local
DATADIR  := $(PREFIX)/share
UNITDIR  := $(DATADIR)/systemd/user

BINS     := nautilusctld nautilusctl nautilusctl-gui

.PHONY: all clean install uninstall

all: $(BINS)

nautilusctld: build/daemon.o build/player.o build/ipc.o build/render.o build/sensors.o $(IMAGE) $(DEVICE)
	$(CC) $(LDFLAGS) -o $@ $^ $(ALL_LDLIBS) $(PANGO_LIBS)

nautilusctl: build/main.o build/ipc.o $(IMAGE)
	$(CC) $(LDFLAGS) -o $@ $^ $(ALL_LDLIBS)

nautilusctl-gui: build/gui.o build/ipc.o build/render.o build/sensors.o $(IMAGE)
	$(CC) $(LDFLAGS) -o $@ $^ $(ALL_LDLIBS) $(GTK_LIBS)

build/gui.o: src/gui.c src/*.h | build
	$(CC) $(ALL_CFLAGS) $(GTK_CF) -c -o $@ $<

build/render.o: src/render.c src/*.h | build
	$(CC) $(ALL_CFLAGS) $(PANGO_CF) -c -o $@ $<

build/%.o: src/%.c src/*.h | build
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: all
	install -Dm755 -t $(PREFIX)/bin $(BINS)
	install -Dm644 data/nautilusctl.desktop $(DATADIR)/applications/nautilusctl.desktop
	install -Dm644 data/nautilusctl.svg $(DATADIR)/icons/hicolor/scalable/apps/nautilusctl.svg
	install -Dm644 data/nautilusctld.service $(UNITDIR)/nautilusctld.service
	-gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null
	-update-desktop-database -q $(DATADIR)/applications 2>/dev/null
	-systemctl --user daemon-reload 2>/dev/null

uninstall:
	-systemctl --user disable --now nautilusctld.service
	rm -f $(addprefix $(PREFIX)/bin/,$(BINS))
	rm -f $(DATADIR)/applications/nautilusctl.desktop
	rm -f $(DATADIR)/icons/hicolor/scalable/apps/nautilusctl.svg
	rm -f $(UNITDIR)/nautilusctld.service
	-systemctl --user daemon-reload

clean:
	rm -rf build $(BINS)
