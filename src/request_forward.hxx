/*
 * Common request forwarding code for the request handlers.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_REQUEST_FORWARD_HXX
#define BENG_PROXY_REQUEST_FORWARD_HXX

#include "strmap.hxx"

#include <http/method.h>

class StringMap;
struct header_forward_settings;
struct Request;
class Istream;

struct ForwardRequest {
    http_method_t method;

    StringMap headers;

    Istream *body;

    ForwardRequest(http_method_t _method, StringMap &&_headers,
                   Istream *_body)
        :method(_method), headers(std::move(_headers)), body(_body) {}
};

ForwardRequest
request_forward(Request &src,
                const struct header_forward_settings &header_forward,
                const char *host_and_port, const char *uri,
                bool exclude_host);

#endif
