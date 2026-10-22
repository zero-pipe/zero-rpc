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
    zri_envelope_t view;
    zri_message_t msg;
    char route[MAX_ROUTE];

    if (size == 0) {
        return 0;
    }
    memset(&view, 0, sizeof(view));
    if (zri_envelope_decode(data, size, &view) != OK) {
        return 0;
    }
    memset(&msg, 0, sizeof(msg));
    zri_envelope_to_message(&view, route, sizeof(route), &msg);
    return 0;
}
