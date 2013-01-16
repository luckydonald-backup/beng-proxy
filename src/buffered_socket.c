/*
 * Wrapper for a socket file descriptor with input and output
 * buffer management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "buffered_socket.h"
#include "fifo-buffer.h"
#include "gerrno.h"

#include <limits.h>
#include <errno.h>

static void
buffered_socket_closed_prematurely(struct buffered_socket *s)
{
    GError *error =
        g_error_new_literal(buffered_socket_quark(), 0,
                            "Peer closed the socket prematurely");
    s->handler->error(error, s->handler_ctx);
}

static void
buffered_socket_ended(struct buffered_socket *s)
{
#ifndef NDEBUG
    s->ended = true;
#endif

    if (s->handler->end == NULL)
        buffered_socket_closed_prematurely(s);
    else
        s->handler->end(s->handler_ctx);
}

static bool
buffered_socket_input_empty(const struct buffered_socket *s)
{
    assert(s != NULL);
    assert(!s->ended);

    return s->input == NULL || fifo_buffer_empty(s->input);
}

static bool
buffered_socket_input_full(const struct buffered_socket *s)
{
    assert(s != NULL);
    assert(!s->ended);

    return s->input != NULL && fifo_buffer_full(s->input);
}

int
buffered_socket_as_fd(struct buffered_socket *s)
{
    if (!buffered_socket_input_empty(s))
        /* can switch to the raw socket descriptor only if the input
           buffer is empty */
        return -1;

    return socket_wrapper_as_fd(&s->base);
}

size_t
buffered_socket_available(const struct buffered_socket *s)
{
    assert(s != NULL);
    assert(!s->ended);

    return s->input != NULL
        ? fifo_buffer_available(s->input)
        : 0;
}

void
buffered_socket_consumed(struct buffered_socket *s, size_t nbytes)
{
    assert(s != NULL);
    assert(!s->ended);
    assert(s->input != NULL);

    fifo_buffer_consume(s->input, nbytes);
}

/**
 * Invokes the data handler, and takes care for #BUFFERED_AGAIN.
 */
static enum buffered_result
buffered_socket_invoke_data(struct buffered_socket *s)
{
    assert(!buffered_socket_input_empty(s));

    while (true) {
        size_t length;
        const void *data = fifo_buffer_read(s->input, &length);
        data = fifo_buffer_read(s->input, &length);
        if (data == NULL)
            return BUFFERED_MORE;

#ifndef NDEBUG
        struct pool_notify notify;
        pool_notify(s->base.pool, &notify);
#endif

        enum buffered_result result =
            s->handler->data(data, length, s->handler_ctx);

#ifndef NDEBUG
        if (pool_denotify(&notify)) {
            assert(result == BUFFERED_CLOSED);
        } else {
            assert((result == BUFFERED_CLOSED) == !buffered_socket_valid(s));
        }
#endif

        if (result != BUFFERED_AGAIN)
            return result;
    }
}

static bool
buffered_socket_submit_from_buffer(struct buffered_socket *s)
{
    if (buffered_socket_input_empty(s))
        return true;

    enum buffered_result result = buffered_socket_invoke_data(s);
    assert((result == BUFFERED_CLOSED) || buffered_socket_valid(s));

    switch (result) {
    case BUFFERED_OK:
        assert(fifo_buffer_empty(s->input));
        assert(!s->expect_more);

        if (!buffered_socket_connected(s)) {
            buffered_socket_ended(s);
            return false;
        }

        return true;

    case BUFFERED_PARTIAL:
        assert(!fifo_buffer_empty(s->input));

        return buffered_socket_connected(s);

    case BUFFERED_MORE:
        s->expect_more = true;

        if (!buffered_socket_connected(s)) {
            buffered_socket_closed_prematurely(s);
            return false;
        }

        if (buffered_socket_full(s)) {
            GError *error =
                g_error_new_literal(buffered_socket_quark(), 0,
                                    "Input buffer overflow");
            s->handler->error(error, s->handler_ctx);
            return false;
        }

        return true;

    case BUFFERED_AGAIN:
        /* unreachable, has been handled by
           buffered_socket_invoke_data() */
        assert(false);
        return false;

    case BUFFERED_BLOCKING:
        socket_wrapper_unschedule_read(&s->base);
        return false;

    case BUFFERED_CLOSED:
        /* the buffered_socket object has been destroyed by the
           handler */
        return false;
    }

    /* unreachable */
    assert(false);
    return false;
}

/**
 * @return true if more data should be read from the socket
 */
static bool
buffered_socket_submit_direct(struct buffered_socket *s)
{
    assert(buffered_socket_connected(s));
    assert(buffered_socket_input_empty(s));

    s->expect_more = false;

    const enum direct_result result =
        s->handler->direct(s->base.fd, s->base.fd_type, s->handler_ctx);
    switch (result) {
    case DIRECT_OK:
        return true;

    case DIRECT_BLOCKING:
        socket_wrapper_unschedule_read(&s->base);
        return true;

    case DIRECT_EMPTY:
        return true;

    case DIRECT_END:
        buffered_socket_ended(s);
        return false;

    case DIRECT_CLOSED:
        return false;
    }

    /* unreachable */
    assert(false);
    return false;
}

static bool
buffered_socket_fill_buffer(struct buffered_socket *s)
{
    assert(buffered_socket_connected(s));

    struct fifo_buffer *buffer = s->input;
    if (buffer == NULL)
        buffer = s->input = fifo_buffer_new(s->base.pool, 8192);

    ssize_t nbytes = socket_wrapper_read_to_buffer(&s->base, buffer, INT_MAX);
    if (gcc_likely(nbytes > 0)) {
        /* success: data was added to the buffer */
        s->expect_more = false;
        return true;
    }

    if (nbytes == 0) {
        /* socket closed */

        if (s->expect_more) {
            buffered_socket_closed_prematurely(s);
            return false;
        }

        assert(s->handler->closed != NULL);

        const size_t remaining = fifo_buffer_available(buffer);

        if (!s->handler->closed(remaining, s->handler_ctx))
            return false;

        assert(!buffered_socket_connected(s));
        assert(s->input == buffer);
        assert(remaining == fifo_buffer_available(buffer));

        if (fifo_buffer_empty(buffer)) {
            buffered_socket_ended(s);
            return false;
        }

        return true;
    }

    if (nbytes == -2) {
        /* input buffer is full */
        socket_wrapper_unschedule_read(&s->base);
        return true;
    }

    if (nbytes == -1) {
        if (errno == EAGAIN) {
            socket_wrapper_schedule_read(&s->base, s->read_timeout);
            return true;
        } else {
            GError *error = new_error_errno_msg("recv() failed");
            s->handler->error(error, s->handler_ctx);
            return false;
        }
    }

    return true;
}

static bool
buffered_socket_try_read2(struct buffered_socket *s)
{
    assert(buffered_socket_valid(s));
    assert(!s->destroyed);
    assert(!s->ended);
    assert(s->reading);

    if (!buffered_socket_connected(s)) {
        assert(!buffered_socket_input_empty(s));

        buffered_socket_submit_from_buffer(s);
        return false;
    } else if (s->direct) {
        /* empty the remaining buffer before doing direct transfer */
        if (!buffered_socket_submit_from_buffer(s))
            return false;

        if (!s->direct)
            /* meanwhile, the "direct" flag was reverted by the
               handler - try again */
            return buffered_socket_try_read2(s);

        if (!buffered_socket_input_empty(s)) {
            /* there's still data in the buffer, but our handler isn't
               ready for consuming it - stop reading from the
               socket */
            socket_wrapper_unschedule_read(&s->base);
            return true;
        }

        if (!buffered_socket_submit_direct(s))
            return false;

        socket_wrapper_schedule_read(&s->base, s->read_timeout);
        return true;
    } else {
        if (!buffered_socket_fill_buffer(s))
            return false;

        if (!buffered_socket_submit_from_buffer(s))
            return false;

        socket_wrapper_schedule_read(&s->base, s->read_timeout);
        return true;
    }
}

static bool
buffered_socket_try_read(struct buffered_socket *s)
{
    assert(buffered_socket_valid(s));
    assert(!s->destroyed);
    assert(!s->ended);
    assert(!s->reading);

#ifndef NDEBUG
    struct pool_notify notify;
    pool_notify(s->base.pool, &notify);
    s->reading = true;
#endif

    const bool result = buffered_socket_try_read2(s);

#ifndef NDEBUG
    if (!pool_denotify(&notify)) {
        assert(s->reading);
        s->reading = false;
    }
#endif

    return result;
}

/*
 * socket_wrapper handler
 *
 */

static bool
buffered_socket_wrapper_write(void *ctx)
{
    struct buffered_socket *s = ctx;
    assert(!s->destroyed);
    assert(!s->ended);

    return s->handler->write(s->handler_ctx);
}

static bool
buffered_socket_wrapper_read(void *ctx)
{
    struct buffered_socket *s = ctx;
    assert(!s->destroyed);
    assert(!s->ended);

    return buffered_socket_try_read(s);
}

static bool
buffered_socket_wrapper_timeout(void *ctx)
{
    struct buffered_socket *s = ctx;
    assert(!s->destroyed);
    assert(!s->ended);

    if (s->handler->timeout != NULL)
        return s->handler->timeout(s->handler_ctx);

    s->handler->error(g_error_new_literal(buffered_socket_quark(), 0,
                                          "Timeout"),
                      s->handler_ctx);
    return false;
}

static const struct socket_handler buffered_socket_handler = {
    .read = buffered_socket_wrapper_read,
    .write = buffered_socket_wrapper_write,
    .timeout = buffered_socket_wrapper_timeout,
};

/*
 * public API
 *
 */

void
buffered_socket_init(struct buffered_socket *s, struct pool *pool,
                     int fd, enum istream_direct fd_type,
                     const struct timeval *read_timeout,
                     const struct timeval *write_timeout,
                     const struct buffered_socket_handler *handler, void *ctx)
{
    assert(handler != NULL);
    assert(handler->data != NULL);
    /* handler method closed() is optional */
    assert(handler->write != NULL);
    assert(handler->error != NULL);

    socket_wrapper_init(&s->base, pool, fd, fd_type,
                        &buffered_socket_handler, s);

    s->read_timeout = read_timeout;
    s->write_timeout = write_timeout;

    s->handler = handler;
    s->handler_ctx = ctx;
    s->input = NULL;
    s->direct = false;
    s->expect_more = true;

#ifndef NDEBUG
    s->reading = false;
    s->ended = false;
    s->destroyed = false;
#endif
}

bool
buffered_socket_empty(const struct buffered_socket *s)
{
    assert(s != NULL);
    assert(!s->ended);

    return s->input == NULL || fifo_buffer_empty(s->input);
}

bool
buffered_socket_full(const struct buffered_socket *s)
{
    return buffered_socket_input_full(s);
}

bool
buffered_socket_read(struct buffered_socket *s)
{
    assert(!s->reading);
    assert(!s->destroyed);
    assert(!s->ended);

    return buffered_socket_try_read(s);
}
