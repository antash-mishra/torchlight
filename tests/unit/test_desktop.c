/* Isolated XDG precedence, visibility, metadata matching, live removal and
 * change-stamp refresh checks. */
#include "test.h"
#include "torchlight/desktop.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* Many checks of the fast test timing: long enough to see a parse if an
 * unchanged tree still caused them. */
#define QUIET_MS 150
#define WAIT_STEPS 1000
/* Filler entries a discovery parses before it reaches the system trees, so
 * a forced parse lasts long enough for a directory to change under it. */
#define FILLER_ENTRIES 3000
/* After a forced parse starts: past its check walk (stats only), long before
 * its discovery reaches the system trees behind the filler entries. */
#define MID_PARSE_MS 40
static void pause_ms(long milliseconds) {
    const struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    CHECK(nanosleep(&delay, NULL) == 0);
}
static void write_exec_entry(const char *directory, const char *id, const char *exec,
                             const char *fields) {
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, id);
    CHECK(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    CHECK(fprintf(file, "[Desktop Entry]\nType=Application\nExec=%s\n%s", exec, fields) > 0);
    CHECK(fclose(file) == 0);
}
static void write_entry(const char *directory, const char *id, const char *fields) {
    write_exec_entry(directory, id, "/bin/true", fields);
}
static size_t query(tl_desktop *desktop, const char *text, tl_result results[10]) {
    size_t count = 0;
    CHECK(desktop_query(desktop, text, results, 10, &count) == TL_OK);
    return count;
}
/* Write an entry beside its target and rename it into place, as package
 * managers do, so a check sees either the old file or the new one. */
static void replace_entry(const char *directory, const char *id, const char *fields) {
    char staged[512], target[512];
    CHECK(snprintf(staged, sizeof(staged), "%s/staged.tmp", directory) > 0);
    CHECK(snprintf(target, sizeof(target), "%s/%s", directory, id) > 0);
    write_entry(directory, "staged.tmp", fields);
    CHECK(rename(staged, target) == 0);
}
/* Wait (bounded) until a query for text has exactly wanted results. */
static void wait_query(tl_desktop *desktop, const char *text, size_t wanted) {
    tl_result results[10];
    size_t count = SIZE_MAX;
    for (size_t i = 0; i < WAIT_STEPS && count != wanted; i++) {
        desktop_acquire(desktop);
        count = query(desktop, text, results);
        desktop_release(desktop);
        if (count != wanted)
            pause_ms(5);
    }
    CHECK(count == wanted);
}
/* GIO's OnlyShowIn/NotShowIn rule, and Cinnamon's menu, which also lists
 * GNOME's entries (GNOME Terminal on Linux Mint) but never GNOME Settings. */
static void shown_in(void) {
    const char *const kde[] = {"KDE", NULL}, *const cinnamon[] = {"X-Cinnamon", NULL};
    const char *const gnome[] = {"GNOME", "Unity", NULL}, *const ubuntu[] = {"ubuntu", NULL};
    const char *const flashback[] = {"GNOME-Flashback", NULL}, *const none[] = {NULL};
    CHECK(desktop_shown_in(NULL, NULL, NULL, "X-Cinnamon"));
    CHECK(desktop_shown_in(NULL, NULL, "/bin/true", NULL));
    CHECK(desktop_shown_in(cinnamon, NULL, NULL, "X-Cinnamon"));
    CHECK(!desktop_shown_in(kde, NULL, NULL, "X-Cinnamon"));
    CHECK(!desktop_shown_in(NULL, cinnamon, NULL, "X-Cinnamon"));
    CHECK(desktop_shown_in(NULL, kde, NULL, "X-Cinnamon"));
    CHECK(!desktop_shown_in(none, NULL, NULL, "X-Cinnamon"));
    /* Without a desktop, OnlyShowIn hides and NotShowIn never does. */
    CHECK(!desktop_shown_in(cinnamon, NULL, NULL, NULL));
    CHECK(!desktop_shown_in(gnome, NULL, NULL, ""));
    CHECK(desktop_shown_in(NULL, cinnamon, NULL, ""));
    /* The first listed desktop that either list names decides. */
    CHECK(desktop_shown_in(gnome, NULL, NULL, "ubuntu:GNOME"));
    CHECK(!desktop_shown_in(gnome, ubuntu, NULL, "ubuntu:GNOME"));
    CHECK(desktop_shown_in(gnome, ubuntu, NULL, "GNOME:ubuntu"));
    CHECK(!desktop_shown_in(flashback, NULL, NULL, "GNOME"));
    CHECK(!desktop_shown_in(gnome, NULL, NULL, "GNOME-Flashback"));
    CHECK(desktop_shown_in(gnome, NULL, NULL, "GNOME-Flashback:GNOME"));
    /* GNOME-only entries: listed on Cinnamon (the regression), nowhere else. */
    CHECK(desktop_shown_in(gnome, NULL, "gnome-terminal", "X-Cinnamon"));
    CHECK(desktop_shown_in(gnome, cinnamon, "gnome-terminal", "X-Cinnamon"));
    CHECK(desktop_shown_in(gnome, NULL, "gnome-terminal", "GNOME"));
    CHECK(!desktop_shown_in(gnome, NULL, "gnome-terminal", "KDE"));
    CHECK(!desktop_shown_in(gnome, NULL, "gnome-terminal", "MATE"));
    CHECK(!desktop_shown_in(gnome, NULL, "gnome-terminal", "X-Cinnamonish"));
    /* GNOME Settings panels stay out of Cinnamon, as its menu leaves them. */
    CHECK(!desktop_shown_in(gnome, NULL, "gnome-control-center display", "X-Cinnamon"));
    CHECK(!desktop_shown_in(NULL, NULL, "gnome-control-center", "X-Cinnamon"));
    CHECK(desktop_shown_in(gnome, NULL, "gnome-control-center display", "GNOME"));
    CHECK(desktop_shown_in(NULL, NULL, "gnome-control-center", "KDE"));
}
static size_t catalog_count(const char *text) {
    tl_desktop *desktop = NULL;
    tl_result results[10];
    CHECK(desktop_create(NULL, &desktop) == TL_OK);
    desktop_acquire(desktop);
    size_t count = query(desktop, text, results);
    desktop_release(desktop);
    desktop_destroy(desktop);
    return count;
}
/* The catalog applies the session's menu rule: on Cinnamon a GNOME-only entry
 * is listed and a GNOME Settings panel is not. A stand-in gnome-control-center
 * on PATH lets GIO accept the panel, so only the rule can hide it. */
static void gnome_entries(const char *temporary, const char *user_apps) {
    char bin[512], program[512], path[4096];
    CHECK(snprintf(bin, sizeof(bin), "%s/bin", temporary) > 0);
    CHECK(snprintf(program, sizeof(program), "%s/gnome-control-center", bin) > 0);
    CHECK(mkdir(bin, 0700) == 0);
    FILE *file = fopen(program, "w");
    CHECK(file != NULL && fputs("#!/bin/sh\n", file) >= 0 && fclose(file) == 0);
    CHECK(chmod(program, 0700) == 0);
    const char *old_path = getenv("PATH");
    char *saved_path = old_path == NULL ? NULL : strdup(old_path);
    CHECK(old_path == NULL || saved_path != NULL);
    CHECK(snprintf(path, sizeof(path), "%s:%s", bin, old_path == NULL ? "/usr/bin" : old_path) > 0);
    CHECK(setenv("PATH", path, 1) == 0);
    write_entry(user_apps, "gnome-terminal.desktop",
                "Name=GnomeOnlyTerminal\nOnlyShowIn=GNOME;Unity;\n");
    write_exec_entry(user_apps, "gnome-panel.desktop", "gnome-control-center display",
                     "Name=GnomeSettingsPanel\nOnlyShowIn=GNOME;Unity;\n");
    write_entry(user_apps, "kde-only.desktop", "Name=GnomeAbsentKde\nOnlyShowIn=KDE;\n");
    CHECK(setenv("XDG_CURRENT_DESKTOP", "X-Cinnamon", 1) == 0);
    CHECK(catalog_count("GnomeOnlyTerminal") == 1);
    CHECK(catalog_count("GnomeSettingsPanel") == 0);
    CHECK(catalog_count("GnomeAbsentKde") == 0);
    CHECK(setenv("XDG_CURRENT_DESKTOP", "GNOME", 1) == 0);
    CHECK(catalog_count("GnomeOnlyTerminal") == 1);
    CHECK(catalog_count("GnomeSettingsPanel") == 1);
    CHECK(setenv("XDG_CURRENT_DESKTOP", "KDE", 1) == 0);
    CHECK(catalog_count("GnomeOnlyTerminal") == 0);
    CHECK(catalog_count("GnomeAbsentKde") == 1);
    CHECK(setenv("XDG_CURRENT_DESKTOP", "X-Cinnamon", 1) == 0);
    const char *ids[] = {"gnome-terminal.desktop", "gnome-panel.desktop", "kde-only.desktop"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        CHECK(snprintf(path, sizeof(path), "%s/%s", user_apps, ids[i]) > 0);
        CHECK(unlink(path) == 0);
    }
    if (saved_path != NULL)
        CHECK(setenv("PATH", saved_path, 1) == 0);
    else
        CHECK(unsetenv("PATH") == 0);
    free(saved_path);
    CHECK(unlink(program) == 0 && rmdir(bin) == 0);
}
/* -1 when query does not return the application called name, else whether
 * desktop_named says the query names it. */
static int naming(tl_desktop *desktop, const char *text, const char *name) {
    tl_result results[10];
    bool named[10];
    size_t count = query(desktop, text, results);
    CHECK(desktop_named(desktop, text, results, count, named) == TL_OK);
    for (size_t i = 0; i < count; i++)
        if (strcmp(desktop_resolve(desktop, results[i].id)->name, name) == 0)
            return named[i];
    return -1;
}
/* A query names an application when every word starts its name, a word of
 * the name or its initials; metadata, infixes and typos never do. */
static void named_entries(const char *user_apps) {
    write_entry(user_apps, "chrome.desktop",
                "Name=Google Chrome\nGenericName=Web Browser\nKeywords=internet;\n");
    write_entry(user_apps, "writer.desktop", "Name=LibreOffice Writer\n");
    write_entry(user_apps, "editor.desktop", "Name=Text Editor\n");
    write_entry(user_apps, "sqlite.desktop", "Name=DB Browser for SQLite\n");
    tl_desktop *desktop = NULL;
    CHECK(desktop_create(NULL, &desktop) == TL_OK);
    desktop_acquire(desktop);
    const char *chrome = "Google Chrome";
    CHECK(naming(desktop, "google", chrome) == 1 && naming(desktop, "CHROME", chrome) == 1);
    CHECK(naming(desktop, "goo", chrome) == 1 && naming(desktop, "c", chrome) == 1);
    CHECK(naming(desktop, "google chrome", chrome) == 1);
    CHECK(naming(desktop, " chr  goo ", chrome) == 1 && naming(desktop, "gc", chrome) == 1);
    CHECK(naming(desktop, "office", "LibreOffice Writer") == 1);
    CHECK(naming(desktop, "writer", "LibreOffice Writer") == 1);
    CHECK(naming(desktop, "text", "Text Editor") == 1);
    /* A later word typed whole although it changes case inside (ADR 0060). */
    CHECK(naming(desktop, "sqlite", "DB Browser for SQLite") == 1);
    CHECK(naming(desktop, "db sqlite", "DB Browser for SQLite") == 1);
    CHECK(naming(desktop, "browser", chrome) == 0 && naming(desktop, "internet", chrome) == 0);
    CHECK(naming(desktop, "google web", chrome) == 0 && naming(desktop, "rome", chrome) == 0);
    CHECK(naming(desktop, "gogle", chrome) == 0);
    tl_result results[10];
    bool named[2] = {true, true};
    size_t count = query(desktop, "google", results);
    CHECK(count >= 1);
    tl_result unknown = {1, "/Applications/Unknown", 0};
    CHECK(desktop_named(desktop, "google", &unknown, 1, named) == TL_OK && !named[0]);
    CHECK(desktop_named(desktop, "  ", results, 1, named) == TL_OK && !named[0]);
    CHECK(desktop_named(desktop, "google", results, 0, NULL) == TL_OK);
    CHECK(desktop_named(desktop, "google", results, 1, NULL) == TL_INVALID);
    char long_query[LEXICAL_QUERY_BYTES + 2];
    memset(long_query, 'g', sizeof(long_query) - 1);
    long_query[sizeof(long_query) - 1] = 0;
    named[0] = true;
    CHECK(desktop_named(desktop, long_query, results, 1, named) == TL_LIMIT && !named[0]);
    desktop_release(desktop);
    desktop_destroy(desktop);
    const char *ids[] = {"chrome.desktop", "writer.desktop", "editor.desktop", "sqlite.desktop"};
    char path[512];
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        CHECK(snprintf(path, sizeof(path), "%s/%s", user_apps, ids[i]) > 0);
        CHECK(unlink(path) == 0);
    }
}
static uint64_t wait_parses(tl_desktop *desktop, uint64_t wanted) {
    for (size_t i = 0; i < WAIT_STEPS && desktop_parses(desktop) < wanted; i++)
        pause_ms(5);
    return desktop_parses(desktop);
}
static void filler_name(char *name, size_t size, const char *user_apps, int index) {
    CHECK(snprintf(name, size, "%s/filler%d.desktop", user_apps, index) > 0);
}
/* A directory unreadable only while a parse reads it reappears once it is
 * readable again: the parse records the stamp of what it read (no vendor
 * entries), which the next check's stamp differs from. Recording the stamp
 * of the check that started the parse would match the restored tree and
 * hide the entry until the ten-minute rescan. One-second checks leave no
 * check between the parse and the restore to repair that by chance. */
static void half_seen_directory(const char *user_apps, const char *vendor) {
    char name[512];
    for (int i = 0; i < FILLER_ENTRIES; i++) {
        filler_name(name, sizeof(name), user_apps, i);
        write_entry(user_apps, strrchr(name, '/') + 1, "Name=Filler\nNoDisplay=true\n");
    }
    const tl_desktop_options slow = {.check_ms = 1000, .settle_ms = 100, .rescan_ms = 3600000};
    tl_desktop *desktop = NULL;
    CHECK(desktop_create(&slow, &desktop) == TL_OK);
    wait_query(desktop, "VendorTool", 1);
    uint64_t parses = desktop_parses(desktop);
    desktop_refresh(desktop);
    pause_ms(MID_PARSE_MS);
    CHECK(chmod(vendor, 0) == 0);
    CHECK(wait_parses(desktop, parses + 1) == parses + 1);
    CHECK(chmod(vendor, 0700) == 0);
    wait_query(desktop, "VendorTool", 1);
    desktop_destroy(desktop);
    for (int i = 0; i < FILLER_ENTRIES; i++) {
        filler_name(name, sizeof(name), user_apps, i);
        CHECK(unlink(name) == 0);
    }
}
/* M7: an unchanged catalog is never parsed again (it was every second), and
 * neither are unrelated files or empty directories. A replaced entry parses
 * once, plus the settle parse; entries in new directories, explicit refreshes
 * and directories that stop or start being listable still parse. */
static void change_stamps(const char *user_apps, const char *system_apps) {
    const tl_desktop_options fast = {.check_ms = 10, .settle_ms = 100, .rescan_ms = 3600000};
    tl_desktop *desktop = NULL;
    CHECK(desktop_create(&fast, &desktop) == TL_OK && desktop_parses(desktop) == 1);
    pause_ms(QUIET_MS);
    CHECK(desktop_parses(desktop) == 1);
    write_entry(system_apps, "mimeinfo.cache", "");
    char vendor[512], removed[512];
    CHECK(snprintf(vendor, sizeof(vendor), "%s/vendor", system_apps) > 0);
    CHECK(mkdir(vendor, 0700) == 0);
    pause_ms(QUIET_MS);
    CHECK(desktop_parses(desktop) == 1);
    replace_entry(user_apps, "sound.desktop", "Name=Mixer\n");
    wait_query(desktop, "Mixer", 1);
    CHECK(wait_parses(desktop, 3) == 3);
    pause_ms(QUIET_MS);
    CHECK(desktop_parses(desktop) == 3);
    replace_entry(vendor, "tool.desktop", "Name=VendorTool\n");
    wait_query(desktop, "VendorTool", 1);
    uint64_t parses = wait_parses(desktop, 5);
    desktop_refresh(desktop);
    CHECK(wait_parses(desktop, parses + 1) == parses + 1);
    /* Permissions bind root only through capabilities; skip the cases there. */
    if (geteuid() != 0) {
        CHECK(chmod(vendor, 0) == 0);
        wait_query(desktop, "VendorTool", 0);
        CHECK(chmod(vendor, 0700) == 0);
        wait_query(desktop, "VendorTool", 1);
    }
    desktop_destroy(desktop);
    if (geteuid() != 0)
        half_seen_directory(user_apps, vendor);
    CHECK(snprintf(removed, sizeof(removed), "%s/tool.desktop", vendor) > 0);
    CHECK(unlink(removed) == 0 && rmdir(vendor) == 0);
    CHECK(snprintf(removed, sizeof(removed), "%s/mimeinfo.cache", system_apps) > 0);
    CHECK(unlink(removed) == 0);
}
void test_desktop(void) {
    shown_in();
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
    write_entry(user_apps, "browser.desktop",
                "Name=WindowedBrowser\nStartupWMClass=browser-main\n");
    char long_class[DESKTOP_WM_CLASS_BYTES + 1], long_fields[DESKTOP_WM_CLASS_BYTES + 64];
    memset(long_class, 'w', DESKTOP_WM_CLASS_BYTES);
    long_class[DESKTOP_WM_CLASS_BYTES] = 0;
    CHECK(snprintf(long_fields, sizeof(long_fields), "Name=LongClass\nStartupWMClass=%s\n",
                   long_class) > 0);
    write_entry(user_apps, "long.desktop", long_fields);
    tl_desktop *desktop = NULL;
    CHECK(desktop_create(NULL, &desktop) == TL_OK);
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
    CHECK(query(desktop, "dislp", results) == 1 && results[0].id == id);
    CHECK(query(desktop, "screen monitor", results) == 1 && results[0].id == id);
    CHECK(query(desktop, "Invisible", results) == 0);
    CHECK(query(desktop, "MaskedSystem", results) == 0);
    CHECK(query(desktop, "SystemDuplicate", results) == 0);
    CHECK(query(desktop, "UserOverride", results) == 1);
    CHECK(query(desktop, "sound", results) == 1);
    CHECK(desktop_resolve(desktop, results[0].id)->settings);
    CHECK(query(desktop, "keyboard", results) == 1);
    CHECK(desktop_resolve(desktop, results[0].id)->settings);
    CHECK(query(desktop, "ShortBrand", results) == 1);
    CHECK(desktop_resolve(desktop, results[0].id)->wm_class == NULL);
    /* StartupWMClass names an entry's windows; overlong values are dropped. */
    CHECK(query(desktop, "WindowedBrowser", results) == 1);
    CHECK(strcmp(desktop_resolve(desktop, results[0].id)->wm_class, "browser-main") == 0);
    CHECK(query(desktop, "LongClass", results) == 1);
    CHECK(desktop_resolve(desktop, results[0].id)->wm_class == NULL);
    CHECK(query(desktop, "DifferentLabel", results) == 1);
    CHECK(query(desktop, "", results) == 0);
    /* Boosts address sorted-id positions and last for one query only:
     * Display (2657) overtakes DifferentLabel (2850), never Sound (8059). */
    CHECK(query(desktop, "s", results) >= 3);
    CHECK(strcmp(desktop_resolve(desktop, results[1].id)->name, "DifferentLabel") == 0);
    size_t display = SIZE_MAX;
    for (size_t i = 0; i < desktop_count(desktop); i++)
        if (strcmp(desktop_entry(desktop, i)->desktop_id, "display.desktop") == 0)
            display = i;
    CHECK(display != SIZE_MAX);
    tl_lexical_boost boost = {display, 200};
    size_t count = 0;
    CHECK(desktop_query_boosted(desktop, "s", &boost, 1, results, 10, &count) == TL_OK);
    CHECK(count >= 3 && strcmp(desktop_resolve(desktop, results[0].id)->name, "Sound") == 0);
    CHECK(strcmp(desktop_resolve(desktop, results[1].id)->name, "Display") == 0);
    CHECK(query(desktop, "s", results) >= 3);
    CHECK(strcmp(desktop_resolve(desktop, results[1].id)->name, "DifferentLabel") == 0);
    tl_lexical_boost invalid = {desktop_count(desktop), 1};
    CHECK(desktop_query_boosted(desktop, "s", &invalid, 1, results, 10, &count) == TL_INVALID);
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
    gnome_entries(temporary, user_apps);
    named_entries(user_apps);
    change_stamps(user_apps, system_apps);
    const char *user_ids[] = {"masked.desktop",  "duplicate.desktop", "private.desktop",
                              "other.desktop",   "excluded.desktop",  "missing.desktop",
                              "sound.desktop",   "keyboard.desktop",  "brand.desktop",
                              "browser.desktop", "long.desktop"};
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
