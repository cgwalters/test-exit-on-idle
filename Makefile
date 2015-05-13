all: org.verbum.TestExitOnIdle

CFLAGS ?= -ggdb -O0
CC ?= gcc

org.verbum.TestExitOnIdle: test-exit-on-idle.c Makefile
	$(CC) $(CFLAGS) $$(pkg-config --cflags --libs gio-2.0 libsystemd-daemon) -o $@ $<

org.verbum.TestExitOnIdle-client: test-exit-on-idle-client.c Makefile
	$(CC) $(CFLAGS) $$(pkg-config --cflags --libs gio-2.0 libsystemd-daemon) -o $@ $<
