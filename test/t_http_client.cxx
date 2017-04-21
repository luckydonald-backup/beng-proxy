#define HAVE_EXPECT_100
#define HAVE_CHUNKED_REQUEST_BODY
#define ENABLE_CLOSE_IGNORED_REQUEST_BODY
#define ENABLE_HUGE_BODY
#define USE_BUCKETS

#include "t_client.hxx"
#include "http_client.hxx"
#include "http_headers.hxx"
#include "system/SetupProcess.hxx"
#include "io/FileDescriptor.hxx"
#include "direct.hxx"
#include "fb_pool.hxx"

#include <sys/wait.h>

struct Connection {
    EventLoop &event_loop;
    const pid_t pid;
    const int fd;

    Connection(EventLoop &_event_loop, pid_t _pid, int _fd)
        :event_loop(_event_loop), pid(_pid), fd(_fd) {}
    static Connection *New(EventLoop &event_loop,
                           const char *path, const char *mode);

    ~Connection();

    void Request(struct pool *pool,
                 Lease &lease,
                 http_method_t method, const char *uri,
                 StringMap &&headers,
                 Istream *body,
                 bool expect_100,
                 HttpResponseHandler &handler,
                 CancellablePointer &cancel_ptr) {
        http_client_request(*pool, event_loop, fd, FdType::FD_SOCKET,
                            lease,
                            "localhost",
                            nullptr, nullptr,
                            method, uri, HttpHeaders(std::move(headers)),
                            body, expect_100,
                            handler, cancel_ptr);
    }

    static Connection *NewMirror(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "mirror");
    }

    static Connection *NewNull(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "null");
    }

    static Connection *NewDummy(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "dummy");
    }

    static Connection *NewClose(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "close");
    }

    static Connection *NewFixed(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "fixed");
    }

    static Connection *NewTiny(struct pool &p, EventLoop &event_loop) {
        return NewFixed(p, event_loop);
    }

    static Connection *NewHuge(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "huge");
    }

    static Connection *NewTwice100(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/twice_100.sh", nullptr);
    }

    static Connection *NewClose100(struct pool &, EventLoop &event_loop);

    static Connection *NewHold(struct pool &, EventLoop &event_loop) {
        return New(event_loop, "./test/run_http_server", "hold");
    }
};

Connection::~Connection()
{
    assert(pid >= 1);
    assert(fd >= 0);

    close(fd);

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid() failed");
        exit(EXIT_FAILURE);
    }

    assert(!WIFSIGNALED(status));
}

Connection *
Connection::New(EventLoop &event_loop, const char *path, const char *mode)
{
    FileDescriptor client_socket, server_socket;
    if (!FileDescriptor::CreateSocketPair(AF_LOCAL, SOCK_STREAM, 0,
                                          client_socket, server_socket)) {
        perror("socketpair() failed");
        exit(EXIT_FAILURE);
    }

    const auto pid = fork();
    if (pid < 0) {
        perror("fork() failed");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        server_socket.CheckDuplicate(FileDescriptor(STDIN_FILENO));
        server_socket.CheckDuplicate(FileDescriptor(STDOUT_FILENO));

        execl(path, path,
              "0", "0", mode, nullptr);

        const char *srcdir = getenv("srcdir");
        if (srcdir != nullptr) {
            /* support automake out-of-tree build */
            if (chdir(srcdir) == 0)
                execl(path, path,
                      "0", "0", mode, nullptr);
        }

        perror("exec() failed");
        _exit(EXIT_FAILURE);
    }

    server_socket.Close();
    client_socket.SetNonBlocking();
    return new Connection(event_loop, pid, client_socket.Get());
}

Connection *
Connection::NewClose100(struct pool &, EventLoop &event_loop)
{
    FileDescriptor client_socket, server_socket;
    if (!FileDescriptor::CreateSocketPair(AF_LOCAL, SOCK_STREAM, 0,
                                          client_socket, server_socket)) {
        perror("socketpair() failed");
        exit(EXIT_FAILURE);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork() failed");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        client_socket.Close();

        static const char response[] = "HTTP/1.1 100 Continue\n\n";
        (void)server_socket.Write(response, sizeof(response) - 1);
        shutdown(server_socket.Get(), SHUT_WR);

        char buffer[64];
        while (server_socket.Read(buffer, sizeof(buffer)) > 0) {}

        _exit(EXIT_SUCCESS);
    }

    server_socket.Close();
    client_socket.SetNonBlocking();
    return new Connection(event_loop, pid, client_socket.Get());
}

/**
 * Keep-alive disabled, and response body has unknown length, ends
 * when server closes socket.  Check if our HTTP client handles such
 * responses correctly.
 */
template<class Connection>
static void
test_no_keepalive(Context<Connection> &c)
{
    c.connection = Connection::NewClose(*c.pool, c.event_loop);
    c.connection->Request(c.pool, c,
                          HTTP_METHOD_GET, "/foo", StringMap(*c.pool),
                          nullptr,
#ifdef HAVE_EXPECT_100
                          false,
#endif
                          c, c.cancel_ptr);
    pool_unref(c.pool);
    pool_commit();

    c.WaitForResponse();

    assert(c.status == HTTP_STATUS_OK);
    assert(c.request_error == nullptr);

    /* receive the rest of the response body from the buffer */
    c.event_loop.Dispatch();

    assert(c.released);
    assert(c.body_eof);
    assert(c.body_data > 0);
    assert(c.body_error == nullptr);
}

/*
 * main
 *
 */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    SetupProcess();

    direct_global_init();
    const ScopeFbPoolInit fb_pool_init;

    run_all_tests<Connection>();
    run_test<Connection>(test_no_keepalive);
}
