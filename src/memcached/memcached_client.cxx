/*
 * memcached client implementation.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "memcached_client.hxx"
#include "memcached_packet.hxx"
#include "buffered_socket.hxx"
#include "please.hxx"
#include "async.hxx"
#include "pevent.hxx"
#include "istream/istream_internal.hxx"
#include "istream/istream_oo.hxx"
#include "pool.hxx"
#include "util/Cast.hxx"
#include "util/ByteOrder.hxx"

#include <daemon/log.h>

#include <glib.h>

#include <errno.h>
#include <sys/socket.h>
#include <string.h>

struct MemcachedClient {
    enum class ReadState {
        HEADER,
        EXTRAS,
        KEY,
        VALUE,
        END,
    };

    struct pool *pool, *caller_pool;

    /* I/O */
    BufferedSocket socket;
    struct lease_ref lease_ref;

    /* request */
    struct {
        const struct memcached_client_handler *handler;
        void *handler_ctx;

        struct istream *istream;
    } request;

    struct async_operation request_async;

    /* response */
    struct {
        ReadState read_state;

        /**
         * This flag is true if we are currently calling the
         * #memcached_client_handler.  During this period,
         * memcached_client_socket_read() does nothing, to prevent
         * recursion.
         */
        bool in_handler;

        struct memcached_response_header header;

        unsigned char *extras;

        struct {
            void *buffer;
            unsigned char *tail;
            size_t remaining;
        } key;

        /**
         * Total number of bytes remaining to read from the response,
         * including extras and key.
         */
        size_t remaining;
    } response;

    struct istream response_value;

    /* istream handler */

    size_t OnData(const void *data, size_t length);

    ssize_t OnDirect(gcc_unused FdType type, gcc_unused int fd,
                     gcc_unused size_t max_length) {
        gcc_unreachable();
    }

    void OnEof();
    void OnError(GError *error);
};

static const struct timeval memcached_client_timeout = {
    .tv_sec = 5,
    .tv_usec = 0,
};

static inline bool
memcached_connection_valid(const MemcachedClient *client)
{
    return client->socket.IsValid();
}

static inline bool
memcached_client_check_direct(const MemcachedClient *client)
{
    assert(client->socket.IsConnected());
    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);

    return istream_check_direct(&client->response_value,
                                client->socket.GetType());
}

static void
memcached_client_schedule_write(MemcachedClient *client)
{
    client->socket.ScheduleWrite();
}

/**
 * Release the socket held by this object.
 */
static void
memcached_client_release_socket(MemcachedClient *client, bool reuse)
{
    assert(client != nullptr);

    client->socket.Abandon();
    p_lease_release(client->lease_ref, reuse, *client->pool);
}

static void
memcached_client_destroy_socket(MemcachedClient *client, bool reuse)
{
    assert(client != nullptr);

    if (client->socket.IsConnected())
        memcached_client_release_socket(client, reuse);
    client->socket.Destroy();
}

/**
 * Release resources held by this object: the event object, the socket
 * lease, and the pool reference.
 */
static void
memcached_client_release(MemcachedClient *client, bool reuse)
{
    assert(client != nullptr);

    if (client->socket.IsValid())
        memcached_client_destroy_socket(client, reuse);

    pool_unref(client->pool);
}

static void
memcached_connection_abort_response_header(MemcachedClient *client,
                                           GError *error)
{
    assert(client != nullptr);
    assert(client->response.read_state == MemcachedClient::ReadState::HEADER ||
           client->response.read_state == MemcachedClient::ReadState::EXTRAS ||
           client->response.read_state == MemcachedClient::ReadState::KEY);

    client->request_async.Finished();

    if (client->socket.IsValid())
        memcached_client_destroy_socket(client, false);

    client->request.handler->error(error, client->request.handler_ctx);
    pool_unref(client->caller_pool);

    client->response.read_state = MemcachedClient::ReadState::END;

    if (client->request.istream != nullptr)
        istream_free_handler(&client->request.istream);

    pool_unref(client->pool);
}

static void
memcached_connection_abort_response_value(MemcachedClient *client,
                                          GError *error)
{
    assert(client != nullptr);
    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->request.istream == nullptr);

    if (client->socket.IsValid())
        memcached_client_destroy_socket(client, false);

    client->response.read_state = MemcachedClient::ReadState::END;
    istream_deinit_abort(&client->response_value, error);

    pool_unref(client->caller_pool);
    pool_unref(client->pool);
}

static void
memcached_connection_abort_response(MemcachedClient *client,
                                    GError *error)
{
    assert(client->response.read_state != MemcachedClient::ReadState::END);

    switch (client->response.read_state) {
    case MemcachedClient::ReadState::HEADER:
    case MemcachedClient::ReadState::EXTRAS:
    case MemcachedClient::ReadState::KEY:
        memcached_connection_abort_response_header(client, error);
        return;

    case MemcachedClient::ReadState::VALUE:
        memcached_connection_abort_response_value(client, error);
        return;

    case MemcachedClient::ReadState::END:
        assert(false);
        break;
    }

    /* unreachable */
    assert(false);
}

/*
 * response value istream
 *
 */

static inline MemcachedClient *
istream_to_memcached_client(struct istream *istream)
{
    return &ContainerCast2(*istream, &MemcachedClient::response_value);
}

static off_t
istream_memcached_available(struct istream *istream, gcc_unused bool partial)
{
    MemcachedClient *client = istream_to_memcached_client(istream);

    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->request.istream == nullptr);

    return client->response.remaining;
}

static void
istream_memcached_read(struct istream *istream)
{
    MemcachedClient *client = istream_to_memcached_client(istream);

    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->request.istream == nullptr);

    if (client->response.in_handler)
        /* avoid recursion; the memcached_client_handler caller will
           continue parsing the response if possible */
        return;

    if (client->socket.IsConnected())
        client->socket.SetDirect(memcached_client_check_direct(client));

    client->socket.Read(true);
}

static void
istream_memcached_close(struct istream *istream)
{
    MemcachedClient *client = istream_to_memcached_client(istream);
    struct pool *caller_pool = client->caller_pool;

    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->request.istream == nullptr);

    memcached_client_release(client, false);

    istream_deinit(&client->response_value);
    pool_unref(caller_pool);
}

static const struct istream_class memcached_response_value = {
    .available = istream_memcached_available,
    .read = istream_memcached_read,
    .close = istream_memcached_close,
};

/*
 * response parser
 *
 */

static BufferedResult
memcached_submit_response(MemcachedClient *client)
{
    assert(client->response.read_state == MemcachedClient::ReadState::KEY);

    client->request_async.Finished();

    if (client->request.istream != nullptr) {
        /* at this point, the request must have been sent */
        GError *error =
            g_error_new_literal(memcached_client_quark(), 0,
                                "memcached server sends response too early");
        memcached_connection_abort_response_header(client, error);
        return BufferedResult::CLOSED;
    }

    if (client->response.remaining > 0) {
        /* there's a value: pass it to the callback, continue
           reading */
        struct istream *value;
        bool valid;

        client->response.read_state = MemcachedClient::ReadState::VALUE;

        istream_init(&client->response_value, &memcached_response_value,
                     client->pool);
        value = &client->response_value;

        pool_ref(client->pool);

        /* we need this additional reference in case the handler
           closes the body */
        pool_ref(client->caller_pool);

        client->response.in_handler = true;
        client->request.handler->response((memcached_response_status)FromBE16(client->response.header.status),
                                          client->response.extras,
                                          client->response.header.extras_length,
                                          client->response.key.buffer,
                                          FromBE16(client->response.header.key_length),
                                          value, client->request.handler_ctx);
        client->response.in_handler = false;

        pool_unref(client->caller_pool);

        /* check if the callback has closed the value istream */
        valid = memcached_connection_valid(client);

        if (valid && client->socket.IsConnected())
            client->socket.SetDirect(memcached_client_check_direct(client));

        pool_unref(client->pool);

        return valid
            ? BufferedResult::AGAIN_EXPECT
            : BufferedResult::CLOSED;
    } else {
        /* no value: invoke the callback, quit */

        memcached_client_destroy_socket(client, client->socket.IsEmpty());

        client->response.read_state = MemcachedClient::ReadState::END;

        client->request.handler->response((memcached_response_status)FromBE16(client->response.header.status),
                                          client->response.extras,
                                          client->response.header.extras_length,
                                          client->response.key.buffer,
                                          FromBE16(client->response.header.key_length),
                                          nullptr, client->request.handler_ctx);
        pool_unref(client->caller_pool);

        memcached_client_release(client, false);
        return BufferedResult::CLOSED;
    }
}

static BufferedResult
memcached_begin_key(MemcachedClient *client)
{
    assert(client->response.read_state == MemcachedClient::ReadState::EXTRAS);

    client->response.read_state = MemcachedClient::ReadState::KEY;

    client->response.key.remaining =
        FromBE16(client->response.header.key_length);
    if (client->response.key.remaining == 0) {
        client->response.key.buffer = nullptr;
        return memcached_submit_response(client);
    }

    client->response.key.buffer
        = client->response.key.tail
        = (unsigned char *)p_malloc(client->pool,
                                    client->response.key.remaining);

    return BufferedResult::AGAIN_EXPECT;
}

static BufferedResult
memcached_feed_header(MemcachedClient *client,
                      const void *data, size_t length)
{
    assert(client->response.read_state == MemcachedClient::ReadState::HEADER);

    if (length < sizeof(client->response.header))
        /* not enough data yet */
        return BufferedResult::MORE;

    memcpy(&client->response.header, data, sizeof(client->response.header));
    client->socket.Consumed(sizeof(client->response.header));

    client->response.read_state = MemcachedClient::ReadState::EXTRAS;

    client->response.remaining = FromBE32(client->response.header.body_length);
    if (client->response.header.magic != MEMCACHED_MAGIC_RESPONSE ||
        FromBE16(client->response.header.key_length) +
        client->response.header.extras_length > client->response.remaining) {
        /* protocol error: abort the connection */
        GError *error =
            g_error_new_literal(memcached_client_quark(), 0,
                                "memcached protocol error");
        memcached_connection_abort_response_header(client, error);
        return BufferedResult::CLOSED;
    }

    if (client->response.header.extras_length == 0) {
        client->response.extras = nullptr;
        return memcached_begin_key(client);
    }

    return BufferedResult::AGAIN_EXPECT;
}

static BufferedResult
memcached_feed_extras(MemcachedClient *client,
                      const void *data, size_t length)
{
    assert(client->response.read_state == MemcachedClient::ReadState::EXTRAS);
    assert(client->response.header.extras_length > 0);

    if (data == nullptr ||
        length < sizeof(client->response.header.extras_length))
        return BufferedResult::MORE;

    client->response.extras = (unsigned char *)
        p_malloc(client->pool,
                 client->response.header.extras_length);
    memcpy(client->response.extras, data,
           client->response.header.extras_length);

    client->socket.Consumed(client->response.header.extras_length);
    client->response.remaining -= client->response.header.extras_length;

    return memcached_begin_key(client);
}

static BufferedResult
memcached_feed_key(MemcachedClient *client,
                   const void *data, size_t length)
{
    assert(client->response.read_state == MemcachedClient::ReadState::KEY);
    assert(client->response.key.remaining > 0);

    if (length > client->response.key.remaining)
        length = client->response.key.remaining;

    memcpy(client->response.key.tail, data, length);
    client->response.key.tail += length;
    client->response.key.remaining -= length;
    client->response.remaining -=
        FromBE16(client->response.header.key_length);

    client->socket.Consumed(length);

    if (client->response.key.remaining == 0)
        return memcached_submit_response(client);

    return BufferedResult::MORE;
}

static BufferedResult
memcached_feed_value(MemcachedClient *client,
                     const void *data, size_t length)
{
    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->response.remaining > 0);

    if (client->socket.IsConnected() &&
        length >= client->response.remaining)
        memcached_client_release_socket(client,
                                        length == client->response.remaining);

    if (length > client->response.remaining)
        length = client->response.remaining;

    size_t nbytes = istream_invoke_data(&client->response_value, data, length);
    if (nbytes == 0)
        return memcached_connection_valid(client)
            ? BufferedResult::BLOCKING
            : BufferedResult::CLOSED;

    client->socket.Consumed(nbytes);

    client->response.remaining -= nbytes;
    if (client->response.remaining > 0)
        return nbytes < length
            ? BufferedResult::PARTIAL
            : BufferedResult::MORE;

    assert(!client->socket.IsConnected());
    assert(client->request.istream == nullptr);

    client->response.read_state = MemcachedClient::ReadState::END;
    istream_deinit_eof(&client->response_value);
    pool_unref(client->caller_pool);

    memcached_client_release(client, false);
    return BufferedResult::CLOSED;
}

static BufferedResult
memcached_feed(MemcachedClient *client,
               const void *data, size_t length)
{
    switch (client->response.read_state) {
    case MemcachedClient::ReadState::HEADER:
        return memcached_feed_header(client, data, length);

    case MemcachedClient::ReadState::EXTRAS:
        return memcached_feed_extras(client, data, length);

    case MemcachedClient::ReadState::KEY:
        return memcached_feed_key(client, data, length);

    case MemcachedClient::ReadState::VALUE:
        return memcached_feed_value(client, data, length);

    case MemcachedClient::ReadState::END:
        /* unreachable */
        assert(false);
        return BufferedResult::CLOSED;
    }

    /* unreachable */
    assert(false);
    return BufferedResult::CLOSED;
}

static DirectResult
memcached_client_try_read_direct(MemcachedClient *client,
                                 int fd, FdType type)
{
    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->response.remaining > 0);

    ssize_t nbytes = istream_invoke_direct(&client->response_value, type, fd,
                                           client->response.remaining);
    if (likely(nbytes > 0)) {
        client->response.remaining -= nbytes;

        if (client->response.remaining == 0) {
            memcached_client_destroy_socket(client, true);
            istream_deinit_eof(&client->response_value);
            pool_unref(client->caller_pool);
            pool_unref(client->pool);
            return DirectResult::CLOSED;
        } else
            return DirectResult::OK;
    } else if (unlikely(nbytes == ISTREAM_RESULT_EOF)) {
        return DirectResult::END;
    } else if (nbytes == ISTREAM_RESULT_BLOCKING) {
        return DirectResult::BLOCKING;
    } else if (nbytes == ISTREAM_RESULT_CLOSED) {
        return DirectResult::CLOSED;
    } else if (errno == EAGAIN) {
        return DirectResult::EMPTY;
    } else {
        return DirectResult::ERRNO;
    }
}

/*
 * socket_wrapper handler
 *
 */

/**
 * The libevent callback for sending the request to the socket.
 */
static bool
memcached_client_socket_write(void *ctx)
{
    auto *client = (MemcachedClient *)ctx;
    assert(client->response.read_state != MemcachedClient::ReadState::END);

    const ScopePoolRef ref(*client->pool TRACE_ARGS);

    istream_read(client->request.istream);

    return client->socket.IsValid() && client->socket.IsConnected();
}

static BufferedResult
memcached_client_socket_data(const void *buffer, size_t size, void *ctx)
{
    auto *client = (MemcachedClient *)ctx;
    assert(client->response.read_state != MemcachedClient::ReadState::END);

    const ScopePoolRef ref(*client->pool TRACE_ARGS);
    return memcached_feed(client, buffer, size);
}

static DirectResult
memcached_client_socket_direct(int fd, FdType type, void *ctx)
{
    auto *client = (MemcachedClient *)ctx;
    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);
    assert(client->response.remaining > 0);
    assert(memcached_client_check_direct(client));

    return memcached_client_try_read_direct(client, fd, type);
}

static bool
memcached_client_socket_closed(void *ctx)
{
    auto *client = (MemcachedClient *)ctx;

    /* the rest of the response may already be in the input buffer */
    memcached_client_release_socket(client, false);
    return true;
}

static bool
memcached_client_socket_remaining(gcc_unused size_t remaining, void *ctx)
{
    gcc_unused
    auto *client = (MemcachedClient *)ctx;

    /* only READ_VALUE could have blocked */
    assert(client->response.read_state == MemcachedClient::ReadState::VALUE);

    /* the rest of the response may already be in the input buffer */
    return true;
}

static void
memcached_client_socket_error(GError *error, void *ctx)
{
    auto *client = (MemcachedClient *)ctx;

    g_prefix_error(&error, "memcached connection failed: ");
    memcached_connection_abort_response(client, error);
}

static constexpr BufferedSocketHandler memcached_client_socket_handler = {
    .data = memcached_client_socket_data,
    .direct = memcached_client_socket_direct,
    .closed = memcached_client_socket_closed,
    .remaining = memcached_client_socket_remaining,
    .write = memcached_client_socket_write,
    .error = memcached_client_socket_error,
};

/*
 * istream handler for the request
 *
 */

inline size_t
MemcachedClient::OnData(const void *data, size_t length)
{
    assert(request.istream != nullptr);
    assert(response.read_state == ReadState::HEADER ||
           response.read_state == ReadState::EXTRAS ||
           response.read_state == ReadState::KEY);
    assert(data != nullptr);
    assert(length > 0);

    ssize_t nbytes = socket.Write(data, length);
    if (nbytes < 0) {
        if (nbytes == WRITE_BLOCKING || nbytes == WRITE_DESTROYED)
            return 0;

        GError *error =
            g_error_new(memcached_client_quark(), 0,
                        "write error on memcached connection: %s",
                        strerror(errno));
        memcached_connection_abort_response(this, error);
        return 0;
    }

    memcached_client_schedule_write(this);
    return (size_t)nbytes;
}

inline void
MemcachedClient::OnEof()
{
    assert(request.istream != nullptr);
    assert(response.read_state == ReadState::HEADER ||
           response.read_state == ReadState::EXTRAS ||
           response.read_state == ReadState::KEY);

    request.istream = nullptr;

    socket.UnscheduleWrite();
    socket.Read(true);
}

inline void
MemcachedClient::OnError(GError *error)
{
    assert(request.istream != nullptr);
    assert(response.read_state == ReadState::HEADER ||
           response.read_state == ReadState::EXTRAS ||
           response.read_state == ReadState::KEY);

    request.istream = nullptr;

    memcached_connection_abort_response(this, error);
}

/*
 * async operation
 *
 */

static MemcachedClient *
async_to_memcached_client(struct async_operation *ao)
{
    return &ContainerCast2(*ao, &MemcachedClient::request_async);
}

static void
memcached_client_request_abort(struct async_operation *ao)
{
    MemcachedClient *client
        = async_to_memcached_client(ao);
    struct pool *caller_pool = client->caller_pool;
    struct istream *request_istream = client->request.istream;

    /* async_operation_ref::Abort() can only be used before the
       response was delivered to our callback */
    assert(client->response.read_state == MemcachedClient::ReadState::HEADER ||
           client->response.read_state == MemcachedClient::ReadState::EXTRAS ||
           client->response.read_state == MemcachedClient::ReadState::KEY);

    memcached_client_release(client, false);
    pool_unref(caller_pool);

    if (request_istream != nullptr)
        istream_close_handler(request_istream);
}

static const struct async_operation_class memcached_client_async_operation = {
    .abort = memcached_client_request_abort,
};

/*
 * constructor
 *
 */

void
memcached_client_invoke(struct pool *caller_pool,
                        int fd, FdType fd_type,
                        Lease &lease,
                        enum memcached_opcode opcode,
                        const void *extras, size_t extras_length,
                        const void *key, size_t key_length,
                        struct istream *value,
                        const struct memcached_client_handler *handler,
                        void *handler_ctx,
                        struct async_operation_ref *async_ref)
{
    struct istream *request;

    assert(extras_length <= MEMCACHED_EXTRAS_MAX);
    assert(key_length <= MEMCACHED_KEY_MAX);

    struct pool *pool = pool_new_linear(caller_pool, "memcached_client", 4096);

    request = memcached_request_packet(*pool, opcode, extras, extras_length,
                                       key, key_length, value,
                                       0x1234 /* XXX? */);
    if (request == nullptr) {
        lease.ReleaseLease(true);

        GError *error =
            g_error_new_literal(memcached_client_quark(), 0,
                                "failed to generate memcached request packet");
        handler->error(error, handler_ctx);
        return;
    }

    pool_ref(caller_pool);

    auto client = NewFromPool<MemcachedClient>(*pool);
    client->pool = pool;
    client->caller_pool = caller_pool;

    client->socket.Init(*pool, fd, fd_type,
                        nullptr, &memcached_client_timeout,
                        memcached_client_socket_handler, client);

    p_lease_ref_set(client->lease_ref, lease,
                    *pool, "memcached_client_lease");

    istream_assign_handler(&client->request.istream, request,
                           &MakeIstreamHandler<MemcachedClient>::handler, client,
                           0);

    client->request.handler = handler;
    client->request.handler_ctx = handler_ctx;

    client->request_async.Init(memcached_client_async_operation);
    async_ref->Set(client->request_async);

    client->response.read_state = MemcachedClient::ReadState::HEADER;

    istream_read(client->request.istream);
}
