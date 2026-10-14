/* GNOME Shell window source over Torchlight's GNOME Shell extension, and enabling that
 * extension once per user (ADR 0051). */
#include "windows_gnome.h"
#include <string.h>
#define SHELL_NAME "org.gnome.Shell"
#define WINDOWS_PATH "/org/torchlight/Windows"
#define WINDOWS_INTERFACE "org.torchlight.Windows"
/* GNOME Shell serves its extensions interface on its own name and object. */
#define EXTENSIONS_PATH "/org/gnome/Shell"
#define EXTENSIONS_INTERFACE "org.gnome.Shell.Extensions"
/* The launcher's own application id: its popup is never listed as one of its windows. */
#define LAUNCHER_ID "org.torchlight.Launcher"
/* GNOME Shell's ExtensionState for an active (enabled and running) extension. */
#define EXTENSION_ACTIVE 1.0
bool windows_gnome_desktop(const char *desktop) {
    if (desktop == NULL)
        return false;
    char **names = g_strsplit(desktop, ":", -1);
    bool gnome = false;
    for (char **name = names; *name != NULL && !gnome; name++)
        gnome = g_ascii_strcasecmp(*name, "GNOME") == 0;
    g_strfreev(names);
    return gnome;
}
static GVariant *windows_call(GDBusConnection *bus, const char *method, GVariant *arguments,
                              const char *reply) {
    return g_dbus_connection_call_sync(
        bus, SHELL_NAME, WINDOWS_PATH, WINDOWS_INTERFACE, method, arguments, G_VARIANT_TYPE(reply),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, WINDOWS_GNOME_TIMEOUT_MS, NULL, NULL);
}
static void copy_window(tl_window *window, guint64 handle, const char *instance,
                        const char *class_name, const char *app_id, const char *title) {
    *window = (tl_window){.handle = handle};
    windows_copy_name(window->instance, sizeof(window->instance), instance);
    windows_copy_name(window->class_name, sizeof(window->class_name), class_name);
    windows_copy_name(window->app_id, sizeof(window->app_id), app_id);
    windows_copy_text(window->title, sizeof(window->title), title, strlen(title), false);
}
static tl_status gnome_list(void *context, tl_window *out, size_t capacity, size_t *count) {
    *count = 0;
    GVariant *reply = windows_call(context, "List", NULL, "(a(tssss))");
    if (reply == NULL)
        return TL_IO;
    GVariantIter *windows = NULL;
    g_variant_get(reply, "(a(tssss))", &windows);
    guint64 handle = 0;
    const char *instance = NULL, *class_name = NULL, *app_id = NULL, *title = NULL;
    while (*count < capacity && g_variant_iter_next(windows, "(t&s&s&s&s)", &handle, &instance,
                                                    &class_name, &app_id, &title)) {
        if (handle == 0 || strcmp(class_name, LAUNCHER_ID) == 0 || strcmp(app_id, LAUNCHER_ID) == 0)
            continue;
        copy_window(&out[(*count)++], handle, instance, class_name, app_id, title);
    }
    g_variant_iter_free(windows);
    g_variant_unref(reply);
    return TL_OK;
}
static tl_status gnome_activate(void *context, uint64_t handle) {
    GVariant *reply =
        windows_call(context, "Activate", g_variant_new("(t)", (guint64)handle), "(b)");
    if (reply == NULL)
        return TL_IO;
    gboolean found = false;
    g_variant_get(reply, "(b)", &found);
    g_variant_unref(reply);
    return found ? TL_OK : TL_STATE;
}
static void gnome_destroy(void *context) {
    g_object_unref(context);
}
/* A reference to bus, or to the session bus when bus is NULL; NULL without one. */
static GDBusConnection *connection(GDBusConnection *bus) {
    return bus != NULL ? g_object_ref(bus) : g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
}
tl_status windows_gnome_source(GDBusConnection *bus, tl_window_source *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    GDBusConnection *owned = connection(bus);
    if (owned == NULL)
        return TL_IO;
    *out = (tl_window_source){owned, gnome_list, gnome_activate, gnome_destroy};
    return TL_OK;
}
static GVariant *extensions_call(GDBusConnection *bus, const char *interface, const char *method,
                                 GVariant *arguments, const char *reply) {
    return g_dbus_connection_call_sync(
        bus, SHELL_NAME, EXTENSIONS_PATH, interface, method, arguments, G_VARIANT_TYPE(reply),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, WINDOWS_GNOME_TIMEOUT_MS, NULL, NULL);
}
/* What GNOME Shell knows of the extension. */
typedef enum { EXTENSION_UNKNOWN, EXTENSION_INSTALLED, EXTENSION_ENABLED } extension_state;
static extension_state lookup_extension(GDBusConnection *bus) {
    GVariant *reply = extensions_call(bus, EXTENSIONS_INTERFACE, "GetExtensionInfo",
                                      g_variant_new("(s)", WINDOWS_GNOME_EXTENSION), "(a{sv})");
    if (reply == NULL)
        return EXTENSION_UNKNOWN;
    GVariant *info = g_variant_get_child_value(reply, 0);
    double state = 0;
    gboolean enabled = false;
    bool has_state = g_variant_lookup(info, "state", "d", &state);
    /* Newer shells say whether it is enabled; older ones only report its state. */
    bool has_enabled = g_variant_lookup(info, "enabled", "b", &enabled);
    g_variant_unref(info);
    g_variant_unref(reply);
    if (!has_state && !has_enabled)
        return EXTENSION_UNKNOWN;
    bool on = has_enabled ? enabled != 0 : state == EXTENSION_ACTIVE;
    return on ? EXTENSION_ENABLED : EXTENSION_INSTALLED;
}
/* Whether the person allows extensions at all (GNOME's disable-user-extensions setting). */
static bool extensions_allowed(GDBusConnection *bus) {
    GVariant *reply = extensions_call(
        bus, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", EXTENSIONS_INTERFACE, "UserExtensionsEnabled"), "(v)");
    if (reply == NULL)
        return false;
    GVariant *value = NULL;
    g_variant_get(reply, "(v)", &value);
    bool allowed =
        g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
    g_variant_unref(value);
    g_variant_unref(reply);
    return allowed;
}
static bool enable_extension(GDBusConnection *bus) {
    GVariant *reply = extensions_call(bus, EXTENSIONS_INTERFACE, "EnableExtension",
                                      g_variant_new("(s)", WINDOWS_GNOME_EXTENSION), "(b)");
    if (reply == NULL)
        return false;
    gboolean enabled = false;
    g_variant_get(reply, "(b)", &enabled);
    g_variant_unref(reply);
    return enabled != 0;
}
static void write_marker(const char *marker) {
    char *folder = g_path_get_dirname(marker);
    if (g_mkdir_with_parents(folder, 0700) == 0)
        (void)g_file_set_contents(marker, "", 0, NULL);
    g_free(folder);
}
void windows_gnome_enable_once(GDBusConnection *bus, const char *marker) {
    if (marker == NULL || g_file_test(marker, G_FILE_TEST_EXISTS))
        return;
    GDBusConnection *owned = connection(bus);
    if (owned == NULL)
        return;
    extension_state state = lookup_extension(owned);
    bool enabled = state == EXTENSION_ENABLED;
    if (state == EXTENSION_INSTALLED && extensions_allowed(owned))
        enabled = enable_extension(owned);
    if (enabled)
        write_marker(marker);
    g_object_unref(owned);
}
