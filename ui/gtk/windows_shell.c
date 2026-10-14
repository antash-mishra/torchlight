/* Window source over Torchlight's GNOME Shell and Cinnamon extensions, and enabling the
 * extension once per user (ADR 0051, ADR 0054). */
#include "windows_shell.h"
#include <string.h>
#define WINDOWS_PATH "/org/torchlight/Windows"
#define WINDOWS_INTERFACE "org.torchlight.Windows"
/* GNOME Shell serves its extensions interface on its own name and object. */
#define EXTENSIONS_PATH "/org/gnome/Shell"
#define EXTENSIONS_INTERFACE "org.gnome.Shell.Extensions"
/* GNOME Shell's ExtensionState for an active (enabled and running) extension. */
#define EXTENSION_ACTIVE 1.0
/* Cinnamon's list of enabled extensions, which it watches and applies at once. */
#define CINNAMON_ENABLED_KEY "enabled-extensions"
/* The launcher's own application id: its popup is never listed as one of its windows. */
#define LAUNCHER_ID "org.torchlight.Launcher"
struct shell_source {
    GDBusConnection *bus;
    char *shell;
};
bool windows_shell_desktop(const char *desktops, const char *name) {
    if (desktops == NULL || name == NULL)
        return false;
    char **names = g_strsplit(desktops, ":", -1);
    bool found = false;
    for (char **entry = names; *entry != NULL && !found; entry++)
        found = g_ascii_strcasecmp(*entry, name) == 0;
    g_strfreev(names);
    return found;
}
/* A reference to bus, or to the session bus when bus is NULL; NULL without one. */
static GDBusConnection *connection(GDBusConnection *bus) {
    return bus != NULL ? g_object_ref(bus) : g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
}
bool windows_shell_name_owned(GDBusConnection *bus, const char *name) {
    GDBusConnection *owned = name != NULL ? connection(bus) : NULL;
    if (owned == NULL)
        return false;
    GVariant *reply = g_dbus_connection_call_sync(
        owned, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameHasOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE,
        WINDOWS_SHELL_TIMEOUT_MS, NULL, NULL);
    g_object_unref(owned);
    if (reply == NULL)
        return false;
    gboolean has_owner = false;
    g_variant_get(reply, "(b)", &has_owner);
    g_variant_unref(reply);
    return has_owner != 0;
}
static GVariant *windows_call(const struct shell_source *source, const char *method,
                              GVariant *arguments, const char *reply) {
    return g_dbus_connection_call_sync(source->bus, source->shell, WINDOWS_PATH, WINDOWS_INTERFACE,
                                       method, arguments, G_VARIANT_TYPE(reply),
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START, WINDOWS_SHELL_TIMEOUT_MS,
                                       NULL, NULL);
}
static void copy_window(tl_window *window, guint64 handle, const char *instance,
                        const char *class_name, const char *app_id, const char *title) {
    *window = (tl_window){.handle = handle};
    windows_copy_name(window->instance, sizeof(window->instance), instance);
    windows_copy_name(window->class_name, sizeof(window->class_name), class_name);
    windows_copy_name(window->app_id, sizeof(window->app_id), app_id);
    windows_copy_text(window->title, sizeof(window->title), title, strlen(title), false);
}
static tl_status shell_list(void *context, tl_window *out, size_t capacity, size_t *count) {
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
static tl_status shell_activate(void *context, uint64_t handle) {
    GVariant *reply =
        windows_call(context, "Activate", g_variant_new("(t)", (guint64)handle), "(b)");
    if (reply == NULL)
        return TL_IO;
    gboolean found = false;
    g_variant_get(reply, "(b)", &found);
    g_variant_unref(reply);
    return found ? TL_OK : TL_STATE;
}
static void shell_destroy(void *context) {
    struct shell_source *source = context;
    if (source == NULL)
        return;
    g_object_unref(source->bus);
    g_free(source->shell);
    g_free(source);
}
tl_status windows_shell_source(GDBusConnection *bus, const char *shell, tl_window_source *out) {
    if (out == NULL || shell == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    struct shell_source *source = g_try_new0(struct shell_source, 1);
    if (source == NULL)
        return TL_NOMEM;
    source->bus = connection(bus);
    source->shell = g_strdup(shell);
    if (source->bus == NULL) {
        g_free(source->shell);
        g_free(source);
        return TL_IO;
    }
    *out = (tl_window_source){
        .list = shell_list, .activate = shell_activate, .destroy = shell_destroy};
    out->context = source;
    return TL_OK;
}
static GVariant *extensions_call(GDBusConnection *bus, const char *interface, const char *method,
                                 GVariant *arguments, const char *reply) {
    return g_dbus_connection_call_sync(bus, WINDOWS_SHELL_GNOME, EXTENSIONS_PATH, interface, method,
                                       arguments, G_VARIANT_TYPE(reply),
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START, WINDOWS_SHELL_TIMEOUT_MS,
                                       NULL, NULL);
}
/* What GNOME Shell knows of the extension. */
typedef enum { EXTENSION_UNKNOWN, EXTENSION_INSTALLED, EXTENSION_ENABLED } extension_state;
static extension_state lookup_extension(GDBusConnection *bus) {
    GVariant *reply = extensions_call(bus, EXTENSIONS_INTERFACE, "GetExtensionInfo",
                                      g_variant_new("(s)", WINDOWS_SHELL_EXTENSION), "(a{sv})");
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
                                      g_variant_new("(s)", WINDOWS_SHELL_EXTENSION), "(b)");
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
void windows_shell_enable_gnome_once(GDBusConnection *bus, const char *marker) {
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
/* Whether a data folder holds the Cinnamon extension, as Cinnamon looks for it. Listing an
 * extension Cinnamon cannot find only fills its log with errors. */
static bool cinnamon_extension_installed(const char *const *data_dirs) {
    bool found = false;
    for (const char *const *folder = data_dirs; folder != NULL && *folder != NULL && !found;
         folder++) {
        char *metadata = g_build_filename(*folder, "cinnamon", "extensions",
                                          WINDOWS_SHELL_EXTENSION, "metadata.json", NULL);
        found = g_file_test(metadata, G_FILE_TEST_IS_REGULAR);
        g_free(metadata);
    }
    return found;
}
/* Add the extension to Cinnamon's list; whether the setting took the new list. */
static bool list_cinnamon_extension(GSettings *settings, char **enabled) {
    GStrvBuilder *builder = g_strv_builder_new();
    g_strv_builder_addv(builder, (const char **)enabled);
    g_strv_builder_add(builder, WINDOWS_SHELL_EXTENSION);
    char **updated = g_strv_builder_end(builder);
    g_strv_builder_unref(builder);
    bool listed =
        g_settings_set_strv(settings, CINNAMON_ENABLED_KEY, (const char *const *)updated) != 0;
    g_strfreev(updated);
    return listed;
}
void windows_shell_enable_cinnamon_once(GSettings *settings, const char *const *data_dirs,
                                        const char *marker) {
    if (settings == NULL || marker == NULL || g_file_test(marker, G_FILE_TEST_EXISTS) ||
        !cinnamon_extension_installed(data_dirs))
        return;
    char **enabled = g_settings_get_strv(settings, CINNAMON_ENABLED_KEY);
    bool listed = g_strv_contains((const char *const *)enabled, WINDOWS_SHELL_EXTENSION) ||
                  list_cinnamon_extension(settings, enabled);
    g_strfreev(enabled);
    if (listed)
        write_marker(marker);
}
