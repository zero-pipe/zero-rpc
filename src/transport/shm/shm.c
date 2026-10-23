/*
 * POSIX shared-memory transport provider.
 *
 * Layout: one named region per channel, with a request ring written by the
 * client (endpoint id 2) and a response ring written by the server (id 1).
 * The rings carry the framework's encoded v2 envelope, so semantics stay
 * identical to the socket/DDS providers. Process-shared mutexes protect the
 * ring heads/tails and process-shared semaphores wake the peer thread.
 *
 * This first version deliberately supports one requester per channel. Use a
 * unique channel per server/client pair. The region is created by whichever
 * process starts first and unlinked by the server on destroy.
 */
#include "transport_internal.h"

#include <shm.h>

#include "endpoint.h"
#include "envelope.h"
#include "util.h"

#define NB_LOG_TAG "shm"
#include <log.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NB_SHM_MAGIC 0x4e425348u /* "NBSH" */
#define NB_SHM_VERSION 1u
#define NB_SHM_SLOT_COUNT 32u
#define NB_SHM_SLOT_BYTES NB_SHM_MAX_MESSAGE_BYTES
#define NB_SHM_WAIT_MS 10u

typedef struct nb_shm_slot {
    uint32_t len;
    uint32_t reserved;
    uint64_t seq;
    uint8_t data[NB_SHM_SLOT_BYTES];
} nb_shm_slot_t;

typedef struct nb_shm_ring {
    uint64_t head;
    uint64_t tail;
    uint64_t dropped;
    nb_shm_slot_t slots[NB_SHM_SLOT_COUNT];
} nb_shm_ring_t;

typedef struct nb_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t initialized;
    uint32_t slot_bytes;
    uint32_t slot_count;
    uint32_t reserved;
    pthread_mutex_t req_lock;
    pthread_mutex_t resp_lock;
    sem_t req_sem;
    sem_t resp_sem;
    nb_shm_ring_t req;
    nb_shm_ring_t resp;
} nb_shm_header_t;

typedef struct nb_shm_instance {
    nb_transport_provider_options_t options;
    char channel[NB_ENDPOINT_MAX];
    char name[128];
    uint16_t self_id;
    int fd;
    nb_shm_header_t *header;
    size_t map_size;
    pthread_t thread;
    int running;
    int thread_started;
} nb_shm_instance_t;

static void nb_shm_make_name(const char *channel, char *out, size_t cap)
{
    static const char prefix[] = "/zrpc-shm-";
    size_t i = 0;
    size_t j = 0;

    while (prefix[i] && j + 1 < cap) {
        out[j++] = prefix[i++];
    }
    for (i = 0; channel && channel[i] && j + 1 < cap; i++) {
        char c = channel[i];
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out[j++] = keep ? c : '_';
    }
    if (cap) {
        out[j] = '\0';
    }
}

static void nb_shm_wait_sem(sem_t *sem)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return;
    }
    ts.tv_nsec += (long)NB_SHM_WAIT_MS * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (sem_timedwait(sem, &ts) == -1 && errno == EINTR) {
    }
}

static void nb_shm_drain_sem(sem_t *sem)
{
    while (sem_trywait(sem) == 0) {
    }
}

static int nb_shm_init_header(nb_shm_header_t *header)
{
    pthread_mutexattr_t attr;
    int rc = NB_OK;

    memset(header, 0, sizeof(*header));
    header->magic = NB_SHM_MAGIC;
    header->version = NB_SHM_VERSION;
    header->slot_bytes = NB_SHM_SLOT_BYTES;
    header->slot_count = NB_SHM_SLOT_COUNT;
    if (pthread_mutexattr_init(&attr) != 0) {
        return NB_ERR_IO;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        rc = NB_ERR_IO;
    } else if (pthread_mutex_init(&header->req_lock, &attr) != 0 ||
               pthread_mutex_init(&header->resp_lock, &attr) != 0) {
        rc = NB_ERR_IO;
    }
    pthread_mutexattr_destroy(&attr);
    if (rc != NB_OK) {
        return rc;
    }
    if (sem_init(&header->req_sem, 1, 0) != 0 ||
        sem_init(&header->resp_sem, 1, 0) != 0) {
        return NB_ERR_IO;
    }
    __atomic_store_n(&header->initialized, 1u, __ATOMIC_RELEASE);
    return NB_OK;
}

static int nb_shm_open_region(nb_shm_instance_t *inst)
{
    struct stat st;
    void *map;
    int created = 0;
    int fd;
    int i;

    nb_shm_make_name(inst->channel, inst->name, sizeof(inst->name));
    fd = shm_open(inst->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
        created = 1;
    } else if (errno == EEXIST) {
        fd = shm_open(inst->name, O_RDWR, 0600);
        if (fd < 0) {
            nb_log_error("shm_open(%s) failed: %s", inst->name, strerror(errno));
            return NB_ERR_IO;
        }
    } else {
        nb_log_error("shm_open(%s) failed: %s", inst->name, strerror(errno));
        return NB_ERR_IO;
    }

    inst->map_size = sizeof(nb_shm_header_t);
    if (created) {
        if (ftruncate(fd, (off_t)inst->map_size) != 0) {
            nb_log_error("ftruncate(%s) failed: %s", inst->name, strerror(errno));
            close(fd);
            shm_unlink(inst->name);
            return NB_ERR_IO;
        }
    } else if (fstat(fd, &st) != 0 || (size_t)st.st_size < inst->map_size) {
        nb_log_error("shared region %s is too small", inst->name);
        close(fd);
        return NB_ERR_IO;
    }

    map = mmap(NULL, inst->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        nb_log_error("mmap(%s) failed: %s", inst->name, strerror(errno));
        close(fd);
        if (created) {
            shm_unlink(inst->name);
        }
        return NB_ERR_IO;
    }
    inst->fd = fd;
    inst->header = (nb_shm_header_t *)map;

    if (created && nb_shm_init_header(inst->header) != NB_OK) {
        nb_log_error("failed to initialize shared region %s", inst->name);
        munmap(inst->header, inst->map_size);
        inst->header = NULL;
        close(fd);
        inst->fd = -1;
        shm_unlink(inst->name);
        return NB_ERR_IO;
    }

    for (i = 0; i < 5000; i++) {
        if (__atomic_load_n(&inst->header->initialized, __ATOMIC_ACQUIRE)) {
            break;
        }
        usleep(1000);
    }
    if (!__atomic_load_n(&inst->header->initialized, __ATOMIC_ACQUIRE)) {
        nb_log_error("shared region %s was not initialized in time", inst->name);
        munmap(inst->header, inst->map_size);
        inst->header = NULL;
        close(fd);
        inst->fd = -1;
        return NB_ERR_TIMEOUT;
    }
    if (inst->header->magic != NB_SHM_MAGIC ||
        inst->header->version != NB_SHM_VERSION ||
        inst->header->slot_bytes != NB_SHM_SLOT_BYTES ||
        inst->header->slot_count != NB_SHM_SLOT_COUNT) {
        nb_log_error("shared region %s has an incompatible layout", inst->name);
        munmap(inst->header, inst->map_size);
        inst->header = NULL;
        close(fd);
        inst->fd = -1;
        return NB_ERR_STATE;
    }

    /*
     * The server owns the lifecycle. Starting a fresh server generation
     * discards stale ring contents from an unclean previous run.
     */
    if (inst->self_id == 1) {
        pthread_mutex_lock(&inst->header->req_lock);
        inst->header->req.head = 0;
        inst->header->req.tail = 0;
        inst->header->req.dropped = 0;
        pthread_mutex_unlock(&inst->header->req_lock);
        pthread_mutex_lock(&inst->header->resp_lock);
        inst->header->resp.head = 0;
        inst->header->resp.tail = 0;
        inst->header->resp.dropped = 0;
        pthread_mutex_unlock(&inst->header->resp_lock);
        nb_shm_drain_sem(&inst->header->req_sem);
        nb_shm_drain_sem(&inst->header->resp_sem);
    }
    return NB_OK;
}

static void nb_shm_close_region(nb_shm_instance_t *inst)
{
    if (!inst) {
        return;
    }
    if (inst->header) {
        munmap(inst->header, inst->map_size);
        inst->header = NULL;
    }
    if (inst->fd >= 0) {
        close(inst->fd);
        inst->fd = -1;
    }
    if (inst->self_id == 1 && inst->name[0]) {
        shm_unlink(inst->name);
    }
}

static void nb_shm_fill_envelope(const nb_transport_message_t *message, nb_envelope_t *env)
{
    memset(env, 0, sizeof(*env));
    env->kind = message->kind;
    env->encoding = message->payload.encoding;
    env->flags = message->flags;
    env->status = message->status;
    env->request_id = message->request_id;
    env->timeout_ms = message->timeout_ms;
    env->route = message->route;
    env->metadata = message->metadata;
    env->metadata_len = message->metadata_len;
    env->status_msg = message->status_msg;
    env->status_msg_len = message->status_msg_len;
    env->payload = message->payload.data;
    env->payload_len = message->payload.len;
    if (message->has_stream) {
        env->stream_flags = (uint8_t)message->stream.flags;
        env->stream_id = message->stream.stream_id;
        env->stream_offset = message->stream.offset;
        env->stream_total_size = message->stream.total_size;
    }
}

static int nb_shm_publish(nb_shm_instance_t *inst, int response,
                          const nb_transport_message_t *message)
{
    nb_envelope_t env;
    nb_shm_ring_t *ring;
    pthread_mutex_t *lock;
    sem_t *sem;
    nb_shm_slot_t *slot;
    size_t need;
    size_t written;

    if (!inst || !inst->header || !message) {
        return NB_ERR_INVALID;
    }
    nb_shm_fill_envelope(message, &env);
    need = nb_envelope_size(&env);
    if (need == 0 || need > NB_SHM_SLOT_BYTES) {
        return NB_ERR_TOOBIG;
    }
    ring = response ? &inst->header->resp : &inst->header->req;
    lock = response ? &inst->header->resp_lock : &inst->header->req_lock;
    sem = response ? &inst->header->resp_sem : &inst->header->req_sem;

    pthread_mutex_lock(lock);
    if (ring->head - ring->tail >= NB_SHM_SLOT_COUNT) {
        ring->dropped++;
        pthread_mutex_unlock(lock);
        return NB_ERR_AGAIN;
    }
    slot = &ring->slots[ring->head % NB_SHM_SLOT_COUNT];
    written = nb_envelope_encode(slot->data, NB_SHM_SLOT_BYTES, &env);
    if (written != need) {
        pthread_mutex_unlock(lock);
        return NB_ERR_IO;
    }
    slot->len = (uint32_t)written;
    slot->reserved = 0;
    slot->seq = ring->head;
    ring->head++;
    pthread_mutex_unlock(lock);
    sem_post(sem);
    return NB_OK;
}

static void nb_shm_deliver(nb_shm_instance_t *inst, const uint8_t *buf, size_t len,
                           int server)
{
    nb_envelope_t env;
    nb_transport_message_t msg;
    char route_buf[MAX_ROUTE];
    uint16_t peer_port = server ? 2u : 1u;

    if (nb_envelope_decode(buf, len, &env) != NB_OK) {
        return;
    }
    memset(&msg, 0, sizeof(msg));
    msg.kind = env.kind;
    msg.flags = env.flags;
    msg.status = env.status;
    msg.request_id = env.request_id;
    msg.timeout_ms = env.timeout_ms;
    msg.metadata = env.metadata;
    msg.metadata_len = env.metadata_len;
    msg.status_msg = env.status_msg;
    msg.status_msg_len = env.status_msg_len;
    if (env.route && env.route_len < sizeof(route_buf)) {
        memcpy(route_buf, env.route, env.route_len);
        route_buf[env.route_len] = '\0';
        msg.route = route_buf;
    }
    msg.payload.data = env.payload;
    msg.payload.len = env.payload_len;
    msg.payload.encoding = env.encoding;
    if (env.stream_flags != 0 || env.stream_id != 0) {
        msg.has_stream = 1;
        msg.stream.flags = env.stream_flags;
        msg.stream.stream_id = env.stream_id;
        msg.stream.offset = env.stream_offset;
        msg.stream.total_size = env.stream_total_size;
    }
    if (inst->options.on_receive) {
        inst->options.on_receive(inst->options.receiver, NULL, inst->channel, peer_port, &msg);
    }
}

static void *nb_shm_thread_main(void *arg)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)arg;
    uint8_t *scratch;

    if (!inst || !inst->header) {
        return NULL;
    }
    scratch = (uint8_t *)malloc(NB_SHM_SLOT_BYTES);
    if (!scratch) {
        return NULL;
    }
    while (inst->running) {
        int server = inst->self_id == 1;
        nb_shm_ring_t *ring = server ? &inst->header->req : &inst->header->resp;
        pthread_mutex_t *lock = server ? &inst->header->req_lock : &inst->header->resp_lock;
        sem_t *sem = server ? &inst->header->req_sem : &inst->header->resp_sem;

        nb_shm_wait_sem(sem);
        for (;;) {
            nb_shm_slot_t *slot;
            uint32_t len = 0;

            pthread_mutex_lock(lock);
            if (ring->tail == ring->head) {
                pthread_mutex_unlock(lock);
                break;
            }
            slot = &ring->slots[ring->tail % NB_SHM_SLOT_COUNT];
            len = slot->len;
            if (len > 0 && len <= NB_SHM_SLOT_BYTES) {
                memcpy(scratch, slot->data, len);
            } else {
                len = 0;
            }
            ring->tail++;
            pthread_mutex_unlock(lock);
            if (len > 0) {
                nb_shm_deliver(inst, scratch, len, server);
            }
        }
    }
    free(scratch);
    return NULL;
}

static int nb_shm_op_create(const nb_transport_provider_options_t *options,
                            nb_transport_instance_t **out)
{
    nb_shm_instance_t *inst;
    nb_endpoint_t endpoint;
    int rc;

    if (!options || !out || !options->endpoint) {
        return NB_ERR_INVALID;
    }
    *out = NULL;
    if (nb_endpoint_parse(options->endpoint, &endpoint) != NB_OK ||
        strcmp(endpoint.scheme, "shm") != 0 || endpoint.port == 0) {
        return NB_ERR_INVALID;
    }
    if (endpoint.port != 1 && endpoint.port < 2) {
        return NB_ERR_INVALID;
    }
    inst = (nb_shm_instance_t *)calloc(1, sizeof(*inst));
    if (!inst) {
        return NB_ERR_NOMEM;
    }
    inst->options = *options;
    inst->self_id = endpoint.port;
    inst->fd = -1;
    nb_copy_str(inst->channel, sizeof(inst->channel), endpoint.host, "");
    if (!inst->channel[0]) {
        free(inst);
        return NB_ERR_INVALID;
    }
    rc = nb_shm_open_region(inst);
    if (rc != NB_OK) {
        free(inst);
        return rc;
    }
    *out = (nb_transport_instance_t *)inst;
    return NB_OK;
}

static int nb_shm_op_start(nb_transport_instance_t *instance)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    int rc;

    if (!inst || !inst->header) {
        return NB_ERR_INVALID;
    }
    if (inst->thread_started) {
        return NB_OK;
    }
    inst->running = 1;
    rc = pthread_create(&inst->thread, NULL, nb_shm_thread_main, inst);
    if (rc != 0) {
        inst->running = 0;
        return NB_ERR_NOMEM;
    }
    inst->thread_started = 1;
    return NB_OK;
}

static void nb_shm_op_stop(nb_transport_instance_t *instance)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst || !inst->thread_started) {
        return;
    }
    inst->running = 0;
    sem_post(&inst->header->req_sem);
    sem_post(&inst->header->resp_sem);
    pthread_join(inst->thread, NULL);
    inst->thread_started = 0;
}

static void nb_shm_op_destroy(nb_transport_instance_t *instance)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst) {
        return;
    }
    nb_shm_op_stop(instance);
    nb_shm_close_region(inst);
    free(inst);
}

static int nb_shm_op_send(nb_transport_instance_t *instance,
                          const char *host,
                          uint16_t port,
                          const nb_transport_message_t *message)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;

    if (!inst || !message || !host) {
        return NB_ERR_INVALID;
    }
    if (strcmp(host, inst->channel) != 0) {
        return NB_ERR_NOTFOUND;
    }
    if (inst->self_id == 1 || port != 1) {
        return NB_ERR_NOT_SUPPORTED;
    }
    return nb_shm_publish(inst, 0, message);
}

static int nb_shm_op_reply(nb_transport_instance_t *instance,
                           void *reply_token,
                           const char *host,
                           uint16_t port,
                           const nb_transport_message_t *message)
{
    nb_shm_instance_t *inst = (nb_shm_instance_t *)instance;
    (void)reply_token;
    (void)host;
    (void)port;

    if (!inst || !message) {
        return NB_ERR_INVALID;
    }
    if (inst->self_id != 1) {
        return NB_ERR_NOT_SUPPORTED;
    }
    return nb_shm_publish(inst, 1, message);
}

static const nb_transport_provider_ops_t nb_g_shm_ops = {
    nb_shm_op_create,
    nb_shm_op_start,
    nb_shm_op_stop,
    nb_shm_op_destroy,
    nb_shm_op_send,
    nb_shm_op_reply,
    NULL,
    NULL,
};

static const nb_transport_provider_t nb_g_shm_provider = {
    "shm",
    &nb_g_shm_ops,
};

const nb_transport_provider_t *nb_shm_provider(void)
{
    return &nb_g_shm_provider;
}

int nb_shm_register(void)
{
    if (nb_transport_provider_find("shm")) {
        return NB_OK;
    }
    return nb_transport_provider_register(&nb_g_shm_provider);
}

#else /* !__linux__ */

int nb_shm_register(void)
{
    return NB_ERR_NOT_SUPPORTED;
}

const nb_transport_provider_t *nb_shm_provider(void)
{
    return NULL;
}

#endif
