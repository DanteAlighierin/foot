#undef NDEBUG
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <tllist.h>

int
main(int argc, const char *const *argv)
{
    tll(int) l = tll_init();
    assert(tll_length(l) == 0);

    /* push back */
    tll_push_back(l, 123); assert(tll_length(l) == 1);
    tll_push_back(l, 456); assert(tll_length(l) == 2);
    tll_push_back(l, 789); assert(tll_length(l) == 3);

    assert(tll_front(l) == 123);
    assert(tll_back(l) == 789);

    /* push front */
    tll_push_front(l, 0xabc); assert(tll_length(l) == 4);

    assert(tll_front(l) == 0xabc);
    assert(tll_back(l) == 789);

    /* Pop back */
    assert(tll_pop_back(l) == 789);
    assert(tll_back(l) == 456);

    /* Pop front */
    assert(tll_pop_front(l) == 0xabc);
    assert(tll_front(l) == 123);

    /* foreach */
    assert(tll_length(l) == 2);

    int seen[tll_length(l)];
    memset(seen, 0, tll_length(l) * sizeof(seen[0]));

    size_t count = 0;
    tll_foreach(l, it)
        seen[count++] = it->item;

    assert(count == tll_length(l));
    assert(seen[0] == 123);
    assert(seen[1] == 456);

    /* rforeach */
    memset(seen, 0, tll_length(l) * sizeof(seen[0]));
    count = 0;
    tll_rforeach(l, it)
        seen[count++] = it->item;

    assert(count == tll_length(l));
    assert(seen[0] == 456);
    assert(seen[1] == 123);

    /* remove */
    tll_push_back(l, 789);
    tll_foreach(l, it) {
        if (it->item > 123 && it->item < 789)
            tll_remove(l, it);
    }
    assert(tll_length(l) == 2);
    assert(tll_front(l) == 123);
    assert(tll_back(l) == 789);

    /* insert before */
    tll_foreach(l, it) {
        if (it->item == 123)
            tll_insert_before(l, it, 0xabc);
    }
    assert(tll_length(l) == 3);
    assert(tll_front(l) == 0xabc);
    assert(tll_back(l) == 789);

    /* insert after */
    tll_foreach(l, it) {
        if (it->item == 789)
            tll_insert_after(l, it, 999);
    }
    assert(tll_length(l) == 4);
    assert(tll_front(l) == 0xabc);
    assert(tll_back(l) == 999);

    /* free */
    tll_free(l);
    assert(tll_length(l) == 0);
    assert(l.head == NULL);
    assert(l.tail == NULL);

    return EXIT_SUCCESS;
}
