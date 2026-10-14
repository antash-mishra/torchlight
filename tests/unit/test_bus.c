/* A private D-Bus session bus for unit tests, and connections to it. */
#include "test_bus.h"
#include "test.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
GPid test_bus_start(char *address, size_t capacity) {
    char *argv[] = {"dbus-daemon", "--session", "--nofork", "--print-address=1", NULL};
    GPid pid = 0;
    int out = -1;
    CHECK(g_spawn_async_with_pipes(NULL, argv, NULL,
                                   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                                   &pid, NULL, &out, NULL, NULL));
    size_t used = 0;
    while (used + 1 < capacity && read(out, address + used, 1) == 1 && address[used] != '\n')
        used++;
    address[used] = 0;
    close(out);
    CHECK(used > 0);
    return pid;
}
void test_bus_stop(GPid pid) {
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, NULL, 0) == pid);
    g_spawn_close_pid(pid);
}
GDBusConnection *test_bus_connect(const char *address) {
    GDBusConnection *bus =
        g_dbus_connection_new_for_address_sync(address,
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    CHECK(bus != NULL);
    return bus;
}
void test_bus_own(GDBusConnection *bus, const char *name) {
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", name, 0), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        NULL);
    CHECK(reply != NULL);
    guint32 result = 0;
    g_variant_get(reply, "(u)", &result);
    /* DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER */
    CHECK(result == 1);
    g_variant_unref(reply);
}
