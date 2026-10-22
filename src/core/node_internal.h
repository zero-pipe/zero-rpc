#ifndef NODE_INTERNAL_H
#define NODE_INTERNAL_H

/*
 * 节点内部结构：节点与 codec 注册项。
 *
 * 这是各子系统的“组合根”：node 内嵌 pending / recv_streams / 发现状态 / 路由 /
 * registry / 传输等存储，因此本头聚合了这些子系统的定义。其它模块应尽量通过
 * 访问器/专用头与本模块交互，而不是各自穿透 node 字段。
 */

#include <types.h>
#include <node.h>
#include <codec.h>
#include <stub.h>
#include <ntk.h>

#include <stddef.h>
#include <stdint.h>

#include "caps.h"
#include "dedup.h"
#include "discovery.h"
#include "pending.h"
#include "registry.h"
#include "registry_backend.h"
#include "router.h"
#include "service_internal.h"
#include "transport_internal.h"

/* codec 注册项 */
typedef struct zri_codec_entry {
    struct zri_codec_entry *next;
    codec_t codec;
} zri_codec_entry_t;

/*
 * 极简定长对象复用池。trylock 抢不到锁就直接 malloc/free，保证池只是优化，
 * 不会在热路径上引入阻塞或锁竞争尾延迟。
 */
typedef struct zri_obj_pool {
    void *free_list;
    ntk_mutex *lock;
    unsigned count;
    unsigned limit;
} zri_obj_pool_t;

struct node {
    node_config_t cfg;
    char name[ZRI_NAME_MAX];
    uint32_t node_id;
    char bind_host[ENDPOINT_MAX];
    char discovery_host[ENDPOINT_MAX];
    uint16_t discovery_port;
    unsigned io_threads;
    uint32_t hello_interval_ms;
    uint32_t node_ttl_ms;
    uint16_t frag_bytes;
    uint32_t max_msg_bytes;
    uint32_t bp_high_water;
    uint8_t drop_percent; /* 测试用：UDP 出站丢包百分比 */
    uint32_t rtcp_interval_ms;
    int rtcp_disabled;

    ntk_poller_pool *pool;
    ntk_poller *io_pollers[IO_MAX];   /* 出站分片：按 request_id 固定到某个 poller */
    unsigned io_count;                /* io_pollers 有效个数（>= 1） */
    zri_pending_t *pending_lanes[IO_MAX]; /* 每 poller 的 pending lane（owner-only 访问） */
    ntk_mutex *pending_lock;          /* 保护 pending_hash 的跨 poller 查找 */
    ntk_timer *pending_timers[IO_MAX]; /* 每 poller 的 pending 超时/重传定时器 */
    ntk_poller *poller;               /* io 0，用于发现/客户端调用 */
    ntk_thread_pool *handler_pool;    /* 可选：业务 handler 线程池（0 = 关闭） */
    int worker_fallback_warned;       /* handler_threads=0 时只告警一次 */
    ntk_mutex *handler_limit_mutex;   /* 保护 handler_inflight / external_inflight */
    unsigned handler_max_concurrency; /* 0 = 不限制 */
    unsigned handler_inflight;        /* worker 排队 + 执行中的任务数 */
    unsigned external_inflight;       /* 自定义 executor 未完成任务数（诊断用） */
    uint32_t max_inflight;             /* 客户端 pending 上限 */
    unsigned pending_count;            /* 当前未完成调用数（跨 poller 原子访问） */
    int shutting_down;                 /* node_destroy 已开始 */
    zri_obj_pool_t worker_call_pool;  /* worker 路径 call_t 复用池 */
    zri_obj_pool_t handler_task_pool; /* worker/executor 任务头复用池 */
    zri_obj_pool_t reply_task_pool;   /* 跨线程回复任务头复用池 */
    zri_transport_t *transports[MAX_BINDINGS];
    size_t transport_count;
    ntk_timer *hello_timer;
    ntk_timer *node_timer;

    service_t *services;
    ntk_mutex *services_lock; /* 保护 services/methods 链表（注册 vs 请求分发） */
    ntk_mutex *codecs_lock;   /* 保护 codecs 链表 */
    zri_discovery_state_t discovery;
    zri_pending_t *pending;      /* 全部 pending 槽位（按需分配） */
    zri_pending_t *pending_hash; /* request_id -> pending，O(1) 查找 */
    zri_pending_t *pending_free; /* 可复用的空闲槽位 */
    zri_dedup_t dedup;           /* 服务端请求去重 / 响应重放（UDP unary） */
    zri_recv_stream_t recv_streams[MAX_RECV_STREAMS];
    zri_cancel_slot_t cancels[CANCEL_SLOTS]; /* 在途调用取消登记（best-effort） */
    unsigned cancel_count;                   /* 已登记取消数；原子访问，0 时查询走快路径 */
    zri_codec_entry_t *codecs;

    node_mode_t mode;
    zri_route_table_t routes;
    zri_registry_t registry;
    const zri_registry_backend_t *registry_backend;
    metrics_t metrics;

    ntk_mutex *id_lock;
    uint64_t next_request_id;
    int started;
};

/* node.c */
uint64_t zri_now_ms(void);
/* 出站分片：按 key 稳定选择 poller；未启用池时回退到 node->poller。 */
ntk_poller *zri_node_pick_poller(const node_t *node, uint64_t key);
/* poller 在 node 出站分片中的下标；找不到返回 -1。 */
int zri_node_poller_index(const node_t *node, ntk_poller *poller);
void zri_node_on_message(node_t *node, zri_transport_t *transport, const zri_message_t *message);
int zri_node_send(node_t *node,
                  transport_kind_t transport_kind,
                  const char *scheme,
                  const zri_message_t *message);
/* 组装 "service.method" 路由并发送一次调用（stub/proxy/重传共用）。 */
int zri_node_send_call(node_t *node,
                       const stub_target_t *target,
                       const char *service,
                       const char *method,
                       const payload_t *payload,
                       uint8_t kind,
                       uint64_t request_id,
                       uint64_t timeout_ms,
                       const void *metadata,
                       uint16_t metadata_len);
/* 默认解析：静态路由表 / 服务发现（不含 proxy 的端点覆盖）。 */
int zri_node_resolve(node_t *node, const char *service, int prefer_stream, stub_target_t *out);

/* 定长对象复用池：trylock 失败即回退 malloc/free，不阻塞。 */
int zri_obj_pool_init(zri_obj_pool_t *pool, unsigned limit);
void zri_obj_pool_destroy(zri_obj_pool_t *pool);
void *zri_obj_pool_alloc(zri_obj_pool_t *pool, size_t size);
void zri_obj_pool_free(zri_obj_pool_t *pool, void *obj);

#endif /* NODE_INTERNAL_H */
