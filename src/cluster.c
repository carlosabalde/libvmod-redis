#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "cache/cache.h"

#include "crc16.h"
#include "core.h"
#include "cluster.h"

#define BANNED_COMMANDS "|INFO|MULTI|EXEC|SLAVEOF|REPLICAOF|CONFIG|SHUTDOWN|SCRIPT|"
#define KEY_INDEX3_COMMANDS "|EVAL|EVALSHA|EVAL_RO|EVALSHA_RO|"

#define CLUSTER_DISCOVERY_COMMAND "CLUSTER SHARDS"

static void unsafe_discover_slots(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config, redis_server_t *server);

static int get_key_index(const char *command);
static unsigned get_cluster_slot(const char *key);

void
discover_cluster_slots(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config, redis_server_t *server)
{
    Lck_Lock(&config->mutex);
    Lck_Lock(&db->mutex);
    unsafe_discover_slots(ctx, db, config, server);
    Lck_Unlock(&db->mutex);
    Lck_Unlock(&config->mutex);
}

redisReply *
cluster_execute(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config, task_state_t *state,
    struct timeval timeout, unsigned max_retries, unsigned argc, const char *argv[],
    unsigned *retries, unsigned master)
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
        while (result == NULL) {
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

                    // Get config & database locks.
                    Lck_Lock(&config->mutex);
                    Lck_Lock(&db->mutex);

                    // Add / fetch server.
                    server = unsafe_add_redis_server(
                        ctx, db, config, location, REDIS_SERVER_TBD_ROLE);
                    AN(server);

                    // ASK vs. MOVED.
                    if (strncmp(result->str, "MOVED", 5) == 0) {
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
                        unsafe_discover_slots(ctx, db, config, server);
                    } else {
                        // Update stats.
                        db->stats.cluster.replies.ask++;

                        // Next attempt should send a ASKING command to the
                        // server in the ASK reply.
                        asking = 1;
                    }

                    // Release config & database locks.
                    Lck_Unlock(&db->mutex);
                    Lck_Unlock(&config->mutex);

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

static unsigned
unsafe_add_slot(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config, unsigned start,
    unsigned stop, const char *endpoint, int port, enum REDIS_SERVER_ROLE role)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    Lck_AssertHeld(&db->mutex);

    // Beware the endpoint is provided by the discovered server: a too long
    // value would silently truncate the location (e.g. dropping the ':port'
    // suffix, which would then be parsed as a UNIX socket path) ==> simply
    // discard it.
    char location[256];
    int n = snprintf(location, sizeof(location), "%s:%d", endpoint, port);
    if ((n < 0) || ((size_t) n >= sizeof(location))) {
        REDIS_LOG_ERROR(ctx,
            "Failed to register slots: server location is too long (db=%s, endpoint=%s)",
            db->name, endpoint);
        return 0;
    }

    // Add / update server. Beware this may fail (e.g. invalid location).
    redis_server_t *server = unsafe_add_redis_server(ctx, db, config, location, role);
    if (server == NULL) {
        return 0;
    }

    // Register slots.
    for (int i = start; i <= stop; i++) {
        server->cluster.slots[i] = 1;
    }

    // Done!
    return 1;
}

static unsigned
unsafe_discover_slots_aux(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config, redis_server_t *server)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
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
        // Set command execution timeout early: this also bounds the upcoming
        // TLS handshake & AUTH / HELLO commands.
        int tr = redisSetTimeout(rcontext, db->command_timeout);
        if (tr != REDIS_OK) {
            REDIS_LOG_ERROR(ctx,
                "Failed to set cluster discovery command execution timeout (error=%d, db=%s, server=%s)",
                tr, server->db->name, server->location.raw);
        }

        // Optionally setup TLS & submit AUTH / HELLO command.
        REDIS_BLESS_CONTEXT(
            ctx, rcontext, server->db,
            "Failed to initialize cluster discovery connection",
            "db=%s, server=%s",
            server->db->name, server->location.raw);

        // Do not continue if failed to initialize the connection.
        if (rcontext != NULL) {
            // Send command.
            redisReply *reply = redisCommand(rcontext, CLUSTER_DISCOVERY_COMMAND);

            // Check reply.
            if ((!rcontext->err) && (reply != NULL)) {
                // Log reply.
                if (db->debug) {
                    struct vsb *reply_vsb = redis_reply_to_string(reply);
                    REDIS_LOG_DEBUG(NULL,
                        "Cluster discovery reply received (db=%s, server=%s): %s",
                        db->name, server->location.raw, VSB_data(reply_vsb));
                    VSB_destroy(&reply_vsb);
                }

                if (reply->type == REDIS_REPLY_ARRAY) {
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

                    // Iterate shards.
                    unsigned parse_errors = 0;
                    unsigned slot_ranges = 0;
                    for (int i = 0; i < reply->elements; i++) {
                        const redisReply *shard = reply->element[i];
                        if ((shard->type != REDIS_REPLY_ARRAY) &&
                            RESP3_SWITCH(shard->type != REDIS_REPLY_MAP, 1)) {
                            parse_errors++;
                            continue;
                        }

                        // Find "slots" and "nodes" properties by iterating key-value pairs.
                        const redisReply *slots = NULL;
                        const redisReply *nodes = NULL;
                        for (int j = 0; j + 1 < shard->elements; j += 2) {
                            if (shard->element[j]->type != REDIS_REPLY_STRING) {
                                parse_errors++;
                                continue;
                            }

                            const char *key = shard->element[j]->str;
                            if ((strcmp(key, "slots") == 0) &&
                                (shard->element[j+1]->type == REDIS_REPLY_ARRAY)) {
                                slots = shard->element[j+1];
                            } else if ((strcmp(key, "nodes") == 0) &&
                                    (shard->element[j+1]->type == REDIS_REPLY_ARRAY) &&
                                    (shard->element[j+1]->elements >= 1)) {
                                nodes = shard->element[j+1];
                            }
                        }
                        if (slots == NULL || nodes == NULL) {
                            parse_errors++;
                            continue;
                        }

                        // Beware a shard may legitimately own no slots (e.g.
                        // while resharding, or when its primary is failing and
                        // therefore no slot info is available) ==> that's not
                        // an error, simply skip it.
                        if (slots->elements == 0) {
                            continue;
                        }
                        if ((slots->elements % 2) != 0) {
                            parse_errors++;
                        }

                        // Iterate nodes.
                        for (int j = 0; j < nodes->elements; j++) {
                            const redisReply *node = nodes->element[j];
                            if ((node->type != REDIS_REPLY_ARRAY) &&
                                RESP3_SWITCH(node->type != REDIS_REPLY_MAP, 1)) {
                                parse_errors++;
                                continue;
                            }

                            // Initializations.
                            const char *endpoint = NULL;
                            unsigned port = 0;
                            unsigned tls_port = 0;
                            enum REDIS_SERVER_ROLE role = REDIS_SERVER_TBD_ROLE;

                            // Extract node data.
                            for (int k = 0; k + 1 < node->elements; k += 2) {
                                if (node->element[k]->type != REDIS_REPLY_STRING) {
                                    parse_errors++;
                                    continue;
                                }

                                const char *name = node->element[k]->str;
                                if (strcmp(name, "endpoint") == 0) {
                                    if (node->element[k+1]->type == REDIS_REPLY_STRING) {
                                        endpoint = node->element[k+1]->str;
                                    } else if (node->element[k+1]->type == REDIS_REPLY_NIL) {
                                        endpoint = "";
                                    }
                                } else if (strcmp(name, "port") == 0) {
                                    if ((node->element[k+1]->type == REDIS_REPLY_INTEGER) &&
                                                (node->element[k+1]->integer > 0) &&
                                                (node->element[k+1]->integer <= UINT16_MAX)) {
                                        port = (unsigned)node->element[k+1]->integer;
                                    }
                                } else if (strcmp(name, "tls-port") == 0) {
                                    if ((node->element[k+1]->type == REDIS_REPLY_INTEGER) &&
                                                (node->element[k+1]->integer > 0) &&
                                                (node->element[k+1]->integer <= UINT16_MAX)) {
                                        tls_port = (unsigned)node->element[k+1]->integer;
                                    }
                                } else if ((strcmp(name, "role") == 0) &&
                                            (node->element[k+1]->type == REDIS_REPLY_STRING)) {
                                    const char *value = node->element[k+1]->str;
                                    if (strstr(value, "master") != NULL) {
                                        role = REDIS_SERVER_MASTER_ROLE;
                                    } else if (strstr(value, "replica") != NULL) {
                                        role = REDIS_SERVER_SLAVE_ROLE;
                                    }
                                }
                            }
                            // "?" means misconfigured hostname; skip.
                            if (endpoint != NULL && strcmp(endpoint, "?") == 0) {
                                parse_errors++;
                                continue;
                            }
                            // NULL or "" means use the server we queried.
                            if (endpoint == NULL || endpoint[0] == '\0') {
                                endpoint = server->location.parsed.address.host;
                            }
                            // Prefer tls-port or port depending on the db configuration.
#ifdef TLS_ENABLED
                            unsigned effective_port = (db->tls_ssl_ctx != NULL) ? tls_port : port;
                            if (effective_port == 0) {
                                effective_port = (db->tls_ssl_ctx != NULL) ? port : tls_port;
                            }
#else
                            unsigned effective_port = port;
#endif
                            // Check node data.
                            if ((endpoint == NULL) ||
                                (effective_port == 0) ||
                                (role == REDIS_SERVER_TBD_ROLE)) {
                                parse_errors++;
                                continue;
                            }

                            // Iterate slot ranges.
                            for (int k = 0; k + 1 < slots->elements; k += 2) {
                                // Extract slot data.
                                if ((slots->element[k]->type != REDIS_REPLY_INTEGER) ||
                                    (slots->element[k + 1]->type != REDIS_REPLY_INTEGER)) {
                                    parse_errors++;
                                    continue;
                                }
                                int start = slots->element[k]->integer;
                                int end = slots->element[k + 1]->integer;

                                // Check slot data.
                                if ((start < 0) || (start >= NREDIS_CLUSTER_SLOTS) ||
                                    (end < 0) || (end >= NREDIS_CLUSTER_SLOTS)) {
                                    parse_errors++;
                                    continue;
                                }

                                // Add / update server and register slots.
                                if (unsafe_add_slot(
                                        ctx, db, config, start, end,
                                        endpoint, effective_port, role)) {
                                    slot_ranges++;
                                } else {
                                    parse_errors++;
                                }
                            }
                        }
                    }

                    // Log parse errors.
                    if (parse_errors > 0) {
                        REDIS_LOG_WARNING(ctx,
                            "Failed to parse some cluster discovery data (parse_errors=%u, db=%s, server=%s)",
                            parse_errors, db->name, server->location.raw);
                    }

                    // Stop execution, but only if some slot range has been
                    // registered: an empty topology is useless (all execution
                    // plans would end up empty) ==> it's better to retry the
                    // discovery using some other server.
                    if (slot_ranges > 0) {
                        done = 1;
                        db->stats.cluster.discoveries.total++;
                    } else {
                        REDIS_LOG_ERROR(ctx,
                            "Failed to discover any slot range (db=%s, server=%s)",
                            db->name, server->location.raw);
                        db->stats.cluster.discoveries.failed++;
                    }
                } else {
                    REDIS_LOG_ERROR(ctx,
                        "Unexpected cluster discovery reply (type=%d, db=%s, server=%s)",
                        reply->type, db->name, server->location.raw);
                    db->stats.cluster.discoveries.failed++;
                }
            } else {
                REDIS_LOG_ERROR(ctx,
                    "Failed to execute cluster discovery command (error=%d, db=%s, server=%s): %s",
                    rcontext->err, db->name, server->location.raw,
                    HIREDIS_ERRSTR(rcontext, reply));
                db->stats.cluster.discoveries.failed++;
            }

            // Release reply.
            if (reply != NULL) {
                freeReplyObject(reply);
            }
        } else {
            db->stats.cluster.discoveries.failed++;
        }
    } else {
        if (rcontext != NULL) {
            REDIS_LOG_ERROR(ctx,
                "Failed to establish cluster discovery connection (error=%d, db=%s, server=%s): %s",
                rcontext->err, db->name, server->location.raw, HIREDIS_ERRSTR(rcontext));
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
unsafe_discover_slots(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config, redis_server_t *server)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    Lck_AssertHeld(&db->mutex);

    // Contact already known servers and try to fetch the slots-servers mapping.
    // Always use the provided server instance in the first place.
    if (!unsafe_discover_slots_aux(ctx, db, config, server)) {
        for (unsigned iweight = 0; iweight < NREDIS_SERVER_WEIGHTS; iweight++) {
            for (enum REDIS_SERVER_ROLE irole = 0; irole < NREDIS_SERVER_ROLES; irole++) {
                redis_server_t *iserver;
                VTAILQ_FOREACH(iserver, &db->servers[iweight][irole], list) {
                    CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                    if ((iserver != server) &&
                        (unsafe_discover_slots_aux(ctx, db, config, iserver))) {
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
