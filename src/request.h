/*
 * The BENG request struct.  This is only used by the handlers
 * (handler.c, file-handler.c etc.).
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_REQUEST_H
#define __BENG_REQUEST_H

#include "uri.h"
#include "translate.h"
#include "strmap.h"
#include "processor.h"

struct request {
    struct http_server_request *request;
    struct parsed_uri uri;

    strmap_t args;

    char session_id_buffer[9];
    struct session *session;

    struct {
        struct translate_request request;
        const struct translate_response *response;
    } translate;

    struct processor_env env;

    struct url_stream *url_stream;

    struct filter *filter;

    unsigned filtered, processed, response_sent;
};

void
request_get_session(struct request *request, const char *session_id);

struct session *
request_make_session(struct request *request);


struct growing_buffer;

void
response_dispatch(struct request *request2,
                  http_status_t status, struct growing_buffer *headers,
                  off_t content_length, istream_t body);

extern const struct http_response_handler response_handler;

#endif
