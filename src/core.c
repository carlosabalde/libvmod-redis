#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "core.h"

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
            break;

        case REDIS_SERVER_SOCKET_TYPE:
            free((void *) server->location.path);
            server->location.path = NULL;
            break;
    }

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

    redis_server_t *iserver;
    while (!VTAILQ_EMPTY(&priv->servers)) {
        iserver = VTAILQ_FIRST(&priv->servers);
        VTAILQ_REMOVE(&priv->servers, iserver, list);
        free_redis_server(iserver);
    }

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
    redis_context_t *icontext;
    while (!VTAILQ_EMPTY(&state->contexts)) {
        icontext = VTAILQ_FIRST(&state->contexts);
        VTAILQ_REMOVE(&state->contexts, icontext, list);
        free_redis_context(icontext);
    }

    if (state->reply != NULL) {
        freeReplyObject(state->reply);
    }

    FREE_OBJ(state);
}

redis_context_t *
get_context(
    const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv, thread_state_t *state,
    unsigned int version)
{
    redis_context_t *icontext;
    redis_server_t *iserver;

    // Initializations.
    redis_context_t *result = NULL;
    vcl_priv_t *config = vcl_priv->priv;
    time_t now = time(NULL);

    // Select an existing context matching the requested tag.
    VTAILQ_FOREACH(icontext, &state->contexts, list) {
        if ((state->tag == NULL) ||
            (strcmp(state->tag, icontext->server->tag) == 0)) {
            CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
            result = icontext;
            break;
        }
    }

    // If a context was found, move it to the end of the list (this ensures a
    // nice distribution of load between all available contexts).
    if (result != NULL) {
        VTAILQ_REMOVE(&state->contexts, result, list);
        VTAILQ_INSERT_TAIL(&state->contexts, result, list);
    }

    // Is the previously selected context valid?
    if (result != NULL) {
        // Discard context if it's in an error state or if it's too old.
        if ((result->rcontext->err) ||
            (result->version != version) ||
            ((result->server->ttl > 0) &&
             (now - result->tst > result->server->ttl))) {
            // Release context.
            VTAILQ_REMOVE(&state->contexts, result, list);
            state->ncontexts--;
            free_redis_context(result);

            // A new context needs to be created.
            result = NULL;

        // Also discard context if connection has been hung up by the server.
        } else {
            struct pollfd fds;

            fds.fd = result->rcontext->fd;
            fds.events = POLLOUT;

            if ((poll(&fds, 1, 0) != 1) || (fds.revents & POLLHUP)) {
                // Release context.
                VTAILQ_REMOVE(&state->contexts, result, list);
                state->ncontexts--;
                free_redis_context(result);

                // A new context needs to be created.
                result = NULL;
            }
        }
    }


    // If required, create new context using a randomly selected server matching
    // the requested tag. If any error arises discard the context and continue.
    if (result == NULL) {
        // Select server matching the requested tag.
        redis_server_t *server = NULL;
        AZ(pthread_mutex_lock(&config->mutex));
        VTAILQ_FOREACH(iserver, &config->servers, list) {
            if ((state->tag == NULL) ||
                (strcmp(state->tag, iserver->tag) == 0)) {
                CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                server = iserver;
                break;
            }
        }

        // If an server was found, move it to the end of the list (this
        // ensures a nice distribution of load between all available servers).
        if (server != NULL) {
            VTAILQ_REMOVE(&config->servers, server, list);
            VTAILQ_INSERT_TAIL(&config->servers, server, list);
        }
        AZ(pthread_mutex_unlock(&config->mutex));

        // Do not continue if a server was not found.
        if (server != NULL) {
            // If an empty slot is not available, release an existing context.
            if (state->ncontexts >= config->max_pool_size) {
                icontext = VTAILQ_FIRST(&state->contexts);
                VTAILQ_REMOVE(&state->contexts, icontext, list);
                state->ncontexts--;
                free_redis_context(result);
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            redisContext *rcontext;
            switch (server->type) {
                case REDIS_SERVER_HOST_TYPE:
                    rcontext = redisConnectWithTimeout(
                        server->location.address.host,
                        server->location.address.port,
                        server->timeout);
                    break;

                case REDIS_SERVER_SOCKET_TYPE:
                    rcontext = redisConnectUnixWithTimeout(
                        server->location.path,
                        server->timeout);
                    break;

                default:
                    rcontext = NULL;
            }
            AN(rcontext);
            if (rcontext->err) {
                REDIS_LOG(ctx,
                    "Failed to establish Redis connection (%d): %s",
                    rcontext->err,
                    rcontext->errstr);
                redisFree(rcontext);
            } else {
                result = new_redis_context(server, rcontext, version, now);
                VTAILQ_INSERT_TAIL(&state->contexts, result, list);
                state->ncontexts++;
            }
        } else {
            REDIS_LOG(ctx, "The requested server does not exist: %s", state->tag);
        }
    }

    // Done!
    return result;
}

void
free_context(redis_context_t * context)
{
    // Nothing to do. At the moment only private pools are supported.
}
