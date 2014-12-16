#define _GNU_SOURCE
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
#define MAX_REDIRECTIONS 16

static void unsafe_add_slot(
    vcl_priv_t *config,
    unsigned start, unsigned stop, const char *host, unsigned port);

static const char *get_cluster_tag(vcl_priv_t *config, const char *key);

static int get_key_index(const char *command);

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
                                unsafe_add_slot(config, start, end, host, port);
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

redisReply *
cluster_execute(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned version, unsigned argc, const char *argv[])
{
    // Initializations.
    redisReply *result = NULL;

    // Can the command be executed in a clustered setup?
    int index = get_key_index(argv[0]);
    if ((index > 0) && (index < argc)) {
        // Initializations.
        const char *key_tag = get_cluster_tag(config, argv[index]);

    // Invalid Redis Cluster command.
    } else {
        REDIS_LOG(sp,
            "Invalid Redis Cluster command (%s)",
            argv[0]);
    }

    // Done!
    return result;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static void
unsafe_add_slot(
    vcl_priv_t *config,
    unsigned start, unsigned stop, const char *host, unsigned port)
{
    // Create new server instance.
    char location[256];
    snprintf(location, sizeof(location), "%s:%d", host, port);
    redis_server_t *server = new_redis_server(
        CLUSTERED_REDIS_SERVER_TAG, location, config->timeout, config->ttl);
    AN(server);

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

static int
get_key_index(const char *command)
{
    // Initializations.
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "|%s|", command);

    // Some commands (e.g. INFO) are explicitly banned returning -1. Some other
    // commands (e.g. EVAL) are explicitly handled to return the correct
    // location of the key value. Finally, all other commands are assumed to
    // contains the key as the first argument after the command name. This is
    // indeed the key for most commands, and when it is not true the cluster
    // redirection will point to the right node anyway.
    if (strcasestr("|INFO|MULTI|EXEC|SLAVEOF|CONFIG|SHUTDOWN|SCRIPT|", buffer) != NULL) {
        return -1;
    } else if (strcasestr("|EVAL|EVALSHA|", buffer) != NULL) {
        return 3;
    }
    return 1;
}

static unsigned
get_cluster_slot(const char *key)
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

static const char *
get_cluster_tag(vcl_priv_t *config, const char *key)
{
    // Initializations.
    const char *result = NULL;
    unsigned slot = get_cluster_slot(key);

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
    // function failed to fetch the slots-servers mapping), use the tag of
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
