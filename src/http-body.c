/*
 * Utilities for reading a HTTP body, either request or response.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "http-body.h"

#include <assert.h>
#include <limits.h>

static inline int
http_body_valid(const struct http_body_reader *body)
{
    return body->output.pool != NULL;
}

/** determine how much can be read from the body */
static inline size_t
http_body_max_read(struct http_body_reader *body, size_t length)
{
    if (body->rest != (off_t)-1 && body->rest < (off_t)length)
        return (size_t)body->rest;
    else
        return length;
}

static void
http_body_consumed(struct http_body_reader *body, size_t nbytes)
{
    pool_t pool = body->output.pool;

    if (body->rest == (off_t)-1)
        return;

    assert((off_t)nbytes <= body->rest);

    body->rest -= (off_t)nbytes;
    if (body->rest > 0)
        return;

    pool_ref(pool);

    istream_invoke_eof(&body->output);

    pool_unref(pool);
}

void
http_body_consume_body(struct http_body_reader *body,
                       fifo_buffer_t buffer)
{
    const int chunked = body->rest == (off_t)-1;
    const void *data;
    size_t length, consumed;

    data = fifo_buffer_read(buffer, &length);
    if (data == NULL)
        return;

    length = http_body_max_read(body, length);
    consumed = istream_invoke_data(&body->output, data, length);
    assert(consumed <= length);

    if (!http_body_valid(body))
        return;

    if (consumed > 0) {
        fifo_buffer_consume(buffer, consumed);
        if (!chunked || body->rest != 0)
            http_body_consumed(body, consumed);
    }
}

ssize_t
http_body_try_direct(struct http_body_reader *body, int fd)
{
    ssize_t nbytes;

    assert(fd >= 0);
    assert(body->output.handler_direct & ISTREAM_SOCKET);
    assert(body->output.handler->direct != NULL);

    nbytes = istream_invoke_direct(&body->output,
                                   ISTREAM_SOCKET, fd,
                                   http_body_max_read(body, INT_MAX));
    if (nbytes > 0)
        http_body_consumed(body, (size_t)nbytes);

    return nbytes;
}

static void
http_body_dechunked_eof(void *ctx)
{
    struct http_body_reader *body = ctx;

    assert(http_body_valid(body));
    assert(body->rest == (off_t)-1);

    body->rest = 0;
}

istream_t
http_body_init(struct http_body_reader *body,
               const struct istream *stream, pool_t stream_pool,
               pool_t pool, off_t content_length)
{
    istream_t istream;

    assert(pool_contains(stream_pool, body, sizeof(*body)));

    body->output = *stream;
    body->output.pool = stream_pool;
    body->rest = content_length;

    istream = http_body_istream(body);
    if (content_length == (off_t)-1)
        istream = istream_dechunk_new(pool, istream,
                                      http_body_dechunked_eof, body);

    return istream;
}
