all: org.verbum.TestExitOnIdle org.verbum.TestExitOnIdle-client

CFLAGS ?= -ggdb -O0
CC ?= gcc

org.verbum.TestExitOnIdle: test-exit-on-idle.c Makefile
	$(CC) $(CFLAGS) $$(pkg-config --cflags --libs gio-2.0 libsystemd-daemon) -o $@ $<

org.verbum.TestExitOnIdle-client: test-exit-on-idle-client.c Makefile
	$(CC) $(CFLAGS) $$(pkg-config --cflags --libs gio-2.0 libsystemd-daemon) -o $@ $<

install:
	install -D org.verbum.TestExitOnIdle $(DESTDIR)/usr/lib/org.verbum.TestExitOnIdle
	install -D org.verbum.TestExitOnIdle.conf $(DESTDIR)/etc/dbus-1/system.d/org.verbum.TestExitOnIdle.conf 
	install -D org.verbum.TestExitOnIdle.service $(DESTDIR)/usr/share/dbus-1/system-services/org.verbum.TestExitOnIdle.service
	install -D dbus-org.verbum.TestExitOnIdle.service $(DESTDIR)/usr/lib/systemd/system/dbus-org.verbum.TestExitOnIdle.service
	if test -z "$(DESTDIR)"; then \
	  killall -HUP dbus-daemon; \
	  systemctl daemon-reload; \
	fi
