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

static unsigned version = 0;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static thread_state_t *get_thread_state(VRT_CTX, unsigned flush);
static void flush_thread_state(thread_state_t *state);
static void make_thread_key();

static redis_server_t *unsafe_add_redis_server(
    VRT_CTX, struct vmod_redis_db *db, const char *location);

static const char *get_reply(VRT_CTX, redisReply *reply);

static void handle_vcl_cold_event(VRT_CTX, vcl_priv_t *config);

/******************************************************************************
 * VMOD INITIALIZATION.
 *****************************************************************************/

int
event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;

    // Check event.
    switch (e) {
        case VCL_EVENT_LOAD:
            // Initialize (once) the key required to store thread-specific data.
            AZ(pthread_once(&thread_once, make_thread_key));

            // Initialize the local VCL data structure and set its free function.
            // Code initializing / freeing the VCL private data structure *is
            // not required* to be thread safe.
            vcl_priv->priv = new_vcl_priv();
            vcl_priv->free = (vmod_priv_free_f *)free_vcl_priv;
            break;

        case VCL_EVENT_WARM:
            // Increase the global version. This will be used to (1) reestablish
            // Redis connections binded to worker threads; and (2) regenerate
            // pooled connections shared between threads.
            AZ(pthread_mutex_lock(&mutex));
            version++;
            AZ(pthread_mutex_unlock(&mutex));
            break;

        case VCL_EVENT_COLD:
            // Close connections in shared pools.
            handle_vcl_cold_event(ctx, config);
            break;

        default:
            break;
    }

    // Done!
    return 0;
}

/******************************************************************************
 * DB OBJECT.
 *****************************************************************************/

VCL_VOID
vmod_db__init(
    VRT_CTX, struct vmod_redis_db **db, const char *vcl_name, struct vmod_priv *vcl_priv,
    VCL_STRING location, VCL_INT connection_timeout, VCL_INT context_ttl,
    VCL_INT command_timeout, VCL_INT command_retries, VCL_BOOL shared_contexts,
    VCL_INT max_contexts, VCL_BOOL clustered, VCL_INT max_cluster_hops)
{
    // Assert input.
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(db);
    AZ(*db);

    // Check input.
    if ((location != NULL) && (strlen(location) > 0) &&
        (max_contexts > 0)) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;
        struct timeval connection_timeout_tv;
        connection_timeout_tv.tv_sec = connection_timeout / 1000;
        connection_timeout_tv.tv_usec = (connection_timeout % 1000) * 1000;
        struct timeval command_timeout_tv;
        command_timeout_tv.tv_sec = command_timeout / 1000;
        command_timeout_tv.tv_usec = (command_timeout % 1000) * 1000;

        // Create new database instance.
        struct vmod_redis_db *instance = new_vmod_redis_db(
            connection_timeout_tv, context_ttl, command_timeout_tv,
            command_retries, shared_contexts, max_contexts,
            clustered, max_cluster_hops);

        // Add initial server.
        redis_server_t *server = unsafe_add_redis_server(ctx, instance, location);

        // Do not continue if we failed to create the server instance.
        if (server != NULL) {
            // Populate the slots-tags mapping.
            if (instance->cluster.enabled) {
                discover_cluster_slots(ctx, instance);
            }

            // Register & return the new database instance.
            // XXX: is there a better way to keep track of database instances?
            vcl_priv_db_t *vcl_priv_db = new_vcl_priv_db(instance);
            VTAILQ_INSERT_TAIL(&config->dbs, vcl_priv_db, list);
            *db = instance;
        } else {
            free_vmod_redis_db(instance);
            *db = NULL;
        }
    }
}

VCL_VOID
vmod_db__fini(struct vmod_redis_db **db)
{
    // Assert input.
    AN(db);
    AN(*db);

    // Release database instance.
    free_vmod_redis_db(*db);
    *db = NULL;
}

/******************************************************************************
 * .add_server();
 *****************************************************************************/

VCL_VOID
vmod_db_add_server(VRT_CTX, struct vmod_redis_db *db, VCL_STRING location)
{
    // Check input.
    if ((location != NULL) && (strlen(location) > 0)) {
        // Get database lock.
        AZ(pthread_mutex_lock(&db->mutex));

        // Add new server.
        unsafe_add_redis_server(ctx, db, location);

        // Release database lock.
        AZ(pthread_mutex_unlock(&db->mutex));
    }
}

/******************************************************************************
 * .command();
 *****************************************************************************/

VCL_VOID
vmod_db_command(VRT_CTX, struct vmod_redis_db *db, VCL_STRING name)
{
    // Check input.
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(ctx, 1);

        // Initialize.
        state->command.db = db;
        state->command.timeout = db->command_timeout;
        state->command.retries = db->command_retries;
        state->command.argc = 1;
        state->command.argv[0] = WS_Copy(ctx->ws, name, -1);
        if (state->command.argv[0] == NULL) {
            REDIS_LOG(ctx,
                "Failed to allocate memory in workspace (%p)",
                ctx->ws);
            flush_thread_state(state);
        }
    }
}

/******************************************************************************
 * .timeout();
 *****************************************************************************/

VCL_VOID
vmod_db_timeout(VRT_CTX, struct vmod_redis_db *db, VCL_INT command_timeout)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the initial call to .command() was not executed
    // or if running this in a different database.
    if ((state->command.argc >= 1) && (state->command.db == db)) {
        state->command.timeout.tv_sec = command_timeout / 1000;
        state->command.timeout.tv_usec = (command_timeout % 1000) * 1000;
    }
}

/******************************************************************************
 * .retries();
 *****************************************************************************/

VCL_VOID
vmod_db_retries(VRT_CTX, struct vmod_redis_db *db, VCL_INT command_retries)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the initial call to .command() was not executed
    // or if running this in a different database.
    if ((state->command.argc >= 1) && (state->command.db == db)) {
        state->command.retries = command_retries;
    }
}

/******************************************************************************
 * .push();
 *****************************************************************************/

VCL_VOID
vmod_db_push(VRT_CTX, struct vmod_redis_db *db, VCL_STRING arg)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to .command() was not executed or
    // if running this in a different database.
    if ((state->command.argc >= 1) &&
        (state->command.argc < MAX_REDIS_COMMAND_ARGS) &&
        (state->command.db == db)) {
        // Handle NULL arguments as empty strings.
        if (arg != NULL) {
            state->command.argv[state->command.argc++] = WS_Copy(ctx->ws, arg, -1);;
        } else {
            state->command.argv[state->command.argc++] = WS_Copy(ctx->ws, "", -1);
        }
        if (state->command.argv[state->command.argc - 1] == NULL) {
            REDIS_LOG(ctx,
                "Failed to allocate memory in workspace (%p)",
                ctx->ws);
            flush_thread_state(state);
        }
    } else {
        REDIS_LOG(ctx,
            "Failed to push Redis argument (limit is %d)",
            MAX_REDIS_COMMAND_ARGS);
    }
}

/******************************************************************************
 * .execute();
 *****************************************************************************/

VCL_VOID
vmod_db_execute(VRT_CTX, struct vmod_redis_db *db)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the initial call to redis.command() was not executed
    // or if running this in a different database or if the workspace is
    // already overflowed.
    if ((state->command.argc >= 1) &&
        (state->command.db == db) &&
        (!WS_Overflowed(ctx->ws))) {
        // Clustered vs. classic execution.
        if (db->cluster.enabled) {
            state->command.reply = cluster_execute(
                ctx, db, state, version,
                state->command.timeout, state->command.retries,
                state->command.argc, state->command.argv);
        } else {
            int tries = 1 + state->command.retries;
            while ((tries > 0) &&
                   (state->command.reply == NULL) &&
                   (!WS_Overflowed(ctx->ws))) {
                state->command.reply = redis_execute(
                    ctx, db, state, NULL, version,
                    state->command.timeout,
                    state->command.argc, state->command.argv, 0);
                tries--;
            }
        }

        // Log error replies (other errors are already logged while executing
        // commands).
        if ((state->command.reply != NULL) &&
            (state->command.reply->type == REDIS_REPLY_ERROR)) {
            REDIS_LOG(ctx,
                "Got error reply while executing Redis command (%s): %s",
                state->command.argv[0],
                state->command.reply->str);
        }
    }
}

/******************************************************************************
 * .replied();
 *****************************************************************************/

VCL_BOOL
vmod_db_replied(VRT_CTX, struct vmod_redis_db *db)
{
    thread_state_t *state = get_thread_state(ctx, 0);
    return (state->command.db == db) && (state->command.reply != NULL);
}

/******************************************************************************
 * .reply_is_error();
 * .reply_is_nil();
 * .reply_is_status();
 * .reply_is_integer();
 * .reply_is_string();
 * .reply_is_array();
 *****************************************************************************/

#define VMOD_STORAGE_REPLY_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_db_reply_is_ ## lower(VRT_CTX, struct vmod_redis_db *db) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
    return \
        (state->command.db == db) && \
        (state->command.reply != NULL) && \
        (state->command.reply->type == REDIS_REPLY_ ## upper); \
}

VMOD_STORAGE_REPLY_IS_FOO(error, ERROR)
VMOD_STORAGE_REPLY_IS_FOO(nil, NIL)
VMOD_STORAGE_REPLY_IS_FOO(status, STATUS)
VMOD_STORAGE_REPLY_IS_FOO(integer, INTEGER)
VMOD_STORAGE_REPLY_IS_FOO(string, STRING)
VMOD_STORAGE_REPLY_IS_FOO(array, ARRAY)

/******************************************************************************
 * .get_reply();
 *****************************************************************************/

VCL_STRING
vmod_db_get_reply(VRT_CTX, struct vmod_redis_db *db)
{
    thread_state_t *state = get_thread_state(ctx, 0);
    if ((state->command.db == db) &&
        (state->command.reply != NULL)) {
        return get_reply(ctx, state->command.reply);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * .get_error_reply();
 * .get_status_reply();
 * .get_integer_reply();
 * .get_string_reply();
 *****************************************************************************/

VCL_INT
vmod_db_get_integer_reply(VRT_CTX, struct vmod_redis_db *db)
{
    thread_state_t *state = get_thread_state(ctx, 0);
    if ((state->command.db == db) &&
        (state->command.reply != NULL) &&
        (state->command.reply->type == REDIS_REPLY_INTEGER)) {
        return state->command.reply->integer;
    } else {
        return 0;
    }
}

#define VMOD_STORAGE_GET_FOO_REPLY(lower, upper) \
VCL_STRING \
vmod_db_get_ ## lower ## _reply(VRT_CTX, struct vmod_redis_db *db) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
    if ((state->command.db == db) && \
        (state->command.reply != NULL) && \
        (state->command.reply->type == REDIS_REPLY_ ## upper)) { \
        char *result = WS_Copy(ctx->ws, state->command.reply->str, state->command.reply->len + 1); \
        if (result == NULL) { \
            REDIS_LOG(ctx, \
                "Failed to allocate memory in workspace (%p)", \
                ctx->ws); \
        } \
        return result; \
    } else { \
        return NULL; \
    } \
}

VMOD_STORAGE_GET_FOO_REPLY(error, ERROR)
VMOD_STORAGE_GET_FOO_REPLY(status, STATUS)
VMOD_STORAGE_GET_FOO_REPLY(string, STRING)

/******************************************************************************
 * .get_array_reply_length();
 *****************************************************************************/

VCL_INT
vmod_db_get_array_reply_length(VRT_CTX, struct vmod_redis_db *db)
{
    thread_state_t *state = get_thread_state(ctx, 0);
    if ((state->command.db == db) &&
        (state->command.reply != NULL) &&
        (state->command.reply->type == REDIS_REPLY_ARRAY)) {
        return state->command.reply->elements;
    } else {
        return 0;
    }
}

/******************************************************************************
 * .array_reply_is_error();
 * .array_reply_is_nil();
 * .array_reply_is_status();
 * .array_reply_is_integer();
 * .array_reply_is_string();
 * .array_reply_is_array();
 *****************************************************************************/

#define VMOD_STORAGE_ARRAY_REPLY_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_db_array_reply_is_ ## lower(VRT_CTX, struct vmod_redis_db *db, VCL_INT index) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
    return \
        (state->command.db == db) && \
        (state->command.reply != NULL) && \
        (state->command.reply->type == REDIS_REPLY_ARRAY) && \
        (index < state->command.reply->elements) && \
        (state->command.reply->element[index]->type == REDIS_REPLY_ ## upper); \
}

VMOD_STORAGE_ARRAY_REPLY_IS_FOO(error, ERROR)
VMOD_STORAGE_ARRAY_REPLY_IS_FOO(nil, NIL)
VMOD_STORAGE_ARRAY_REPLY_IS_FOO(status, STATUS)
VMOD_STORAGE_ARRAY_REPLY_IS_FOO(integer, INTEGER)
VMOD_STORAGE_ARRAY_REPLY_IS_FOO(string, STRING)
VMOD_STORAGE_ARRAY_REPLY_IS_FOO(array, ARRAY)

/******************************************************************************
 * .get_array_reply_value();
 *****************************************************************************/

VCL_STRING
vmod_db_get_array_reply_value(VRT_CTX, struct vmod_redis_db *db, VCL_INT index)
{
    thread_state_t *state = get_thread_state(ctx, 0);
    if ((state->command.db == db) &&
        (state->command.reply != NULL) &&
        (state->command.reply->type == REDIS_REPLY_ARRAY) &&
        (index < state->command.reply->elements)) {
        return get_reply(ctx, state->command.reply->element[index]);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * .free();
 *****************************************************************************/

VCL_VOID
vmod_db_free(VRT_CTX, struct vmod_redis_db *db)
{
    get_thread_state(ctx, 1);
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static thread_state_t *
get_thread_state(VRT_CTX, unsigned flush)
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

    // Flush enqueued command?
    if (flush) {
        flush_thread_state(result);
    }

    // Done!
    return result;
}

static void
flush_thread_state(thread_state_t *state)
{
    state->command.db = NULL;
    state->command.timeout = (struct timeval){ 0 };
    state->command.retries = 0;
    state->command.argc = 0;
    if (state->command.reply != NULL) {
        freeReplyObject(state->command.reply);
        state->command.reply = NULL;
    }
}

static void
make_thread_key()
{
    AZ(pthread_key_create(&thread_key, (void *) free_thread_state));
}

static redis_server_t *
unsafe_add_redis_server(VRT_CTX, struct vmod_redis_db *db, const char *location)
{
    // Initializations.
    redis_server_t *result = new_redis_server(db, location);

    // Do not continue if we failed to create the server instance.
    // Caller should own db->mutex!
    if (result != NULL) {
        // Add new server.
        VTAILQ_INSERT_TAIL(&db->servers, result, list);

        // If required, add new pool.
        if (db->shared_contexts) {
            if (unsafe_get_context_pool(db, result->tag) == NULL) {
                redis_context_pool_t *pool = new_redis_context_pool(result->tag);
                VTAILQ_INSERT_TAIL(&db->pools, pool, list);
            }
        }
    } else {
        REDIS_LOG(ctx,
            "Failed to add server '%s'",
            location);
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
            if (result == NULL) {
                REDIS_LOG(ctx,
                    "Failed to allocate memory in workspace (%p)",
                    ctx->ws);
            }
            break;

        case REDIS_REPLY_INTEGER:
            result = WS_Printf(ctx->ws, "%lld", reply->integer);
            if (result == NULL) {
                REDIS_LOG(ctx,
                    "Failed to allocate memory in workspace (%p)",
                    ctx->ws);
            }
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
handle_vcl_cold_event(VRT_CTX, vcl_priv_t *config)
{
    // Iterate through registered database instances and close connections in
    // shared pools.
    vcl_priv_db_t *idb;
    VTAILQ_FOREACH(idb, &config->dbs, list) {
        if ((idb != NULL) && (idb->magic == VCL_PRIV_DB_MAGIC)) {
            // Get database lock.
            AZ(pthread_mutex_lock(&idb->db->mutex));

            // Release contexts in all pools.
            redis_context_pool_t *ipool;
            VTAILQ_FOREACH(ipool, &idb->db->pools, list) {
                // Get pool lock.
                AZ(pthread_mutex_lock(&ipool->mutex));

                // Release all contexts (both free an busy; this method is
                // assumed to be called when threads are not using the pool).
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
                AZ(pthread_mutex_unlock(&ipool->mutex));
            }

            // Release database lock.
            AZ(pthread_mutex_unlock(&idb->db->mutex));
        }
    }
}
