/*
 * Handler 线程池：阻塞 handler 注册为 worker 后不应卡死 poller。
 *
 * 覆盖点：
 *   - 用 nb_service_method_options_t 选择 WORKER，回复跨线程回投 poller 且正确；
 *   - 一个阻塞 worker handler 执行期间，同一节点上的普通内联 handler 仍能响应；
 *   - worker handler 内的请求 payload 由框架拷贝，调用方可立即释放。
 *   - handler_threads=0 时 WORKER 退化为内联仍能正确响应；
 *   - 非法 options / 流式 WORKER 被拒绝。
 *   - handler_max_concurrency 超限时新请求立即回 NB_ERR_AGAIN，而不是无界排队。
 *   - deadline 传播到 req/call，排队期间过期的任务在启动前被跳过；
 *   - nb_proxy_cancel 把取消传播到服务端，排队任务被跳过。
 */
#include <client.h>
#include <server.h>
#include <platform.h>
#include <thread/sem.h>
#include <thread/sync.h>
#include <thread/thread.h>

#define NB_LOG_TAG "test"
#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 阻塞 handler：sleep 后回显。用于制造 poller 卡顿。 */
static void slow_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    ntk_sleep_ms(400);
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

/* 快速内联 handler：worker 阻塞时它仍应在 poller 上及时响应。 */
static void fast_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    (void)user;
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

/* 校验用：流式注册在非法/不支持选项下应在建表前被拒绝。 */
static void dummy_chunk_handler(nb_call_t *call,
                                const nb_request_t *req,
                                const nb_chunk_t *chunk,
                                void *user)
{
    (void)call;
    (void)req;
    (void)chunk;
    (void)user;
}

/* 校验用：同步风格 worker handler。 */
static int sync_echo_handler(const nb_request_t *req, nb_message_view_t *resp, void *user)
{
    (void)user;
    return nb_message_view_set_data(resp, req->payload.data, req->payload.len);
}

/* 阻塞 worker handler：占住 worker，直到测试放行，用来制造过载。 */
typedef struct block_state {
    ntk_sem *started;
    ntk_sem *release;
} block_state_t;

static void blocking_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    block_state_t *st = (block_state_t *)user;
    ntk_sem_post(st->started, 1);
    ntk_sem_wait(st->release);
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

/* ---- 测试用自定义 executor ---- */

/* 同步直通 executor：验证 submit 被调用以及注册/分派链路。 */
typedef struct direct_executor {
    int submitted;
} direct_executor_t;

static int direct_executor_submit(void *ctx, void (*fn)(void *), void *user)
{
    direct_executor_t *e = (direct_executor_t *)ctx;
    e->submitted++;
    fn(user);
    return NB_OK;
}

/* 拒绝 executor：验证 submit 返回错误时客户端收到该状态。 */
static int reject_executor_submit(void *ctx, void (*fn)(void *), void *user)
{
    (void)ctx;
    (void)fn;
    (void)user;
    return NB_ERR_AGAIN;
}

/*
 * 单线程后台 executor：验证自定义 executor 在非 poller 线程执行，
 * nb_reply() 仍能回投原 poller。一次只接收一个任务，够测试用。
 */
typedef struct thread_executor {
    ntk_mutex *lock;
    ntk_sem *sem;
    ntk_thread *thread;
    void (*fn)(void *);
    void *user;
    int stop;
    int submitted;
} thread_executor_t;

static void thread_executor_loop(void *arg)
{
    thread_executor_t *e = (thread_executor_t *)arg;
    for (;;) {
        void (*fn)(void *);
        void *user;
        ntk_sem_wait(e->sem);
        ntk_mutex_lock(e->lock);
        if (e->stop) {
            ntk_mutex_unlock(e->lock);
            break;
        }
        fn = e->fn;
        user = e->user;
        e->fn = NULL;
        e->user = NULL;
        ntk_mutex_unlock(e->lock);
        if (fn) {
            fn(user);
        }
    }
}

static int thread_executor_submit(void *ctx, void (*fn)(void *), void *user)
{
    thread_executor_t *e = (thread_executor_t *)ctx;
    int rc = NB_OK;
    ntk_mutex_lock(e->lock);
    if (e->fn) {
        rc = NB_ERR_AGAIN;
    } else {
        e->fn = fn;
        e->user = user;
        e->submitted++;
    }
    ntk_mutex_unlock(e->lock);
    if (rc == NB_OK) {
        ntk_sem_post(e->sem, 1);
    }
    return rc;
}

static int thread_executor_start(thread_executor_t *e)
{
    memset(e, 0, sizeof(*e));
    e->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    e->sem = ntk_sem_create(0);
    if (!e->lock || !e->sem) {
        return 1;
    }
    e->thread = ntk_thread_create(thread_executor_loop, e);
    return e->thread ? 0 : 1;
}

static void thread_executor_stop(thread_executor_t *e)
{
    if (e->lock) {
        ntk_mutex_lock(e->lock);
        e->stop = 1;
        ntk_mutex_unlock(e->lock);
    }
    if (e->sem) {
        ntk_sem_post(e->sem, 1);
    }
    if (e->thread) {
        ntk_thread_join(e->thread);
        ntk_thread_destroy(e->thread);
        e->thread = NULL;
    }
    if (e->sem) {
        ntk_sem_destroy(e->sem);
        e->sem = NULL;
    }
    if (e->lock) {
        ntk_mutex_destroy(e->lock);
        e->lock = NULL;
    }
}

static nb_service_method_options_t method_opts(nb_service_exec_mode_t exec,
                                            const nb_service_executor_t *executor)
{
    nb_service_method_options_t opts = NB_SERVICE_METHOD_OPTIONS_INIT;
    opts.exec = exec;
    opts.executor = executor;
    return opts;
}

typedef struct client_state {
    ntk_sem *sem;
    int status;     /* 传输层状态：收到响应即 NB_OK */
    int app_status; /* 服务端应用状态（nb_response_t.status） */
    char buf[512];
    size_t len;
    uint64_t done_ms;
} client_state_t;

static void on_response(int status, const nb_response_t *resp, void *user)
{
    client_state_t *st = (client_state_t *)user;
    st->status = status;
    st->app_status = resp ? resp->status : status;
    if (status == NB_OK && resp && resp->payload.len <= sizeof(st->buf)) {
        memcpy(st->buf, resp->payload.data, resp->payload.len);
        st->len = resp->payload.len;
    }
    ntk_sem_post(st->sem, 1);
}

static int run_case(nb_transport_kind_t kind, const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    client_state_t slow;
    client_state_t fast;
    nb_payload_t pl;
    uint16_t sport;
    uint16_t cport;
    int failed = 0;

    memset(&slow, 0, sizeof(slow));
    memset(&fast, 0, sizeof(fast));

    sport = kind == NB_TRANSPORT_TCP ? 44211 : 44111;
    cport = kind == NB_TRANSPORT_TCP ? 44212 : 44112;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "worker-srv";
    scfg.transport.transport = kind;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 2;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "worker-cli";
    ccfg.transport.transport = kind;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        nb_log_error("[%s] node create failed", label);
        if (server) {
            nb_node_destroy(server);
        }
        if (client) {
            nb_node_destroy(client);
        }
        return 1;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    {
        nb_service_method_options_t worker = NB_SERVICE_METHOD_OPTIONS_INIT;
        worker.exec = NB_SERVICE_EXEC_WORKER;
        if (nb_service_create(server, "pool", &svc) != NB_OK ||
            nb_service_add_method_ex(svc, "slow", slow_handler, NULL, &worker) != NB_OK ||
            nb_service_add_method(svc, "fast", fast_handler, NULL) != NB_OK) {
            nb_log_error("[%s] service setup failed", label);
            failed = 1;
            goto cleanup;
        }
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK) {
        nb_log_error("[%s] proxy create failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_node_wait_for_service(client, "pool", 5000) != NB_OK) {
        nb_log_error("[%s] service not discovered", label);
        failed = 1;
        goto cleanup;
    }

    slow.sem = ntk_sem_create(0);
    fast.sem = ntk_sem_create(0);
    if (!slow.sem || !fast.sem) {
        failed = 1;
        goto cleanup;
    }

    /* 先发阻塞调用，再发内联调用；后者应显著早于前者返回。 */
    pl.data = "slow-payload";
    pl.len = (size_t)strlen("slow-payload") + 1;
    pl.encoding = 0;
    if (nb_proxy_call_async(proxy, "pool", "slow", &pl, on_response, &slow, 5000, NULL) != NB_OK) {
        nb_log_error("[%s] slow call failed", label);
        failed = 1;
        goto cleanup;
    }
    /* 模拟动态内存：调用返回后立刻释放，框架应已拷贝。 */
    {
        char *dyn = (char *)malloc(pl.len);
        if (!dyn) {
            failed = 1;
            goto cleanup;
        }
        memcpy(dyn, "fast-payload", pl.len);
        pl.data = dyn;
        if (nb_proxy_call_async(proxy, "pool", "fast", &pl, on_response, &fast, 5000, NULL) != NB_OK) {
            free(dyn);
            nb_log_error("[%s] fast call failed", label);
            failed = 1;
            goto cleanup;
        }
        free(dyn);
    }

    if (ntk_sem_timedwait(fast.sem, 300) != NTK_OK) {
        nb_log_error("[%s] fast call stalled behind slow handler", label);
        failed = 1;
    } else if (fast.status != NB_OK || fast.len != pl.len ||
               memcmp(fast.buf, "fast-payload", pl.len) != 0) {
        nb_log_error("[%s] fast response bad status=%d len=%zu", label, fast.status, fast.len);
        failed = 1;
    }

    if (ntk_sem_timedwait(slow.sem, 3000) != NTK_OK) {
        nb_log_error("[%s] slow call timeout", label);
        failed = 1;
    } else if (slow.status != NB_OK || slow.len != (size_t)strlen("slow-payload") + 1 ||
               memcmp(slow.buf, "slow-payload", slow.len) != 0) {
        nb_log_error("[%s] slow response bad status=%d len=%zu", label, slow.status, slow.len);
        failed = 1;
    }

cleanup:
    if (slow.sem) {
        ntk_sem_destroy(slow.sem);
    }
    if (fast.sem) {
        ntk_sem_destroy(fast.sem);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    return failed;
}

/*
 * handler_threads=0 时 WORKER 方法没有池可用，应退化为 poller 内联执行，
 * 而不是丢请求或报错。
 */
static int run_fallback_case(nb_transport_kind_t kind, const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    client_state_t state;
    nb_payload_t pl;
    uint16_t sport;
    uint16_t cport;
    int failed = 0;

    memset(&state, 0, sizeof(state));
    sport = kind == NB_TRANSPORT_TCP ? 44411 : 44311;
    cport = kind == NB_TRANSPORT_TCP ? 44412 : 44312;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "fallback-srv";
    scfg.transport.transport = kind;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 0;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "fallback-cli";
    ccfg.transport.transport = kind;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        nb_log_error("[%s] node create failed", label);
        failed = 1;
        goto cleanup;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_service_create(server, "fallback", &svc) != NB_OK ||
        nb_service_add_method_worker(svc, "slow", slow_handler, NULL) != NB_OK) {
        nb_log_error("[%s] service setup failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK ||
        nb_node_wait_for_service(client, "fallback", 5000) != NB_OK) {
        nb_log_error("[%s] client setup failed", label);
        failed = 1;
        goto cleanup;
    }

    state.sem = ntk_sem_create(0);
    if (!state.sem) {
        failed = 1;
        goto cleanup;
    }
    pl.data = "fallback-payload";
    pl.len = (size_t)strlen("fallback-payload") + 1;
    pl.encoding = 0;
    if (nb_proxy_call_async(proxy, "fallback", "slow", &pl, on_response, &state, 5000, NULL) != NB_OK) {
        nb_log_error("[%s] fallback call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(state.sem, 3000) != NTK_OK) {
        nb_log_error("[%s] fallback call timeout", label);
        failed = 1;
    } else if (state.status != NB_OK || state.len != pl.len ||
               memcmp(state.buf, "fallback-payload", pl.len) != 0) {
        nb_log_error(
            "[%s] fallback response bad status=%d len=%zu", label, state.status, state.len);
        failed = 1;
    }

cleanup:
    if (state.sem) {
        ntk_sem_destroy(state.sem);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    return failed;
}

/* 注册 API 的参数校验：非法组合必须被拒绝，合法组合必须成功。 */
static int run_api_checks(void)
{
    nb_node_config_t cfg;
    nb_node_t *node = NULL;
    nb_service_t *svc = NULL;
    nb_service_method_options_t opts;
    nb_service_executor_t good_exec;
    nb_service_executor_t bad_exec;
    direct_executor_t direct;
    int failed = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "worker-api";
    cfg.transport.transport = NB_TRANSPORT_UDP;
    cfg.transport.bind_host = "127.0.0.1";
    cfg.transport.data_port = 0;
    cfg.runtime.io_threads = 1;
    cfg.runtime.handler_threads = 1;
    cfg.mode = NB_MODE_STATIC;

    memset(&direct, 0, sizeof(direct));
    good_exec.ctx = &direct;
    good_exec.submit = direct_executor_submit;
    bad_exec.ctx = NULL;
    bad_exec.submit = NULL;

    if (nb_node_create(&cfg, &node) != NB_OK || nb_service_create(node, "api", &svc) != NB_OK) {
        nb_log_error("[api] setup failed");
        failed = 1;
        goto cleanup;
    }

    opts = method_opts((nb_service_exec_mode_t)99, NULL);
    if (nb_service_add_method_ex(svc, "bad", fast_handler, NULL, &opts) != NB_ERR_INVALID) {
        nb_log_error("[api] invalid exec accepted");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_WORKER, NULL);
    if (nb_service_add_stream_method_ex(svc, "workerstream", dummy_chunk_handler, NULL, &opts) != NB_OK) {
        nb_log_error("[api] stream worker registration failed");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_EXECUTOR, &good_exec);
    if (nb_service_add_stream_method_ex(svc, "execstream", dummy_chunk_handler, NULL, &opts) != NB_OK) {
        nb_log_error("[api] stream executor registration failed");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_EXECUTOR, NULL);
    if (nb_service_add_method_ex(svc, "noexec", fast_handler, NULL, &opts) != NB_ERR_INVALID) {
        nb_log_error("[api] executor without impl accepted");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_EXECUTOR, &bad_exec);
    if (nb_service_add_method_ex(svc, "badsubmit", fast_handler, NULL, &opts) != NB_ERR_INVALID) {
        nb_log_error("[api] executor without submit accepted");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_INLINE, &good_exec);
    if (nb_service_add_method_ex(svc, "inlineexec", fast_handler, NULL, &opts) != NB_ERR_INVALID) {
        nb_log_error("[api] inline with executor accepted");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_WORKER, &good_exec);
    if (nb_service_add_method_ex(svc, "workerexec", fast_handler, NULL, &opts) != NB_ERR_INVALID) {
        nb_log_error("[api] worker with executor accepted");
        failed = 1;
    }

    if (nb_service_add_method_ex(svc, "inline", fast_handler, NULL, NULL) != NB_OK) {
        nb_log_error("[api] inline registration failed");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_WORKER, NULL);
    if (nb_service_add_method_ex(svc, "worker", fast_handler, NULL, &opts) != NB_OK ||
        nb_service_add_sync_method_ex(svc, "syncworker", sync_echo_handler, NULL, &opts) != NB_OK) {
        nb_log_error("[api] worker registration failed");
        failed = 1;
    }

    opts = method_opts(NB_SERVICE_EXEC_EXECUTOR, &good_exec);
    if (nb_service_add_method_ex(svc, "executor", fast_handler, NULL, &opts) != NB_OK ||
        nb_service_add_sync_method_ex(svc, "syncexecutor", sync_echo_handler, NULL, &opts) != NB_OK) {
        nb_log_error("[api] executor registration failed");
        failed = 1;
    }

cleanup:
    if (node) {
        nb_node_destroy(node);
    }
    return failed;
}

/*
 * 单 worker + max_concurrency=1：第一个请求占住 worker 后，第二个请求必须
 * 立刻收到 NB_ERR_AGAIN，而不是排到无界队列里。
 */
static int run_overload_case(nb_transport_kind_t kind, const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    block_state_t block;
    client_state_t first;
    client_state_t second;
    nb_service_method_options_t worker = NB_SERVICE_METHOD_OPTIONS_INIT;
    nb_payload_t pl;
    uint16_t sport;
    uint16_t cport;
    int released = 0;
    int failed = 0;

    memset(&block, 0, sizeof(block));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    worker.exec = NB_SERVICE_EXEC_WORKER;
    sport = kind == NB_TRANSPORT_TCP ? 44611 : 44511;
    cport = kind == NB_TRANSPORT_TCP ? 44612 : 44512;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "overload-srv";
    scfg.transport.transport = kind;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 1;
    scfg.runtime.handler_max_concurrency = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "overload-cli";
    ccfg.transport.transport = kind;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        nb_log_error("[%s] node create failed", label);
        failed = 1;
        goto cleanup;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    block.started = ntk_sem_create(0);
    block.release = ntk_sem_create(0);
    first.sem = ntk_sem_create(0);
    second.sem = ntk_sem_create(0);
    if (!block.started || !block.release || !first.sem || !second.sem) {
        nb_log_error("[%s] sem create failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_service_create(server, "overload", &svc) != NB_OK ||
        nb_service_add_method_ex(svc, "block", blocking_handler, &block, &worker) != NB_OK) {
        nb_log_error("[%s] service setup failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK ||
        nb_node_wait_for_service(client, "overload", 5000) != NB_OK) {
        nb_log_error("[%s] client setup failed", label);
        failed = 1;
        goto cleanup;
    }

    pl.data = "overload-payload";
    pl.len = (size_t)strlen("overload-payload") + 1;
    pl.encoding = 0;
    if (nb_proxy_call_async(proxy, "overload", "block", &pl, on_response, &first, 5000, NULL) != NB_OK) {
        nb_log_error("[%s] first call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(block.started, 3000) != NTK_OK) {
        nb_log_error("[%s] worker did not start", label);
        failed = 1;
        goto cleanup;
    }

    if (nb_proxy_call_async(proxy, "overload", "block", &pl, on_response, &second, 5000, NULL) != NB_OK) {
        nb_log_error("[%s] second call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(second.sem, 2000) != NTK_OK) {
        nb_log_error("[%s] overloaded call was not rejected", label);
        failed = 1;
    } else if (second.status != NB_ERR_AGAIN) {
        nb_log_error("[%s] overloaded call status=%d (want %d)", label, second.status, NB_ERR_AGAIN);
        failed = 1;
    }

    ntk_sem_post(block.release, 1);
    released = 1;
    if (ntk_sem_timedwait(first.sem, 3000) != NTK_OK) {
        nb_log_error("[%s] first call timeout", label);
        failed = 1;
    } else if (first.status != NB_OK || first.app_status != NB_OK || first.len != pl.len ||
               memcmp(first.buf, "overload-payload", pl.len) != 0) {
        nb_log_error("[%s] first response bad status=%d app_status=%d len=%zu",
                      label,
                      first.status,
                      first.app_status,
                      first.len);
        failed = 1;
    }

cleanup:
    /* 失败路径也要放行阻塞 handler，否则 nb_node_destroy drain 时会卡住。 */
    if (block.release && !released) {
        ntk_sem_post(block.release, 1);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    if (block.started) {
        ntk_sem_destroy(block.started);
    }
    if (block.release) {
        ntk_sem_destroy(block.release);
    }
    if (first.sem) {
        ntk_sem_destroy(first.sem);
    }
    if (second.sem) {
        ntk_sem_destroy(second.sem);
    }
    return failed;
}

/*
 * 自定义 executor：服务端不配置内置 handler 池，worker 方法仍能被后台线程
 * 执行，且 nb_reply() 回投原 poller；submit 返回错误时客户端拿到该状态。
 */
static int run_executor_case(nb_transport_kind_t kind, const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    thread_executor_t te;
    nb_service_executor_t texec;
    nb_service_executor_t rexec;
    nb_service_method_options_t opts;
    client_state_t custom;
    client_state_t rejected;
    nb_payload_t pl;
    uint16_t sport = kind == NB_TRANSPORT_TCP ? 44811 : 44711;
    uint16_t cport = kind == NB_TRANSPORT_TCP ? 44812 : 44712;
    int te_started = 0;
    int failed = 0;

    memset(&te, 0, sizeof(te));
    memset(&custom, 0, sizeof(custom));
    memset(&rejected, 0, sizeof(rejected));

    if (thread_executor_start(&te) != 0) {
        nb_log_error("[%s] executor start failed", label);
        return 1;
    }
    te_started = 1;
    texec.ctx = &te;
    texec.submit = thread_executor_submit;
    rexec.ctx = NULL;
    rexec.submit = reject_executor_submit;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "exec-srv";
    scfg.transport.transport = kind;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 0; /* 证明自定义 executor 不依赖内置池 */
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "exec-cli";
    ccfg.transport.transport = kind;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        nb_log_error("[%s] node create failed", label);
        failed = 1;
        goto cleanup;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_service_create(server, "exec", &svc) != NB_OK) {
        nb_log_error("[%s] service create failed", label);
        failed = 1;
        goto cleanup;
    }
    opts = method_opts(NB_SERVICE_EXEC_EXECUTOR, &texec);
    if (nb_service_add_method_ex(svc, "custom", fast_handler, NULL, &opts) != NB_OK) {
        nb_log_error("[%s] custom registration failed", label);
        failed = 1;
        goto cleanup;
    }
    opts = method_opts(NB_SERVICE_EXEC_EXECUTOR, &rexec);
    if (nb_service_add_method_ex(svc, "reject", fast_handler, NULL, &opts) != NB_OK) {
        nb_log_error("[%s] reject registration failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK ||
        nb_node_wait_for_service(client, "exec", 5000) != NB_OK) {
        nb_log_error("[%s] client setup failed", label);
        failed = 1;
        goto cleanup;
    }

    custom.sem = ntk_sem_create(0);
    rejected.sem = ntk_sem_create(0);
    if (!custom.sem || !rejected.sem) {
        nb_log_error("[%s] sem create failed", label);
        failed = 1;
        goto cleanup;
    }

    pl.data = "executor-payload";
    pl.len = (size_t)strlen("executor-payload") + 1;
    pl.encoding = 0;
    if (nb_proxy_call_async(proxy, "exec", "custom", &pl, on_response, &custom, 5000, NULL) != NB_OK) {
        nb_log_error("[%s] custom call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(custom.sem, 3000) != NTK_OK) {
        nb_log_error("[%s] custom call timeout", label);
        failed = 1;
    } else if (custom.status != NB_OK || custom.app_status != NB_OK || custom.len != pl.len ||
               memcmp(custom.buf, "executor-payload", pl.len) != 0) {
        nb_log_error("[%s] custom response bad status=%d app_status=%d len=%zu",
                      label,
                      custom.status,
                      custom.app_status,
                      custom.len);
        failed = 1;
    } else if (te.submitted != 1) {
        nb_log_error("[%s] custom executor submitted=%d (want 1)", label, te.submitted);
        failed = 1;
    }

    if (nb_proxy_call_async(proxy, "exec", "reject", &pl, on_response, &rejected, 5000, NULL) != NB_OK) {
        nb_log_error("[%s] reject call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(rejected.sem, 3000) != NTK_OK) {
        nb_log_error("[%s] reject call timeout", label);
        failed = 1;
    } else if (rejected.status != NB_ERR_AGAIN) {
        nb_log_error("[%s] reject response bad status=%d", label, rejected.status);
        failed = 1;
    }

cleanup:
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    /* 先停 executor 再拆 server：满足“nb_node_destroy 前排空”的借用契约。 */
    if (te_started) {
        thread_executor_stop(&te);
    }
    if (server) {
        nb_node_destroy(server);
    }
    if (custom.sem) {
        ntk_sem_destroy(custom.sem);
    }
    if (rejected.sem) {
        ntk_sem_destroy(rejected.sem);
    }
    return failed;
}

/* ---- deadline / cancellation ---- */

typedef struct deadline_probe {
    volatile uint64_t req_deadline;
    volatile uint64_t call_deadline;
} deadline_probe_t;

static void deadline_probe_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    deadline_probe_t *p = (deadline_probe_t *)user;
    p->req_deadline = req->deadline_ms;
    p->call_deadline = nb_call_deadline_ms(call);
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

typedef struct run_counter {
    volatile int runs;
} run_counter_t;

static void run_count_handler(nb_call_t *call, const nb_request_t *req, void *user)
{
    run_counter_t *c = (run_counter_t *)user;
    c->runs++;
    nb_reply_bytes(call, 0, req->payload.data, req->payload.len);
}

/*
 * deadline：客户端 timeout 应传播到 req->deadline_ms / nb_call_deadline_ms；
 * 排队期间过期的 worker 任务在启动前被跳过，不执行 handler。
 */
static int run_deadline_case(const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    block_state_t block;
    deadline_probe_t probe;
    run_counter_t queued;
    client_state_t probe_resp;
    client_state_t block_resp;
    client_state_t queued_resp;
    nb_service_method_options_t worker = NB_SERVICE_METHOD_OPTIONS_INIT;
    nb_payload_t pl;
    uint16_t sport = 45111;
    uint16_t cport = 45112;
    int released = 0;
    int failed = 0;

    memset(&block, 0, sizeof(block));
    memset(&probe, 0, sizeof(probe));
    memset(&queued, 0, sizeof(queued));
    memset(&probe_resp, 0, sizeof(probe_resp));
    memset(&block_resp, 0, sizeof(block_resp));
    memset(&queued_resp, 0, sizeof(queued_resp));
    worker.exec = NB_SERVICE_EXEC_WORKER;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "deadline-srv";
    scfg.transport.transport = NB_TRANSPORT_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 1; /* 单 worker：第二个请求必然排队 */
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "deadline-cli";
    ccfg.transport.transport = NB_TRANSPORT_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    block.started = ntk_sem_create(0);
    block.release = ntk_sem_create(0);
    probe_resp.sem = ntk_sem_create(0);
    block_resp.sem = ntk_sem_create(0);
    queued_resp.sem = ntk_sem_create(0);
    if (!block.started || !block.release || !probe_resp.sem || !block_resp.sem ||
        !queued_resp.sem) {
        nb_log_error("[%s] sem create failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        nb_log_error("[%s] node create failed", label);
        failed = 1;
        goto cleanup;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_service_create(server, "deadline", &svc) != NB_OK ||
        nb_service_add_method_ex(svc, "probe", deadline_probe_handler, &probe, &worker) != NB_OK ||
        nb_service_add_method_ex(svc, "block", blocking_handler, &block, &worker) != NB_OK ||
        nb_service_add_method_ex(svc, "queued", run_count_handler, &queued, &worker) != NB_OK) {
        nb_log_error("[%s] service setup failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK ||
        nb_node_wait_for_service(client, "deadline", 5000) != NB_OK) {
        nb_log_error("[%s] client setup failed", label);
        failed = 1;
        goto cleanup;
    }

    pl.data = "deadline";
    pl.len = (size_t)strlen("deadline") + 1;
    pl.encoding = 0;

    /* 1) deadline 传播：worker 里读到的两个视图应一致且非 0。 */
    if (nb_proxy_call_async(proxy, "deadline", "probe", &pl, on_response, &probe_resp, 3000, NULL) !=
        NB_OK) {
        nb_log_error("[%s] probe call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(probe_resp.sem, 3000) != NTK_OK || probe_resp.status != NB_OK) {
        nb_log_error("[%s] probe timeout/status=%d", label, probe_resp.status);
        failed = 1;
    } else if (probe.req_deadline == 0 || probe.call_deadline != probe.req_deadline) {
        nb_log_error("[%s] deadline not propagated req=%llu call=%llu",
                      label,
                      (unsigned long long)probe.req_deadline,
                      (unsigned long long)probe.call_deadline);
        failed = 1;
    }

    /* 2) 占住唯一 worker，再发一个短 deadline 的请求排在其后。 */
    if (nb_proxy_call_async(proxy, "deadline", "block", &pl, on_response, &block_resp, 5000, NULL) !=
        NB_OK) {
        nb_log_error("[%s] block call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(block.started, 3000) != NTK_OK) {
        nb_log_error("[%s] block handler did not start", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_call_async(proxy, "deadline", "queued", &pl, on_response, &queued_resp, 60, NULL) !=
        NB_OK) {
        nb_log_error("[%s] queued call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(queued_resp.sem, 2000) != NTK_OK) {
        nb_log_error("[%s] queued deadline did not fire", label);
        failed = 1;
    } else if (queued_resp.status != NB_ERR_TIMEOUT) {
        nb_log_error("[%s] queued status=%d (want timeout)", label, queued_resp.status);
        failed = 1;
    }

    /* 确保服务端 deadline 已过，再放行 blocker；排队的任务应被跳过。 */
    ntk_sleep_ms(150);
    ntk_sem_post(block.release, 1);
    released = 1;
    if (ntk_sem_timedwait(block_resp.sem, 3000) != NTK_OK || block_resp.status != NB_OK) {
        nb_log_error("[%s] block response timeout/status=%d", label, block_resp.status);
        failed = 1;
    }
    ntk_sleep_ms(300);
    if (queued.runs != 0) {
        nb_log_error("[%s] expired handler ran %d time(s)", label, queued.runs);
        failed = 1;
    }

cleanup:
    if (block.release && !released) {
        ntk_sem_post(block.release, 1);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    if (block.started) {
        ntk_sem_destroy(block.started);
    }
    if (block.release) {
        ntk_sem_destroy(block.release);
    }
    if (probe_resp.sem) {
        ntk_sem_destroy(probe_resp.sem);
    }
    if (block_resp.sem) {
        ntk_sem_destroy(block_resp.sem);
    }
    if (queued_resp.sem) {
        ntk_sem_destroy(queued_resp.sem);
    }
    return failed;
}

/*
 * cancellation：客户端 nb_proxy_cancel 应把 CANCEL 传播到服务端；排队中的 worker
 * 任务在启动前被跳过，客户端立即收到取消回调。
 */
static int run_cancel_case(const char *label)
{
    nb_node_config_t scfg;
    nb_node_config_t ccfg;
    nb_node_t *server = NULL;
    nb_node_t *client = NULL;
    nb_service_t *svc = NULL;
    nb_proxy_t *proxy = NULL;
    block_state_t block;
    run_counter_t queued;
    client_state_t block_resp;
    client_state_t queued_resp;
    client_state_t ping_resp;
    nb_service_method_options_t worker = NB_SERVICE_METHOD_OPTIONS_INIT;
    nb_payload_t pl;
    uint16_t sport = 45211;
    uint16_t cport = 45212;
    uint64_t queued_id = 0;
    int released = 0;
    int failed = 0;

    memset(&block, 0, sizeof(block));
    memset(&queued, 0, sizeof(queued));
    memset(&block_resp, 0, sizeof(block_resp));
    memset(&queued_resp, 0, sizeof(queued_resp));
    memset(&ping_resp, 0, sizeof(ping_resp));
    worker.exec = NB_SERVICE_EXEC_WORKER;

    memset(&scfg, 0, sizeof(scfg));
    scfg.name = "cancel-srv";
    scfg.transport.transport = NB_TRANSPORT_TCP;
    scfg.transport.bind_host = "127.0.0.1";
    scfg.discovery.host = "127.0.0.1";
    scfg.discovery.port = sport;
    scfg.runtime.io_threads = 1;
    scfg.runtime.handler_threads = 1;
    scfg.discovery.hello_interval_ms = 200;
    scfg.discovery.node_ttl_ms = 3000;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.name = "cancel-cli";
    ccfg.transport.transport = NB_TRANSPORT_TCP;
    ccfg.transport.bind_host = "127.0.0.1";
    ccfg.discovery.host = "127.0.0.1";
    ccfg.discovery.port = cport;
    ccfg.runtime.io_threads = 1;
    ccfg.discovery.hello_interval_ms = 200;
    ccfg.discovery.node_ttl_ms = 3000;

    block.started = ntk_sem_create(0);
    block.release = ntk_sem_create(0);
    block_resp.sem = ntk_sem_create(0);
    queued_resp.sem = ntk_sem_create(0);
    ping_resp.sem = ntk_sem_create(0);
    if (!block.started || !block.release || !block_resp.sem || !queued_resp.sem || !ping_resp.sem) {
        nb_log_error("[%s] sem create failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_node_create(&scfg, &server) != NB_OK || nb_node_create(&ccfg, &client) != NB_OK) {
        nb_log_error("[%s] node create failed", label);
        failed = 1;
        goto cleanup;
    }
    (void)nb_node_add_discovery_peer(server, "127.0.0.1", cport);
    (void)nb_node_add_discovery_peer(client, "127.0.0.1", sport);

    if (nb_service_create(server, "cancel", &svc) != NB_OK ||
        nb_service_add_method_ex(svc, "block", blocking_handler, &block, &worker) != NB_OK ||
        nb_service_add_method_ex(svc, "queued", run_count_handler, &queued, &worker) != NB_OK ||
        nb_service_add_method(svc, "ping", fast_handler, NULL) != NB_OK) {
        nb_log_error("[%s] service setup failed", label);
        failed = 1;
        goto cleanup;
    }
    if (nb_proxy_create(client, NULL, &proxy) != NB_OK ||
        nb_node_wait_for_service(client, "cancel", 5000) != NB_OK) {
        nb_log_error("[%s] client setup failed", label);
        failed = 1;
        goto cleanup;
    }

    pl.data = "cancel";
    pl.len = (size_t)strlen("cancel") + 1;
    pl.encoding = 0;

    /* 占住唯一 worker。 */
    if (nb_proxy_call_async(proxy, "cancel", "block", &pl, on_response, &block_resp, 5000, NULL) !=
        NB_OK) {
        nb_log_error("[%s] block call failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(block.started, 3000) != NTK_OK) {
        nb_log_error("[%s] block handler did not start", label);
        failed = 1;
        goto cleanup;
    }

    /* 第二个请求排在 worker 队列里。 */
    if (nb_proxy_call_async(
            proxy, "cancel", "queued", &pl, on_response, &queued_resp, 5000, &queued_id) != NB_OK) {
        nb_log_error("[%s] queued call failed", label);
        failed = 1;
        goto cleanup;
    }
    /* TCP 有序：ping 响应返回说明 queued 已被服务端接收并派发。 */
    if (nb_proxy_call_async(proxy, "cancel", "ping", &pl, on_response, &ping_resp, 3000, NULL) != NB_OK ||
        ntk_sem_timedwait(ping_resp.sem, 3000) != NTK_OK || ping_resp.status != NB_OK) {
        nb_log_error("[%s] ping before cancel failed status=%d", label, ping_resp.status);
        failed = 1;
        goto cleanup;
    }

    if (nb_proxy_cancel(proxy, queued_id) != NB_OK) {
        nb_log_error("[%s] nb_proxy_cancel failed", label);
        failed = 1;
        goto cleanup;
    }
    if (ntk_sem_timedwait(queued_resp.sem, 2000) != NTK_OK) {
        nb_log_error("[%s] cancel callback missing", label);
        failed = 1;
    } else if (queued_resp.status != NB_ERR_NOTFOUND) {
        nb_log_error("[%s] cancel status=%d (want notfound)", label, queued_resp.status);
        failed = 1;
    }

    /* 再 ping 一次，确保服务端 poller 已处理 CANCEL。 */
    {
        client_state_t ping2;
        memset(&ping2, 0, sizeof(ping2));
        ping2.sem = ntk_sem_create(0);
        if (!ping2.sem ||
            nb_proxy_call_async(proxy, "cancel", "ping", &pl, on_response, &ping2, 3000, NULL) != NB_OK ||
            ntk_sem_timedwait(ping2.sem, 3000) != NTK_OK || ping2.status != NB_OK) {
            nb_log_error("[%s] ping after cancel failed status=%d", label, ping2.status);
            failed = 1;
        }
        if (ping2.sem) {
            ntk_sem_destroy(ping2.sem);
        }
    }

    ntk_sem_post(block.release, 1);
    released = 1;
    if (ntk_sem_timedwait(block_resp.sem, 3000) != NTK_OK || block_resp.status != NB_OK) {
        nb_log_error("[%s] block response timeout/status=%d", label, block_resp.status);
        failed = 1;
    }
    ntk_sleep_ms(300);
    if (queued.runs != 0) {
        nb_log_error("[%s] canceled handler ran %d time(s)", label, queued.runs);
        failed = 1;
    }

cleanup:
    if (block.release && !released) {
        ntk_sem_post(block.release, 1);
    }
    if (proxy) {
        nb_proxy_destroy(proxy);
    }
    if (client) {
        nb_node_destroy(client);
    }
    if (server) {
        nb_node_destroy(server);
    }
    if (block.started) {
        ntk_sem_destroy(block.started);
    }
    if (block.release) {
        ntk_sem_destroy(block.release);
    }
    if (block_resp.sem) {
        ntk_sem_destroy(block_resp.sem);
    }
    if (queued_resp.sem) {
        ntk_sem_destroy(queued_resp.sem);
    }
    if (ping_resp.sem) {
        ntk_sem_destroy(ping_resp.sem);
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    failed += run_case(NB_TRANSPORT_UDP, "udp");
    failed += run_case(NB_TRANSPORT_TCP, "tcp");
    failed += run_fallback_case(NB_TRANSPORT_UDP, "udp-fallback");
    failed += run_fallback_case(NB_TRANSPORT_TCP, "tcp-fallback");
    failed += run_overload_case(NB_TRANSPORT_UDP, "udp-overload");
    failed += run_overload_case(NB_TRANSPORT_TCP, "tcp-overload");
    failed += run_executor_case(NB_TRANSPORT_TCP, "tcp-executor");
    failed += run_deadline_case("tcp-deadline");
    failed += run_cancel_case("tcp-cancel");
    failed += run_api_checks();
    if (failed) {
        nb_log_error("worker: FAIL");
        return 1;
    }
    nb_log_info("worker: NB_OK");
    return 0;
}
