#include "dedup.h"

#include "envelope.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static nb_dedup_shard_t *nb_shard_for(nb_dedup_t *d, uint32_t request_id)
{
    return &d->shards[request_id % NB_DEDUP_SHARDS];
}

static void nb_slot_free(nb_dedup_shard_t *sh, nb_dedup_slot_t *s)
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

static int nb_slot_matches(const nb_dedup_slot_t *s, const nb_message_t *req)
{
    return s->used && s->request_id == req->request_id && s->peer_port == req->peer_port &&
        strcmp(s->peer_ip, req->peer_ip ? req->peer_ip : "") == 0;
}

static nb_dedup_slot_t *nb_slot_find(nb_dedup_shard_t *sh, const nb_message_t *req)
{
    int i;
    for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
        if (nb_slot_matches(&sh->slots[i], req)) {
            return &sh->slots[i];
        }
    }
    return NULL;
}

/* 在持锁状态下重放缓存响应。 */
static void nb_dedup_replay_locked(nb_transport_t *transport,
                                const nb_message_t *req,
                                const nb_dedup_slot_t *s)
{
    nb_message_t nb_reply;
    nb_payload_t pl;
    nb_reply = *req;
    nb_reply.kind = KIND_RESPONSE;
    nb_reply.flags = 0;
    nb_reply.status = s->status;
    nb_reply.route = NULL;
    nb_reply.metadata = s->metadata;
    nb_reply.metadata_len = s->metadata_len;
    nb_reply.status_msg = s->message;
    nb_reply.status_msg_len = s->message_len;
    nb_reply.has_stream = 0;
    pl.data = s->payload;
    pl.len = s->payload_len;
    pl.encoding = s->encoding;
    nb_reply.payload = pl;
    (void)nb_transport_reply(transport, &nb_reply);
}

void nb_dedup_init(nb_dedup_t *d)
{
    int i;
    if (!d) {
        return;
    }
    memset(d, 0, sizeof(*d));
    for (i = 0; i < NB_DEDUP_SHARDS; i++) {
        d->shards[i].lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    }
}

void nb_dedup_cleanup(nb_dedup_t *d)
{
    int i;
    int j;
    if (!d) {
        return;
    }
    for (i = 0; i < NB_DEDUP_SHARDS; i++) {
        nb_dedup_shard_t *sh = &d->shards[i];
        if (sh->lock) {
            ntk_mutex_lock(sh->lock);
        }
        for (j = 0; j < NB_DEDUP_SLOTS_PER_SHARD; j++) {
            if (sh->slots[j].used) {
                nb_slot_free(sh, &sh->slots[j]);
            }
        }
        if (sh->lock) {
            ntk_mutex_unlock(sh->lock);
            ntk_mutex_destroy(sh->lock);
            sh->lock = NULL;
        }
    }
}

int nb_dedup_begin(nb_dedup_t *d,
                    nb_transport_t *transport,
                    const nb_message_t *req,
                    int *state)
{
    nb_dedup_shard_t *sh;
    nb_dedup_slot_t *s;
    nb_dedup_slot_t *victim = NULL;
    uint64_t now = nb_now_ms();
    int i;

    if (!d || !transport || !req || !state) {
        return NB_ERR_INVALID;
    }
    *state = 0;
    sh = nb_shard_for(d, req->request_id);
    if (!sh->lock) {
        return NB_OK;
    }
    ntk_mutex_lock(sh->lock);
    for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
        s = &sh->slots[i];
        if (s->used && s->expires_ms <= now) {
            nb_slot_free(sh, s);
        }
        if (nb_slot_matches(s, req)) {
            if (s->completed) {
                nb_dedup_replay_locked(transport, req, s);
                *state = 1;
            } else {
                *state = 2;
            }
            ntk_mutex_unlock(sh->lock);
            return NB_OK;
        }
    }
    for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
        if (!sh->slots[i].used) {
            victim = &sh->slots[i];
            break;
        }
        if (!victim || sh->slots[i].last_ms < victim->last_ms) {
            victim = &sh->slots[i];
        }
    }
    if (victim) {
        nb_slot_free(sh, victim);
        victim->used = 1;
        victim->completed = 0;
        victim->request_id = req->request_id;
        nb_copy_str(victim->peer_ip, sizeof(victim->peer_ip), req->peer_ip, "");
        victim->peer_port = req->peer_port;
        victim->expires_ms = now + NB_DEDUP_TTL_MS;
        victim->last_ms = now;
    }
    ntk_mutex_unlock(sh->lock);
    return NB_OK;
}

void nb_dedup_complete(nb_dedup_t *d,
                        nb_transport_t *transport,
                        const nb_message_t *req,
                        int status,
                        const nb_payload_t *payload,
                        const void *metadata,
                        uint16_t metadata_len,
                        const char *message,
                        uint16_t message_len)
{
    nb_dedup_shard_t *sh;
    nb_dedup_slot_t *s;
    size_t plen = (payload && payload->len > 0) ? payload->len : 0;
    size_t need;
    uint64_t now = nb_now_ms();
    int i;

    (void)transport;
    if (!d || !req) {
        return;
    }
    sh = nb_shard_for(d, req->request_id);
    if (!sh->lock) {
        return;
    }
    need = plen + metadata_len + message_len;
    if (need > NB_DEDUP_MAX_BYTES) {
        return; /* 太大，不缓存 */
    }
    ntk_mutex_lock(sh->lock);
    s = nb_slot_find(sh, req);
    if (!s || s->completed) {
        ntk_mutex_unlock(sh->lock);
        return;
    }
    while (sh->bytes + need > NB_DEDUP_MAX_BYTES) {
        nb_dedup_slot_t *oldest = NULL;
        for (i = 0; i < NB_DEDUP_SLOTS_PER_SHARD; i++) {
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
        nb_slot_free(sh, oldest);
    }
    if (plen > 0 && payload && payload->data) {
        s->payload = (uint8_t *)malloc(plen);
        if (!s->payload) {
            nb_slot_free(sh, s);
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
            nb_slot_free(sh, s);
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
            nb_slot_free(sh, s);
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
    s->expires_ms = now + NB_DEDUP_TTL_MS;
    ntk_mutex_unlock(sh->lock);
}

void nb_dedup_abort(nb_dedup_t *d, const nb_message_t *req)
{
    nb_dedup_shard_t *sh;
    nb_dedup_slot_t *s;
    if (!d || !req) {
        return;
    }
    sh = nb_shard_for(d, req->request_id);
    if (!sh->lock) {
        return;
    }
    ntk_mutex_lock(sh->lock);
    s = nb_slot_find(sh, req);
    if (s) {
        nb_slot_free(sh, s);
    }
    ntk_mutex_unlock(sh->lock);
}

void nb_dedup_expire(nb_dedup_t *d, uint64_t now_ms)
{
    int i;
    int j;
    if (!d) {
        return;
    }
    for (i = 0; i < NB_DEDUP_SHARDS; i++) {
        nb_dedup_shard_t *sh = &d->shards[i];
        if (!sh->lock) {
            continue;
        }
        ntk_mutex_lock(sh->lock);
        for (j = 0; j < NB_DEDUP_SLOTS_PER_SHARD; j++) {
            if (sh->slots[j].used && sh->slots[j].expires_ms <= now_ms) {
                nb_slot_free(sh, &sh->slots[j]);
            }
        }
        ntk_mutex_unlock(sh->lock);
    }
}
