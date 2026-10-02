/* Component-aware scope checks over raw Linux path bytes. */
#include "torchlight/path.h"
#include <stddef.h>
#include <string.h>
bool path_within(const char *path, const char *scope) {
    if (path == NULL || scope == NULL || scope[0] != '/')
        return false;
    size_t length = strlen(scope);
    return strncmp(path, scope, length) == 0 &&
           (path[length] == 0 || path[length] == '/' || length == 1);
}
