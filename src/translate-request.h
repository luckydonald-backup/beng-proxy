/*
 * The translation request struct.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_TRANSLATE_REQUEST_H
#define BENG_PROXY_TRANSLATE_REQUEST_H

#include "strref.h"

#include <http/status.h>

#include <stddef.h>

struct translate_request {
    const struct sockaddr *local_address;
    size_t local_address_length;

    const char *remote_host;
    const char *host;
    const char *user_agent;
    const char *accept_language;

    /**
     * The value of the "Authorization" HTTP request header.
     */
    const char *authorization;

    const char *uri;
    const char *args;
    const char *query_string;
    const char *widget_type;
    const char *session;
    const char *param;

    /**
     * The payload of the CHECK packet.  If
     * strref_is_null(&esponse.check), then no CHECK packet will be
     * sent.
     */
    struct strref check;

    http_status_t error_document_status;
};

#endif
