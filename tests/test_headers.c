/* Compile-time smoke test for the role-based public headers. */
#include <runtime.h>
#include <server.h>
#include <client.h>
#include <transport.h>
#include <extensions.h>
#include <config.h>

int main(void)
{
    payload_t payload = { 0 };
    endpoint_t endpoint = { 0 };
    message_view_t view;

    message_view_init(&view);
    message_view_to_payload(&view, &payload);
    (void)endpoint;
    return payload.data == NULL && payload.len == 0 ? 0 : 1;
}
