#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <hiredis/hiredis.h>
#include <arpa/inet.h>

#include "vrt.h"
#include "cache/cache.h"

#include "sha1.h"
#include "sentinel.h"
#include "core.h"

#define ROLE_DISCOVERY_COMMAND "ROLE"

vmod_state_t vmod_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .version = 0,
    .locks.refs = 0,
    .locks.config = NULL,
    .locks.db = NULL
};

struct plan {
    // Ordered list of private contexts, including a reference to the next item
    // to be considered during the execution. This is only used when the
    // database is configured to use private contexts.
    struct {
        unsigned n;
        redis_context_t **list;
        unsigned next;
    } contexts;

    // Ordered circular list of servers, including a reference to the next item
    // to be considered during the execution.
    struct {
        unsigned n;
        redis_server_t **list;
        unsigned next;
    } servers;
};

static enum REDIS_SERVER_ROLE unsafe_discover_redis_server_role(
    VRT_CTX, redis_server_t *server);

static struct plan *plan_execution(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state,
    unsigned size, redis_server_t *server, unsigned master, unsigned slot);

static redis_context_t *lock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct plan *plan);

static void unlock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, redis_context_t *context);

static redisReply *get_redis_repy(
    VRT_CTX, redis_context_t *context, struct timeval timeout, unsigned argc,
    const char *argv[], unsigned asking);

static const char *sha1(VRT_CTX, const char *script);

redis_server_t *
new_redis_server(
    struct vmod_redis_db *db, const char *location, enum REDIS_SERVER_ROLE role)
{
    redis_server_t *result;
    ALLOC_OBJ(result, REDIS_SERVER_MAGIC);
    AN(result);

    char *ptr = strrchr(location, ':');
    if (ptr != NULL) {
        result->location.type = REDIS_SERVER_LOCATION_HOST_TYPE;
        result->location.parsed.address.host = strndup(location, ptr - location);
        AN(result->location.parsed.address.host);
        result->location.parsed.address.port = atoi(ptr + 1);
    } else {
        result->location.type = REDIS_SERVER_LOCATION_SOCKET_TYPE;
        result->location.parsed.path = strdup(location);
        AN(result->location.parsed.path);
    }

    // Do not continue if this is a clustered database but the location is not
    // provided using the IP + port format.
    struct in_addr ia4;
    if ((db->cluster.enabled) &&
        ((result->location.type != REDIS_SERVER_LOCATION_HOST_TYPE) ||
         (inet_pton(AF_INET, result->location.parsed.address.host, &ia4) == 0))) {
        free((void *) result->location.parsed.address.host);
        FREE_OBJ(result);
        return NULL;
    }

    result->db = db;

    result->location.raw = strdup(location);
    AN(result->location.raw);

    result->role = role;

    result->weight = 0;

    AZ(pthread_cond_init(&result->pool.cond, NULL));

    result->pool.ncontexts = 0;
    VTAILQ_INIT(&result->pool.free_contexts);
    VTAILQ_INIT(&result->pool.busy_contexts);

    for (int i = 0; i < NREDIS_CLUSTER_SLOTS; i++) {
        result->cluster.slots[i] = 0;
    }

    result->sickness.tst = 0;
    result->sickness.exp = 0;

    // Done!
    return result;
}

void
free_redis_server(redis_server_t *server)
{
    server->db = NULL;

    free((void *) server->location.raw);
    server->location.raw = NULL;
    switch (server->location.type) {
        case REDIS_SERVER_LOCATION_HOST_TYPE:
            free((void *) server->location.parsed.address.host);
            server->location.parsed.address.host = NULL;
            server->location.parsed.address.port = 0;
            break;

        case REDIS_SERVER_LOCATION_SOCKET_TYPE:
            free((void *) server->location.parsed.path);
            server->location.parsed.path = NULL;
            break;
    }

    server->role = REDIS_SERVER_TBD_ROLE;

    server->weight = 0;

    AZ(pthread_cond_destroy(&server->pool.cond));

    server->pool.ncontexts = 0;
    redis_context_t *icontext;
    while (!VTAILQ_EMPTY(&server->pool.free_contexts)) {
        icontext = VTAILQ_FIRST(&server->pool.free_contexts);
        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
        VTAILQ_REMOVE(&server->pool.free_contexts, icontext, list);
        free_redis_context(icontext);
    }
    while (!VTAILQ_EMPTY(&server->pool.busy_contexts)) {
        icontext = VTAILQ_FIRST(&server->pool.busy_contexts);
        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
        VTAILQ_REMOVE(&server->pool.busy_contexts, icontext, list);
        free_redis_context(icontext);
    }

    for (int i = 0; i < NREDIS_CLUSTER_SLOTS; i++) {
        server->cluster.slots[i] = 0;
    }

    server->sickness.tst = 0;
    server->sickness.exp = 0;

    FREE_OBJ(server);
}

redis_context_t *
new_redis_context(
    redis_server_t *server, redisContext *rcontext, time_t tst)
{
    redis_context_t *result;
    ALLOC_OBJ(result, REDIS_CONTEXT_MAGIC);
    AN(result);

    result->server = server;
    result->rcontext = rcontext;
    result->version = vmod_state.version;
    result->tst = tst;

    return result;
}

void
free_redis_context(redis_context_t *context)
{
    context->server = NULL;
    if (context->rcontext != NULL) {
        redisFree(context->rcontext);
        context->rcontext = NULL;
    }
    context->version = 0;
    context->tst = 0;

    FREE_OBJ(context);
}

struct vmod_redis_db *
new_vmod_redis_db(
    vcl_state_t *config, const char *name, struct timeval connection_timeout,
    unsigned connection_ttl, struct timeval command_timeout, unsigned max_command_retries,
    unsigned shared_connections, unsigned max_connections, const char *password,
    unsigned sickness_ttl, unsigned ignore_slaves, unsigned clustered,
    unsigned max_cluster_hops)
{
    struct vmod_redis_db *result;
    ALLOC_OBJ(result, VMOD_REDIS_DATABASE_MAGIC);
    AN(result);

    Lck_New(&result->mutex, vmod_state.locks.db);

    result->config = config;

    for (unsigned weight = 0; weight < NREDIS_SERVER_WEIGHTS; weight++) {
        for (enum REDIS_SERVER_ROLE role = 0; role < NREDIS_SERVER_ROLES; role++) {
            VTAILQ_INIT(&result->servers[weight][role]);
        }
    }

    result->name = strdup(name);
    AN(result->name);
    result->connection_timeout = connection_timeout;
    result->connection_ttl = connection_ttl;
    result->command_timeout = command_timeout;
    result->max_command_retries = max_command_retries;
    result->shared_connections = shared_connections;
    result->max_connections = max_connections;
    if (strlen(password) > 0) {
        result->password = strdup(password);
        AN(result->password);
    } else {
        result->password = NULL;
    }
    result->sickness_ttl = sickness_ttl;
    result->ignore_slaves = ignore_slaves;

    result->cluster.enabled = clustered;
    result->cluster.max_hops = max_cluster_hops;

    result->stats.servers.total = 0;
    result->stats.servers.failed = 0;
    result->stats.connections.total = 0;
    result->stats.connections.failed = 0;
    result->stats.connections.dropped.error = 0;
    result->stats.connections.dropped.hung_up = 0;
    result->stats.connections.dropped.overflow = 0;
    result->stats.connections.dropped.ttl = 0;
    result->stats.connections.dropped.version = 0;
    result->stats.connections.dropped.sick = 0;
    result->stats.workers.blocked = 0;
    result->stats.commands.total = 0;
    result->stats.commands.failed = 0;
    result->stats.commands.retried = 0;
    result->stats.commands.error = 0;
    result->stats.commands.noscript = 0;
    result->stats.cluster.discoveries.total = 0;
    result->stats.cluster.discoveries.failed = 0;
    result->stats.cluster.replies.moved = 0;
    result->stats.cluster.replies.ask = 0;

    return result;
}

void
free_vmod_redis_db(struct vmod_redis_db *db)
{
    Lck_Delete(&db->mutex);

    db->config = NULL;

    for (unsigned weight = 0; weight < NREDIS_SERVER_WEIGHTS; weight++) {
        for (enum REDIS_SERVER_ROLE role = 0; role < NREDIS_SERVER_ROLES; role++) {
            redis_server_t *iserver;
            while (!VTAILQ_EMPTY(&db->servers[weight][role])) {
                iserver = VTAILQ_FIRST(&db->servers[weight][role]);
                CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                VTAILQ_REMOVE(&db->servers[weight][role], iserver, list);
                free_redis_server(iserver);
            }
        }
    }

    free((void *) db->name);
    db->name = NULL;
    db->connection_timeout = (struct timeval){ 0 };
    db->connection_ttl = 0;
    db->command_timeout = (struct timeval){ 0 };
    db->max_command_retries = 0;
    db->shared_connections = 0;
    db->max_connections = 0;
    if (db->password != NULL) {
        free((void *) db->password);
        db->password = NULL;
    }
    db->sickness_ttl = 0;
    db->ignore_slaves = 0;

    db->cluster.enabled = 0;
    db->cluster.max_hops = 0;

    db->stats.servers.total = 0;
    db->stats.servers.failed = 0;
    db->stats.connections.total = 0;
    db->stats.connections.failed = 0;
    db->stats.connections.dropped.error = 0;
    db->stats.connections.dropped.hung_up = 0;
    db->stats.connections.dropped.overflow = 0;
    db->stats.connections.dropped.ttl = 0;
    db->stats.connections.dropped.version = 0;
    db->stats.connections.dropped.sick = 0;
    db->stats.workers.blocked = 0;
    db->stats.commands.total = 0;
    db->stats.commands.failed = 0;
    db->stats.commands.retried = 0;
    db->stats.commands.error = 0;
    db->stats.commands.noscript = 0;
    db->stats.cluster.discoveries.total = 0;
    db->stats.cluster.discoveries.failed = 0;
    db->stats.cluster.replies.moved = 0;
    db->stats.cluster.replies.ask = 0;

    FREE_OBJ(db);
}

task_state_t *
new_task_state()
{
    task_state_t *result;
    ALLOC_OBJ(result, TASK_STATE_MAGIC);
    AN(result);

    result->ncontexts = 0;
    VTAILQ_INIT(&result->contexts);

    result->db = NULL;

    result->command.db = NULL;
    result->command.timeout = (struct timeval){ 0 };
    result->command.max_retries = 0;
    result->command.argc = 0;
    result->command.reply = NULL;

    return result;
}

void
free_task_state(task_state_t *state)
{
    state->ncontexts = 0;
    redis_context_t *icontext;
    while (!VTAILQ_EMPTY(&state->contexts)) {
        icontext = VTAILQ_FIRST(&state->contexts);
        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
        VTAILQ_REMOVE(&state->contexts, icontext, list);
        free_redis_context(icontext);
    }

    state->db = NULL;

    state->command.db = NULL;
    state->command.timeout = (struct timeval){ 0 };
    state->command.max_retries = 0;
    state->command.argc = 0;
    if (state->command.reply != NULL) {
        freeReplyObject(state->command.reply);
    }

    FREE_OBJ(state);
}

vcl_state_t *
new_vcl_state()
{
    vcl_state_t *result;
    ALLOC_OBJ(result, VCL_STATE_MAGIC);
    AN(result);

    Lck_New(&result->mutex, vmod_state.locks.config);

    VTAILQ_INIT(&result->subnets);

    VTAILQ_INIT(&result->dbs);

    result->sentinels.locations = NULL;
    result->sentinels.period = 0;
    result->sentinels.connection_timeout = (struct timeval){ 0 };
    result->sentinels.command_timeout = (struct timeval){ 0 };
    result->sentinels.thread = 0;
    result->sentinels.active = 0;
    result->sentinels.discovery = 0;

    return result;
}

void
free_vcl_state(vcl_state_t *priv)
{
    Lck_Delete(&priv->mutex);

    subnet_t *isubnet;
    while (!VTAILQ_EMPTY(&priv->subnets)) {
        isubnet = VTAILQ_FIRST(&priv->subnets);
        CHECK_OBJ_NOTNULL(isubnet, SUBNET_MAGIC);
        VTAILQ_REMOVE(&priv->subnets, isubnet, list);
        free_subnet(isubnet);
    }

    database_t *idb;
    while (!VTAILQ_EMPTY(&priv->dbs)) {
        idb = VTAILQ_FIRST(&priv->dbs);
        CHECK_OBJ_NOTNULL(idb, DATABASE_MAGIC);
        VTAILQ_REMOVE(&priv->dbs, idb, list);
        free_database(idb);
    }

    if (priv->sentinels.locations != NULL) {
        free((void *) priv->sentinels.locations);
        priv->sentinels.locations = NULL;
    }
    priv->sentinels.period = 0;
    priv->sentinels.connection_timeout = (struct timeval){ 0 };
    priv->sentinels.command_timeout = (struct timeval){ 0 };
    priv->sentinels.thread = 0;
    priv->sentinels.active = 0;
    priv->sentinels.discovery = 0;

    FREE_OBJ(priv);
}

subnet_t *
new_subnet(unsigned weight, struct in_addr ia4, unsigned bits)
{
    subnet_t *result;
    ALLOC_OBJ(result, SUBNET_MAGIC);
    AN(result);

    result->weight = weight;
    result->mask.s_addr = (bits == 0 ? 0x0 : (0xffffffff << (32 - bits)));
    result->address.s_addr = ntohl(ia4.s_addr) & result->mask.s_addr;

    return result;
}

void
free_subnet(subnet_t *subnet)
{
    subnet->weight = 0;
    subnet->mask = (struct in_addr){ 0 };
    subnet->address = (struct in_addr){ 0 };

    FREE_OBJ(subnet);
}

database_t *
new_database(struct vmod_redis_db *db)
{
    database_t *result;
    ALLOC_OBJ(result, DATABASE_MAGIC);
    AN(result);

    result->db = db;

    return result;
}

void
free_database(database_t *db)
{
    free_vmod_redis_db(db->db);
    db->db = NULL;

    FREE_OBJ(db);
}

redisReply *
redis_execute(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct timeval timeout,
    unsigned max_retries, unsigned argc, const char *argv[], unsigned *retries,
    redis_server_t *server, unsigned asking, unsigned master, unsigned slot)
{
    // Assertions.
    assert(*retries <= max_retries);

    // Initializations.
    redisReply *result = NULL;

    // Build the execution plan.
    struct plan *plan = plan_execution(
        ctx, db, state,
        (*retries > 0) ? max_retries - *retries : 1 + max_retries - *retries,
        server, master, slot);

    // Do not continue if an execution plan is not available of if it's empty.
    if ((plan != NULL) &&
        ((plan->contexts.n > 0) || (plan->servers.n > 0))) {
        // Execute command, retrying up to some limit.
        while ((result == NULL) && (!WS_Overflowed(ctx->ws))) {
            // Initializations.
            redis_context_t *context = lock_redis_context(ctx, db, state, plan);

            // Do not continue if a context is not available.
            if (context != NULL) {
                // Initializations.
                unsigned done = 0;

                // When executing EVAL commands, first try with EVALSHA.
                if ((strcasecmp(argv[0], "EVAL") == 0) && (argc >= 2)) {
                    // Replace EVAL with EVALSHA.
                    argv[0] = WS_Copy(ctx->ws, "EVALSHA", -1);
                    if (argv[0] == NULL) {
                        *retries = max_retries;
                        REDIS_LOG_ERROR(ctx,
                            "Failed to allocate memory in workspace (ws=%p)",
                            ctx->ws);
                        goto unlock;
                    }
                    const char *script = argv[1];
                    argv[1] = sha1(ctx, script);
                    if (argv[1] == NULL) {
                        *retries = max_retries;
                        goto unlock;
                    }

                    // Execute the EVALSHA command.
                    result = get_redis_repy(ctx, context, timeout, argc, argv, asking);

                    // Check reply. If replied with a NOSCRIPT, the original
                    // EVAL command should be executed to register the script
                    // for the first time in the Redis server.
                    if (!context->rcontext->err &&
                        (result != NULL) &&
                        (result->type == REDIS_REPLY_ERROR) &&
                        (strncmp(result->str, "NOSCRIPT", 8) == 0)) {
                        // Replace EVALSHA with EVAL.
                        argv[0] = WS_Copy(ctx->ws, "EVAL", -1);
                        if (argv[0] == NULL) {
                            *retries = max_retries;
                            REDIS_LOG_ERROR(ctx,
                                "Failed to allocate memory in workspace (ws=%p)",
                                ctx->ws);
                            goto unlock;
                        }
                        argv[1] = script;

                        // Release previous reply object.
                        freeReplyObject(result);
                        result = NULL;

                        // Update stats.
                        Lck_Lock(&db->mutex);
                        db->stats.commands.noscript++;
                        Lck_Unlock(&db->mutex);

                    // Command execution is completed.
                    } else {
                        done = 1;
                    }
                }

                // Send command, unless it was originally an EVAL command and it
                // was already executed using EVALSHA.
                if (!done) {
                    result = get_redis_repy(ctx, context, timeout, argc, argv, asking);
                }

                // Log failed executions.
                if (context->rcontext->err) {
                    REDIS_LOG_ERROR(ctx,
                        "Failed to execute command (command=%s, error=%d, db=%s, server=%s): %s",
                        argv[0], context->rcontext->err, db->name,
                        context->server->location.raw, context->rcontext->errstr);
                } else if (result == NULL) {
                    REDIS_LOG_ERROR(ctx,
                        "Failed to execute command (command=%s, db=%s, server=%s)",
                        argv[0], db->name, context->server->location.raw);
                }
    unlock:
                // Release context.
                unlock_redis_context(ctx, db, state, context);

            // Context not available.
            } else {
                REDIS_LOG_ERROR(ctx,
                    "Failed to execute command (command=%s, db=%s): context not available",
                    argv[0], db->name);
            }

            // Update stats.
            Lck_Lock(&db->mutex);
            if (*retries > 0) {
                db->stats.commands.retried++;
            }
            if (result == NULL) {
                db->stats.commands.failed++;
            } else {
                db->stats.commands.total++;
            }
            Lck_Unlock(&db->mutex);

            // Try again?
            if (result == NULL) {
                if (*retries < max_retries) {
                    (*retries)++;
                } else {
                    break;
                }
            }
        }

    // Execution plan not available.
    } else {
        *retries = max_retries;
        REDIS_LOG_ERROR(ctx,
            "Failed to execute command (command=%s, db=%s): execution plan not available",
            argv[0], db->name);
    }

    // Done!
    return result;
}

redis_server_t *
unsafe_add_redis_server(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config,
    const char *location, enum REDIS_SERVER_ROLE role)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    Lck_AssertHeld(&db->mutex);

    // Initializations.
    redis_server_t *result = NULL;

    // Look for a server matching the location. If found, remove if from the
    // list. It would be reinserted later, perhaps in a different list.
    for (unsigned iweight = 0;
         result == NULL && iweight < NREDIS_SERVER_WEIGHTS;
         iweight++) {
        for (enum REDIS_SERVER_ROLE irole = 0;
             result == NULL && irole < NREDIS_SERVER_ROLES;
             irole++) {
            redis_server_t *iserver;
            VTAILQ_FOREACH(iserver, &db->servers[iweight][irole], list) {
                CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                if (strcmp(iserver->location.raw, location) == 0) {
                    VTAILQ_REMOVE(&db->servers[iweight][irole], iserver, list);
                    result = iserver;
                    break;
                }
            }
        }
    }

    // Create a new server instance?
    if (result == NULL) {
        // Create new instance.
        result = new_redis_server(db, location, role);
        if (result != NULL) {
            // If role is unknown try to discover it. This won't be retried. On
            // failures discovering the role the module will depend on other
            // discovery capabilities such as Redis Sentinel or Redis Cluster.
            if (result->role == REDIS_SERVER_TBD_ROLE) {
                result->role = unsafe_discover_redis_server_role(ctx, result);
            }

            // Calculate weight.
            if (result->location.type == REDIS_SERVER_LOCATION_HOST_TYPE) {
                struct in_addr ia4;
                if (inet_pton(AF_INET, result->location.parsed.address.host, &ia4)) {
                    result->weight = NREDIS_SERVER_WEIGHTS - 1;
                    subnet_t *isubnet;
                    VTAILQ_FOREACH(isubnet, &config->subnets, list) {
                        CHECK_OBJ_NOTNULL(isubnet, SUBNET_MAGIC);
                        if ((ntohl(ia4.s_addr) & isubnet->mask.s_addr) ==
                            (isubnet->address.s_addr & isubnet->mask.s_addr)) {
                            result->weight = isubnet->weight;
                            break;
                        }
                    }
                } else {
                    result->weight = NREDIS_SERVER_WEIGHTS - 1;
                }
            } else {
                result->weight = 0;
            }

            // Log event.
            REDIS_LOG_INFO(ctx,
                "New server registered (db=%s, server=%s, role=%d, weight=%d)",
                db->name, result->location.raw, result->role, result->weight);

            // Update stats.
            db->stats.servers.total++;
        } else {
            REDIS_LOG_ERROR(ctx,
                "Failed to register server (db=%s, server=%s)",
                db->name, location);
            db->stats.servers.failed++;
        }

    // Update existing server instance.
    } else {
        // If role is unknown try to discover it. This won't be retried. On
        // failures the module stays with the info it has so far. Discovery of
        // the real role will depend on other discovery capabilities such as
        // Redis Sentinel or Redis Cluster.
        if (role == REDIS_SERVER_TBD_ROLE) {
            enum REDIS_SERVER_ROLE r = unsafe_discover_redis_server_role(ctx, result);
            if (r != REDIS_SERVER_TBD_ROLE) {
                result->role = r;
            }
        } else {
            result->role = role;
        }

        // Flush the sickness flag.
        unsigned now = time(NULL);
        if (result->sickness.exp > now) {
            result->sickness.exp = now;
        }

        // Log event.
        REDIS_LOG_INFO(ctx,
            "Server updated (db=%s, server=%s, role=%d, weight=%d)",
            db->name, result->location.raw, result->role, result->weight);
    }

    // Do not continue if a server instance is not available.
    if (result != NULL) {
        // Register server instance in the right list.
        VTAILQ_INSERT_TAIL(
            &db->servers[result->weight][result->role],
            result,
            list);
    }

    // Done!
    return result;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static redisContext *
new_rcontext(
    VRT_CTX, redis_server_t * server, time_t now,
    unsigned ephemeral, unsigned dblocked)
{
    // Assertions.
    if (dblocked) Lck_AssertHeld(&server->db->mutex);

    // Create context.
    redisContext *result;
    if ((server->db->connection_timeout.tv_sec > 0) ||
        (server->db->connection_timeout.tv_usec > 0)) {
        switch (server->location.type) {
            case REDIS_SERVER_LOCATION_HOST_TYPE:
                result = redisConnectWithTimeout(
                    server->location.parsed.address.host,
                    server->location.parsed.address.port,
                    server->db->connection_timeout);
                break;

            case REDIS_SERVER_LOCATION_SOCKET_TYPE:
                result = redisConnectUnixWithTimeout(
                    server->location.parsed.path,
                    server->db->connection_timeout);
                break;

            default:
                result = NULL;
        }
    } else {
        switch (server->location.type) {
            case REDIS_SERVER_LOCATION_HOST_TYPE:
                result = redisConnect(
                    server->location.parsed.address.host,
                    server->location.parsed.address.port);
                break;

            case REDIS_SERVER_LOCATION_SOCKET_TYPE:
                result = redisConnectUnix(
                    server->location.parsed.path);
                break;

            default:
                result = NULL;
        }
    }
    AN(result);

    // Check created context.
    if (result->err) {
        REDIS_LOG_ERROR(ctx,
            "Failed to establish connection (error=%d, db=%s, server=%s): %s",
            result->err, server->db->name, server->location.raw, result->errstr);
        redisFree(result);
        result = NULL;
    }

    // Submit AUTH command.
    if ((result != NULL) && (server->db->password != NULL)) {
        REDIS_AUTH(
            ctx, result, server->db->password,
            "Failed to authenticate connection",
            "db=%s, server=%s",
            server->db->name, server->location.raw);
    }

    // Update stats & sickness flag.
    if (!dblocked) Lck_Lock(&server->db->mutex);
    if (result != NULL) {
        if (server->sickness.exp > now) {
            server->sickness.exp = now;
            REDIS_LOG_INFO(ctx,
                "Server sickness tag cleared (db=%s, server=%s)",
                server->db->name, server->location.raw);
        }
        if (!ephemeral) {
            server->db->stats.connections.total++;
        }
    } else {
        server->sickness.tst = now;
        server->sickness.exp = now + server->db->sickness_ttl;
        if (!ephemeral) {
            server->db->stats.connections.failed++;
        }
        REDIS_LOG_INFO(ctx,
            "Server sickness tag set (db=%s, server=%s)",
            server->db->name, server->location.raw);
    }
    if (!dblocked) Lck_Unlock(&server->db->mutex);

#if HIREDIS_MAJOR >= 0 && HIREDIS_MINOR >= 12
    // Enable TCP keep-alive.
    if ((result != NULL) &&
        (server->location.type == REDIS_SERVER_LOCATION_HOST_TYPE)) {
        redisEnableKeepAlive(result);
    }
#endif

    // Done!
    return result;
}

static enum REDIS_SERVER_ROLE
unsafe_discover_redis_server_role(VRT_CTX, redis_server_t *server)
{
    // Assertions.
    Lck_AssertHeld(&server->db->mutex);

    // Initializations.
    enum REDIS_SERVER_ROLE result = REDIS_SERVER_TBD_ROLE;

    // Create context.
    redisContext *rcontext = new_rcontext(ctx, server, time(NULL), 1, 1);
    if ((rcontext != NULL) && (!rcontext->err)) {
        // Set command execution timeout.
        int tr = redisSetTimeout(rcontext, server->db->command_timeout);
        if (tr != REDIS_OK) {
            REDIS_LOG_ERROR(ctx,
                "Failed to set role discovery command execution timeout (error=%d, db=%s, server=%s)",
                tr, server->db->name, server->location.raw);
        }

        // Send command.
        redisReply *reply = redisCommand(rcontext, ROLE_DISCOVERY_COMMAND);

        // Check reply.
        if ((!rcontext->err) &&
            (reply != NULL) &&
            (reply->type == REDIS_REPLY_ARRAY) &&
            (reply->elements > 0) &&
            (reply->element[0]->type == REDIS_REPLY_STRING)) {
            if (strcmp(reply->element[0]->str, "master") == 0) {
                result = REDIS_SERVER_MASTER_ROLE;
            } else if (strcmp(reply->element[0]->str, "slave") == 0) {
                result = REDIS_SERVER_SLAVE_ROLE;
            }
            if (result != REDIS_SERVER_TBD_ROLE) {
                REDIS_LOG_INFO(ctx,
                    "Server role discovered (db=%s, server=%s, role=%d)",
                    server->db->name, server->location.raw, result);
            }
        } else {
            REDIS_LOG_ERROR(ctx,
                "Failed to execute role discovery command (db=%s, server=%s)",
                server->db->name, server->location.raw);
        }

        // Release reply.
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    } else {
        if (rcontext != NULL) {
            REDIS_LOG_ERROR(ctx,
                "Failed to establish role discovery connection (error=%d, db=%s, server=%s): %s",
                rcontext->err, server->db->name, server->location.raw, rcontext->errstr);
        } else {
            REDIS_LOG_ERROR(ctx,
                "Failed to establish role discovery connection (db=%s, server=%s)",
                server->db->name, server->location.raw);
        }
    }

    // Release context.
    if (rcontext != NULL) {
        redisFree(rcontext);
    }

    // Done!
    return result;
}

static unsigned
is_valid_redis_context(redis_context_t *context, time_t now, unsigned dblocked)
{
    // Assertions.
    if (dblocked) Lck_AssertHeld(&context->server->db->mutex);

    // Check if context is in an error state.
    if (context->rcontext->err) {
        if (!dblocked) Lck_Lock(&context->server->db->mutex);
        context->server->db->stats.connections.dropped.error++;
        if (!dblocked) Lck_Unlock(&context->server->db->mutex);
        return 0;
    }

    // Check if context is too old (version).
    if (context->version != vmod_state.version) {
        if (!dblocked) Lck_Lock(&context->server->db->mutex);
        context->server->db->stats.connections.dropped.version++;
        if (!dblocked) Lck_Unlock(&context->server->db->mutex);
        return 0;
    }

    // Check if context is too old (TTL).
    if ((context->server->db->connection_ttl > 0) &&
        (now - context->tst > context->server->db->connection_ttl)) {
        if (!dblocked) Lck_Lock(&context->server->db->mutex);
        context->server->db->stats.connections.dropped.ttl++;
        if (!dblocked) Lck_Unlock(&context->server->db->mutex);
        return 0;
    }

    // Check if context was created before the server was flagged
    // as sick.
    unsigned sick = 0;
    if (!dblocked) Lck_Lock(&context->server->db->mutex);
    if (context->tst <= context->server->sickness.tst) {
        sick = 1;
        context->server->db->stats.connections.dropped.sick++;
    }
    if (!dblocked) Lck_Unlock(&context->server->db->mutex);
    if (sick) {
        return 0;
    }

    // Check if context connection has been hung up by the server.
    struct pollfd fds;
    fds.fd = context->rcontext->fd;
    fds.events = POLLOUT;
    if ((poll(&fds, 1, 0) != 1) || (fds.revents & POLLHUP)) {
        if (!dblocked) Lck_Lock(&context->server->db->mutex);
        context->server->db->stats.connections.dropped.hung_up++;
        if (!dblocked) Lck_Unlock(&context->server->db->mutex);
        return 0;
    }

    // Valid!
    return 1;
}

static struct plan *
new_execution_plan(VRT_CTX, struct vmod_redis_db *db)
{
    struct plan *result = (void *)WS_Alloc(ctx->ws, sizeof(struct plan));
    if (result == NULL) {
        REDIS_LOG_ERROR(ctx,
            "Failed to allocate memory in workspace (ws=%p)",
            ctx->ws);
        return NULL;
    }

    result->contexts.n = 0;
    result->contexts.next = 0;
    result->contexts.list = NULL;

    result->servers.n = 0;
    result->servers.next = 0;
    result->servers.list = NULL;

    return result;
}

void
populate_simple_execution_plan(
    VRT_CTX, struct plan *plan, struct vmod_redis_db *db, task_state_t *state,
    unsigned max_size, redis_server_t *server)
{
    // Populate list of contexts?
    if (!db->shared_connections) {
        // Initializations.
        time_t now = time(NULL);
        unsigned free_ws = WS_Reserve(ctx->ws, 0);
        unsigned used_ws = 0;
        plan->contexts.list = (redis_context_t **) WS_Front(ctx->ws);
        plan->contexts.n = 0;

        // Search for contexts matching the requested conditions.
        redis_context_t *icontext, *icontext_tmp;
        VTAILQ_FOREACH_SAFE(icontext, &state->contexts, list, icontext_tmp) {
            CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
            if ((icontext->server->db == db) &&
                (icontext->server == server)) {
                if (is_valid_redis_context(icontext, now, 0)) {
                    if (free_ws >= sizeof(redis_context_t *)) {
                        used_ws += sizeof(redis_context_t *);
                        plan->contexts.list[plan->contexts.n++] = icontext;
                        if (plan->contexts.n == max_size) {
                            break;
                        }
                    } else {
                        WS_Release(ctx->ws, used_ws);
                        WS_MarkOverflow(ctx->ws);
                        REDIS_LOG_ERROR(ctx,
                            "Failed to allocate memory in workspace (ws=%p)",
                            ctx->ws);
                        return;
                    }
                } else {
                    VTAILQ_REMOVE(&state->contexts, icontext, list);
                    state->ncontexts--;
                    free_redis_context(icontext);
                }
            }
        }

        // Done!
        WS_Release(ctx->ws, used_ws);
    }

    // Build list of servers.
    unsigned free_ws = WS_Reserve(ctx->ws, 0);
    if (free_ws >= sizeof(redis_server_t *)) {
        plan->servers.list = (redis_server_t **) WS_Front(ctx->ws);
        plan->servers.n = 1;
        plan->servers.list[0] = server;
        WS_Release(ctx->ws, sizeof(redis_server_t *));
    } else {
        WS_Release(ctx->ws, 0);
        WS_MarkOverflow(ctx->ws);
        REDIS_LOG_ERROR(ctx,
            "Failed to allocate memory in workspace (ws=%p)",
            ctx->ws);
    }
}

void
populate_execution_plan(
    VRT_CTX, struct plan *plan, struct vmod_redis_db *db, task_state_t *state,
    unsigned max_size, unsigned master, unsigned slot)
{
    // Initializations.
    time_t now = time(NULL);

    // Populate list of contexts?
    if (!db->shared_connections) {
        // Initializations.
        unsigned free_ws = WS_Reserve(ctx->ws, 0);
        unsigned used_ws = 0;
        plan->contexts.list = (redis_context_t **) WS_Front(ctx->ws);
        plan->contexts.n = 0;

        // Search for contexts matching the requested conditions.
        redis_context_t *icontext, *icontext_tmp;
        VTAILQ_FOREACH_SAFE(icontext, &state->contexts, list, icontext_tmp) {
            CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
            if ((icontext->server->db == db) &&
                ((master && (icontext->server->role == REDIS_SERVER_MASTER_ROLE)) ||
                 (!master && (icontext->server->role != REDIS_SERVER_MASTER_ROLE))) &&
                ((!db->cluster.enabled) ||
                 (icontext->server->cluster.slots[slot]))) {
                if (is_valid_redis_context(icontext, now, 0)) {
                    if (free_ws >= sizeof(redis_context_t *)) {
                        used_ws += sizeof(redis_context_t *);
                        plan->contexts.list[plan->contexts.n++] = icontext;
                        if (plan->contexts.n == max_size) {
                            break;
                        }
                    } else {
                        WS_Release(ctx->ws, used_ws);
                        WS_MarkOverflow(ctx->ws);
                        REDIS_LOG_ERROR(ctx,
                            "Failed to allocate memory in workspace (ws=%p)",
                            ctx->ws);
                        return;
                    }
                } else {
                    VTAILQ_REMOVE(&state->contexts, icontext, list);
                    state->ncontexts--;
                    free_redis_context(icontext);
                }
            }
        }

        // If some context was added to the execution plan, move it to the end
        // of the list. This ensures a nice distribution of load between all
        // available contexts.
        if (plan->contexts.n > 0) {
            VTAILQ_REMOVE(&state->contexts, plan->contexts.list[0], list);
            VTAILQ_INSERT_TAIL(&state->contexts, plan->contexts.list[0], list);
        }

        // Done!
        WS_Release(ctx->ws, used_ws);
    }

    // Populate list of servers?
    if (plan->contexts.n < max_size) {
        // Initializations.
        unsigned remaining = max_size - plan->contexts.n;
        unsigned free_ws = WS_Reserve(ctx->ws, 0);
        unsigned used_ws = 0;
        plan->servers.list = (redis_server_t **) WS_Front(ctx->ws);
        plan->servers.n = 0;

        // Get database lock.
        Lck_Lock(&db->mutex);

        // Build list of servers.
        //   - First round:
        //       + Give higher priority to:
        //           * Slave servers.
        //           * Servers with lower weight.
        //       + Skip servers not matching 'slot' if clustering is enabled.
        //       + Skip sick servers.
        //       + Skip slave & TBD servers if 'master' is set.
        //   - Second round (only if clustering is disabled):
        //       + Consider sick and TBD servers skipped during the first round.
        for (unsigned round = 1; round <= (db->cluster.enabled ? 1 : 2); round++) {
            for (unsigned iweight = 0;
                 remaining > 0 && iweight < NREDIS_SERVER_WEIGHTS;
                 iweight++) {
                for (enum REDIS_SERVER_ROLE irole = 0;
                     remaining > 0 && irole < NREDIS_SERVER_ROLES;
                     irole++) {
                    if ((!master) ||
                        (((round == 1) &&
                          (irole == REDIS_SERVER_MASTER_ROLE)) ||
                         ((round == 2) &&
                          ((irole == REDIS_SERVER_MASTER_ROLE) ||
                           (irole == REDIS_SERVER_TBD_ROLE))))) {
                        unsigned nservers = plan->servers.n;
                        redis_server_t *iserver;
                        VTAILQ_FOREACH(iserver, &db->servers[iweight][irole], list) {
                            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                            assert(iserver->weight == iweight);
                            assert(iserver->role == irole);
                            if (((!db->cluster.enabled) ||
                                 (iserver->cluster.slots[slot])) &&
                                (((round == 1) &&
                                  (iserver->sickness.exp <= now)) ||
                                 ((round == 2) &&
                                  (((master) &&
                                    (iserver->role == REDIS_SERVER_TBD_ROLE)) ||
                                   (iserver->sickness.exp > now))))) {
                                if (free_ws >= sizeof(redis_server_t *)) {
                                    used_ws += sizeof(redis_server_t *);
                                    plan->servers.list[plan->servers.n++] = iserver;
                                    if (--remaining == 0) {
                                        break;
                                    }
                                } else {
                                    WS_Release(ctx->ws, used_ws);
                                    WS_MarkOverflow(ctx->ws);
                                    REDIS_LOG_ERROR(ctx,
                                        "Failed to allocate memory in workspace (ws=%p)",
                                        ctx->ws);
                                    return;
                                }
                            }
                        }

                        // If some server in this list was added to the
                        // execution plan, move it to the end of the list. This
                        // ensures a nice distribution of load between all
                        // servers.
                        if ((round == 1) && (nservers < plan->servers.n)) {
                            VTAILQ_REMOVE(
                                &db->servers[iweight][irole],
                                plan->servers.list[nservers],
                                list);
                            VTAILQ_INSERT_TAIL(
                                &db->servers[iweight][irole],
                                plan->servers.list[nservers],
                                list);
                        }
                    }
                }
            }
        }

        // Continue building list of servers.
        //   - Only executed when clustering is enabled and if the execution
        //   plan is still empty. Any server will be ok to get a redirection to
        //   the right server and the trigger a discovery of the cluster
        //   topology.
        //   - Give higher priority to:
        //       + Slave servers.
        //       + Servers with lower weight.
        //   - Skip sick servers during the third round.
        //   - Skip healthy servers during the fourth round.
        if ((db->cluster.enabled) && (plan->servers.n == 0)) {
            for (unsigned round = 3; round <= 4; round++) {
                for (unsigned iweight = 0;
                     remaining > 0 && iweight < NREDIS_SERVER_WEIGHTS;
                     iweight++) {
                    for (enum REDIS_SERVER_ROLE irole = 0;
                         remaining > 0 && irole < NREDIS_SERVER_ROLES;
                         irole++) {
                        redis_server_t *iserver;
                        VTAILQ_FOREACH(iserver, &db->servers[iweight][irole], list) {
                            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                            assert(iserver->weight == iweight);
                            assert(iserver->role == irole);
                            if (((round == 3) &&
                                 (iserver->sickness.exp <= now)) ||
                                ((round == 4) &&
                                 (iserver->sickness.exp > now))) {
                                if (free_ws >= sizeof(redis_server_t *)) {
                                    used_ws += sizeof(redis_server_t *);
                                    plan->servers.list[plan->servers.n++] = iserver;
                                    if (--remaining == 0) {
                                        break;
                                    }
                                } else {
                                    WS_Release(ctx->ws, used_ws);
                                    WS_MarkOverflow(ctx->ws);
                                    REDIS_LOG_ERROR(ctx,
                                        "Failed to allocate memory in workspace (ws=%p)",
                                        ctx->ws);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Release database lock.
        Lck_Unlock(&db->mutex);

        // Done!
        WS_Release(ctx->ws, used_ws);
    }
}

static struct plan *
plan_execution(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state,
    unsigned max_size, redis_server_t *server, unsigned master, unsigned slot)
{
    // Initializations.
    struct plan *result = new_execution_plan(ctx, db);

    // Do not continue if something failed while creating an empty execution
    // plan.
    if (result != NULL) {
        if (server != NULL) {
            populate_simple_execution_plan(ctx, result, db, state, max_size, server);
        } else {
            populate_execution_plan(ctx, result, db, state, max_size, master, slot);
        }
    }

    // Done!
    return result;
}

static redis_context_t *
lock_private_redis_context(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct plan *plan)
{
    // Initializations.
    redis_context_t *result = NULL;

    // Is there any context in the execution plan?
    if (plan->contexts.next < plan->contexts.n) {
        result = plan->contexts.list[plan->contexts.next++];

    // No contexts in the execution plan. Create new context according with
    // the execution plan. If any error arises discard the context and continue.
    } else {
        // Initializations.
        time_t now = time(NULL);

        // Select next server in the execution plan.
        assert(plan->servers.next < plan->servers.n);
        redis_server_t *server = plan->servers.list[plan->servers.next];
        plan->servers.next = (plan->servers.next + 1) % plan->servers.n;

        // If an empty slot is not available, release an existing context.
        if (state->ncontexts >= db->max_connections) {
            redis_context_t *context = VTAILQ_FIRST(&state->contexts);
            CHECK_OBJ_NOTNULL(context, REDIS_CONTEXT_MAGIC);
            VTAILQ_REMOVE(&state->contexts, context, list);
            state->ncontexts--;
            free_redis_context(context);
            Lck_Lock(&db->mutex);
            db->stats.connections.dropped.overflow++;
            Lck_Unlock(&db->mutex);
        }

        // Create new context using the previously selected server. If any
        // error arises discard the context and return.
        redisContext *rcontext = new_rcontext(ctx, server, now, 0, 0);
        if (rcontext != NULL) {
            result = new_redis_context(server, rcontext, now);
            VTAILQ_INSERT_TAIL(&state->contexts, result, list);
            state->ncontexts++;
        }
    }

    // Done!
    return result;
}

static redis_context_t *
lock_shared_redis_context(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct plan *plan)
{
    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Select next server in the execution plan.
    assert(plan->servers.next < plan->servers.n);
    redis_server_t *server = plan->servers.list[plan->servers.next];
    plan->servers.next = (plan->servers.next + 1) % plan->servers.n;

    // Get database lock.
    Lck_Lock(&server->db->mutex);

retry:
    // Look for an existing free context.
    while (!VTAILQ_EMPTY(&server->pool.free_contexts)) {
        // Extract context.
        result = VTAILQ_FIRST(&server->pool.free_contexts);
        CHECK_OBJ_NOTNULL(result, REDIS_CONTEXT_MAGIC);

        // Mark the context as busy.
        VTAILQ_REMOVE(&server->pool.free_contexts, result, list);
        VTAILQ_INSERT_TAIL(&server->pool.busy_contexts, result, list);

        // Is the context valid?
        if (!is_valid_redis_context(result, now, 1)) {
            // Release context.
            VTAILQ_REMOVE(&server->pool.busy_contexts, result, list);
            server->pool.ncontexts--;
            free_redis_context(result);

            // A new context needs to be selected.
            result = NULL;

        // A valid free context was found.
        } else {
            break;
        }
    }

    // If required, create new context using the currently selected server. If any
    // error arises discard the context and continue. If maximum number of contexts
    // has been reached, wait for another thread releasing some context.
    if (result == NULL) {
        // If an no more contexts can be created, wait for another thread.
        if (server->pool.ncontexts >= db->max_connections) {
            Lck_CondWait(&server->pool.cond, &server->db->mutex, 0);
            db->stats.workers.blocked++;
            goto retry;
        }

        // Create new context using the previously selected server. If any
        // error arises discard the context and return.
        redisContext *rcontext = new_rcontext(ctx, server, now, 0, 1);
        if (rcontext != NULL) {
            result = new_redis_context(server, rcontext, now);
            VTAILQ_INSERT_TAIL(&server->pool.busy_contexts, result, list);
            server->pool.ncontexts++;
        }
    }

    // Release database lock.
    Lck_Unlock(&server->db->mutex);

    // Done!
    return result;
}

static redis_context_t *
lock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct plan *plan)
{
    if (db->shared_connections) {
        return lock_shared_redis_context(ctx, db, state, plan);
    } else {
        return lock_private_redis_context(ctx, db, state, plan);
    }
}

static void
unlock_shared_redis_context(
    VRT_CTX, struct vmod_redis_db *db, redis_context_t *context)
{
    // Assertions.
    CHECK_OBJ_NOTNULL(context, REDIS_CONTEXT_MAGIC);
    CHECK_OBJ_NOTNULL(context->server, REDIS_SERVER_MAGIC);

    // Return context to the pool's free list.
    Lck_Lock(&context->server->db->mutex);
    VTAILQ_REMOVE(&context->server->pool.busy_contexts, context, list);
    VTAILQ_INSERT_TAIL(&context->server->pool.free_contexts, context, list);
    AZ(pthread_cond_signal(&context->server->pool.cond));
    Lck_Unlock(&context->server->db->mutex);
}

static void
unlock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state,  redis_context_t *context)
{
    if (db->shared_connections) {
        return unlock_shared_redis_context(ctx, db, context);
    }
}

static redisReply *
get_redis_repy(
    VRT_CTX, redis_context_t *context, struct timeval timeout, unsigned argc,
    const char *argv[], unsigned asking)
{
    // Initializations.
    redisReply *result = NULL;
    redisReply *reply;
    unsigned readonly =
        ((context->server->db->cluster.enabled) &&
         (context->server->role == REDIS_SERVER_SLAVE_ROLE));

    // Set command execution timeout.
    int tr = redisSetTimeout(context->rcontext, timeout);
    if (tr != REDIS_OK) {
        REDIS_LOG_ERROR(ctx,
            "Failed to set command execution timeout (error=%d, db=%s, server=%s)",
            tr, context->server->db->name, context->server->location.raw);
    }

    // Build pipeline.
    if (readonly) {
        redisAppendCommand(context->rcontext, "READONLY");
    }
    if (asking) {
        redisAppendCommand(context->rcontext, "ASKING");
    }
    redisAppendCommandArgv(context->rcontext, argc, argv, NULL);
    if (readonly) {
        redisAppendCommand(context->rcontext, "READWRITE");
    }

    // Fetch READONLY command reply?
    if (readonly) {
        reply = NULL;
        redisGetReply(context->rcontext, (void **)&reply);
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    }

    // Fetch ASKING command reply.
    if (asking) {
        reply = NULL;
        redisGetReply(context->rcontext, (void **)&reply);
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    }

    // Fetch command reply.
    redisGetReply(context->rcontext, (void **)&result);

    // Fetch READWRITE command reply?
    if (readonly) {
        reply = NULL;
        redisGetReply(context->rcontext, (void **)&reply);
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    }

    // Done!
    return result;
}

static const char *
sha1(VRT_CTX, const char *script)
{
    // Hash.
    unsigned char buffer[20];
    SHA1_CTX sha1_ctx;
    SHA1Init(&sha1_ctx);
    SHA1Update(&sha1_ctx, (const unsigned char *) script, strlen(script));
    SHA1Final(buffer, &sha1_ctx);

    // Encode.
    char *result = WS_Alloc(ctx->ws, 41);;
    if (result != NULL) {
        char *ptr = result;
        for (int i = 0; i < 20; i++) {
            sprintf(ptr, "%02x", buffer[i]);
            ptr += 2;
        }
    } else {
        REDIS_LOG_ERROR(ctx,
            "Failed to allocate memory in workspace (ws=%p)",
            ctx->ws);
    }

    // Done!
    return result;
}
