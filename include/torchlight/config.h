/* Generic XDG data paths plus a simple "key = value" configuration file,
 * independent of indexing and storage: callers interpret the keys. */
#ifndef TORCHLIGHT_CONFIG_H
#define TORCHLIGHT_CONFIG_H
#include "torchlight/common.h"
typedef struct tl_config tl_config;
/** Create owned configuration for application (a single safe path component).
 *
 * Database: canonicalize database_override when provided (its parent must exist);
 * otherwise create XDG_DATA_HOME/application or HOME/.local/share/application
 * with mode 0700 and select catalog.db there.
 *
 * File: read file_override when provided (it must exist), otherwise
 * XDG_CONFIG_HOME/application/config or HOME/.config/application/config if it
 * exists. Lines are "key = value"; blank lines and lines starting with '#'
 * are ignored, surrounding whitespace is trimmed, keys are [a-z_]+ and values
 * are nonempty. A value starting with "~/" is expanded with HOME. Keys may
 * repeat; values keep file order.
 *
 * Absolute XDG/HOME values are required. out is NULL on failure: TL_INVALID
 * for bad arguments or a malformed line (its 1-based number is written to
 * error_line when non-NULL; 0 otherwise), TL_IO for unreadable files or
 * directories, TL_NOMEM, TL_LIMIT. */
tl_status config_create(const char *application, const char *database_override,
                        const char *file_override, size_t *error_line, tl_config **out);
/** Free configuration; NULL allowed, no errors. */
void config_destroy(tl_config *config);
/** Borrow database path until destruction; NULL for NULL config, no errors. */
const char *config_database(const tl_config *config);
/** Borrow canonical default state directory until destruction; NULL for custom
 * database/NULL config. This directory should always be excluded from scans. */
const char *config_state_directory(const tl_config *config);
/** Borrow the configuration file path that was read, or NULL when none was. */
const char *config_file(const tl_config *config);
/** Number of entries in the file, in order; zero for NULL. */
size_t config_entry_count(const tl_config *config);
/** Borrow key and value of entry index until destruction. TL_INVALID for NULL
 * arguments or an index out of range. */
tl_status config_entry(const tl_config *config, size_t index, const char **key, const char **value);
#endif
