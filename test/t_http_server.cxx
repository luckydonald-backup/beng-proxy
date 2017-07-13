#include "http_server/http_server.hxx"
#include "http_server/Request.hxx"
#include "http_server/Handler.hxx"
#include "http_headers.hxx"
#include "direct.hxx"
#include "PInstance.hxx"
#include "pool.hxx"
#include "istream/istream.hxx"
#include "istream/istream_catch.hxx"
#include "fb_pool.hxx"
#include "net/SocketDescriptor.hxx"
#include "io/FileDescriptor.hxx"
#include "util/PrintException.hxx"

#include <stdio.h>
#include <stdlib.h>

struct Instance final : HttpServerConnectionHandler {
    struct pool *pool;

    HttpServerConnection *connection = nullptr;

    explicit Instance(struct pool &_pool)
        :pool(pool_new_libc(&_pool, "catch")) {}

    ~Instance() {
        CheckCloseConnection();
    }

    void CloseConnection() {
        http_server_connection_close(connection);
        connection = nullptr;
    }

    void CheckCloseConnection() {
        if (connection != nullptr)
            CloseConnection();
    }

    /* virtual methods from class HttpServerConnectionHandler */
    void HandleHttpRequest(HttpServerRequest &request,
                           CancellablePointer &cancel_ptr) override;

    void LogHttpRequest(HttpServerRequest &,
                        http_status_t, int64_t,
                        uint64_t, uint64_t) override {}

    void HttpConnectionError(std::exception_ptr e) override;
    void HttpConnectionClosed() override;
};

static std::exception_ptr 
catch_callback(std::exception_ptr ep, gcc_unused void *ctx)
{
    PrintException(ep);
    return {};
}

void
Instance::HandleHttpRequest(HttpServerRequest &request,
                            gcc_unused CancellablePointer &cancel_ptr)
{
    http_server_response(&request, HTTP_STATUS_OK, HttpHeaders(request.pool),
                         istream_catch_new(&request.pool, *request.body,
                                           catch_callback, nullptr));

    CloseConnection();
}

void
Instance::HttpConnectionError(std::exception_ptr e)
{
    connection = nullptr;

    PrintException(e);
}

void
Instance::HttpConnectionClosed()
{
    connection = nullptr;
}

static void
test_catch(EventLoop &event_loop, struct pool *_pool)
{
    SocketDescriptor client_socket, server_socket;
    if (!SocketDescriptor::CreateSocketPair(AF_LOCAL, SOCK_STREAM, 0,
                                            client_socket, server_socket)) {
        perror("socketpair() failed");
        exit(EXIT_FAILURE);
    }

    static constexpr char request[] =
        "POST / HTTP/1.1\r\nContent-Length: 1024\r\n\r\nfoo";
    send(client_socket.Get(), request, sizeof(request) - 1, 0);

    Instance instance(*_pool);
    instance.connection =
        http_server_connection_new(instance.pool, event_loop,
                                   server_socket, FdType::FD_SOCKET,
                                   nullptr, nullptr,
                                   nullptr, nullptr,
                                   true, instance);
    pool_unref(instance.pool);

    event_loop.Dispatch();

    client_socket.Close();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    direct_global_init();
    const ScopeFbPoolInit fb_pool_init;
    PInstance instance;

    test_catch(instance.event_loop, instance.root_pool);
}
