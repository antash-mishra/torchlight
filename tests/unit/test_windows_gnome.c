/* GNOME Shell window source against a fake GNOME Shell on a private bus: listing order,
 * the launcher's own window left out, cleaned titles, activation, an absent extension, and
 * enabling the extension once. */
#include "../../ui/gtk/windows_gnome.h"
#include "test.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
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
/* A stand-in for GNOME Shell with Torchlight's extension, served on its own thread. */
struct fake_shell {
    GMutex lock;
    GCond ready;
    GMainContext *context;
    GMainLoop *loop;
    GThread *thread;
    const char *address;
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
static void own_name(GDBusConnection *bus, const char *name) {
    GVariant *reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", name, 0), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
        NULL);
    CHECK(reply != NULL);
    g_variant_unref(reply);
}
static GDBusConnection *connect_bus(const char *address) {
    GDBusConnection *bus =
        g_dbus_connection_new_for_address_sync(address,
                                               G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                               NULL, NULL, NULL);
    CHECK(bus != NULL);
    return bus;
}
static gpointer serve(gpointer data) {
    struct fake_shell *shell = data;
    g_main_context_push_thread_default(shell->context);
    GDBusConnection *bus = connect_bus(shell->address);
    if (shell->windows_exported)
        export(bus, shell, WINDOWS_XML, "/org/torchlight/Windows");
    /* As GNOME Shell 46 serves it: on its own name, at /org/gnome/Shell. */
    export(bus, shell, EXTENSIONS_XML, "/org/gnome/Shell");
    own_name(bus, "org.gnome.Shell");
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
static void start_shell(struct fake_shell *shell, const char *address) {
    g_mutex_init(&shell->lock);
    g_cond_init(&shell->ready);
    shell->address = address;
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
    CHECK(windows_gnome_source(client, &source) == TL_OK);
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
    start_shell(&shell, address);
    GDBusConnection *client = connect_bus(address);
    tl_window_source source;
    CHECK(windows_gnome_source(client, &source) == TL_OK);
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
    windows_gnome_enable_once(client, marker);
    CHECK(enable_calls(shell) == 0 && !g_file_test(marker, G_FILE_TEST_EXISTS));
    /* Found, but the person switched extensions off: left alone. */
    set_extension(shell, true, false, false);
    windows_gnome_enable_once(client, marker);
    CHECK(enable_calls(shell) == 0 && !g_file_test(marker, G_FILE_TEST_EXISTS));
    set_extension(shell, true, false, true);
    windows_gnome_enable_once(client, marker);
    CHECK(enable_calls(shell) == 1 && g_file_test(marker, G_FILE_TEST_EXISTS));
    /* Once enabled, a person's own later disable stands. */
    set_extension(shell, true, false, true);
    windows_gnome_enable_once(client, marker);
    CHECK(enable_calls(shell) == 1);
    CHECK(g_remove(marker) == 0);
    char *state = g_path_get_dirname(marker);
    CHECK(g_rmdir(state) == 0 && g_rmdir(folder) == 0);
    g_free(state);
    g_free(marker);
    g_free(folder);
}
/* A private session bus: dbus-daemon printing its address on a pipe. It is spawned and
 * reaped directly, so GLib starts no child-watch thread that would outlive the test. */
static GPid start_bus(char *address, size_t capacity) {
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
static void stop_bus(GPid pid) {
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, NULL, 0) == pid);
    g_spawn_close_pid(pid);
}
void test_windows_gnome(void) {
    CHECK(windows_gnome_desktop("ubuntu:GNOME") && windows_gnome_desktop("gnome"));
    CHECK(!windows_gnome_desktop("X-Cinnamon") && !windows_gnome_desktop(NULL));
    CHECK(!windows_gnome_desktop("GNOME-Flashback") &&
          windows_gnome_desktop("GNOME-Flashback:GNOME"));
    char address[512];
    GPid bus = start_bus(address, sizeof(address));
    check_absent(address);
    struct fake_shell shell = {.windows_exported = true};
    start_shell(&shell, address);
    GDBusConnection *client = connect_bus(address);
    check_listing(client, &shell);
    check_enable_once(client, &shell);
    CHECK(g_dbus_connection_close_sync(client, NULL, NULL));
    g_object_unref(client);
    stop_shell(&shell);
    stop_bus(bus);
}
