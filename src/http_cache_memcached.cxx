/*
 * Caching HTTP responses.  Memcached backend.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http_cache_memcached.hxx"
#include "http_cache_choice.hxx"
#include "http_cache_internal.hxx"
#include "memcached_stock.hxx"
#include "memcached_client.hxx"
#include "growing_buffer.hxx"
#include "serialize.hxx"
#include "sink_header.hxx"
#include "strmap.h"
#include "tpool.h"
#include "background.hxx"
#include "istream_gb.hxx"
#include "istream.h"
#include "util/ConstBuffer.hxx"

#include <glib.h>

#include <string.h>

enum http_cache_memcached_type {
    TYPE_DOCUMENT = 2,
};

struct http_cache_memcached_request {
    struct pool *pool;

    struct memcached_stock *stock;

    struct pool *background_pool;
    struct background_manager *background;

    const char *uri;

    struct strmap *request_headers;

    bool in_choice;
    struct http_cache_choice *choice;

    uint32_t header_size;

    union {
        struct memcached_set_extras set;
    } extras;

    union {
        http_cache_memcached_flush_t flush;
        http_cache_memcached_get_t get;
        http_cache_memcached_put_t put;
    } callback;

    void *callback_ctx;

    struct async_operation_ref *async_ref;

    http_cache_memcached_request(struct pool &_pool)
        :pool(&_pool) {}

    http_cache_memcached_request(struct pool &_pool,
                                 struct memcached_stock &_stock,
                                 struct pool &_background_pool,
                                 struct background_manager &_background,
                                 const char *_uri,
                                 struct async_operation_ref &_async_ref)
        :pool(&_pool), stock(&_stock),
         background_pool(&_background_pool), background(&_background),
         uri(_uri),
         async_ref(&_async_ref) {}

    http_cache_memcached_request(struct pool &_pool,
                                 struct memcached_stock &_stock,
                                 struct pool &_background_pool,
                                 struct background_manager &_background,
                                 const char *_uri,
                                 struct strmap *_request_headers,
                                 http_cache_memcached_get_t _callback,
                                 void *_callback_ctx,
                                 struct async_operation_ref &_async_ref)
        :pool(&_pool), stock(&_stock),
         background_pool(&_background_pool), background(&_background),
         uri(_uri), request_headers(_request_headers),
         in_choice(false),
         callback_ctx(_callback_ctx),
         async_ref(&_async_ref) {
        callback.get = _callback;
    }

    http_cache_memcached_request(const struct http_cache_memcached_request &) = delete;
};

/*
 * Public functions and memcached-client callbacks
 *
 */

static void
http_cache_memcached_flush_response(enum memcached_response_status status,
                                    G_GNUC_UNUSED const void *extras,
                                    G_GNUC_UNUSED size_t extras_length,
                                    G_GNUC_UNUSED const void *key,
                                    G_GNUC_UNUSED size_t key_length,
                                    struct istream *value, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    if (value != nullptr)
        istream_close_unused(value);

    request->callback.flush(status == MEMCACHED_STATUS_NO_ERROR,
                            nullptr, request->callback_ctx);
}

static void
http_cache_memcached_flush_error(GError *error, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    request->callback.flush(false, error, request->callback_ctx);
}

static const struct memcached_client_handler http_cache_memcached_flush_handler = {
    .response = http_cache_memcached_flush_response,
    .error = http_cache_memcached_flush_error,
};

void
http_cache_memcached_flush(struct pool *pool, struct memcached_stock *stock,
                           http_cache_memcached_flush_t callback,
                           void *callback_ctx,
                           struct async_operation_ref *async_ref)
{
    auto request =
        NewFromPool<http_cache_memcached_request>(pool, *pool);

    request->callback.flush = callback;
    request->callback_ctx = callback_ctx;

    memcached_stock_invoke(pool, stock, MEMCACHED_OPCODE_FLUSH,
                           nullptr, 0,
                           nullptr, 0,
                           nullptr,
                           &http_cache_memcached_flush_handler, request,
                           async_ref);
}

static struct http_cache_document *
mcd_deserialize_document(struct pool *pool, ConstBuffer<void> &header,
                         const struct strmap *request_headers)
{
    auto document = PoolAlloc<http_cache_document>(pool);

    http_cache_info_init(&document->info);

    document->info.expires = deserialize_uint64(header);
    document->vary = deserialize_strmap(header, pool);
    document->status = (http_status_t)deserialize_uint16(header);
    document->headers = deserialize_strmap(header, pool);

    if (header.IsNull() || !http_status_is_valid(document->status))
        return nullptr;

    document->info.last_modified =
        strmap_get_checked(document->headers, "last-modified");
    document->info.etag = strmap_get_checked(document->headers, "etag");
    document->info.vary = strmap_get_checked(document->headers, "vary");

    if (!http_cache_document_fits(document, request_headers))
        /* Vary mismatch */
        return nullptr;

    return document;
}

static void
http_cache_memcached_get_response(enum memcached_response_status status,
                                  const void *extras, size_t extras_length,
                                  const void *key, size_t key_length,
                                  struct istream *value, void *ctx);

static void
http_cache_memcached_get_error(GError *error, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    request->callback.get(nullptr, nullptr, error, request->callback_ctx);
}

static const struct memcached_client_handler http_cache_memcached_get_handler = {
    .response = http_cache_memcached_get_response,
    .error = http_cache_memcached_get_error,
};

static void
background_callback(GError *error, void *ctx)
{
    background_job *job = (background_job *)ctx;

    if (error != nullptr) {
        cache_log(2, "http-cache: memcached failed: %s\n", error->message);
        g_error_free(error);
    }

    background_manager_remove(job);
}

static void
mcd_choice_get_callback(const char *key, bool unclean,
                        GError *error, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    if (unclean) {
        /* this choice record is unclean - start cleanup as a
           background job */
        struct pool *pool = pool_new_linear(request->background_pool,
                                      "http_cache_choice_cleanup", 8192);
        auto job = PoolAlloc<background_job>(pool);

        http_cache_choice_cleanup(pool, request->stock, request->uri,
                                  background_callback, job,
                                  background_job_add(request->background,
                                                     job));
        pool_unref(pool);
    }

    if (key == nullptr) {
        request->callback.get(nullptr, nullptr, error, request->callback_ctx);
        return;
    }

    request->in_choice = true;
    memcached_stock_invoke(request->pool, request->stock, MEMCACHED_OPCODE_GET,
                           nullptr, 0,
                           key, strlen(key),
                           nullptr,
                           &http_cache_memcached_get_handler, request,
                           request->async_ref);
}

static void
http_cache_memcached_header_done(void *header_ptr, size_t length,
                                 struct istream *tail, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;
    struct http_cache_document *document;

    ConstBuffer<void> header(header_ptr, length);

    auto type = (http_cache_memcached_type)deserialize_uint32(header);
    switch (type) {
    case TYPE_DOCUMENT:
        document = mcd_deserialize_document(request->pool, header,
                                            request->request_headers);
        if (document == nullptr) {
            if (request->in_choice)
                break;

            istream_close_unused(tail);

            http_cache_choice_get(request->pool, request->stock,
                                  request->uri, request->request_headers,
                                  mcd_choice_get_callback, request,
                                  request->async_ref);
            return;
        }

        request->callback.get(document, tail, nullptr, request->callback_ctx);
        return;
    }

    istream_close_unused(tail);
    request->callback.get(nullptr, nullptr, nullptr, request->callback_ctx);
}

static void
http_cache_memcached_header_error(GError *error, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    request->callback.get(nullptr, nullptr, error, request->callback_ctx);
}

static const struct sink_header_handler http_cache_memcached_header_handler = {
    .done = http_cache_memcached_header_done,
    .error = http_cache_memcached_header_error,
};

static void
http_cache_memcached_get_response(enum memcached_response_status status,
                                  G_GNUC_UNUSED const void *extras,
                                  G_GNUC_UNUSED size_t extras_length,
                                  G_GNUC_UNUSED const void *key,
                                  G_GNUC_UNUSED size_t key_length,
                                  struct istream *value, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    if (status == MEMCACHED_STATUS_KEY_NOT_FOUND && !request->in_choice) {
        if (value != nullptr)
            istream_close_unused(value);

        http_cache_choice_get(request->pool, request->stock,
                              request->uri, request->request_headers,
                              mcd_choice_get_callback, request,
                              request->async_ref);
        return;
    }

    if (status != MEMCACHED_STATUS_NO_ERROR || value == nullptr) {
        if (value != nullptr)
            istream_close_unused(value);

        request->callback.get(nullptr, nullptr, nullptr, request->callback_ctx);
        return;
    }

    sink_header_new(request->pool, value,
                    &http_cache_memcached_header_handler, request,
                    request->async_ref);
}

void
http_cache_memcached_get(struct pool *pool, struct memcached_stock *stock,
                         struct pool *background_pool,
                         struct background_manager *background,
                         const char *uri, struct strmap *request_headers,
                         http_cache_memcached_get_t callback,
                         void *callback_ctx,
                         struct async_operation_ref *async_ref)
{
    auto request =
        NewFromPool<http_cache_memcached_request>(pool, *pool,
                                                  *stock,
                                                  *background_pool,
                                                  *background,
                                                  uri, request_headers,
                                                  callback, callback_ctx,
                                                  *async_ref);

    const char *key;
    key = http_cache_choice_vary_key(pool, uri, nullptr);

    memcached_stock_invoke(pool, stock, MEMCACHED_OPCODE_GET,
                           nullptr, 0,
                           key, strlen(key),
                           nullptr,
                           &http_cache_memcached_get_handler, request,
                           async_ref);
}

static void
mcd_choice_commit_callback(GError *error, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    request->callback.put(error, request->callback_ctx);
}

static void
http_cache_memcached_put_response(enum memcached_response_status status,
                                  G_GNUC_UNUSED const void *extras,
                                  G_GNUC_UNUSED size_t extras_length,
                                  G_GNUC_UNUSED const void *key,
                                  G_GNUC_UNUSED size_t key_length,
                                  struct istream *value, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    if (value != nullptr)
        istream_close_unused(value);

    if (status != MEMCACHED_STATUS_NO_ERROR || /* error */
        request->choice == nullptr) { /* or no choice entry needed */
        request->callback.put(nullptr, request->callback_ctx);
        return;
    }

    http_cache_choice_commit(request->choice, request->stock,
                             mcd_choice_commit_callback, request,
                             request->async_ref);
}

static void
http_cache_memcached_put_error(GError *error, void *ctx)
{
    auto request = (http_cache_memcached_request *)ctx;

    request->callback.put(error, request->callback_ctx);
}

static const struct memcached_client_handler http_cache_memcached_put_handler = {
    .response = http_cache_memcached_put_response,
    .error = http_cache_memcached_put_error,
};

void
http_cache_memcached_put(struct pool *pool, struct memcached_stock *stock,
                         struct pool *background_pool,
                         struct background_manager *background,
                         const char *uri,
                         const struct http_cache_info *info,
                         struct strmap *request_headers,
                         http_status_t status, struct strmap *response_headers,
                         struct istream *value,
                         http_cache_memcached_put_t callback, void *callback_ctx,
                         struct async_operation_ref *async_ref)
{
    auto request =
        NewFromPool<http_cache_memcached_request>(pool, *pool, *stock,
                                                  *background_pool,
                                                  *background,
                                                  uri, *async_ref);

    struct strmap *vary;
    struct growing_buffer *gb;
    const char *key;

    const AutoRewindPool auto_rewind(tpool);

    vary = info->vary != nullptr
        ? http_cache_copy_vary(tpool, info->vary, request_headers)
        : nullptr;

    request->choice = vary != nullptr
        ? http_cache_choice_prepare(pool, uri, info, vary)
        : nullptr;

    key = http_cache_choice_vary_key(pool, uri, vary);

    gb = growing_buffer_new(pool, 1024);

    /* type */
    serialize_uint32(gb, TYPE_DOCUMENT);

    serialize_uint64(gb, info->expires);
    serialize_strmap(gb, vary);

    /* serialize status + response headers */
    serialize_uint16(gb, status);
    serialize_strmap(gb, response_headers);

    request->header_size = g_htonl(growing_buffer_size(gb));

    /* append response body */
    value = istream_cat_new(pool,
                            istream_memory_new(pool, &request->header_size,
                                               sizeof(request->header_size)),
                            istream_gb_new(pool, gb), value, nullptr);

    request->extras.set.flags = 0;
    request->extras.set.expiration = info->expires > 0
        ? g_htonl(info->expires) : g_htonl(3600);

    request->callback.put = callback;
    request->callback_ctx = callback_ctx;

    memcached_stock_invoke(pool, stock,
                           MEMCACHED_OPCODE_SET,
                           &request->extras.set, sizeof(request->extras.set),
                           key, strlen(key),
                           value,
                           &http_cache_memcached_put_handler, request,
                           async_ref);
}

static void
mcd_background_response(G_GNUC_UNUSED enum memcached_response_status status,
                        G_GNUC_UNUSED const void *extras,
                        G_GNUC_UNUSED size_t extras_length,
                        G_GNUC_UNUSED const void *key,
                        G_GNUC_UNUSED size_t key_length,
                        struct istream *value, void *ctx)
{
    background_job *job = (background_job *)ctx;

    if (value != nullptr)
        istream_close_unused(value);

    background_manager_remove(job);
}

static void
mcd_background_error(GError *error, void *ctx)
{
    background_job *job = (background_job *)ctx;

    if (error != nullptr) {
        cache_log(2, "http-cache: put failed: %s\n", error->message);
        g_error_free(error);
    }

    background_manager_remove(job);
}

static const struct memcached_client_handler mcd_background_handler = {
    .response = mcd_background_response,
    .error = mcd_background_error,
};

static void
mcd_background_delete(struct memcached_stock *stock,
                      struct pool *background_pool,
                      struct background_manager *background,
                      const char *uri, struct strmap *vary)
{
    struct pool *pool = pool_new_linear(background_pool,
                                        "http_cache_memcached_bkg_delete", 1024);
    auto job = PoolAlloc<background_job>(pool);
    const char *key = http_cache_choice_vary_key(pool, uri, vary);

    memcached_stock_invoke(pool, stock,
                           MEMCACHED_OPCODE_DELETE,
                           nullptr, 0,
                           key, strlen(key),
                           nullptr,
                           &mcd_background_handler, job,
                           background_job_add(background, job));
    pool_unref(pool);
}

void
http_cache_memcached_remove_uri(struct memcached_stock *stock,
                                struct pool *background_pool,
                                struct background_manager *background,
                                const char *uri)
{
    mcd_background_delete(stock, background_pool, background, uri, nullptr);

    struct pool *pool = pool_new_linear(background_pool,
                                        "http_cache_memcached_remove_uri", 8192);
    auto job = PoolAlloc<background_job>(pool);
    http_cache_choice_delete(pool, stock, uri,
                             background_callback, job,
                             background_job_add(background, job));

    pool_unref(pool);
}

struct match_data {
    struct background_job job;

    struct memcached_stock *stock;
    struct pool *background_pool;
    struct background_manager *background;
    const char *uri;
    struct strmap *headers;
};

static bool
mcd_delete_filter_callback(const struct http_cache_document *document,
                           GError *error, void *ctx)
{
    match_data *data = (match_data *)ctx;

    if (document != nullptr) {
        /* discard documents matching the Vary specification */
        if (http_cache_document_fits(document, data->headers)) {
            mcd_background_delete(data->stock, data->background_pool,
                                  data->background, data->uri,
                                  document->vary);
            return false;
        } else
            return true;
    } else {
        if (error != nullptr) {
            cache_log(2, "http-cache: memcached failed: %s\n", error->message);
            g_error_free(error);
        }

        background_manager_remove(&data->job);
        return false;
    }
}

void
http_cache_memcached_remove_uri_match(struct memcached_stock *stock,
                                      struct pool *background_pool,
                                      struct background_manager *background,
                                      const char *uri, struct strmap *headers)
{
    /* delete the main document */
    mcd_background_delete(stock, background_pool, background, uri, nullptr);

    struct pool *pool = pool_new_linear(background_pool,
                                        "http_cache_memcached_remove_uri", 8192);

    /* now delete all matching Vary documents */
    auto data = PoolAlloc<match_data>(pool);
    data->stock = stock;
    data->background_pool = background_pool;
    data->background = background;
    data->uri = p_strdup(pool, uri);
    data->headers = strmap_dup(pool, headers, 17);

    http_cache_choice_filter(pool, stock, uri,
                             mcd_delete_filter_callback, data,
                             background_job_add(background, &data->job));

    pool_unref(pool);
}
