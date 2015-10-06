#ifndef CLUSTER_H_INCLUDED
#define CLUSTER_H_INCLUDED

#include <hiredis/hiredis.h>

#include "core.h"

void discover_cluster_slots(const struct vrt_ctx *ctx, vcl_priv_t *config);

redisReply *cluster_execute(
    const struct vrt_ctx *ctx, vcl_priv_t *config, task_priv_t *state,
    unsigned version, struct timeval timeout, unsigned argc, const char *argv[]);

#endif
