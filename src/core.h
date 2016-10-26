#ifndef CORE_H_INCLUDED
#define CORE_H_INCLUDED

#include <syslog.h>
#include <pthread.h>
#include <hiredis/hiredis.h>
#include <netinet/in.h>

#include "vqueue.h"

#define NREDIS_SERVER_ROLES 3
#define NREDIS_SERVER_WEIGHTS 4
#define NREDIS_CLUSTER_SLOTS 16384

// Required lock ordering to avoid deadlocks:
//   1. vcl_state->mutex.
//   2. vmod_redis_db->mutex.

// WARNING: ordering of roles in this enumeration is relevant when populating
// an execution plan.
enum REDIS_SERVER_ROLE {
    REDIS_SERVER_SLAVE_ROLE = 0,
    REDIS_SERVER_MASTER_ROLE = 1,
    REDIS_SERVER_TBD_ROLE = 2
};

enum REDIS_SERVER_LOCATION_TYPE {
    REDIS_SERVER_LOCATION_HOST_TYPE,
    REDIS_SERVER_LOCATION_SOCKET_TYPE
};

typedef struct redis_server {
    // Object marker.
#define REDIS_SERVER_MAGIC 0xac587b11
    unsigned magic;

    // Database.
    struct vmod_redis_db *db;

    // Location (allocated in the heap).
    struct {
        const char *raw;
        enum REDIS_SERVER_LOCATION_TYPE type;
        union {
            struct {
                const char *host;
                unsigned port;
            } address;
            const char *path;
        } parsed;
    } location;

    // Role (rw field to be protected by db->mutex).
    enum REDIS_SERVER_ROLE role;

    // Weight.
    unsigned weight;

    // Shared pool.
    struct {
        // Condition variable.
        pthread_cond_t cond;

        // Contexts (rw fields -allocated in the heap- to be protected by
        // db->mutex and the associated condition variable).
        unsigned ncontexts;
        VTAILQ_HEAD(,redis_context) free_contexts;
        VTAILQ_HEAD(,redis_context) busy_contexts;
    } pool;

    // Redis Cluster state (rw fields to be protected by db->mutex).
    struct {
        unsigned slots[NREDIS_CLUSTER_SLOTS];
    } cluster;

    // Sickness timestamps (rw fields to be protected by db->mutex): last time
    // the server was flagged as sick, and expiration of the last sickness
    // condition.
    struct {
        time_t tst;
        time_t exp;
    } sickness;

    // Tail queue.
    VTAILQ_ENTRY(redis_server) list;
} redis_server_t;

typedef struct redis_context {
    // Object marker.
#define REDIS_CONTEXT_MAGIC 0xe11eaa70
    unsigned magic;

    // Server.
    redis_server_t *server;

    // Data (allocated in the heap).
    redisContext *rcontext;
    unsigned version;
    time_t tst;

    // Tail queue.
    VTAILQ_ENTRY(redis_context) list;
} redis_context_t;

struct vcl_state;
typedef struct vcl_state vcl_state_t;

struct vmod_redis_db {
    // Object marker.
    unsigned magic;
#define VMOD_REDIS_DATABASE_MAGIC 0xef35182b

    // Mutex.
    struct lock mutex;

    // Configuration.
    // XXX: required because PRIV_VCL pointers are not available when the
    // VMOD releases database instances. This should be fixed in future
    // Varnish releases.
    vcl_state_t *config;

    // General options (allocated in the heap).
    const char *name;
    struct timeval connection_timeout;
    unsigned connection_ttl;
    struct timeval command_timeout;
    unsigned max_command_retries;
    unsigned shared_connections;
    unsigned max_connections;
    const char *password;
    time_t sickness_ttl;
    unsigned ignore_slaves;

    // Redis servers (rw field -allocated in the heap- to be protected by the
    // associated mutex), clustered by weight & role.
    VTAILQ_HEAD(,redis_server) servers[NREDIS_SERVER_WEIGHTS][NREDIS_SERVER_ROLES];

    // Redis Cluster options.
    struct {
        unsigned enabled;
        unsigned max_hops;
    } cluster;

    // Stats (rw fields to be protected by the associated mutex).
    struct stats {
        struct {
            // Number of successfully created servers.
            unsigned total;
            // Number of failures while trying to create new servers.
            unsigned failed;
        } servers;

        struct {
            // Number of successfully created connections.
            unsigned total;
            // Number of failures while trying to create new connections.
            unsigned failed;
            // Number of (established and probably healthy) connections dropped.
            struct {
                unsigned error;
                unsigned hung_up;
                unsigned overflow;
                unsigned ttl;
                unsigned version;
                unsigned sick;
            } dropped;
        } connections;

        struct {
            // Number of times some worker thread have been blocked waiting for
            // a free connection.
            unsigned blocked;
        } workers;

        struct {
            // Number of successfully executed commands (this includes Redis
            // error replies).
            unsigned total;
            // Number of failed command executions (this does not include Redis
            // error replies). If retries have been requested, each failed try
            // is considered as a separate command.
            unsigned failed;
            // Number of retried command executions (this includes both
            // successful and failed executions).
            unsigned retried;
            // Number of successfully executed commands returning a Redis error
            // reply.
            unsigned error;
            // Number of NOSCRIPT error replies while executing EVALSHA
            // commands.
            unsigned noscript;
        } commands;

        struct {
            struct {
                // Number of successfully executed discoveries.
                unsigned total;
                // Number of failed discoveries (this includes connection
                // failures, unexpected responses, etc.).
                unsigned failed;
            } discoveries;
            struct {
                // Number of MOVED replies.
                unsigned moved;
                // Number of ASK replies.
                unsigned ask;
            } replies;
        } cluster;
    } stats;
};

typedef struct task_state {
    // Object marker.
#define TASK_STATE_MAGIC 0xa6bc103e
    unsigned magic;

    // Private contexts (allocated in the heap).
    unsigned ncontexts;
    VTAILQ_HEAD(,redis_context) contexts;

    // Redis command:
    //   - Database.
    //   - Arguments (allocated in the session workspace).
    //   - Reply (allocated in the heap).
#define MAX_REDIS_COMMAND_ARGS 128
    struct {
        struct vmod_redis_db *db;
        struct timeval timeout;
        unsigned max_retries;
        unsigned argc;
        const char *argv[MAX_REDIS_COMMAND_ARGS];
        redisReply *reply;
    } command;
} task_state_t;

typedef struct subnet {
    // Object marker.
#define SUBNET_MAGIC 0x27facd57
    unsigned magic;

    // Weight.
    unsigned weight;

    // Address and mask stored in unsigned 32 bit variables (in_addr.s_addr)
    // using host byte oder.
    // XXX: only IPv4 subnets supported.
    struct in_addr address;
    struct in_addr mask;

    // Tail queue.
    VTAILQ_ENTRY(subnet) list;
} subnet_t;

typedef struct database {
    // Object marker.
#define DATABASE_MAGIC 0x9200fda1
    unsigned magic;

    // Database.
    struct vmod_redis_db *db;

    // Tail queue.
    VTAILQ_ENTRY(database) list;
} database_t;

struct vcl_state {
    // Object marker.
#define VCL_STATE_MAGIC 0x77feec11
    unsigned magic;

    // Mutex.
    struct lock mutex;

    // Subnets (rw field to be protected by the associated mutex).
    VTAILQ_HEAD(,subnet) subnets;

    // Databases (rw field to be protected by the associated mutex).
    VTAILQ_HEAD(,database) dbs;

    // Sentinel (rw fields to be protected by the associated mutex).
    struct {
        // Raw configuration.
        const char *locations;
        unsigned period;
        struct timeval connection_timeout;
        struct timeval command_timeout;

        // Thread reference + shared state.
        pthread_t thread;
        unsigned active;
        unsigned discovery;
    } sentinels;
};

typedef struct vmod_state {
    // Mutex.
    pthread_mutex_t mutex;

    // Version increased on every VCL warm event (rw field protected by the
    // associated mutex on writes; it's ok to ignore the lock during reads).
    // This will be used to (1) reestablish connections binded to worker
    // threads; and (2) regenerate pooled connections shared between threads.
    unsigned version;

    // Varnish locks.
    struct {
        unsigned refs;
        struct VSC_C_lck *config;
        struct VSC_C_lck *db;
    } locks;
} vmod_state_t;

extern vmod_state_t vmod_state;

#define REDIS_LOG(ctx, level, message, ...) \
    do { \
        const struct vrt_ctx *_ctx = ctx; \
        \
        char *_buffer; \
        if (level == LOG_ERR) { \
            assert(asprintf( \
                &_buffer, \
                "[REDIS][%s] %s", __func__, message) > 0); \
        } else { \
            assert(asprintf( \
                &_buffer, \
                "[REDIS] %s", message) > 0); \
        } \
        \
        syslog(level, _buffer, ##__VA_ARGS__); \
        \
        unsigned _tag; \
        if (level == LOG_ERR) { \
            _tag = SLT_VCL_Error; \
        } else { \
            _tag = SLT_VCL_Log; \
        } \
        if ((_ctx != NULL) && (_ctx->vsl != NULL)) { \
            VSLb(_ctx->vsl, _tag, _buffer, ##__VA_ARGS__); \
        } else { \
            VSL(_tag, 0, _buffer, ##__VA_ARGS__); \
        } \
        \
        free(_buffer); \
    } while (0)

#define REDIS_LOG_ERROR(ctx, message, ...) \
    REDIS_LOG(ctx, LOG_ERR, message, ##__VA_ARGS__)
#define REDIS_LOG_WARNING(ctx, message, ...) \
    REDIS_LOG(ctx, LOG_WARNING, message, ##__VA_ARGS__)
#define REDIS_LOG_INFO(ctx, message, ...) \
    REDIS_LOG(ctx, LOG_INFO, message, ##__VA_ARGS__)

#define REDIS_AUTH(ctx, rcontext, password, message1, message2, ...) \
    do { \
        redisReply *reply = redisCommand(rcontext, "AUTH %s", password); \
        \
        if ((rcontext->err) || \
            (reply == NULL) || \
            (reply->type != REDIS_REPLY_STATUS) || \
            (strcmp(reply->str, "OK") != 0)) { \
            if (rcontext->err) { \
                REDIS_LOG_ERROR(ctx, \
                    message1 " (error=%d, " message2 "): %s", \
                    rcontext->err, ##__VA_ARGS__, rcontext->errstr); \
            } else if ((reply != NULL) && \
                       ((reply->type == REDIS_REPLY_ERROR) || \
                        (reply->type == REDIS_REPLY_STATUS) || \
                        (reply->type == REDIS_REPLY_STRING))) { \
                REDIS_LOG_ERROR(ctx, \
                    message1 " (error=%d, " message2 "): %s", \
                    reply->type, ##__VA_ARGS__, reply->str); \
            } else { \
                REDIS_LOG_ERROR(ctx, \
                    message1 " (" message2 ")", \
                    ##__VA_ARGS__); \
            } \
            redisFree(rcontext); \
            rcontext = NULL; \
        } \
         \
        if (reply != NULL) {  \
            freeReplyObject(reply);  \
        } \
    } while (0)

redis_server_t *new_redis_server(
    struct vmod_redis_db *db, const char *location, enum REDIS_SERVER_ROLE role);
void free_redis_server(redis_server_t *server);

redis_context_t *new_redis_context(
    redis_server_t *server, redisContext *rcontext, time_t tst);
void free_redis_context(redis_context_t *context);

struct vmod_redis_db *new_vmod_redis_db(
    vcl_state_t *config, const char *name, struct timeval connection_timeout,
    unsigned connection_ttl, struct timeval command_timeout, unsigned max_command_retries,
    unsigned shared_connections, unsigned max_connections, const char *password,
    unsigned sickness_ttl, unsigned ignore_slaves, unsigned clustered,
    unsigned max_cluster_hops);
void free_vmod_redis_db(struct vmod_redis_db *db);

task_state_t *new_task_state();
void free_task_state(task_state_t *state);

vcl_state_t *new_vcl_state();
void free_vcl_state(vcl_state_t *priv);

subnet_t *new_subnet(unsigned weight, struct in_addr ia4, unsigned bits);
void free_subnet(subnet_t *subnet);

database_t *new_database(struct vmod_redis_db *db);
void free_database(database_t *db);

redisReply *redis_execute(
    VRT_CTX, struct vmod_redis_db *db, task_state_t *state, struct timeval timeout,
    unsigned max_retries, unsigned argc, const char *argv[], unsigned *retries,
    redis_server_t *server, unsigned asking, unsigned master, unsigned slot);

redis_server_t * unsafe_add_redis_server(
    VRT_CTX, struct vmod_redis_db *db, vcl_state_t *config,
    const char *location, enum REDIS_SERVER_ROLE role);

#endif
