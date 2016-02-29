#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "cache/cache.h"

#include "crc16.h"
#include "core.h"
#include "cluster.h"

#define DISCOVERY_COMMAND "CLUSTER SLOTS"

static void unsafe_discover_slots(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server);

static redis_server_t *unsafe_get_cluster_server(struct vmod_redis_db *db, const char *key);
static redis_server_t *unsafe_get_random_cluster_server(struct vmod_redis_db *db);

static int get_key_index(const char *command);

void
discover_cluster_slots(
    VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    AZ(pthread_mutex_lock(&db->mutex));
    unsafe_discover_slots(ctx, db, server);
    AZ(pthread_mutex_unlock(&db->mutex));
}

redisReply *
cluster_execute(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, unsigned version,
    struct timeval timeout, unsigned retries, unsigned argc, const char *argv[])
{
    // Initializations.
    redisReply *result = NULL;

    // Can the command be executed in a clustered setup?
    int index = get_key_index(argv[0]);
    if ((index > 0) && (index < argc)) {
        // Initializations.
        int hops = db->cluster.max_hops > 0 ? db->cluster.max_hops : UINT_MAX;
        int tries = 1 + retries;
        redis_server_t *server = NULL;
        unsigned asking = 0;
        unsigned random = 0;

        // Execute command, retrying & following redirections, up to some limit.
        for (; tries > 0 && hops > 0; hops--) {
            // Get destination server: random vs. slots-servers mapping based
            // selection. Note that the later may return NULL (this is possible
            // e.g. if the discovery function failed to fetch the slots-servers
            // mapping). If that happens, simply use any known server running
            // in clustered mode (servers are never deleted; at least one
            // clustered server must exist).
            if (!asking) {
                AZ(pthread_mutex_lock(&db->mutex));
                if (!random) {
                    server = unsafe_get_cluster_server(db, argv[index]);
                    if (server == NULL) {
                        server = unsafe_get_random_cluster_server(db);
                    }
                } else {
                    server = unsafe_get_random_cluster_server(db);
                }
                AZ(pthread_mutex_unlock(&db->mutex));
            }
            AN(server);

            // Execute command.
            result = redis_execute(ctx, db, state, server, version, timeout, argc, argv, asking);

            // Reset flags.
            server = NULL;
            random = 0;
            asking = 0;

            // Check reply.
            if (result != NULL) {
                // Is this a MOVED or ASK error reply?
                if ((result->type == REDIS_REPLY_ERROR) &&
                    ((strncmp(result->str, "MOVED", 5) == 0) ||
                     (strncmp(result->str, "ASK", 3) == 0))) {
                    // Extract location (e.g. ASK 3999 127.0.0.1:6381).
                    char *ptr = strchr(result->str, ' ');
                    AN(ptr);
                    char *location = strchr(ptr + 1, ' ');
                    AN(location);
                    location++;

                    // Get database lock.
                    AZ(pthread_mutex_lock(&db->mutex));

                    // Add / fetch server.
                    redis_server_t *aserver = unsafe_add_redis_server(
                        ctx, db, location);
                    AN(aserver);

                    // ASK vs. MOVED.
                    if (strncmp(result->str, "MOVED", 3) == 0) {
                        // Update stats.
                        db->stats.cluster.replies.moved++;

                        // Rediscover the cluster topology asking to the server
                        // in the MOVED reply (or to any other server is that
                        // one fails). Giving priority to the server in the
                        // MOVED reply ensures that the right topology will be
                        // discovered even when it has not yet been propagated
                        // to the whole cluster.
                        // XXX: at the moment this implementation may result in
                        // multiple threads executing multiple -serialized-
                        // cluster discoveries.
                        unsafe_discover_slots(ctx, db, aserver);
                    } else {
                        // Update stats.
                        db->stats.cluster.replies.ask++;

                        // Next attempt should send a ASKING command to the
                        // server in the ASK reply.
                        asking = 1;
                        server = aserver;
                    }

                    // Release database lock.
                    AZ(pthread_mutex_unlock(&db->mutex));

                    // Release reply object.
                    freeReplyObject(result);
                    result = NULL;

                // Execution completed.
                } else {
                    break;
                }

            // No reply. If some retries are available, during next execution
            // try with a random clustered server.
            } else {
                tries--;
                random = 1;

                AZ(pthread_mutex_lock(&db->mutex));
                db->stats.commands.retried++;
                AZ(pthread_mutex_unlock(&db->mutex));
            }
        }

        // Too many retries / redirections?
        if ((tries <= 0) || (hops <= 0)) {
            REDIS_LOG(ctx,
                "Too many %s while executing Redis Cluster command (%s)",
                tries <= 0 ? "retries" : "redirections",
                argv[0]);
        }

    // Invalid Redis Cluster command.
    } else {
        REDIS_LOG(ctx,
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
    VRT_CTX, struct vmod_redis_db *db,
    unsigned start, unsigned stop, const char *location)
{
    redis_server_t *server = unsafe_add_redis_server(ctx, db, location);
    AN(server);
    db->cluster.slots[stop] = server;
}

static unsigned
unsafe_discover_slots_aux(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    // Initializations.
    int i;
    char location[256];
    unsigned done = 0;

    // Check server.
    CHECK_OBJ_NOTNULL(server, REDIS_SERVER_MAGIC);
    assert(server->location.type == REDIS_SERVER_LOCATION_HOST_TYPE);

    // Create context.
    redisContext *rcontext;
    if ((db->connection_timeout.tv_sec > 0) ||
        (db->connection_timeout.tv_usec > 0)) {
        rcontext = redisConnectWithTimeout(
            server->location.parsed.address.host,
            server->location.parsed.address.port,
            db->connection_timeout);
    } else {
        rcontext = redisConnect(
            server->location.parsed.address.host,
            server->location.parsed.address.port);
    }
    AN(rcontext);

    // Check context.
    if (!rcontext->err) {
        // Set command execution timeout.
        int tr = redisSetTimeout(rcontext, db->command_timeout);
        if (tr != REDIS_OK) {
            REDIS_LOG(ctx, "Failed to set command execution timeout (%d)", tr);
        }

        // Send command.
        redisReply *reply = redisCommand(rcontext, DISCOVERY_COMMAND);

        // Check reply.
        if ((!rcontext->err) &&
            (reply != NULL) &&
            (reply->type == REDIS_REPLY_ARRAY)) {
            // Reset previous slots.
            for (i = 0; i < MAX_REDIS_CLUSTER_SLOTS; i++) {
                db->cluster.slots[i] = NULL;
            }

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
                        unsafe_add_slot(ctx, db, start, end, location);
                    }
                }
            }

            // Stop execution.
            done = 1;
            db->stats.cluster.discoveries.total++;
        } else {
            REDIS_LOG(ctx,
                "Failed to execute Redis command (%s)",
                DISCOVERY_COMMAND);
            db->stats.cluster.discoveries.failed++;
        }

        // Release reply.
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    } else {
        REDIS_LOG(ctx,
            "Failed to establish Redis connection (%d): %s",
            rcontext->err,
            rcontext->errstr);
        db->stats.cluster.discoveries.failed++;
    }

    // Release context.
    redisFree(rcontext);

    // Done.
    return done;
}


static void
unsafe_discover_slots(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    // Contact already known clustered servers and try to fetch the
    // slots-servers mapping. Always use the provided server instance in the
    // first place.
    if (!unsafe_discover_slots_aux(ctx, db, server)) {
        redis_server_t *iserver;
        VTAILQ_FOREACH(iserver, &db->servers, list) {
            if (unsafe_discover_slots_aux(ctx, db, iserver)) {
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
    // contain the key as the first argument after the command name. This is
    // indeed the case for most commands, and when it is not true the cluster
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
    // Start-end indexes of '{'' and '}'.
    int s, e;

    // Search the first occurrence of '{'.
    int keylen = strlen(key);
    for (s = 0; s < keylen; s++) {
        if (key[s] == '{') {
            break;
        }
    }

    // No '{'? Hash the whole key. This is the base case.
    if (s == keylen) {
        return crc16(key, keylen) & (MAX_REDIS_CLUSTER_SLOTS - 1);
    }

    // '{' found? Check if we have the corresponding '}'.
    for (e = s+1; e < keylen; e++){
        if (key[e] == '}') {
            break;
        }
    }

    // No '}' or nothing between {}? Hash the whole key.
    if ((e == keylen) || (e == s + 1)) {
        return crc16(key, keylen) & (MAX_REDIS_CLUSTER_SLOTS - 1);
    }

    // If we are here there is both a '{' and a '}' on its right. Hash
    // what is in the middle between '{' and '}'.
    return crc16(key + s + 1, e - s - 1) & (MAX_REDIS_CLUSTER_SLOTS - 1);
}

static redis_server_t *
unsafe_get_cluster_server(struct vmod_redis_db *db, const char *key)
{
    // Initializations.
    redis_server_t *result = NULL;

    // Select a server according with the current slots-servers mapping.
    unsigned slot = get_cluster_slot(key);
    for (int i = slot; i < MAX_REDIS_CLUSTER_SLOTS; i++) {
        if (db->cluster.slots[i] != NULL) {
            result = db->cluster.slots[i];
            break;
        }
    }

    // Done!
    return result;
}

static redis_server_t *
unsafe_get_random_cluster_server(struct vmod_redis_db *db)
{
    // Initializations.
    redis_server_t *result = NULL;

    // Look for a clustered server.
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &db->servers, list) {
        // Found!
        CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
        result = iserver;

        // Move the server to the end of the list (this ensures a nice
        // distribution of load between all available servers).
        VTAILQ_REMOVE(&db->servers, iserver, list);
        VTAILQ_INSERT_TAIL(&db->servers, iserver, list);

        // Done!
        break;
    }

    // Done!
    return result;
}
