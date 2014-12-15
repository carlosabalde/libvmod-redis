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

#define DISCOVERY_COMMAND "CLUSTER SLOTS"

static void unsafe_add_slot(
    vcl_priv_t *config, redis_server_t *iserver,
    unsigned start, unsigned stop, const char *host, unsigned port);

static unsigned key2slot(const char *key);

void
discover_cluster_slots(struct sess *sp, vcl_priv_t *config)
{
    // Initializations.
    int i;
    unsigned stop = 0;

    // Get config lock.
    AZ(pthread_mutex_lock(&config->mutex));

    // Reset previous slots.
    for (i = 0; i < MAX_REDIS_CLUSTER_SLOTS; i++) {
        config->slots[i] = NULL;
    }

    // Contact already known clustered servers and try to fetch the
    // slots-servers mapping.
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
                redisReply *reply = redisCommand(rcontext, DISCOVERY_COMMAND);

                // Check reply.
                if ((!rcontext->err) &&
                    (reply != NULL) &&
                    (reply->type == REDIS_REPLY_ARRAY)) {
                    // Extract slots.
                    for (i = 0; i < reply->elements; i++) {
                        if ((reply->element[i]->type == REDIS_REPLY_ARRAY) &&
                            (reply->element[i]->elements >= 3) &&
                            (reply->element[i]->element[2]->type == REDIS_REPLY_ARRAY) &&
                            (reply->element[i]->element[2]->elements == 2)) {
                            // Extract slot data.
                            int start = reply->element[i]->element[0]->integer;
                            int end = reply->element[i]->element[1]->integer;
                            char *host = reply->element[i]->element[2]->element[0]->str;
                            int port = reply->element[i]->element[2]->element[1]->integer;

                            // Check slot data.
                            if ((start >= 0) && (start < MAX_REDIS_CLUSTER_SLOTS) &&
                                (end >= 0) && (end < MAX_REDIS_CLUSTER_SLOTS)) {
                                unsafe_add_slot(config, iserver, start, end, host, port);
                            }
                        }
                    }

                    // Stop execution.
                    stop = 1;
                } else {
                    REDIS_LOG(sp,
                        "Failed to execute Redis command (%s)",
                        DISCOVERY_COMMAND);
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

            // Slots-severs mapping already discovered?
            if (stop) {
                break;
            }
        }
    }

    // Release config lock.
    AZ(pthread_mutex_unlock(&config->mutex));
}

const char *
get_cluster_tag(struct sess *sp, vcl_priv_t *config, const char *key)
{
    // Initializations.
    const char *result = NULL;
    unsigned slot = key2slot(key);

    // Get config lock.
    AZ(pthread_mutex_lock(&config->mutex));

    // Select a tag according with the current slot-tag mapping.
    for (int i = slot; i < MAX_REDIS_CLUSTER_SLOTS; i++) {
        if (config->slots[i] != NULL) {
            result = config->slots[i];
            break;
        }
    }

    // If failed to find the tag (this is possible e.g. if the discovery
    // function failed to fetch the slots-servers mapping), user the tag of
    // any known server running in clustered mode (servers are never deleted;
    // at least one clustered server must exist).
    if (result == NULL) {
        redis_server_t *iserver;
        VTAILQ_FOREACH(iserver, &config->servers, list) {
            if (iserver->clustered) {
                // Check server.
                CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);

                // Fetch tag.
                result = iserver->tag;
            }
        }
    }
    AN(result);

    // Release config lock.
    AZ(pthread_mutex_unlock(&config->mutex));

    // Done!
    return result;
}

redisReply *
cluster_execute(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned version, unsigned argc, const char *argv[])
{
    return NULL;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static void
unsafe_add_slot(
    vcl_priv_t *config, redis_server_t *iserver,
    unsigned start, unsigned stop, const char *host, unsigned port)
{
    // Create new server instance (timeout & TTL are copied from the server used
    // to discover this new server).
    char location[256];
    snprintf(location, sizeof(location), "%s:%d", host, port);
    redis_server_t *server = new_redis_server(
        CLUSTERED_REDIS_SERVER_TAG,
        location,
        iserver->timeout.tv_sec * 1000 + iserver->timeout.tv_usec / 1000,
        iserver->ttl);

    // Add new slot (and release previous one, if required).
    if (config->slots[stop] != NULL) {
        free((void *)(config->slots[stop]));
    }
    config->slots[stop] = strdup(server->tag);
    AN(config->slots[stop]);

    // If required, register new server & pool
    if (!unsafe_redis_server_exists(config, server->tag)) {
        VTAILQ_INSERT_TAIL(&config->servers, server, list);
        if (!unsafe_context_pool_exists(config, server->tag)) {
            redis_context_pool_t *pool = new_redis_context_pool(server->tag);
            VTAILQ_INSERT_TAIL(&config->pools, pool, list);
        }
    } else {
        free_redis_server(server);
    }
}

static unsigned
key2slot(const char *key)
{
    // Extract data to be hashed. Only hash what is inside {...} if there is
    // such a pattern in 'key'. Note that the specification requires the
    // content that is between the first { and the first } after the first {.
    // If {} is found without nothing in the middle, the whole key should
    // be hashed.
    const char *ptr = NULL;
    unsigned len;
    const char *left = strchr(key, '{');
    if (left != NULL) {
        const char *right = strchr(left, '}');
        if ((right != NULL) && (right-left > 1)) {
            ptr = left + 1;
            len = right - ptr;
        }
    }
    if (ptr == NULL) {
        ptr = key;
        len = strlen(key);
    }

    // Done!
    return crc16(ptr, len) % MAX_REDIS_CLUSTER_SLOTS;
}
