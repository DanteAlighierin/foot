#include "misc.h"

#include <wctype.h>

bool
isword(wchar_t wc, bool spaces_only)
{
    if (spaces_only)
        return iswgraph(wc);

    switch (wc) {
    default: return iswgraph(wc);

    case L'(': case L')':
    case L'[': case L']':
    case L'{': case L'}':
    case L'<': case L'>':
    case L'│': case L'|':
    case L',':
    case L'`': case L'"': case L'\'':
    case L':':
        return false;
    }
}
