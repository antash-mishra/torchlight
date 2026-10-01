/* Scan matching symbols in order after a conservative mask rejection. */
#include "torchlight/subseq.h"
bool subseq_matches(tl_text text, tl_text query) {
    if (query.length == 0)
        return true;
    if (text.symbols == NULL || query.symbols == NULL)
        return false;
    if ((text.mask & query.mask) != query.mask)
        return false;
    size_t matched = 0;
    for (size_t i = 0; i < text.length; i++) {
        if (text.symbols[i] == query.symbols[matched])
            matched++;
        if (matched == query.length)
            return true;
    }
    return false;
}
