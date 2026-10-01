/* Explicit data-path overrides without environment dependence. */
#include "test.h"
#include "torchlight/config.h"
#include <string.h>
void test_config(void) {
    tl_config *config = NULL;
    CHECK(config_create("..", "/tmp/catalog.db", &config) == TL_INVALID && config == NULL);
    CHECK(config_create("torchlight", "", &config) == TL_INVALID && config == NULL);
    CHECK(config_create("torchlight", "/tmp/custom-catalog.db", &config) == TL_OK);
    CHECK(strcmp(config_database(config), "/tmp/custom-catalog.db") == 0);
    CHECK(config_state_directory(config) == NULL);
    config_destroy(config);
    config_destroy(NULL);
}
