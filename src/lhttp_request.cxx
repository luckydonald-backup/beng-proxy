/*
 * High level "Local HTTP" client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lhttp_request.hxx"
#include "lhttp_stock.hxx"
#include "lhttp_address.hxx"
#include "http_response.hxx"
#include "http_client.hxx"
#include "http_headers.hxx"
#include "stock/Item.hxx"
#include "lease.hxx"
#include "istream/istream.hxx"
#include "header_writer.hxx"
#include "pool.hxx"
#include "GException.hxx"
#include "net/SocketDescriptor.hxx"

#include <stdexcept>

struct LhttpRequest final : Lease {
    StockItem &stock_item;

    explicit LhttpRequest(StockItem &_stock_item)
        :stock_item(_stock_item) {}

    /* virtual methods from class Lease */
    void ReleaseLease(bool reuse) override {
        stock_item.Put(!reuse);
    }
};

/*
 * constructor
 *
 */

void
lhttp_request(struct pool &pool, EventLoop &event_loop,
              LhttpStock &lhttp_stock,
              const LhttpAddress &address,
              http_method_t method, HttpHeaders &&headers,
              Istream *body,
              HttpResponseHandler &handler,
              CancellablePointer &cancel_ptr)
{
    try {
        address.options.Check();
    } catch (...) {
        /* need to hold this pool reference because it is guaranteed
           that the pool stays alive while the HttpResponseHandler
           runs, even if all other pool references are removed */
        const ScopePoolRef ref(pool TRACE_ARGS);

        if (body != nullptr)
            body->CloseUnused();

        handler.InvokeError(ToGError(std::current_exception()));
        return;
    }

    GError *error = nullptr;
    StockItem *stock_item =
        lhttp_stock_get(&lhttp_stock, &pool, &address,
                        &error);
    if (stock_item == nullptr) {
        /* need to hold this pool reference because it is guaranteed
           that the pool stays alive while the HttpResponseHandler
           runs, even if all other pool references are removed */
        const ScopePoolRef ref(pool TRACE_ARGS);

        if (body != nullptr)
            body->CloseUnused();

        handler.InvokeError(error);
        return;
    }

    auto request = NewFromPool<LhttpRequest>(pool, *stock_item);

    if (address.host_and_port != nullptr)
        headers.Write("host", address.host_and_port);

    http_client_request(pool, event_loop,
                        lhttp_stock_item_get_socket(*stock_item),
                        lhttp_stock_item_get_type(*stock_item),
                        *request,
                        stock_item->GetStockName(),
                        nullptr, nullptr,
                        method, address.uri, std::move(headers), body, true,
                        handler, cancel_ptr);
}
