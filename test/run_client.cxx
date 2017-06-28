#include "ajp/ajp_client.hxx"
#include "strmap.hxx"
#include "http_client.hxx"
#include "http_headers.hxx"
#include "http_response.hxx"
#include "lease.hxx"
#include "direct.hxx"
#include "istream/istream_file.hxx"
#include "istream/istream_pipe.hxx"
#include "istream/istream.hxx"
#include "istream/sink_fd.hxx"
#include "direct.hxx"
#include "PInstance.hxx"
#include "fb_pool.hxx"
#include "ssl/ssl_init.hxx"
#include "ssl/ssl_client.hxx"
#include "system/SetupProcess.hxx"
#include "io/FileDescriptor.hxx"
#include "net/Resolver.hxx"
#include "net/AddressInfo.hxx"
#include "net/ConnectSocket.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "event/ShutdownListener.hxx"
#include "event/Duration.hxx"
#include "util/Cancellable.hxx"
#include "util/PrintException.hxx"

#include <inline/compiler.h>

#include <glib.h>

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
        HTTP, HTTPS, AJP,
    } protocol;

    char *host;

    int default_port;

    const char *uri;
};

static bool
parse_url(struct parsed_url *dest, const char *url)
{
    assert(dest != nullptr);
    assert(url != nullptr);

    if (memcmp(url, "ajp://", 6) == 0) {
        url += 6;
        dest->protocol = parsed_url::AJP;
        dest->default_port = 8009;
    } else if (memcmp(url, "http://", 7) == 0) {
        url += 7;
        dest->protocol = parsed_url::HTTP;
        dest->default_port = 80;
    } else if (memcmp(url, "https://", 8) == 0) {
        url += 8;
        dest->protocol = parsed_url::HTTPS;
        dest->default_port = 443;
    } else
        return false;

    dest->uri = strchr(url, '/');
    if (dest->uri == nullptr || dest->uri == url)
        return false;

    dest->host = g_strndup(url, dest->uri - url);
    return true;
}

struct Context final
    : PInstance, ConnectSocketHandler, Lease, HttpResponseHandler {

    struct pool *pool;

    struct parsed_url url;

    ShutdownListener shutdown_listener;

    CancellablePointer cancel_ptr;

    http_method_t method;
    Istream *request_body;

    UniqueSocketDescriptor fd;
    bool idle, reuse, aborted, got_response = false;
    http_status_t status;

    SinkFd *body;
    bool body_eof, body_abort, body_closed;

    Context()
        :shutdown_listener(event_loop, BIND_THIS_METHOD(ShutdownCallback)) {}

    void ShutdownCallback();

    /* virtual methods from class ConnectSocketHandler */
    void OnSocketConnectSuccess(UniqueSocketDescriptor &&fd) override;
    void OnSocketConnectError(std::exception_ptr ep) override;

    /* virtual methods from class Lease */
    void ReleaseLease(bool _reuse) override {
        assert(!idle);
        assert(fd.IsDefined());

        idle = true;
        reuse = _reuse;

        fd.Close();
    }

    /* virtual methods from class HttpResponseHandler */
    void OnHttpResponse(http_status_t status, StringMap &&headers,
                        Istream *body) override;
    void OnHttpError(std::exception_ptr ep) override;
};

void
Context::ShutdownCallback()
{
    if (body != nullptr) {
        sink_fd_close(body);
        body = nullptr;
        body_abort = true;
    } else {
        aborted = true;
        cancel_ptr.Cancel();
    }
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
                        Istream *_body)
{
    got_response = true;
    status = _status;

    if (_body != nullptr) {
        _body = istream_pipe_new(pool, *_body, nullptr);
        body = sink_fd_new(event_loop, *pool, *_body,
                           FileDescriptor(STDOUT_FILENO),
                           guess_fd_type(STDOUT_FILENO),
                           my_sink_fd_handler, this);
        _body->Read();
    } else {
        body_eof = true;
        shutdown_listener.Disable();
    }
}

void
Context::OnHttpError(std::exception_ptr ep)
{
    PrintException(ep);

    aborted = true;
}


/*
 * client_socket_handler
 *
 */

void
Context::OnSocketConnectSuccess(UniqueSocketDescriptor &&new_fd)
try {
    fd = std::move(new_fd);
    idle = false;

    StringMap headers(*pool);
    headers.Add("host", url.host);

    switch (url.protocol) {
    case parsed_url::AJP:
        ajp_client_request(*pool, event_loop,
                           fd, FdType::FD_TCP,
                           *this,
                           "http", "127.0.0.1", "localhost",
                           "localhost", 80, false,
                           method, url.uri, headers, request_body,
                           *this,
                           cancel_ptr);
        break;

    case parsed_url::HTTP:
        http_client_request(*pool, event_loop,
                            fd, FdType::FD_TCP,
                            *this,
                            "localhost",
                            nullptr, nullptr,
                            method, url.uri,
                            HttpHeaders(std::move(headers)),
                            request_body, false,
                            *this,
                            cancel_ptr);
        break;

    case parsed_url::HTTPS: {
        void *filter_ctx = ssl_client_create(event_loop,
                                             url.host);

        auto filter = &ssl_client_get_filter();
        http_client_request(*pool, event_loop,
                            fd, FdType::FD_TCP,
                            *this,
                            "localhost",
                            filter, filter_ctx,
                            method, url.uri,
                            HttpHeaders(std::move(headers)),
                            request_body, false,
                            *this,
                            cancel_ptr);
        break;
    }
    }
} catch (const std::runtime_error &e) {
    PrintException(e);

    aborted = true;

    if (request_body != nullptr)
        request_body->CloseUnused();

    shutdown_listener.Disable();
 }

void
Context::OnSocketConnectError(std::exception_ptr ep)
{
    PrintException(ep);

    aborted = true;

    if (request_body != nullptr)
        request_body->CloseUnused();

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

    if (!parse_url(&ctx.url, argv[1])) {
        fprintf(stderr, "Invalid or unsupported URL.\n");
        return EXIT_FAILURE;
    }

    direct_global_init();
    SetupProcess();
    const ScopeFbPoolInit fb_pool_init;

    const ScopeSslGlobalInit ssl_init;
    ssl_client_init();

    /* connect socket */

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_STREAM;

    const auto ail = Resolve(ctx.url.host, ctx.url.default_port, &hints);
    const auto &ai = ail.front();

    /* initialize */

    ctx.shutdown_listener.Enable();

    struct pool *pool = pool_new_linear(ctx.root_pool, "test", 8192);
    ctx.pool = pool;

    /* open request body */

    if (argc >= 3) {
        struct stat st;

        if (stat(argv[2], &st) < 0) {
            fprintf(stderr, "Failed to stat %s: %s\n",
                    argv[2], strerror(errno));
            return EXIT_FAILURE;
        }

        ctx.method = HTTP_METHOD_POST;

        ctx.request_body = istream_file_new(ctx.event_loop, *pool,
                                            argv[2], st.st_size);
    } else {
        ctx.method = HTTP_METHOD_GET;
        ctx.request_body = nullptr;
    }

    /* connect */

    ConnectSocket connect(ctx.event_loop, ctx);
    ctx.cancel_ptr = connect;
    connect.Connect(ai, EventDuration<30>::value);

    /* run test */

    ctx.event_loop.Dispatch();

    assert(!ctx.got_response || ctx.body_eof || ctx.body_abort || ctx.aborted);

    if (ctx.got_response)
        fprintf(stderr, "reuse=%d\n", ctx.reuse);

    /* cleanup */

    pool_unref(pool);
    pool_commit();

    ssl_client_deinit();

    g_free(ctx.url.host);

    return ctx.got_response && ctx.body_eof ? EXIT_SUCCESS : EXIT_FAILURE;
} catch (const std::exception &e) {
    PrintException(e);
    return EXIT_FAILURE;
}
