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

static int get_key_index(const char *command);
static unsigned get_cluster_slot(const char *key);

void
discover_cluster_slots(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    Lck_Lock(&db->mutex);
    unsafe_discover_slots(ctx, db, server);
    Lck_Unlock(&db->mutex);
}

redisReply *
cluster_execute(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct timeval timeout,
    unsigned max_retries, unsigned argc, const char *argv[], unsigned *retries,
    unsigned master)
{
    // Initializations.
    redisReply *result = NULL;

    // Can the command be executed in a clustered setup?
    int index = get_key_index(argv[0]);
    if ((index > 0) && (index < argc)) {
        // Initializations.
        unsigned slot = get_cluster_slot(argv[index]);
        unsigned hops = db->cluster.max_hops > 0 ? db->cluster.max_hops : UINT_MAX;
        unsigned asking = 0;
        unsigned hop = 0;
        redis_server_t *server = NULL;

        // Execute command, retrying and following redirections up to
        // some limit.
        while ((result == NULL) && (!WS_Overflowed(ctx->ws))) {
            // Execute command:
            //   - server != NULL ==> only include 'server' in the execution plan.
            //   - !master ==> use READONLY + READWRITE when dealing with slaves.
            //   - unknown slot ==> random server selection.
            result = redis_execute(
                ctx, db, state, timeout, max_retries, argc, argv,
                retries, server, asking, master, slot);

            // Reset flags.
            asking = 0;
            hop = 0;
            server = NULL;

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

                    // Set hop flag.
                    hop = 1;

                    // Get database lock.
                    Lck_Lock(&db->mutex);

                    // Add / fetch server.
                    server = unsafe_add_redis_server(
                        ctx, db, location, REDIS_SERVER_TBD_ROLE);
                    AN(server);

                    // ASK vs. MOVED.
                    if (strncmp(result->str, "MOVED", 3) == 0) {
                        // Update stats.
                        db->stats.cluster.replies.moved++;

                        // Rediscover the cluster topology asking to the server
                        // in the MOVED reply (or to any other server if that
                        // one fails). Giving priority to the server in the
                        // MOVED reply ensures that the right topology will be
                        // discovered even when it has not yet been propagated
                        // to the whole cluster.
                        //
                        // Even though using 'server' in the next execution plan
                        // is not strictly required because the cluster topology
                        // has just been rediscovered, this allows handling in a
                        // nice way rw commands sent to ro slaves.
                        //
                        // XXX: at the moment this implementation may result in
                        // multiple threads executing multiple -serialized-
                        // cluster discoveries.
                        unsafe_discover_slots(ctx, db, server);
                    } else {
                        // Update stats.
                        db->stats.cluster.replies.ask++;

                        // Next attempt should send a ASKING command to the
                        // server in the ASK reply.
                        asking = 1;
                    }

                    // Release database lock.
                    Lck_Unlock(&db->mutex);

                    // Release reply object.
                    freeReplyObject(result);
                    result = NULL;

                // Execution completed: some reply, excluding cluster
                // redirections.
                } else {
                    break;
                }
            }

            // Try again?
            if (result == NULL) {
                if (hop && (hops > 0) && (*retries <= max_retries)) {
                    hops--;
                } else {
                    break;
                }
            }
        }

        // Too many redirections?
        if (hops == 0) {
            REDIS_LOG_ERROR(ctx,
                "Too many redirections while executing cluster command (command=%s, db=%s)",
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
    char *host, int port, enum REDIS_SERVER_ROLE role)
{
    // Assertions.
    Lck_AssertHeld(&db->mutex);

    // Add / update server.
    char location[256];
    snprintf(location, sizeof(location), "%s:%d", host, port);
    redis_server_t *server = unsafe_add_redis_server(ctx, db, location, role);
    AN(server);

    // Register slots.
    for (int i = start; i <= stop; i++) {
        server->cluster.slots[i] = 1;
    }
}

static unsigned
unsafe_discover_slots_aux(VRT_CTX, struct vmod_redis_db *db, redis_server_t *server)
{
    // Assertions.
    Lck_AssertHeld(&db->mutex);
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
            redis_server_t *iserver;
            for (unsigned iweight = 0; iweight < NREDIS_SERVER_WEIGHTS; iweight++) {
                for (enum REDIS_SERVER_ROLE irole = 0; irole < NREDIS_SERVER_ROLES; irole++) {
                    VTAILQ_FOREACH(iserver, &db->servers[iweight][irole], list) {
                        for (int i = 0; i < NREDIS_CLUSTER_SLOTS; i++) {
                            iserver->cluster.slots[i] = 0;
                        }
                    }
                }
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
                            reply->element[i]->element[2]->element[1]->integer,
                            REDIS_SERVER_MASTER_ROLE);

                        // Extract slave servers data.
                        for (int j = 3; j < reply->element[i]->elements; j++) {
                            if ((reply->element[i]->element[j]->type == REDIS_REPLY_ARRAY) &&
                                (reply->element[i]->element[j]->elements >= 2) &&
                                (reply->element[i]->element[j]->element[0]->type == REDIS_REPLY_STRING) &&
                                (reply->element[i]->element[j]->element[1]->type == REDIS_REPLY_INTEGER)) {
                                unsafe_add_slot(
                                    ctx, db, start, end,
                                    reply->element[i]->element[j]->element[0]->str,
                                    reply->element[i]->element[j]->element[1]->integer,
                                    REDIS_SERVER_SLAVE_ROLE);
                            }
                        }
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
    Lck_AssertHeld(&db->mutex);

    // Contact already known servers and try to fetch the slots-servers mapping.
    // Always use the provided server instance in the first place.
    if (!unsafe_discover_slots_aux(ctx, db, server)) {
        for (unsigned iweight = 0; iweight < NREDIS_SERVER_WEIGHTS; iweight++) {
            for (enum REDIS_SERVER_ROLE irole = 0; irole < NREDIS_SERVER_ROLES; irole++) {
                redis_server_t *iserver;
                VTAILQ_FOREACH(iserver, &db->servers[iweight][irole], list) {
                    CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                    if ((iserver != server) &&
                        (unsafe_discover_slots_aux(ctx, db, iserver))) {
                        // Lists of servers are only modified on a successful
                        // discovery ==> it's safe to iterate on these data
                        // structures because once they are modified the
                        // iteration will finish.
                        return;
                    }
                }
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
