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

#define BANNED_COMMANDS "|INFO|MULTI|EXEC|SLAVEOF|CONFIG|SHUTDOWN|SCRIPT|"
#define KEY_INDEX3_COMMANDS "|EVAL|EVALSHA|"

#define CLUSTER_DISCOVERY_COMMAND "CLUSTER SLOTS"

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
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, struct timeval timeout,
    unsigned retries, unsigned argc, const char *argv[])
{
    // Initializations.
    redisReply *result = NULL;

    // Can the command be executed in a clustered setup?
    int index = get_key_index(argv[0]);
    if ((index > 0) && (index < argc)) {
        // Initializations.
        int hops = db->cluster.max_hops > 0 ? db->cluster.max_hops : INT_MAX;
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
            result = redis_execute(ctx, db, state, server, timeout, argc, argv, asking);

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

                // Execution completed: some reply, excluding cluster
                // redirections.
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
            REDIS_LOG_ERROR(ctx,
                "Too many %s while executing cluster command (command=%s, db=%s)",
                tries <= 0 ? "retries" : "redirections",
                argv[0], db->name);
        }

    // Invalid Redis Cluster command.
    } else {
        REDIS_LOG_ERROR(ctx,
            "Invalid cluster command (command=%s, db=%s)",
            argv[0], db->name);
    }

    // Done!
    return result;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static void
unsafe_add_slot(
    VRT_CTX, struct vmod_redis_db *db, unsigned start, unsigned stop,
    char *host, int port)
{
    // Assertions.
    //   - db->mutex locked.

    // Add / update server & register slot.
    char location[256];
    snprintf(location, sizeof(location), "%s:%d", host, port);
    redis_server_t *server = unsafe_add_redis_server(ctx, db, location);
    AN(server);
    db->cluster.slots[stop] = server;
}

static unsigned
unsafe_discover_slots_aux(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    // Assertions.
    //   - db->mutex locked.
    assert(server->location.type == REDIS_SERVER_LOCATION_HOST_TYPE);

    // Log event.
    REDIS_LOG_INFO(ctx,
        "Discovery of cluster topology started (db=%s, server=%s)",
        db->name, server->location.raw);

    // Initializations.
    unsigned done = 0;

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

    // Check context.
    if ((rcontext != NULL) && (!rcontext->err)) {
        // Set command execution timeout.
        int tr = redisSetTimeout(rcontext, db->command_timeout);
        if (tr != REDIS_OK) {
            REDIS_LOG_ERROR(ctx,
                "Failed to set cluster discovery command execution timeout (error=%d, db=%s, server=%s)",
                tr, server->db->name, server->location.raw);
        }

        // Send command.
        redisReply *reply = redisCommand(rcontext, CLUSTER_DISCOVERY_COMMAND);

        // Check reply.
        if ((!rcontext->err) &&
            (reply != NULL) &&
            (reply->type == REDIS_REPLY_ARRAY)) {
            // Reset previous slots.
            for (int i = 0; i < NREDIS_CLUSTER_SLOTS; i++) {
                db->cluster.slots[i] = NULL;
            }

            // Extract slots.
            for (int i = 0; i < reply->elements; i++) {
                if ((reply->element[i]->type == REDIS_REPLY_ARRAY) &&
                    (reply->element[i]->elements >= 3) &&
                    (reply->element[i]->element[0]->type == REDIS_REPLY_INTEGER) &&
                    (reply->element[i]->element[1]->type == REDIS_REPLY_INTEGER) &&
                    (reply->element[i]->element[2]->type == REDIS_REPLY_ARRAY) &&
                    (reply->element[i]->element[2]->elements >= 2) &&
                    (reply->element[i]->element[2]->element[0]->type == REDIS_REPLY_STRING) &&
                    (reply->element[i]->element[2]->element[1]->type == REDIS_REPLY_INTEGER)) {
                    // Extract slot data.
                    int start = reply->element[i]->element[0]->integer;
                    int end = reply->element[i]->element[1]->integer;

                    // Check slot data.
                    if ((start >= 0) && (start < NREDIS_CLUSTER_SLOTS) &&
                        (end >= 0) && (end < NREDIS_CLUSTER_SLOTS)) {
                        unsafe_add_slot(
                            ctx, db, start, end,
                            reply->element[i]->element[2]->element[0]->str,
                            reply->element[i]->element[2]->element[1]->integer);
                    }
                }
            }

            // Stop execution.
            done = 1;
            db->stats.cluster.discoveries.total++;
        } else {
            REDIS_LOG_ERROR(ctx,
                "Failed to execute cluster discovery command (db=%s, server=%s)",
                db->name, server->location.raw);
            db->stats.cluster.discoveries.failed++;
        }

        // Release reply.
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    } else {
        if (rcontext != NULL) {
            REDIS_LOG_ERROR(ctx,
                "Failed to establish cluster discovery connection (error=%d, db=%s, server=%s): %s",
                rcontext->err, db->name, server->location.raw, rcontext->errstr);
        } else {
            REDIS_LOG_ERROR(ctx,
                "Failed to establish cluster discovery connection (db=%s, server=%s)",
                db->name, server->location.raw);
        }
        db->stats.cluster.discoveries.failed++;
    }

    // Release context.
    if (rcontext != NULL) {
        redisFree(rcontext);
    }

    // Done.
    return done;
}

static void
unsafe_discover_slots(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    // Assertions.
    //   - db->mutex locked.

    // Contact already known servers and try to fetch the slots-servers mapping.
    // Always use the provided server instance in the first place.
    if (!unsafe_discover_slots_aux(ctx, db, server)) {
        redis_server_t *iserver;
        VTAILQ_FOREACH(iserver, &db->servers, list) {
            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
            if ((iserver != server) &&
                (unsafe_discover_slots_aux(ctx, db, iserver))) {
                // The list of servers is only modified on a successful
                // discovery ==> it's safe to iterate on this data
                // structure because once it is modified the
                // iteration will finish.
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
    //
    // XXX: beware that cluster redirections trigger expensive cluster
    // rediscoveries ==> they must be avoided at all costs.
    if (strcasestr(BANNED_COMMANDS, buffer) != NULL) {
        return -1;
    } else if (strcasestr(KEY_INDEX3_COMMANDS, buffer) != NULL) {
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
        return crc16(key, keylen) & (NREDIS_CLUSTER_SLOTS - 1);
    }

    // '{' found? Check if we have the corresponding '}'.
    for (e = s+1; e < keylen; e++){
        if (key[e] == '}') {
            break;
        }
    }

    // No '}' or nothing between {}? Hash the whole key.
    if ((e == keylen) || (e == s + 1)) {
        return crc16(key, keylen) & (NREDIS_CLUSTER_SLOTS - 1);
    }

    // If we are here there is both a '{' and a '}' on its right. Hash
    // what is in the middle between '{' and '}'.
    return crc16(key + s + 1, e - s - 1) & (NREDIS_CLUSTER_SLOTS - 1);
}

static redis_server_t *
unsafe_get_cluster_server(struct vmod_redis_db *db, const char *key)
{
    // Assertions.
    //   - db->mutex locked.

    // Initializations.
    redis_server_t *result = NULL;

    // Select a server according with the current slots-servers mapping.
    unsigned slot = get_cluster_slot(key);
    for (int i = slot; i < NREDIS_CLUSTER_SLOTS; i++) {
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
    // Assertions.
    //   - db->mutex locked.

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
