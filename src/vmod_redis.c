#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vrt.h"
#include "bin/varnishd/cache.h"
#include "vcc_if.h"

#include "sha1.h"

#define DEFAULT_TAG "main"
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 6379
#define DEFAULT_TIMEOUT 500
#define DEFAULT_TTL 0

static unsigned version = 0;

typedef struct redis_server {
    // Tag (i.e. 'main', 'master', 'slave', etc.).
    const char *tag;

    // Host & port.
    const char *host;
    unsigned port;

    // Context timeout & TTL.
    struct timeval timeout;
    unsigned ttl;
} redis_server_t;

typedef struct vcl_priv {
#define VCL_PRIV_MAGIC 0x77feec11
    unsigned magic;
    // Redis servers.
#define MAX_REDIS_SERVERS 32
    unsigned nservers;
    redis_server_t servers[MAX_REDIS_SERVERS];

    // Max Redis contexts (i.e. redis_context_t) to be created.
    unsigned max_contexts;
} vcl_priv_t;

typedef struct redis_context {
    redis_server_t *server;
    redisContext *context;
    unsigned version;
    time_t tst;
} redis_context_t;

typedef struct thread_state {
#define THREAD_STATE_MAGIC 0xa6bc103e
    unsigned magic;
    // Redis contexts.
#define MAX_REDIS_CONTEXTS 8
    redis_context_t contexts[MAX_REDIS_CONTEXTS];

    // Redis command:
    //   - Tag (allocated in the session workspace).
    //   - Arguments (allocated in the session workspace).
    //   - Reply (allocated in the heap).
#define MAX_REDIS_COMMAND_ARGS 128
    const char *tag;
    unsigned argc;
    const char *argv[MAX_REDIS_COMMAND_ARGS];
    redisReply *reply;
} thread_state_t;

#define REDIS_LOG(sp, message, ...) \
    do { \
        char _buffer[512]; \
        snprintf( \
            _buffer, sizeof(_buffer), \
            "[REDIS][%s] %s", __func__, message); \
        WSP(sp, SLT_Error, _buffer, ##__VA_ARGS__); \
    } while (0)

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static vcl_priv_t * new_vcl_priv();
static void free_vcl_priv(vcl_priv_t *priv);

static thread_state_t *get_thread_state(struct sess *sp, unsigned drop_reply);
static void make_thread_key();

static redisContext *get_context(struct sess *sp, struct vmod_priv *vcl_priv, thread_state_t *state);

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
        vcl_priv->priv = new_vcl_priv(
            DEFAULT_TAG,
            DEFAULT_HOST, DEFAULT_PORT, DEFAULT_TIMEOUT, DEFAULT_TTL);
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
    const char *tag, const char *host, int port, int timeout, int ttl)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (host != NULL) && (strlen(host) > 0)) {
        // Set new configuration.
        vcl_priv_t *old = vcl_priv->priv;
        vcl_priv->priv = new_vcl_priv(tag, host, port, timeout, ttl);

        // Release previous configuration.
        if (old != NULL) {
            free_vcl_priv(old);
        }
    }
}

/******************************************************************************
 * redis.add_server();
 *****************************************************************************/

void
vmod_add_server(
    struct sess *sp, struct vmod_priv *vcl_priv,
    const char *tag, const char *host, int port, int timeout, int ttl)
{
    // Check input.
    if ((tag != NULL) && (strlen(tag) > 0) &&
        (host != NULL) && (strlen(host) > 0)) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;

        // Update configuration.
        if (config->nservers < MAX_REDIS_SERVERS) {
            AZ(pthread_mutex_lock(&mutex));

            redis_server_t *server = &(config->servers[config->nservers]);
            server->tag = strdup(tag);
            AN(server->tag);
            server->host = strdup(host);
            AN(server->host);
            server->port = port;
            server->timeout.tv_sec = timeout / 1000;
            server->timeout.tv_usec = (timeout % 1000) * 1000;
            server->ttl = ttl;

            config->nservers++;

            AZ(pthread_mutex_unlock(&mutex));
        } else {
            REDIS_LOG(sp,
                "Failed to add new server (limit is %d)",
                MAX_REDIS_SERVERS);
        }
    }
}

/******************************************************************************
 * redis.set_max_connections();
 *****************************************************************************/

void
vmod_set_max_connections(
    struct sess *sp, struct vmod_priv *vcl_priv, int limit)
{
    // Check input.
    if (limit > 0) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;

        // Update configuration.
        if (limit <= MAX_REDIS_CONTEXTS) {
            AZ(pthread_mutex_lock(&mutex));
            config->max_contexts = limit;
            AZ(pthread_mutex_unlock(&mutex));
        } else {
            REDIS_LOG(sp,
                "Failed to set the maximum number of connections per worker thread (limit is %d)",
                MAX_REDIS_CONTEXTS);
        }
    }
}

/******************************************************************************
 * redis.call();
 *****************************************************************************/

void
vmod_call(struct sess *sp, struct vmod_priv *vcl_priv, const char *command, ...)
{
    // Check input.
    if (command != NULL) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(sp, 1);

        // Fetch Redis context.
        // XXX: redis.call() does not allow selecting the destination sever.
        redisContext *context = get_context(sp, vcl_priv, state);

        // Do not continue if a Redis context is not available.
        if (context != NULL) {
            // Send command.
            // XXX: beware of the ugly hack to support usage of '%s'
            // placeholders in VCL.
            // XXX: redis.call() does not allow optimistic execution of EVALSHA
            // commands.
            va_list args;
            va_start(args, command);
            state->reply = redisvCommand(context, command, args);
            va_end(args);

            // Check reply.
            if (context->err) {
                REDIS_LOG(sp,
                    "Failed to execute Redis command (%s): [%d] %s",
                    command,
                    context->err,
                    context->errstr);
            } else if (state->reply == NULL) {
                REDIS_LOG(sp,
                    "Failed to execute Redis command (%s)",
                    command);
            } else if (state->reply->type == REDIS_REPLY_ERROR) {
                REDIS_LOG(sp,
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
            state->tag = tag;
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
            state->argv[state->argc++] = arg;
        } else {
            state->argv[state->argc++] = WS_Dup(sp->ws, "");
            AN(state->argv[state->argc - 1]);
        }
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

    // Fetch Redis context.
    redisContext *context = get_context(sp, vcl_priv, state);

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
                context,
                state->argc,
                state->argv,
                NULL);

            // Check reply. If Redis replies with a NOSCRIPT, the original
            // EVAL command should be executed to register the script for
            // the first time in the Redis server.
            if (!context->err &&
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
                context,
                state->argc,
                state->argv,
                NULL);
        }

        // Check reply.
        if (context->err) {
            REDIS_LOG(sp,
                "Failed to execute Redis command (%s): [%d] %s",
                state->argv[0],
                context->err,
                context->errstr);
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
        return WS_Dup(sp->wrk->ws, state->reply->str); \
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

static vcl_priv_t *
new_vcl_priv(
    const char *tag,
    const char *host, unsigned port, unsigned timeout, unsigned ttl)
{
    vcl_priv_t *result;
    ALLOC_OBJ(result, VCL_PRIV_MAGIC);
    AN(result);

    redis_server_t *server = &(result->servers[0]);
    server->tag = strdup(tag);
    AN(server->tag);
    server->host = strdup(host);
    AN(server->host);
    server->port = port;
    server->timeout.tv_sec = timeout / 1000;
    server->timeout.tv_usec = (timeout % 1000) * 1000;
    server->ttl = ttl;

    result->nservers = 1;

    result->max_contexts = 1;

    return result;
}

static void
free_vcl_priv(vcl_priv_t *priv)
{
    while (priv->nservers > 0) {
        priv->nservers--;
        free((void *) priv->servers[priv->nservers].tag);
        free((void *) priv->servers[priv->nservers].host);
    }

    FREE_OBJ(priv);
}

static thread_state_t *
get_thread_state(struct sess *sp, unsigned flush)
{
    // Initializations.
    thread_state_t *result = pthread_getspecific(thread_key);
    time_t now = time(NULL);

    // Create thread state if not created yet.
    if (result == NULL) {
        ALLOC_OBJ(result, THREAD_STATE_MAGIC);
        AN(result);

        for (int i = 0; i < MAX_REDIS_CONTEXTS; i++) {
            result->contexts[i].server = NULL;
            result->contexts[i].context = NULL;
            result->contexts[i].version = 0;
            result->contexts[i].tst = 0;
        }
        result->tag = NULL;
        result->argc = 0;
        result->reply = NULL;

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
free_thread_state(thread_state_t *state)
{
    for (int i = 0; i < MAX_REDIS_CONTEXTS; i++) {
        if ((state->contexts[i].server != NULL) &&
            (state->contexts[i].context != NULL)) {
            redisFree(state->contexts[i].context);
        }
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

static redisContext *
get_context(struct sess *sp, struct vmod_priv *vcl_priv, thread_state_t *state)
{
    int i, j;

    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;
    time_t now = time(NULL);

    // Randomly select an existing context matching the requested tag.
    redis_context_t *context = NULL;
    i = rand() % config->max_contexts;
    for (j = 0; j < config->max_contexts; j++) {
        if ((state->contexts[i].server != NULL) &&
            (state->contexts[i].context != NULL)) {
            if ((state->tag == NULL) ||
                (strcmp(state->tag, state->contexts[i].server->tag) == 0)) {
                context = &(state->contexts[i]);
                break;
            }
        }
        i = (i + 1) % config->max_contexts;
    }

    // Discard previously selected context if it's in an error state or it's
    // too old.
    if ((context != NULL) &&
        ((context->context->err) ||
         (context->version != version) ||
         (context->server->ttl > 0) && (now - context->tst > context->server->ttl))) {
        // Release context.
        context->server = NULL;
        redisFree(context->context);
        context->context = NULL;
        context->version = 0;
        context->tst = 0;

        // A new context needs to be created.
        context = NULL;
    }

    // If required, create new context using a randomly selected server matching
    // the requested tag. If any error arises discard the context and continue.
    if (context == NULL) {
        // Select random server matching the requested tag.
        redis_server_t *server = NULL;
        i = rand() % config->nservers;
        for (j = 0; j < config->nservers; j++) {
            if ((state->tag == NULL) ||
                (strcmp(state->tag, config->servers[i].tag) == 0)) {
                server = &(config->servers[i]);
            }
            i = (i + 1) % config->max_contexts;
        }

        // Do not continue if a server was not found.
        if (server != NULL) {
            // Fetch an empty context slot.
            for (i = 0; i < config->max_contexts; i++) {
                if ((state->contexts[i].server == NULL) &&
                    (state->contexts[i].context == NULL)) {
                    context = &(state->contexts[i]);
                    break;
                }
            }

            // If an empty slot was not available, randomly select an existing
            // context and release it.
            if (context == NULL) {
                context = &(state->contexts[rand() % config->max_contexts]);
                context->server = NULL;
                redisFree(context->context);
                context->context = NULL;
                context->version = 0;
                context->tst = 0;
            }

            // Create new context using the previously selected server. If any
            // error arises discard the context and continue.
            context->context = redisConnectWithTimeout(
                server->host,
                server->port,
                server->timeout);
            AN(context->context);
            if (context->context->err) {
                REDIS_LOG(sp,
                    "Failed to establish Redis connection (%d): %s",
                    context->context->err,
                    context->context->errstr);
                redisFree(context->context);
                context->context = NULL;
            } else {
                context->server = server;
                context->version = version;
                context->tst = now;
            }
        } else {
            REDIS_LOG(sp, "The requested server does not exist: %s", state->tag);
        }
    }

    // Done!
    if (context != NULL)
        return context->context;
    else {
        return NULL;
    }
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
