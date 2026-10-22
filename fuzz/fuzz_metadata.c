/*
 * libFuzzer harness for metadata TLV decode (must never read out of bounds).
 */
#include <types.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nb_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));
    (void)nb_metadata_decode(&metadata, data, size);
    return 0;
}
