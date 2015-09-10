/*
 * High level WAS client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "was_glue.hxx"
#include "was_quark.h"
#include "was_stock.hxx"
#include "was_client.hxx"
#include "http_response.hxx"
#include "lease.hxx"
#include "tcp_stock.hxx"
#include "stock/GetHandler.hxx"
#include "abort_close.hxx"
#include "ChildOptions.hxx"
#include "istream/istream.hxx"
#include "istream/istream_hold.hxx"
#include "pool.hxx"
#include "util/ConstBuffer.hxx"

#include <daemon/log.h>

#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

struct WasRequest final : public StockGetHandler, Lease {
    struct pool &pool;

    StockMap &was_stock;
    const char *action;
    StockItem *stock_item;

    http_method_t method;
    const char *uri;
    const char *script_name;
    const char *path_info;
    const char *query_string;
    struct strmap *headers;
    struct istream *body;

    ConstBuffer<const char *> parameters;

    struct http_response_handler_ref handler;
    struct async_operation_ref *async_ref;

    WasRequest(struct pool &_pool, StockMap &_was_stock,
               const char *_action,
               http_method_t _method, const char *_uri,
               const char *_script_name, const char *_path_info,
               const char *_query_string,
               struct strmap *_headers,
               ConstBuffer<const char *> _parameters,
               const struct http_response_handler &_handler,
               void *_handler_ctx,
               struct async_operation_ref *_async_ref)
        :pool(_pool), was_stock(_was_stock),
         action(_action), method(_method),
         uri(_uri), script_name(_script_name),
         path_info(_path_info), query_string(_query_string),
         headers(_headers), parameters(_parameters),
         async_ref(_async_ref) {
        handler.Set(_handler, _handler_ctx);
    }

    /* virtual methods from class StockGetHandler */
    void OnStockItemReady(StockItem &item) override;
    void OnStockItemError(GError *error) override;

    /* virtual methods from class Lease */
    void ReleaseLease(bool reuse) override {
        was_stock_put(&was_stock, *stock_item, !reuse);
    }
};

/*
 * stock callback
 *
 */

void
WasRequest::OnStockItemReady(StockItem &item)
{
    stock_item = &item;

    const struct was_process &process = was_stock_item_get(item);

    was_client_request(&pool, process.control_fd,
                       process.input_fd, process.output_fd,
                       *this,
                       method, uri,
                       script_name, path_info,
                       query_string,
                       headers, body,
                       parameters,
                       handler.handler, handler.ctx,
                       async_ref);
}

void
WasRequest::OnStockItemError(GError *error)
{
    handler.InvokeAbort(error);

    if (body != nullptr)
        istream_close_unused(body);
}

/*
 * constructor
 *
 */

void
was_request(struct pool *pool, StockMap *was_stock,
            const ChildOptions &options,
            const char *action,
            const char *path,
            ConstBuffer<const char *> args,
            ConstBuffer<const char *> env,
            http_method_t method, const char *uri,
            const char *script_name, const char *path_info,
            const char *query_string,
            struct strmap *headers, struct istream *body,
            ConstBuffer<const char *> parameters,
            const struct http_response_handler *handler,
            void *handler_ctx,
            struct async_operation_ref *async_ref)
{
    if (action == nullptr)
        action = path;

    auto request = NewFromPool<WasRequest>(*pool, *pool, *was_stock,
                                           action, method, uri, script_name,
                                           path_info, query_string,
                                           headers, parameters,
                                           *handler, handler_ctx,
                                           async_ref);

    if (body != nullptr) {
        request->body = istream_hold_new(pool, body);
        async_ref = &async_close_on_abort(*pool, *request->body, *async_ref);
    } else
        request->body = nullptr;

    was_stock_get(was_stock, pool,
                  options,
                  action, args, env,
                  *request, *async_ref);
}
