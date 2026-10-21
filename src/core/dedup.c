#include "dedup.h"

#include "envelope.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static zri_dedup_shard_t *shard_for(zri_dedup_t *d, uint32_t request_id)
{
    return &d->shards[request_id % ZRI_DEDUP_SHARDS];
}

static void slot_free(zri_dedup_shard_t *sh, zri_dedup_slot_t *s)
{
    if (s->payload) {
        sh->bytes -= s->payload_len;
        free(s->payload);
    }
    if (s->metadata) {
        sh->bytes -= s->metadata_len;
        free(s->metadata);
    }
    if (s->message) {
        sh->bytes -= s->message_len;
        free(s->message);
    }
    memset(s, 0, sizeof(*s));
}

static int slot_matches(const zri_dedup_slot_t *s, const zri_message_t *req)
{
    return s->used && s->request_id == req->request_id && s->peer_port == req->peer_port &&
        strcmp(s->peer_ip, req->peer_ip ? req->peer_ip : "") == 0;
}

static zri_dedup_slot_t *slot_find(zri_dedup_shard_t *sh, const zri_message_t *req)
{
    int i;
    for (i = 0; i < ZRI_DEDUP_SLOTS_PER_SHARD; i++) {
        if (slot_matches(&sh->slots[i], req)) {
            return &sh->slots[i];
        }
    }
    return NULL;
}

/* 在持锁状态下重放缓存响应。 */
static void dedup_replay_locked(zri_transport_t *transport,
                                const zri_message_t *req,
                                const zri_dedup_slot_t *s)
{
    zri_message_t reply;
    payload_t pl;
    reply = *req;
    reply.kind = KIND_RESPONSE;
    reply.flags = 0;
    reply.status = s->status;
    reply.route = NULL;
    reply.metadata = s->metadata;
    reply.metadata_len = s->metadata_len;
    reply.status_msg = s->message;
    reply.status_msg_len = s->message_len;
    reply.has_stream = 0;
    pl.data = s->payload;
    pl.len = s->payload_len;
    pl.encoding = s->encoding;
    reply.payload = pl;
    (void)zri_transport_reply(transport, &reply);
}

void zri_dedup_init(zri_dedup_t *d)
{
    int i;
    if (!d) {
        return;
    }
    memset(d, 0, sizeof(*d));
    for (i = 0; i < ZRI_DEDUP_SHARDS; i++) {
        d->shards[i].lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    }
}

void zri_dedup_cleanup(zri_dedup_t *d)
{
    int i;
    int j;
    if (!d) {
        return;
    }
    for (i = 0; i < ZRI_DEDUP_SHARDS; i++) {
        zri_dedup_shard_t *sh = &d->shards[i];
        if (sh->lock) {
            ntk_mutex_lock(sh->lock);
        }
        for (j = 0; j < ZRI_DEDUP_SLOTS_PER_SHARD; j++) {
            if (sh->slots[j].used) {
                slot_free(sh, &sh->slots[j]);
            }
        }
        if (sh->lock) {
            ntk_mutex_unlock(sh->lock);
            ntk_mutex_destroy(sh->lock);
            sh->lock = NULL;
        }
    }
}

int zri_dedup_begin(zri_dedup_t *d,
                    zri_transport_t *transport,
                    const zri_message_t *req,
                    int *state)
{
    zri_dedup_shard_t *sh;
    zri_dedup_slot_t *s;
    zri_dedup_slot_t *victim = NULL;
    uint64_t now = zri_now_ms();
    int i;

    if (!d || !transport || !req || !state) {
        return ERR_INVALID;
    }
    *state = 0;
    sh = shard_for(d, req->request_id);
    if (!sh->lock) {
        return OK;
    }
    ntk_mutex_lock(sh->lock);
    for (i = 0; i < ZRI_DEDUP_SLOTS_PER_SHARD; i++) {
        s = &sh->slots[i];
        if (s->used && s->expires_ms <= now) {
            slot_free(sh, s);
        }
        if (slot_matches(s, req)) {
            if (s->completed) {
                dedup_replay_locked(transport, req, s);
                *state = 1;
            } else {
                *state = 2;
            }
            ntk_mutex_unlock(sh->lock);
            return OK;
        }
    }
    for (i = 0; i < ZRI_DEDUP_SLOTS_PER_SHARD; i++) {
        if (!sh->slots[i].used) {
            victim = &sh->slots[i];
            break;
        }
        if (!victim || sh->slots[i].last_ms < victim->last_ms) {
            victim = &sh->slots[i];
        }
    }
    if (victim) {
        slot_free(sh, victim);
        victim->used = 1;
        victim->completed = 0;
        victim->request_id = req->request_id;
        zri_copy_str(victim->peer_ip, sizeof(victim->peer_ip), req->peer_ip, "");
        victim->peer_port = req->peer_port;
        victim->expires_ms = now + ZRI_DEDUP_TTL_MS;
        victim->last_ms = now;
    }
    ntk_mutex_unlock(sh->lock);
    return OK;
}

void zri_dedup_complete(zri_dedup_t *d,
                        zri_transport_t *transport,
                        const zri_message_t *req,
                        int status,
                        const payload_t *payload,
                        const void *metadata,
                        uint16_t metadata_len,
                        const char *message,
                        uint16_t message_len)
{
    zri_dedup_shard_t *sh;
    zri_dedup_slot_t *s;
    size_t plen = (payload && payload->len > 0) ? payload->len : 0;
    size_t need;
    uint64_t now = zri_now_ms();
    int i;

    (void)transport;
    if (!d || !req) {
        return;
    }
    sh = shard_for(d, req->request_id);
    if (!sh->lock) {
        return;
    }
    need = plen + metadata_len + message_len;
    if (need > ZRI_DEDUP_MAX_BYTES) {
        return; /* 太大，不缓存 */
    }
    ntk_mutex_lock(sh->lock);
    s = slot_find(sh, req);
    if (!s || s->completed) {
        ntk_mutex_unlock(sh->lock);
        return;
    }
    while (sh->bytes + need > ZRI_DEDUP_MAX_BYTES) {
        zri_dedup_slot_t *oldest = NULL;
        for (i = 0; i < ZRI_DEDUP_SLOTS_PER_SHARD; i++) {
            if (&sh->slots[i] == s || !sh->slots[i].used) {
                continue;
            }
            if (!oldest || sh->slots[i].last_ms < oldest->last_ms) {
                oldest = &sh->slots[i];
            }
        }
        if (!oldest) {
            break;
        }
        slot_free(sh, oldest);
    }
    if (plen > 0 && payload && payload->data) {
        s->payload = (uint8_t *)malloc(plen);
        if (!s->payload) {
            slot_free(sh, s);
            ntk_mutex_unlock(sh->lock);
            return;
        }
        memcpy(s->payload, payload->data, plen);
        s->payload_len = plen;
        sh->bytes += plen;
    }
    if (metadata_len > 0 && metadata) {
        s->metadata = (uint8_t *)malloc(metadata_len);
        if (!s->metadata) {
            slot_free(sh, s);
            ntk_mutex_unlock(sh->lock);
            return;
        }
        memcpy(s->metadata, metadata, metadata_len);
        s->metadata_len = metadata_len;
        sh->bytes += metadata_len;
    }
    if (message_len > 0 && message) {
        s->message = (char *)malloc((size_t)message_len + 1);
        if (!s->message) {
            slot_free(sh, s);
            ntk_mutex_unlock(sh->lock);
            return;
        }
        memcpy(s->message, message, message_len);
        s->message[message_len] = '\0';
        s->message_len = message_len;
        sh->bytes += message_len;
    }
    s->status = status;
    s->encoding = payload ? payload->encoding : 0;
    s->completed = 1;
    s->last_ms = now;
    s->expires_ms = now + ZRI_DEDUP_TTL_MS;
    ntk_mutex_unlock(sh->lock);
}

void zri_dedup_abort(zri_dedup_t *d, const zri_message_t *req)
{
    zri_dedup_shard_t *sh;
    zri_dedup_slot_t *s;
    if (!d || !req) {
        return;
    }
    sh = shard_for(d, req->request_id);
    if (!sh->lock) {
        return;
    }
    ntk_mutex_lock(sh->lock);
    s = slot_find(sh, req);
    if (s) {
        slot_free(sh, s);
    }
    ntk_mutex_unlock(sh->lock);
}

void zri_dedup_expire(zri_dedup_t *d, uint64_t now_ms)
{
    int i;
    int j;
    if (!d) {
        return;
    }
    for (i = 0; i < ZRI_DEDUP_SHARDS; i++) {
        zri_dedup_shard_t *sh = &d->shards[i];
        if (!sh->lock) {
            continue;
        }
        ntk_mutex_lock(sh->lock);
        for (j = 0; j < ZRI_DEDUP_SLOTS_PER_SHARD; j++) {
            if (sh->slots[j].used && sh->slots[j].expires_ms <= now_ms) {
                slot_free(sh, &sh->slots[j]);
            }
        }
        ntk_mutex_unlock(sh->lock);
    }
}
