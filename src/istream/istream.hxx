/*
 * Asynchronous input stream API.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef ISTREAM_HXX
#define ISTREAM_HXX

#include "Handler.hxx"
#include "istream_oo.hxx"
#include "pool.h"
#include "FdType.hxx"

#include <inline/compiler.h>

#include <assert.h>

gcc_pure
static inline off_t
istream_available(struct istream *istream, bool partial)
{
    assert(istream != nullptr);

    return Istream::Cast(*istream).GetAvailable(partial);
}

static inline off_t
istream_skip(struct istream *istream, off_t length)
{
    assert(istream != nullptr);

    return Istream::Cast(*istream).Skip(length);
}

static inline void
istream_read(struct istream *istream)
{
    assert(istream != nullptr);

    Istream::Cast(*istream).Read();
}

gcc_pure
static inline int
istream_as_fd(struct istream *istream)
{
    assert(istream != nullptr);

    return Istream::Cast(*istream).AsFd();
}

static inline void
istream_close(struct istream *istream)
{
    assert(istream != nullptr);

    Istream::Cast(*istream).Close();
}

static inline void
istream_free(struct istream **istream_r)
{
    struct istream *istream = *istream_r;
    *istream_r = nullptr;
    istream_close(istream);
}

static inline void
istream_handler_set(struct istream *istream,
                    const struct istream_handler *handler,
                    void *handler_ctx,
                    FdTypeMask handler_direct)
{
    assert(istream != nullptr);
    assert(pool_contains(&istream->pool, istream, sizeof(*istream)));

    Istream::Cast(*istream).SetHandler(*handler, handler_ctx, handler_direct);
}

static inline void
istream_handler_set_direct(struct istream *istream,
                           FdTypeMask handler_direct)
{
    assert(istream != nullptr);

    Istream::Cast(*istream).SetDirect(handler_direct);
}

static inline void
istream_handler_clear(struct istream *istream)
{
    assert(istream != nullptr);

    Istream::Cast(*istream).ClearHandler();
}

/**
 * Close an istream which was never used, i.e. it does not have a
 * handler yet.
 */
static inline void
istream_close_unused(struct istream *istream)
{
    assert(istream != nullptr);
    assert(!istream->destroyed);
    assert(!istream_has_handler(istream));

    istream_close(istream);
}

/**
 * Free an istream which was never used, i.e. it does not have a
 * handler yet.
 */
static inline void
istream_free_unused(struct istream **istream_r)
{
    assert(istream_r != nullptr);
    assert(*istream_r != nullptr);
    assert(!istream_has_handler(*istream_r));

    istream_free(istream_r);
}

#endif
