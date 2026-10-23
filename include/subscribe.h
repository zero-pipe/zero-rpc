#ifndef NB_SUBSCRIBE_H
#define NB_SUBSCRIBE_H

/*
 * Subscriber-facing aliases. A subscriber is an ordinary service whose method
 * names a topic. Topic-based transports deliver inbound samples as EVENT
 * messages to that method.
 */

#include <service.h>

typedef nb_service_t nb_subscriber_t;
typedef handler_fn nb_subscriber_handler_fn;

#define nb_subscriber_create(node, name, out) nb_service_create((node), (name), (out))
#define nb_subscriber_destroy nb_service_destroy
#define nb_subscriber_subscribe nb_service_add_method

#endif /* NB_SUBSCRIBE_H */
