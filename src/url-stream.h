/*
 * High level HTTP client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_URL_STREAM_H
#define __BENG_URL_STREAM_H

#include "pool.h"
#include "strmap.h"
#include "growing-buffer.h"
#include "http.h"
#include "istream.h"

typedef struct url_stream *url_stream_t;

typedef void (*url_stream_callback_t)(http_status_t status, strmap_t headers,
                                      off_t content_length, istream_t body,
                                      void *ctx);

url_stream_t attr_malloc
url_stream_new(pool_t pool,
               http_method_t method, const char *url,
               growing_buffer_t headers,
               off_t content_length, istream_t body,
               url_stream_callback_t callback, void *ctx);

/**
 * Cancels the transfer.  You must not call this method after the
 * callback has been invoked.
 */
void
url_stream_close(url_stream_t us);

#endif
