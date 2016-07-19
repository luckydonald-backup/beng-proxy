/*
 * TCP client connection pooling.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "tcp_stock.hxx"
#include "stock/MapStock.hxx"
#include "stock/Stock.hxx"
#include "stock/Class.hxx"
#include "stock/Item.hxx"
#include "address_list.hxx"
#include "gerrno.h"
#include "pool.hxx"
#include "event/SocketEvent.hxx"
#include "event/Duration.hxx"
#include "net/ConnectSocket.hxx"
#include "net/SocketAddress.hxx"
#include "net/SocketDescriptor.hxx"
#include "util/Cancellable.hxx"

#include <daemon/log.h>
#include <socket/address.h>

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>

struct TcpStockRequest {
    bool ip_transparent;

    SocketAddress bind_address, address;

    unsigned timeout;
};

struct TcpStockConnection final
    : HeapStockItem, ConnectSocketHandler, Cancellable {

    /**
     * To cancel the ClientSocket.
     */
    CancellablePointer cancel_ptr;

    int fd = -1;

    const int domain;

    SocketEvent event;

    TcpStockConnection(CreateStockItem c, int _domain,
                       CancellablePointer &_cancel_ptr)
        :HeapStockItem(c), domain(_domain),
         event(c.stock.GetEventLoop(), BIND_THIS_METHOD(EventCallback)) {
        _cancel_ptr = *this;

        cancel_ptr = nullptr;
    }

    ~TcpStockConnection() override;

    void EventCallback(short events);

    /* virtual methods from class Cancellable */
    void Cancel() override {
        assert(cancel_ptr);

        cancel_ptr.Cancel();
        InvokeCreateAborted();
    }

    /* virtual methods from class ConnectSocketHandler */
    void OnSocketConnectSuccess(SocketDescriptor &&fd) override;
    void OnSocketConnectError(GError *error) override;

    /* virtual methods from class StockItem */
    bool Borrow(gcc_unused void *ctx) override {
        event.Delete();
        return true;
    }

    bool Release(gcc_unused void *ctx) override {
        event.Add(EventDuration<60>::value);
        return true;
    }
};


/*
 * libevent callback
 *
 */

inline void
TcpStockConnection::EventCallback(short events)
{
    if ((events & EV_TIMEOUT) == 0) {
        char buffer;
        ssize_t nbytes;

        assert((events & EV_READ) != 0);

        nbytes = recv(fd, &buffer, sizeof(buffer), MSG_DONTWAIT);
        if (nbytes < 0)
            daemon_log(2, "error on idle TCP connection: %s\n",
                       strerror(errno));
        else if (nbytes > 0)
            daemon_log(2, "unexpected data in idle TCP connection\n");
    }

    InvokeIdleDisconnect();
    pool_commit();
}


/*
 * client_socket callback
 *
 */

void
TcpStockConnection::OnSocketConnectSuccess(SocketDescriptor &&new_fd)
{
    cancel_ptr = nullptr;

    fd = new_fd.Steal();
    event.Set(fd, EV_READ|EV_TIMEOUT);

    InvokeCreateSuccess();
}

void
TcpStockConnection::OnSocketConnectError(GError *error)
{
    cancel_ptr = nullptr;

    g_prefix_error(&error, "failed to connect to '%s': ", GetStockName());
    InvokeCreateError(error);
}

/*
 * stock class
 *
 */

static void
tcp_stock_create(gcc_unused void *ctx,
                 CreateStockItem c,
                 void *info,
                 struct pool &caller_pool,
                 CancellablePointer &cancel_ptr)
{
    TcpStockRequest *request = (TcpStockRequest *)info;

    auto *connection = new TcpStockConnection(c,
                                              request->address.GetFamily(),
                                              cancel_ptr);

    client_socket_new(c.stock.GetEventLoop(), caller_pool,
                      connection->domain, SOCK_STREAM, 0,
                      request->ip_transparent,
                      request->bind_address,
                      request->address,
                      request->timeout,
                      *connection,
                      connection->cancel_ptr);
}

TcpStockConnection::~TcpStockConnection()
{
    if (cancel_ptr)
        cancel_ptr.Cancel();
    else if (fd >= 0) {
        event.Delete();
        close(fd);
    }
}

static constexpr StockClass tcp_stock_class = {
    .create = tcp_stock_create,
};


/*
 * interface
 *
 */

StockMap *
tcp_stock_new(EventLoop &event_loop, unsigned limit)
{
    return new StockMap(event_loop, tcp_stock_class, nullptr,
                        limit, 16);
}

void
tcp_stock_get(StockMap *tcp_stock, struct pool *pool, const char *name,
              bool ip_transparent,
              SocketAddress bind_address,
              SocketAddress address,
              unsigned timeout,
              StockGetHandler &handler,
              CancellablePointer &cancel_ptr)
{
    assert(!address.IsNull());

    auto request = NewFromPool<TcpStockRequest>(*pool);
    request->ip_transparent = ip_transparent;
    request->bind_address = bind_address;
    request->address = address;
    request->timeout = timeout;

    if (name == nullptr) {
        char buffer[1024];
        if (!socket_address_to_string(buffer, sizeof(buffer),
                                      address.GetAddress(), address.GetSize()))
            buffer[0] = 0;

        if (!bind_address.IsNull()) {
            char bind_buffer[1024];
            if (!socket_address_to_string(bind_buffer, sizeof(bind_buffer),
                                          bind_address.GetAddress(),
                                          bind_address.GetSize()))
                bind_buffer[0] = 0;
            name = p_strcat(pool, bind_buffer, ">", buffer, nullptr);
        } else
            name = p_strdup(pool, buffer);
    }

    tcp_stock->Get(*pool, name, request, handler, cancel_ptr);
}

int
tcp_stock_item_get(const StockItem &item)
{
    auto *connection = (const TcpStockConnection *)&item;

    return connection->fd;
}

int
tcp_stock_item_get_domain(const StockItem &item)
{
    auto *connection = (const TcpStockConnection *)&item;

    assert(connection->fd >= 0);

    return connection->domain;
}
