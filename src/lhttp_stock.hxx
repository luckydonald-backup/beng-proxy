/*
 * Launch and manage "Local HTTP" child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_LHTTP_STOCK_HXX
#define BENG_PROXY_LHTTP_STOCK_HXX

#include "istream-direct.h"
#include "glibfwd.hxx"

#include <inline/compiler.h>

struct pool;
struct lhttp_stock;
struct StockItem;
struct StockGetHandler;
struct lhttp_address;
struct async_operation_ref;

struct lhttp_stock *
lhttp_stock_new(struct pool *pool, unsigned limit, unsigned max_idle);

void
lhttp_stock_free(struct lhttp_stock *lhttp_stock);

StockItem *
lhttp_stock_get(struct lhttp_stock *lhttp_stock, struct pool *pool,
                const struct lhttp_address *address,
                GError **error_r);

/**
 * Returns the socket descriptor of the specified stock item.
 */
gcc_pure
int
lhttp_stock_item_get_socket(const StockItem *item);

gcc_pure
enum istream_direct
lhttp_stock_item_get_type(const StockItem *item);

/**
 * Wrapper for hstock_put().
 */
void
lhttp_stock_put(struct lhttp_stock *stock, StockItem *item, bool destroy);

#endif
