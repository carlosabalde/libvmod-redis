#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "cache/cache.h"

#include "sha1.h"
#include "core.h"

vmod_state_t vmod_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .version = 0
};

static redis_context_t *lock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, redis_server_t *server);

static void unlock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, redis_context_t *context);

static redisReply *get_redis_repy(
    VRT_CTX, redis_context_t *context, struct timeval timeout, unsigned argc,
    const char *argv[], unsigned asking);

static const char *sha1(VRT_CTX, const char *script);

redis_server_t *
new_redis_server(struct vmod_redis_db *db, const char *location)
{
    // Initializations.
    redis_server_t *result = NULL;
    char *ptr = strrchr(location, ':');

    // Do not continue if this is a clustered database but the location is not
    // provided using the host + port format.
    if (!db->cluster.enabled || (ptr != NULL)) {
        ALLOC_OBJ(result, REDIS_SERVER_MAGIC);
        AN(result);

        result->db = db;

        result->location.raw = strdup(location);
        AN(result->location.raw);
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

        AZ(pthread_mutex_init(&result->pool.mutex, NULL));
        AZ(pthread_cond_init(&result->pool.cond, NULL));

        result->pool.ncontexts = 0;
        VTAILQ_INIT(&result->pool.free_contexts);
        VTAILQ_INIT(&result->pool.busy_contexts);
    }

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

    AZ(pthread_mutex_destroy(&server->pool.mutex));
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
    const char *name, struct timeval connection_timeout, unsigned connection_ttl,
    struct timeval command_timeout, unsigned max_command_retries,
    unsigned shared_connections, unsigned max_connections, const char *password,
    unsigned clustered, unsigned max_cluster_hops)
{
    struct vmod_redis_db *result;
    ALLOC_OBJ(result, VMOD_REDIS_DB_MAGIC);
    AN(result);

    AZ(pthread_mutex_init(&result->mutex, NULL));

    result->name = strdup(name);
    AN(result->name);
    result->connection_timeout = connection_timeout;
    result->connection_ttl = connection_ttl;
    result->command_timeout = command_timeout;
    result->max_command_retries = max_command_retries;
    result->shared_connections = shared_connections;
    result->max_connections = max_connections;
    if (!clustered && (strlen(password) > 0)) {
        result->password = strdup(password);
        AN(result->password);
    } else {
        result->password = NULL;
    }

    VTAILQ_INIT(&result->servers);

    result->cluster.enabled = clustered;
    result->cluster.max_hops = max_cluster_hops;
    for (int i = 0; i < NREDIS_CLUSTER_SLOTS; i++) {
        result->cluster.slots[i] = NULL;
    }

    result->stats.servers.total = 0;
    result->stats.servers.failed = 0;
    result->stats.connections.total = 0;
    result->stats.connections.failed = 0;
    result->stats.connections.dropped.error = 0;
    result->stats.connections.dropped.hung_up = 0;
    result->stats.connections.dropped.overflow = 0;
    result->stats.connections.dropped.ttl = 0;
    result->stats.connections.dropped.version = 0;
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
    AZ(pthread_mutex_destroy(&db->mutex));

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

    redis_server_t *iserver;
    while (!VTAILQ_EMPTY(&db->servers)) {
        iserver = VTAILQ_FIRST(&db->servers);
        CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
        VTAILQ_REMOVE(&db->servers, iserver, list);
        free_redis_server(iserver);
    }

    db->cluster.enabled = 0;
    db->cluster.max_hops = 0;
    for (int i = 0; i < NREDIS_CLUSTER_SLOTS; i++) {
        db->cluster.slots[i] = NULL;
    }

    db->stats.servers.total = 0;
    db->stats.servers.failed = 0;
    db->stats.connections.total = 0;
    db->stats.connections.failed = 0;
    db->stats.connections.dropped.error = 0;
    db->stats.connections.dropped.hung_up = 0;
    db->stats.connections.dropped.overflow = 0;
    db->stats.connections.dropped.ttl = 0;
    db->stats.connections.dropped.version = 0;
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

thread_state_t *
new_thread_state()
{
    thread_state_t *result;
    ALLOC_OBJ(result, THREAD_STATE_MAGIC);
    AN(result);

    result->ncontexts = 0;
    VTAILQ_INIT(&result->contexts);

    result->command.db = NULL;
    result->command.timeout = (struct timeval){ 0 };
    result->command.max_retries = 0;
    result->command.argc = 0;
    result->command.reply = NULL;

    return result;
}

void
free_thread_state(thread_state_t *state)
{
    state->ncontexts = 0;
    redis_context_t *icontext;
    while (!VTAILQ_EMPTY(&state->contexts)) {
        icontext = VTAILQ_FIRST(&state->contexts);
        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
        VTAILQ_REMOVE(&state->contexts, icontext, list);
        free_redis_context(icontext);
    }

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

    return result;
}

void
free_vcl_state(vcl_state_t *priv)
{
    FREE_OBJ(priv);
}

redisReply *
redis_execute(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    redis_server_t *server, struct timeval timeout,
    unsigned argc, const char *argv[], unsigned asking)
{
    // Initializations.
    redisReply *result = NULL;
    redis_context_t *context = lock_redis_context(ctx, db, state, server);

    // Do not continue if a Redis context is not available.
    if (context != NULL) {
        // Initializations.
        unsigned done = 0;

        // When executing EVAL commands, first try with EVALSHA.
        if ((strcasecmp(argv[0], "EVAL") == 0) && (argc >= 2)) {
            // Replace EVAL with EVALSHA.
            argv[0] = WS_Copy(ctx->ws, "EVALSHA", -1);
            AN(argv[0]);
            const char *script = argv[1];
            argv[1] = sha1(ctx, script);

            // Execute the EVALSHA command.
            result = get_redis_repy(ctx, context, timeout, argc, argv, asking);

            // Check reply. If Redis replies with a NOSCRIPT, the original
            // EVAL command should be executed to register the script for
            // the first time in the Redis server.
            if (!context->rcontext->err &&
                (result != NULL) &&
                (result->type == REDIS_REPLY_ERROR) &&
                (strncmp(result->str, "NOSCRIPT", 8) == 0)) {
                // Replace EVALSHA with EVAL.
                argv[0] = WS_Copy(ctx->ws, "EVAL", -1);
                AN(argv[0]);
                argv[1] = script;

                // Release previous reply object.
                freeReplyObject(result);
                result = NULL;

                // Update stats.
                AZ(pthread_mutex_lock(&db->mutex));
                db->stats.commands.noscript++;
                AZ(pthread_mutex_unlock(&db->mutex));

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

        // Check reply.
        AZ(pthread_mutex_lock(&db->mutex));
        if (context->rcontext->err) {
            REDIS_LOG_ERROR(ctx,
                "Failed to execute command (command=%s, error=%d, db=%s, server=%s): %s",
                argv[0], context->rcontext->err, db->name,
                context->server->location.raw, context->rcontext->errstr);
            db->stats.commands.failed++;
        } else if (result == NULL) {
            REDIS_LOG_ERROR(ctx,
                "Failed to execute command (command=%s, db=%s, server=%s)",
                argv[0], db->name, context->server->location.raw);
            db->stats.commands.failed++;
        } else {
            db->stats.commands.total++;
        }
        AZ(pthread_mutex_unlock(&db->mutex));

        // Release context.
        unlock_redis_context(ctx, db, state, context);
    }

    // Done!
    return result;
}

redis_server_t *
unsafe_add_redis_server(VRT_CTX, struct vmod_redis_db *db, const char *location)
{
    // Assertions.
    //   - db->mutex locked.

    // Initializations.
    redis_server_t *result = NULL;

    // Look for a server matching the location.
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &db->servers, list) {
        CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
        if (strcmp(iserver->location.raw, location) == 0) {
            result = iserver;
            break;
        }
    }

    // Register new server if required.
    if (result == NULL) {
        result = new_redis_server(db, location);
        if (result != NULL) {
            VTAILQ_INSERT_TAIL(&db->servers, result, list);
            REDIS_LOG_INFO(ctx,
                "New server registered (db=%s, server=%s)",
                db->name, result->location.raw);
            db->stats.servers.total++;
        } else {
            REDIS_LOG_ERROR(ctx,
                "Failed to register server (db=%s, server=%s)",
                db->name, location);
            db->stats.servers.failed++;
        }
    }

    // Done!
    return result;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static unsigned
is_valid_redis_context(redis_context_t *context, time_t now)
{
    // Check if context is in an error state.
    if (context->rcontext->err) {
        AZ(pthread_mutex_lock(&context->server->db->mutex));
        context->server->db->stats.connections.dropped.error++;
        AZ(pthread_mutex_unlock(&context->server->db->mutex));
        return 0;

    // Check if context is too old (version).
    } else if (context->version != vmod_state.version) {
        AZ(pthread_mutex_lock(&context->server->db->mutex));
        context->server->db->stats.connections.dropped.version++;
        AZ(pthread_mutex_unlock(&context->server->db->mutex));
        return 0;

    // Check if context is too old (TTL).
    } else if ((context->server->db->connection_ttl > 0) &&
               (now - context->tst > context->server->db->connection_ttl)) {
        AZ(pthread_mutex_lock(&context->server->db->mutex));
        context->server->db->stats.connections.dropped.ttl++;
        AZ(pthread_mutex_unlock(&context->server->db->mutex));
        return 0;

    // Check if context connection has been hung up by the server.
    } else {
        struct pollfd fds;
        fds.fd = context->rcontext->fd;
        fds.events = POLLOUT;
        if ((poll(&fds, 1, 0) != 1) || (fds.revents & POLLHUP)) {
            AZ(pthread_mutex_lock(&context->server->db->mutex));
            context->server->db->stats.connections.dropped.hung_up++;
            AZ(pthread_mutex_unlock(&context->server->db->mutex));
            return 0;
        }
    }

    // Valid!
    return 1;
}

static redis_server_t *
unsafe_pick_redis_server(struct vmod_redis_db *db)
{
    // Assertions.
    //   - db->mutex locked.

    // Initializations.
    redis_server_t *result = NULL;

    // Look for a server. Caller should own db->mutex!
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &db->servers, list) {
        // Found!
        CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
        result = iserver;

        // Move the server to the end of the list (this ensures a nice
        // distribution of load between all available servers).
        VTAILQ_REMOVE(&db->servers, result, list);
        VTAILQ_INSERT_TAIL(&db->servers, result, list);

        // Done!
        break;
    }

    // Done!
    return result;
}

static redisContext *
new_rcontext(VRT_CTX, redis_server_t * server, time_t now)
{
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
        // Send command.
        redisReply *reply = redisCommand(result, "AUTH %s", server->db->password);

        // Check reply.
        if ((result->err) ||
            (reply == NULL) ||
            (reply->type != REDIS_REPLY_STATUS) ||
            (strcmp(reply->str, "OK") != 0)) {
            if (result->err) {
                REDIS_LOG_ERROR(ctx,
                    "Failed to authenticate connection (error=%d, db=%s, server=%s): %s",
                    result->err, server->db->name, server->location.raw, result->errstr);
            } else if ((reply != NULL) &&
                       ((reply->type == REDIS_REPLY_ERROR) ||
                        (reply->type == REDIS_REPLY_STATUS) ||
                        (reply->type == REDIS_REPLY_STRING))) {
                REDIS_LOG_ERROR(ctx,
                    "Failed to authenticate connection (error=%d, db=%s, server=%s): %s",
                    reply->type, server->db->name, server->location.raw, reply->str);
            } else {
                REDIS_LOG_ERROR(ctx,
                    "Failed to authenticate connection (db=%s, server=%s)",
                    server->db->name, server->location.raw);
            }
            redisFree(result);
            result = NULL;
        }

        // Release reply.
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    }

    // Update stats.
    AZ(pthread_mutex_lock(&server->db->mutex));
    if (result != NULL) {
        server->db->stats.connections.total++;
    } else {
        server->db->stats.connections.failed++;
    }
    AZ(pthread_mutex_unlock(&server->db->mutex));

#if HIREDIS_MAJOR >= 0 && HIREDIS_MINOR >= 12
    // Enable TCP keep-alive.
    if ((result != NULL) && (server->location.type == REDIS_SERVER_LOCATION_HOST_TYPE)) {
        redisEnableKeepAlive(result);
    }
#endif

    // Done!
    return result;
}

static redis_context_t *
lock_private_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, redis_server_t *server)
{
    redis_context_t *icontext;

    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Select an existing context matching the requested database **and** server (it
    // may exist or not, but no more that one instance is possible).
    VTAILQ_FOREACH(icontext, &state->contexts, list) {
        if ((icontext->server->db == db) &&
            ((server == NULL) || (server == icontext->server))) {
            // Found!
            CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
            result = icontext;

            // Move the context to the end of the list (this ensures a
            // nice distribution of load between all available contexts).
            VTAILQ_REMOVE(&state->contexts, result, list);
            VTAILQ_INSERT_TAIL(&state->contexts, result, list);

            // Done!
            break;
        }
    }

    // Is the previously selected context valid?
    if ((result != NULL) && (!is_valid_redis_context(result, now))) {
        // Release context.
        VTAILQ_REMOVE(&state->contexts, result, list);
        state->ncontexts--;
        free_redis_context(result);

        // A new context needs to be created.
        result = NULL;
    }

    // If required, create new context using the requested server or a randomly
    // selected server if none was specified. If any error arises discard the
    // context and continue.
    if (result == NULL) {
        // Select server.
        if (server == NULL) {
            AZ(pthread_mutex_lock(&db->mutex));
            server = unsafe_pick_redis_server(db);
            AZ(pthread_mutex_unlock(&db->mutex));
        }

        // Do not continue if a server was not found.
        if (server != NULL) {
            // If an empty slot is not available, release an existing context.
            if (state->ncontexts >= db->max_connections) {
                icontext = VTAILQ_FIRST(&state->contexts);
                CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                VTAILQ_REMOVE(&state->contexts, icontext, list);
                state->ncontexts--;
                free_redis_context(icontext);
                AZ(pthread_mutex_lock(&db->mutex));
                db->stats.connections.dropped.overflow++;
                AZ(pthread_mutex_unlock(&db->mutex));
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            redisContext *rcontext = new_rcontext(ctx, server, now);
            if (rcontext != NULL) {
                result = new_redis_context(server, rcontext, now);
                VTAILQ_INSERT_TAIL(&state->contexts, result, list);
                state->ncontexts++;
            }
        } else {
            REDIS_LOG_ERROR(ctx, "Failed to pick server (db=%s)", db->name);
        }
    }

    // Done!
    return result;
}

static redis_context_t *
lock_shared_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, redis_server_t *server)
{
    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Select server.
    if (server == NULL) {
        AZ(pthread_mutex_lock(&db->mutex));
        server = unsafe_pick_redis_server(db);
        AZ(pthread_mutex_unlock(&db->mutex));
    }

    // Do not continue if a server was not found.
    if (server != NULL) {
        // Get pool lock.
        AZ(pthread_mutex_lock(&server->pool.mutex));

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
            if (!is_valid_redis_context(result, now)) {
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
            // If an empty slot is not available, wait for another thread.
            if (server->pool.ncontexts >= db->max_connections) {
                AZ(pthread_cond_wait(&server->pool.cond, &server->pool.mutex));
                AZ(pthread_mutex_lock(&db->mutex));
                db->stats.workers.blocked++;
                AZ(pthread_mutex_unlock(&db->mutex));
                goto retry;
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            redisContext *rcontext = new_rcontext(ctx, server, now);
            if (rcontext != NULL) {
                result = new_redis_context(server, rcontext, now);
                VTAILQ_INSERT_TAIL(&server->pool.busy_contexts, result, list);
                server->pool.ncontexts++;
            }
        }

        // Release pool lock.
        AZ(pthread_mutex_unlock(&server->pool.mutex));

    // The server was not found.
    } else {
        REDIS_LOG_ERROR(ctx, "Failed to pick server (db=%s)", db->name);
    }

    // Done!
    return result;
}

static redis_context_t *
lock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, redis_server_t *server)
{
    if (db->shared_connections) {
        return lock_shared_redis_context(ctx, db, state, server);
    } else {
        return lock_private_redis_context(ctx, db, state, server);
    }
}

static void
unlock_shared_redis_context(
    VRT_CTX, struct vmod_redis_db *db, redis_context_t *context)
{
    // Check input.
    CHECK_OBJ_NOTNULL(context, REDIS_CONTEXT_MAGIC);
    CHECK_OBJ_NOTNULL(context->server, REDIS_SERVER_MAGIC);

    // Return context to the pool's free list.
    AZ(pthread_mutex_lock(&context->server->pool.mutex));
    VTAILQ_REMOVE(&context->server->pool.busy_contexts, context, list);
    VTAILQ_INSERT_TAIL(&context->server->pool.free_contexts, context, list);
    AZ(pthread_cond_signal(&context->server->pool.cond));
    AZ(pthread_mutex_unlock(&context->server->pool.mutex));
}

static void
unlock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state, redis_context_t *context)
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
    redisReply *reply;

    // Set command execution timeout.
    int tr = redisSetTimeout(context->rcontext, timeout);
    if (tr != REDIS_OK) {
        REDIS_LOG_ERROR(ctx,
            "Failed to set command execution timeout (error=%d, db=%s, server=%s)",
            tr, context->server->db->name, context->server->location.raw);
    }

    // Prepare pipeline.
    if (asking) {
        redisAppendCommand(context->rcontext, "ASKING");
    }
    redisAppendCommandArgv(context->rcontext, argc, argv, NULL);

    // Fetch ASKING command reply.
    if (asking) {
        reply = NULL;
        redisGetReply(context->rcontext, (void **)&reply);
        if (reply != NULL) {
            freeReplyObject(reply);
        }
    }

    // Fetch command reply.
    reply = NULL;
    redisGetReply(context->rcontext, (void **)&reply);
    return reply;
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
    AN(result);
    char *ptr = result;
    for (int i = 0; i < 20; i++) {
        sprintf(ptr, "%02x", buffer[i]);
        ptr += 2;
    }

    // Done!
    return result;
}
