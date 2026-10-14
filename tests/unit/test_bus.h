/* A private D-Bus session bus for unit tests, and connections to it. */
#ifndef TORCHLIGHT_TEST_BUS_H
#define TORCHLIGHT_TEST_BUS_H
#include <gio/gio.h>
#include <stddef.h>
/** Start dbus-daemon on a private session bus and write its address into address (capacity
 * bytes, NUL-terminated). The daemon is spawned and reaped directly, so GLib starts no
 * child-watch thread that would outlive the test. Stop it with test_bus_stop. Aborts on
 * failure. */
GPid test_bus_start(char *address, size_t capacity);
/** Stop and reap the bus started as pid. Aborts on failure. */
void test_bus_stop(GPid pid);
/** A new connection to the bus at borrowed address, owned by the caller. Aborts on failure. */
GDBusConnection *test_bus_connect(const char *address);
/** Own borrowed name on bus, which must be free. Aborts on failure. */
void test_bus_own(GDBusConnection *bus, const char *name);
#endif
