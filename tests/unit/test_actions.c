/* Worker action regressions: byte argv, native %k expansion and stale revisions. */
#include "../../ui/gtk/actions.h"
#include "test.h"
#include "torchlight/hashmap.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct action_result {
    bool done, accepted;
};
static void completed(GObject *source, GAsyncResult *result, gpointer context) {
    (void)source;
    struct action_result *state = context;
    GError *error = NULL;
    state->accepted = g_task_propagate_boolean(G_TASK(result), &error);
    g_clear_error(&error);
    state->done = true;
}
static bool run_action(struct launch *launch) {
    struct action_result state = {0};
    GCancellable *cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancel, completed, &state);
    g_task_set_task_data(task, launch, NULL);
    g_task_run_in_thread(task, actions_worker);
    while (!state.done)
        g_main_context_iteration(NULL, true);
    g_object_unref(task);
    g_object_unref(cancel);
    return state.accepted;
}
static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    CHECK(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}
static uint64_t revision(const char *path) {
    GKeyFile *file = g_key_file_new();
    CHECK(g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL));
    gsize length = 0;
    char *data = g_key_file_to_data(file, &length, NULL);
    uint64_t hash = hashmap_hash(HASHMAP_HASH_SEED, path, strlen(path));
    hash = hashmap_hash(hash, data, length);
    g_free(data);
    g_key_file_unref(file);
    return hash;
}
static void expect_marker(const char *path, const char *expected) {
    char bytes[1024] = {0};
    struct timespec pause = {.tv_nsec = 10000000};
    for (size_t i = 0; i < 200; i++) {
        FILE *file = fopen(path, "r");
        if (file != NULL) {
            size_t length = fread(bytes, 1, sizeof(bytes) - 1, file);
            CHECK(fclose(file) == 0);
            if (length == strlen(expected)) {
                CHECK(strcmp(bytes, expected) == 0);
                return;
            }
        }
        nanosleep(&pause, NULL);
    }
    CHECK(false);
}
struct file_manager {
    const char *path;
    size_t requests;
};
static void show_items(GDBusConnection *connection, const gchar *sender, const gchar *object,
                       const gchar *interface, const gchar *method, GVariant *parameters,
                       GDBusMethodInvocation *invocation, gpointer context) {
    (void)connection;
    (void)sender;
    (void)object;
    (void)interface;
    struct file_manager *manager = context;
    CHECK(strcmp(method, "ShowItems") == 0);
    char **uris = NULL, *startup = NULL;
    g_variant_get(parameters, "(^ass)", &uris, &startup);
    CHECK(uris[0] != NULL && uris[1] == NULL);
    GError *error = NULL;
    char *path = g_filename_from_uri(uris[0], NULL, &error);
    CHECK(error == NULL && path != NULL && strcmp(path, manager->path) == 0);
    manager->requests++;
    g_free(path);
    g_free(startup);
    g_strfreev(uris);
    g_dbus_method_invocation_return_value(invocation, NULL);
}
static GSubprocess *start_bus(char **saved_address) {
    GError *error = NULL;
    /* An explicit daemon avoids GTestDBus's forked watchdog inheriting the
     * already-running GIO threads and producing false child-process leaks. */
    GSubprocess *bus = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "dbus-daemon",
                                        "--session", "--nofork", "--print-address=1", NULL);
    CHECK(bus != NULL && error == NULL);
    GDataInputStream *output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(bus));
    char *address = g_data_input_stream_read_line(output, NULL, NULL, &error);
    CHECK(address != NULL && error == NULL);
    const char *old_address = getenv("DBUS_SESSION_BUS_ADDRESS");
    *saved_address = old_address == NULL ? NULL : strdup(old_address);
    CHECK(old_address == NULL || *saved_address != NULL);
    CHECK(setenv("DBUS_SESSION_BUS_ADDRESS", address, 1) == 0);
    g_free(address);
    g_object_unref(output);
    return bus;
}
static void stop_bus(GSubprocess *bus, char *saved_address) {
    GError *error = NULL;
    g_subprocess_force_exit(bus);
    CHECK(g_subprocess_wait(bus, NULL, &error) && error == NULL);
    g_object_unref(bus);
    if (saved_address != NULL)
        CHECK(setenv("DBUS_SESSION_BUS_ADDRESS", saved_address, 1) == 0);
    else
        CHECK(unsetenv("DBUS_SESSION_BUS_ADDRESS") == 0);
    free(saved_address);
}
static void test_reveal(struct launch *launch, const char *marker) {
    char *saved_address = NULL;
    GSubprocess *bus = start_bus(&saved_address);
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    CHECK(connection != NULL && error == NULL);
    GVariant *reply = g_dbus_connection_call_sync(
        connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new("(su)", "org.freedesktop.FileManager1", 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, IPC_DEADLINE_MS, NULL, &error);
    CHECK(reply != NULL && error == NULL);
    g_variant_unref(reply);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.freedesktop.FileManager1'><method name='ShowItems'>"
        "<arg type='as' direction='in'/><arg type='s' direction='in'/>"
        "</method></interface></node>",
        &error);
    CHECK(node != NULL && error == NULL);
    const GDBusInterfaceVTable table = {.method_call = show_items};
    struct file_manager manager = {launch->row.path, 0};
    guint registration =
        g_dbus_connection_register_object(connection, "/org/freedesktop/FileManager1",
                                          node->interfaces[0], &table, &manager, NULL, &error);
    CHECK(registration != 0 && error == NULL);
    launch->reveal = true;
    CHECK(run_action(launch));
    CHECK(manager.requests == 1);
    expect_marker(marker, "/tmp");
    CHECK(unlink(marker) == 0);
    CHECK(g_dbus_connection_unregister_object(connection, registration));
    g_dbus_node_info_unref(node);
    CHECK(g_dbus_connection_close_sync(connection, NULL, &error) && error == NULL);
    g_object_unref(connection);
    stop_bus(bus, saved_address);
    launch->reveal = false;
}
void test_actions(void) {
    char directory[] = "/tmp/torchlight-actions-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char script[256], marker[256], desktop[256], contents[1024];
    CHECK(snprintf(script, sizeof(script), "%s/xdg-open", directory) > 0);
    CHECK(snprintf(marker, sizeof(marker), "%s/marker", directory) > 0);
    CHECK(snprintf(desktop, sizeof(desktop), "%s/fixture.desktop", directory) > 0);
    CHECK(snprintf(contents, sizeof(contents), "#!/bin/sh\nprintf '%%s' \"$1\" > '%s'\n", marker) >
          0);
    write_text(script, contents);
    CHECK(chmod(script, 0700) == 0);
    const char *old_path = getenv("PATH");
    char *saved_path = old_path == NULL ? NULL : strdup(old_path);
    char launch_path[512];
    CHECK(snprintf(launch_path, sizeof(launch_path), "%s:/usr/bin:/bin", directory) > 0);
    CHECK(setenv("PATH", launch_path, 1) == 0);
    struct launch launch = {0};
    const char *raw = "/tmp/quote ' $(touch SHOULD_NOT_EXIST) \xff\n";
    memcpy(launch.row.path, raw, strlen(raw) + 1);
    CHECK(run_action(&launch));
    expect_marker(marker, raw);
    CHECK(unlink(marker) == 0);
    test_reveal(&launch, marker);
    CHECK(snprintf(contents, sizeof(contents),
                   "[Desktop Entry]\nType=Application\nName=Fixture\nExec=%s %%k\n", script) > 0);
    write_text(desktop, contents);
    memcpy(launch.row.path, desktop, strlen(desktop) + 1);
    launch.row.application = true;
    launch.row.desktop_revision = revision(desktop);
    CHECK(run_action(&launch));
    expect_marker(marker, desktop);
    CHECK(unlink(marker) == 0);
    write_text(desktop, "[Desktop Entry]\nType=Application\nName=Replacement\nExec=/bin/true\n");
    CHECK(!run_action(&launch));
    CHECK(access(marker, F_OK) != 0);
    if (saved_path != NULL)
        CHECK(setenv("PATH", saved_path, 1) == 0);
    else
        CHECK(unsetenv("PATH") == 0);
    free(saved_path);
    CHECK(unlink(script) == 0 && unlink(desktop) == 0 && rmdir(directory) == 0);
}
