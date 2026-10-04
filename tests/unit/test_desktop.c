/* Isolated XDG precedence, visibility, metadata matching and live removal checks. */
#include "test.h"
#include "torchlight/desktop.h"
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static void write_entry(const char *directory, const char *id, const char *fields) {
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, id);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fprintf(file, "[Desktop Entry]\nType=Application\nExec=/bin/true\n%s", fields) > 0);
    CHECK(fclose(file) == 0);
}
static size_t query(tl_desktop *desktop, const char *text, tl_result results[10]) {
    size_t count = 0;
    CHECK(desktop_query(desktop, text, results, 10, &count) == TL_OK);
    return count;
}
void test_desktop(void) {
    char temporary[] = "/tmp/torchlight-desktop-XXXXXX";
    CHECK(mkdtemp(temporary) != NULL);
    char user[512], system[512], user_apps[512], system_apps[512];
    CHECK(snprintf(user, sizeof(user), "%s/user", temporary) > 0);
    CHECK(snprintf(system, sizeof(system), "%s/system", temporary) > 0);
    CHECK(snprintf(user_apps, sizeof(user_apps), "%s/applications", user) > 0);
    CHECK(snprintf(system_apps, sizeof(system_apps), "%s/applications", system) > 0);
    CHECK(mkdir(user, 0700) == 0 && mkdir(system, 0700) == 0);
    CHECK(mkdir(user_apps, 0700) == 0 && mkdir(system_apps, 0700) == 0);
    CHECK(setenv("XDG_DATA_HOME", user, 1) == 0 && setenv("XDG_DATA_DIRS", system, 1) == 0);
    CHECK(setenv("XDG_CURRENT_DESKTOP", "X-Cinnamon", 1) == 0);
    write_entry(system_apps, "display.desktop",
                "Name=Display\nGenericName=Monitor "
                "configuration\nKeywords=screen;resolution;\nCategories=Settings;\nOnlyShowIn=X-"
                "Cinnamon;\n");
    write_entry(system_apps, "masked.desktop", "Name=MaskedSystem\n");
    write_entry(user_apps, "masked.desktop", "Hidden=true\n");
    write_entry(system_apps, "duplicate.desktop", "Name=SystemDuplicate\n");
    write_entry(user_apps, "duplicate.desktop", "Name=UserOverride\n");
    write_entry(user_apps, "private.desktop", "Name=InvisiblePrivate\nNoDisplay=true\n");
    write_entry(user_apps, "other.desktop", "Name=InvisibleOther\nOnlyShowIn=KDE;\n");
    write_entry(user_apps, "excluded.desktop", "Name=InvisibleExcluded\nNotShowIn=X-Cinnamon;\n");
    write_entry(user_apps, "missing.desktop",
                "Name=InvisibleMissing\nTryExec=/nonexistent/torchlight-executable\n");
    write_entry(user_apps, "sound.desktop",
                "Name=Sound\nKeywords=audio;volume;\nCategories=Settings\n");
    write_entry(user_apps, "keyboard.desktop", "Name=Keyboard\nCategories=Utility;Settings\n");
    write_entry(user_apps, "brand.desktop", "Name=ShortBrand\nX-GNOME-FullName=DifferentLabel\n");
    tl_desktop *desktop = NULL;
    CHECK(desktop_create(&desktop) == TL_OK);
    desktop_acquire(desktop);
    tl_result results[10];
    size_t invalid_count = 99;
    CHECK(desktop_query(desktop, "", NULL, 10, &invalid_count) == TL_INVALID && invalid_count == 0);
    CHECK(desktop_query(desktop, "", results, 0, &invalid_count) == TL_INVALID);
    CHECK(query(desktop, "display", results) == 1);
    uint64_t id = results[0].id;
    CHECK(id >= DESKTOP_ID_BASE);
    const tl_desktop_entry *entry = desktop_resolve(desktop, id);
    CHECK(entry != NULL && entry->settings && strcmp(entry->name, "Display") == 0);
    CHECK(query(desktop, "screen", results) == 1 && results[0].id == id);
    CHECK(query(desktop, "resolution", results) == 1 && results[0].id == id);
    CHECK(query(desktop, "monitor", results) == 1 && results[0].id == id);
    CHECK(query(desktop, "Invisible", results) == 0);
    CHECK(query(desktop, "MaskedSystem", results) == 0);
    CHECK(query(desktop, "SystemDuplicate", results) == 0);
    CHECK(query(desktop, "UserOverride", results) == 1);
    CHECK(query(desktop, "sound", results) == 1);
    CHECK(desktop_resolve(desktop, results[0].id)->settings);
    CHECK(query(desktop, "keyboard", results) == 1);
    CHECK(desktop_resolve(desktop, results[0].id)->settings);
    CHECK(query(desktop, "ShortBrand", results) == 1);
    CHECK(query(desktop, "DifferentLabel", results) == 1);
    CHECK(query(desktop, "", results) == 0);
    desktop_release(desktop);
    char removed[512];
    CHECK(snprintf(removed, sizeof(removed), "%s/display.desktop", system_apps) > 0);
    CHECK(unlink(removed) == 0);
    desktop_refresh(desktop);
    bool stale = false;
    struct timespec pause = {.tv_nsec = 10000000};
    for (size_t i = 0; i < 500 && !stale; i++) {
        desktop_acquire(desktop);
        stale = desktop_resolve(desktop, id) == NULL;
        desktop_release(desktop);
        if (!stale)
            nanosleep(&pause, NULL);
    }
    CHECK(stale);
    desktop_destroy(desktop);
    const char *user_ids[] = {"masked.desktop", "duplicate.desktop", "private.desktop",
                              "other.desktop",  "excluded.desktop",  "missing.desktop",
                              "sound.desktop",  "keyboard.desktop",  "brand.desktop"};
    for (size_t i = 0; i < sizeof(user_ids) / sizeof(user_ids[0]); i++) {
        CHECK(snprintf(removed, sizeof(removed), "%s/%s", user_apps, user_ids[i]) > 0);
        CHECK(unlink(removed) == 0);
    }
    CHECK(snprintf(removed, sizeof(removed), "%s/masked.desktop", system_apps) > 0);
    CHECK(unlink(removed) == 0);
    CHECK(snprintf(removed, sizeof(removed), "%s/duplicate.desktop", system_apps) > 0);
    CHECK(unlink(removed) == 0);
    CHECK(rmdir(user_apps) == 0 && rmdir(system_apps) == 0);
    CHECK(rmdir(user) == 0 && rmdir(system) == 0 && rmdir(temporary) == 0);
    CHECK(unsetenv("XDG_DATA_HOME") == 0 && unsetenv("XDG_DATA_DIRS") == 0);
}
