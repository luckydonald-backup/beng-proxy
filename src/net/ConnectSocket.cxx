/*
 * TCP client socket with asynchronous connect.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "ConnectSocket.hxx"
#include "SocketDescriptor.hxx"
#include "SocketAddress.hxx"
#include "system/fd_util.h"
#include "stopwatch.hxx"
#include "event/SocketEvent.hxx"
#include "gerrno.h"
#include "pool.hxx"
#include "util/Cancellable.hxx"

#include <socket/util.h>

#ifdef ENABLE_STOPWATCH
#include <socket/address.h>
#endif

#include <assert.h>
#include <stddef.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

void
ConnectSocketHandler::OnSocketConnectTimeout()
{
    /* default implementation falls back to OnSocketConnectError() */
    auto error = g_error_new_literal(errno_quark(), ETIMEDOUT, "timeout");
    OnSocketConnectError(error);
}

class ConnectSocket final : Cancellable {
    struct pool &pool;
    SocketDescriptor fd;
    SocketEvent event;

#ifdef ENABLE_STOPWATCH
    Stopwatch &stopwatch;
#endif

    ConnectSocketHandler &handler;

public:
    ConnectSocket(EventLoop &event_loop, struct pool &_pool,
                  SocketDescriptor &&_fd, unsigned timeout,
#ifdef ENABLE_STOPWATCH
                  Stopwatch &_stopwatch,
#endif
                  ConnectSocketHandler &_handler,
                  CancellablePointer &cancel_ptr)
        :pool(_pool), fd(std::move(_fd)),
         event(event_loop, fd.Get(), EV_WRITE|EV_TIMEOUT,
               BIND_THIS_METHOD(EventCallback)),
#ifdef ENABLE_STOPWATCH
         stopwatch(_stopwatch),
#endif
         handler(_handler) {
        pool_ref(&pool);

        cancel_ptr = *this;

        const struct timeval tv = {
            .tv_sec = time_t(timeout),
            .tv_usec = 0,
        };
        event.Add(tv);
    }

    void Delete() {
        DeleteUnrefPool(pool, this);
    }

private:
    void EventCallback(short events);

    /* virtual methods from class Cancellable */
    void Cancel() override;
};


/*
 * async operation
 *
 */

void
ConnectSocket::Cancel()
{
    assert(fd.IsDefined());

    event.Delete();
    Delete();
}


/*
 * libevent callback
 *
 */

inline void
ConnectSocket::EventCallback(short events)
{
    if (events & EV_TIMEOUT) {
        handler.OnSocketConnectTimeout();
        Delete();
        return;
    }

    int s_err = fd.GetError();
    if (s_err == 0) {
#ifdef ENABLE_STOPWATCH
        stopwatch_event(&stopwatch, "connect");
        stopwatch_dump(&stopwatch);
#endif

        handler.OnSocketConnectSuccess(std::move(fd));
    } else {
        GError *error = new_error_errno2(s_err);
        handler.OnSocketConnectError(error);
    }

    Delete();

    pool_commit();
}


/*
 * constructor
 *
 */

void
client_socket_new(EventLoop &event_loop, struct pool &pool,
                  int domain, int type, int protocol,
                  bool ip_transparent,
                  const SocketAddress bind_address,
                  const SocketAddress address,
                  unsigned timeout,
                  ConnectSocketHandler &handler,
                  CancellablePointer &cancel_ptr)
{
    assert(!address.IsNull());

    SocketDescriptor fd;
    if (!fd.Create(domain, type, protocol)) {
        GError *error = new_error_errno();
        handler.OnSocketConnectError(error);
        return;
    }

    if ((domain == PF_INET || domain == PF_INET6) && type == SOCK_STREAM &&
        !socket_set_nodelay(fd.Get(), true)) {
        GError *error = new_error_errno();
        handler.OnSocketConnectError(error);
        return;
    }

    if (ip_transparent) {
        int on = 1;
        if (setsockopt(fd.Get(), SOL_IP, IP_TRANSPARENT, &on, sizeof on) < 0) {
            GError *error = new_error_errno_msg("Failed to set IP_TRANSPARENT");
            handler.OnSocketConnectError(error);
            return;
        }
    }

    if (!bind_address.IsNull() && bind_address.IsDefined() &&
        !fd.Bind(bind_address)) {
        GError *error = new_error_errno();
        handler.OnSocketConnectError(error);
        return;
    }

#ifdef ENABLE_STOPWATCH
    Stopwatch *stopwatch =
        stopwatch_sockaddr_new(&pool, address.GetAddress(), address.GetSize(),
                               nullptr);
#endif

    if (fd.Connect(address)) {
#ifdef ENABLE_STOPWATCH
        stopwatch_event(stopwatch, "connect");
        stopwatch_dump(stopwatch);
#endif

        handler.OnSocketConnectSuccess(std::move(fd));
    } else if (errno == EINPROGRESS) {
        NewFromPool<ConnectSocket>(pool, event_loop, pool,
                                   std::move(fd), timeout,
#ifdef ENABLE_STOPWATCH
                                   *stopwatch,
#endif
                                   handler, cancel_ptr);
    } else {
        GError *error = new_error_errno();
        handler.OnSocketConnectError(error);
    }
}
