/*
 * Widget declarations.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_WIDGET_CLASS_H
#define BENG_PROXY_WIDGET_CLASS_H

#include "widget-view.h"
#include "resource-address.h"
#include "header-forward.h"

/**
 * A widget class is a server which provides a widget.
 */
struct widget_class {
    /** the base URI of this widget, as specified in the template */
    struct resource_address address;

    /**
     * A linked list of view descriptions.
     */
    struct widget_view views;

    /**
     * The (beng-proxy) hostname on which requests to this widget are
     * allowed.  If not set, then this is a trusted widget.  Requests
     * from an untrusted widget to a trusted one are forbidden.
     */
    const char *untrusted_host;

    /**
     * The (beng-proxy) hostname prefix on which requests to this
     * widget are allowed.  If not set, then this is a trusted widget.
     * Requests from an untrusted widget to a trusted one are
     * forbidden.
     */
    const char *untrusted_prefix;

    /** does beng-proxy remember the state (path_info and
        query_string) of this widget? */
    bool stateful;

    /**
     * Filter client error messages?
     */
    bool filter_4xx;

    /**
     * Which request headers are forwarded?
     */
    struct header_forward_settings request_header_forward;

    /**
     * Which response headers are forwarded?
     */
    struct header_forward_settings response_header_forward;
};

extern const struct widget_class root_widget_class;

bool
widget_class_is_container(const struct widget_class *class,
                          const char *view_name);

#endif
