#ifndef CLUSTER_H_INCLUDED
#define CLUSTER_H_INCLUDED

#include <hiredis/hiredis.h>

#include "core.h"

void discover_cluster_slots(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server);

redisReply *cluster_execute(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    struct timeval timeout, unsigned max_retries, unsigned argc, const char *argv[],
    unsigned *retries, unsigned master);

#endif
