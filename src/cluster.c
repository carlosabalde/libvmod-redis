#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"
#include "vcc_if.h"

#include "crc16.h"
#include "core.h"
#include "cluster.h"

void
fetch_cluster_slots(struct sess *sp, vcl_priv_t *config)
{
    // Initializations.
    unsigned stop = 0;

    // Get config lock.
    AZ(pthread_mutex_lock(&config->mutex));

    // Look for clustered servers.
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &config->servers, list) {
        if (iserver->clustered) {
            // Check server.
            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
            assert(iserver->type == REDIS_SERVER_HOST_TYPE);

            // Create context.
            redisContext *rcontext = redisConnectWithTimeout(
                iserver->location.address.host,
                iserver->location.address.port,
                iserver->timeout);
            AN(rcontext);

            // Check context.
            if (!rcontext->err) {
                // Send command.
                redisReply *reply = redisCommand(rcontext, "CLUSTER SLOTS");

                // Check reply.
                if ((!rcontext->err) &&
                    (reply != NULL) &&
                    (reply->type == REDIS_REPLY_ARRAY)) {
                    // Extract slots.
                    for (int i = 0; i < reply->elements; i++) {
                        if ((reply->element[i]->type == REDIS_REPLY_ARRAY) &&
                            (reply->element[i]->elements >= 3) &&
                            (reply->element[i]->element[2]->type == REDIS_REPLY_ARRAY) &&
                            (reply->element[i]->element[2]->elements == 2)) {

                            // TODO: add to the list of slots.
                            // REDIS_LOG(sp, "====> %d - %d (%s:%d)",
                            //     reply->element[i]->element[0]->integer,
                            //     reply->element[i]->element[1]->integer,
                            //     reply->element[i]->element[2]->element[0]->str,
                            //     reply->element[i]->element[2]->element[1]->integer);

                        }
                    }

                    // Stop execution.
                    stop = 1;
                } else {
                    REDIS_LOG(sp, "Failed to execute Redis command CLUSTER SLOTS");
                }

                // Release reply.
                if (reply != NULL) {
                    freeReplyObject(reply);
                }
            } else {
                REDIS_LOG(sp,
                    "Failed to establish Redis connection (%d): %s",
                    rcontext->err,
                    rcontext->errstr);
            }

            // Release context.
            redisFree(rcontext);

            // Slots already fetched?
            if (stop) {
                break;
            }
        }
    }

    // Release config lock.
    AZ(pthread_mutex_unlock(&config->mutex));
}
