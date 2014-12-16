#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "cluster.h"
#include "core.h"

#define DEFAULT_SHARED_CONTEXTS 0
#define DEFAULT_MAX_CONTEXTS 1

static unsigned version = 0;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static thread_state_t *get_thread_state(const struct vrt_ctx *ctx, unsigned drop_reply);
static void make_thread_key();

static redis_server_t *unsafe_add_redis_server(
    const struct vrt_ctx *ctx, vcl_priv_t *config,
    const char *tag, const char *location, unsigned timeout, unsigned ttl);

static const char *get_reply(const struct vrt_ctx *ctx, redisReply *reply);

/******************************************************************************
 * VMOD INITIALIZATION.
 *****************************************************************************/

int
init_function(struct vmod_priv *vcl_priv, const struct VCL_conf *conf)
{
    // Initialize global state shared with all VCLs. This code *is
    // required* to be thread safe.
    //   - Initialize (once) the key required to store thread-specific data.
    //   - Increase (every time the VMOD is initialized) the global version.
    //     This will be used to reestablish Redis connections binded to
    //     worker threads during reloads. Pooled connections shared between
    //     threads are stored in the local VCL data structure, which is
    //     regenerated every time the VMOD is initialized.
    AZ(pthread_once(&thread_once, make_thread_key));
    AZ(pthread_mutex_lock(&mutex));
    version++;
    AZ(pthread_mutex_unlock(&mutex));

    // Initialize the local VCL data structure and set its free function.
    // Code initializing / freeing the VCL private data structure *is
    // not required* to be thread safe.
    if (vcl_priv->priv == NULL) {
        vcl_priv->priv = new_vcl_priv(DEFAULT_SHARED_CONTEXTS, DEFAULT_MAX_CONTEXTS);
        vcl_priv->free = (vmod_priv_free_f *)free_vcl_priv;
    }

    // Done!
    return 0;
}

/******************************************************************************
 * redis.init();
 *****************************************************************************/

void
vmod_init(
    const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv,
    VCL_STRING tag, VCL_STRING location, VCL_INT timeout, VCL_INT ttl,
    VCL_BOOL shared_contexts, VCL_INT max_contexts)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (location != NULL) && (strlen(location) > 0) &&
        (max_contexts > 0)) {
        // Create new configuration.
        vcl_priv_t *config = new_vcl_priv(shared_contexts, max_contexts);

        // Add initial server.
        redis_server_t *server = unsafe_add_redis_server(
            ctx, config, tag, location, timeout, ttl);

        // Do not continue if we failed to create the server instance.
        if (server != NULL) {
            // If a clustered server was added, enable clustering, set clustering
            // options, and populate the slots-tags mapping.
            if (server->clustered) {
                config->clustered = 1;
                config->timeout = timeout;
                config->ttl = ttl;
                discover_cluster_slots(ctx, config);
            }

            // Set the new configuration & release the previous one.
            vcl_priv_t *old_config = vcl_priv->priv;
            vcl_priv->priv = config;
            free_vcl_priv(old_config);

        // Failed to create the server instance.
        } else {
            free_vcl_priv(config);
        }
    }
}

/******************************************************************************
 * redis.add_server();
 *****************************************************************************/

void
vmod_add_server(
    const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv,
    VCL_STRING tag, VCL_STRING location, VCL_INT timeout, VCL_INT ttl)
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
            // Get config lock.
            AZ(pthread_mutex_lock(&config->mutex));

            // Add new server.
            unsafe_add_redis_server(ctx, config, tag, location, timeout, ttl);

            // Release config lock.
            AZ(pthread_mutex_unlock(&config->mutex));

        // Reserved tag.
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

void
vmod_add_cserver(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv, const char *location)
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
                location, config->timeout, config->ttl);

            // Release config lock.
            AZ(pthread_mutex_unlock(&config->mutex));

        // Clustering is not enabled
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

void
vmod_command(const struct vrt_ctx *ctx, VCL_STRING name)
{
    // Check input.
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(ctx, 1);

        // Initialize.
        state->argc = 1;
        state->argv[0] = WS_Copy(ctx->ws, name, -1);
        AN(state->argv[0]);
    }
}

/******************************************************************************
 * redis.server();
 *****************************************************************************/

void
vmod_server(const struct vrt_ctx *ctx, const char *tag)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0)) {
        // Fetch local thread state.
        thread_state_t *state = get_thread_state(ctx, 0);

        // Do not continue if the initial call to redis.command() was not
        // executed.
        if (state->argc >= 1) {
            state->tag = WS_Copy(ctx->ws, tag, -1);
            AN(state->tag);
        }
    }
}

/******************************************************************************
 * redis.push();
 *****************************************************************************/

void
vmod_push(const struct vrt_ctx *ctx, VCL_STRING arg)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

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

void
vmod_execute(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the initial call to redis.command() was not
    // executed.
    if (state->argc >= 1) {
        // Clustered vs. classic execution.
        if ((config->clustered) &&
            ((state->tag == NULL) ||
             (strcmp(state->tag, CLUSTERED_REDIS_SERVER_TAG) == 0))) {
            state->reply = cluster_execute(
                ctx, config, state,
                version, state->argc, state->argv);
        } else {
            state->reply = redis_execute(
                ctx, config, state,
                state->tag, version, state->argc, state->argv, 0);
        }

        // Log error replies.
        if ((state->reply != NULL) && (state->reply->type == REDIS_REPLY_ERROR)) {
            REDIS_LOG(ctx,
                "Got error reply while executing Redis command (%s): %s",
                state->argv[0],
                state->reply->str);
        }
    }
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
vmod_reply_is_ ## lower(const struct vrt_ctx *ctx) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
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
vmod_get_reply(const struct vrt_ctx *ctx)
{
    thread_state_t *state = get_thread_state(ctx, 0);
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
vmod_get_integer_reply(const struct vrt_ctx *ctx)
{
    thread_state_t *state = get_thread_state(ctx, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_INTEGER)) {
        return state->reply->integer;
    } else {
        return 0;
    }
}

#define VMOD_GET_FOO_REPLY(lower, upper) \
VCL_STRING \
vmod_get_ ## lower ## _reply(const struct vrt_ctx *ctx) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
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
vmod_get_array_reply_length(const struct vrt_ctx *ctx)
{
    thread_state_t *state = get_thread_state(ctx, 0);
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
vmod_array_reply_is_ ## lower(const struct vrt_ctx *ctx, VCL_INT index) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
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
vmod_get_array_reply_value(const struct vrt_ctx *ctx, VCL_INT index)
{
    thread_state_t *state = get_thread_state(ctx, 0);
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

void
vmod_free(const struct vrt_ctx *ctx)
{
    get_thread_state(ctx, 1);
}

/******************************************************************************
 * redis.fini();
 *****************************************************************************/

void
vmod_fini(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;

    // Get config lock.
    AZ(pthread_mutex_lock(&config->mutex));

    redis_context_pool_t *ipool;
    VTAILQ_FOREACH(ipool, &config->pools, list) {
        // Check object.
        CHECK_OBJ_NOTNULL(ipool, REDIS_CONTEXT_POOL_MAGIC);

        // Get pool lock.
        AZ(pthread_mutex_lock(&ipool->mutex));

        // Release all connections.
        ipool->ncontexts = 0;
        while (!VTAILQ_EMPTY(&ipool->free_contexts)) {
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
        }

        // Release pool lock.
        AZ(pthread_mutex_lock(&ipool->mutex));
    }

    // Release config lock.
    AZ(pthread_mutex_unlock(&config->mutex));
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static thread_state_t *
get_thread_state(const struct vrt_ctx *ctx, unsigned flush)
{
    // Initializations.
    thread_state_t *result = pthread_getspecific(thread_key);

    // Create thread state if not created yet.
    if (result == NULL) {
        result = new_thread_state();
        pthread_setspecific(thread_key, result);
    } else {
        CHECK_OBJ(result, THREAD_STATE_MAGIC);
    }

    // Drop previously stored Redis command?
    if (flush) {
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

static void
make_thread_key()
{
    AZ(pthread_key_create(&thread_key, (void *) free_thread_state));
}

static redis_server_t *
unsafe_add_redis_server(
    const struct vrt_ctx *ctx, vcl_priv_t *config,
    const char *tag, const char *location, unsigned timeout, unsigned ttl)
{
    // Initializations.
    redis_server_t *result = new_redis_server(tag, location, timeout, ttl);

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

    // Failed to create the server instance.
    } else {
        REDIS_LOG(ctx,
            "Failed to add server '%s' tagged as '%s'",
            location, tag);
    }

    // Done!
    return result;
}

static const char *
get_reply(const struct vrt_ctx *ctx, redisReply *reply)
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
