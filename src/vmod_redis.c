#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>
#include <arpa/inet.h>

#include "cache/cache.h"
#include "vcc_redis_if.h"

#ifdef TLS_ENABLED
#include <openssl/ssl.h>
#if HAVE_DECL_VSSLH_STATUS == 1
#include <vsslh.h>
#endif
#endif

#include "cluster.h"
#include "core.h"
#include "sentinel.h"

static task_state_t *get_task_state(VRT_CTX, struct vmod_priv *task_priv, unsigned flush);
static void flush_task_state(task_state_t *state);

static enum REDIS_SERVER_ROLE type2role(VCL_ENUM type);

static enum REDIS_PROTOCOL parse_protocol(VCL_ENUM protocol);

static const char *get_reply(VRT_CTX, redisReply *reply);

/******************************************************************************
 * VMOD EVENTS.
 *****************************************************************************/

static int
handle_vcl_load_event(VRT_CTX, struct vmod_priv *vcl_priv)
{
    // Initialize Varnish locks.
    if (vmod_state.locks.refs == 0) {
        vmod_state.locks.config = Lck_CreateClass(
            &vmod_state.locks.vsc_seg, "redis.config");
        AN(vmod_state.locks.config);
        vmod_state.locks.db = Lck_CreateClass(
            &vmod_state.locks.vsc_seg, "redis.db");
        AN(vmod_state.locks.db);
    }
    vmod_state.locks.refs++;

    // Initialize configuration in the local VCL data structure.
    vcl_priv->priv = new_vcl_state();
    vcl_priv->free = (vmod_priv_free_f *)free_vcl_state;

#ifdef TLS_ENABLED
    // Ensure OpenSSL global state is initialized only once.
#if HAVE_DECL_VSSLH_STATUS == 1
    AN(VSSLH_status());
#else
    static int openssl_initialized = 0;
    if (!openssl_initialized) {
        openssl_initialized = 1;
        SSL_library_init();
    }
#endif
#endif

    // Done!
    return 0;
}

static int
handle_vcl_warm_event(VRT_CTX, vcl_state_t *config)
{
    // Increase global version
    AZ(pthread_mutex_lock(&vmod_state.mutex));
    vmod_state.version++;
    AZ(pthread_mutex_unlock(&vmod_state.mutex));

    // Start Sentinel thread?
    Lck_Lock(&config->mutex);
    if ((config->sentinels.locations != NULL) &&
        (!config->sentinels.active)) {
        unsafe_sentinel_start(config);
    }
    Lck_Unlock(&config->mutex);

    // Done!
    return 0;
}

static int
handle_vcl_cold_event(VRT_CTX, vcl_state_t *config)
{
    // If required, stop Sentinel thread and wait for termination. This
    // guarantees the Sentinel thread won't loose the config reference
    // in its internal state unexpectedly.
    Lck_Lock(&config->mutex);
    if (config->sentinels.active) {
        unsafe_sentinel_stop(config);
        Lck_Unlock(&config->mutex);
        AN(config->sentinels.thread);
        AZ(pthread_join(config->sentinels.thread, NULL));
        config->sentinels.thread = 0;
    } else {
        Lck_Unlock(&config->mutex);
    }

    // Iterate through registered database instances and close connections in
    // shared pools. Connections binded to worker threads cannot be closed
    // this way.
    unsigned dbs = 0;
    unsigned connections = 0;
    Lck_Lock(&config->mutex);
    database_t *idb;
    VTAILQ_FOREACH(idb, &config->dbs, list) {
        // Assertions.
        CHECK_OBJ_NOTNULL(idb, DATABASE_MAGIC);

        // Initializations.
        dbs++;

        // Get database lock.
        Lck_Lock(&idb->db->mutex);

        // Release contexts in all pools.
        for (unsigned iweight = 0; iweight < NREDIS_SERVER_WEIGHTS; iweight++) {
            for (enum REDIS_SERVER_ROLE irole = 0; irole < NREDIS_SERVER_ROLES; irole++) {
                redis_server_t *iserver;
                VTAILQ_FOREACH(iserver, &(idb->db->servers[iweight][irole]), list) {
                    // Assertions.
                    CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);

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
                }
            }
        }

        // Release database lock.
        Lck_Unlock(&idb->db->mutex);
    }
    Lck_Unlock(&config->mutex);
    REDIS_LOG_INFO(ctx,
        "Released %d pooled connections in %d database objects",
        connections, dbs);

    // Done!
    return 0;
}

static int
handle_vcl_discard_event(VRT_CTX, vcl_state_t *config)
{
    // Assertions.
    assert(vmod_state.locks.refs > 0);

    // Release Varnish locks.
    vmod_state.locks.refs--;
    if (vmod_state.locks.refs == 0) {
        Lck_DestroyClass(&vmod_state.locks.vsc_seg);
    }

    // Done!
    return 0;
}

int
vmod_event_function(VRT_CTX, struct vmod_priv *vcl_priv, enum vcl_event_e e)
{
    // Log event.
    const char *name;
    switch (e) {
        case VCL_EVENT_LOAD: name = "load"; break;
        case VCL_EVENT_WARM: name = "warm"; break;
        case VCL_EVENT_COLD: name = "cold"; break;
        case VCL_EVENT_DISCARD: name = "discard"; break;
        default: name = "-";
    }
    REDIS_LOG_INFO(ctx,
        "VCL event triggered (event=%s)",
        name);

    // Route event.
    switch (e) {
        case VCL_EVENT_LOAD:
            return handle_vcl_load_event(ctx, vcl_priv);
        case VCL_EVENT_WARM:
            AN(vcl_priv->priv);
            return handle_vcl_warm_event(ctx, vcl_priv->priv);
        case VCL_EVENT_COLD:
            AN(vcl_priv->priv);
            return handle_vcl_cold_event(ctx, vcl_priv->priv);
        case VCL_EVENT_DISCARD:
            AN(vcl_priv->priv);
            return handle_vcl_discard_event(ctx, vcl_priv->priv);
        default:
            return 0;
    }
}

/******************************************************************************
 * redis.subnets();
 *****************************************************************************/

static void
unsafe_set_subnets(VRT_CTX, vcl_state_t *config, const char *masks)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);

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
        subnet_t *subnet = new_subnet(weight, ia4, bits);
        VTAILQ_INSERT_TAIL(&config->subnets, subnet, list);

        // More items?
        p = q;
        while (isspace(*p) || (*p == ',')) p++;
    }

    // Check error flag.
    if (error) {
        // Release parsed subnets.
        subnet_t *isubnet;
        while (!VTAILQ_EMPTY(&config->subnets)) {
            isubnet = VTAILQ_FIRST(&config->subnets);
            CHECK_OBJ_NOTNULL(isubnet, SUBNET_MAGIC);
            VTAILQ_REMOVE(&config->subnets, isubnet, list);
            free_subnet(isubnet);
        }

        // Log error.
        REDIS_LOG_ERROR(ctx,
            "Got error while parsing subnets (error=%d, masks=%s)",
            error, masks);
    }
}

VCL_VOID
vmod_subnets(VRT_CTX, struct vmod_priv *vcl_priv, VCL_STRING masks)
{
    // Initializations.
    vcl_state_t *config = vcl_priv->priv;

    // Get configuration lock.
    Lck_Lock(&config->mutex);

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
                "%s already set",
                "Subnets");
        }
    }

    // Release configuration lock.
    Lck_Unlock(&config->mutex);
}

/******************************************************************************
 * redis.sentinels();
 *****************************************************************************/

VCL_VOID
vmod_sentinels(
    VRT_CTX, struct vmod_priv *vcl_priv, VCL_STRING locations, VCL_INT period,
    VCL_INT connection_timeout, VCL_INT command_timeout, VCL_ENUM protocol,
    VCL_BOOL tls, VCL_STRING tls_cafile, VCL_STRING tls_capath,
    VCL_STRING tls_certfile, VCL_STRING tls_keyfile, VCL_STRING tls_sni,
    VCL_STRING password)
{
    // Initializations.
    vcl_state_t *config = vcl_priv->priv;

#ifndef RESP3_ENABLED
    // Abort if RESP3 is not available.
    if (protocol == enum_vmod_redis_RESP3) {
        REDIS_LOG_ERROR(ctx,
            "%s is not supported",
            "RESP3");
        return;
    }
#endif

#ifndef TLS_ENABLED
    // Abort if TLS is not enabled.
    if (tls) {
        REDIS_LOG_ERROR(ctx,
            "%s is not supported",
            "TLS");
        return;
    }
#endif

    // Get configuration lock.
    Lck_Lock(&config->mutex);

    // Do not continue if Sentinels have already been set.
    if (config->sentinels.locations == NULL) {
        if ((connection_timeout >= 0) &&
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
                config->sentinels.protocol = parse_protocol(protocol);
#ifdef TLS_ENABLED
                config->sentinels.tls = tls;
                if ((tls_cafile != NULL) && (strlen(tls_cafile) > 0)) {
                    config->sentinels.tls_cafile = strdup(tls_cafile);
                    AN(config->sentinels.tls_cafile);
                }
                if ((tls_capath != NULL) && (strlen(tls_capath) > 0)) {
                    config->sentinels.tls_capath = strdup(tls_capath);
                    AN(config->sentinels.tls_capath);
                }
                if ((tls_certfile != NULL) && (strlen(tls_certfile) > 0)) {
                    config->sentinels.tls_certfile = strdup(tls_certfile);
                    AN(config->sentinels.tls_certfile);
                }
                if ((tls_keyfile != NULL) && (strlen(tls_keyfile) > 0)) {
                    config->sentinels.tls_keyfile = strdup(tls_keyfile);
                    AN(config->sentinels.tls_keyfile);
                }
                if ((tls_sni != NULL) && (strlen(tls_sni) > 0)) {
                    config->sentinels.tls_sni = strdup(tls_sni);
                    AN(config->sentinels.tls_sni);
                }
#endif
                if ((password != NULL) && (strlen(password) > 0)) {
                    config->sentinels.password = strdup(password);
                    AN(config->sentinels.password);
                }
            }

            // If required, startup of the Sentinel thread and execution of
            // the initial discovery will be triggered during 'warm' event.
            // That cannot be done here because execution of logic in
            // 'vcl_init' after a 'load' event is not necessarily followed
            // by a 'warm' event.
        }
    } else {
        REDIS_LOG_ERROR(ctx,
            "%s already set",
            "Sentinels");
    }

    // Release configuration lock.
    Lck_Unlock(&config->mutex);
}

/******************************************************************************
 * DB OBJECT.
 *****************************************************************************/

VCL_VOID
vmod_db__init(
    VRT_CTX, struct vmod_redis_db **db, const char *vcl_name, struct vmod_priv *vcl_priv,
    VCL_STRING location, VCL_ENUM type, VCL_INT connection_timeout,
    VCL_INT connection_ttl, VCL_INT command_timeout, VCL_INT max_command_retries,
    VCL_BOOL shared_connections, VCL_INT max_connections, VCL_ENUM protocol,
    VCL_BOOL tls, VCL_STRING tls_cafile, VCL_STRING tls_capath,
    VCL_STRING tls_certfile, VCL_STRING tls_keyfile, VCL_STRING tls_sni,
    VCL_STRING user, VCL_STRING password, VCL_INT sickness_ttl,
    VCL_BOOL ignore_slaves, VCL_INT max_cluster_hops)
{
    // Assert input.
    CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
    AN(db);
    AZ(*db);

#ifndef RESP3_ENABLED
    // Abort if RESP3 is not available.
    if (protocol == enum_vmod_redis_RESP3) {
        REDIS_LOG_ERROR(ctx,
            "%s is not supported",
            "RESP3");
        return;
    }
#endif

#ifndef TLS_ENABLED
    // Abort if TLS is not enabled.
    if (tls) {
        REDIS_LOG_ERROR(ctx,
            "%s is not supported",
            "TLS");
        return;
    }
#endif

    // Check input.
    if ((connection_timeout >= 0) &&
        (connection_ttl >= 0) &&
        (command_timeout >= 0) &&
        (max_command_retries >= 0) &&
        (max_connections >= 0) &&
#ifdef TLS_ENABLED
        (tls_cafile != NULL) &&
        (tls_capath != NULL) &&
        (tls_certfile != NULL) &&
        (tls_keyfile != NULL) &&
        (tls_sni != NULL) &&
#endif
        (user != NULL) &&
        (password != NULL) &&
        (sickness_ttl >= 0) &&
        (max_cluster_hops >= 0)) {
        // Initializations.
        vcl_state_t *config = vcl_priv->priv;
        struct timeval connection_timeout_tv;
        connection_timeout_tv.tv_sec = connection_timeout / 1000;
        connection_timeout_tv.tv_usec = (connection_timeout % 1000) * 1000;
        struct timeval command_timeout_tv;
        command_timeout_tv.tv_sec = command_timeout / 1000;
        command_timeout_tv.tv_usec = (command_timeout % 1000) * 1000;

        // Extract role & clustering flag.
        enum REDIS_SERVER_ROLE role = type2role(type);
        unsigned clustered = type == enum_vmod_redis_cluster;

#ifdef TLS_ENABLED
        // Create OpenSSL context.
        SSL_CTX *tls_ssl_ctx = NULL;
        if (tls) {
            tls_ssl_ctx = new_SSL_CTX(
                ctx,
                strlen(tls_cafile) > 0 ? tls_cafile : NULL,
                strlen(tls_capath) > 0 ? tls_capath : NULL,
                strlen(tls_certfile) > 0 ? tls_certfile : NULL,
                strlen(tls_keyfile) > 0 ? tls_keyfile : NULL);
            if (tls_ssl_ctx == NULL) {
                return;
            }
        }
#endif

        // Create new database instance.
        struct vmod_redis_db *instance = new_vmod_redis_db(
            config, vcl_name, connection_timeout_tv, connection_ttl,
            command_timeout_tv, max_command_retries, shared_connections, max_connections,
            parse_protocol(protocol),
#ifdef TLS_ENABLED
            tls_ssl_ctx, tls_sni,
#endif
            user, password, sickness_ttl, ignore_slaves, clustered, max_cluster_hops);

        // Add initial server if provided.
        if ((location != NULL) && (strlen(location) > 0)) {
            // Add initial server.
            Lck_Lock(&config->mutex);
            Lck_Lock(&instance->mutex);
            redis_server_t *server = unsafe_add_redis_server(ctx, instance, config, location, role);
            Lck_Unlock(&instance->mutex);
            Lck_Unlock(&config->mutex);

            // Launch initial discovery of the cluster topology? This is not
            // required (i.e. it will be executed on demand) but it's a nice
            // thing to do while bootstrapping a new database instance.
            if (instance->cluster.enabled) {
                discover_cluster_slots(ctx, instance, config, server);
            }
        }

        // Register & return the new database instance.
        database_t *database = new_database(instance);
        Lck_Lock(&config->mutex);
        VTAILQ_INSERT_TAIL(&config->dbs, database, list);
        Lck_Unlock(&config->mutex);
        *db = instance;

        // Log event.
        REDIS_LOG_INFO(ctx,
            "New database instance registered (db=%s)",
            instance->name);
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

    // Keep config reference before releasing the instance.
    vcl_state_t *config = (*db)->config;

    // Unregister database instance.
    Lck_Lock(&config->mutex);
    database_t *idb;
    VTAILQ_FOREACH(idb, &config->dbs, list) {
        CHECK_OBJ_NOTNULL(idb, DATABASE_MAGIC);
        if (idb->db == *db) {
            VTAILQ_REMOVE(&config->dbs, idb, list);
            free_database(idb);
            break;
        }
    }
    Lck_Unlock(&config->mutex);
    *db = NULL;
}

/******************************************************************************
 * .add_server();
 *****************************************************************************/

VCL_VOID
vmod_db_add_server(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *vcl_priv,
    VCL_STRING location, VCL_ENUM type)
{
    if ((location != NULL) && (strlen(location) > 0) &&
        ((!db->cluster.enabled || type == enum_vmod_redis_cluster))) {
        // Initializations.
        vcl_state_t *config = vcl_priv->priv;
        enum REDIS_SERVER_ROLE role = type2role(type);

        // Add server.
        Lck_Lock(&config->mutex);
        Lck_Lock(&db->mutex);
        redis_server_t *server = unsafe_add_redis_server(ctx, db, config, location, role);
        unsigned discovery =
            (server != NULL) &&
            (db->cluster.enabled) &&
            ((db->stats.cluster.discoveries.total -
              db->stats.cluster.discoveries.failed) == 0);
        Lck_Unlock(&db->mutex);
        Lck_Unlock(&config->mutex);

        // Launch initial discovery of the cluster topology? This flag has been
        // previously calculated while holding the appropriate locks.
        if (discovery) {
            discover_cluster_slots(ctx, db, config, server);
        }
    }
}

/******************************************************************************
 * .command();
 *****************************************************************************/

VCL_VOID
vmod_db_command(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv,
    VCL_STRING name)
{
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch thread state & flush previous command.
        task_state_t *state = get_task_state(ctx, task_priv, 1);

        // Initialize.
        state->command.db = db;
        state->command.timeout = db->command_timeout;
        state->command.max_retries = db->max_command_retries;
        state->command.argc = 1;
        state->command.argv[0] = name;
    }
}

/******************************************************************************
 * .timeout();
 *****************************************************************************/

VCL_VOID
vmod_db_timeout(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv,
    VCL_INT command_timeout)
{
    // Fetch thread state.
    task_state_t *state = get_task_state(ctx, task_priv, 0);

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
vmod_db_retries(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv,
    VCL_INT max_command_retries)
{
    // Fetch thread state.
    task_state_t *state = get_task_state(ctx, task_priv, 0);

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
vmod_db_push(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv,
    VCL_STRING arg)
{
    // Fetch thread state.
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to .command() was not executed or
    // if running this in a different database.
    if ((state->command.argc >= 1) &&
        (state->command.argc < MAX_REDIS_COMMAND_ARGS) &&
        (state->command.db == db)) {
        // Handle NULL arguments as empty strings.
        if (arg == NULL) {
            arg = WS_Copy(ctx->ws, "", -1);
            if (arg == NULL) {
                REDIS_FAIL_WS(ctx, );
            }
        }
        state->command.argv[state->command.argc++] = arg;
    } else {
        REDIS_LOG_ERROR(ctx,
            "Failed to push argument (db=%s, limit=%d)",
            db->name, MAX_REDIS_COMMAND_ARGS);
    }
}

/******************************************************************************
 * .execute();
 *****************************************************************************/

VCL_VOID
vmod_db_execute(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *vcl_priv,
    struct vmod_priv *task_priv, VCL_BOOL master)
{
    // Fetch thread state.
    task_state_t *state = get_task_state(ctx, task_priv, 0);

    // Do not continue if the initial call to redis.command() was not executed
    // or if running this in a different database or if the workspace is
    // already overflowed.
    if ((state->command.argc >= 1) &&
        (state->command.db == db)) {
        // Initializations.
        vcl_state_t *config = vcl_priv->priv;
        unsigned retries = 0;

        // Ignore slave servers if globally requested.
        if ((!master) && (db->ignore_slaves)) {
            master = 1;
        }

        // Force execution of LUA scripts in a master server when Redis Cluster
        // support is enabled. It's responsibility of the caller to avoid
        // execution of LUA scripts in slaves servers when clustering is
        // enabled. However, due it's counter-intuitiveness and the hidden and
        // expensive side effects (redirections followed by executions of
        // discoveries of the cluster topology) we enforce this here.
        if ((db->cluster.enabled) &&
            (!master) &&
            ((strcasecmp(state->command.argv[0], "EVAL") == 0) ||
             (strcasecmp(state->command.argv[0], "EVALSHA") == 0))) {
            master = 1;
        }

        // Clustered vs. standalone execution.
        if (db->cluster.enabled) {
            state->command.reply = cluster_execute(
                ctx, db, config, state,
                state->command.timeout, state->command.max_retries,
                state->command.argc, state->command.argv,
                &retries, master);
        } else {
            state->command.reply = redis_execute(
                ctx, db, state,
                state->command.timeout, state->command.max_retries,
                state->command.argc, state->command.argv,
                &retries, NULL, 0, master, 0);
        }

        // Log error replies (other errors have already logged while executing
        // commands, retries, redirections, etc.).
        if ((state->command.reply != NULL) &&
            (state->command.reply->type == REDIS_REPLY_ERROR)) {
            REDIS_LOG_ERROR(ctx,
                "Got error reply while executing command (command=%s, db=%s): %s",
                state->command.argv[0], db->name, state->command.reply->str);

            Lck_Lock(&db->mutex);
            db->stats.commands.error++;
            Lck_Unlock(&db->mutex);
        }
    }
}

/******************************************************************************
 * .replied();
 *****************************************************************************/

VCL_BOOL
vmod_db_replied(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);
    return (state->command.db == db) && (state->command.reply != NULL);
}

/******************************************************************************
 * .reply_is_error();
 * .reply_is_nil();
 * .reply_is_status();
 * .reply_is_integer();
 * .reply_is_boolean();
 * .reply_is_double();
 * .reply_is_string();
 * .reply_is_array();
 *****************************************************************************/

#define VMOD_DB_REPLY_IS_FOO(foo, condition) \
VCL_BOOL \
vmod_db_reply_is_ ## foo(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv) \
{ \
    task_state_t *state = get_task_state(ctx, task_priv, 0); \
    return \
        (state->command.db == db) && \
        (state->command.reply != NULL) && \
        (condition); \
}

VMOD_DB_REPLY_IS_FOO(
    error,
    state->command.reply->type == REDIS_REPLY_ERROR)
VMOD_DB_REPLY_IS_FOO(
    nil,
    state->command.reply->type == REDIS_REPLY_NIL)
VMOD_DB_REPLY_IS_FOO(
    status,
    state->command.reply->type == REDIS_REPLY_STATUS)
VMOD_DB_REPLY_IS_FOO(
    integer,
    state->command.reply->type == REDIS_REPLY_INTEGER)
VMOD_DB_REPLY_IS_FOO(
    boolean,
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_BOOL, 0))
VMOD_DB_REPLY_IS_FOO(
    double,
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_DOUBLE, 0))
VMOD_DB_REPLY_IS_FOO(
    string,
    state->command.reply->type == REDIS_REPLY_STRING ||
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_VERB, 0))
VMOD_DB_REPLY_IS_FOO(
    array,
    state->command.reply->type == REDIS_REPLY_ARRAY ||
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_MAP, 0) ||
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_SET, 0))

/******************************************************************************
 * .get_reply();
 *****************************************************************************/

VCL_STRING
vmod_db_get_reply(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);
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
 * .get_boolean_reply();
 * .get_double_reply();
 * .get_string_reply();
 *****************************************************************************/

#define VMOD_DB_GET_FOO_REPLY(type, foo, condition, field, fallback) \
VCL_ ## type \
vmod_db_get_ ## foo ## _reply(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv) \
{ \
    task_state_t *state = get_task_state(ctx, task_priv, 0); \
    if ((state->command.db == db) && \
        (state->command.reply != NULL) && \
        (condition)) { \
        return state->command.reply->field; \
    } else { \
        return fallback; \
    } \
}

VMOD_DB_GET_FOO_REPLY(
    INT,
    integer,
    state->command.reply->type == REDIS_REPLY_INTEGER,
    integer,
    0)
VMOD_DB_GET_FOO_REPLY(
    BOOL,
    boolean,
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_BOOL, 0),
    integer,
    0)
VMOD_DB_GET_FOO_REPLY(
    REAL,
    double,
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_DOUBLE, 0),
    RESP3_SWITCH(dval, integer),
    0.0)

#define VMOD_DB_GET_FOO_STRING_REPLY(foo, condition) \
VCL_STRING \
vmod_db_get_ ## foo ## _reply(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv) \
{ \
    task_state_t *state = get_task_state(ctx, task_priv, 0); \
    if ((state->command.db == db) && \
        (state->command.reply != NULL) && \
        (condition)) { \
        char *result = WS_Copy(ctx->ws, state->command.reply->str, state->command.reply->len + 1); \
        if (result == NULL) { \
            REDIS_FAIL_WS(ctx, NULL); \
        } \
        return result; \
    } else { \
        return NULL; \
    } \
}

VMOD_DB_GET_FOO_STRING_REPLY(
    error,
    state->command.reply->type == REDIS_REPLY_ERROR)
VMOD_DB_GET_FOO_STRING_REPLY(
    status,
    state->command.reply->type == REDIS_REPLY_STATUS)
VMOD_DB_GET_FOO_STRING_REPLY(
    string,
    state->command.reply->type == REDIS_REPLY_STRING ||
    RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_VERB, 0))

/******************************************************************************
 * .get_array_reply_length();
 *****************************************************************************/

VCL_INT
vmod_db_get_array_reply_length(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);
    if ((state->command.db == db) &&
        (state->command.reply != NULL) &&
        (state->command.reply->type == REDIS_REPLY_ARRAY ||
         RESP3_SWITCH(state->command.reply->type != REDIS_REPLY_MAP, 0) ||
         RESP3_SWITCH(state->command.reply->type != REDIS_REPLY_SET, 0))) {
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
 * .array_reply_is_boolean();
 * .array_reply_is_double();
 * .array_reply_is_string();
 * .array_reply_is_array();
 *****************************************************************************/

#define VMOD_DB_ARRAY_REPLY_IS_FOO(foo, condition) \
VCL_BOOL \
vmod_db_array_reply_is_ ## foo(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv, VCL_INT index) \
{ \
    task_state_t *state = get_task_state(ctx, task_priv, 0); \
    return \
        (state->command.db == db) && \
        (state->command.reply != NULL) && \
        (state->command.reply->type == REDIS_REPLY_ARRAY) && \
        (index < state->command.reply->elements) && \
        (condition); \
}

VMOD_DB_ARRAY_REPLY_IS_FOO(
    error,
    state->command.reply->element[index]->type == REDIS_REPLY_ERROR)
VMOD_DB_ARRAY_REPLY_IS_FOO(
    nil,
    state->command.reply->element[index]->type == REDIS_REPLY_NIL)
VMOD_DB_ARRAY_REPLY_IS_FOO(
    status,
    state->command.reply->element[index]->type == REDIS_REPLY_STATUS)
VMOD_DB_ARRAY_REPLY_IS_FOO(
    integer,
    state->command.reply->element[index]->type == REDIS_REPLY_INTEGER)
VMOD_DB_ARRAY_REPLY_IS_FOO(
    boolean,
    RESP3_SWITCH(state->command.reply->element[index]->type == REDIS_REPLY_BOOL, 0))
VMOD_DB_ARRAY_REPLY_IS_FOO(
    double,
    RESP3_SWITCH(state->command.reply->element[index]->type == REDIS_REPLY_DOUBLE, 0))
VMOD_DB_ARRAY_REPLY_IS_FOO(
    string,
    state->command.reply->element[index]->type == REDIS_REPLY_STRING ||
    RESP3_SWITCH(state->command.reply->element[index]->type == REDIS_REPLY_VERB, 0))
VMOD_DB_ARRAY_REPLY_IS_FOO(
    array,
    state->command.reply->element[index]->type == REDIS_REPLY_ARRAY ||
    RESP3_SWITCH(state->command.reply->element[index]->type == REDIS_REPLY_MAP, 0) ||
    RESP3_SWITCH(state->command.reply->element[index]->type == REDIS_REPLY_SET, 0))

/******************************************************************************
 * .get_array_reply_value();
 *****************************************************************************/

VCL_STRING
vmod_db_get_array_reply_value(
    VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv,
    VCL_INT index)
{
    task_state_t *state = get_task_state(ctx, task_priv, 0);
    if ((state->command.db == db) &&
        (state->command.reply != NULL) &&
        (state->command.reply->type == REDIS_REPLY_ARRAY ||
         RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_MAP, 0) ||
         RESP3_SWITCH(state->command.reply->type == REDIS_REPLY_SET, 0)) &&
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
vmod_db_free(VRT_CTX, struct vmod_redis_db *db, struct vmod_priv *task_priv)
{
    get_task_state(ctx, task_priv, 1);
}

/******************************************************************************
 * .stats();
 *****************************************************************************/

VCL_STRING
vmod_db_stats(VRT_CTX, struct vmod_redis_db *db)
{
    Lck_Lock(&db->mutex);
    char *result = WS_Printf(ctx->ws,
        "{"
          "\"servers\": {"
            "\"total\": %" PRIu64 ","
            "\"failed\": %" PRIu64
          "},"
          "\"connections\": {"
            "\"total\": %" PRIu64 ","
            "\"failed\": %" PRIu64 ","
            "\"dropped\": {"
              "\"error\": %" PRIu64 ","
              "\"hung_up\": %" PRIu64 ","
              "\"overflow\": %" PRIu64 ","
              "\"ttl\": %" PRIu64 ","
              "\"version\": %" PRIu64 ","
              "\"sick\": %" PRIu64
            "}"
          "},"
          "\"workers\": {"
              "\"blocked\": %" PRIu64
          "},"
          "\"commands\": {"
            "\"total\": %" PRIu64 ","
            "\"failed\": %" PRIu64 ","
            "\"retried\": %" PRIu64 ","
            "\"error\": %" PRIu64 ","
            "\"noscript\": %" PRIu64
          "},"
          "\"cluster\": {"
            "\"discoveries\": {"
              "\"total\": %" PRIu64 ","
              "\"failed\": %" PRIu64
            "},"
            "\"replies\": {"
              "\"moved\": %" PRIu64 ","
              "\"ask\": %" PRIu64
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
    Lck_Unlock(&db->mutex);
    if (result == NULL) {
        REDIS_FAIL_WS(ctx, NULL);
    }
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
            "Failed to fetch counter (name=%s)",
            name);
        return 0;
    }
}

/******************************************************************************
 * PROXY.
 *****************************************************************************/

static struct vmod_redis_db *
get_db_instance(VRT_CTX, vcl_state_t *config, const char *db)
{
    // Initializations.
    struct vmod_redis_db *result = NULL;

    // Search database instance.
    Lck_Lock(&config->mutex);
    database_t *idb;
    VTAILQ_FOREACH(idb, &config->dbs, list) {
        CHECK_OBJ_NOTNULL(idb, DATABASE_MAGIC);
        if (strcmp(idb->db->name, db) == 0) {
            result = idb->db;
            break;
        }
    }
    Lck_Unlock(&config->mutex);

    // Done!
    return result;
}


VCL_VOID
vmod_use(
    VRT_CTX, struct vmod_priv *vcl_priv, struct vmod_priv *task_priv,
    VCL_STRING db)
{
    // Fetch thread state & flush previous command.
    task_state_t *state = get_task_state(ctx, task_priv, 1);

    // Check input.
    if ((db != NULL) && (strlen(db) > 0)) {
        vcl_state_t *config = vcl_priv->priv;
        state->db = get_db_instance(ctx, config, db);
    } else {
        state->db = NULL;
    }

    // Log error?
    if (state->db == NULL) {
        REDIS_LOG_ERROR(ctx,
            "Failed to use database (db=%s)",
            db);
    }
}

#define _COMMA_ ,

#define VMOD_PROXIED_METHOD(type, fallback, method, margs, fargs...) \
VCL_ ## type \
vmod_ ## method(VRT_CTX, struct vmod_priv *vcl_priv, struct vmod_priv *task_priv, ##fargs, VCL_STRING db) \
{ \
    struct vmod_redis_db *instance; \
    if ((db != NULL) && (strlen(db) > 0)) { \
        vcl_state_t *config = vcl_priv->priv; \
        instance = get_db_instance(ctx, config, db); \
    } else { \
        task_state_t *state = get_task_state(ctx, task_priv, 0); \
        instance = state->db; \
    } \
    \
    if (instance != NULL) { \
        return vmod_db_ ## method(ctx, instance margs); \
    } else { \
        REDIS_LOG_ERROR(ctx, \
            "Database instance not available%s", \
            ""); \
        return fallback; \
    } \
}

VMOD_PROXIED_METHOD(
    VOID, , add_server,
    _COMMA_ vcl_priv _COMMA_ location _COMMA_ type,
    VCL_STRING location, VCL_ENUM type)
VMOD_PROXIED_METHOD(
    VOID, , command,
    _COMMA_ task_priv _COMMA_ name,
    VCL_STRING name)
VMOD_PROXIED_METHOD(
    VOID, , timeout,
    _COMMA_ task_priv _COMMA_ command_timeout,
    VCL_INT command_timeout)
VMOD_PROXIED_METHOD(
    VOID, , retries,
    _COMMA_ task_priv _COMMA_ max_command_retries,
    VCL_INT max_command_retries)
VMOD_PROXIED_METHOD(
    VOID, , push,
    _COMMA_ task_priv _COMMA_ arg,
    VCL_STRING arg)
VMOD_PROXIED_METHOD(
    VOID, , execute,
    _COMMA_ vcl_priv _COMMA_ task_priv _COMMA_ master,
    VCL_BOOL master)
VMOD_PROXIED_METHOD(
    BOOL, 0, replied,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_error,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_nil,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_status,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_integer,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_boolean,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_double,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_string,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, reply_is_array,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    STRING, NULL, get_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    STRING, NULL, get_error_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    STRING, NULL, get_status_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    INT, 0, get_integer_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, get_boolean_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    REAL, 0, get_double_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    STRING, NULL, get_string_reply,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    INT, 0, get_array_reply_length,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_error,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_nil,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_status,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_integer,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_boolean,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_double,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_string,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    BOOL, 0, array_reply_is_array,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    STRING, NULL, get_array_reply_value,
    _COMMA_ task_priv _COMMA_ index,
    VCL_INT index)
VMOD_PROXIED_METHOD(
    VOID, , free,
    _COMMA_ task_priv)
VMOD_PROXIED_METHOD(
    STRING, NULL, stats,
    )
VMOD_PROXIED_METHOD(
    INT, 0, counter,
    _COMMA_ name,
    VCL_STRING name)

#undef _COMMA_

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static task_state_t *
get_task_state(VRT_CTX, struct vmod_priv *task_priv, unsigned flush)
{
    // Initializations.
    task_state_t *result = NULL;

    // Create thread state if not created yet.
    if (task_priv->priv == NULL) {
        task_priv->priv = new_task_state();
        task_priv->free = (vmod_priv_free_f *)free_task_state;
        result = task_priv->priv;
    } else {
        result = task_priv->priv;
        CHECK_OBJ(result, TASK_STATE_MAGIC);
    }

    // Flush enqueued command?
    if (flush) {
        flush_task_state(result);
    }

    // Done!
    return result;
}

static void
flush_task_state(task_state_t *state)
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

static enum REDIS_SERVER_ROLE
type2role(VCL_ENUM type)
{
    enum REDIS_SERVER_ROLE result;
    if (type == enum_vmod_redis_master) {
        result = REDIS_SERVER_MASTER_ROLE;
    } else if (type == enum_vmod_redis_slave) {
        result = REDIS_SERVER_SLAVE_ROLE;
    } else if (type == enum_vmod_redis_auto) {
        result = REDIS_SERVER_TBD_ROLE;
    } else if (type == enum_vmod_redis_cluster) {
        result = REDIS_SERVER_TBD_ROLE;
    } else {
        WRONG("Invalid server type value.");
    }
    return result;
}

static enum REDIS_PROTOCOL
parse_protocol(VCL_ENUM protocol)
{
    enum REDIS_PROTOCOL result;
    if (protocol == enum_vmod_redis_default) {
        result = REDIS_PROTOCOL_DEFAULT;
    } else if (protocol == enum_vmod_redis_RESP2) {
        result = REDIS_PROTOCOL_RESP2;
    } else if (protocol == enum_vmod_redis_RESP3) {
        result = REDIS_PROTOCOL_RESP3;
    } else {
        WRONG("Invalid protocol value.");
    }
    return result;
}

static const char *
get_reply(VRT_CTX, redisReply *reply)
{
    // Default result.
    const char *result = NULL;

    // Check type of reply.
    switch (reply->type) {
        case REDIS_REPLY_ERROR:
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
#ifdef RESP3_ENABLED
        case REDIS_REPLY_VERB:
        case REDIS_REPLY_DOUBLE:
#endif
            result = WS_Copy(ctx->ws, reply->str, reply->len + 1);
            if (result == NULL) {
                REDIS_FAIL_WS(ctx, NULL);
            }
            break;

        case REDIS_REPLY_INTEGER:
#ifdef RESP3_ENABLED
        case REDIS_REPLY_BOOL:
#endif
            result = WS_Printf(ctx->ws, "%lld", reply->integer);
            if (result == NULL) {
                REDIS_FAIL_WS(ctx, NULL);
            }
            break;

        case REDIS_REPLY_ARRAY:
#ifdef RESP3_ENABLED
        case REDIS_REPLY_MAP:
        case REDIS_REPLY_SET:
#endif
            // XXX: array, map & set replies are *not* supported.
            result = NULL;
            break;

        default:
            result = NULL;
    }

    // Done!
    return result;
}
