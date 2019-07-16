#pragma once

struct config {
    char *font;
};

struct config config_load(void);
void config_free(struct config conf);
