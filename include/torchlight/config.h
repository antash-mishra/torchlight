/* Generic XDG data-path defaults, independent of indexing and storage. */
#ifndef TORCHLIGHT_CONFIG_H
#define TORCHLIGHT_CONFIG_H
#include "torchlight/common.h"
typedef struct tl_config tl_config;
/** Create owned data-path configuration for application (a single safe path
 * component). Copy database_override when provided; its parent must exist.
 * Otherwise create XDG_DATA_HOME/application or HOME/.local/share/application
 * directories with mode 0700 and select catalog.db. Absolute XDG/HOME values
 * are required. out NULL on TL_INVALID/NOMEM/IO/LIMIT. No config file is loaded. */
tl_status config_create(const char *application, const char *database_override, tl_config **out);
/** Free configuration; NULL allowed, no errors. */
void config_destroy(tl_config *config);
/** Borrow database path until destruction; NULL for NULL config, no errors. */
const char *config_database(const tl_config *config);
/** Borrow canonical default state directory until destruction; NULL for custom
 * database/NULL config. This directory should always be excluded from scans. */
const char *config_state_directory(const tl_config *config);
#endif
