#include "config.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>
#ifdef TLS_ENABLED
#include <hiredis/hiredis_ssl.h>
#endif
#include <hiredis/async.h>
#include <hiredis/adapters/libev.h>
#include <arpa/inet.h>

#include "vrt.h"
#include "cache/cache.h"

#include "core.h"
#include "sentinel.h"

#define SUBSCRIPTION_COMMAND "PSUBSCRIBE +sdown -sdown +odown -odown +switch-master"

struct server {
    // Object marker.
#define SERVER_MAGIC 0x762a900c
    unsigned magic;

    // Location.
    const char *host;
    unsigned port;

    // Most recent discovered properties.
    enum REDIS_SERVER_ROLE role;
    unsigned down;

    // Sentinel responsible of the last change to the previous properties.
    struct sentinel *sentinel;

    // Tail queue.
    VTAILQ_ENTRY(server) list;
};

struct sentinel {
    // Object marker.
#define SENTINEL_MAGIC 0x8fefa255
    unsigned magic;

    // Location.
    const char *host;
    unsigned port;

    // Non-blocking connection.
    redisAsyncContext *context;

    // State reference, useful when processing Pub/Sub messages.
    struct state *state;

    // Tail queue.
    VTAILQ_ENTRY(sentinel) list;
};

struct state {
    // Object marker.
#define STATE_MAGIC 0xd5ae987b
    unsigned magic;

    // Config reference.
    vcl_state_t *config;

    // Configuration.
    VTAILQ_HEAD(,sentinel) sentinels;
    unsigned period;
    struct timeval connection_timeout;
    struct timeval command_timeout;
    enum REDIS_PROTOCOL protocol;
#ifdef TLS_ENABLED
    redisSSLContext *tls_ssl_ctx;
#endif
    const char *password;

    // Timestamps.
    time_t last_change;
    time_t next_discovery;

    // Known servers.
    VTAILQ_HEAD(,server) servers;
};

static void *sentinel_loop(void *object);

static struct state *new_state(
    vcl_state_t *config, unsigned period, struct timeval connection_timeout,
    struct timeval command_timeout, enum REDIS_PROTOCOL protocol,
#ifdef TLS_ENABLED
    redisSSLContext *tls_ssl_ctx,
#endif
    const char *password);
static void free_state(struct state *state);

static void unsafe_set_locations(struct state *state, const char *locations);

static void parse_sentinel_notification(struct sentinel *sentinel, redisReply *reply);

static void discover_servers(struct state *state);

static void unsafe_update_dbs(struct state *state);

void
unsafe_sentinel_start(vcl_state_t *config)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    AN(config->sentinels.locations);
    AZ(config->sentinels.thread);
    AZ(config->sentinels.active);

#ifdef TLS_ENABLED
    // Create Redis SSL context.
    redisSSLContext *tls_ssl_ctx = NULL;
    if (config->sentinels.tls) {
        redisSSLContextError ssl_error;
        tls_ssl_ctx = redisCreateSSLContext(
            config->sentinels.tls_cafile,
            config->sentinels.tls_capath,
            config->sentinels.tls_certfile,
            config->sentinels.tls_keyfile,
            config->sentinels.tls_sni,
            &ssl_error);
        if (tls_ssl_ctx == NULL) {
            REDIS_LOG_ERROR(NULL,
                "Failed to create SSL context: %s",
                redisSSLContextGetError(ssl_error));
            return;
        }
    }
#endif

    // Try to start new thread and launch initial proactive discovery.
    struct state *state = new_state(
        config,
        config->sentinels.period,
        config->sentinels.connection_timeout,
        config->sentinels.command_timeout,
        config->sentinels.protocol,
#ifdef TLS_ENABLED
        tls_ssl_ctx,
#endif
        config->sentinels.password);
    unsafe_set_locations(state, config->sentinels.locations);
    if (!VTAILQ_EMPTY(&state->sentinels)) {
        AZ(pthread_create(
            &config->sentinels.thread,
            NULL,
            &sentinel_loop,
            state));
        config->sentinels.active = 1;
        config->sentinels.discovery = 1;
    } else {
        free_state(state);
    }
}

void
unsafe_sentinel_discovery(vcl_state_t *config)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    AN(config->sentinels.locations);
    AN(config->sentinels.thread);
    AN(config->sentinels.active);

    // Request proactive discovery.
    config->sentinels.discovery = 1;
}

void
unsafe_sentinel_stop(vcl_state_t *config)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    AN(config->sentinels.locations);
    AN(config->sentinels.thread);
    AN(config->sentinels.active);

    // Request thread stop. Caller must wait for thread termination outside
    // the config mutex lock in order to ensure the thread does not loose
    // the config reference in its internal state unexpectedly.
    config->sentinels.active = 0;
}

/******************************************************************************
 * THREAD LOOP.
 *****************************************************************************/

static void
connectCallback(const redisAsyncContext *context, int status)
{
    if (status != REDIS_OK) {
        struct sentinel *sentinel;
        CAST_OBJ_NOTNULL(sentinel, context->data, SENTINEL_MAGIC);

        sentinel->context = NULL;

        REDIS_LOG_ERROR(NULL,
            "Failed to establish Sentinel connection (error=%d, status=%d, sentinel=%s:%d): %s",
            context->err, status, sentinel->host, sentinel->port,
            HIREDIS_ERRSTR(context));
    }
}

static void
disconnectCallback(const redisAsyncContext *context, int status)
{
    struct sentinel *sentinel;
    CAST_OBJ_NOTNULL(sentinel, context->data, SENTINEL_MAGIC);

    sentinel->context = NULL;

    if (status != REDIS_OK) {
        REDIS_LOG_ERROR(NULL,
            "Sentinel connection lost (error=%d, status=%d, sentinel=%s:%d): %s",
            context->err, status, sentinel->host, sentinel->port,
            HIREDIS_ERRSTR(context));
    }
}

static void
authorizeCallback(redisAsyncContext *context, void *r, void *s)
{
    redisReply *reply = r;

    struct sentinel *sentinel;
    CAST_OBJ_NOTNULL(sentinel, s, SENTINEL_MAGIC);

    if (reply == NULL ||
        reply->type != REDIS_REPLY_STATUS ||
        strcmp(reply->str, "OK") != 0) {
        REDIS_LOG_ERROR(NULL,
            "Failed to authenticate Sentinel connection (error=%d, sentinel=%s:%d): %s",
            context->err, sentinel->host, sentinel->port,
            HIREDIS_ERRSTR(context, reply));
    }
}

static void
helloCallback(redisAsyncContext *context, void *r, void *s)
{
    redisReply *reply = r;

    struct sentinel *sentinel;
    CAST_OBJ_NOTNULL(sentinel, s, SENTINEL_MAGIC);

    if (reply == NULL ||
        (reply->type != REDIS_REPLY_ARRAY &&
         RESP3_SWITCH(reply->type != REDIS_REPLY_MAP, 1))) {
        REDIS_LOG_ERROR(NULL,
            "Failed to negotiate protocol in Sentinel connection (error=%d, sentinel=%s:%d): %s",
            context->err, sentinel->host, sentinel->port,
            HIREDIS_ERRSTR(context, reply));
    }
}

static void
subscribeCallback(redisAsyncContext *context, void *reply, void *s)
{
    struct sentinel *sentinel;
    CAST_OBJ_NOTNULL(sentinel, s, SENTINEL_MAGIC);

    parse_sentinel_notification(sentinel, reply);
}

static void *
sentinel_loop(void *object)
{
    // Assertions.
    struct state *state;
    CAST_OBJ_NOTNULL(state, object, STATE_MAGIC);
    CHECK_OBJ_NOTNULL(state->config, VCL_STATE_MAGIC);

    // Log event.
    Lck_Lock(&state->config->mutex);
    REDIS_LOG_INFO(NULL,
        "Sentinel thread started (locations=%s, period=%d)",
        state->config->sentinels.locations,
        state->config->sentinels.period);
    Lck_Unlock(&state->config->mutex);

    // Initializations.
    struct ev_loop* loop = ev_loop_new(EVFLAG_AUTO);
    AN(loop);

    // Thread loop.
    while (1) {
        // Assertions.
        CHECK_OBJ_NOTNULL(state, STATE_MAGIC);
        CHECK_OBJ_NOTNULL(state->config, VCL_STATE_MAGIC);

        // Terminate the thread loop?
        Lck_Lock(&state->config->mutex);
        if (!state->config->sentinels.active) {
            Lck_Unlock(&state->config->mutex);
            break;
        }

        // Initializations.
        time_t now = time(NULL);

        // Is time to execute a new discovery?
        if ((state->config->sentinels.discovery) ||
            ((state->period > 0) && (state->next_discovery <= now))) {
            Lck_Unlock(&state->config->mutex);
            discover_servers(state);
            Lck_Lock(&state->config->mutex);
            state->next_discovery = now + state->period;
        }

        // Terminate the thread loop?
        if (!state->config->sentinels.active) {
            Lck_Unlock(&state->config->mutex);
            break;
        }
        Lck_Unlock(&state->config->mutex);

        // Check Pub/Sub connections.
        struct sentinel *isentinel;
        VTAILQ_FOREACH(isentinel, &state->sentinels, list) {
            CHECK_OBJ_NOTNULL(isentinel, SENTINEL_MAGIC);
            if (isentinel->context == NULL) {
                isentinel->context = redisAsyncConnect(isentinel->host, isentinel->port);
                if ((isentinel->context != NULL) && (!isentinel->context->err)) {
#ifdef TLS_ENABLED
                    if (state->tls_ssl_ctx != NULL &&
                        redisInitiateSSLWithContext(&isentinel->context->c, state->tls_ssl_ctx) != REDIS_OK) {
                        REDIS_LOG_ERROR(NULL,
                            "Failed to secure asynchronous Sentinel connection (error=%d, sentinel=%s:%d): %s",
                            isentinel->context->c.err, isentinel->host, isentinel->port,
                            HIREDIS_ERRSTR((&isentinel->context->c)));
                        redisAsyncFree(isentinel->context);
                        isentinel->context = NULL;
                    }
#endif
                    if (isentinel->context != NULL) {
                        isentinel->context->data = isentinel;
                        redisLibevAttach(loop, isentinel->context);
                        redisAsyncSetConnectCallback(isentinel->context, connectCallback);
                        redisAsyncSetDisconnectCallback(isentinel->context, disconnectCallback);
                        if (state->password != NULL) {
                            if (redisAsyncCommand(
                                    isentinel->context, authorizeCallback, isentinel,
                                    "AUTH %s", state->password) != REDIS_OK) {
                                REDIS_LOG_ERROR(NULL,
                                    "Failed to enqueue asynchronous Sentinel AUTH command (error=%d, sentinel=%s:%d): %s",
                                    isentinel->host, isentinel->port,
                                    HIREDIS_ERRSTR(isentinel->context));
                                redisAsyncFree(isentinel->context);
                                isentinel->context = NULL;
                            }
                        }
                    }
                    if (isentinel->context != NULL) {
                        if (state->protocol != REDIS_PROTOCOL_DEFAULT) {
                            if (redisAsyncCommand(
                                    isentinel->context, helloCallback, isentinel,
                                    "HELLO %d", state->protocol) != REDIS_OK) {
                                REDIS_LOG_ERROR(NULL,
                                    "Failed to enqueue asynchronous Sentinel HELLO command (error=%d, sentinel=%s:%d): %s",
                                    isentinel->host, isentinel->port,
                                    HIREDIS_ERRSTR(isentinel->context));
                                redisAsyncFree(isentinel->context);
                                isentinel->context = NULL;
                            }
                        }
                    }
                    if (isentinel->context != NULL) {
                        if (redisAsyncCommand(
                                isentinel->context, subscribeCallback, isentinel,
                                SUBSCRIPTION_COMMAND) != REDIS_OK) {
                            REDIS_LOG_ERROR(NULL,
                                "Failed to enqueue asynchronous Sentinel subscription command (error=%d, sentinel=%s:%d): %s",
                                isentinel->host, isentinel->port,
                                HIREDIS_ERRSTR(isentinel->context));
                            redisAsyncFree(isentinel->context);
                            isentinel->context = NULL;
                        }
                    }
                } else {
                    if (isentinel->context != NULL) {
                        REDIS_LOG_ERROR(NULL,
                            "Failed to establish Sentinel connection (error=%d, sentinel=%s:%d): %s",
                            isentinel->context->err, isentinel->host,
                            isentinel->port, HIREDIS_ERRSTR(isentinel->context));
                        redisAsyncFree(isentinel->context);
                        isentinel->context = NULL;
                    } else {
                        REDIS_LOG_ERROR(NULL,
                            "Failed to establish Sentinel connection (sentinel=%s:%d)",
                            isentinel->host, isentinel->port);
                    }
                }
            }
        }

        // Look for pending Pub/Sub events, handle those events, update servers
        // and continue execution.
        ev_loop(loop, EVRUN_NOWAIT);

        // Only update database objects if a proactive discovery has been
        // explicitly requested or if some change was found during this check.
        Lck_Lock(&state->config->mutex);
        if ((state->config->sentinels.discovery) ||
            (state->last_change >= now)) {
            unsafe_update_dbs(state);
            state->config->sentinels.discovery = 0;
        }
        Lck_Unlock(&state->config->mutex);

        // Wait for the next check.
        usleep(1000000);
    }

    // Log event.
    Lck_Lock(&state->config->mutex);
    REDIS_LOG_INFO(NULL,
        "Sentinel thread stopped (locations=%s, period=%d)",
        state->config->sentinels.locations,
        state->config->sentinels.period);
    Lck_Unlock(&state->config->mutex);

    // Done!
    free_state(state);
    ev_loop_destroy(loop);
    return NULL;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static struct server *
new_server(
    struct sentinel *sentinel, const char *host, unsigned port,
    enum REDIS_SERVER_ROLE role, unsigned down)
{
    struct server *result;
    ALLOC_OBJ(result, SERVER_MAGIC);
    AN(result);

    result->host = strdup(host);
    AN(result->host);
    result->port = port;

    result->role = role;
    result->down = down;

    result->sentinel = sentinel;

    return result;
}

static void
free_server(struct server *server)
{
    CHECK_OBJ_NOTNULL(server, SERVER_MAGIC);

    free((void *) server->host);
    server->host = NULL;
    server->port = 0;

    server->role = REDIS_SERVER_TBD_ROLE;
    server->down = 0;

    server->sentinel = NULL;

    FREE_OBJ(server);
}

static struct sentinel *
new_sentinel(struct state *state, const char *host, unsigned host_len, unsigned port)
{
    struct sentinel *result;
    ALLOC_OBJ(result, SENTINEL_MAGIC);
    AN(result);

    result->host = strndup(host, host_len);
    AN(result->host);
    result->port = port;

    result->context = NULL;

    result->state = state;

    return result;
}

static void
free_sentinel(struct sentinel *sentinel)
{
    CHECK_OBJ_NOTNULL(sentinel, SENTINEL_MAGIC);

    free((void *) sentinel->host);
    sentinel->host = NULL;
    sentinel->port = 0;

    if (sentinel->context != NULL) {
        redisAsyncFree(sentinel->context);
        sentinel->context = NULL;
    }

    sentinel->state = NULL;

    FREE_OBJ(sentinel);
}

static struct state *
new_state(
    vcl_state_t *config, unsigned period, struct timeval connection_timeout,
    struct timeval command_timeout, enum REDIS_PROTOCOL protocol,
#ifdef TLS_ENABLED
    redisSSLContext *tls_ssl_ctx,
#endif
    const char *password)
{
    struct state *result;
    ALLOC_OBJ(result, STATE_MAGIC);
    AN(result);

    result->config = config;

    VTAILQ_INIT(&result->sentinels);
    result->period = period;
    result->connection_timeout = connection_timeout;
    result->command_timeout = command_timeout;
    result->protocol = protocol;
#ifdef TLS_ENABLED
    result->tls_ssl_ctx = tls_ssl_ctx;
#endif
    if (password != NULL) {
        result->password = strdup(password);
        AN(result->password);
    } else {
        result->password = NULL;
    }

    result->last_change = 0;
    result->next_discovery = 0;

    VTAILQ_INIT(&result->servers);

    return result;
}

static void
free_state(struct state *state)
{
    CHECK_OBJ_NOTNULL(state, STATE_MAGIC);

    state->config = NULL;

    struct sentinel *isentinel;
    while (!VTAILQ_EMPTY(&state->sentinels)) {
        isentinel = VTAILQ_FIRST(&state->sentinels);
        CHECK_OBJ_NOTNULL(isentinel, SENTINEL_MAGIC);
        VTAILQ_REMOVE(&state->sentinels, isentinel, list);
        free_sentinel(isentinel);
    }
    state->period = 0;
    state->connection_timeout = (struct timeval){ 0 };
    state->command_timeout = (struct timeval){ 0 };
    state->protocol = REDIS_PROTOCOL_DEFAULT;
#ifdef TLS_ENABLED
    if (state->tls_ssl_ctx != NULL) {
        redisFreeSSLContext(state->tls_ssl_ctx);
        state->tls_ssl_ctx = NULL;
    }
#endif
    if (state->password != NULL) {
        free((void *) state->password);
        state->password = NULL;
    }

    state->last_change = 0;
    state->next_discovery = 0;

    struct server *iserver;
    while (!VTAILQ_EMPTY(&state->servers)) {
        iserver = VTAILQ_FIRST(&state->servers);
        CHECK_OBJ_NOTNULL(iserver, SERVER_MAGIC);
        VTAILQ_REMOVE(&state->servers, iserver, list);
        free_server(iserver);
    }

    FREE_OBJ(state);
}

static void
unsafe_set_locations(struct state *state, const char *locations)
{
    // Initializations
    unsigned error = 0;

    // Parse input.
    const char *p = locations;
    while (*p != '\0') {
        // Parse host.
        while (isspace(*p)) p++;
        const char *q = p;
        while (*q != '\0' && *q != ':') {
            q++;
        }
        if ((p == q) || (*q != ':')) {
            error = 10;
            break;
        }
        const char *host = p;
        unsigned host_len = q - p;

        // Parse port.
        p = q + 1;
        if (!isdigit(*p)) {
            error = 20;
            break;
        }
        int port = strtoul(p, (char **)&q, 10);
        if ((p == q) || (port < 0) || (port > 65536)) {
            error = 30;
            break;
        }

        // Store parsed Sentinel.
        struct sentinel *sentinel = new_sentinel(state, host, host_len, port);
        VTAILQ_INSERT_TAIL(&state->sentinels, sentinel, list);

        // More items?
        p = q;
        while (isspace(*p) || (*p == ',')) p++;
    }

    // Check error flag.
    if (error) {
        // Release parsed Sentinels.
        struct sentinel *isentinel;
        while (!VTAILQ_EMPTY(&state->sentinels)) {
            isentinel = VTAILQ_FIRST(&state->sentinels);
            CHECK_OBJ_NOTNULL(isentinel, SENTINEL_MAGIC);
            VTAILQ_REMOVE(&state->sentinels, isentinel, list);
            free_sentinel(isentinel);
        }

        // Log error.
        REDIS_LOG_ERROR(NULL,
            "Got error while parsing Sentinels (error=%d, locations=%s)",
            error, locations);
    }
}

static void
store_sentinel_reply(
    struct sentinel *sentinel, const char *host, unsigned port,
    enum REDIS_SERVER_ROLE role, int down)
{
    // Initializations.
    struct server *server = NULL;

    // Search for server matching host & port.
    VTAILQ_FOREACH(server, &sentinel->state->servers, list) {
        CHECK_OBJ_NOTNULL(server, SERVER_MAGIC);
        if ((server->port == port) &&
            (strcmp(server->host, host) == 0)) {
            break;
        }
    }

    // Register / update server.
    if (server == NULL) {
        server = new_server(sentinel, host, port, role, down > 0);
        VTAILQ_INSERT_TAIL(&sentinel->state->servers, server, list);
        sentinel->state->last_change = time(NULL);
    } else if ((server->role != role) ||
               ((down >= 0) && (server->down != down))) {
        server->sentinel = sentinel;
        server->role = role;
        if (down >= 0) {
            server->down = down;
        }
        sentinel->state->last_change = time(NULL);
    }
}

static void
parse_sentinel_notification(struct sentinel *sentinel, redisReply *reply)
{
    // Check reply format.
    if ((reply != NULL) &&
        ((reply->type == REDIS_REPLY_ARRAY ||
          RESP3_SWITCH(reply->type == REDIS_REPLY_PUSH, 0))) &&
        (reply->elements == 4) &&
        (reply->element[0]->type == REDIS_REPLY_STRING) &&
        (strcmp(reply->element[0]->str, "pmessage") == 0) &&
        (reply->element[2]->type == REDIS_REPLY_STRING) &&
        (reply->element[3]->type == REDIS_REPLY_STRING)) {
        // Initializations.
        char *ctx, *ptr;
        const char *event = reply->element[2]->str;
        char *payload = strdup(reply->element[3]->str);
        AN(payload);

        // +sdown <instance type> <master name> <IP> <port> ...
        // -sdown <instance type> <master name> <IP> <port> ...
        // +odown <instance type> <master name> <IP> <port> ...
        // -odown <instance type> <master name> <IP> <port> ...
        if ((strcmp(event, "+sdown") == 0) ||
            (strcmp(event, "-sdown") == 0) ||
            (strcmp(event, "+odown") == 0) ||
            (strcmp(event, "-odown") == 0)) {
            // Extract <instance type>.
            enum REDIS_SERVER_ROLE role;
            ptr = strtok_r(payload, " ", &ctx);
            if (ptr != NULL) {
                if (strcmp(ptr, "master") == 0) {
                    role = REDIS_SERVER_MASTER_ROLE;
                } else if (strcmp(ptr, "slave") == 0) {
                    role = REDIS_SERVER_SLAVE_ROLE;
                } else {
                    goto stop;
                }
            } else {
                goto stop;
            }

            // Extract <master name>.
            ptr = strtok_r(NULL, " ", &ctx);
            if (ptr == NULL) {
                goto stop;
            }

            // Extract <IP>.
            ptr = strtok_r(NULL, " ", &ctx);
            const char *ip;
            if (ptr != NULL) {
                ip = ptr;
            } else {
                goto stop;
            }

            // Extract <port>.
            ptr = strtok_r(NULL, " ", &ctx);
            unsigned port;
            if (ptr != NULL) {
                port = atoi(ptr);
            } else {
                goto stop;
            }

            // Register / update server.
            store_sentinel_reply(sentinel, ip, port, role, event[0] == '+');

        // +switch-master <master name> <old IP> <old port> <new IP> <new port> ...
        } else if (strcmp(event, "+switch-master") == 0) {
            // Extract <master name>.
            ptr = strtok_r(payload, " ", &ctx);
            if (ptr == NULL) {
                goto stop;
            }

            // Extract <old IP>.
            ptr = strtok_r(NULL, " ", &ctx);
            const char *old_ip;
            if (ptr != NULL) {
                old_ip = ptr;
            } else {
                goto stop;
            }

            // Extract <old port>.
            ptr = strtok_r(NULL, " ", &ctx);
            unsigned old_port;
            if (ptr != NULL) {
                old_port = atoi(ptr);
            } else {
                goto stop;
            }

            // Extract <new IP>.
            ptr = strtok_r(NULL, " ", &ctx);
            const char *new_ip;
            if (ptr != NULL) {
                new_ip = ptr;
            } else {
                goto stop;
            }

            // Extract <new port>.
            ptr = strtok_r(NULL, " ", &ctx);
            unsigned new_port;
            if (ptr != NULL) {
                new_port = atoi(ptr);
            } else {
                goto stop;
            }

            // Register / update server.
            store_sentinel_reply(
                sentinel, old_ip, old_port,
                REDIS_SERVER_SLAVE_ROLE, -1);
            store_sentinel_reply(
                sentinel, new_ip, new_port,
                REDIS_SERVER_MASTER_ROLE, 0);
        }
stop:

        // Release payload.
        free(payload);
    }
}

static void
parse_sentinel_discovery(
    struct state *state, struct sentinel *sentinel,
    redisReply *reply, const char ***master_names)
{
    // Initializations.
    if (master_names != NULL) {
        *master_names = NULL;
    }

    // Check reply format.
    if (reply->type == REDIS_REPLY_ARRAY) {
        // Initializations.
        unsigned imaster_names = 0;
        if (master_names != NULL) {
            *master_names = malloc((reply->elements + 1) * sizeof(const char *));
            AN(*master_names);
            (*master_names)[0] = NULL;
        }

        // Check reply contents.
        const char *name, *value;
        for (int i = 0; i < reply->elements; i++) {
            if (reply->element[i]->type == REDIS_REPLY_ARRAY ||
                RESP3_SWITCH(reply->element[i]->type == REDIS_REPLY_MAP, 0)) {
                // Initializations.
                const char *master_name = NULL;
                const char *host = NULL;
                unsigned port = 0;
                enum REDIS_SERVER_ROLE role = REDIS_SERVER_TBD_ROLE;
                unsigned down = 0;

                // Look for relevant properties.
                for (int j = 0; j + 1 < reply->element[i]->elements; j += 2) {
                    if ((reply->element[i]->element[j]->type == REDIS_REPLY_STRING) &&
                        (reply->element[i]->element[j+1]->type == REDIS_REPLY_STRING)) {
                        name = reply->element[i]->element[j]->str;
                        value = reply->element[i]->element[j+1]->str;
                        if (strcmp(name, "name") == 0) {
                            master_name = value;
                        } else if (strcmp(name, "ip") == 0) {
                            host = value;
                        } else if (strcmp(name, "port") == 0) {
                            port = atoi(value);
                        } else if (strcmp(name, "flags") == 0) {
                            if (strstr(value, "master") != NULL) {
                                role = REDIS_SERVER_MASTER_ROLE;
                            }
                            if (strstr(value, "slave") != NULL) {
                                role = REDIS_SERVER_SLAVE_ROLE;
                            }
                            if ((strstr(value, "s_down") != NULL) ||
                                (strstr(value, "o_down") != NULL)) {
                                down = 1;
                            }
                        }
                    }
                }

                // Insert in the list of discovered master names?
                if ((master_name != NULL) && (master_names != NULL)) {
                    (*master_names)[imaster_names++] = master_name;
                    (*master_names)[imaster_names] = NULL;
                }

                // Register / update server if all required properties have
                // been found.
                if ((host != NULL) &&
                    (port > 0) &&
                    (role != REDIS_SERVER_TBD_ROLE)) {
                    store_sentinel_reply(sentinel, host, port, role, down);
                }
            }
        }
    } else {
        REDIS_LOG_ERROR(NULL,
            "Unexpected Sentinel discovery command reply (type=%d, sentinel=%s:%d)",
            reply->type, sentinel->host, sentinel->port);
    }
}

static void
discover_servers(struct state *state)
{
    // Query all registered Sentinels.
    struct sentinel *isentinel;
    VTAILQ_FOREACH(isentinel, &state->sentinels, list) {
        // Assertions.
        CHECK_OBJ_NOTNULL(isentinel, SENTINEL_MAGIC);

        // Create context.
        redisContext *rcontext;
        if ((state->connection_timeout.tv_sec > 0) ||
            (state->connection_timeout.tv_usec > 0)) {
            rcontext = redisConnectWithTimeout(
                isentinel->host,
                isentinel->port,
                state->connection_timeout);
        } else {
            rcontext = redisConnect(
                isentinel->host,
                isentinel->port);
        }
        if (rcontext == NULL) {
            REDIS_LOG_ERROR(NULL,
                "Failed to establish Sentinel connection (sentinel=%s:%d)",
                isentinel->host, isentinel->port);
        } else if (rcontext->err) {
            REDIS_LOG_ERROR(NULL,
                "Failed to establish Sentinel connection (error=%d, sentinel=%s:%d): %s",
                rcontext->err, isentinel->host,
                isentinel->port, HIREDIS_ERRSTR(rcontext));
            redisFree(rcontext);
            rcontext = NULL;
        }

#ifdef TLS_ENABLED
        // Setup TLS.
        if ((rcontext != NULL) &&
            (state->tls_ssl_ctx != NULL) &&
            (redisInitiateSSLWithContext(rcontext, state->tls_ssl_ctx) != REDIS_OK)) {
            REDIS_LOG_ERROR(NULL,
                "Failed to secure Sentinel connection (error=%d, sentinel=%s:%d): %s",
                rcontext->err, isentinel->host, isentinel->port,
                HIREDIS_ERRSTR(rcontext));
            redisFree(rcontext);
            rcontext = NULL;
        }
#endif

        // Send 'AUTH' command.
        if ((rcontext != NULL) &&
            (state->password != NULL)) {
            redisReply *reply = redisCommand(rcontext, "AUTH %s", state->password);
            if ((rcontext->err) ||
                (reply == NULL) ||
                (reply->type != REDIS_REPLY_STATUS) ||
                (strcmp(reply->str, "OK") != 0)) {
                REDIS_LOG_ERROR(NULL,
                    "Failed to execute Sentinel AUTH command (error=%d, sentinel=%s:%d): %s",
                    rcontext->err, isentinel->host, isentinel->port,
                    HIREDIS_ERRSTR(rcontext, reply));
                redisFree(rcontext);
                rcontext = NULL;
            }
        }

        // Send 'HELLO' command.
        if ((rcontext != NULL) &&
            (state->protocol != REDIS_PROTOCOL_DEFAULT)) {
            redisReply *reply = redisCommand(rcontext, "HELLO %d", state->protocol);
            if ((rcontext->err) ||
                (reply == NULL) ||
                (reply->type != REDIS_REPLY_ARRAY &&
                 RESP3_SWITCH(reply->type != REDIS_REPLY_MAP, 1))
               ) {
                REDIS_LOG_ERROR(NULL,
                    "Failed to execute Sentinel HELLO command (error=%d, sentinel=%s:%d): %s",
                    rcontext->err, isentinel->host, isentinel->port,
                    HIREDIS_ERRSTR(rcontext, reply));
                redisFree(rcontext);
                rcontext = NULL;
            }
        }

        // Check context.
        if (rcontext != NULL) {
            // Set command execution timeout.
            int tr = redisSetTimeout(rcontext, state->command_timeout);
            if (tr != REDIS_OK) {
                REDIS_LOG_ERROR(NULL,
                    "Failed to set Sentinel command execution timeout (error=%d, sentinel=%s:%d)",
                    tr, isentinel->host, isentinel->port);
            }

            // Send 'SENTINEL masters' command in order to get a list of
            // monitored masters and their state.
            const char **master_names = NULL;
            redisReply *reply1 = redisCommand(rcontext, "SENTINEL masters");
            if (reply1 != NULL) {
                parse_sentinel_discovery(state, isentinel, reply1, &master_names);

                // Send 'SENTINEL slaves <master name>' command for each
                // discovered master name in order to get the list of
                // monitored slaves and their state.
                if (master_names != NULL) {
                    for (int i = 0; master_names[i] != NULL ; i++) {
                        if (!rcontext->err) {
                            redisReply *reply2 = redisCommand(rcontext, "SENTINEL slaves %s", master_names[i]);
                            if (reply2 != NULL) {
                                parse_sentinel_discovery(state, isentinel, reply2, NULL);
                                freeReplyObject(reply2);
                            } else {
                                REDIS_LOG_ERROR(NULL,
                                    "Failed to execute Sentinel slaves command (error=%s, master_name=%s, sentinel=%s:%d): %s",
                                    rcontext->err, master_names[i], isentinel->host, isentinel->port,
                                    HIREDIS_ERRSTR(rcontext));
                            }
                        } else {
                            REDIS_LOG_ERROR(NULL,
                                "Failed to reuse Sentinel connection (error=%d, sentinel=%s:%d): %s",
                                rcontext->err, isentinel->host,
                                isentinel->port, HIREDIS_ERRSTR(rcontext));
                            break;
                        }
                    }
                    free(master_names);
                }

                freeReplyObject(reply1);
            } else {
                REDIS_LOG_ERROR(NULL,
                    "Failed to execute Sentinel masters command (error=%d, sentinel=%s:%d): %s",
                    rcontext->err, isentinel->host, isentinel->port,
                    HIREDIS_ERRSTR(rcontext));
            }

            // Release context.
            redisFree(rcontext);
        }
    }
}

static void
unsafe_update_dbs_aux(struct state *state, redis_server_t *server)
{
    // Assertions.
    Lck_AssertHeld(&state->config->mutex);
    Lck_AssertHeld(&server->db->mutex);

    // Look for a discovered server matching this one.
    struct server *is;
    VTAILQ_FOREACH(is, &state->servers, list) {
        CHECK_OBJ_NOTNULL(is, SERVER_MAGIC);
        if ((server->location.parsed.address.port == is->port) &&
            (strcmp(server->location.parsed.address.host, is->host) == 0)) {
            // Change role?
            if (server->role != is->role) {
                VTAILQ_REMOVE(
                    &server->db->servers[server->weight][server->role],
                    server,
                    list);
                server->role = is->role;
                VTAILQ_INSERT_TAIL(
                    &server->db->servers[server->weight][server->role],
                    server,
                    list);
                REDIS_LOG_INFO(NULL,
                    "Server role updated (db=%s, server=%s, sentinel=%s:%d, role=%d)",
                    server->db->name, server->location.raw,
                    is->sentinel->host, is->sentinel->port,
                    is->role);
            }

            // Change sickness flag?
            unsigned now = time(NULL);
            if (server->sickness.exp <= now) {
                if (is->down) {
                    server->sickness.exp = UINT_MAX;
                    REDIS_LOG_INFO(NULL,
                        "Server sickness tag set (db=%s, server=%s, sentinel=%s:%d)",
                        server->db->name, server->location.raw,
                        is->sentinel->host, is->sentinel->port);
                }
            } else {
                if (!is->down) {
                    server->sickness.exp = now;
                    REDIS_LOG_INFO(NULL,
                        "Server sickness tag cleared (db=%s, server=%s, sentinel=%s:%d)",
                        server->db->name, server->location.raw,
                        is->sentinel->host, is->sentinel->port);
                }
            }

            // Found!
            break;
        }
    }
}

static void
unsafe_update_dbs(struct state *state)
{
    // Assertions.
    Lck_AssertHeld(&state->config->mutex);

    // Look for servers matching servers previously discovered by Sentinel.
    database_t *idb;
    VTAILQ_FOREACH(idb, &state->config->dbs, list) {
        CHECK_OBJ_NOTNULL(idb, DATABASE_MAGIC);
        if (!idb->db->cluster.enabled) {
            Lck_Lock(&idb->db->mutex);
            for (unsigned iweight = 0; iweight < NREDIS_SERVER_WEIGHTS; iweight++) {
                for (enum REDIS_SERVER_ROLE irole = 0; irole < NREDIS_SERVER_ROLES; irole++) {
                    redis_server_t *iserver, *iserver_tmp;
                    VTAILQ_FOREACH_SAFE(iserver, &(idb->db->servers[iweight][irole]), list, iserver_tmp) {
                        CHECK_OBJ_NOTNULL(iserver, REDIS_SERVER_MAGIC);
                        if (iserver->location.type == REDIS_SERVER_LOCATION_HOST_TYPE) {
                            unsafe_update_dbs_aux(state, iserver);
                        }
                    }
                }
            }
            Lck_Unlock(&idb->db->mutex);
        }
    }
}
