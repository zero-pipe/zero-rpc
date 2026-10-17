/* 统一消息视图：请求/响应共用的 payload 载体。 */
#include <zrpc/zrpc_message_view.h>

#include <stdlib.h>
#include <string.h>

/* ---- 生命周期 ---- */

void zrpc_message_view_init(zrpc_message_view_t *view) {
    if (!view) {
        return;
    }
    view->data = NULL;
    view->len = 0;
    view->encoding = 0;
    view->owned = NULL;
}

void zrpc_message_view_release(zrpc_message_view_t *view) {
    if (!view) {
        return;
    }
    if (view->owned) {
        free(view->owned);
        view->owned = NULL;
    }
    view->data = NULL;
    view->len = 0;
}

/* ---- 写入 ---- */

int zrpc_message_view_set_borrowed(zrpc_message_view_t *view, const void *data, size_t len) {
    if (!view || (len > 0 && !data)) {
        return ZRPC_ERR_INVALID;
    }
    if (view->owned) {
        free(view->owned);
        view->owned = NULL;
    }
    view->data = data;
    view->len = len;
    return ZRPC_OK;
}

int zrpc_message_view_set_data(zrpc_message_view_t *view, const void *data, size_t len) {
    void *copy = NULL;
    if (!view || (len > 0 && !data)) {
        return ZRPC_ERR_INVALID;
    }
    if (len > 0) {
        copy = malloc(len);
        if (!copy) {
            return ZRPC_ERR_NOMEM;
        }
        memcpy(copy, data, len);
    }
    if (view->owned) {
        free(view->owned);
    }
    view->owned = copy;
    view->data = copy;
    view->len = len;
    return ZRPC_OK;
}

int zrpc_message_view_set_string(zrpc_message_view_t *view, const char *text) {
    if (!text) {
        return ZRPC_ERR_INVALID;
    }
    return zrpc_message_view_set_data(view, text, strlen(text));
}

int zrpc_message_view_set_payload(zrpc_message_view_t *view, const zrpc_payload_t *payload) {
    int rc;
    if (!view) {
        return ZRPC_ERR_INVALID;
    }
    if (!payload) {
        if (view->owned) {
            free(view->owned);
            view->owned = NULL;
        }
        view->data = NULL;
        view->len = 0;
        view->encoding = 0;
        return ZRPC_OK;
    }
    rc = zrpc_message_view_set_data(view, payload->data, payload->len);
    if (rc == ZRPC_OK) {
        view->encoding = payload->encoding;
    }
    return rc;
}

/* ---- 导出 ---- */

void zrpc_message_view_to_payload(const zrpc_message_view_t *view, zrpc_payload_t *out) {
    if (!out) {
        return;
    }
    if (!view) {
        out->data = NULL;
        out->len = 0;
        out->encoding = 0;
        return;
    }
    out->data = view->data;
    out->len = view->len;
    out->encoding = view->encoding;
}
