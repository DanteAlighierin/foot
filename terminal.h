#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct cell {
    char c[5];
    uint32_t foreground;
    uint32_t background;
    bool dirty;
};

struct grid {
    int cols;
    int rows;
    int cell_width;
    int cell_height;

    int cursor;
    struct cell *cells;

    uint32_t foreground;
    uint32_t background;

    bool dirty;
    bool all_dirty;
};

struct vt {
    int state;  /* enum state */
    struct {
        struct {
            unsigned value;
            struct {
                unsigned value[16];
                size_t idx;
            } sub;
        } v[16];
        size_t idx;
    } params;
    struct {
        uint8_t data[2];
        size_t idx;
    } intermediates;
    struct {
        uint8_t data[1024];
        size_t idx;
    } osc;
    struct {
        uint8_t data[4];
        size_t idx;
        size_t left;
    } utf8;
    bool bold;
    bool dim;
    bool italic;
    bool underline;
    bool strikethrough;
    bool blink;
    bool conceal;
    bool reverse;
    uint32_t foreground;
    uint32_t background;
};

struct terminal {
    struct vt vt;
    struct grid grid;
};
