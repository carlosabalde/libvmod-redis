#ifndef CLUSTER_H_INCLUDED
#define CLUSTER_H_INCLUDED

#include <hiredis/hiredis.h>

#include "core.h"

void discover_cluster_slots(struct sess *sp, vcl_priv_t *config);

const char *get_cluster_tag(struct sess *sp, vcl_priv_t *config, const char *key);

redisReply *cluster_execute(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned version, unsigned argc, const char *argv[]);

#endif
