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

static redis_server_t * unsafe_add_redis_server(
    vcl_priv_t *config, const char *location);

static void unsafe_discover_slots(
    struct sess *sp, vcl_priv_t *config);

static const char *unsafe_get_cluster_tag(
    vcl_priv_t *config, const char *key);

static int get_key_index(const char *command);

void
discover_cluster_slots(struct sess *sp, vcl_priv_t *config)
{
    AZ(pthread_mutex_lock(&config->mutex));
    unsafe_discover_slots(sp, config);
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
        int ttl = MAX_REDIRECTIONS;
        unsigned asking = 0;

        // Get initial destination server tag.
        AZ(pthread_mutex_lock(&config->mutex));
        const char *tag = unsafe_get_cluster_tag(config, argv[index]);
        AZ(pthread_mutex_unlock(&config->mutex));

        // Execute command, following redirections, up to some limit.
        for (; ttl > 0; ttl--) {
            // Execute command & reset 'asking' flag.
            result = redis_execute(sp, config, state, tag, version, argc, argv, asking);
            asking = 0;

            // Check reply.
            if (result != NULL) {
                // Is this a MOVED or ASK error reply?
                if ((result->type == REDIS_REPLY_ERROR) &&
                    ((strncmp(result->str, "MOVED", 5) == 0) &&
                     (strncmp(result->str, "ASK", 3) == 0))) {
                    // ASK vs. MOVED.
                    if (strncmp(result->str, "ASK", 3) == 0) {
                        asking = 1;
                    }

                    // ...
                    AZ(pthread_mutex_lock(&config->mutex));
                    if (!asking) {
                        // Ignore reply and rediscover the cluster topology.
                        unsafe_discover_slots(sp, config);

                        // Update tag, according with the updated mapping.
                        tag = unsafe_get_cluster_tag(config, argv[index]);
                    } else {
                        // Extract location (e.g. ASK 3999 127.0.0.1:6381).
                        char *ptr = strchr(result->str, ' ');
                        AN(ptr);
                        char *location = strchr(ptr + 1, ' ');
                        AN(location);
                        location++;

                        // Add server and use its tag.
                        redis_server_t *server = unsafe_add_redis_server(
                            config, location);
                        tag = server->tag;
                    }
                    AZ(pthread_mutex_unlock(&config->mutex));

                    // Release reply object.
                    freeReplyObject(result);
                    result = NULL;

                // Execution completed.
                } else {
                    break;
                }

            // No reply. Do not execute retries in this particular case (note
            // that this does not follow the suggestion in the reference Ruby
            // implementation of Redis Cluster).
            } else {
                break;
            }
        }

        // Too many redirections?
        if (ttl <= 0) {
            REDIS_LOG(sp,
                "Too many redirections while executing Redis Cluster command (%s)",
                argv[0]);
        }

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

static redis_server_t *
unsafe_add_redis_server(vcl_priv_t *config, const char *location)
{
    // Initializations.
    redis_server_t *result = NULL;
    redis_server_t *server = new_redis_server(
        CLUSTERED_REDIS_SERVER_TAG, location, config->timeout, config->ttl);
    AN(server);

    // Register new server & slot if required.
    result = unsafe_get_redis_server(config, server->tag);
    if (result == NULL) {
        // Add new server.
        result = server;
        VTAILQ_INSERT_TAIL(&config->servers, result, list);

        // If required, add new pool.
        if (unsafe_get_context_pool(config, result->tag) == NULL) {
            redis_context_pool_t *pool = new_redis_context_pool(result->tag);
            VTAILQ_INSERT_TAIL(&config->pools, pool, list);
        }
    } else {
        // The initial server instance was only useful to fetch its tag.
        free_redis_server(server);
    }

    // Done!
    return result;
}

static const char *
unsafe_add_slot(
    vcl_priv_t *config,
    unsigned start, unsigned stop, const char *location)
{
    // Register new server & slot if required.
    redis_server_t *server = unsafe_add_redis_server(config, location);

    // Add new slot (and release previous one, if required).
    if (config->slots[stop] != NULL) {
        free((void *)(config->slots[stop]));
    }
    config->slots[stop] = strdup(server->tag);
    AN(config->slots[stop]);

    // Done!
    return config->slots[stop];
}

static void
unsafe_discover_slots(struct sess *sp, vcl_priv_t *config)
{
    // Initializations.
    int i;
    char location[256];
    unsigned stop = 0;

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
                                snprintf(
                                    location, sizeof(location),
                                    "%s:%d", host, port);
                                unsafe_add_slot(config, start, end, location);
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
unsafe_get_cluster_tag(vcl_priv_t *config, const char *key)
{
    // Initializations.
    const char *result = NULL;
    unsigned slot = get_cluster_slot(key);

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

    // Done!
    return result;
}
