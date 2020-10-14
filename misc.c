#include "misc.h"

#include <wctype.h>

bool
isword(wchar_t wc, bool spaces_only, const wchar_t *delimiters)
{
    if (spaces_only)
        return iswgraph(wc);

    if (wcschr(delimiters, wc) != NULL)
        return false;

    return iswgraph(wc);
}
