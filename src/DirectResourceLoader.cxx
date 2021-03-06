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

#include "DirectResourceLoader.hxx"
#include "ResourceAddress.hxx"
#include "HttpResponseHandler.hxx"
#include "fs/SocketFilter.hxx"
#include "fs/Factory.hxx"
#include "http_request.hxx"
#include "file_request.hxx"
#include "file_address.hxx"
#include "lhttp_request.hxx"
#include "http_address.hxx"
#include "http/Headers.hxx"
#include "cgi/cgi_glue.hxx"
#include "cgi_address.hxx"
#include "fcgi/Request.hxx"
#include "fcgi/Remote.hxx"
#include "was/Glue.hxx"
#include "nfs/Address.hxx"
#include "nfs/Glue.hxx"
#include "http/HeaderWriter.hxx"
#include "pipe_filter.hxx"
#include "delegate/Address.hxx"
#include "delegate/HttpRequest.hxx"
#include "strmap.hxx"
#include "istream/istream.hxx"
#include "istream/UnusedPtr.hxx"
#include "ssl/SslSocketFilterFactory.hxx"
#include "pool/pool.hxx"
#include "AllocatorPtr.hxx"
#include "system/Error.hxx"
#include "net/HostParser.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringStrip.hxx"

#include <string.h>
#include <stdlib.h>

gcc_pure
static const char *
extract_remote_addr(const StringMap *headers) noexcept
{
    const char *xff = strmap_get_checked(headers, "x-forwarded-for");
    if (xff == nullptr)
        return nullptr;

    /* extract the last host name in X-Forwarded-For */
    const char *p = strrchr(xff, ',');
    if (p == nullptr)
        p = xff;
    else
        ++p;

    return StripLeft(p);
}

gcc_pure
static const char *
extract_remote_ip(struct pool *pool, const StringMap *headers) noexcept
{
    const char *p = extract_remote_addr(headers);
    if (p == nullptr)
        return p;

    auto eh = ExtractHost(p);
    if (eh.HasFailed() || eh.host.size == strlen(p))
        return p;

    return p_strdup(*pool, eh.host);
}

gcc_pure
static const char *
GetHostWithoutPort(struct pool &pool, const HttpAddress &address) noexcept
{
    const char *host_and_port = address.host_and_port;
    if (host_and_port == nullptr)
        return nullptr;

    auto e = ExtractHost(host_and_port);
    if (e.host.IsNull())
        return nullptr;

    return p_strdup(pool, e.host);
}

void
DirectResourceLoader::SendRequest(struct pool &pool,
                                  sticky_hash_t session_sticky,
                                  gcc_unused const char *cache_tag,
                                  const char *site_name,
                                  http_method_t method,
                                  const ResourceAddress &address,
                                  http_status_t status, StringMap &&headers,
                                  UnusedIstreamPtr body,
                                  gcc_unused const char *body_etag,
                                  HttpResponseHandler &handler,
                                  CancellablePointer &cancel_ptr) noexcept
try {
    switch (address.type) {
        const FileAddress *file;
        const CgiAddress *cgi;
        const NfsAddress *nfs;
        SocketFilterFactory *filter_factory;

    case ResourceAddress::Type::NONE:
        break;

    case ResourceAddress::Type::LOCAL:
        /* static files cannot receive a request body, close it */
        body.Clear();

        file = &address.GetFile();
        if (file->delegate != nullptr) {
            if (delegate_stock == nullptr)
                throw std::runtime_error("No delegate stock");

            delegate_stock_request(event_loop, *delegate_stock, pool,
                                   file->delegate->delegate,
                                   file->delegate->child_options,
                                   file->path,
                                   file->content_type,
                                   handler,
                                   cancel_ptr);
            return;
        }

        static_file_get(event_loop, pool, file->path,
                        file->content_type,
                        handler);
        return;

    case ResourceAddress::Type::NFS:
        /* NFS files cannot receive a request body, close it */
        body.Clear();

        nfs = &address.GetNfs();

        nfs_request(pool, *nfs_cache,
                    nfs->server, nfs->export_name,
                    nfs->path, nfs->content_type,
                    handler, cancel_ptr);
        return;

    case ResourceAddress::Type::PIPE:
        cgi = &address.GetCgi();
        pipe_filter(spawn_service, event_loop, &pool,
                    cgi->path, cgi->args.ToArray(pool),
                    cgi->options,
                    status, std::move(headers), std::move(body),
                    handler);
        return;

    case ResourceAddress::Type::CGI:
        cgi_new(spawn_service, event_loop, &pool,
                method, &address.GetCgi(),
                extract_remote_ip(&pool, &headers),
                headers, std::move(body),
                handler, cancel_ptr);
        return;

    case ResourceAddress::Type::FASTCGI: {
        cgi = &address.GetCgi();

        UniqueFileDescriptor stderr_fd;
        if (cgi->options.stderr_path != nullptr) {
            stderr_fd = cgi->options.OpenStderrPath();
        }

        if (cgi->address_list.IsEmpty())
            fcgi_request(&pool, event_loop, fcgi_stock,
                         site_name,
                         cgi->options,
                         cgi->action,
                         cgi->path,
                         cgi->args.ToArray(pool),
                         method, cgi->GetURI(&pool),
                         cgi->script_name,
                         cgi->path_info,
                         cgi->query_string,
                         cgi->document_root,
                         extract_remote_ip(&pool, &headers),
                         headers, std::move(body),
                         cgi->params.ToArray(pool),
                         std::move(stderr_fd),
                         handler, cancel_ptr);
        else
            fcgi_remote_request(&pool, event_loop, tcp_balancer,
                                &cgi->address_list,
                                cgi->path,
                                method, cgi->GetURI(&pool),
                                cgi->script_name,
                                cgi->path_info,
                                cgi->query_string,
                                cgi->document_root,
                                extract_remote_ip(&pool, &headers),
                                std::move(headers), std::move(body),
                                cgi->params.ToArray(pool),
                                std::move(stderr_fd),
                                handler, cancel_ptr);
        return;
    }

    case ResourceAddress::Type::WAS:
        cgi = &address.GetCgi();
        was_request(pool, *was_stock, site_name,
                    cgi->options,
                    cgi->action,
                    cgi->path,
                    cgi->args.ToArray(pool),
                    method, cgi->GetURI(&pool),
                    cgi->script_name,
                    cgi->path_info,
                    cgi->query_string,
                    headers, std::move(body),
                    cgi->params.ToArray(pool),
                    handler, cancel_ptr);
        return;

    case ResourceAddress::Type::HTTP:
        if (address.GetHttp().ssl) {
            filter_factory = NewFromPool<SslSocketFilterFactory>(pool,
                                                                 event_loop,
                                                                 GetHostWithoutPort(pool, address.GetHttp()),
                                                                 address.GetHttp().certificate);
        } else {
            filter_factory = nullptr;
        }

        http_request(pool, event_loop, fs_balancer, session_sticky,
                     filter_factory,
                     method, address.GetHttp(),
                     HttpHeaders(std::move(headers)), std::move(body),
                     handler, cancel_ptr);
        return;

    case ResourceAddress::Type::LHTTP:
        lhttp_request(pool, event_loop, *lhttp_stock,
                      site_name,
                      address.GetLhttp(),
                      method, HttpHeaders(std::move(headers)),
                      std::move(body),
                      handler, cancel_ptr);
        return;
    }

    /* the resource could not be located, abort the request */

    throw std::runtime_error("Could not locate resource");
} catch (...) {
    body.Clear();
    handler.InvokeError(std::current_exception());
}
