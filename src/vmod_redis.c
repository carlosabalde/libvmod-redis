#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "sha1.h"

static unsigned version = 0;

typedef struct vcl_priv {
#define VCL_PRIV_MAGIC 0x77feec11
    unsigned magic;
    // Redis.
    const char *host;
    unsigned port;
    struct timeval timeout;
    unsigned ttl;
} vcl_priv_t;

typedef struct thread_state {
#define THREAD_STATE_MAGIC 0xa6bc103e
    unsigned magic;
    // Redis context.
    redisContext *context;
    unsigned context_version;
    time_t context_tst;

    // Redis command: arguments (allocated in the session workspace) + reply.
#define MAX_REDIS_COMMAND_ARGS 128
    unsigned argc;
    const char *argv[MAX_REDIS_COMMAND_ARGS];
    redisReply *reply;
} thread_state_t;

#define REDIS_LOG(ctx, message, ...) \
    do { \
        char _buffer[512]; \
        snprintf( \
            _buffer, sizeof(_buffer), \
            "[REDIS][%s] %s", __func__, message); \
        VSLb(ctx->vsl, SLT_Error, _buffer, ##__VA_ARGS__); \
    } while (0)

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static vcl_priv_t * new_vcl_priv();
static void free_vcl_priv(vcl_priv_t *priv);

static thread_state_t *get_thread_state(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv, unsigned drop_reply);
static void make_thread_key();

static const char *get_reply(const struct vrt_ctx *ctx, redisReply *reply);

static const char *sha1(const struct vrt_ctx *ctx, const char *script);

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
        vcl_priv->priv = new_vcl_priv("127.0.0.1", 6379, 500);
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
    VCL_STRING host, VCL_INT port, VCL_INT timeout, VCL_INT ttl)
{
    vcl_priv_t *old = vcl_priv->priv;
    vcl_priv->priv = new_vcl_priv(host, port, timeout, ttl);
    if (old != NULL) {
        free_vcl_priv(old);
    }
}

/******************************************************************************
 * redis.call();
 *****************************************************************************/

void
vmod_call(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv, VCL_STRING command, ...)
{
    // Check input.
    if (command != NULL) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(ctx, vcl_priv, 1);

        // Do not continue if a Redis context is not available.
        if (state->context != NULL) {
            // Send command.
            // XXX: beware of the ugly hack to support usage of '%s'
            // placeholders in VCL.
            va_list args;
            va_start(args, command);
            state->reply = redisvCommand(state->context, command, args);
            va_end(args);

            // Check reply.
            if (state->context->err) {
                REDIS_LOG(ctx,
                    "Failed to execute Redis command (%s): [%d] %s",
                    command,
                    state->context->err,
                    state->context->errstr);
            } else if (state->reply == NULL) {
                REDIS_LOG(ctx,
                    "Failed to execute Redis command (%s)",
                    command);
            } else if (state->reply->type == REDIS_REPLY_ERROR) {
                REDIS_LOG(ctx,
                    "Got error reply while executing Redis command (%s): %s",
                    command,
                    state->reply->str);
            }
        }
    }
}

/******************************************************************************
 * redis.command();
 *****************************************************************************/

void
vmod_command(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv, VCL_STRING name)
{
    // Check input.
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(ctx, vcl_priv, 1);

        // Convert command name to uppercase (for later comparison with the
        // 'EVAL' string).
        char *command = WS_Copy(ctx->ws, name, -1);
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
 * redis.push();
 *****************************************************************************/

void
vmod_push(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv, VCL_STRING arg)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to redis.command() was not executed.
    if ((state->argc >= 1) && (state->argc < MAX_REDIS_COMMAND_ARGS)) {
        // Handle NULL arguments as empty strings.
        if (arg != NULL) {
            state->argv[state->argc++] = arg;
        } else {
            state->argv[state->argc++] = WS_Copy(ctx->ws, "", -1);
            AN(state->argv[state->argc - 1]);
        }
    } else {
        REDIS_LOG(ctx, "Failed to push Redis argument");
    }
}

/******************************************************************************
 * redis.execute();
 *****************************************************************************/

void
vmod_execute(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0);

    // Do not continue if a Redis context is not available or if the initial
    // call to redis.command() was not executed.
    if ((state->argc >= 1) && (state->context != NULL)) {
        // When executing EVAL commands, first try with EVALSHA.
        unsigned done = 0;
        if ((strcmp(state->argv[0], "EVAL") == 0) && (state->argc >= 2)) {
            // Replace EVAL with EVALSHA.
            state->argv[0] = WS_Copy(ctx->ws, "EVALSHA", -1);
            AN(state->argv[0]);
            const char *script = state->argv[1];
            state->argv[1] = sha1(ctx, script);

            // Execute the EVALSHA command.
            state->reply = redisCommandArgv(
                state->context,
                state->argc,
                state->argv,
                NULL);

            // Check reply. If Redis replies with a NOSCRIPT, the original
            // EVAL command should be executed to register the script for
            // the first time in the Redis server.
            if (!state->context->err &&
                (state->reply != NULL) &&
                (state->reply->type == REDIS_REPLY_ERROR) &&
                (strncmp(state->reply->str, "NOSCRIPT", 8) == 0)) {
                // Replace EVALSHA with EVAL.
                state->argv[0] = WS_Copy(ctx->ws, "EVAL", -1);
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
                state->context,
                state->argc,
                state->argv,
                NULL);
        }

        // Check reply.
        if (state->context->err) {
            REDIS_LOG(ctx,
                "Failed to execute Redis command (%s): [%d] %s",
                state->argv[0],
                state->context->err,
                state->context->errstr);
        } else if (state->reply == NULL) {
            REDIS_LOG(ctx,
                "Failed to execute Redis command (%s)",
                state->argv[0]);
        } else if (state->reply->type == REDIS_REPLY_ERROR) {
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
vmod_reply_is_ ## lower(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv) \
{ \
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0); \
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
vmod_get_reply(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0);
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

#define VMOD_GET_FOO_REPLY(lower, upper, return_type, reply_field, fallback_value) \
return_type \
vmod_get_ ## lower ## _reply(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv) \
{ \
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0); \
    if ((state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ ## upper)) { \
        return state->reply->reply_field; \
    } else { \
        return fallback_value; \
    } \
}

VMOD_GET_FOO_REPLY(error, ERROR, VCL_STRING, str, NULL)
VMOD_GET_FOO_REPLY(status, STATUS, VCL_STRING, str, NULL)
VMOD_GET_FOO_REPLY(integer, INTEGER, VCL_INT, integer, 0)

VCL_STRING
vmod_get_string_reply(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_STRING)) {
        return WS_Copy(ctx->ws, state->reply->str, -1);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * redis.get_array_reply_length();
 *****************************************************************************/

VCL_INT
vmod_get_array_reply_length(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_ARRAY)) {
        return state->reply->elements;
    } else {
        return 0;
    }
}

/******************************************************************************
 * redis.get_array_reply_value();
 *****************************************************************************/

VCL_STRING
vmod_get_array_reply_value(
    const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv,
    VCL_INT index)
{
    thread_state_t *state = get_thread_state(ctx, vcl_priv, 0);
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
vmod_free(const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv)
{
    get_thread_state(ctx, vcl_priv, 1);
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static vcl_priv_t *
new_vcl_priv(const char *host, unsigned port, unsigned timeout, unsigned ttl)
{
    vcl_priv_t *result;
    ALLOC_OBJ(result, VCL_PRIV_MAGIC);
    AN(result);

    result->host = strdup(host);
    AN(result->host);
    result->port = port;
    result->timeout.tv_sec = timeout / 1000;;
    result->timeout.tv_usec = (timeout % 1000) * 1000;
    result->ttl = ttl;

    return result;
}

static void
free_vcl_priv(vcl_priv_t *priv)
{
    free((void *) priv->host);

    FREE_OBJ(priv);
}

static thread_state_t *
get_thread_state(
    const struct vrt_ctx *ctx, struct vmod_priv *vcl_priv,
    unsigned flush)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;
    thread_state_t *result = pthread_getspecific(thread_key);
    time_t now = time(NULL);

    // Create thread state if not created yet, and flush Redis context
    // if it's in an error state or it's too old.
    if (result == NULL) {
        ALLOC_OBJ(result, THREAD_STATE_MAGIC);
        AN(result);

        result->context = NULL;
        result->context_version = 0;
        result->context_tst = 0;
        result->argc = 0;
        result->reply = NULL;

        pthread_setspecific(thread_key, result);
    } else {
        CHECK_OBJ(result, THREAD_STATE_MAGIC);

        if ((result->context != NULL) &&
            (result->context->err ||
             (result->context_version != version) ||
             ((config->ttl > 0) && (now - result->context_tst > config->ttl)))) {
            redisFree(result->context);
            result->context = NULL;
            result->context_version = 0;
            result->context_tst = 0;
        }
    }

    // Create Redis context. If any error arises discard the context and
    // continue.
    if (result->context == NULL) {
        result->context = redisConnectWithTimeout(config->host, config->port, config->timeout);
        AN(result->context);
        if (result->context->err) {
            REDIS_LOG(ctx,
                "Failed to establish Redis connection (%d): %s",
                result->context->err,
                result->context->errstr);
            redisFree(result->context);
            result->context = NULL;
            result->context_version = 0;
            result->context_tst = 0;
        } else {
            result->context_version = version;
            result->context_tst = now;
        }
    }

    // Drop previously stored Redis command.
    if (flush) {
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
free_thread_state(thread_state_t *state)
{
    if (state->context != NULL) {
        redisFree(state->context);
    }

    if (state->reply != NULL) {
        freeReplyObject(state->reply);
    }

    FREE_OBJ(state);
}

static void
make_thread_key()
{
    AZ(pthread_key_create(&thread_key, (void *) free_thread_state));
}

static const char *
get_reply(const struct vrt_ctx *ctx, redisReply *reply)
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
            result = WS_Copy(ctx->ws, reply->str, -1);
            AN(result);
            break;

        case REDIS_REPLY_INTEGER:
            sprintf(buffer, "%lld", reply->integer);
            result = WS_Copy(ctx->ws, buffer, -1);
            AN(result);
            break;

        case REDIS_REPLY_ARRAY:
            result = WS_Copy(ctx->ws, "array", -1);
            AN(result);
            break;

        default:
            return NULL;
    }

    // Done!
    return result;
}

static const char *
sha1(const struct vrt_ctx *ctx, const char *script)
{
    // Hash.
    unsigned char buffer[20];
    SHA1_CTX sha1_ctx;
    SHA1Init(&sha1_ctx);
    SHA1Update(&sha1_ctx, script, strlen(script));
    SHA1Final(buffer, &sha1_ctx);

    // Encode.
    char *result = WS_Alloc(ctx->ws, 41);
    AN(result);
    char *ptr = result;
    for (int i = 0; i < 20; i++) {
        sprintf(ptr, "%02x", buffer[i]);
        ptr += 2;
    }

    // Done!
    return result;
}
