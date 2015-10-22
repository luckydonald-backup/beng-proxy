/*
 * An istream sink that copies data into a rubber allocation.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "sink_rubber.hxx"
#include "istream/istream.hxx"
#include "async.hxx"
#include "rubber.hxx"
#include "pool.hxx"
#include "util/Cast.hxx"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

struct RubberSink {
    struct istream *input;

    Rubber *rubber;
    unsigned rubber_id;

    size_t max_size, position;

    const RubberSinkHandler *handler;
    void *handler_ctx;

    struct async_operation async_operation;

    void FailTooLarge();
    void InvokeEof();

    void Abort();
};

static ssize_t
fd_read(FdType type, int fd, void *p, size_t size)
{
    return IsAnySocket(type)
        ? recv(fd, p, size, MSG_DONTWAIT)
        : read(fd, p, size);
}

void
RubberSink::FailTooLarge()
{
    rubber_remove(rubber, rubber_id);
    async_operation.Finished();

    if (input != nullptr)
        istream_free_handler(&input);

    handler->too_large(handler_ctx);
}

void
RubberSink::InvokeEof()
{
    async_operation.Finished();

    if (input != nullptr)
        istream_free_handler(&input);

    if (position == 0) {
        /* the stream was empty; remove the object from the rubber
           allocator */
        rubber_remove(rubber, rubber_id);
        rubber_id = 0;
    } else
        rubber_shrink(rubber, rubber_id, position);

    handler->done(rubber_id, position, handler_ctx);
}

/*
 * istream handler
 *
 */

static size_t
sink_rubber_input_data(const void *data, size_t length, void *ctx)
{
    auto *s = (RubberSink *)ctx;
    assert(s->position <= s->max_size);

    if (s->position + length > s->max_size) {
        /* too large, abort and invoke handler */

        s->FailTooLarge();
        return 0;
    }

    uint8_t *p = (uint8_t *)rubber_write(s->rubber, s->rubber_id);
    memcpy(p + s->position, data, length);
    s->position += length;

    return length;
}

static ssize_t
sink_rubber_input_direct(FdType type, int fd,
                         size_t max_length, void *ctx)
{
    auto *s = (RubberSink *)ctx;
    assert(s->position <= s->max_size);

    size_t length = s->max_size - s->position;
    if (length == 0) {
        /* already full, see what the file descriptor says */

        uint8_t dummy;
        ssize_t nbytes = fd_read(type, fd, &dummy, sizeof(dummy));
        if (nbytes > 0) {
            s->FailTooLarge();
            return ISTREAM_RESULT_CLOSED;
        }

        if (nbytes == 0) {
            s->InvokeEof();
            return ISTREAM_RESULT_CLOSED;
        }

        return ISTREAM_RESULT_ERRNO;
    }

    if (length > max_length)
        length = max_length;

    uint8_t *p = (uint8_t *)rubber_write(s->rubber, s->rubber_id);
    p += s->position;

    ssize_t nbytes = fd_read(type, fd, p, length);
    if (nbytes > 0)
        s->position += (size_t)nbytes;

    return nbytes;
}

static void
sink_rubber_input_eof(void *ctx)
{
    auto *s = (RubberSink *)ctx;

    assert(s->input != nullptr);
    s->input = nullptr;

    s->InvokeEof();
}

static void
sink_rubber_input_abort(GError *error, void *ctx)
{
    auto *s = (RubberSink *)ctx;

    assert(s->input != nullptr);
    s->input = nullptr;

    rubber_remove(s->rubber, s->rubber_id);
    s->async_operation.Finished();
    s->handler->error(error, s->handler_ctx);
}

static const struct istream_handler sink_rubber_input_handler = {
    .data = sink_rubber_input_data,
    .direct = sink_rubber_input_direct,
    .eof = sink_rubber_input_eof,
    .abort = sink_rubber_input_abort,
};


/*
 * async operation
 *
 */

inline void
RubberSink::Abort()
{
    rubber_remove(rubber, rubber_id);

    if (input != nullptr)
        istream_free_handler(&input);
}

/*
 * constructor
 *
 */

void
sink_rubber_new(struct pool *pool, struct istream *input,
                Rubber *rubber, size_t max_size,
                const RubberSinkHandler *handler, void *ctx,
                struct async_operation_ref *async_ref)
{
    assert(input != nullptr);
    assert(!istream_has_handler(input));
    assert(handler != nullptr);
    assert(handler->done != nullptr);
    assert(handler->out_of_memory != nullptr);
    assert(handler->too_large != nullptr);
    assert(handler->error != nullptr);

    const off_t available = istream_available(input, true);
    if (available > (off_t)max_size) {
        istream_close_unused(input);
        handler->too_large(ctx);
        return;
    }

    const off_t size = istream_available(input, false);
    assert(size == -1 || size >= available);
    assert(size <= (off_t)max_size);
    if (size == 0) {
        istream_close_unused(input);
        handler->done(0, 0, ctx);
        return;
    }

    const size_t allocate = size == -1
        ? max_size
        : (size_t)size;

    unsigned rubber_id = rubber_add(rubber, allocate);
    if (rubber_id == 0) {
        istream_close_unused(input);
        handler->out_of_memory(ctx);
        return;
    }

    auto s = NewFromPool<RubberSink>(*pool);
    s->rubber = rubber;
    s->rubber_id = rubber_id;
    s->max_size = allocate;
    s->position = 0;
    s->handler = handler;
    s->handler_ctx = ctx;

    istream_assign_handler(&s->input, input,
                           &sink_rubber_input_handler, s,
                           FD_ANY);

    s->async_operation.Init2<RubberSink, &RubberSink::async_operation>();
    async_ref->Set(s->async_operation);
}
