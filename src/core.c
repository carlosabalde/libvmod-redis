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

static redis_context_t *lock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version);

static void unlock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    redis_context_t *context);

static redisReply *get_redis_repy(
    VRT_CTX, redis_context_t *context,
    struct timeval timeout, unsigned argc, const char *argv[], unsigned asking);

static const char *sha1(VRT_CTX, const char *script);

const char *
new_clustered_redis_server_tag(const char *location)
{
    // Check location (only host + port format is allowed).
    AN(strchr(location, ':'));

    // Build tag.
    char *result = malloc(
        strlen(CLUSTERED_REDIS_SERVER_TAG_PREFIX) +
        strlen(location) +
        1);
    AN(result);
    strcpy(result, CLUSTERED_REDIS_SERVER_TAG_PREFIX);
    strcat(result, location);

    // Done!
    return result;
}

redis_server_t *
new_redis_server(
    struct vmod_redis_db *db, const char *tag, const char *location,
    unsigned clustered, struct timeval connection_timeout, unsigned context_ttl)
{
    // Initializations.
    redis_server_t *result = NULL;
    char *ptr = strrchr(location, ':');

    // Do not continue if this is a clustered server but its location is not
    // provided using the host + port format.
    if (!clustered || (ptr != NULL)) {
        ALLOC_OBJ(result, REDIS_SERVER_MAGIC);
        AN(result);

        result->db = db;

        if (ptr != NULL) {
            result->type = REDIS_SERVER_HOST_TYPE;
            result->location.address.host = strndup(location, ptr - location);
            AN(result->location.address.host);
            result->location.address.port = atoi(ptr + 1);
        } else {
            result->type = REDIS_SERVER_SOCKET_TYPE;
            result->location.path = strdup(location);
            AN(result->location.path);
        }

        if (clustered) {
            result->tag = new_clustered_redis_server_tag(location);
        } else {
            result->tag = strdup(tag);
        }
        AN(result->tag);
        result->connection_timeout = connection_timeout;
        result->context_ttl = context_ttl;
    }

    // Done!
    return result;
}

void
free_redis_server(redis_server_t *server)
{
    server->db = NULL;

    free((void *) server->tag);
    server->tag = NULL;

    switch (server->type) {
        case REDIS_SERVER_HOST_TYPE:
            free((void *) server->location.address.host);
            server->location.address.host = NULL;
            server->location.address.port = 0;
            break;

        case REDIS_SERVER_SOCKET_TYPE:
            free((void *) server->location.path);
            server->location.path = NULL;
            break;
    }

    server->connection_timeout = (struct timeval){ 0 };
    server->context_ttl = 0;

    FREE_OBJ(server);
}

redis_context_t *
new_redis_context(
    redis_server_t *server, redisContext *rcontext, unsigned version, time_t tst)
{
    redis_context_t *result;
    ALLOC_OBJ(result, REDIS_CONTEXT_MAGIC);
    AN(result);

    result->server = server;
    result->rcontext = rcontext;
    result->version = version;
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

redis_context_pool_t *
new_redis_context_pool(const char *tag)
{
    redis_context_pool_t *result;
    ALLOC_OBJ(result, REDIS_CONTEXT_POOL_MAGIC);
    AN(result);

    result->tag = strdup(tag);
    AN(result->tag);

    AZ(pthread_mutex_init(&result->mutex, NULL));
    AZ(pthread_cond_init(&result->cond, NULL));

    result->ncontexts = 0;
    VTAILQ_INIT(&result->free_contexts);
    VTAILQ_INIT(&result->busy_contexts);

    return result;
}

void
free_redis_context_pool(redis_context_pool_t *pool)
{
    free((void *) pool->tag);
    pool->tag = NULL;

    AZ(pthread_mutex_destroy(&pool->mutex));
    AZ(pthread_cond_destroy(&pool->cond));

    pool->ncontexts = 0;
    redis_context_t *icontext;
    while (!VTAILQ_EMPTY(&pool->free_contexts)) {
        icontext = VTAILQ_FIRST(&pool->free_contexts);
        VTAILQ_REMOVE(&pool->free_contexts, icontext, list);
        free_redis_context(icontext);
    }
    while (!VTAILQ_EMPTY(&pool->busy_contexts)) {
        icontext = VTAILQ_FIRST(&pool->busy_contexts);
        VTAILQ_REMOVE(&pool->busy_contexts, icontext, list);
        free_redis_context(icontext);
    }

    FREE_OBJ(pool);
}

vcl_priv_t *
new_vcl_priv()
{
    vcl_priv_t *result;
    ALLOC_OBJ(result, VCL_PRIV_MAGIC);
    AN(result);

    return result;
}

void
free_vcl_priv(vcl_priv_t *priv)
{
    FREE_OBJ(priv);
}

struct vmod_redis_db *
new_vmod_redis_db(
    struct timeval command_timeout, unsigned retries,
    unsigned shared_contexts, unsigned max_contexts)
{
    struct vmod_redis_db *result;
    ALLOC_OBJ(result, VMOD_REDIS_DB_MAGIC);
    AN(result);

    AZ(pthread_mutex_init(&result->mutex, NULL));

    VTAILQ_INIT(&result->servers);

    result->command_timeout = command_timeout;
    result->retries = retries;
    result->shared_contexts = shared_contexts;
    result->max_contexts = max_contexts;

    result->cluster.enabled = 0;
    result->cluster.connection_timeout = (struct timeval){ 0 };
    result->cluster.max_hops = 0;
    result->cluster.context_ttl = 0;
    for (int i = 0; i < MAX_REDIS_CLUSTER_SLOTS; i++) {
        result->cluster.slots[i] = NULL;
    }

    VTAILQ_INIT(&result->pools);

    return result;
}

void
free_vmod_redis_db(struct vmod_redis_db *db)
{
    AZ(pthread_mutex_destroy(&db->mutex));

    redis_server_t *iserver;
    while (!VTAILQ_EMPTY(&db->servers)) {
        iserver = VTAILQ_FIRST(&db->servers);
        VTAILQ_REMOVE(&db->servers, iserver, list);
        free_redis_server(iserver);
    }

    db->command_timeout = (struct timeval){ 0 };
    db->retries = 0;
    db->shared_contexts = 0;
    db->max_contexts = 0;

    db->cluster.enabled = 0;
    db->cluster.connection_timeout = (struct timeval){ 0 };
    db->cluster.max_hops = 0;
    db->cluster.context_ttl = 0;
    for (int i = 0; i < MAX_REDIS_CLUSTER_SLOTS; i++) {
        if (db->cluster.slots[i] != NULL) {
            free((void *) (db->cluster.slots[i]));
            db->cluster.slots[i] = NULL;
        }
    }

    redis_context_pool_t *ipool;
    while (!VTAILQ_EMPTY(&db->pools)) {
        ipool = VTAILQ_FIRST(&db->pools);
        VTAILQ_REMOVE(&db->pools, ipool, list);
        free_redis_context_pool(ipool);
    }

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
    result->command.tag = NULL;
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
        VTAILQ_REMOVE(&state->contexts, icontext, list);
        free_redis_context(icontext);
    }

    state->command.db = NULL;
    state->command.timeout = (struct timeval){ 0 };
    state->command.tag = NULL;
    state->command.argc = 0;
    if (state->command.reply != NULL) {
        freeReplyObject(state->command.reply);
    }

    FREE_OBJ(state);
}

redis_server_t *
unsafe_get_redis_server(struct vmod_redis_db *db, const char *tag)
{
    // Initializations.
    redis_server_t *result = NULL;

    // Look for a server matching the tag.
    // Caller should own db->mutex!
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &db->servers, list) {
        if (strcmp(tag, iserver->tag) == 0) {
            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
            result = iserver;
            break;
        }
    }

    // Done!
    return result;
}

redis_context_pool_t *
unsafe_get_context_pool(struct vmod_redis_db *db, const char *tag)
{
    // Initializations.
    redis_context_pool_t *result = NULL;

    // Look for a pool matching the tag.
    // Caller should own db->mutex!
    redis_context_pool_t *ipool;
    VTAILQ_FOREACH(ipool, &db->pools, list) {
        if (strcmp(tag, ipool->tag) == 0) {
            CHECK_OBJ_NOTNULL(ipool, REDIS_CONTEXT_POOL_MAGIC);
            result = ipool;
            break;
        }
      }

    // Done!
    return result;
}

redisReply *
redis_execute(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version, struct timeval timeout, unsigned argc,
    const char *argv[], unsigned asking)
{
    // Initializations.
    redisReply *result = NULL;
    redis_context_t *context = lock_redis_context(ctx, db, state, tag, version);

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
        if (context->rcontext->err) {
            REDIS_LOG(ctx,
                "Failed to execute Redis command (%s): [%d] %s",
                argv[0],
                context->rcontext->err,
                context->rcontext->errstr);
        } else if (result == NULL) {
            REDIS_LOG(ctx,
                "Failed to execute Redis command (%s)",
                argv[0]);
        }

        // Release context.
        unlock_redis_context(ctx, db, state, context);
    }

    // Done!
    return result;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static unsigned
is_valid_redis_context(redis_context_t *context, unsigned version, time_t now)
{
    // Check if context is in an error state or if it's too old.
    if ((context->rcontext->err) ||
        (context->version != version) ||
        ((context->server->context_ttl > 0) &&
         (now - context->tst > context->server->context_ttl))) {
        return 0;

    // Check if context connection has been hung up by the server.
    } else {
        struct pollfd fds;
        fds.fd = context->rcontext->fd;
        fds.events = POLLOUT;
        if ((poll(&fds, 1, 0) != 1) || (fds.revents & POLLHUP)) {
            return 0;
        }
    }

    // Valid!
    return 1;
}

static redis_server_t *
unsafe_pick_redis_server(struct vmod_redis_db *db, const char *tag)
{
    // Initializations.
    redis_server_t *result = NULL;

    // Look for a server matching the tag.
    // Caller should own db->mutex!
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &db->servers, list) {
        if ((tag == NULL) || (strcmp(tag, iserver->tag) == 0)) {
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
    }

    // Done!
    return result;
}

static redis_context_pool_t *
unsafe_pick_context_pool(struct vmod_redis_db *db, const char *tag)
{
    // Initializations.
    redis_context_pool_t *result = NULL;

    // Look for a pool matching the tag.
    // Caller should own db->mutex!
    redis_context_pool_t *ipool;
    VTAILQ_FOREACH(ipool, &db->pools, list) {
        if ((tag == NULL) || (strcmp(tag, ipool->tag) == 0)) {
            // Found!
            CHECK_OBJ_NOTNULL(ipool, REDIS_CONTEXT_POOL_MAGIC);
            result = ipool;

            // Move the pool to the end of the list (this ensures a nice
            // distribution of load between all available pools).
            VTAILQ_REMOVE(&db->pools, result, list);
            VTAILQ_INSERT_TAIL(&db->pools, result, list);

            // Done!
            break;
        }
    }

    // Done!
    return result;
}

static redisContext *
new_rcontext(VRT_CTX, redis_server_t * server, unsigned version, time_t now)
{
    redisContext *result;

    // Create context.
    if ((server->connection_timeout.tv_sec > 0) || (server->connection_timeout.tv_usec > 0)) {
        switch (server->type) {
            case REDIS_SERVER_HOST_TYPE:
                result = redisConnectWithTimeout(
                    server->location.address.host,
                    server->location.address.port,
                    server->connection_timeout);
                break;

            case REDIS_SERVER_SOCKET_TYPE:
                result = redisConnectUnixWithTimeout(
                    server->location.path,
                    server->connection_timeout);
                break;

            default:
                result = NULL;
        }
    } else {
        switch (server->type) {
            case REDIS_SERVER_HOST_TYPE:
                result = redisConnect(
                    server->location.address.host,
                    server->location.address.port);
                break;

            case REDIS_SERVER_SOCKET_TYPE:
                result = redisConnectUnix(
                    server->location.path);
                break;

            default:
                result = NULL;
        }
    }
    AN(result);

    // Check created context.
    if (result->err) {
        REDIS_LOG(ctx,
            "Failed to establish Redis connection (%d): %s",
            result->err,
            result->errstr);
        redisFree(result);
        result = NULL;
    }

#if HIREDIS_MAJOR >= 0 && HIREDIS_MINOR >= 12
    // Enable TCP keep-alive.
    if ((result != NULL) && (server->type == REDIS_SERVER_HOST_TYPE)) {
        redisEnableKeepAlive(result);
    }
#endif

    // Done!
    return result;
}

static redis_context_t *
lock_private_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version)
{
    redis_context_t *icontext;

    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Select an existing context matching the requested database **and** tag (it
    // may exist or not, but no more that one instance is possible).
    VTAILQ_FOREACH(icontext, &state->contexts, list) {
        if ((icontext->server->db == db) &&
            ((tag == NULL) || (strcmp(tag, icontext->server->tag) == 0))) {
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
    if ((result != NULL) && (!is_valid_redis_context(result, version, now))) {
        // Release context.
        VTAILQ_REMOVE(&state->contexts, result, list);
        state->ncontexts--;
        free_redis_context(result);

        // A new context needs to be created.
        result = NULL;
    }

    // If required, create new context using a randomly selected server matching
    // the requested tag. If any error arises discard the context and continue.
    if (result == NULL) {
        // Select server matching the requested tag.
        AZ(pthread_mutex_lock(&db->mutex));
        redis_server_t *server = unsafe_pick_redis_server(db, tag);
        AZ(pthread_mutex_unlock(&db->mutex));

        // Do not continue if a server was not found.
        if (server != NULL) {
            // If an empty slot is not available, release an existing context.
            if (state->ncontexts >= db->max_contexts) {
                icontext = VTAILQ_FIRST(&state->contexts);
                CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                VTAILQ_REMOVE(&state->contexts, icontext, list);
                state->ncontexts--;
                free_redis_context(icontext);
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            redisContext *rcontext = new_rcontext(ctx, server, version, now);
            if (rcontext != NULL) {
                result = new_redis_context(server, rcontext, version, now);
                VTAILQ_INSERT_TAIL(&state->contexts, result, list);
                state->ncontexts++;
            }
        } else {
            REDIS_LOG(ctx, "The requested server does not exist: %s", tag);
        }
    }

    // Done!
    return result;
}

static redis_context_t *
lock_shared_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version)
{
    redis_context_t *icontext;

    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Fetch pool instance.
    AZ(pthread_mutex_lock(&db->mutex));
    redis_context_pool_t *pool = unsafe_pick_context_pool(db, tag);
    AZ(pthread_mutex_unlock(&db->mutex));

    // Do not continue if a pool was not found.
    if (pool != NULL) {
        // Get pool lock.
        AZ(pthread_mutex_lock(&pool->mutex));

retry:
        // Look for an existing free context.
        while (!VTAILQ_EMPTY(&pool->free_contexts)) {
            // Extract context.
            result = VTAILQ_FIRST(&pool->free_contexts);
            CHECK_OBJ_NOTNULL(result, REDIS_CONTEXT_MAGIC);

            // Mark the context as busy.
            VTAILQ_REMOVE(&pool->free_contexts, result, list);
            VTAILQ_INSERT_TAIL(&pool->busy_contexts, result, list);

            // Is the context valid?
            if (!is_valid_redis_context(result, version, now)) {
                // Release context.
                VTAILQ_REMOVE(&pool->busy_contexts, result, list);
                pool->ncontexts--;
                free_redis_context(result);

                // A new context needs to be selected.
                result = NULL;

            // A valid free context was found.
            } else {
                break;
            }
        }

        // If required, create new context using a randomly selected server matching
        // the requested tag. If any error arises discard the context and continue.
        // If maximum number of contexts has been reached, wait for another thread
        // releasing some context or, if possible, discard some existing free context.
        if (result == NULL) {
            // Select server matching the requested tag.
            AZ(pthread_mutex_lock(&db->mutex));
            redis_server_t *server = unsafe_pick_redis_server(db, tag);
            AZ(pthread_mutex_unlock(&db->mutex));

            // Do not continue if a server was not found.
            if (server != NULL) {
                // If an empty slot is not available, release an existing free context
                // or wait for another thread.
                if (pool->ncontexts >= db->max_contexts) {
                    if (!VTAILQ_EMPTY(&pool->free_contexts)) {
                        icontext = VTAILQ_FIRST(&pool->free_contexts);
                        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                        VTAILQ_REMOVE(&pool->free_contexts, icontext, list);
                        pool->ncontexts--;
                        free_redis_context(icontext);
                    } else {
                        AZ(pthread_cond_wait(&pool->cond, &pool->mutex));
                        goto retry;
                    }
                }

                // Create new context using the previously selected server. If any
                // error arises discard the context and continue.
                redisContext *rcontext = new_rcontext(ctx, server, version, now);
                if (rcontext != NULL) {
                    result = new_redis_context(server, rcontext, version, now);
                    VTAILQ_INSERT_TAIL(&pool->busy_contexts, result, list);
                    pool->ncontexts++;
                }
            } else {
                REDIS_LOG(ctx, "The requested server does not exist: %s", tag);
            }
        }

        // Release pool lock.
        AZ(pthread_mutex_unlock(&pool->mutex));

    // The poll was not found.
    } else {
        REDIS_LOG(ctx, "The requested server does not exist: %s", tag);
    }

    // Done!
    return result;
}

static redis_context_t *
lock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version)
{
    if (db->shared_contexts) {
        return lock_shared_redis_context(ctx, db, state, tag, version);
    } else {
        return lock_private_redis_context(ctx, db, state, tag, version);
    }
}

static void
unlock_shared_redis_context(
    VRT_CTX, struct vmod_redis_db *db, redis_context_t *context)
{
    // Check input.
    CHECK_OBJ_NOTNULL(context, REDIS_CONTEXT_MAGIC);
    CHECK_OBJ_NOTNULL(context->server, REDIS_SERVER_MAGIC);

    // Fetch pool instance.
    AZ(pthread_mutex_lock(&db->mutex));
    redis_context_pool_t *pool = unsafe_get_context_pool(db, context->server->tag);
    AN(pool);
    AZ(pthread_mutex_unlock(&db->mutex));

    // Return context to the pool's free list.
    AZ(pthread_mutex_lock(&pool->mutex));
    VTAILQ_REMOVE(&pool->busy_contexts, context, list);
    VTAILQ_INSERT_TAIL(&pool->free_contexts, context, list);
    AZ(pthread_cond_signal(&pool->cond));
    AZ(pthread_mutex_unlock(&pool->mutex));
}

static void
unlock_redis_context(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    redis_context_t *context)
{
    if (db->shared_contexts) {
        return unlock_shared_redis_context(ctx, db, context);
    }
}

static redisReply *
get_redis_repy(
    VRT_CTX, redis_context_t *context,
    struct timeval timeout, unsigned argc, const char *argv[], unsigned asking)
{
    redisReply *reply;

    // Set command execution timeout.
    int tr = redisSetTimeout(context->rcontext, timeout);
    if (tr != REDIS_OK) {
        REDIS_LOG(ctx, "Failed to set command execution timeout (%d)", tr);
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
