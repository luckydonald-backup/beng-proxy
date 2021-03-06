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

#include "strmap.hxx"
#include "http_client.hxx"
#include "http/Headers.hxx"
#include "HttpResponseHandler.hxx"
#include "lease.hxx"
#include "direct.hxx"
#include "istream/FileIstream.hxx"
#include "istream/AutoPipeIstream.hxx"
#include "istream/istream.hxx"
#include "istream/sink_fd.hxx"
#include "istream/UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "direct.hxx"
#include "PInstance.hxx"
#include "fb_pool.hxx"
#include "fs/FilteredSocket.hxx"
#include "ssl/Init.hxx"
#include "ssl/Client.hxx"
#include "ssl/Config.hxx"
#include "system/SetupProcess.hxx"
#include "io/FileDescriptor.hxx"
#include "net/HostParser.hxx"
#include "net/Resolver.hxx"
#include "net/AddressInfo.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "event/net/ConnectSocket.hxx"
#include "event/ShutdownListener.hxx"
#include "util/Cancellable.hxx"
#include "util/PrintException.hxx"

#include "util/Compiler.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

struct parsed_url {
    enum {
        HTTP, HTTPS,
    } protocol;

    std::string host;

    int default_port;

    const char *uri;
};

static struct parsed_url
parse_url(const char *url)
{
    assert(url != nullptr);

    struct parsed_url dest;

    if (memcmp(url, "http://", 7) == 0) {
        url += 7;
        dest.protocol = parsed_url::HTTP;
        dest.default_port = 80;
    } else if (memcmp(url, "https://", 8) == 0) {
        url += 8;
        dest.protocol = parsed_url::HTTPS;
        dest.default_port = 443;
    } else
        throw std::runtime_error("Unsupported URL");

    dest.uri = strchr(url, '/');
    if (dest.uri == nullptr || dest.uri == url)
        throw std::runtime_error("Missing URI path");

    dest.host = std::string(url, dest.uri);

    return dest;
}

gcc_pure
static const char *
GetHostWithoutPort(struct pool &pool, const struct parsed_url &url) noexcept
{
    if (url.host.empty())
        return nullptr;

    auto e = ExtractHost(url.host.c_str());
    if (e.host.IsNull())
        return nullptr;

    return p_strdup(pool, e.host);
}

struct Context final
    : PInstance, ConnectSocketHandler, Lease, HttpResponseHandler {

    struct parsed_url url;

    ShutdownListener shutdown_listener;

    PoolPtr pool;

    CancellablePointer cancel_ptr;

    http_method_t method;
    UnusedIstreamPtr request_body;

    UniqueSocketDescriptor fd;
    FilteredSocket fs;

    bool idle, reuse, aborted, got_response = false;
    http_status_t status;

    SinkFd *body = nullptr;
    bool body_eof, body_abort;

    Context()
        :shutdown_listener(event_loop, BIND_THIS_METHOD(ShutdownCallback)),
         pool(pool_new_linear(root_pool, "test", 8192)),
         fs(event_loop) {}

    void ShutdownCallback() noexcept;

    /* virtual methods from class ConnectSocketHandler */
    void OnSocketConnectSuccess(UniqueSocketDescriptor &&fd) noexcept override;
    void OnSocketConnectError(std::exception_ptr ep) noexcept override;

    /* virtual methods from class Lease */
    void ReleaseLease(bool _reuse) noexcept override {
        assert(!idle);
        assert(url.protocol == parsed_url::HTTP ||
               url.protocol == parsed_url::HTTPS ||
               fd.IsDefined());

        idle = true;
        reuse = _reuse;

        if (url.protocol == parsed_url::HTTP ||
            url.protocol == parsed_url::HTTPS) {
            if (fs.IsConnected())
                fs.Close();
            fs.Destroy();
        } else
            fd.Close();
    }

    /* virtual methods from class HttpResponseHandler */
    void OnHttpResponse(http_status_t status, StringMap &&headers,
                        UnusedIstreamPtr body) noexcept override;
    void OnHttpError(std::exception_ptr ep) noexcept override;
};

void
Context::ShutdownCallback() noexcept
{
    if (body != nullptr) {
        sink_fd_close(body);
        body = nullptr;
        body_abort = true;
    } else {
        aborted = true;
        cancel_ptr.Cancel();
    }

    shutdown_listener.Disable();
}

/*
 * istream handler
 *
 */

static void
my_sink_fd_input_eof(void *ctx)
{
    auto *c = (Context *)ctx;

    c->body = nullptr;
    c->body_eof = true;

    c->shutdown_listener.Disable();
}

static void
my_sink_fd_input_error(std::exception_ptr ep, void *ctx)
{
    auto *c = (Context *)ctx;

    PrintException(ep);

    c->body = nullptr;
    c->body_abort = true;

    c->shutdown_listener.Disable();
}

static bool
my_sink_fd_send_error(int error, void *ctx)
{
    auto *c = (Context *)ctx;

    fprintf(stderr, "%s\n", strerror(error));

    c->body = nullptr;
    c->body_abort = true;

    c->shutdown_listener.Disable();
    return true;
}

static constexpr SinkFdHandler my_sink_fd_handler = {
    .input_eof = my_sink_fd_input_eof,
    .input_error = my_sink_fd_input_error,
    .send_error = my_sink_fd_send_error,
};


/*
 * http_response_handler
 *
 */

void
Context::OnHttpResponse(http_status_t _status, gcc_unused StringMap &&headers,
                        UnusedIstreamPtr _body) noexcept
{
    got_response = true;
    status = _status;

    if (_body) {
        body = sink_fd_new(event_loop, *pool,
                           NewAutoPipeIstream(pool, std::move(_body), nullptr),
                           FileDescriptor(STDOUT_FILENO),
                           guess_fd_type(STDOUT_FILENO),
                           my_sink_fd_handler, this);
        sink_fd_read(body);
    } else {
        body_eof = true;
        shutdown_listener.Disable();
    }
}

void
Context::OnHttpError(std::exception_ptr ep) noexcept
{
    PrintException(ep);

    aborted = true;

    shutdown_listener.Disable();
}


/*
 * client_socket_handler
 *
 */

void
Context::OnSocketConnectSuccess(UniqueSocketDescriptor &&new_fd) noexcept
try {
    fd = std::move(new_fd);
    idle = false;

    StringMap headers(*pool);
    headers.Add("host", url.host.c_str());

    switch (url.protocol) {
    case parsed_url::HTTP:
        fs.InitDummy(fd.Release(), FdType::FD_TCP);

        http_client_request(*pool, fs,
                            *this,
                            "localhost",
                            method, url.uri,
                            HttpHeaders(std::move(headers)),
                            std::move(request_body), false,
                            *this,
                            cancel_ptr);
        break;

    case parsed_url::HTTPS:
        fs.InitDummy(fd.Release(), FdType::FD_TCP,
                     ssl_client_create(event_loop,
                                       GetHostWithoutPort(*pool, url),
                                       nullptr));

        http_client_request(*pool, fs,
                            *this,
                            "localhost",
                            method, url.uri,
                            HttpHeaders(std::move(headers)),
                            std::move(request_body), false,
                            *this,
                            cancel_ptr);
        break;
    }
} catch (const std::runtime_error &e) {
    PrintException(e);

    aborted = true;
    request_body.Clear();

    shutdown_listener.Disable();
 }

void
Context::OnSocketConnectError(std::exception_ptr ep) noexcept
{
    PrintException(ep);

    aborted = true;
    request_body.Clear();

    shutdown_listener.Disable();
}

/*
 * main
 *
 */

int
main(int argc, char **argv)
try {
    Context ctx;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: run_client URL [BODY]\n");
        return EXIT_FAILURE;
    }

    ctx.url = parse_url(argv[1]);

    direct_global_init();
    SetupProcess();
    const ScopeFbPoolInit fb_pool_init;

    const ScopeSslGlobalInit ssl_init;
    ssl_client_init(SslClientConfig());

    /* connect socket */

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;

    const auto ail = Resolve(ctx.url.host.c_str(), ctx.url.default_port,
                             &hints);
    const auto &ai = ail.front();

    /* initialize */

    ctx.shutdown_listener.Enable();

    /* open request body */

    if (argc >= 3) {
        struct stat st;

        if (stat(argv[2], &st) < 0) {
            fprintf(stderr, "Failed to stat %s: %s\n",
                    argv[2], strerror(errno));
            return EXIT_FAILURE;
        }

        ctx.method = HTTP_METHOD_POST;

        ctx.request_body = UnusedIstreamPtr(istream_file_new(ctx.event_loop, ctx.pool,
                                                             argv[2], st.st_size));
    } else {
        ctx.method = HTTP_METHOD_GET;
    }

    /* connect */

    ConnectSocket connect(ctx.event_loop, ctx);
    ctx.cancel_ptr = connect;
    connect.Connect(ai, std::chrono::seconds(30));

    /* run test */

    ctx.event_loop.Dispatch();

    assert(!ctx.got_response || ctx.body_eof || ctx.body_abort || ctx.aborted);

    if (ctx.got_response)
        fprintf(stderr, "reuse=%d\n", ctx.reuse);

    /* cleanup */

    ctx.pool.reset();
    pool_commit();

    ssl_client_deinit();

    return ctx.got_response && ctx.body_eof ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception &e) {
    PrintException(e);
    return EXIT_FAILURE;
}
