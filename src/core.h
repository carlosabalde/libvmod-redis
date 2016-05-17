#ifndef CORE_H_INCLUDED
#define CORE_H_INCLUDED

#include <syslog.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vqueue.h"

#define NREDIS_CLUSTER_SLOTS 16384

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

    // Shared pool.
    struct {
        // Mutex & condition variable.
        pthread_mutex_t mutex;
        pthread_cond_t cond;

        // Contexts (rw fields -allocated in the heap- to be protected by the
        // associated mutex and condition variable).
        unsigned ncontexts;
        VTAILQ_HEAD(,redis_context) free_contexts;
        VTAILQ_HEAD(,redis_context) busy_contexts;
    } pool;

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

struct vmod_redis_db {
    // Object marker.
    unsigned magic;
#define VMOD_REDIS_DB_MAGIC 0xef35182b

    // Mutex.
    pthread_mutex_t mutex;

    // General options (allocated in the heap).
    const char *name;
    struct timeval connection_timeout;
    unsigned connection_ttl;
    struct timeval command_timeout;
    unsigned max_command_retries;
    unsigned shared_connections;
    unsigned max_connections;
    const char *password;

    // Redis servers (rw field -allocated in the heap- to be protected by the
    // associated mutex).
    VTAILQ_HEAD(,redis_server) servers;

    // Redis Cluster options & state.
    struct {
        unsigned enabled;
        unsigned max_hops;
        redis_server_t *slots[NREDIS_CLUSTER_SLOTS];
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

typedef struct thread_state {
    // Object marker.
#define THREAD_STATE_MAGIC 0xa6bc103e
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
} thread_state_t;

typedef struct vcl_state {
    // Object marker.
#define VCL_STATE_MAGIC 0x77feec11
    unsigned magic;
} vcl_state_t;

typedef struct vmod_state {
    // Mutex.
    pthread_mutex_t mutex;

    // Version increased every time the VMOD is initialized (rw field protected
    // by the associated mutex on writes; it's ok to ignore the lock during reads).
    // This will be used to (1) reestablish connections binded to worker
    // threads; and (2) regenerate pooled connections shared between threads.
    unsigned version;
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

redis_server_t *new_redis_server(struct vmod_redis_db *db, const char *location);
void free_redis_server(redis_server_t *server);

redis_context_t *new_redis_context(
    redis_server_t *server, redisContext *rcontext, time_t tst);
void free_redis_context(redis_context_t *context);

struct vmod_redis_db *new_vmod_redis_db(
    const char *name, struct timeval connection_timeout, unsigned connection_ttl,
    struct timeval command_timeout, unsigned command_retries,
    unsigned shared_contexts, unsigned max_contexts, const char *password,
    unsigned clustered, unsigned max_cluster_hops);
void free_vmod_redis_db(struct vmod_redis_db *db);

thread_state_t *new_thread_state();
void free_thread_state(thread_state_t *state);

vcl_state_t *new_vcl_state();
void free_vcl_state(vcl_state_t *priv);

redisReply *redis_execute(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    redis_server_t *server, struct timeval timeout,
    unsigned argc, const char *argv[], unsigned asking);

redis_server_t * unsafe_add_redis_server(
    VRT_CTX, struct vmod_redis_db *db, const char *location);

#endif
