#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"
#include "vcc_if.h"

#include "sha1.h"
#include "core.h"

#define DEFAULT_SHARED_CONTEXTS 0
#define DEFAULT_MAX_CONTEXTS 1

static unsigned version = 0;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static thread_state_t *get_thread_state(struct sess *sp, unsigned drop_reply);
static void make_thread_key();

static unsigned unsafe_server_exists(vcl_priv_t *config, const char *tag);
static unsigned unsafe_pool_exists(vcl_priv_t *config, const char *tag);

static const char *get_reply(struct sess *sp, redisReply *reply);

static const char *sha1(struct sess *sp, const char *script);

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
    //     This will be used to reestablish Redis connections on reloads.
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
    struct sess *sp, struct vmod_priv *vcl_priv,
    const char *tag, const char *location, int timeout, int ttl,
    unsigned shared_contexts, int max_contexts)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (location != NULL) && (strlen(location) > 0) &&
        (max_contexts > 0)) {
        // Do not continue if 'tag' starts with the reserved prefix internally
        // used for clustered servers.
        if (strncmp(tag, CLUSTERED_REDIS_SERVER_TAG_PREFIX,
                    strlen(CLUSTERED_REDIS_SERVER_TAG_PREFIX)) != 0) {
            // Initializations.
            redis_server_t *server = new_redis_server(tag, location, timeout, ttl);

            // Do not continue if we failed to create the server instance.
            if (server != NULL) {
                // Create new configuration.
                vcl_priv_t *new = new_vcl_priv(shared_contexts, max_contexts);
                VTAILQ_INSERT_TAIL(&new->servers, server, list);
                redis_context_pool_t *pool = new_redis_context_pool(server->tag);
                VTAILQ_INSERT_TAIL(&new->pools, pool, list);

                // Set new configuration & release previous one.
                vcl_priv_t *old = vcl_priv->priv;
                vcl_priv->priv = new;
                free_vcl_priv(old);

            // Failed to create the server instance.
            } else {
                REDIS_LOG(sp,
                    "Failed to add server '%s' tagged as '%s'",
                    location, tag);
            }

        // Invalid tag prefix.
        } else {
            REDIS_LOG(sp,
                "Tags prefixed with the '%s' string are not allowed",
                CLUSTERED_REDIS_SERVER_TAG_PREFIX);
        }
    }
}

/******************************************************************************
 * redis.add_server();
 *****************************************************************************/

void
vmod_add_server(
    struct sess *sp, struct vmod_priv *vcl_priv,
    const char *tag, const char *location, int timeout, int ttl)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (location != NULL) && (strlen(location) > 0)) {
        // Do not continue if 'tag' starts with the reserved prefix internally
        // used for clustered servers.
        if (strncmp(tag, CLUSTERED_REDIS_SERVER_TAG_PREFIX,
                    strlen(CLUSTERED_REDIS_SERVER_TAG_PREFIX)) != 0) {
            // Initializations.
            vcl_priv_t *config = vcl_priv->priv;
            redis_server_t *server = new_redis_server(tag, location, timeout, ttl);

            // Do not continue if we failed to create the server instance.
            if (server != NULL) {
                // Get config lock.
                AZ(pthread_mutex_lock(&config->mutex));

                // Clustered server shouldn't be duplicated.
                if (!server->clustered || !unsafe_server_exists(config, server->tag)) {
                    // Add new server.
                    VTAILQ_INSERT_TAIL(&config->servers, server, list);

                    // If required, add new pool.
                    if (!unsafe_pool_exists(config, server->tag)) {
                        redis_context_pool_t *pool = new_redis_context_pool(server->tag);
                        VTAILQ_INSERT_TAIL(&config->pools, pool, list);
                    }

                // Duplicated clustered server.
                } else {
                    free_redis_server(server);
                }

                // Release config lock.
                AZ(pthread_mutex_unlock(&config->mutex));

            // Failed to create the server instance.
            } else {
                REDIS_LOG(sp,
                    "Failed to add server '%s' tagged as '%s'",
                    location, tag);
            }

        // Invalid tag prefix.
        } else {
            REDIS_LOG(sp,
                "Tags prefixed with the '%s' string are not allowed",
                CLUSTERED_REDIS_SERVER_TAG_PREFIX);
        }
    }
}

/******************************************************************************
 * redis.command();
 *****************************************************************************/

void
vmod_command(struct sess *sp, const char *name)
{
    // Check input.
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(sp, 1);

        // Convert command name to uppercase (for later comparison with the
        // 'EVAL' string).
        char *command = WS_Dup(sp->ws, name);
        AN(command);
        char *ptr = command;
        while (*ptr) {
            *ptr = toupper(*ptr);
            ptr++;
        }

        // Initialize.
        state->argc = 1;
        state->argv[0] = command;
    }
}

/******************************************************************************
 * redis.server();
 *****************************************************************************/

void
vmod_server(struct sess *sp, const char *tag)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0)) {
        // Fetch local thread state.
        thread_state_t *state = get_thread_state(sp, 0);

        // Do not continue if the initial call to redis.command() was not
        // executed.
        if (state->argc >= 1) {
            state->tag = WS_Dup(sp->ws, tag);
            AN(state->tag);
        }
    }
}

/******************************************************************************
 * redis.push();
 *****************************************************************************/

void
vmod_push(struct sess *sp, const char *arg)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(sp, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to redis.command() was not executed.
    if ((state->argc >= 1) && (state->argc < MAX_REDIS_COMMAND_ARGS)) {
        // Handle NULL arguments as empty strings.
        if (arg != NULL) {
            state->argv[state->argc++] = WS_Dup(sp->ws, arg);
        } else {
            state->argv[state->argc++] = WS_Dup(sp->ws, "");
        }
        AN(state->argv[state->argc - 1]);
    } else {
        REDIS_LOG(sp,
            "Failed to push Redis argument (limit is %d)",
            MAX_REDIS_COMMAND_ARGS);
    }
}

/******************************************************************************
 * redis.execute();
 *****************************************************************************/

void
vmod_execute(struct sess *sp, struct vmod_priv *vcl_priv)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(sp, 0);

    // Fetch context.
    redis_context_t *context = get_context(sp, vcl_priv, state, version);

    // Do not continue if a Redis context is not available or if the initial
    // call to redis.command() was not executed.
    if ((state->argc >= 1) && (context != NULL)) {
        // When executing EVAL commands, first try with EVALSHA.
        unsigned done = 0;
        if ((strcmp(state->argv[0], "EVAL") == 0) && (state->argc >= 2)) {
            // Replace EVAL with EVALSHA.
            state->argv[0] = WS_Dup(sp->ws, "EVALSHA");
            AN(state->argv[0]);
            const char *script = state->argv[1];
            state->argv[1] = sha1(sp, script);

            // Execute the EVALSHA command.
            state->reply = redisCommandArgv(
                context->rcontext,
                state->argc,
                state->argv,
                NULL);

            // Check reply. If Redis replies with a NOSCRIPT, the original
            // EVAL command should be executed to register the script for
            // the first time in the Redis server.
            if (!context->rcontext->err &&
                (state->reply != NULL) &&
                (state->reply->type == REDIS_REPLY_ERROR) &&
                (strncmp(state->reply->str, "NOSCRIPT", 8) == 0)) {
                // Replace EVALSHA with EVAL.
                state->argv[0] = WS_Dup(sp->ws, "EVAL");
                AN(state->argv[0]);
                state->argv[1] = script;

            // The command execution is completed.
            } else {
                done = 1;
            }
        }

        // Send command, unless it was originally an EVAL command and it
        // was already executed using EVALSHA.
        if (!done) {
            state->reply = redisCommandArgv(
                context->rcontext,
                state->argc,
                state->argv,
                NULL);
        }

        // Check reply.
        if (context->rcontext->err) {
            REDIS_LOG(sp,
                "Failed to execute Redis command (%s): [%d] %s",
                state->argv[0],
                context->rcontext->err,
                context->rcontext->errstr);
        } else if (state->reply == NULL) {
            REDIS_LOG(sp,
                "Failed to execute Redis command (%s)",
                state->argv[0]);
        } else if (state->reply->type == REDIS_REPLY_ERROR) {
            REDIS_LOG(sp,
                "Got error reply while executing Redis command (%s): %s",
                state->argv[0],
                state->reply->str);
        }

        // Release context.
        free_context(sp, vcl_priv, state, context);
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
unsigned \
vmod_reply_is_ ## lower(struct sess *sp) \
{ \
    thread_state_t *state = get_thread_state(sp, 0); \
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

const char *
vmod_get_reply(struct sess *sp)
{
    thread_state_t *state = get_thread_state(sp, 0);
    if (state->reply != NULL) {
        return get_reply(sp, state->reply);
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

int
vmod_get_integer_reply(struct sess *sp)
{
    thread_state_t *state = get_thread_state(sp, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_INTEGER)) {
        return state->reply->integer;
    } else {
        return 0;
    }
}

#define VMOD_GET_FOO_REPLY(lower, upper) \
const char * \
vmod_get_ ## lower ## _reply(struct sess *sp) \
{ \
    thread_state_t *state = get_thread_state(sp, 0); \
    if ((state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ ## upper)) { \
        char *result = WS_Dup(sp->wrk->ws, state->reply->str); \
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

int
vmod_get_array_reply_length(struct sess *sp)
{
    thread_state_t *state = get_thread_state(sp, 0);
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
unsigned \
vmod_array_reply_is_ ## lower(struct sess *sp, int index) \
{ \
    thread_state_t *state = get_thread_state(sp, 0); \
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

const char *
vmod_get_array_reply_value(struct sess *sp, int index)
{
    thread_state_t *state = get_thread_state(sp, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_ARRAY) &&
        (index < state->reply->elements)) {
        return get_reply(sp, state->reply->element[index]);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * redis.free();
 *****************************************************************************/

void
vmod_free(struct sess *sp)
{
    get_thread_state(sp, 1);
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static thread_state_t *
get_thread_state(struct sess *sp, unsigned flush)
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

static unsigned
unsafe_server_exists(vcl_priv_t *config, const char *tag)
{
    // Initializations.
    unsigned result = 0;

    // Look for a server matching the tag.
    // Caller should own config->mutex!
    redis_server_t *iserver;
    VTAILQ_FOREACH(iserver, &config->servers, list) {
        if (strcmp(tag, iserver->tag) == 0) {
            CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
            result = 1;
            break;
        }
    }

    // Done!
    return result;
}

static unsigned
unsafe_pool_exists(vcl_priv_t *config, const char *tag)
{
    // Initializations.
    unsigned result = 0;

    // Look for a pool matching the tag.
    // Caller should own config->mutex!
    redis_context_pool_t *ipool;
    VTAILQ_FOREACH(ipool, &config->pools, list) {
        if (strcmp(tag, ipool->tag) == 0) {
            CHECK_OBJ_NOTNULL(ipool, REDIS_CONTEXT_POOL_MAGIC);
            result = 1;
            break;
        }
    }

    // Done!
    return result;
}

static const char *
get_reply(struct sess *sp, redisReply *reply)
{
    // Default result.
    const char *result = NULL;

    // Check type of Redis reply.
    // XXX: array replies are *not* supported.
    char buffer[64];
    switch (reply->type) {
        case REDIS_REPLY_ERROR:
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
            result = WS_Dup(sp->wrk->ws, reply->str);
            AN(result);
            break;

        case REDIS_REPLY_INTEGER:
            sprintf(buffer, "%lld", reply->integer);
            result = WS_Dup(sp->wrk->ws, buffer);
            AN(result);
            break;

        case REDIS_REPLY_ARRAY:
            result = WS_Dup(sp->wrk->ws, "array");
            AN(result);
            break;

        default:
            result = NULL;
    }

    // Done!
    return result;
}

static const char *
sha1(struct sess *sp, const char *script)
{
    // Hash.
    unsigned char buffer[20];
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, script, strlen(script));
    SHA1Final(buffer, &ctx);

    // Encode.
    char *result = WS_Alloc(sp->wrk->ws, 41);
    AN(result);
    char *ptr = result;
    for (int i = 0; i < 20; i++) {
        sprintf(ptr, "%02x", buffer[i]);
        ptr += 2;
    }

    // Done!
    return result;
}
