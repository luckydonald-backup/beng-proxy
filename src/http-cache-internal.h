#ifndef BENG_PROXY_HTTP_CACHE_INTERNAL_H
#define BENG_PROXY_HTTP_CACHE_INTERNAL_H

#include "http-cache.h"

#ifdef CACHE_LOG
#include <daemon/log.h>
#define cache_log(...) daemon_log(__VA_ARGS__)
#else
#define cache_log(...) do {} while (0)
#endif

static const off_t cacheable_size_limit = 256 * 1024;

struct http_cache_info {
    bool only_if_cached;

    /** does the request URI have a query string?  This information is
        important for RFC 2616 13.9 */
    bool has_query_string;

    /** when will the cached resource expire? (beng-proxy time) */
    time_t expires;

    /** when was the cached resource last modified on the widget
        server? (widget server time) */
    const char *last_modified;

    const char *etag;

    const char *vary;
};

struct http_cache_document {
    struct http_cache_info info;

    struct strmap *vary;

    http_status_t status;
    struct strmap *headers;

    size_t size;
    unsigned char *data;
};

struct http_cache_info *
http_cache_request_evaluate(pool_t pool,
                            http_method_t method, const char *uri,
                            const struct strmap *headers,
                            istream_t body);

/**
 * Checks whether the specified cache item fits the current request.
 * This is not true if the Vary headers mismatch.
 */
bool
http_cache_document_fits(const struct http_cache_document *document,
                         const struct strmap *headers);

/**
 * Check whether the request should invalidate the existing cache.
 */
bool
http_cache_request_invalidate(http_method_t method);

/**
 * Check whether the HTTP response should be put into the cache.
 */
bool
http_cache_response_evaluate(struct http_cache_info *info,
                             http_status_t status, const struct strmap *headers,
                             off_t body_available);

#endif
