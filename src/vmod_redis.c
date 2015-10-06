#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "cluster.h"
#include "core.h"

#define DEFAULT_RETRIES 0
#define DEFAULT_SHARED_CONTEXTS 0
#define DEFAULT_MAX_CONTEXTS 1

static unsigned version = 0;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static task_priv_t *get_task_priv(
    VRT_CTX, struct vmod_priv *task_priv, unsigned drop_reply);

static redis_server_t *unsafe_add_redis_server(
    VRT_CTX, vcl_priv_t *config,
    const char *tag, const char *location, struct timeval connection_timeout, unsigned context_ttl);

static const char *get_reply(VRT_CTX, redisReply *reply);

static void fini(vcl_priv_t *config);

/******************************************************************************
 * VMOD INITIALIZATION.
 *****************************************************************************/

int
init_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    // Check event.
    switch (e) {
        case VCL_EVENT_LOAD:
            // Initialize the local VCL data structure and set its free function.
            // Code initializing / freeing the VCL private data structure *is
            // not required* to be thread safe.
            vcl_priv->priv = new_vcl_priv(
                (struct timeval){ 0 },
                DEFAULT_RETRIES,
                DEFAULT_SHARED_CONTEXTS,
                DEFAULT_MAX_CONTEXTS);
            vcl_priv->free = (vmod_priv_free_f *)free_vcl_priv;
            break;

        case VCL_EVENT_USE:
            // Every time the VMOD is used by some VCL increase the global
            // version. This will be used to (1) reestablish Redis connections
            // binded to worker threads; and (2) regenerate pooled connections
            // shared between threads (and stored in the local VCL data
            // structure) every time the VCL is reloaded.
            AZ(pthread_mutex_lock(&mutex));
            version++;
            AZ(pthread_mutex_unlock(&mutex));
            break;

        case VCL_EVENT_COLD:
            fini(vcl_priv->priv);
            break;

        default:
            break;
    }

    // Done!
    return 0;
}

/******************************************************************************
 * redis.init();
 *****************************************************************************/

VCL_VOID
vmod_init(
    VRT_CTX, struct vmod_priv *vcl_priv,
    VCL_STRING tag, VCL_STRING location, VCL_INT connection_timeout, VCL_INT context_ttl,
    VCL_INT command_timeout, VCL_INT max_cluster_hops,
    VCL_INT retries, VCL_BOOL shared_contexts, VCL_INT max_contexts)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (location != NULL) && (strlen(location) > 0) &&
        (max_contexts > 0)) {
        // Initializations.
        struct timeval connection_timeout_tv;
        connection_timeout_tv.tv_sec = connection_timeout / 1000;
        connection_timeout_tv.tv_usec = (connection_timeout % 1000) * 1000;
        struct timeval command_timeout_tv;
        command_timeout_tv.tv_sec = command_timeout / 1000;
        command_timeout_tv.tv_usec = (command_timeout % 1000) * 1000;

        // Create new configuration.
        vcl_priv_t *config = new_vcl_priv(command_timeout_tv, retries, shared_contexts, max_contexts);

        // Add initial server.
        redis_server_t *server = unsafe_add_redis_server(
            ctx, config, tag, location, connection_timeout_tv, context_ttl);

        // Do not continue if we failed to create the server instance.
        if (server != NULL) {
            // If a clustered server was added, enable clustering, set clustering
            // options, and populate the slots-tags mapping.
            if (server->clustered) {
                config->clustered = 1;
                config->connection_timeout = connection_timeout_tv;
                config->max_cluster_hops = max_cluster_hops;
                config->context_ttl = context_ttl;
                discover_cluster_slots(ctx, config);
            }

            // Set the new configuration & release the previous one.
            vcl_priv_t *old_config = vcl_priv->priv;
            vcl_priv->priv = config;
            free_vcl_priv(old_config);
        } else {
            free_vcl_priv(config);
        }
    }
}

/******************************************************************************
 * redis.add_server();
 *****************************************************************************/

VCL_VOID
vmod_add_server(
    VRT_CTX, struct vmod_priv *vcl_priv,
    VCL_STRING tag, VCL_STRING location, VCL_INT connection_timeout, VCL_INT context_ttl)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (location != NULL) && (strlen(location) > 0)) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;

        // Do not allow usage of tags internally reserved for clustering.
        if ((strcmp(tag, CLUSTERED_REDIS_SERVER_TAG) != 0) &&
            (strncmp(tag, CLUSTERED_REDIS_SERVER_TAG_PREFIX,
                     strlen(CLUSTERED_REDIS_SERVER_TAG_PREFIX)) != 0)) {
            // Initializations.
            struct timeval connection_timeout_tv;
            connection_timeout_tv.tv_sec = connection_timeout / 1000;
            connection_timeout_tv.tv_usec = (connection_timeout % 1000) * 1000;

            // Get config lock.
            AZ(pthread_mutex_lock(&config->mutex));

            // Add new server.
            unsafe_add_redis_server(ctx, config, tag, location, connection_timeout_tv, context_ttl);

            // Release config lock.
            AZ(pthread_mutex_unlock(&config->mutex));
        } else {
            REDIS_LOG(ctx,
                "Failed to add server '%s' using reserved tag '%s'",
                location, tag);
        }
    }
}

/******************************************************************************
 * redis.add_cserver();
 *****************************************************************************/

VCL_VOID
vmod_add_cserver(VRT_CTX, struct vmod_priv *vcl_priv, VCL_STRING location)
{
    // Check input.
    if ((location != NULL) && (strlen(location) > 0)) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;

        // Do not continue if clustering has not been enabled.
        if (config->clustered) {
            // Get config lock.
            AZ(pthread_mutex_lock(&config->mutex));

            // Add new server.
            unsafe_add_redis_server(
                ctx, config,
                CLUSTERED_REDIS_SERVER_TAG,
                location, config->connection_timeout, config->context_ttl);

            // Release config lock.
            AZ(pthread_mutex_unlock(&config->mutex));
        } else {
            REDIS_LOG(ctx,
                "Failed to add cluster server '%s' while clustering is disabled",
                location);
        }
    }
}

/******************************************************************************
 * redis.command();
 *****************************************************************************/

VCL_VOID
vmod_command(
    VRT_CTX, struct vmod_priv *vcl_priv, struct vmod_priv *task_priv,
    VCL_STRING name)
{
    // Check input.
    if ((name != NULL) && (strlen(name) > 0)) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;

        // Fetch local thread state & flush previous command.
        task_priv_t *state = get_task_priv(ctx, task_priv, 1);

        // Initialize.
        state->timeout = config->command_timeout;
        state->argc = 1;
        state->argv[0] = WS_Copy(ctx->ws, name, -1);
        AN(state->argv[0]);
    }
}

/******************************************************************************
 * redis.server();
 *****************************************************************************/

VCL_VOID
vmod_server(VRT_CTX, struct vmod_priv *task_priv, VCL_STRING tag)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0)) {
        // Fetch local thread state.
        task_priv_t *state = get_task_priv(ctx, task_priv, 0);

        // Do not continue if the initial call to redis.command() was not
        // executed.
        if (state->argc >= 1) {
            state->tag = WS_Copy(ctx->ws, tag, -1);
            AN(state->tag);
        }
    }
}

/******************************************************************************
 * redis.timeout();
 *****************************************************************************/

VCL_VOID
vmod_timeout(VRT_CTX, struct vmod_priv *task_priv, VCL_INT command_timeout)
{
    // Fetch local thread state.
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);

    // Do not continue if the initial call to redis.command() was not
    // executed.
    if (state->argc >= 1) {
        state->timeout.tv_sec = command_timeout / 1000;
        state->timeout.tv_usec = (command_timeout % 1000) * 1000;
    }
}

/******************************************************************************
 * redis.push();
 *****************************************************************************/

VCL_VOID
vmod_push(VRT_CTX, struct vmod_priv *task_priv, VCL_STRING arg)
{
    // Fetch local thread state.
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to redis.command() was not executed.
    if ((state->argc >= 1) && (state->argc < MAX_REDIS_COMMAND_ARGS)) {
        // Handle NULL arguments as empty strings.
        if (arg != NULL) {
            state->argv[state->argc++] = WS_Copy(ctx->ws, arg, -1);;
        } else {
            state->argv[state->argc++] = WS_Copy(ctx->ws, "", -1);
        }
        AN(state->argv[state->argc - 1]);
    } else {
        REDIS_LOG(ctx,
            "Failed to push Redis argument (limit is %d)",
            MAX_REDIS_COMMAND_ARGS);
    }
}

/******************************************************************************
 * redis.execute();
 *****************************************************************************/

VCL_VOID
vmod_execute(VRT_CTX, struct vmod_priv *vcl_priv, struct vmod_priv *task_priv)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);

    // Do not continue if the initial call to redis.command() was not
    // executed.
    if (state->argc >= 1) {
        // Clustered vs. classic execution.
        if ((config->clustered) &&
            ((state->tag == NULL) ||
             (strcmp(state->tag, CLUSTERED_REDIS_SERVER_TAG) == 0))) {
            state->reply = cluster_execute(
                ctx, config, state,
                version, state->timeout, state->argc, state->argv);
        } else {
            int tries = 1 + config->retries;
            while ((tries > 0) && (state->reply == NULL)) {
                state->reply = redis_execute(
                    ctx, config, state,
                    state->tag, version, state->timeout, state->argc, state->argv, 0);
                tries--;
            }
        }

        // Log error replies (other errors are already logged while executing
        // commands).
        if ((state->reply != NULL) && (state->reply->type == REDIS_REPLY_ERROR)) {
            REDIS_LOG(ctx,
                "Got error reply while executing Redis command (%s): %s",
                state->argv[0],
                state->reply->str);
        }
    }
}

/******************************************************************************
 * redis.replied();
 *****************************************************************************/

VCL_BOOL
vmod_replied(VRT_CTX, struct vmod_priv *task_priv)
{
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);
    return (state->reply != NULL);
}

/******************************************************************************
 * redis.reply_is_error();
 * redis.reply_is_nil();
 * redis.reply_is_status();
 * redis.reply_is_integer();
 * redis.reply_is_string();
 * redis.reply_is_array();
 *****************************************************************************/

#define VMOD_REPLY_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_reply_is_ ## lower(VRT_CTX, struct vmod_priv *task_priv) \
{ \
    task_priv_t *state = get_task_priv(ctx, task_priv, 0); \
    return \
        (state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ ## upper); \
}

VMOD_REPLY_IS_FOO(error, ERROR)
VMOD_REPLY_IS_FOO(nil, NIL)
VMOD_REPLY_IS_FOO(status, STATUS)
VMOD_REPLY_IS_FOO(integer, INTEGER)
VMOD_REPLY_IS_FOO(string, STRING)
VMOD_REPLY_IS_FOO(array, ARRAY)

/******************************************************************************
 * redis.get_reply();
 *****************************************************************************/

VCL_STRING
vmod_get_reply(VRT_CTX, struct vmod_priv *task_priv)
{
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);
    if (state->reply != NULL) {
        return get_reply(ctx, state->reply);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * redis.get_error_reply();
 * redis.get_status_reply();
 * redis.get_integer_reply();
 * redis.get_string_reply();
 *****************************************************************************/

VCL_INT
vmod_get_integer_reply(VRT_CTX, struct vmod_priv *task_priv)
{
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_INTEGER)) {
        return state->reply->integer;
    } else {
        return 0;
    }
}

#define VMOD_GET_FOO_REPLY(lower, upper) \
VCL_STRING \
vmod_get_ ## lower ## _reply(VRT_CTX, struct vmod_priv *task_priv) \
{ \
    task_priv_t *state = get_task_priv(ctx, task_priv, 0); \
    if ((state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ ## upper)) { \
        char *result = WS_Copy(ctx->ws, state->reply->str, state->reply->len + 1); \
        AN(result); \
        return result; \
    } else { \
        return NULL; \
    } \
}

VMOD_GET_FOO_REPLY(error, ERROR)
VMOD_GET_FOO_REPLY(status, STATUS)
VMOD_GET_FOO_REPLY(string, STRING)

/******************************************************************************
 * redis.get_array_reply_length();
 *****************************************************************************/

VCL_INT
vmod_get_array_reply_length(VRT_CTX, struct vmod_priv *task_priv)
{
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_ARRAY)) {
        return state->reply->elements;
    } else {
        return 0;
    }
}

/******************************************************************************
 * redis.array_reply_is_error();
 * redis.array_reply_is_nil();
 * redis.array_reply_is_status();
 * redis.array_reply_is_integer();
 * redis.array_reply_is_string();
 * redis.array_reply_is_array();
 *****************************************************************************/

#define VMOD_ARRAY_REPLY_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_array_reply_is_ ## lower(VRT_CTX, struct vmod_priv *task_priv, VCL_INT index) \
{ \
    task_priv_t *state = get_task_priv(ctx, task_priv, 0); \
    return \
        (state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ARRAY) && \
        (index < state->reply->elements) && \
        (state->reply->element[index]->type == REDIS_REPLY_ ## upper); \
}

VMOD_ARRAY_REPLY_IS_FOO(error, ERROR)
VMOD_ARRAY_REPLY_IS_FOO(nil, NIL)
VMOD_ARRAY_REPLY_IS_FOO(status, STATUS)
VMOD_ARRAY_REPLY_IS_FOO(integer, INTEGER)
VMOD_ARRAY_REPLY_IS_FOO(string, STRING)
VMOD_ARRAY_REPLY_IS_FOO(array, ARRAY)

/******************************************************************************
 * redis.get_array_reply_value();
 *****************************************************************************/

VCL_STRING
vmod_get_array_reply_value(VRT_CTX, struct vmod_priv *task_priv, VCL_INT index)
{
    task_priv_t *state = get_task_priv(ctx, task_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_ARRAY) &&
        (index < state->reply->elements)) {
        return get_reply(ctx, state->reply->element[index]);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * redis.free();
 *****************************************************************************/

VCL_VOID
vmod_free(VRT_CTX, struct vmod_priv *task_priv)
{
    get_task_priv(ctx, task_priv, 1);
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static task_priv_t *
get_task_priv(VRT_CTX, struct vmod_priv *task_priv, unsigned flush)
{
    // Initializations.
    task_priv_t *result = NULL;

    // Create thread state if not created yet.
    if (task_priv->priv == NULL) {
        task_priv->priv = new_task_priv();
        task_priv->free = (vmod_priv_free_f *)free_task_priv;
        result = task_priv->priv;
    } else {
        result = task_priv->priv;
        CHECK_OBJ(result, TASK_PRIV_MAGIC);
    }

    // Drop previously stored Redis command?
    if (flush) {
        result->timeout = (struct timeval){ 0 };
        result->tag = NULL;
        result->argc = 0;
        if (result->reply != NULL) {
            freeReplyObject(result->reply);
            result->reply = NULL;
        }
    }

    // Done!
    return result;
}

static redis_server_t *
unsafe_add_redis_server(
    VRT_CTX, vcl_priv_t *config,
    const char *tag, const char *location, struct timeval connection_timeout, unsigned context_ttl)
{
    // Initializations.
    redis_server_t *result = new_redis_server(tag, location, connection_timeout, context_ttl);

    // Do not continue if we failed to create the server instance.
    // Caller should own config->mutex!
    if (result != NULL) {
        // Add new server.
        VTAILQ_INSERT_TAIL(&config->servers, result, list);

        // If required, add new pool.
        if (unsafe_get_context_pool(config, result->tag) == NULL) {
            redis_context_pool_t *pool = new_redis_context_pool(result->tag);
            VTAILQ_INSERT_TAIL(&config->pools, pool, list);
        }
    } else {
        REDIS_LOG(ctx,
            "Failed to add server '%s' tagged as '%s'",
            location, tag);
    }

    // Done!
    return result;
}

static const char *
get_reply(VRT_CTX, redisReply *reply)
{
    // Default result.
    const char *result = NULL;

    // Check type of Redis reply.
    switch (reply->type) {
        case REDIS_REPLY_ERROR:
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
            result = WS_Copy(ctx->ws, reply->str, reply->len + 1);
            AN(result);
            break;

        case REDIS_REPLY_INTEGER:
            result = WS_Printf(ctx->ws, "%lld", reply->integer);
            AN(result);
            break;

        case REDIS_REPLY_ARRAY:
            // XXX: array replies are *not* supported.
            result = NULL;
            break;

        default:
            result = NULL;
    }

    // Done!
    return result;
}

static void
fini(vcl_priv_t *config)
{
    // Get config lock.
    AZ(pthread_mutex_lock(&config->mutex));

    // Release contexts in all pools.
    redis_context_pool_t *ipool;
    VTAILQ_FOREACH(ipool, &config->pools, list) {
        // Get pool lock.
        AZ(pthread_mutex_lock(&ipool->mutex));

        // Release all contexts.
        ipool->ncontexts = 0;
        redis_context_t *icontext;
        while (!VTAILQ_EMPTY(&ipool->free_contexts)) {
            icontext = VTAILQ_FIRST(&ipool->free_contexts);
            VTAILQ_REMOVE(&ipool->free_contexts, icontext, list);
            free_redis_context(icontext);
        }
        while (!VTAILQ_EMPTY(&ipool->busy_contexts)) {
            icontext = VTAILQ_FIRST(&ipool->busy_contexts);
            VTAILQ_REMOVE(&ipool->busy_contexts, icontext, list);
            free_redis_context(icontext);
        }

        // Release pool lock.
        AZ(pthread_mutex_lock(&ipool->mutex));
    }

    // Release config lock.
    AZ(pthread_mutex_unlock(&config->mutex));
}
