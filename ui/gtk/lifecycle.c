/* Read-only systemd lifecycle checks, used off the GTK thread after hidden idle. */
#include "lifecycle.h"
#include <string.h>
#include <unistd.h>
#define LIFECYCLE_DBUS_TIMEOUT_MS 1000
bool lifecycle_in_service(const char *groups) {
    if (groups == NULL)
        return true;
    char **lines = g_strsplit(groups, "\n", -1);
    bool managed = false;
    for (size_t i = 0; lines[i] != NULL; i++) {
        char **parts = g_strsplit(lines[i], "/", -1);
        bool service = false;
        for (size_t j = 0; parts[j] != NULL; j++) {
            if (g_str_has_suffix(parts[j], ".service"))
                service = true;
            else if (g_str_has_suffix(parts[j], ".scope"))
                service = false;
        }
        managed = managed || service;
        g_strfreev(parts);
    }
    g_strfreev(lines);
    return managed;
}
static bool forced_restart(GVariant *properties) {
    GVariant *forced =
        g_variant_lookup_value(properties, "RestartForceExitStatus", G_VARIANT_TYPE("(aiai)"));
    if (forced == NULL)
        return true;
    GVariant *codes = g_variant_get_child_value(forced, 0);
    bool configured = g_variant_n_children(codes) != 0;
    g_variant_unref(codes);
    g_variant_unref(forced);
    return configured;
}
bool lifecycle_service_allows_exit(GVariant *properties, guint32 pid) {
    guint32 main_pid = 0;
    const char *restart = NULL, *exit_type = NULL, *type = NULL;
    guint64 watchdog = 0;
    if (properties == NULL || !g_variant_lookup(properties, "MainPID", "u", &main_pid) ||
        main_pid == 0 || !g_variant_lookup(properties, "Type", "&s", &type) ||
        strcmp(type, "dbus") == 0)
        return false;
    /* Exiting a member cannot complete a service whose main process is another app. */
    if (main_pid != pid)
        return true;
    if (!g_variant_lookup(properties, "Restart", "&s", &restart) ||
        (strcmp(restart, "no") != 0 && strcmp(restart, "on-failure") != 0 &&
         strcmp(restart, "on-abnormal") != 0 && strcmp(restart, "on-abort") != 0 &&
         strcmp(restart, "on-watchdog") != 0))
        return false;
    /* Only cgroup lifetime keeps service stop hooks deferred until children end.
     * KillMode=process alone cannot protect children from custom ExecStop hooks. */
    return g_variant_lookup(properties, "ExitType", "&s", &exit_type) &&
           strcmp(exit_type, "cgroup") == 0 &&
           g_variant_lookup(properties, "WatchdogUSec", "t", &watchdog) && watchdog == 0 &&
           !forced_restart(properties);
}
static GVariant *service_properties(GDBusConnection *bus, GCancellable *cancel) {
    GVariant *unit = g_dbus_connection_call_sync(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "GetUnitByPID", g_variant_new("(u)", (guint32)getpid()),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, LIFECYCLE_DBUS_TIMEOUT_MS, cancel,
        NULL);
    if (unit == NULL)
        return NULL;
    const char *path = NULL;
    g_variant_get(unit, "(&o)", &path);
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.systemd1", path, "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.freedesktop.systemd1.Service"), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, LIFECYCLE_DBUS_TIMEOUT_MS, cancel, NULL);
    g_variant_unref(unit);
    if (reply == NULL)
        return NULL;
    GVariant *properties = g_variant_get_child_value(reply, 0);
    g_variant_unref(reply);
    return properties;
}
tl_status lifecycle_can_exit(GCancellable *cancel, bool *allowed) {
    if (allowed == NULL)
        return TL_INVALID;
    *allowed = false;
    char *groups = NULL;
    if (!g_file_get_contents("/proc/self/cgroup", &groups, NULL, NULL))
        return TL_IO;
    bool service = lifecycle_in_service(groups);
    g_free(groups);
    if (!service) {
        *allowed = true;
        return TL_OK;
    }
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancel, NULL);
    if (bus == NULL)
        return TL_IO;
    GVariant *properties = service_properties(bus, cancel);
    g_object_unref(bus);
    if (properties == NULL)
        return TL_IO;
    *allowed = lifecycle_service_allows_exit(properties, (guint32)getpid());
    g_variant_unref(properties);
    return TL_OK;
}
