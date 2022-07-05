#pragma once

#include "fdm.h"
#include "config.h"
#include "reaper.h"
#include "wayland.h"

struct server;
struct server *server_init(const struct config *conf, struct fdm *fdm,
                           struct reaper *reaper, struct wayland *wayl);
void server_destroy(struct server *server);
