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

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static thread_state_t *get_thread_state(VRT_CTX, unsigned flush);
static void flush_thread_state(thread_state_t *state);

static const char *get_reply(VRT_CTX, redisReply *reply);

/******************************************************************************
 * VMOD INITIALIZATION.
 *****************************************************************************/

static void
make_thread_key()
{
    AZ(pthread_key_create(&thread_key, (void *) free_thread_state));
}

int
init_function(struct vmod_priv *vcl_priv, const struct VCL_conf *conf)
{
    // Initialize (once) the key required to store thread-specific data.
    AZ(pthread_once(&thread_once, make_thread_key));

    // Increase global version
    AZ(pthread_mutex_lock(&vmod_state.mutex));
    vmod_state.version++;
    AZ(pthread_mutex_unlock(&vmod_state.mutex));

    // Initialize configuration in the local VCL data structure.
    if (vcl_priv->priv == NULL) {
        vcl_priv->priv = new_vcl_state();
        vcl_priv->free = (vmod_priv_free_f *)free_vcl_state;
    }

    // Done!
    return 0;
}

/******************************************************************************
 * DB OBJECT.
 *****************************************************************************/

VCL_VOID
vmod_db__init(
    VRT_CTX, struct vmod_redis_db **db, const char *vcl_name,
    VCL_STRING location, VCL_INT connection_timeout, VCL_INT connection_ttl,
    VCL_INT command_timeout, VCL_INT max_command_retries, VCL_BOOL shared_connections,
    VCL_INT max_connections, VCL_STRING password, VCL_BOOL clustered,
    VCL_INT max_cluster_hops)
{
    // Assert input.
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(db);
    AZ(*db);

    // Check input.
    if ((location != NULL) && (strlen(location) > 0) &&
        (connection_timeout >= 0) &&
        (connection_ttl >= 0) &&
        (command_timeout >= 0) &&
        (max_command_retries >= 0) &&
        (max_connections >= 0) &&
        (password != NULL) &&
        (max_cluster_hops >= 0)) {
        // Initializations.
        struct timeval connection_timeout_tv;
        connection_timeout_tv.tv_sec = connection_timeout / 1000;
        connection_timeout_tv.tv_usec = (connection_timeout % 1000) * 1000;
        struct timeval command_timeout_tv;
        command_timeout_tv.tv_sec = command_timeout / 1000;
        command_timeout_tv.tv_usec = (command_timeout % 1000) * 1000;

        // Create new database instance.
        struct vmod_redis_db *instance = new_vmod_redis_db(
            vcl_name, connection_timeout_tv, connection_ttl, command_timeout_tv,
            max_command_retries, shared_connections, max_connections,
            password, clustered, max_cluster_hops);

        // Add initial server.
        AZ(pthread_mutex_lock(&instance->mutex));
        redis_server_t *server = unsafe_add_redis_server(ctx, instance, location);
        AZ(pthread_mutex_unlock(&instance->mutex));

        // Do not continue if we failed to create the server instance.
        if (server != NULL) {
            // Launch initial discovery of the cluster topology?
            if (instance->cluster.enabled) {
                discover_cluster_slots(ctx, instance, server);
            }

            // Return the new database instance.
            *db = instance;

            // Log event.
            REDIS_LOG_INFO(ctx,
                "New database instance registered (db=%s)",
                instance->name);
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

    // Log event.
    REDIS_LOG_INFO(NULL,
        "Unregistering database instance (db=%s)",
        (*db)->name);

    // Unregister database instance.
    free_vmod_redis_db(*db);
    *db = NULL;
}

/******************************************************************************
 * .add_server();
 *****************************************************************************/

VCL_VOID
vmod_db_add_server(VRT_CTX, struct vmod_redis_db *db, VCL_STRING location)
{
    if ((location != NULL) && (strlen(location) > 0)) {
        AZ(pthread_mutex_lock(&db->mutex));
        unsafe_add_redis_server(ctx, db, location);
        AZ(pthread_mutex_unlock(&db->mutex));
    }
}

/******************************************************************************
 * .command();
 *****************************************************************************/

VCL_VOID
vmod_db_command(VRT_CTX, struct vmod_redis_db *db, VCL_STRING name)
{
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(ctx, 1);

        // Initialize.
        state->command.db = db;
        state->command.timeout = db->command_timeout;
        state->command.max_retries = db->max_command_retries;
        state->command.argc = 1;
        state->command.argv[0] = WS_Copy(ctx->ws, name, -1);
        AN(state->command.argv[0]);
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
vmod_db_retries(VRT_CTX, struct vmod_redis_db *db, VCL_INT max_command_retries)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the initial call to .command() was not executed
    // or if running this in a different database.
    if ((state->command.argc >= 1) && (state->command.db == db)) {
        state->command.max_retries = max_command_retries;
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
        AN(state->command.argv[state->command.argc - 1]);
    } else {
        REDIS_LOG_ERROR(ctx,
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

    // Do not continue if the initial call to .command() was not executed
    // or if running this in a different database.
    if ((state->command.argc >= 1) && (state->command.db == db)) {
        // Clustered vs. standalone execution.
        if (db->cluster.enabled) {
            state->command.reply = cluster_execute(
                ctx, db, state,
                state->command.timeout, state->command.max_retries,
                state->command.argc, state->command.argv);
        } else {
            int tries = 1 + state->command.max_retries;
            while ((tries > 0) && (state->command.reply == NULL)) {
                state->command.reply = redis_execute(
                    ctx, db, state, NULL,
                    state->command.timeout,
                    state->command.argc, state->command.argv, 0);
                tries--;
            }

            if (tries < state->command.max_retries) {
                AZ(pthread_mutex_lock(&db->mutex));
                db->stats.commands.retried += state->command.max_retries - tries;
                AZ(pthread_mutex_unlock(&db->mutex));
            }
        }

        // Log error replies (other errors are already logged while executing
        // commands).
        if ((state->command.reply != NULL) &&
            (state->command.reply->type == REDIS_REPLY_ERROR)) {
            REDIS_LOG_ERROR(ctx,
                "Got error reply while executing command (command=%s, db=%s): %s",
                state->command.argv[0], db->name, state->command.reply->str);

            AZ(pthread_mutex_lock(&db->mutex));
            db->stats.commands.error++;
            AZ(pthread_mutex_unlock(&db->mutex));
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

#define VMOD_DB_REPLY_IS_FOO(lower, upper) \
VCL_BOOL \
vmod_db_reply_is_ ## lower(VRT_CTX, struct vmod_redis_db *db) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
    return \
        (state->command.db == db) && \
        (state->command.reply != NULL) && \
        (state->command.reply->type == REDIS_REPLY_ ## upper); \
}

VMOD_DB_REPLY_IS_FOO(error, ERROR)
VMOD_DB_REPLY_IS_FOO(nil, NIL)
VMOD_DB_REPLY_IS_FOO(status, STATUS)
VMOD_DB_REPLY_IS_FOO(integer, INTEGER)
VMOD_DB_REPLY_IS_FOO(string, STRING)
VMOD_DB_REPLY_IS_FOO(array, ARRAY)

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

#define VMOD_DB_GET_FOO_REPLY(lower, upper) \
VCL_STRING \
vmod_db_get_ ## lower ## _reply(VRT_CTX, struct vmod_redis_db *db) \
{ \
    thread_state_t *state = get_thread_state(ctx, 0); \
    if ((state->command.db == db) && \
        (state->command.reply != NULL) && \
        (state->command.reply->type == REDIS_REPLY_ ## upper)) { \
        char *result = WS_Copy(ctx->ws, state->command.reply->str, state->command.reply->len + 1); \
        AN(result); \
        return result; \
    } else { \
        return NULL; \
    } \
}

VMOD_DB_GET_FOO_REPLY(error, ERROR)
VMOD_DB_GET_FOO_REPLY(status, STATUS)
VMOD_DB_GET_FOO_REPLY(string, STRING)

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

#define VMOD_DB_ARRAY_REPLY_IS_FOO(lower, upper) \
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

VMOD_DB_ARRAY_REPLY_IS_FOO(error, ERROR)
VMOD_DB_ARRAY_REPLY_IS_FOO(nil, NIL)
VMOD_DB_ARRAY_REPLY_IS_FOO(status, STATUS)
VMOD_DB_ARRAY_REPLY_IS_FOO(integer, INTEGER)
VMOD_DB_ARRAY_REPLY_IS_FOO(string, STRING)
VMOD_DB_ARRAY_REPLY_IS_FOO(array, ARRAY)

/******************************************************************************
 * .get_array_reply_value();
 *****************************************************************************/

VCL_STRING
vmod_db_get_array_reply_value(
    VRT_CTX, struct vmod_redis_db *db, VCL_INT index)
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
 * .stats();
 *****************************************************************************/

VCL_STRING
vmod_db_stats(VRT_CTX, struct vmod_redis_db *db)
{
    AZ(pthread_mutex_lock(&db->mutex));
    char *result = WS_Printf(ctx->ws,
        "{"
          "\"servers\": {"
            "\"total\": %d,"
            "\"failed\": %d"
          "},"
          "\"connections\": {"
            "\"total\": %d,"
            "\"failed\": %d,"
            "\"dropped\": {"
              "\"error\": %d,"
              "\"hung_up\": %d,"
              "\"overflow\": %d,"
              "\"ttl\": %d,"
              "\"version\": %d"
            "}"
          "},"
          "\"workers\": {"
              "\"blocked\": %d"
          "},"
          "\"commands\": {"
            "\"total\": %d,"
            "\"failed\": %d,"
            "\"retried\": %d,"
            "\"error\": %d,"
            "\"noscript\": %d"
          "},"
          "\"cluster\": {"
            "\"discoveries\": {"
              "\"total\": %d,"
              "\"failed\": %d"
            "},"
            "\"replies\": {"
              "\"moved\": %d,"
              "\"ask\": %d"
            "}"
          "}"
        "}",
        db->stats.servers.total,
        db->stats.servers.failed,
        db->stats.connections.total,
        db->stats.connections.failed,
        db->stats.connections.dropped.error,
        db->stats.connections.dropped.hung_up,
        db->stats.connections.dropped.overflow,
        db->stats.connections.dropped.ttl,
        db->stats.connections.dropped.version,
        db->stats.workers.blocked,
        db->stats.commands.total,
        db->stats.commands.failed,
        db->stats.commands.retried,
        db->stats.commands.error,
        db->stats.commands.noscript,
        db->stats.cluster.discoveries.total,
        db->stats.cluster.discoveries.failed,
        db->stats.cluster.replies.moved,
        db->stats.cluster.replies.ask);
    AZ(pthread_mutex_unlock(&db->mutex));
    return result;
}

/******************************************************************************
 * .counter();
 *****************************************************************************/

VCL_INT
vmod_db_counter(VRT_CTX, struct vmod_redis_db *db, VCL_STRING name)
{
    if (strcmp(name, "servers.total") == 0) {
        return db->stats.servers.total;
    } else if (strcmp(name, "servers.failed") == 0) {
        return db->stats.servers.failed;
    } else if (strcmp(name, "connections.total") == 0) {
        return db->stats.connections.total;
    } else if (strcmp(name, "connections.failed") == 0) {
        return db->stats.connections.failed;
    } else if (strcmp(name, "connections.dropped.error") == 0) {
        return db->stats.connections.dropped.error;
    } else if (strcmp(name, "connections.dropped.hung_up") == 0) {
        return db->stats.connections.dropped.hung_up;
    } else if (strcmp(name, "connections.dropped.overflow") == 0) {
        return db->stats.connections.dropped.overflow;
    } else if (strcmp(name, "connections.dropped.ttl") == 0) {
        return db->stats.connections.dropped.ttl;
    } else if (strcmp(name, "connections.dropped.version") == 0) {
        return db->stats.connections.dropped.version;
    } else if (strcmp(name, "workers.blocked") == 0) {
        return db->stats.workers.blocked;
    } else if (strcmp(name, "commands.total") == 0) {
        return db->stats.commands.total;
    } else if (strcmp(name, "commands.failed") == 0) {
        return db->stats.commands.failed;
    } else if (strcmp(name, "commands.retried") == 0) {
        return db->stats.commands.retried;
    } else if (strcmp(name, "commands.error") == 0) {
        return db->stats.commands.error;
    } else if (strcmp(name, "commands.noscript") == 0) {
        return db->stats.commands.noscript;
    } else if (strcmp(name, "cluster.discoveries.total") == 0) {
        return db->stats.cluster.discoveries.total;
    } else if (strcmp(name, "cluster.discoveries.failed") == 0) {
        return db->stats.cluster.discoveries.failed;
    } else if (strcmp(name, "cluster.replies.moved") == 0) {
        return db->stats.cluster.replies.moved;
    } else if (strcmp(name, "cluster.replies.ask") == 0) {
        return db->stats.cluster.replies.ask;
    } else {
        REDIS_LOG_ERROR(ctx,
            "Failed to fetch counter '%s'",
            name);
        return 0;
    }
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
    state->command.max_retries = 0;
    state->command.argc = 0;
    if (state->command.reply != NULL) {
        freeReplyObject(state->command.reply);
        state->command.reply = NULL;
    }
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
