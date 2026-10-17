/* Compile-time smoke test for the role-based public headers. */
#include <zrpc/zrpc_runtime.h>
#include <zrpc/zrpc_server.h>
#include <zrpc/zrpc_client.h>
#include <zrpc/zrpc_transport.h>
#include <zrpc/zrpc_extensions.h>
#include <zrpc/zrpc_config.h>

int main(void) {
    zrpc_payload_t payload = {0};
    zrpc_endpoint_t endpoint = {0};
    zrpc_message_view_t view;

    zrpc_message_view_init(&view);
    zrpc_message_view_to_payload(&view, &payload);
    (void)endpoint;
    return payload.data == NULL && payload.len == 0 ? 0 : 1;
}
