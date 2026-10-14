/* KWin script window source against a fake KWin on a private bus: listing in KWin's order,
 * the launcher's own window and malformed entries left out, Plasma 5's script paths and
 * lower-cased classes, a script name left loaded, activation, a silent KWin, a forged
 * report and nonsense, with every script unloaded afterwards. */
#include "../../ui/gtk/windows_kwin.h"
#include "test.h"
#include "test_bus.h"
#include <glib/gstdio.h>
#include <string.h>
#define SCRIPTING_XML                                                                              \
    "<node><interface name='org.kde.kwin.Scripting'>"                                              \
    "<method name='loadScript'><arg type='s' direction='in'/><arg type='s' direction='in'/>"       \
    "<arg type='i' direction='out'/></method>"                                                     \
    "<method name='unloadScript'><arg type='s' direction='in'/><arg type='b' direction='out'/>"    \
    "</method></interface></node>"
#define SCRIPT_XML                                                                                 \
    "<node><interface name='org.kde.kwin.Script'><method name='run'/></interface></node>"
/* KWin's windows as its scripts would report them, topmost first. */
#define NOTES_ID "{a1b2c3d4-0000-4000-8000-000000000001}"
#define BROWSER_ID "{0e0f1011-1213-4415-9617-18191a1b1c1d}"
#define WINDOWS_REPORT                                                                             \
    "[[\"" NOTES_ID "\",\"Shopping\\nlist\",\"org.example.Notes\",\"org.example.Notes\","          \
    "\"notes\"],"                                                                                  \
    "[\"{b0000000-0000-4000-8000-000000000002}\",\"Torchlight\",\"org.torchlight.Launcher\","      \
    "\"org.torchlight.Launcher\",\"torchlight-gtk\"],"                                             \
    "[\"evil'); quit(); ('\",\"Bad id\",\"\",\"\",\"\"],"                                          \
    "[\"{c0000000-0000-4000-8000-000000000003}\",\"Short\"],"                                      \
    "[\"" BROWSER_ID "\",\"Docs \\u2014 Mozilla Firefox\",\"/usr/share/applications/"              \
    "firefox.desktop\",\"firefox\",\"Navigator\"]]"
/* How the fake behaves. */
typedef enum {
    KWIN_PLASMA6,  /* scripts at /Scripting/Script<id> */
    KWIN_PLASMA5,  /* scripts at /<id> */
    KWIN_SILENT,   /* runs scripts that never report */
    KWIN_IMPOSTOR, /* reports from another connection than KWin's */
    KWIN_NONSENSE  /* reports text that is not a window list */
} kwin_mode;
struct fake_kwin {
    GMutex lock;
    GCond ready;
    GMainContext *context;
    GMainLoop *loop;
    GThread *thread;
    const char *address;
    kwin_mode mode;
    bool started, refuse_once;
    GDBusConnection *bus, *impostor;
    GDBusNodeInfo *script_node;
    /* The loaded script: its name, file and object, and what it was run with. */
    char *name, *path;
    guint object;
    int next_id, loads, runs;
    char activated[64];
};
/* The quoted strings of the callDBus line in a script: name, path, interface, method, token. */
static void report_target(const char *script, char *name, char *token, size_t capacity) {
    const char *call = strstr(script, "callDBus(");
    CHECK(call != NULL);
    char **parts = g_strsplit(call, "'", 12);
    CHECK(g_strv_length(parts) >= 11 && strlen(parts[1]) < capacity && strlen(parts[9]) < capacity);
    memcpy(name, parts[1], strlen(parts[1]) + 1);
    memcpy(token, parts[9], strlen(parts[9]) + 1);
    g_strfreev(parts);
}
/* What the script reports: the window list, or whether it found the window to activate. */
static const char *script_report(struct fake_kwin *kwin, const char *script) {
    if (kwin->mode == KWIN_NONSENSE)
        return "{\"not\": \"a list\"}";
    const char *target = strstr(script, "!== '");
    if (target == NULL)
        return WINDOWS_REPORT;
    target += strlen("!== '");
    const char *end = strchr(target, '\'');
    CHECK(end != NULL && (size_t)(end - target) < sizeof(kwin->activated));
    bool open = strncmp(target, NOTES_ID, strlen(NOTES_ID)) == 0 ||
                strncmp(target, BROWSER_ID, strlen(BROWSER_ID)) == 0;
    if (open) {
        memcpy(kwin->activated, target, (size_t)(end - target));
        kwin->activated[end - target] = 0;
    }
    return open ? "1" : "0";
}
/* Run the loaded script as KWin would: report to the launcher with callDBus. */
static void run_script(struct fake_kwin *kwin) {
    kwin->runs++;
    char *script = NULL;
    CHECK(g_file_get_contents(kwin->path, &script, NULL, NULL));
    char name[64], token[64];
    report_target(script, name, token, sizeof(name));
    if (kwin->mode != KWIN_SILENT) {
        GDBusConnection *sender = kwin->mode == KWIN_IMPOSTOR ? kwin->impostor : kwin->bus;
        g_dbus_connection_call(sender, name, "/org/torchlight/KWin", "org.torchlight.KWin",
                               "Report", g_variant_new("(ss)", token, script_report(kwin, script)),
                               NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
    g_free(script);
}
static GVariant *load_script(struct fake_kwin *kwin, GVariant *parameters,
                             const GDBusInterfaceVTable *script_vtable) {
    const char *path = NULL, *name = NULL;
    g_variant_get(parameters, "(&s&s)", &path, &name);
    kwin->loads++;
    /* A name still loaded is refused, as KWin does. */
    if (kwin->name != NULL || kwin->refuse_once) {
        kwin->refuse_once = false;
        if (kwin->name == NULL)
            kwin->name = g_strdup(name);
        return g_variant_new("(i)", -1);
    }
    int id = kwin->next_id++;
    char *object = kwin->mode == KWIN_PLASMA5 ? g_strdup_printf("/%d", id)
                                              : g_strdup_printf("/Scripting/Script%d", id);
    kwin->object = g_dbus_connection_register_object(
        kwin->bus, object, kwin->script_node->interfaces[0], script_vtable, kwin, NULL, NULL);
    CHECK(kwin->object != 0);
    g_free(object);
    kwin->name = g_strdup(name);
    kwin->path = g_strdup(path);
    return g_variant_new("(i)", id);
}
static GVariant *unload_script(struct fake_kwin *kwin, GVariant *parameters) {
    const char *name = NULL;
    g_variant_get(parameters, "(&s)", &name);
    bool loaded = g_strcmp0(kwin->name, name) == 0;
    if (loaded) {
        if (kwin->object != 0)
            CHECK(g_dbus_connection_unregister_object(kwin->bus, kwin->object));
        kwin->object = 0;
        g_clear_pointer(&kwin->name, g_free);
        g_clear_pointer(&kwin->path, g_free);
    }
    return g_variant_new("(b)", loaded);
}
static void handle_method(GDBusConnection *bus, const char *sender, const char *path,
                          const char *interface, const char *method, GVariant *parameters,
                          GDBusMethodInvocation *invocation, gpointer data);
static const GDBusInterfaceVTable VTABLE = {.method_call = handle_method};
static void handle_method(GDBusConnection *bus, const char *sender, const char *path,
                          const char *interface, const char *method, GVariant *parameters,
                          GDBusMethodInvocation *invocation, gpointer data) {
    (void)bus;
    (void)sender;
    (void)path;
    (void)interface;
    struct fake_kwin *kwin = data;
    GVariant *reply = NULL;
    g_mutex_lock(&kwin->lock);
    if (strcmp(method, "loadScript") == 0)
        reply = load_script(kwin, parameters, &VTABLE);
    else if (strcmp(method, "unloadScript") == 0)
        reply = unload_script(kwin, parameters);
    else if (strcmp(method, "run") == 0)
        run_script(kwin);
    g_mutex_unlock(&kwin->lock);
    g_dbus_method_invocation_return_value(invocation, reply);
}
static gpointer serve(gpointer data) {
    struct fake_kwin *kwin = data;
    g_main_context_push_thread_default(kwin->context);
    kwin->bus = test_bus_connect(kwin->address);
    kwin->impostor = test_bus_connect(kwin->address);
    kwin->script_node = g_dbus_node_info_new_for_xml(SCRIPT_XML, NULL);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(SCRIPTING_XML, NULL);
    CHECK(node != NULL && kwin->script_node != NULL);
    CHECK(g_dbus_connection_register_object(kwin->bus, "/Scripting", node->interfaces[0], &VTABLE,
                                            kwin, NULL, NULL) != 0);
    g_dbus_node_info_unref(node);
    test_bus_own(kwin->bus, WINDOWS_KWIN_NAME);
    g_mutex_lock(&kwin->lock);
    kwin->started = true;
    g_cond_signal(&kwin->ready);
    g_mutex_unlock(&kwin->lock);
    g_main_loop_run(kwin->loop);
    CHECK(g_dbus_connection_close_sync(kwin->impostor, NULL, NULL));
    CHECK(g_dbus_connection_close_sync(kwin->bus, NULL, NULL));
    g_object_unref(kwin->impostor);
    g_object_unref(kwin->bus);
    g_dbus_node_info_unref(kwin->script_node);
    g_main_context_pop_thread_default(kwin->context);
    return NULL;
}
static void start_kwin(struct fake_kwin *kwin, const char *address, kwin_mode mode) {
    g_mutex_init(&kwin->lock);
    g_cond_init(&kwin->ready);
    kwin->address = address;
    kwin->mode = mode;
    kwin->context = g_main_context_new();
    kwin->loop = g_main_loop_new(kwin->context, false);
    kwin->thread = g_thread_new("fake-kwin", serve, kwin);
    g_mutex_lock(&kwin->lock);
    while (!kwin->started)
        g_cond_wait(&kwin->ready, &kwin->lock);
    g_mutex_unlock(&kwin->lock);
}
static void stop_kwin(struct fake_kwin *kwin) {
    g_main_loop_quit(kwin->loop);
    g_thread_join(kwin->thread);
    g_main_loop_unref(kwin->loop);
    g_main_context_unref(kwin->context);
    g_cond_clear(&kwin->ready);
    g_mutex_clear(&kwin->lock);
    g_free(kwin->name);
    g_free(kwin->path);
}
/* Whether every script the source loaded was unloaded again. */
static bool nothing_loaded(struct fake_kwin *kwin) {
    g_mutex_lock(&kwin->lock);
    bool unloaded = kwin->name == NULL;
    g_mutex_unlock(&kwin->lock);
    return unloaded;
}
static void check_listing(const char *address, const char *folder, kwin_mode mode) {
    struct fake_kwin kwin = {0};
    start_kwin(&kwin, address, mode);
    tl_window_source source;
    CHECK(windows_kwin_source(address, folder, &source) == TL_OK);
    tl_windows *windows = NULL;
    CHECK(windows_create(&source, &windows) == TL_OK);
    CHECK(windows_refresh(windows) == TL_OK && nothing_loaded(&kwin));
    /* KWin's order stands; the launcher, an unsafe id and a short entry are left out. */
    CHECK(windows_count(windows) == 2);
    const tl_window *notes = windows_get(windows, 0), *browser = windows_get(windows, 1);
    /* The handle is the id's first 64 bits. */
    CHECK(strcmp(notes->title, "Shopping list") == 0 &&
          notes->handle == UINT64_C(0xa1b2c3d400004000));
    CHECK(windows_evidence(notes, "org.example.Notes.desktop", NULL) == WINDOWS_EVIDENCE_APP_ID);
    /* A desktop file given as a path still names the application. */
    CHECK(strcmp(browser->app_id, "firefox") == 0 && strcmp(browser->instance, "Navigator") == 0);
    CHECK(strcmp(browser->title, "Docs \xe2\x80\x94 Mozilla Firefox") == 0);
    /* Plasma 5 lower-cases X11 classes, so StartupWMClass is compared ignoring case. */
    CHECK(browser->fold_case == (mode == KWIN_PLASMA5));
    CHECK(windows_activate(windows, 1) == TL_OK && nothing_loaded(&kwin));
    g_mutex_lock(&kwin.lock);
    CHECK(strcmp(kwin.activated, BROWSER_ID) == 0);
    g_mutex_unlock(&kwin.lock);
    /* A handle never listed is refused without asking KWin. */
    CHECK(source.activate(source.context, 42) == TL_STATE);
    windows_destroy(windows);
    stop_kwin(&kwin);
}
/* A name left loaded by an earlier call is unloaded, and loading tried again. */
static void check_reload(const char *address, const char *folder) {
    struct fake_kwin kwin = {.refuse_once = true};
    start_kwin(&kwin, address, KWIN_PLASMA6);
    tl_window_source source;
    CHECK(windows_kwin_source(address, folder, &source) == TL_OK);
    tl_window out[WINDOWS_MAX];
    size_t count = 0;
    CHECK(source.list(source.context, out, WINDOWS_MAX, &count) == TL_OK && count == 2);
    CHECK(kwin.loads == 2 && nothing_loaded(&kwin));
    source.destroy(source.context);
    stop_kwin(&kwin);
}
/* KWin that does not answer, forged answers and nonsense all list nothing, in bounded time. */
static void check_failures(const char *address, const char *folder) {
    kwin_mode modes[] = {KWIN_SILENT, KWIN_IMPOSTOR, KWIN_NONSENSE};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        struct fake_kwin kwin = {0};
        start_kwin(&kwin, address, modes[i]);
        tl_window_source source;
        CHECK(windows_kwin_source(address, folder, &source) == TL_OK);
        tl_window out[WINDOWS_MAX];
        size_t count = 7;
        gint64 start = g_get_monotonic_time();
        CHECK(source.list(source.context, out, WINDOWS_MAX, &count) == TL_IO && count == 0);
        gint64 waited = (g_get_monotonic_time() - start) / G_TIME_SPAN_MILLISECOND;
        CHECK(waited < 4 * WINDOWS_KWIN_TIMEOUT_MS && nothing_loaded(&kwin));
        source.destroy(source.context);
        stop_kwin(&kwin);
    }
}
void test_windows_kwin(void) {
    char address[512];
    GPid bus = test_bus_start(address, sizeof(address));
    char *folder = g_dir_make_tmp("torchlight-kwin-XXXXXX", NULL);
    /* No KWin on the bus: nothing to list, and no wait. */
    tl_window_source source;
    CHECK(windows_kwin_source(address, folder, &source) == TL_OK);
    tl_window out[2];
    size_t count = 1;
    CHECK(source.list(source.context, out, 2, &count) == TL_IO && count == 0);
    source.destroy(source.context);
    CHECK(windows_kwin_source(address, folder, NULL) == TL_INVALID);
    check_listing(address, folder, KWIN_PLASMA6);
    check_listing(address, folder, KWIN_PLASMA5);
    check_reload(address, folder);
    check_failures(address, folder);
    /* The source removes its script file when destroyed. */
    CHECK(g_rmdir(folder) == 0);
    g_free(folder);
    test_bus_stop(bus);
}
