/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Serve HTTP requests from another HTTP server.
 */

#include "Handler.hxx"
#include "Connection.hxx"
#include "Instance.hxx"
#include "Request.hxx"
#include "ForwardRequest.hxx"
#include "ResourceLoader.hxx"
#include "HttpResponseHandler.hxx"
#include "http_server/Request.hxx"
#include "http_address.hxx"
#include "cgi_address.hxx"
#include "uri/Extract.hxx"
#include "istream/AutoPipeIstream.hxx"
#include "lhttp_address.hxx"
#include "pool/pool.hxx"

/**
 * Return a copy of the URI for forwarding to the next server.  This
 * omits the beng-proxy request "arguments".
 */
gcc_pure
static const char *
ForwardURI(struct pool &pool, const DissectedUri &uri)
{
    if (uri.query.empty())
        return p_strdup(pool, uri.base);
    else
        return p_strncat(&pool,
                         uri.base.data, uri.base.size,
                         "?", (size_t)1,
                         uri.query.data, uri.query.size,
                         nullptr);
}

/**
 * Return a copy of the original request URI for forwarding to the
 * next server.  This omits the beng-proxy request "arguments" (unless
 * the translation server declared the "transparent" mode).
 */
gcc_pure
static const char *
ForwardURI(const Request &r)
{
    const TranslateResponse &t = *r.translate.response;
    if (t.transparent || r.dissected_uri.args == nullptr)
        /* transparent or no args: return the full URI as-is */
        return r.request.uri;
    else
        /* remove the "args" part */
        return ForwardURI(r.pool, r.dissected_uri);
}

void
proxy_handler(Request &request2)
{
    struct pool &pool = request2.pool;
    const TranslateResponse &tr = *request2.translate.response;
    ResourceAddress address(ShallowCopy(), request2.translate.address);

    assert(address.type == ResourceAddress::Type::HTTP ||
           address.type == ResourceAddress::Type::LHTTP ||
           address.type == ResourceAddress::Type::NFS ||
           address.IsCgiAlike());

    if (request2.translate.response->transparent &&
        (!request2.dissected_uri.args.IsNull() ||
         !request2.dissected_uri.path_info.empty()))
        address = address.WithArgs(pool,
                                   request2.dissected_uri.args,
                                   request2.dissected_uri.path_info);

    if (!request2.processor_focus)
        /* forward query string */
        address = address.WithQueryStringFrom(pool, request2.request.uri);

    if (address.IsCgiAlike() &&
        address.GetCgi().script_name == nullptr &&
        address.GetCgi().uri == nullptr)
        /* pass the "real" request URI to the CGI (but without the
           "args", unless the request is "transparent") */
        address.GetCgi().uri = ForwardURI(request2);

    request2.cookie_uri = address.GetUriPath();

    auto forward = request_forward(request2,
                                   tr.request_header_forward,
                                   request2.GetCookieHost(),
                                   request2.GetCookieURI(),
                                   address.IsAnyHttp());

#ifdef SPLICE
    if (forward.body)
        forward.body = NewAutoPipeIstream(&pool, std::move(forward.body),
                                          request2.instance.pipe_stock);
#endif

    for (const auto &i : tr.request_headers)
        forward.headers.SecureSet(i.key, i.value);

    request2.collect_cookies = true;

    auto &rl = tr.uncached
        ? *request2.instance.direct_resource_loader
        : *request2.instance.cached_resource_loader;

    rl.SendRequest(pool,
                   request2.session_id.GetClusterHash(),
                   nullptr, tr.site,
                   forward.method, address, HTTP_STATUS_OK,
                   std::move(forward.headers),
                   std::move(forward.body),
                   nullptr,
                   request2, request2.cancel_ptr);
}
