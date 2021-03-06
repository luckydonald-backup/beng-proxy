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

#include "lhttp_request.hxx"
#include "lhttp_stock.hxx"
#include "lhttp_address.hxx"
#include "http_client.hxx"
#include "http/Headers.hxx"
#include "HttpResponseHandler.hxx"
#include "fs/FilteredSocket.hxx"
#include "stock/Item.hxx"
#include "lease.hxx"
#include "istream/UnusedPtr.hxx"
#include "http/HeaderWriter.hxx"
#include "pool/pool.hxx"
#include "pool/LeakDetector.hxx"
#include "net/SocketDescriptor.hxx"

#include <stdexcept>

class LhttpRequest final : Lease, PoolLeakDetector {
    StockItem &stock_item;

    FilteredSocket socket;

public:
    explicit LhttpRequest(struct pool &_pool, EventLoop &event_loop,
                          StockItem &_stock_item) noexcept
        :PoolLeakDetector(_pool),
         stock_item(_stock_item), socket(event_loop)
    {
        socket.InitDummy(lhttp_stock_item_get_socket(stock_item),
                         lhttp_stock_item_get_type(stock_item));
    }

    void Start(struct pool &pool,
               const LhttpAddress &address,
               http_method_t method, HttpHeaders &&headers,
               UnusedIstreamPtr body,
               HttpResponseHandler &handler,
               CancellablePointer &cancel_ptr) noexcept {
        http_client_request(pool,
                            socket,
                            *this,
                            stock_item.GetStockName(),
                            method, address.uri, std::move(headers),
                            std::move(body), true,
                            handler, cancel_ptr);
    }

private:
    void Destroy() {
        this->~LhttpRequest();
    }

    /* virtual methods from class Lease */
    void ReleaseLease(bool reuse) noexcept override {
        if (socket.IsConnected())
            socket.Abandon();
        socket.Destroy();

        stock_item.Put(!reuse);

        Destroy();
    }
};

/*
 * constructor
 *
 */

void
lhttp_request(struct pool &pool, EventLoop &event_loop,
              LhttpStock &lhttp_stock,
              const char *site_name,
              const LhttpAddress &address,
              http_method_t method, HttpHeaders &&headers,
              UnusedIstreamPtr body,
              HttpResponseHandler &handler,
              CancellablePointer &cancel_ptr) noexcept
{
    try {
        address.options.Check();
    } catch (...) {
        body.Clear();

        handler.InvokeError(std::current_exception());
        return;
    }

    StockItem *stock_item;

    try {
        stock_item = lhttp_stock_get(&lhttp_stock, &address);
    } catch (...) {
        body.Clear();

        handler.InvokeError(std::current_exception());
        return;
    }

    lhttp_stock_item_set_site(*stock_item, site_name);
    lhttp_stock_item_set_uri(*stock_item, address.uri);

    auto request = NewFromPool<LhttpRequest>(pool, pool,
                                             event_loop, *stock_item);

    if (address.host_and_port != nullptr)
        headers.Write("host", address.host_and_port);

    request->Start(pool, address,
                   method, std::move(headers),
                   std::move(body),
                   handler, cancel_ptr);
}
