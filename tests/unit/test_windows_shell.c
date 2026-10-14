/* Shell extension window source against a fake GNOME Shell and a fake Cinnamon on a private
 * bus: listing order, the launcher's own window left out, cleaned titles, activation, an
 * absent extension, telling which shell runs, and enabling the extension once in each. */
#include "../../ui/gtk/windows_shell.h"
#include "test.h"
#include "test_bus.h"
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gio.h>
#include <gio/gsettingsbackend.h>
#include <glib/gstdio.h>
#include <string.h>
#define WINDOWS_XML                                                                                \
    "<node><interface name='org.torchlight.Windows'>"                                              \
    "<method name='List'><arg type='a(tssss)' direction='out'/></method>"                          \
    "<method name='Activate'><arg type='t' direction='in'/><arg type='b' direction='out'/>"        \
    "</method></interface></node>"
#define EXTENSIONS_XML                                                                             \
    "<node><interface name='org.gnome.Shell.Extensions'>"                                          \
    "<method name='GetExtensionInfo'><arg type='s' direction='in'/>"                               \
    "<arg type='a{sv}' direction='out'/></method>"                                                 \
    "<method name='EnableExtension'><arg type='s' direction='in'/>"                                \
    "<arg type='b' direction='out'/></method>"                                                     \
    "<property name='UserExtensionsEnabled' type='b' access='read'/>"                              \
    "</interface></node>"
/* A stand-in for GNOME Shell or Cinnamon with Torchlight's extension, served on its own
 * thread under the shell's bus name. */
struct fake_shell {
    GMutex lock;
    GCond ready;
    GMainContext *context;
    GMainLoop *loop;
    GThread *thread;
    const char *address, *name;
    bool started, windows_exported, known, enabled, user_extensions;
    int enable_calls;
    guint64 activated;
};
static GVariant *list_reply(void) {
    GVariantBuilder windows;
    g_variant_builder_init(&windows, G_VARIANT_TYPE("a(tssss)"));
    char long_name[WINDOWS_CLASS_BYTES + 8];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    g_variant_builder_add(&windows, "(tssss)", (guint64)7, "gnome-terminal-server",
                          "Gnome-terminal", "org.gnome.Terminal", "~/work\nnotes");
    g_variant_builder_add(&windows, "(tssss)", (guint64)0, "", "zero", "", "No handle");
    g_variant_builder_add(&windows, "(tssss)", (guint64)11, "org.torchlight.Launcher",
                          "org.torchlight.Launcher", "", "Torchlight");
    g_variant_builder_add(&windows, "(tssss)", (guint64)9, long_name, "firefox", "firefox_firefox",
                          "Docs \xe2\x80\x94 Mozilla Firefox");
    return g_variant_new("(a(tssss))", &windows);
}
static GVariant *extension_info(const struct fake_shell *shell) {
    GVariantBuilder info;
    g_variant_builder_init(&info, G_VARIANT_TYPE("a{sv}"));
    if (shell->known) {
        g_variant_builder_add(&info, "{sv}", "state", g_variant_new_double(shell->enabled ? 1 : 2));
        g_variant_builder_add(&info, "{sv}", "enabled", g_variant_new_boolean(shell->enabled));
    }
    return g_variant_new("(a{sv})", &info);
}
static void handle_method(GDBusConnection *bus, const char *sender, const char *path,
                          const char *interface, const char *method, GVariant *parameters,
                          GDBusMethodInvocation *invocation, gpointer data) {
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    struct fake_shell *shell = data;
    GVariant *reply = NULL;
    g_mutex_lock(&shell->lock);
    if (strcmp(method, "List") == 0)
        reply = list_reply();
    else if (strcmp(method, "Activate") == 0) {
        guint64 id = 0;
        g_variant_get(parameters, "(t)", &id);
        bool open = id == 7 || id == 9;
        if (open)
            shell->activated = id;
        reply = g_variant_new("(b)", open);
    } else if (strcmp(method, "GetExtensionInfo") == 0)
        reply = extension_info(shell);
    else if (strcmp(method, "EnableExtension") == 0) {
        shell->enable_calls++;
        shell->enabled = true;
        reply = g_variant_new("(b)", true);
    }
    g_mutex_unlock(&shell->lock);
    g_dbus_method_invocation_return_value(invocation, reply);
}
static GVariant *get_property(GDBusConnection *bus, const char *sender, const char *path,
                              const char *interface, const char *property, GError **error,
                              gpointer data) {
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    (void)property;
    (void)error;
    struct fake_shell *shell = data;
    g_mutex_lock(&shell->lock);
    GVariant *value = g_variant_new_boolean(shell->user_extensions);
    g_mutex_unlock(&shell->lock);
    return value;
}
static const GDBusInterfaceVTable VTABLE = {.method_call = handle_method,
                                            .get_property = get_property};
static void export(GDBusConnection *bus, struct fake_shell *shell, const char *xml,
                   const char *path) {
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
    CHECK(node != NULL);
    CHECK(g_dbus_connection_register_object(bus, path, node->interfaces[0], &VTABLE, shell, NULL,
                                            NULL) != 0);
    g_dbus_node_info_unref(node);
}
static gpointer serve(gpointer data) {
    struct fake_shell *shell = data;
    g_main_context_push_thread_default(shell->context);
    GDBusConnection *bus = test_bus_connect(shell->address);
    if (shell->windows_exported)
        export(bus, shell, WINDOWS_XML, "/org/torchlight/Windows");
    /* As GNOME Shell 46 serves it: on its own name, at /org/gnome/Shell. */
    export(bus, shell, EXTENSIONS_XML, "/org/gnome/Shell");
    test_bus_own(bus, shell->name);
    g_mutex_lock(&shell->lock);
    shell->started = true;
    g_cond_signal(&shell->ready);
    g_mutex_unlock(&shell->lock);
    g_main_loop_run(shell->loop);
    CHECK(g_dbus_connection_close_sync(bus, NULL, NULL));
    g_object_unref(bus);
    g_main_context_pop_thread_default(shell->context);
    return NULL;
}
static void start_shell(struct fake_shell *shell, const char *address, const char *name) {
    g_mutex_init(&shell->lock);
    g_cond_init(&shell->ready);
    shell->address = address;
    shell->name = name;
    shell->context = g_main_context_new();
    shell->loop = g_main_loop_new(shell->context, false);
    shell->thread = g_thread_new("fake-gnome-shell", serve, shell);
    g_mutex_lock(&shell->lock);
    while (!shell->started)
        g_cond_wait(&shell->ready, &shell->lock);
    g_mutex_unlock(&shell->lock);
}
static void stop_shell(struct fake_shell *shell) {
    g_main_loop_quit(shell->loop);
    g_thread_join(shell->thread);
    g_main_loop_unref(shell->loop);
    g_main_context_unref(shell->context);
    g_cond_clear(&shell->ready);
    g_mutex_clear(&shell->lock);
}
static void check_listing(GDBusConnection *client, struct fake_shell *shell) {
    tl_window_source source;
    CHECK(windows_shell_source(client, shell->name, &source) == TL_OK);
    tl_windows *windows = NULL;
    CHECK(windows_create(&source, &windows) == TL_OK);
    CHECK(windows_refresh(windows) == TL_OK);
    /* The shell's order stands; no handle and the launcher's own popup are left out. */
    CHECK(windows_count(windows) == 2);
    const tl_window *terminal = windows_get(windows, 0), *browser = windows_get(windows, 1);
    CHECK(terminal->handle == 7 && strcmp(terminal->title, "~/work notes") == 0);
    CHECK(windows_evidence(terminal, "org.gnome.Terminal.desktop", NULL) ==
          WINDOWS_EVIDENCE_APP_ID);
    /* An identifier too long to compare is dropped; the class still matches the app. */
    CHECK(browser->handle == 9 && browser->instance[0] == 0);
    CHECK(windows_evidence(browser, "firefox_firefox.desktop", NULL) == WINDOWS_EVIDENCE_APP_ID);
    CHECK(windows_activate(windows, 1) == TL_OK);
    g_mutex_lock(&shell->lock);
    CHECK(shell->activated == 9);
    g_mutex_unlock(&shell->lock);
    CHECK(source.activate(source.context, 42) == TL_STATE);
    windows_destroy(windows);
}
/* Without the extension running there are no windows, and no waiting. */
static void check_absent(const char *address) {
    struct fake_shell shell = {.windows_exported = false};
    start_shell(&shell, address, WINDOWS_SHELL_GNOME);
    GDBusConnection *client = test_bus_connect(address);
    tl_window_source source;
    CHECK(windows_shell_source(client, WINDOWS_SHELL_GNOME, &source) == TL_OK);
    tl_window out[2];
    size_t count = 1;
    CHECK(source.list(source.context, out, 2, &count) == TL_IO && count == 0);
    source.destroy(source.context);
    CHECK(g_dbus_connection_close_sync(client, NULL, NULL));
    g_object_unref(client);
    stop_shell(&shell);
}
/* Change what the fake shell reports, between calls. */
static void set_extension(struct fake_shell *shell, bool known, bool enabled,
                          bool user_extensions) {
    g_mutex_lock(&shell->lock);
    shell->known = known;
    shell->enabled = enabled;
    shell->user_extensions = user_extensions;
    g_mutex_unlock(&shell->lock);
}
static int enable_calls(struct fake_shell *shell) {
    g_mutex_lock(&shell->lock);
    int calls = shell->enable_calls;
    g_mutex_unlock(&shell->lock);
    return calls;
}
static void check_enable_once(GDBusConnection *client, struct fake_shell *shell) {
    char *folder = g_dir_make_tmp("torchlight-gnome-XXXXXX", NULL);
    char *marker = g_build_filename(folder, "state", "gnome-extension-enabled", NULL);
    /* Not found yet (installed after login): nothing to do. */
    windows_shell_enable_gnome_once(client, marker);
    CHECK(enable_calls(shell) == 0 && !g_file_test(marker, G_FILE_TEST_EXISTS));
    /* Found, but the person switched extensions off: left alone. */
    set_extension(shell, true, false, false);
    windows_shell_enable_gnome_once(client, marker);
    CHECK(enable_calls(shell) == 0 && !g_file_test(marker, G_FILE_TEST_EXISTS));
    set_extension(shell, true, false, true);
    windows_shell_enable_gnome_once(client, marker);
    CHECK(enable_calls(shell) == 1 && g_file_test(marker, G_FILE_TEST_EXISTS));
    /* Once enabled, a person's own later disable stands. */
    set_extension(shell, true, false, true);
    windows_shell_enable_gnome_once(client, marker);
    CHECK(enable_calls(shell) == 1);
    CHECK(g_remove(marker) == 0);
    char *state = g_path_get_dirname(marker);
    CHECK(g_rmdir(state) == 0 && g_rmdir(folder) == 0);
    g_free(state);
    g_free(marker);
    g_free(folder);
}
/* Cinnamon's enabled-extensions key, in a schema compiled for the test. */
#define CINNAMON_SCHEMA "org.torchlight.test.cinnamon"
#define CINNAMON_SCHEMA_XML                                                                        \
    "<schemalist><schema id='" CINNAMON_SCHEMA "' path='/org/torchlight/test/cinnamon/'>"          \
    "<key name='enabled-extensions' type='as'><default>['other@example']</default></key>"          \
    "</schema></schemalist>"
/* Settings held in memory, so the test never touches the person's own. */
static GSettings *cinnamon_settings(const char *folder) {
    char *xml = g_build_filename(folder, CINNAMON_SCHEMA ".gschema.xml", NULL);
    CHECK(g_file_set_contents(xml, CINNAMON_SCHEMA_XML, -1, NULL));
    char *argv[] = {"glib-compile-schemas", (char *)folder, NULL};
    int status = 0;
    CHECK(
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
    CHECK(g_spawn_check_wait_status(status, NULL));
    GSettingsSchemaSource *schemas =
        g_settings_schema_source_new_from_directory(folder, NULL, true, NULL);
    CHECK(schemas != NULL);
    GSettingsSchema *schema = g_settings_schema_source_lookup(schemas, CINNAMON_SCHEMA, false);
    CHECK(schema != NULL);
    GSettingsBackend *backend = g_memory_settings_backend_new();
    GSettings *settings = g_settings_new_full(schema, backend, NULL);
    g_object_unref(backend);
    g_settings_schema_unref(schema);
    g_settings_schema_source_unref(schemas);
    CHECK(g_remove(xml) == 0);
    g_free(xml);
    char *compiled = g_build_filename(folder, "gschemas.compiled", NULL);
    CHECK(g_remove(compiled) == 0);
    g_free(compiled);
    return settings;
}
static bool cinnamon_lists(GSettings *settings, const char *uuid) {
    char **enabled = g_settings_get_strv(settings, "enabled-extensions");
    bool listed = g_strv_contains((const char *const *)enabled, uuid);
    g_strfreev(enabled);
    return listed;
}
static void check_cinnamon_enable_once(void) {
    char *folder = g_dir_make_tmp("torchlight-cinnamon-XXXXXX", NULL);
    GSettings *settings = cinnamon_settings(folder);
    char *data = g_build_filename(folder, "data", NULL);
    const char *const data_dirs[] = {"/nonexistent", data, NULL};
    char *marker = g_build_filename(folder, "state", "cinnamon-extension-enabled", NULL);
    /* Not installed in any data folder: Cinnamon could not load it, so it is not listed. */
    windows_shell_enable_cinnamon_once(settings, data_dirs, marker);
    CHECK(!cinnamon_lists(settings, WINDOWS_SHELL_EXTENSION));
    CHECK(!g_file_test(marker, G_FILE_TEST_EXISTS));
    char *extension =
        g_build_filename(data, "cinnamon", "extensions", WINDOWS_SHELL_EXTENSION, NULL);
    char *metadata = g_build_filename(extension, "metadata.json", NULL);
    CHECK(g_mkdir_with_parents(extension, 0700) == 0 &&
          g_file_set_contents(metadata, "{}", -1, NULL));
    windows_shell_enable_cinnamon_once(settings, data_dirs, marker);
    CHECK(cinnamon_lists(settings, WINDOWS_SHELL_EXTENSION) &&
          cinnamon_lists(settings, "other@example"));
    CHECK(g_file_test(marker, G_FILE_TEST_EXISTS));
    /* Once enabled, a person's own later disable stands. */
    const char *const other[] = {"other@example", NULL};
    CHECK(g_settings_set_strv(settings, "enabled-extensions", other));
    windows_shell_enable_cinnamon_once(settings, data_dirs, marker);
    CHECK(!cinnamon_lists(settings, WINDOWS_SHELL_EXTENSION));
    /* Already listed by the person: only the marker is written, with no duplicate. */
    CHECK(g_remove(marker) == 0);
    const char *const both[] = {WINDOWS_SHELL_EXTENSION, "other@example", NULL};
    CHECK(g_settings_set_strv(settings, "enabled-extensions", both));
    windows_shell_enable_cinnamon_once(settings, data_dirs, marker);
    char **enabled = g_settings_get_strv(settings, "enabled-extensions");
    CHECK(g_strv_length(enabled) == 2 && g_file_test(marker, G_FILE_TEST_EXISTS));
    g_strfreev(enabled);
    g_object_unref(settings);
    char *state = g_path_get_dirname(marker);
    CHECK(g_remove(marker) == 0 && g_rmdir(state) == 0 && g_remove(metadata) == 0);
    char *extensions = g_path_get_dirname(extension), *cinnamon = g_path_get_dirname(extensions);
    CHECK(g_rmdir(extension) == 0 && g_rmdir(extensions) == 0 && g_rmdir(cinnamon) == 0);
    CHECK(g_rmdir(data) == 0 && g_rmdir(folder) == 0);
    g_free(cinnamon);
    g_free(extensions);
    g_free(state);
    g_free(metadata);
    g_free(extension);
    g_free(marker);
    g_free(data);
    g_free(folder);
}
void test_windows_shell(void) {
    CHECK(windows_shell_desktop("ubuntu:GNOME", "GNOME") &&
          windows_shell_desktop("gnome", "GNOME"));
    CHECK(!windows_shell_desktop("X-Cinnamon", "GNOME") && !windows_shell_desktop(NULL, "GNOME"));
    CHECK(!windows_shell_desktop("GNOME-Flashback", "GNOME") &&
          windows_shell_desktop("GNOME-Flashback:GNOME", "GNOME"));
    CHECK(windows_shell_desktop("X-Cinnamon", "X-Cinnamon") && !windows_shell_desktop("KDE", NULL));
    check_cinnamon_enable_once();
    char address[512];
    GPid bus = test_bus_start(address, sizeof(address));
    check_absent(address);
    struct fake_shell gnome = {.windows_exported = true};
    start_shell(&gnome, address, WINDOWS_SHELL_GNOME);
    GDBusConnection *client = test_bus_connect(address);
    CHECK(windows_shell_name_owned(client, WINDOWS_SHELL_GNOME));
    CHECK(!windows_shell_name_owned(client, WINDOWS_SHELL_CINNAMON));
    CHECK(!windows_shell_name_owned(client, NULL));
    check_listing(client, &gnome);
    check_enable_once(client, &gnome);
    /* Cinnamon's extension serves the same interface on Cinnamon's own name. */
    struct fake_shell cinnamon = {.windows_exported = true};
    start_shell(&cinnamon, address, WINDOWS_SHELL_CINNAMON);
    CHECK(windows_shell_name_owned(client, WINDOWS_SHELL_CINNAMON));
    check_listing(client, &cinnamon);
    CHECK(windows_shell_source(client, NULL, &(tl_window_source){0}) == TL_INVALID);
    CHECK(g_dbus_connection_close_sync(client, NULL, NULL));
    g_object_unref(client);
    stop_shell(&cinnamon);
    stop_shell(&gnome);
    test_bus_stop(bus);
}
