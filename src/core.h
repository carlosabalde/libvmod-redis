#ifndef CORE_H_INCLUDED
#define CORE_H_INCLUDED

#include <syslog.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vqueue.h"

#define MAX_REDIS_CLUSTER_SLOTS 16384

enum REDIS_SERVER_TYPE {
    REDIS_SERVER_HOST_TYPE,
    REDIS_SERVER_SOCKET_TYPE
};

typedef struct redis_server {
    // Object marker.
#define REDIS_SERVER_MAGIC 0xac587b11
    unsigned magic;

    // Database.
    struct vmod_redis_db *db;

    // Tag: NULL or ip:port (allocated in the heap).
    const char *tag;

    // Type & location.
    enum REDIS_SERVER_TYPE type;
    union {
        struct {
            const char *host;
            unsigned port;
        } address;
        const char *path;
    } location;

    // Shared pool.
    struct {
        // Mutex & condition variable.
        pthread_mutex_t mutex;
        pthread_cond_t cond;

        // Contexts (allocated in the heap).
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

    // Redis servers (allocated in the heap).
    VTAILQ_HEAD(,redis_server) servers;

    // General options.
    struct timeval connection_timeout;
    unsigned context_ttl;
    struct timeval command_timeout;
    unsigned command_retries;
    unsigned shared_contexts;
    unsigned max_contexts;

    // Redis Cluster options / state (allocated in the heap).
    struct {
        unsigned enabled;
        unsigned max_hops;
        const char *slots[MAX_REDIS_CLUSTER_SLOTS];
    } cluster;

    // Stats.
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
        unsigned retries;
        unsigned argc;
        const char *argv[MAX_REDIS_COMMAND_ARGS];
        redisReply *reply;
    } command;
} thread_state_t;

typedef struct vcl_priv_db {
    // Object marker.
#define VCL_PRIV_DB_MAGIC 0x9200fda1
    unsigned magic;

    // Database.
    struct vmod_redis_db *db;

    // Tail queue.
    VTAILQ_ENTRY(vcl_priv_db) list;
} vcl_priv_db_t;

typedef struct vcl_priv {
    // Object marker.
#define VCL_PRIV_MAGIC 0x77feec11
    unsigned magic;

    // Databases.
    VTAILQ_HEAD(,vcl_priv_db) dbs;
} vcl_priv_t;

#define REDIS_LOG(ctx, message, ...) \
    do { \
        char _buffer[512]; \
        snprintf( \
            _buffer, sizeof(_buffer), \
            "[REDIS][%s] %s", __func__, message); \
        syslog(LOG_ERR, _buffer, ##__VA_ARGS__); \
        if ((ctx != NULL) && (ctx->vsl != NULL)) { \
            VSLb(ctx->vsl, SLT_Error, _buffer, ##__VA_ARGS__); \
        } \
    } while (0)

redis_server_t *new_redis_server(struct vmod_redis_db *db, const char *location);
void free_redis_server(redis_server_t *server);

redis_context_t *new_redis_context(
    redis_server_t *server, redisContext *rcontext, unsigned version, time_t tst);
void free_redis_context(redis_context_t *context);

struct vmod_redis_db *new_vmod_redis_db(
    struct timeval connection_timeout, unsigned context_ttl,
    struct timeval command_timeout, unsigned command_retries,
    unsigned shared_contexts, unsigned max_contexts, unsigned clustered,
    unsigned max_cluster_hops);
void free_vmod_redis_db(struct vmod_redis_db *db);

thread_state_t *new_thread_state();
void free_thread_state(thread_state_t *state);

vcl_priv_t *new_vcl_priv();
void free_vcl_priv(vcl_priv_t *priv);

vcl_priv_db_t *new_vcl_priv_db(struct vmod_redis_db *db);
void free_vcl_priv_db(vcl_priv_db_t *db);

redisReply *redis_execute(
    VRT_CTX, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version, struct timeval timeout,
    unsigned argc, const char *argv[], unsigned asking);

redis_server_t * unsafe_add_redis_server(
    VRT_CTX, struct vmod_redis_db *db, const char *location);

#endif
