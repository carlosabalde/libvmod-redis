#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>
#include <arpa/inet.h>

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vcc_if.h"

#include "cluster.h"
#include "core.h"
#include "sentinel.h"

static unsigned version = 0;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static thread_state_t *get_thread_state(VRT_CTX, unsigned flush);
static void flush_thread_state(thread_state_t *state);
static void make_thread_key();

static void unsafe_set_subnets(VRT_CTX, vcl_priv_t *config, const char *masks);

static const char *get_reply(VRT_CTX, redisReply *reply);

static void handle_vcl_warm_event(VRT_CTX, vcl_priv_t *config);
static void handle_vcl_cold_event(VRT_CTX, vcl_priv_t *config);

/******************************************************************************
 * VMOD INITIALIZATION.
 *****************************************************************************/

int
event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    // Log event.
    const char *name;
    switch (e) {
        case VCL_EVENT_LOAD: name = "load"; break;
        case VCL_EVENT_WARM: name = "warm"; break;
        case VCL_EVENT_USE: name = "use"; break;
        case VCL_EVENT_COLD: name = "cold"; break;
        case VCL_EVENT_DISCARD: name = "discard"; break;
        default: name = "-";
    }
    REDIS_LOG_INFO(ctx,
        "VCL event triggered (event=%s)",
        name);

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
            AN(vcl_priv->priv);
            handle_vcl_warm_event(ctx, vcl_priv->priv);
            break;

        case VCL_EVENT_COLD:
            AN(vcl_priv->priv);
            handle_vcl_cold_event(ctx, vcl_priv->priv);
            break;

        default:
            break;
    }

    // Done!
    return 0;
}

/******************************************************************************
 * redis.subnets();
 *****************************************************************************/

VCL_VOID
vmod_subnets(VRT_CTX, struct vmod_priv *vcl_priv, VCL_STRING masks)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;

    // Get configuration lock.
    AZ(pthread_mutex_lock(&config->mutex));

    // Silently ignore calls to this function if any database instance has
    // already been registered.
    if (VTAILQ_EMPTY(&config->dbs)) {
        // Do not continue if subnets have already been set.
        if (VTAILQ_EMPTY(&config->subnets)) {
            const char *value = NULL;
            if ((masks != NULL) && (strlen(masks) > 0)) {
                value = masks;
            } else {
                value = getenv("VMOD_REDIS_SUBNETS");
            }
            if ((value != NULL) && (strlen(value) > 0)) {
                unsafe_set_subnets(ctx, config, value);
            }
        } else {
            REDIS_LOG_ERROR(ctx,
                "Subnets already set (%s)",
                masks);
        }
    }

    // Release configuration lock.
    AZ(pthread_mutex_unlock(&config->mutex));
}

/******************************************************************************
 * redis.sentinels();
 *****************************************************************************/

VCL_VOID
vmod_sentinels(
    VRT_CTX, struct vmod_priv *vcl_priv, VCL_STRING locations, VCL_INT period,
    VCL_INT connection_timeout, VCL_INT command_timeout)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;

    // Get configuration lock.
    AZ(pthread_mutex_lock(&config->mutex));

    // Silently ignore calls to this function if any database instance has
    // already been registered.
    if (VTAILQ_EMPTY(&config->dbs)) {
        // Do not continue if Sentinels have already been set.
        if (config->sentinels.locations == NULL) {
            if ((period > 0) &&
                (connection_timeout >= 0) &&
                (command_timeout >= 0)) {
                // Store settings.
                const char *value = NULL;
                if ((locations != NULL) && (strlen(locations) > 0)) {
                    value = locations;
                } else {
                    value = getenv("VMOD_REDIS_SENTINELS");
                }
                if ((value != NULL) && (strlen(value) > 0)) {
                    config->sentinels.locations = strdup(value);
                    AN(config->sentinels.locations);
                    config->sentinels.period = period;
                    config->sentinels.connection_timeout.tv_sec =
                        connection_timeout / 1000;
                    config->sentinels.connection_timeout.tv_usec =
                        (connection_timeout % 1000) * 1000;
                    config->sentinels.command_timeout.tv_sec =
                        command_timeout / 1000;
                    config->sentinels.command_timeout.tv_usec =
                        (command_timeout % 1000) * 1000;
                }

                // Start Sentinel thread?
                if ((config->sentinels.locations != NULL) &&
                    (!config->sentinels.active)) {
                    unsafe_sentinel_start(config);
                }
            }
        } else {
            REDIS_LOG_ERROR(ctx,
                "Sentinels already set (%s)",
                locations);
        }
    }

    // Release configuration lock.
    AZ(pthread_mutex_unlock(&config->mutex));
}

/******************************************************************************
 * DB OBJECT.
 *****************************************************************************/

VCL_VOID
vmod_db__init(
    VRT_CTX, struct vmod_redis_db **db, const char *vcl_name, struct vmod_priv *vcl_priv,
    VCL_STRING location, VCL_ENUM type, VCL_INT connection_timeout, VCL_INT connection_ttl,
    VCL_INT command_timeout, VCL_INT max_command_retries, VCL_BOOL shared_connections,
    VCL_INT max_connections, VCL_STRING password, VCL_INT sickness_ttl,
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
        (sickness_ttl >= 0) &&
        (max_cluster_hops >= 0)) {
        // Initializations.
        vcl_priv_t *config = vcl_priv->priv;
        struct timeval connection_timeout_tv;
        connection_timeout_tv.tv_sec = connection_timeout / 1000;
        connection_timeout_tv.tv_usec = (connection_timeout % 1000) * 1000;
        struct timeval command_timeout_tv;
        command_timeout_tv.tv_sec = command_timeout / 1000;
        command_timeout_tv.tv_usec = (command_timeout % 1000) * 1000;

        // Extract role & clustering flag.
        unsigned clustered;
        enum REDIS_SERVER_ROLE role;
        if (strcmp(type, "master") == 0) {
            role = REDIS_SERVER_MASTER_ROLE;
            clustered = 0;
        } else if (strcmp(type, "slave") == 0) {
            role = REDIS_SERVER_SLAVE_ROLE;
            clustered = 0;
        } else if (strcmp(type, "auto") == 0) {
            role = REDIS_SERVER_TBD_ROLE;
            clustered = 0;
        } else if (strcmp(type, "cluster") == 0) {
            role = REDIS_SERVER_TBD_ROLE;
            clustered = 1;
        } else {
            WRONG("Invalid server type value.");
        }

        // Create new database instance.
        struct vmod_redis_db *instance = new_vmod_redis_db(
            config, vcl_name, connection_timeout_tv, connection_ttl,
            command_timeout_tv, max_command_retries, shared_connections, max_connections,
            password, sickness_ttl, clustered, max_cluster_hops);

        // Add initial server.
        redis_server_t *server = unsafe_add_redis_server(ctx, instance, location, role);

        // Do not continue if we failed to create the server instance.
        if (server != NULL) {
            // Launch initial discovery of the cluster topology?
            if (instance->cluster.enabled) {
                discover_cluster_slots(ctx, instance, server);
            }

            // Register & return the new database instance.
            vcl_priv_db_t *vcl_priv_db = new_vcl_priv_db(instance);
            AZ(pthread_mutex_lock(&config->mutex));
            VTAILQ_INSERT_TAIL(&config->dbs, vcl_priv_db, list);
            AZ(pthread_mutex_unlock(&config->mutex));
            *db = instance;

            // Log event.
            REDIS_LOG_INFO(ctx,
                "New database instance registered (name=%s)",
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
        "Unregistering database instance (name=%s)",
        (*db)->name);

    // Keep config reference before releasing the instance.
    vcl_priv_t *config = (*db)->config;

    // Unregister database instance.
    AZ(pthread_mutex_lock(&config->mutex));
    vcl_priv_db_t *idb;
    VTAILQ_FOREACH(idb, &config->dbs, list) {
        CHECK_OBJ_NOTNULL(idb, VCL_PRIV_DB_MAGIC);
        if (idb->db == *db) {
            VTAILQ_REMOVE(&config->dbs, idb, list);
            free_vcl_priv_db(idb);
            break;
        }
    }
    AZ(pthread_mutex_unlock(&config->mutex));
    *db = NULL;
}

/******************************************************************************
 * .add_server();
 *****************************************************************************/

VCL_VOID
vmod_db_add_server(
    VRT_CTX, struct vmod_redis_db *db, VCL_STRING location, VCL_ENUM type)
{
    if ((location != NULL) && (strlen(location) > 0) &&
        ((!db->cluster.enabled || strcmp(type, "cluster") == 0))) {
        // Extract role.
        enum REDIS_SERVER_ROLE role;
        if (strcmp(type, "master") == 0) {
            role = REDIS_SERVER_MASTER_ROLE;
        } else if (strcmp(type, "slave") == 0) {
            role = REDIS_SERVER_SLAVE_ROLE;
        } else if (strcmp(type, "auto") == 0) {
            role = REDIS_SERVER_TBD_ROLE;
        } else if (strcmp(type, "cluster") == 0) {
            role = REDIS_SERVER_TBD_ROLE;
        } else {
            WRONG("Invalid server type value.");
        }

        // Add server.
        AZ(pthread_mutex_lock(&db->mutex));
        unsafe_add_redis_server(ctx, db, location, role);
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
        if (state->command.argv[0] == NULL) {
            REDIS_LOG_ERROR(ctx,
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
        if (state->command.argv[state->command.argc - 1] == NULL) {
            REDIS_LOG_ERROR(ctx,
                "Failed to allocate memory in workspace (%p)",
                ctx->ws);
            flush_thread_state(state);
        }
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
vmod_db_execute(VRT_CTX, struct vmod_redis_db *db, VCL_BOOL master)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(ctx, 0);

    // Do not continue if the initial call to redis.command() was not executed
    // or if running this in a different database or if the workspace is
    // already overflowed.
    if ((state->command.argc >= 1) &&
        (state->command.db == db) &&
        (!WS_Overflowed(ctx->ws))) {
        // Initializations.
        unsigned retries = 0;

        // Force execution of LUA scripts in a master server when Redis Cluster
        // support is enabled. It's responsibility of the caller to avoid
        // execution of LUA scripts in slaves servers when clustering is
        // enabled. However, due it's counter-intuitiveness and the hidden and
        // expensive side effects (redirections followed by executions of
        // discoveries of the cluster topology) we enforce this here.
        if (db->cluster.enabled &&
            !master &&
            ((strcasecmp(state->command.argv[0], "EVAL") == 0) ||
             (strcasecmp(state->command.argv[0], "EVALSHA") == 0))) {
            master = 1;
        }

        // Clustered vs. standalone execution.
        if (db->cluster.enabled) {
            state->command.reply = cluster_execute(
                ctx, db, state, version,
                state->command.timeout, state->command.max_retries,
                state->command.argc, state->command.argv,
                &retries, master);
        } else {
            state->command.reply = redis_execute(
                ctx, db, state, version,
                state->command.timeout, state->command.max_retries,
                state->command.argc, state->command.argv,
                &retries, NULL, 0, master, 0);
        }

        // Log error replies (other errors are already logged while executing
        // commands, retries, redirections, etc.).
        if ((state->command.reply != NULL) &&
            (state->command.reply->type == REDIS_REPLY_ERROR)) {
            REDIS_LOG_ERROR(ctx,
                "Got error reply while executing Redis command (%s): %s",
                state->command.argv[0],
                state->command.reply->str);

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
        if (result == NULL) { \
            REDIS_LOG_ERROR(ctx, \
                "Failed to allocate memory in workspace (%p)", \
                ctx->ws); \
        } \
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
              "\"version\": %d,"
              "\"sick\": %d"
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
        db->stats.connections.dropped.sick,
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
    } else if (strcmp(name, "connections.dropped.sick") == 0) {
        return db->stats.connections.dropped.sick;
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

static void
make_thread_key()
{
    AZ(pthread_key_create(&thread_key, (void *) free_thread_state));
}

static void
unsafe_set_subnets(VRT_CTX, vcl_priv_t *config, const char *masks)
{
    // Assertions.
    //   - config->mutex locked.

    // Initializations
    unsigned error = 0;

    // Parse input.
    const char *p = masks;
    while (*p != '\0') {
        // Initializations.
        const char *q;

        // Parse weight.
        int weight = strtoul(p, (char **)&q, 10);
        if ((p == q) || (weight < 0) || (weight >= NREDIS_SERVER_WEIGHTS)) {
            error = 10;
            break;
        }

        // Parse address in the mask.
        char address[32];
        p = q;
        while (isspace(*p)) p++;
        q = p;
        while (*q != '\0' && *q != '/') {
            q++;
        }
        if ((p == q) || (*q != '/') || (q - p >= sizeof(address))) {
            error = 20;
            break;
        }
        memcpy(address, p, q - p);
        address[q - p] = '\0';
        struct in_addr ia4;
        if (inet_pton(AF_INET, address, &ia4) == 0) {
            error = 30;
            break;
        }

        // Parse number of bits in the mask.
        p = q + 1;
        if (!isdigit(*p)) {
            error = 40;
            break;
        }
        int bits = strtoul(p, (char **)&q, 10);
        if ((p == q) || (bits < 0) || (bits > 32)) {
            error = 50;
            break;
        }

        // Store parsed subnet.
        vcl_priv_subnet_t *subnet = new_vcl_priv_subnet(weight, ia4, bits);
        VTAILQ_INSERT_TAIL(&config->subnets, subnet, list);

        // More items?
        p = q;
        while (isspace(*p) || (*p == ',')) p++;
    }

    // Check error flag.
    if (error) {
        // Release parsed subnets.
        vcl_priv_subnet_t *isubnet;
        while (!VTAILQ_EMPTY(&config->subnets)) {
            isubnet = VTAILQ_FIRST(&config->subnets);
            CHECK_OBJ_NOTNULL(isubnet, VCL_PRIV_SUBNET_MAGIC);
            VTAILQ_REMOVE(&config->subnets, isubnet, list);
            free_vcl_priv_subnet(isubnet);
        }

        // Log error.
        REDIS_LOG_ERROR(ctx,
            "Got error while parsing subnets (%d)",
            error);
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
            if (result == NULL) {
                REDIS_LOG_ERROR(ctx,
                    "Failed to allocate memory in workspace (%p)",
                    ctx->ws);
            }
            break;

        case REDIS_REPLY_INTEGER:
            result = WS_Printf(ctx->ws, "%lld", reply->integer);
            if (result == NULL) {
                REDIS_LOG_ERROR(ctx,
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
handle_vcl_warm_event(VRT_CTX, vcl_priv_t *config)
{
    // Increase the global version. This will be used to (1) reestablish Redis
    // connections binded to worker threads; and (2) regenerate pooled
    // connections shared between threads.
    AZ(pthread_mutex_lock(&mutex));
    version++;
    AZ(pthread_mutex_unlock(&mutex));

    // Start Sentinel thread?
    AZ(pthread_mutex_lock(&config->mutex));
    if ((config->sentinels.locations != NULL) &&
        (!config->sentinels.active)) {
        unsafe_sentinel_start(config);
    }
    AZ(pthread_mutex_unlock(&config->mutex));
}

static void
handle_vcl_cold_event(VRT_CTX, vcl_priv_t *config)
{
    // If required, stop Sentinel thread and wait for termination. This
    // guarantees the Sentinel thread won't loose the config reference
    // in its internal state unexpectedly.
    AZ(pthread_mutex_lock(&config->mutex));
    if (config->sentinels.active) {
        unsafe_sentinel_stop(config);
        AZ(pthread_mutex_unlock(&config->mutex));
        AN(config->sentinels.thread);
        AZ(pthread_join(config->sentinels.thread, NULL));
        config->sentinels.thread = 0;
    } else {
        AZ(pthread_mutex_unlock(&config->mutex));
    }

    // Iterate through registered database instances and close connections in
    // shared pools. Connections binded to worker threads cannot be closed
    // this way.
    unsigned dbs = 0;
    unsigned connections = 0;
    AZ(pthread_mutex_lock(&config->mutex));
    vcl_priv_db_t *idb;
    VTAILQ_FOREACH(idb, &config->dbs, list) {
        // Assertions.
        CHECK_OBJ_NOTNULL(idb, VCL_PRIV_DB_MAGIC);

        // Initializations.
        dbs++;

        // Get database lock.
        AZ(pthread_mutex_lock(&idb->db->mutex));

        // Release contexts in all pools.
        for (unsigned iweight = 0; iweight < NREDIS_SERVER_WEIGHTS; iweight++) {
            for (enum REDIS_SERVER_ROLE irole = 0; irole < NREDIS_SERVER_ROLES; irole++) {
                redis_server_t *iserver;
                VTAILQ_FOREACH(iserver, &(idb->db->servers[iweight][irole]), list) {
                    // Assertions.
                    CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);

                    // Get pool lock.
                    AZ(pthread_mutex_lock(&iserver->pool.mutex));

                    // Release all contexts (both free an busy; this method is
                    // assumed to be called when threads are not using the pool).
                    iserver->pool.ncontexts = 0;
                    redis_context_t *icontext;
                    while (!VTAILQ_EMPTY(&iserver->pool.free_contexts)) {
                        icontext = VTAILQ_FIRST(&iserver->pool.free_contexts);
                        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                        connections++;
                        VTAILQ_REMOVE(&iserver->pool.free_contexts, icontext, list);
                        free_redis_context(icontext);
                    }
                    while (!VTAILQ_EMPTY(&iserver->pool.busy_contexts)) {
                        icontext = VTAILQ_FIRST(&iserver->pool.busy_contexts);
                        CHECK_OBJ_NOTNULL(icontext, REDIS_CONTEXT_MAGIC);
                        connections++;
                        VTAILQ_REMOVE(&iserver->pool.busy_contexts, icontext, list);
                        free_redis_context(icontext);
                    }

                    // Release pool lock.
                    AZ(pthread_mutex_unlock(&iserver->pool.mutex));
                }
            }
        }

        // Release database lock.
        AZ(pthread_mutex_unlock(&idb->db->mutex));
    }
    AZ(pthread_mutex_unlock(&config->mutex));
    REDIS_LOG_INFO(ctx,
        "Released %d pooled connections in %d database objects",
        connections, dbs);
}
