#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <hiredis/hiredis.h>
#include <arpa/inet.h>

#include "vrt.h"
#include "cache/cache.h"

#include "core.h"
#include "sentinel.h"

#define MASTER_FLAG_NAME "master"
#define MASTER_FLAG_MASK (1 << 0)

#define SLAVE_FLAG_NAME "slave"
#define SLAVE_FLAG_MASK (1 << 1)

#define S_DOWN_FLAG_NAME "s_down"
#define S_DOWN_FLAG_MASK (1 << 2)

#define O_DOWN_FLAG_NAME "o_down"
#define O_DOWN_FLAG_MASK (1 << 3)

#define FAILOVER_IN_PROGRESS_FLAG_NAME "failover_in_progress"
#define FAILOVER_IN_PROGRESS_FLAG_MASK (1 << 4)

#define RECONF_INPROG_FLAG_NAME "reconf_inprog"
#define RECONF_INPROG_FLAG_MASK (1 << 5)

struct server {
    // Object marker.
#define SERVER_MAGIC 0x762a900c
    unsigned magic;

    // Properties found during the last Sentinel discovery.
    // When multiple Sentinels have inconsistent information about a server,
    // properties returned by the last queried Sentinel will be used.
    struct sentinel *sentinel;
    const char *host;
    unsigned port;
    unsigned flags;

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

    // Next periodical discovery.
    time_t next_discovery;

    // Discovered servers.
    VTAILQ_HEAD(,server) servers;
};

static void *sentinel_loop(void *object);

static struct state *new_state(
    vcl_state_t *config, unsigned period, struct timeval connection_timeout,
    struct timeval command_timeout);
static void free_state(struct state *state);

static void unsafe_set_locations(struct state *state, const char *locations);

static void update_state(struct state *state);

static unsigned unsafe_update_dbs(struct state *state, time_t now);

void
unsafe_sentinel_start(vcl_state_t *config)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    AN(config->sentinels.locations);
    assert(config->sentinels.period > 0);
    AZ(config->sentinels.thread);
    AZ(config->sentinels.active);

    // Try to start new thread and launch initial discovery.
    struct state *state = new_state(
            config,
            config->sentinels.period,
            config->sentinels.connection_timeout,
            config->sentinels.command_timeout);
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
    assert(config->sentinels.period > 0);
    AN(config->sentinels.thread);
    AN(config->sentinels.active);

    // Request Sentinel discovery.
    config->sentinels.discovery = 1;
}

void
unsafe_sentinel_stop(vcl_state_t *config)
{
    // Assertions.
    Lck_AssertHeld(&config->mutex);
    AN(config->sentinels.locations);
    assert(config->sentinels.period > 0);
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

static void*
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

    // Thread loop.
    while (1) {
        // Assertions.
        CHECK_OBJ_NOTNULL(state, STATE_MAGIC);
        CHECK_OBJ_NOTNULL(state->config, VCL_STATE_MAGIC);

        // Initializations.
        time_t now = time(NULL);

        // Terminate the thread loop?
        Lck_Lock(&state->config->mutex);
        if (!state->config->sentinels.active) {
            Lck_Unlock(&state->config->mutex);
            break;
        }

        // Is time to execute a new discovery?
        // XXX: simple polling-based implementation using information provided
        // by the last contacted Sentinel server. To be explored a better
        // implementation based on pub/sub.
        if ((state->config->sentinels.discovery) ||
            (state->next_discovery <= now)) {
            // The config->mutex lock is not needed while querying Sentinels.
            Lck_Unlock(&state->config->mutex);

            // Query all Sentinels in order to update the internal state
            // of the thread.
            update_state(state);

            // Terminate the thread loop?
            Lck_Lock(&state->config->mutex);
            if (!state->config->sentinels.active) {
                Lck_Unlock(&state->config->mutex);
                break;
            }

            // Update databases.
            // XXX: this is a lot of work to be done on every discovery with
            // config->mutex locked. Not sure how terrible this would be in
            // a realistic setup.
            state->config->sentinels.discovery = 0;
            state->next_discovery = now + unsafe_update_dbs(state, now);
            Lck_Unlock(&state->config->mutex);
        } else {
            Lck_Unlock(&state->config->mutex);
        }

        // Wait for the next check.
        usleep(1000);
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
    return NULL;
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static struct server *
new_server(struct sentinel *sentinel, const char *host, unsigned port, unsigned flags)
{
    struct server *result;
    ALLOC_OBJ(result, SERVER_MAGIC);
    AN(result);

    result->sentinel = sentinel;
    result->host = strdup(host);
    AN(result->host);
    result->port = port;
    result->flags = flags;

    return result;
}

static void
free_server(struct server *server)
{
    server->sentinel = NULL;
    free((void *) server->host);
    server->host = NULL;
    server->port = 0;
    server->flags = 0;

    FREE_OBJ(server);
}

static struct sentinel *
new_sentinel(const char *host, unsigned port)
{
    struct sentinel *result;
    ALLOC_OBJ(result, SENTINEL_MAGIC);
    AN(result);

    result->host = strdup(host);
    AN(result->host);
    result->port = port;

    return result;
}

static void
free_sentinel(struct sentinel *sentinel)
{
    free((void *) sentinel->host);
    sentinel->host = NULL;
    sentinel->port = 0;

    FREE_OBJ(sentinel);
}

static struct state *
new_state(
    vcl_state_t *config, unsigned period, struct timeval connection_timeout,
    struct timeval command_timeout)
{
    struct state *result;
    ALLOC_OBJ(result, STATE_MAGIC);
    AN(result);

    result->config = config;
    VTAILQ_INIT(&result->sentinels);
    result->period = period;
    result->connection_timeout = connection_timeout;
    result->command_timeout = command_timeout;
    result->next_discovery = 0;
    VTAILQ_INIT(&result->servers);

    return result;
}

static void
free_state(struct state *state)
{
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
        // Parse IP.
        char ip[32];
        while (isspace(*p)) p++;
        const char *q = p;
        while (*q != '\0' && *q != ':') {
            q++;
        }
        if ((p == q) || (*q != ':') || (q - p >= sizeof(ip))) {
            error = 10;
            break;
        }
        memcpy(ip, p, q - p);
        ip[q - p] = '\0';
        struct in_addr ia4;
        if (inet_pton(AF_INET, ip, &ia4) == 0) {
            error = 20;
            break;
        }

        // Parse port.
        p = q + 1;
        if (!isdigit(*p)) {
            error = 30;
            break;
        }
        int port = strtoul(p, (char **)&q, 10);
        if ((p == q) || (port < 0) || (port > 65536)) {
            error = 40;
            break;
        }

        // Store parsed Sentinel.
        struct sentinel *sentinel = new_sentinel(ip, port);
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
    struct state *state, struct sentinel *sentinel,
    const char *host, unsigned port, unsigned flags)
{
    // Initializations.
    struct server *server = NULL;

    // Search for server matching host & port.
    VTAILQ_FOREACH(server, &state->servers, list) {
        CHECK_OBJ_NOTNULL(server, SERVER_MAGIC);
        if ((server->port == port) &&
            (strcmp(server->host, host) == 0)) {
            break;
        }
    }

    // Register / update server.
    if (server == NULL) {
        server = new_server(sentinel, host, port, flags);
        VTAILQ_INSERT_TAIL(&state->servers, server, list);
    } else {
        server->sentinel = sentinel;
        server->flags = flags;
    }
}

static void
parse_sentinel_reply(
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
            if (reply->element[i]->type == REDIS_REPLY_ARRAY) {
                // Initializations.
                const char *master_name = NULL;
                const char *host = NULL;
                unsigned port = 0;
                unsigned flags = 0;

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
                            port = strtoul(value, NULL, 10);
                        } else if (strcmp(name, "flags") == 0) {
                            if (strstr(value, MASTER_FLAG_NAME) != NULL) {
                                flags = flags | MASTER_FLAG_MASK;
                            }
                            if (strstr(value, SLAVE_FLAG_NAME) != NULL) {
                                flags = flags | SLAVE_FLAG_MASK;
                            }
                            if (strstr(value, S_DOWN_FLAG_NAME) != NULL) {
                                flags = flags | S_DOWN_FLAG_MASK;
                            }
                            if (strstr(value, O_DOWN_FLAG_NAME) != NULL) {
                                flags = flags | O_DOWN_FLAG_MASK;
                            }
                            if (strstr(value, FAILOVER_IN_PROGRESS_FLAG_NAME) != NULL) {
                                flags = flags | FAILOVER_IN_PROGRESS_FLAG_MASK;
                            }
                            if (strstr(value, RECONF_INPROG_FLAG_NAME) != NULL) {
                                flags = flags | RECONF_INPROG_FLAG_MASK;
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
                    (flags & (MASTER_FLAG_MASK | SLAVE_FLAG_MASK))) {
                    store_sentinel_reply(state, sentinel, host, port, flags);
                }
            }
        }
    }
}

static void
update_state(struct state *state)
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
        AN(rcontext);

        // Check context.
        if (!rcontext->err) {
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
                parse_sentinel_reply(state, isentinel, reply1, &master_names);

                // Send 'SENTINEL slaves <master name>' command for each
                // discovered master name in order to get the list of
                // monitored slaves and their state.
                if (master_names != NULL) {
                    for (int i = 0; master_names[i] != NULL ; i++) {
                        if (!rcontext->err) {
                            redisReply *reply2 = redisCommand(rcontext, "SENTINEL slaves %s", master_names[i]);
                            if (reply2 != NULL) {
                                parse_sentinel_reply(state, isentinel, reply2, NULL);
                                freeReplyObject(reply2);
                            } else {
                                REDIS_LOG_ERROR(NULL,
                                    "Failed to execute Sentinel slaves command (master_name=%s, sentinel=%s:%d)",
                                    master_names[i], isentinel->host, isentinel->port);
                            }
                        } else {
                            REDIS_LOG_ERROR(NULL,
                                "Failed to reuse Sentinel connection (error=%d, sentinel=%s:%d): %s",
                                rcontext->err, isentinel->host,
                                isentinel->port, rcontext->errstr);
                            break;
                        }
                    }
                    free(master_names);
                }

                freeReplyObject(reply1);
            } else {
                REDIS_LOG_ERROR(NULL,
                    "Failed to execute Sentinel masters command (sentinel=%s:%d)",
                    isentinel->host, isentinel->port);
            }
        } else {
            REDIS_LOG_ERROR(NULL,
                "Failed to establish Sentinel connection (error=%d, sentinel=%s:%d): %s",
                rcontext->err, isentinel->host,
                isentinel->port, rcontext->errstr);
        }

        // Release context.
        redisFree(rcontext);
    }
}

static unsigned
unsafe_update_dbs_aux(struct state *state, redis_server_t *server, time_t now)
{
    // Assertions.
    Lck_AssertHeld(&state->config->mutex);
    Lck_AssertHeld(&server->db->mutex);

    // Initializations.
    unsigned result = state->period;

    // Look for a discovered server matching this one.
    struct server *is;
    VTAILQ_FOREACH(is, &state->servers, list) {
        CHECK_OBJ_NOTNULL(is, SERVER_MAGIC);
        if ((server->location.parsed.address.port == is->port) &&
            (strcmp(server->location.parsed.address.host, is->host) == 0)) {
            // Change role?
            enum REDIS_SERVER_ROLE role;
            if (is->flags & MASTER_FLAG_MASK) {
                role = REDIS_SERVER_MASTER_ROLE;
            } else {
                AN(is->flags & SLAVE_FLAG_MASK);
                role = REDIS_SERVER_SLAVE_ROLE;
            }
            if (server->role != role) {
                VTAILQ_REMOVE(
                    &server->db->servers[server->weight][server->role],
                    server,
                    list);
                server->role = role;
                VTAILQ_INSERT_TAIL(
                    &server->db->servers[server->weight][server->role],
                    server,
                    list);
                REDIS_LOG_INFO(NULL,
                    "Server role updated (db=%s, server=%s, sentinel=%s:%d, role=%d)",
                    server->db->name, server->location.raw,
                    is->sentinel->host, is->sentinel->port,
                    role);
            }

            // Change sickness flag?
            //   - The ODOWN condition only applies to masters. For other kind
            //   of instances Sentinel doesn't require to act, so the ODOWN
            //   state is never reached for slaves and other sentinels, but only
            //   SDOWN is.
            //   - However SDOWN has also semantic implications. For example a
            //   slave in SDOWN state is not selected to be promoted by a
            //   Sentinel performing a failover.
            unsigned sick = is->flags & S_DOWN_FLAG_MASK;
            if (server->sickness.exp <= now) {
                if (sick) {
                    server->sickness.exp = UINT_MAX;
                    REDIS_LOG_INFO(NULL,
                        "Server sickness tag set (db=%s, server=%s, sentinel=%s:%d)",
                        server->db->name, server->location.raw,
                        is->sentinel->host, is->sentinel->port);
                }
            } else {
                if (!sick) {
                    server->sickness.exp = now;
                    REDIS_LOG_INFO(NULL,
                        "Server sickness tag cleared (db=%s, server=%s, sentinel=%s:%d)",
                        server->db->name, server->location.raw,
                        is->sentinel->host, is->sentinel->port);
                }
            }

            // Schedule next discovery?
            if (is->flags &
                (FAILOVER_IN_PROGRESS_FLAG_MASK | RECONF_INPROG_FLAG_MASK)) {
                REDIS_LOG_INFO(NULL,
                    "Early Sentinel discovery scheduled (db=%s, server=%s, sentinel=%s:%d)",
                    server->db->name, server->location.raw,
                    is->sentinel->host, is->sentinel->port);
                return 1;
            }

            // Found!
            break;
        }
    }

    // Done!
    return result;
}

static unsigned
unsafe_update_dbs(struct state *state, time_t now)
{
    // Assertions.
    Lck_AssertHeld(&state->config->mutex);

    // Initializations.
    unsigned result = state->period;

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
                            unsigned period = unsafe_update_dbs_aux(state, iserver, now);
                            if (period < result) {
                                result = period;
                            }
                        }
                    }
                }
            }
            Lck_Unlock(&idb->db->mutex);
        }
    }

    // Done!
    return result;
}
