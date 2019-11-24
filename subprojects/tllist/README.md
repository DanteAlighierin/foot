# tllist

**tllist** is a *T*yped *L*inked *L*ist C header file only
library implemented using pre-processor macros.


1. [Description](#description)
1. [Usage](#usage)
   1. [Declaring a variable](#declaring-a-variable)
   1. [Adding items - basic](#adding-items-basic)
   1. [List length](#list-length)
   1. [Accessing items](#accessing-items)
   1. [Iterating](#iterating)
   1. [Removing items - basic](#removing-items-basic)
   1. [Adding items - advanced](#adding-items-advanced)
   1. [Removing items - advanced](#removing-items-advanced)
   1. [Freeing](#freeing)
1. [Integrating](#integrating)
   1. [Meson](#meson)
1. [API](#api)
   1. [Cheat sheet](#cheat-sheet)


## Description

Most C implementations of linked list are untyped. That is, their data
carriers are typically `void *`. This is error prone since your
compiler will not be able to help you correct your mistakes (_oh, was
it pointer-to-a-pointer... I though it was just a pointer..._).

**tllist** addresses this by using pre-processor macros to implement
dynamic types, where the data carrier is typed to whatever you want;
both **primitive** data types are supported as well as aggregated ones
such as **structs**, **enums** and **unions**.

Being a double-linked list, most operations are constant in time
(including pushing and popping both to/from front and back).

The memory overhead is fairly small; each item carries, besides its
data, a _prev_ and _next_ pointer (i.e. a constant 16 byte overhead
per item on 64-bit architectures).

The list itself has two _head_ and _tail_ pointers, plus a _length_
variable (typically 8 bytes on 64-bit architectures) to make list
length lookup constant in time.

Thus, assuming 64-bit pointers (and a 64-bit `size_t` type), the total
overhead is `3*8 + n*2*8` bytes.


## Usage

### Declaring a variable

1. **Declare a variable**

   ```c
   /* Declare a variable using an anonymous type */
   tll(int) an_integer_list = tll_init();
   ```


2. **Typedef**

   ```c
   /* First typedef the list type */
   typedef tll(int) an_integer_list_t;

   /* Then declare a variable using that typedef */
   an_integer_list_t an_integer_list = tll_init();
   ```

3. **Named struct**

   ```c
   /* First declare named struct */
   tll(int, an_integer_list);

   /* Then declare a variable using that named struct */
   struct an_integer_list an_integer_list = tll_init();
   ```

### Adding items - basic

Use `tll_push_back()` or `tll_push_front()` to add elements to the
back or front of the list.

```c
tll_push_back(an_integer_list, 4711);
tll_push_front(an_integer_list, 1234);
```


### List length

`tll_length()` returns the length (number of items) in a list.

```c
tll_push_back(an_integer_list, 1234);
tll_push_back(an_integer_list, 5678);
printf("length: %zu\n", tll_length(an_integer_list));
```

Outputs:

    length: 2


### Accessing items

For the front and back items, you can use `tll_front()` and
`tll_back()` respectively. For any other item in the list, you need to
iterate the list and find the item yourself.

```c
tll_push_back(an_integer_list, 1234);
tll_push_back(an_integer_list, 5555);
tll_push_back(an_integer_list, 6789);

printf("front: %d\n", tll_front(an_integer_list));
printf("back: %d\n", tll_back(an_integer_list));
```

Outputs:

    front: 1234
    back: 6789


### Iterating

You can iterate the list either forward or backwards, using
`tll_foreach()` and `tll_rforeach()` respectively.

The `it` variable should be treated as an opaque iterator type, where
`it->item` is the item.

In reality, it is simply a pointer to the linked list entry, and since
tllist is a header-only implementation, you do have access to e.g. the
next/prev pointers. There should not be any need to access anything
except `item` however.

Note that `it` can be named anything.

```c
tll_push_back(an_integer_list, 1);
tll_push_back(an_integer_list, 2);

tll_foreach(an_integer_list, it) {
    printf("forward: %d\n", it->item);
}

tll_rforeach(an_integer_list, it) {
    printf("reverse: %d\n", it->item);
}
```

Outputs:

    forward: 1
    forward: 2
    reverse: 2
    reverse: 1


### Removing items - basic

`tll_pop_front()` and `tll_pop_back()` removes the front/back item and
returns it.

```c
tll_push_back(an_integer_list, 1234);
tll_push_back(an_integer_list, 5678);

printf("front: %d\n", tll_pop_front(an_integer_list));
printf("back: %d\n", tll_pop_back(an_integer_list));
printf("length: %zu\n", tll_length(an_integer_list));
```

Outputs:

    front: 1234
    back: 5678
    length: 0


### Adding items - advanced

Given an iterator, you can insert new items before or after that
iterator, using `tll_insert_before()` and `tll_insert_after()`.

```c
tll_foreach(an_integer_list, it) {
    if (it->item == 1234) {
        tll_insert_before(an_integer_list, it, 7777);
        break;
    }
}
```

Q: Why do I have to pass **both** the _list_ and the _iterator_ to
   `tll_insert_before()`?

A: If not, **each** element in the list would have to contain a
   pointer to the owning list, which would significantly increase the
   overhead.


### Removing items - advanced

Similar to how you can add items while iterating a list, you can also
remove them.

Note that the `*foreach()` functions are **safe** in this regard - it
is perfectly OK to remove the "current" item.

```c
tll_foreach(an_integer_list, it) {
    if (it->item.delete_me)
        tll_remove(an_integer_list, it);
}
```

To make it slightly easier to handle cases where the item _itself_
must be free:d as well, there is also `tll_remove_and_free()`. It
works just like `tll_remove()`, but takes an additional argument; a
callback that will be called for each item.

```c
tll(int *) int_p_list = tll_init();

int *a = malloc(sizeof(*a));
int *b = malloc(sizeof(*b));

*a = 1234;
*b = 5678;

tll_push_back(int_p_list, a);
tll_push_back(int_p_list, b);

tll_foreach(int_p_list, it) {
    tll_remove_and_free(int_p_list, it, free);
}
```


### Freeing

To remove **all** items, use `tll_free()`, or
`tll_free_and_free()`. These are just convenience functions and
calling these are equivalent to:

```c
tll_foreach(an_integer_list, it) {
    tll_remove(an_integer_list, it);
}
```

Note that there is no need to call `tll_free()` on an empty
(`tll_length(list) == 0`) list.


## Integrating

The easiest way may be to simply copy `tllist.h` into your
project. But see sections below for other ways.


### Meson

You can use tllist as a subproject. In your main project's
`meson.build`, to something like:

```meson
tllist = subproject('tllist').get_variable('tllist')
executable('you-executable', ..., dependencies: [tllist])
```


## API

### Cheat sheet

| Function                            | Description                                           | Context            | Complexity |
|-------------------------------------|-------------------------------------------------------|--------------------|-----------:|
| `list = tll_init()`                 | initialize a new tllist variable to an empty list     | Variable init      |       O(1) |
| `tll_length(list)`                  | returns the length (number of items) of a list        |                    |       O(1) |
| `tll_push_front(list, item)`        | inserts _item_ at the beginning of the list           |                    |       O(1) |
| `tll_push_back(list, item)`         | inserts  _item_ at the end of the list                |                    |       O(1) |
| `tll_front(list)`                   | returns the first  item in the list                   |                    |       O(1) |
| `tll_back(list)`                    | returns the last item in the list                     |                    |       O(1) |
| `tll_pop_front(list)`               | removes and returns the first item in the list        |                    |       O(1) |
| `tll_pop_back(list)`                | removes and returns the last item in the list         |                    |       O(1) |
| `tll_foreach(list, it)`             | iterates the list from the beginning to the end       |                    |       O(n) |
| `tll_rforeach(list, it)`            | iterates the list from the end to the beginning       |                    |       O(n) |
| `tll_insert_before(list, it, item)` | inserts _item_ before _it_.                           | `tll_(r)foreach()` |       O(1) |
| `tll_insert_after(list, it, item)`  | inserts _item_ after _it_.                            | `tll_(r)foreach()` |       O(1) |
| `tll_remove(list, it)`              | removes _it_ from the list.                           | `tll_(r)foreach()` |       O(1) |
| `tll_remove_and_free(list, it, cb)` | removes _it_ from the list, and calls `cb(it->item)`. | `tll_(r)foreach()` |       O(1) |
| `tll_free(list)`                    | removes **all** items from the list                   |                    |       O(n) |
| `tll_free_and_free(list, cb)`       | removes **all** items from the list, and calls `cb(it->item)` for each item. | |   O(n) |
