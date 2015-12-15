#ifndef CORE_H_INCLUDED
#define CORE_H_INCLUDED

#include <syslog.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vqueue.h"

#define CLUSTERED_REDIS_SERVER_TAG "cluster"
#define CLUSTERED_REDIS_SERVER_TAG_PREFIX ":"

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

    // Tag (allocated in the heap; i.e. 'main', 'master', 'slave', etc.).
    const char *tag;

    // Type & location.
    enum REDIS_SERVER_TYPE type;
    union {
        struct address {
            const char *host;
            unsigned port;
        } address;
        const char *path;
    } location;

    // Connection timeout.
    struct timeval connection_timeout;

    // Context TTL.
    unsigned context_ttl;

    // Tail queue.
    VTAILQ_ENTRY(redis_server) list;
} redis_server_t;

typedef struct redis_context {
    // Object marker.
#define REDIS_CONTEXT_MAGIC 0xe11eaa70
    unsigned magic;

    // Data (allocated in the heap).
    redis_server_t *server;
    redisContext *rcontext;
    unsigned version;
    time_t tst;

    // Tail queue.
    VTAILQ_ENTRY(redis_context) list;
} redis_context_t;

typedef struct redis_context_pool {
    // Object marker.
#define REDIS_CONTEXT_POOL_MAGIC 0x9700a5ef
    unsigned magic;

    // Tag (allocated in the heap).
    const char *tag;

    // Mutex & condition variable.
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    // Contexts (allocated in the heap).
    unsigned ncontexts;
    VTAILQ_HEAD(,redis_context) free_contexts;
    VTAILQ_HEAD(,redis_context) busy_contexts;

    // Tail queue.
    VTAILQ_ENTRY(redis_context_pool) list;
} redis_context_pool_t;

typedef struct vcl_priv {
    // Object marker.
#define VCL_PRIV_MAGIC 0x77feec11
    unsigned magic;
} vcl_priv_t;

struct vmod_redis_db {
    // Object marker.
    unsigned magic;
#define VMOD_REDIS_DB_MAGIC 0xef35182b

    // Mutex.
    pthread_mutex_t mutex;

    // Redis servers (allocated in the heap).
    VTAILQ_HEAD(,redis_server) servers;

    // General options.
    struct timeval command_timeout;
    unsigned command_retries;
    unsigned shared_contexts;
    unsigned max_contexts;

    // Redis Cluster options / state (allocated in the heap).
    struct cluster {
        unsigned enabled;
        struct timeval connection_timeout;
        unsigned max_hops;
        unsigned context_ttl;
        const char *slots[MAX_REDIS_CLUSTER_SLOTS];
    } cluster;

    // Shared contexts (allocated in the heap).
    VTAILQ_HEAD(,redis_context_pool) pools;
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
    //   - Tag (allocated in the session workspace).
    //   - Arguments (allocated in the session workspace).
    //   - Reply (allocated in the heap).
#define MAX_REDIS_COMMAND_ARGS 128
    struct command {
        struct vmod_redis_db *db;
        struct timeval timeout;
        unsigned retries;
        const char *tag;
        unsigned argc;
        const char *argv[MAX_REDIS_COMMAND_ARGS];
        redisReply *reply;
    } command;
} thread_state_t;

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

const char *new_clustered_redis_server_tag(const char *location);

redis_server_t *new_redis_server(
    struct vmod_redis_db *db, const char *tag, const char *location,
    unsigned clustered, struct timeval connection_timeout, unsigned context_ttl);
void free_redis_server(redis_server_t *server);

redis_context_t *new_redis_context(
    redis_server_t *server, redisContext *rcontext, unsigned version, time_t tst);
void free_redis_context(redis_context_t *context);

redis_context_pool_t *new_redis_context_pool(const char *tag);
void free_redis_context_pool(redis_context_pool_t *pool);

vcl_priv_t *new_vcl_priv();
void free_vcl_priv(vcl_priv_t *priv);

struct vmod_redis_db *new_vmod_redis_db(
    struct timeval command_timeout, unsigned command_retries,
    unsigned shared_contexts, unsigned max_contexts);
void free_vmod_redis_db(struct vmod_redis_db *db);

thread_state_t *new_thread_state();
void free_thread_state(thread_state_t *state);

redis_server_t *unsafe_get_redis_server(
    struct vmod_redis_db *db, const char *tag);
redis_context_pool_t *unsafe_get_context_pool(
    struct vmod_redis_db *db, const char *tag);

redisReply *redis_execute(
    const struct vrt_ctx *ctx, struct vmod_redis_db *db, thread_state_t *state,
    const char *tag, unsigned version, struct timeval timeout,
    unsigned argc, const char *argv[], unsigned asking);

#endif
