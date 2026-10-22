/*
 * libFuzzer harness for envelope decode + message conversion.
 *
 * Build: cmake -DBUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang ...
 * Run:   ./fuzz/fuzz_envelope -max_total_time=60
 */
#include "envelope.h"
#include "caps.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nb_envelope_t view;
    nb_message_t msg;
    char route[MAX_ROUTE];

    if (size == 0) {
        return 0;
    }
    memset(&view, 0, sizeof(view));
    if (nb_envelope_decode(data, size, &view) != NB_OK) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    nb_envelope_to_message(&view, route, sizeof(route), &msg);
    return 0;
}
