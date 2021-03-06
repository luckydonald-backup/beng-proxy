/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "TestPool.hxx"
#include "GrowingBuffer.hxx"
#include "direct.hxx"
#include "istream_gb.hxx"
#include "istream/istream.hxx"
#include "istream/Handler.hxx"
#include "istream/Pointer.hxx"
#include "istream/UnusedPtr.hxx"
#include "fb_pool.hxx"
#include "util/ConstBuffer.hxx"
#include "util/WritableBuffer.hxx"

#include <gtest/gtest.h>

#include <string.h>
#include <stdio.h>

struct Context final : IstreamHandler {
    PoolPtr pool;
    bool got_data = false, eof = false, abort = false, closed = false;
    IstreamPointer abort_istream;

    template<typename P>
    explicit Context(P &&_pool) noexcept
        :pool(std::forward<P>(_pool)), abort_istream(nullptr) {}

    /* virtual methods from class IstreamHandler */
    size_t OnData(const void *data, size_t length) noexcept override;
    void OnEof() noexcept override;
    void OnError(std::exception_ptr ep) noexcept override;
};

/*
 * istream handler
 *
 */

size_t
Context::OnData(gcc_unused const void *data, size_t length) noexcept
{
    got_data = true;

    if (abort_istream.IsDefined()) {
        closed = true;
        abort_istream.ClearAndClose();
        pool.reset();
        return 0;
    }

    return length;
}

void
Context::OnEof() noexcept
{
    eof = true;

    pool.reset();
}

void
Context::OnError(std::exception_ptr) noexcept
{
    abort = true;

    pool.reset();
}

/*
 * utils
 *
 */

static void
istream_read_expect(Context *ctx, IstreamPointer &istream)
{
    ASSERT_FALSE(ctx->eof);

    ctx->got_data = false;

    istream.Read();
    ASSERT_TRUE(ctx->eof || ctx->got_data);
}

static void
run_istream_ctx(Context *ctx, PoolPtr pool, UnusedIstreamPtr _istream)
{
    gcc_unused off_t a1 = _istream.GetAvailable(false);
    gcc_unused off_t a2 = _istream.GetAvailable(true);

    IstreamPointer istream(std::move(_istream), *ctx);

#ifndef NO_GOT_DATA_ASSERT
    while (!ctx->eof)
        istream_read_expect(ctx, istream);
#else
    for (int i = 0; i < 1000 && !ctx->eof; ++i)
        istream->Read();
#endif

    if (!ctx->eof && !ctx->abort)
        istream.Close();

    if (!ctx->eof) {
        pool_trash(pool);
        pool.reset();
    }

    pool_commit();
}

static void
run_istream(PoolPtr pool, UnusedIstreamPtr istream)
{
    Context ctx(pool);
    run_istream_ctx(&ctx, std::move(pool), std::move(istream));
}

static UnusedIstreamPtr
create_test(struct pool *pool)
{
    GrowingBuffer gb;
    gb.Write("foo");
    return istream_gb_new(*pool, std::move(gb));
}

static UnusedIstreamPtr
create_empty(struct pool *pool)
{
    GrowingBuffer gb;
    return istream_gb_new(*pool, std::move(gb));
}

static bool
Equals(WritableBuffer<void> a, const char *b)
{
    return a.size == strlen(b) && memcmp(a.data, b, a.size) == 0;
}


/*
 * tests
 *
 */

/** normal run */
TEST(GrowingBufferTest, Normal)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;

    auto istream = create_test(pool);
    run_istream(pool.Steal(), std::move(istream));
}

/** empty input */
TEST(GrowingBufferTest, Empty)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;

    auto istream = create_empty(pool);
    run_istream(pool.Steal(), std::move(istream));
}

/** first buffer is too small, empty */
TEST(GrowingBufferTest, FirstEmpty)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;

    GrowingBuffer buffer;

    buffer.Write("0123456789abcdefg");

    ASSERT_EQ(buffer.GetSize(), 17u);
    ASSERT_TRUE(Equals(buffer.Dup(pool), "0123456789abcdefg"));

    GrowingBufferReader reader(std::move(buffer));
    auto x = reader.Read();
    ASSERT_FALSE(x.IsNull());
    ASSERT_EQ(x.size, 17u);

    reader.Consume(x.size);
}

/** test growing_buffer_reader_skip() */
TEST(GrowingBufferTest, Skip)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;
    GrowingBuffer buffer;

    buffer.Write("0123");
    buffer.Write("4567");
    buffer.Write("89ab");
    buffer.Write("cdef");

    ASSERT_EQ(buffer.GetSize(), 16u);
    ASSERT_TRUE(Equals(buffer.Dup(pool), "0123456789abcdef"));

    constexpr size_t buffer_size = FB_SIZE - sizeof(void *) - sizeof(DefaultChunkAllocator) - 2 * sizeof(size_t);

    static char zero[buffer_size * 2];
    buffer.Write(zero, sizeof(zero));
    ASSERT_EQ(buffer.GetSize(), 16 + buffer_size * 2);

    GrowingBufferReader reader(std::move(buffer));
    ASSERT_EQ(reader.Available(), 16 + buffer_size * 2);
    reader.Skip(buffer_size - 2);
    ASSERT_EQ(reader.Available(), 18 + buffer_size);

    auto x = reader.Read();
    ASSERT_FALSE(x.IsNull());
    ASSERT_EQ(x.size, 2u);
    reader.Consume(1);
    ASSERT_EQ(reader.Available(), 17 + buffer_size);

    reader.Skip(5);
    ASSERT_EQ(reader.Available(), 12 + buffer_size);

    x = reader.Read();
    ASSERT_FALSE(x.IsNull());
    ASSERT_EQ(x.size, buffer_size - 4);
    reader.Consume(4);
    ASSERT_EQ(reader.Available(), 8 + buffer_size);

    x = reader.Read();
    ASSERT_FALSE(x.IsNull());
    ASSERT_EQ(x.size, buffer_size - 8);

    reader.Skip(buffer_size);
    ASSERT_EQ(reader.Available(), 8);

    x = reader.Read();
    ASSERT_FALSE(x.IsNull());
    ASSERT_EQ(x.size, 8u);

    reader.Skip(8);
    ASSERT_EQ(reader.Available(), 0);

    x = reader.Read();
    ASSERT_TRUE(x.IsNull());
}

/** test reading the head while appending to the tail */
TEST(GrowingBufferTest, ConcurrentRW)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;

    GrowingBuffer buffer;

    buffer.Write("0123");
    buffer.Write("4567");
    buffer.Write("89ab");

    ASSERT_EQ(buffer.GetSize(), 12u);
    ASSERT_TRUE(Equals(buffer.Dup(pool), "0123456789ab"));

    buffer.Skip(12);
    ASSERT_TRUE(buffer.IsEmpty());
    ASSERT_EQ(buffer.GetSize(), 0u);

    buffer.Write("cdef");

    ASSERT_FALSE(buffer.IsEmpty());
    ASSERT_EQ(buffer.GetSize(), 4u);
    ASSERT_TRUE(Equals(buffer.Dup(pool), "cdef"));

    auto x = buffer.Read();
    ASSERT_FALSE(x.IsNull());
    ASSERT_EQ(x.size, 4u);
}

/** abort without handler */
TEST(GrowingBufferTest, AbortWithoutHandler)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;

    auto istream = create_test(pool);
    istream.Clear();
}

/** abort with handler */
TEST(GrowingBufferTest, AbortWithHandler)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;
    Context ctx(pool.Steal());

    Istream *istream = create_test(ctx.pool).Steal();
    istream->SetHandler(ctx);

    istream->Close();
    ctx.pool.reset();

    ASSERT_FALSE(ctx.abort);
}

/** abort in handler */
TEST(GrowingBufferTest, AbortInHandler)
{
    const ScopeFbPoolInit fb_pool_init;
    TestPool pool;
    Context ctx(pool.Steal());

    ctx.abort_istream.Set(create_test(ctx.pool), ctx);

    while (!ctx.eof && !ctx.abort && !ctx.closed)
        istream_read_expect(&ctx, ctx.abort_istream);

    ASSERT_FALSE(ctx.abort_istream.IsDefined());
    ASSERT_FALSE(ctx.abort);
    ASSERT_TRUE(ctx.closed);
}
