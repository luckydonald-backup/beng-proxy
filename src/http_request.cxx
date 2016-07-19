/*
 * High level HTTP client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http_request.hxx"
#include "http_response.hxx"
#include "http_client.hxx"
#include "http_headers.hxx"
#include "http_address.hxx"
#include "header_writer.hxx"
#include "tcp_stock.hxx"
#include "tcp_balancer.hxx"
#include "stock/GetHandler.hxx"
#include "stock/Item.hxx"
#include "async.hxx"
#include "growing_buffer.hxx"
#include "lease.hxx"
#include "abort_close.hxx"
#include "failure.hxx"
#include "istream/istream.hxx"
#include "istream/istream_hold.hxx"
#include "filtered_socket.hxx"
#include "pool.hxx"
#include "net/SocketAddress.hxx"

#include <inline/compiler.h>

#include <string.h>

struct HttpRequest final : public StockGetHandler, Lease, HttpResponseHandler {
    struct pool &pool;
    EventLoop &event_loop;

    TcpBalancer &tcp_balancer;

    const unsigned session_sticky;

    const SocketFilter *const filter;
    SocketFilterFactory *const filter_factory;

    StockItem *stock_item;
    SocketAddress current_address;

    const http_method_t method;
    const HttpAddress &address;
    HttpHeaders headers;
    Istream *body;

    unsigned retries;

    HttpResponseHandler &handler;
    struct async_operation_ref *const async_ref;

    HttpRequest(struct pool &_pool, EventLoop &_event_loop,
                TcpBalancer &_tcp_balancer,
                unsigned _session_sticky,
                const SocketFilter *_filter,
                SocketFilterFactory *_filter_factory,
                http_method_t _method,
                const HttpAddress &_address,
                HttpHeaders &&_headers,
                HttpResponseHandler &_handler,
                struct async_operation_ref &_async_ref)
        :pool(_pool), event_loop(_event_loop), tcp_balancer(_tcp_balancer),
         session_sticky(_session_sticky),
         filter(_filter), filter_factory(_filter_factory),
         method(_method), address(_address),
         headers(std::move(_headers)),
         handler(_handler), async_ref(&_async_ref)
    {
    }

    void Dispose() {
        if (body != nullptr)
            body->CloseUnused();
    }

    void Failed(GError *error) {
        Dispose();
        handler.InvokeError(error);
    }

    /* virtual methods from class StockGetHandler */
    void OnStockItemReady(StockItem &item) override;
    void OnStockItemError(GError *error) override;

private:
    /* virtual methods from class Lease */
    void ReleaseLease(bool reuse) override {
        stock_item->Put(!reuse);
    }

    /* virtual methods from class HttpResponseHandler */
    void OnHttpResponse(http_status_t status, StringMap &&headers,
                        Istream *body) override;
    void OnHttpError(GError *error) override;
};

/**
 * Is the specified error a server failure, that justifies
 * blacklisting the server for a while?
 */
static bool
is_server_failure(GError *error)
{
    return error->domain == http_client_quark() &&
        error->code != HTTP_CLIENT_UNSPECIFIED;
}

/*
 * HTTP response handler
 *
 */

void
HttpRequest::OnHttpResponse(http_status_t status, StringMap &&_headers,
                            Istream *_body)
{
    failure_unset(current_address, FAILURE_RESPONSE);

    handler.InvokeResponse(status, std::move(_headers), _body);
}

void
HttpRequest::OnHttpError(GError *error)
{
    if (retries > 0 && body == nullptr &&
        error->domain == http_client_quark() &&
        error->code == HTTP_CLIENT_REFUSED) {
        /* the server has closed the connection prematurely, maybe
           because it didn't want to get any further requests on that
           TCP connection.  Let's try again. */

        g_error_free(error);

        --retries;
        tcp_balancer_get(tcp_balancer, pool,
                         false, SocketAddress::Null(),
                         session_sticky,
                         address.addresses,
                         30,
                         *this, *async_ref);
    } else {
        if (is_server_failure(error))
            failure_set(current_address, FAILURE_RESPONSE,
                        std::chrono::seconds(20));

        handler.InvokeError(error);
    }
}

/*
 * stock callback
 *
 */

void
HttpRequest::OnStockItemReady(StockItem &item)
{
    stock_item = &item;
    current_address = tcp_balancer_get_last();

    void *filter_ctx = nullptr;
    if (filter_factory != nullptr) {
        GError *error = nullptr;
        filter_ctx = filter_factory->CreateFilter(&error);
        if (filter_ctx == nullptr) {
            ReleaseLease(true);
            Failed(error);
            return;
        }
    }

    http_client_request(pool, event_loop,
                        tcp_stock_item_get(item),
                        tcp_stock_item_get_domain(item) == AF_LOCAL
                        ? FdType::FD_SOCKET : FdType::FD_TCP,
                        *this,
                        item.GetStockName(),
                        filter, filter_ctx,
                        method, address.path, std::move(headers),
                        body, true,
                        *this, *async_ref);
}

void
HttpRequest::OnStockItemError(GError *error)
{
    Failed(error);
}

/*
 * constructor
 *
 */

void
http_request(struct pool &pool, EventLoop &event_loop,
             TcpBalancer &tcp_balancer,
             unsigned session_sticky,
             const SocketFilter *filter, SocketFilterFactory *filter_factory,
             http_method_t method,
             const HttpAddress &uwa,
             HttpHeaders &&headers,
             Istream *body,
             HttpResponseHandler &handler,
             struct async_operation_ref &_async_ref)
{
    assert(uwa.host_and_port != nullptr);
    assert(uwa.path != nullptr);
    assert(body == nullptr || !body->HasHandler());

    auto hr = NewFromPool<HttpRequest>(pool, pool, event_loop, tcp_balancer,
                                       session_sticky, filter, filter_factory,
                                       method, uwa, std::move(headers),
                                       handler, _async_ref);

    CancellablePointer *async_ref = &_async_ref;
    if (body != nullptr) {
        body = istream_hold_new(pool, *body);
        async_ref = &async_close_on_abort(pool, *body, *async_ref);
    }

    hr->body = body;

    if (uwa.host_and_port != nullptr)
        hr->headers.Write("host", uwa.host_and_port);

    hr->headers.Write("connection", "keep-alive");

    hr->retries = 2;
    tcp_balancer_get(tcp_balancer, pool,
                     false, SocketAddress::Null(),
                     session_sticky,
                     uwa.addresses,
                     30,
                     *hr, *async_ref);
}
