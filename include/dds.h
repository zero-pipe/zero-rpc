#ifndef NB_DDS_H
#define NB_DDS_H

/*
 * Cyclone DDS transport provider.
 *
 * Register the provider before creating any node, then use a DDS binding URI
 * of the form "dds://<domain>:1". The host component selects the DDS domain;
 * the port component is ignored by the provider but must be non-zero so the
 * existing endpoint parser and proxy routing accept the URI.
 *
 * Topics are derived from the method name:
 *   <method>_req  request topic
 *   <method>_res  response topic
 *   <method>_evt  one-way event / pub-sub topic
 */

#include <transport_provider.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotently register the built-in "dds" provider. */
int nb_cyclone_dds_register(void);

/* Static provider descriptor, for callers that prefer explicit registration. */
const nb_transport_provider_t *nb_cyclone_dds_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_H */
