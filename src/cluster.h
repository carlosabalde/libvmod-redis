#ifndef CLUSTER_H_INCLUDED
#define CLUSTER_H_INCLUDED

#include "core.h"

void discover_cluster_slots(struct sess *sp, vcl_priv_t *config);

const char *get_cluster_tag(struct sess *sp, vcl_priv_t *config, const char *key);

#endif
