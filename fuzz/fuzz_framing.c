/*
 * libFuzzer harness for transport framing parsers (RTP/RTCP/TCP length prefix).
 */
#include "framing.h"
#include "caps.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    nb_frag_t frag;
    const uint8_t *body = NULL;
    size_t body_len = 0;
    uint32_t ssrc = 0;
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    uint16_t seqs[MAX_NACK_SEQS];
    nb_rtcp_sr_t sr;
    nb_rtcp_rr_t rr;

    if (!data || size == 0) {
        return 0;
    }
    memset(&frag, 0, sizeof(frag));
    (void)nb_frame_parse_data(data, size, &frag);
    (void)nb_frame_parse_nack(
        data, size, &sender_ssrc, &media_ssrc, seqs, sizeof(seqs) / sizeof(seqs[0]));
    memset(&sr, 0, sizeof(sr));
    memset(&rr, 0, sizeof(rr));
    (void)nb_frame_parse_sr(data, size, &sr);
    (void)nb_frame_parse_rr(data, size, &rr);
    (void)nb_frame_parse_bye(data, size, &ssrc);
    (void)nb_tcp_frame_size(data, size);
    (void)nb_tcp_parse(data, size, &body, &body_len);
    return 0;
}
