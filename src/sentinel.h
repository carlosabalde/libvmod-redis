#ifndef SENTINEL_H_INCLUDED
#define SENTINEL_H_INCLUDED

#include "core.h"

void unsafe_sentinel_start(vcl_state_t *config);

void unsafe_sentinel_discovery(vcl_state_t *config);

void unsafe_sentinel_stop(vcl_state_t *config);

#endif
