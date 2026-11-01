/* Idle exit must neither tear down launched children nor restart a service. */
#include "../../ui/gtk/lifecycle.h"
#include "test.h"
static GVariant *policy(guint32 main_pid, const char *restart, const char *exit_type,
                        const char *kill_mode) {
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&properties, "{sv}", "MainPID", g_variant_new_uint32(main_pid));
    g_variant_builder_add(&properties, "{sv}", "Type", g_variant_new_string("exec"));
    g_variant_builder_add(&properties, "{sv}", "WatchdogUSec", g_variant_new_uint64(0));
    GVariant *empty = g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0);
    g_variant_builder_add(
        &properties, "{sv}", "RestartForceExitStatus",
        g_variant_new("(@ai@ai)", empty, g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0)));
    if (restart != NULL)
        g_variant_builder_add(&properties, "{sv}", "Restart", g_variant_new_string(restart));
    if (exit_type != NULL)
        g_variant_builder_add(&properties, "{sv}", "ExitType", g_variant_new_string(exit_type));
    if (kill_mode != NULL)
        g_variant_builder_add(&properties, "{sv}", "KillMode", g_variant_new_string(kill_mode));
    return g_variant_ref_sink(g_variant_builder_end(&properties));
}
static bool allowed(guint32 main_pid, const char *restart, const char *exit_type,
                    const char *kill_mode) {
    GVariant *value = policy(main_pid, restart, exit_type, kill_mode);
    bool result = lifecycle_service_allows_exit(value, 42);
    g_variant_unref(value);
    return result;
}
static void reject_override(const char *key, GVariant *override) {
    GVariant *base = policy(42, "no", "cgroup", "control-group");
    GVariantDict properties;
    g_variant_dict_init(&properties, base);
    g_variant_dict_insert_value(&properties, key, override);
    GVariant *value = g_variant_ref_sink(g_variant_dict_end(&properties));
    CHECK(!lifecycle_service_allows_exit(value, 42));
    g_variant_unref(value);
    g_variant_unref(base);
}
void test_lifecycle(void) {
    CHECK(lifecycle_in_service(NULL));
    CHECK(!lifecycle_in_service("0::/\n"));
    CHECK(!lifecycle_in_service("0::/user.slice/user@1000.service/app.slice/app-foo.scope\n"));
    CHECK(lifecycle_in_service("0::/user.slice/user@1000.service/app.slice/popup.service\n"));
    CHECK(lifecycle_in_service("0::/app.slice/popup.service/subgroup\n"));
    CHECK(lifecycle_in_service("1:name=systemd:/app.slice/popup.service\n2:cpu:/\n"));
    CHECK(!lifecycle_service_allows_exit(NULL, 42));
    CHECK(!allowed(0, "no", "cgroup", "control-group"));
    CHECK(allowed(41, NULL, NULL, NULL));
    CHECK(!allowed(42, NULL, "cgroup", "control-group"));
    CHECK(!allowed(42, "no", "main", "control-group"));
    CHECK(!allowed(42, "no", "main", "mixed"));
    CHECK(allowed(42, "no", "cgroup", "control-group"));
    CHECK(allowed(42, "on-failure", "cgroup", "control-group"));
    CHECK(!allowed(42, "on-abnormal", "main", "process"));
    CHECK(!allowed(42, "on-abort", NULL, "none"));
    CHECK(!allowed(42, "always", "cgroup", "process"));
    CHECK(!allowed(42, "on-success", "cgroup", "none"));
    CHECK(!allowed(42, "future-policy", "cgroup", "none"));
    CHECK(!allowed(42, "no", NULL, NULL));
    reject_override("Type", g_variant_new_string("dbus"));
    reject_override("WatchdogUSec", g_variant_new_uint64(1000000));
    gint32 zero = 0;
    reject_override(
        "RestartForceExitStatus",
        g_variant_new("(@ai@ai)",
                      g_variant_new_fixed_array(G_VARIANT_TYPE_INT32, &zero, 1, sizeof(zero)),
                      g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0)));
    CHECK(lifecycle_can_exit(NULL, NULL) == TL_INVALID);
}
