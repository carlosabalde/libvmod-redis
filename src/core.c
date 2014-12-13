#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"
#include "vcc_if.h"

#include "core.h"

static redis_context_t *
get_private_context(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned int version);

static redis_context_t *
get_shared_context(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned int version);

static void
free_shared_context(
    struct sess *sp, vcl_priv_t *config, redis_context_t *context);

redis_server_t *
new_redis_server(
    const char *tag, const char *location, int timeout, int ttl)
{
    redis_server_t *result;
    ALLOC_OBJ(result, REDIS_SERVER_MAGIC);
    AN(result);

    char *ptr = strrchr(location, ':');
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

    result->tag = strdup(tag);
    AN(result->tag);
    result->timeout.tv_sec = timeout / 1000;
    result->timeout.tv_usec = (timeout % 1000) * 1000;
    result->ttl = ttl;

    return result;
}

void
free_redis_server(redis_server_t *server)
{
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

    server->timeout.tv_sec = 0;
    server->timeout.tv_usec = 0;
    server->ttl = 0;

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

vcl_priv_t *
new_vcl_priv(
    const char *tag, const char *location, unsigned timeout, unsigned ttl,
    unsigned shared_pool, unsigned max_pool_size)
{
    vcl_priv_t *result;
    ALLOC_OBJ(result, VCL_PRIV_MAGIC);
    AN(result);

    AZ(pthread_mutex_init(&result->mutex, NULL));
    AZ(pthread_cond_init(&result->cond, NULL));

    VTAILQ_INIT(&result->servers);
    redis_server_t *server = new_redis_server(tag, location, timeout, ttl);
    VTAILQ_INSERT_HEAD(&result->servers, server, list);

    result->shared_pool = shared_pool;
    result->max_pool_size = max_pool_size;

    result->ncontexts = 0;
    VTAILQ_INIT(&result->free_contexts);
    VTAILQ_INIT(&result->busy_contexts);

    return result;
}

void
free_vcl_priv(vcl_priv_t *priv)
{
    AZ(pthread_mutex_destroy(&priv->mutex));
    AZ(pthread_cond_destroy(&priv->cond));

    redis_server_t *iserver;
    while (!VTAILQ_EMPTY(&priv->servers)) {
        iserver = VTAILQ_FIRST(&priv->servers);
        VTAILQ_REMOVE(&priv->servers, iserver, list);
        free_redis_server(iserver);
    }

    priv->shared_pool = 0;
    priv->max_pool_size = 0;

    priv->ncontexts = 0;
    redis_context_t *icontext;
    while (!VTAILQ_EMPTY(&priv->free_contexts)) {
        icontext = VTAILQ_FIRST(&priv->free_contexts);
        VTAILQ_REMOVE(&priv->free_contexts, icontext, list);
        free_redis_context(icontext);
    }
    while (!VTAILQ_EMPTY(&priv->busy_contexts)) {
        icontext = VTAILQ_FIRST(&priv->busy_contexts);
        VTAILQ_REMOVE(&priv->busy_contexts, icontext, list);
        free_redis_context(icontext);
    }

    FREE_OBJ(priv);
}

thread_state_t *
new_thread_state()
{
    thread_state_t *result;
    ALLOC_OBJ(result, THREAD_STATE_MAGIC);
    AN(result);

    result->ncontexts = 0;
    VTAILQ_INIT(&result->contexts);
    result->tag = NULL;
    result->argc = 0;
    result->reply = NULL;

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

    state->tag = NULL;
    state->argc = 0;
    if (state->reply != NULL) {
        freeReplyObject(state->reply);
    }

    FREE_OBJ(state);
}

redis_context_t *
get_context(
    struct sess *sp, struct vmod_priv *vcl_priv, thread_state_t *state,
    unsigned int version)
{
    vcl_priv_t *config = vcl_priv->priv;
    if (config->shared_pool) {
        return get_shared_context(sp, config, state, version);
    } else {
        return get_private_context(sp, config, state, version);
    }
}

void
free_context(
    struct sess *sp, struct vmod_priv *vcl_priv, thread_state_t *state,
    redis_context_t *context)
{
    vcl_priv_t *config = vcl_priv->priv;
    if (config->shared_pool) {
        return free_shared_context(sp, config, context);
    }
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static unsigned
is_valid_context(redis_context_t *context, unsigned int version, time_t now)
{
    // Check if context is in an error state or if it's too old.
    if ((context->rcontext->err) ||
        (context->version != version) ||
        ((context->server->ttl > 0) &&
         (now - context->tst > context->server->ttl))) {
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
get_server(vcl_priv_t *config, const char *tag)
{
    // Initializations.
    redis_server_t *result = NULL;

    // Look for a server matching the tag.
    // Caller should own the corresponding lock!
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &config->servers, list) {
        if ((tag == NULL) || (strcmp(tag, iserver->tag) == 0)) {
            // Found!
            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
            result = iserver;

            // Move the server to the end of the list (this ensures a nice
            // distribution of load between all available servers).
            VTAILQ_REMOVE(&config->servers, result, list);
            VTAILQ_INSERT_TAIL(&config->servers, result, list);

            // Done!
            break;
        }
    }

    // Done!
    return result;
}

static redisContext *
new_rcontext(
    struct sess *sp, redis_server_t * server,
    unsigned int version, time_t now)
{
    redisContext *result;

    // Create context.
    switch (server->type) {
        case REDIS_SERVER_HOST_TYPE:
            result = redisConnectWithTimeout(
                server->location.address.host,
                server->location.address.port,
                server->timeout);
            break;

        case REDIS_SERVER_SOCKET_TYPE:
            result = redisConnectUnixWithTimeout(
                server->location.path,
                server->timeout);
            break;

        default:
            result = NULL;
    }
    AN(result);

    // Check created context.
    if (result->err) {
        REDIS_LOG(sp,
            "Failed to establish Redis connection (%d): %s",
            result->err,
            result->errstr);
        redisFree(result);
        result = NULL;
    }

    // Done!
    return result;
}

static redis_context_t *
get_private_context(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned int version)
{
    redis_context_t *icontext;
    redis_server_t *iserver;

    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Select an existing context matching the requested tag.
    VTAILQ_FOREACH(icontext, &state->contexts, list) {
        if ((state->tag == NULL) ||
            (strcmp(state->tag, icontext->server->tag) == 0)) {
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
    if ((result != NULL) && (!is_valid_context(result, version, now))) {
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
        AZ(pthread_mutex_lock(&config->mutex));
        redis_server_t *server = get_server(config, state->tag);
        AZ(pthread_mutex_unlock(&config->mutex));

        // Do not continue if a server was not found.
        if (server != NULL) {
            // If an empty slot is not available, release an existing context.
            if (state->ncontexts >= config->max_pool_size) {
                icontext = VTAILQ_FIRST(&state->contexts);
                CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                VTAILQ_REMOVE(&state->contexts, icontext, list);
                state->ncontexts--;
                free_redis_context(icontext);
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            redisContext *rcontext = new_rcontext(sp, server, version, now);
            if (rcontext != NULL) {
                result = new_redis_context(server, rcontext, version, now);
                VTAILQ_INSERT_TAIL(&state->contexts, result, list);
                state->ncontexts++;
            }
        } else {
            REDIS_LOG(sp, "The requested server does not exist: %s", state->tag);
        }
    }

    // Done!
    return result;
}

static redis_context_t *
get_shared_context(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    unsigned int version)
{
    redis_context_t *icontext;
    redis_server_t *iserver;

    // Initializations.
    redis_context_t *result = NULL;
    time_t now = time(NULL);

    // Get shared config lock.
    AZ(pthread_mutex_lock(&config->mutex));

retry:
    // Look for an existing free context matching the requested tag.
    while (1) {
        // Select an existing free context matching the requested tag.
        VTAILQ_FOREACH(icontext, &config->free_contexts, list) {
            if ((state->tag == NULL) ||
                (strcmp(state->tag, icontext->server->tag) == 0)) {
                // Found!
                CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                result = icontext;

                // Mark the context as busy.
                VTAILQ_REMOVE(&config->free_contexts, result, list);
                VTAILQ_INSERT_HEAD(&config->busy_contexts, result, list);

                // Done!
                break;
            }
        }

        // Is the previously selected context valid?
        if ((result != NULL) && (!is_valid_context(result, version, now))) {
            // Release context.
            VTAILQ_REMOVE(&config->busy_contexts, result, list);
            config->ncontexts--;
            free_redis_context(result);

            // A new context needs to be selected.
            result = NULL;

        // No free context found, or found & valid.
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
        redis_server_t *server = get_server(config, state->tag);

        // Do not continue if a server was not found.
        if (server != NULL) {
            // If an empty slot is not available, release an existing free context
            // or wait for another thread.
            if (config->ncontexts >= config->max_pool_size) {
                if (!VTAILQ_EMPTY(&config->free_contexts)) {
                    icontext = VTAILQ_FIRST(&config->free_contexts);
                    CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                    VTAILQ_REMOVE(&config->free_contexts, icontext, list);
                    config->ncontexts--;
                    free_redis_context(icontext);
                } else {
                    AZ(pthread_cond_wait(&config->cond, &config->mutex));
                    goto retry;
                }
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            redisContext *rcontext = new_rcontext(sp, server, version, now);
            if (rcontext != NULL) {
                result = new_redis_context(server, rcontext, version, now);
                VTAILQ_INSERT_TAIL(&config->busy_contexts, result, list);
                config->ncontexts++;
            }
        } else {
            REDIS_LOG(sp, "The requested server does not exist: %s", state->tag);
        }
    }

    // Release shared config lock.
    AZ(pthread_mutex_unlock(&config->mutex));

    // Done!
    return result;
}

static void
free_shared_context(
    struct sess *sp, vcl_priv_t *config, redis_context_t *context)
{
    CHECK_OBJ_NOTNULL(context, REDIS_CONTEXT_MAGIC);
    AZ(pthread_mutex_lock(&config->mutex));
    VTAILQ_REMOVE(&config->busy_contexts, context, list);
    VTAILQ_INSERT_TAIL(&config->free_contexts, context, list);
    AZ(pthread_cond_signal(&config->cond));
    AZ(pthread_mutex_unlock(&config->mutex));
}
