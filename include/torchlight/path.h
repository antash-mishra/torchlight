/* Byte-wise path scope helpers independent of filesystem/indexing policy. */
#ifndef TORCHLIGHT_PATH_H
#define TORCHLIGHT_PATH_H
#include <stdbool.h>
/** Whether path equals absolute scope or lies below it, component-aware.
 * NULL/relative scope returns false. No allocation, I/O or ownership. */
bool path_within(const char *path, const char *scope);
#endif
