/* 统一消息视图：请求/响应共用的 payload 载体。 */
#include <message_view.h>

#include <stdlib.h>
#include <string.h>

/* ---- 生命周期 ---- */

void nb_message_view_init(nb_message_view_t *view)
{
    if (!view) {
        return;
    }
    view->data = NULL;
    view->len = 0;
    view->encoding = 0;
    view->owned = NULL;
}

void nb_message_view_release(nb_message_view_t *view)
{
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

int nb_message_view_set_borrowed(nb_message_view_t *view, const void *data, size_t len)
{
    if (!view || (len > 0 && !data)) {
        return NB_ERR_INVALID;
    }
    if (view->owned) {
        free(view->owned);
        view->owned = NULL;
    }
    view->data = data;
    view->len = len;
    return NB_OK;
}

int nb_message_view_set_data(nb_message_view_t *view, const void *data, size_t len)
{
    void *copy = NULL;
    if (!view || (len > 0 && !data)) {
        return NB_ERR_INVALID;
    }
    if (len > 0) {
        copy = malloc(len);
        if (!copy) {
            return NB_ERR_NOMEM;
        }
        memcpy(copy, data, len);
    }
    if (view->owned) {
        free(view->owned);
    }
    view->owned = copy;
    view->data = copy;
    view->len = len;
    return NB_OK;
}

int nb_message_view_set_string(nb_message_view_t *view, const char *text)
{
    if (!text) {
        return NB_ERR_INVALID;
    }
    return nb_message_view_set_data(view, text, strlen(text));
}

int nb_message_view_set_payload(nb_message_view_t *view, const nb_payload_t *payload)
{
    int rc;
    if (!view) {
        return NB_ERR_INVALID;
    }
    if (!payload) {
        if (view->owned) {
            free(view->owned);
            view->owned = NULL;
        }
        view->data = NULL;
        view->len = 0;
        view->encoding = 0;
        return NB_OK;
    }
    rc = nb_message_view_set_data(view, payload->data, payload->len);
    if (rc == NB_OK) {
        view->encoding = payload->encoding;
    }
    return rc;
}

/* ---- 导出 ---- */

void nb_message_view_to_payload(const nb_message_view_t *view, nb_payload_t *out)
{
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
