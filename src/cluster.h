#ifndef CLUSTER_H_INCLUDED
#define CLUSTER_H_INCLUDED

#include <hiredis/hiredis.h>

#include "core.h"

void discover_cluster_slots(const struct vrt_ctx *ctx, vcl_priv_t *config);

redisReply *cluster_execute(
    const struct vrt_ctx *ctx, vcl_priv_t *config, thread_state_t *state,
    unsigned version, unsigned argc, const char *argv[]);

#endif
