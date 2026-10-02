/* Data-path overrides and "key = value" file parsing without environment dependence. */
#include "test.h"
#include "torchlight/config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void write_file(const char *path, const char *text) {
    FILE *stream = fopen(path, "w");
    CHECK(stream != NULL);
    CHECK(fputs(text, stream) >= 0 && fclose(stream) == 0);
}
static void file_parsing(void) {
    char file[] = "/tmp/torchlight-config-XXXXXX";
    int fd = mkstemp(file);
    CHECK(fd >= 0 && close(fd) == 0);
    write_file(file,
               "# comment\n\n  root = /data/one  \nallow=/home/x/.config/nvim\nroot = /two\n");
    tl_config *config = NULL;
    size_t line = 99;
    CHECK(config_create("torchlight", "/tmp/c.db", file, &line, &config) == TL_OK && line == 0);
    CHECK(strcmp(config_file(config), file) == 0 && config_entry_count(config) == 3);
    const char *key = NULL, *value = NULL;
    CHECK(config_entry(config, 0, &key, &value) == TL_OK);
    CHECK(strcmp(key, "root") == 0 && strcmp(value, "/data/one") == 0);
    CHECK(config_entry(config, 2, &key, &value) == TL_OK && strcmp(value, "/two") == 0);
    CHECK(config_entry(config, 3, &key, &value) == TL_INVALID);
    config_destroy(config);
    /* "~/" expands with HOME. */
    write_file(file, "root = ~/notes\n");
    CHECK(config_create("torchlight", "/tmp/c.db", file, NULL, &config) == TL_OK);
    CHECK(config_entry(config, 0, &key, &value) == TL_OK);
    CHECK(value[0] == '/' && strcmp(value + strlen(value) - 6, "/notes") == 0);
    config_destroy(config);
    /* Malformed lines report their number and create nothing. */
    const char *bad[] = {"root = /a\nno equals sign\n", "root = /a\nBad-Key = 1\n",
                         "root = /a\nroot =   \n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file(file, bad[i]);
        CHECK(config_create("torchlight", "/tmp/c.db", file, &line, &config) == TL_INVALID);
        CHECK(config == NULL && line == 2);
    }
    CHECK(unlink(file) == 0);
    CHECK(config_create("torchlight", "/tmp/c.db", file, &line, &config) == TL_IO);
}
static void database_identity(void) {
    char directory[] = "/tmp/torchlight-config-path-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char database[256], alias[256], spelling[256];
    CHECK(snprintf(database, sizeof(database), "%s/catalog.db", directory) > 0);
    CHECK(snprintf(alias, sizeof(alias), "%s/alias.db", directory) > 0);
    CHECK(snprintf(spelling, sizeof(spelling), "%s/./catalog.db", directory) > 0);
    tl_config *config = NULL;
    CHECK(config_create("torchlight", spelling, "/dev/null", NULL, &config) == TL_OK);
    CHECK(strcmp(config_database(config), database) == 0);
    config_destroy(config);
    write_file(database, "");
    CHECK(symlink(database, alias) == 0);
    CHECK(config_create("torchlight", alias, "/dev/null", NULL, &config) == TL_OK);
    CHECK(strcmp(config_database(config), database) == 0);
    config_destroy(config);
    CHECK(unlink(alias) == 0 && unlink(database) == 0 && rmdir(directory) == 0);
}
void test_config(void) {
    tl_config *config = NULL;
    CHECK(config_create("..", "/tmp/catalog.db", NULL, NULL, &config) == TL_INVALID);
    CHECK(config == NULL);
    CHECK(config_create("torchlight", "", NULL, NULL, &config) == TL_INVALID && config == NULL);
    CHECK(config_create("torchlight", "/tmp/c.db", "", NULL, &config) == TL_INVALID);
    CHECK(config_create("torchlight", "/tmp/custom-catalog.db", "/dev/null", NULL, &config) ==
          TL_OK);
    CHECK(strcmp(config_database(config), "/tmp/custom-catalog.db") == 0);
    CHECK(config_state_directory(config) == NULL && config_entry_count(config) == 0);
    config_destroy(config);
    config_destroy(NULL);
    file_parsing();
    database_identity();
}
