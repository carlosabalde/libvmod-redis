#ifndef CORE_H_INCLUDED
#define CORE_H_INCLUDED

#include <syslog.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

#include "vqueue.h"

#define CLUSTERED_REDIS_SERVER_TAG "cluster"
#define CLUSTERED_REDIS_SERVER_TAG_PREFIX ":"
#define CLUSTERED_REDIS_SERVER_TAG_FORMAT "%s%s:%u"

#define MAX_REDIS_CLUSTER_SLOTS 16384

enum REDIS_SERVER_TYPE {
    REDIS_SERVER_HOST_TYPE,
    REDIS_SERVER_SOCKET_TYPE
};

typedef struct redis_server {
    // Object marker.
#define REDIS_SERVER_MAGIC 0xac587b11
    unsigned magic;

    // Tag (i.e. 'main', 'master', 'slave', etc.).
    const char *tag;
    unsigned clustered;

    // Type & location.
    enum REDIS_SERVER_TYPE type;
    union {
        struct address {
            const char *host;
            unsigned port;
        } address;
        const char *path;
    } location;

    // Context timeout & TTL.
    struct timeval timeout;
    unsigned ttl;

    // Tail queue.
    VTAILQ_ENTRY(redis_server) list;
} redis_server_t;

typedef struct redis_context {
    // Object marker.
#define REDIS_CONTEXT_MAGIC 0xe11eaa70
    unsigned magic;

    // Data.
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

    // Tag.
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

    // Mutex.
    pthread_mutex_t mutex;

    // Redis servers (allocated in the heap).
    VTAILQ_HEAD(,redis_server) servers;

    // Redis Cluster slots (allocated in the heap).
    const char *slots[MAX_REDIS_CLUSTER_SLOTS];

    // General options.
    unsigned clustered;
    unsigned shared_contexts;
    unsigned max_contexts;

    // Shared contexts (allocated in the heap).
    VTAILQ_HEAD(,redis_context_pool) pools;
} vcl_priv_t;

typedef struct thread_state {
    // Object marker.
#define THREAD_STATE_MAGIC 0xa6bc103e
    unsigned magic;

    // Private contexts (allocated in the heap).
    unsigned ncontexts;
    VTAILQ_HEAD(,redis_context) contexts;

    // Redis command:
    //   - Tag (allocated in the session workspace).
    //   - Arguments (allocated in the session workspace).
    //   - Reply (allocated in the heap).
#define MAX_REDIS_COMMAND_ARGS 128
    const char *tag;
    unsigned argc;
    const char *argv[MAX_REDIS_COMMAND_ARGS];
    redisReply *reply;
} thread_state_t;

#define REDIS_LOG(sp, message, ...) \
    do { \
        char _buffer[512]; \
        snprintf( \
            _buffer, sizeof(_buffer), \
            "[REDIS][%s] %s", __func__, message); \
        syslog(LOG_ERR, _buffer, ##__VA_ARGS__); \
        if (sp != NULL) { \
            WSP(sp, SLT_Error, _buffer, ##__VA_ARGS__); \
        } \
    } while (0)

redis_server_t *new_redis_server(
    const char *tag, const char *location, int timeout, int ttl);
void free_redis_server(redis_server_t *server);

redis_context_t *new_redis_context(
    redis_server_t *server, redisContext *rcontext, unsigned version, time_t tst);
void free_redis_context(redis_context_t *context);

redis_context_pool_t *new_redis_context_pool(const char *tag);
void free_redis_context_pool(redis_context_pool_t *pool);

vcl_priv_t *new_vcl_priv(unsigned shared_pool, unsigned max_pool_size);
void free_vcl_priv(vcl_priv_t *priv);

thread_state_t *new_thread_state();
void free_thread_state(thread_state_t *state);

unsigned unsafe_server_exists(vcl_priv_t *config, const char *tag);
unsigned unsafe_pool_exists(vcl_priv_t *config, const char *tag);

redis_context_t *get_context(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    const char *tag, unsigned int version);
void free_context(
    struct sess *sp, vcl_priv_t *config, thread_state_t *state,
    redis_context_t * context);

#endif
