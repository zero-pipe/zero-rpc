#ifndef NB_PUBLISH_H
#define NB_PUBLISH_H

/*
 * Publisher-facing aliases. A publisher is an ordinary proxy that sends
 * one-way EVENT messages. The second argument of nb_publisher_publish is the
 * target method for route-based transports and the topic for topic-based
 * transports such as DDS.
 */

#include <proxy.h>

typedef nb_proxy_t nb_publisher_t;
typedef nb_proxy_options_t nb_publisher_options_t;

#define nb_publisher_create nb_proxy_create
#define nb_publisher_destroy nb_proxy_destroy
#define nb_publisher_publish nb_proxy_call_event

#endif /* NB_PUBLISH_H */
