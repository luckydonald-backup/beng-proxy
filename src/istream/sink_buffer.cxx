#include "sink_buffer.hxx"
#include "istream.hxx"
#include "Sink.hxx"
#include "pool.hxx"
#include "util/Cancellable.hxx"

#include <glib.h>

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

struct BufferSink final : IstreamSink, Cancellable {
    struct pool *pool;

    unsigned char *const buffer;
    const size_t size;
    size_t position = 0;

    const struct sink_buffer_handler *const handler;
    void *handler_ctx;

    BufferSink(struct pool &_pool, Istream &_input, size_t available,
               const struct sink_buffer_handler &_handler, void *ctx,
               CancellablePointer &cancel_ptr)
        :IstreamSink(_input, FD_ANY), pool(&_pool),
         buffer((unsigned char *)p_malloc(pool, available)),
         size(available),
         handler(&_handler), handler_ctx(ctx) {
        cancel_ptr = *this;
    }

    /* virtual methods from class Cancellable */
    void Cancel() override;

    /* virtual methods from class IstreamHandler */
    size_t OnData(const void *data, size_t length) override;
    ssize_t OnDirect(FdType type, int fd, size_t max_length) override;
    void OnEof() override;
    void OnError(GError *error) override;
};

static GQuark
sink_buffer_quark(void)
{
    return g_quark_from_static_string("sink_buffer");
}

/*
 * istream handler
 *
 */

inline size_t
BufferSink::OnData(const void *data, size_t length)
{
    assert(position < size);
    assert(length <= size - position);

    memcpy(buffer + position, data, length);
    position += length;

    return length;
}

inline ssize_t
BufferSink::OnDirect(FdType type, int fd, size_t max_length)
{
    size_t length = size - position;
    if (length > max_length)
        length = max_length;

    ssize_t nbytes = IsAnySocket(type)
        ? recv(fd, buffer + position, length, MSG_DONTWAIT)
        : read(fd, buffer + position, length);
    if (nbytes > 0)
        position += (size_t)nbytes;

    return nbytes;
}

inline void
BufferSink::OnEof()
{
    assert(position == size);

    handler->done(buffer, size, handler_ctx);
}

inline void
BufferSink::OnError(GError *error)
{
    handler->error(error, handler_ctx);
}


/*
 * async operation
 *
 */

void
BufferSink::Cancel()
{
    const ScopePoolRef ref(*pool TRACE_ARGS);
    input.Close();
}


/*
 * constructor
 *
 */

void
sink_buffer_new(struct pool &pool, Istream &input,
                const struct sink_buffer_handler &handler, void *ctx,
                CancellablePointer &cancel_ptr)
{
    static char empty_buffer[1];

    assert(!input.HasHandler());
    assert(handler.done != nullptr);
    assert(handler.error != nullptr);

    off_t available = input.GetAvailable(false);
    if (available == -1 || available >= 0x10000000) {
        input.CloseUnused();

        GError *error =
            g_error_new_literal(sink_buffer_quark(), 0,
                                available < 0
                                ? "unknown stream length"
                                : "stream is too large");
        handler.error(error, ctx);
        return;
    }

    if (available == 0) {
        input.CloseUnused();
        handler.done(empty_buffer, 0, ctx);
        return;
    }

    NewFromPool<BufferSink>(pool, pool, input, available,
                            handler, ctx, cancel_ptr);
}
