/*
 * Set up load balancer objects.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lb_setup.h"
#include "lb_config.h"
#include "lb_instance.h"
#include "lb_listener.h"

bool
init_all_listeners(struct lb_instance *instance)
{
    bool success = true;

    for (struct lb_listener_config *config = (struct lb_listener_config *)instance->config->listeners.next;
         &config->siblings != &instance->config->listeners;
         config = (struct lb_listener_config *)config->siblings.next) {
        struct lb_listener *listener = lb_listener_new(instance, config);
        if (listener == NULL)
            success = false;
    }

    return success;
}

void
deinit_all_listeners(struct lb_instance *instance)
{
    for (struct lb_listener *listener = (struct lb_listener *)instance->listeners.next;
         &listener->siblings != &instance->listeners;
         listener = (struct lb_listener *)listener->siblings.next)
        lb_listener_free(listener);
    list_init(&instance->listeners);
}
