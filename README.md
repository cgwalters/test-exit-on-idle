Demo of a race-free GDBus daemon that exits when idle

Background:
https://bugs.freedesktop.org/show_bug.cgi?id=11454  

Inspired by http://cgit.freedesktop.org/systemd/systemd/tree/src/libsystemd/sd-bus/bus-util.c?id=91077af69e4eb5f7631e25a2bc5ae8e3c9c08178

### Use sd_notify(STOPPING=1) to ensure systemd queues start requests

This is what the above does - systemd is tracking our process state,
and thus ensures mutual exclusion.

### Do the asynchronous release of the name

Needs a GDBus patch to clean up.

### Ignore `SIGTERM` as systemd seems to send that quickly for some reason

TODO: Debug why this is

### Need a clean tristate for RUNNING/FLUSHING/EXITING

The systemd event loop has special support for exiting, which
GMainLoop doesn't.  This demo app uses a tristate.

### Save our state on disk

A stateful service needs to actually save things on disk, this app
demos how to do that in an async/timed fashion.
