/*
 * A wrapper that turns a growing_buffer into an istream.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_gb.hxx"
#include "istream_oo.hxx"
#include "growing_buffer.hxx"
#include "util/ConstBuffer.hxx"

class GrowingBufferIstream final : public Istream {
    GrowingBufferReader reader;

public:
    GrowingBufferIstream(struct pool &p, const GrowingBuffer &_gb)
        :Istream(p), reader(_gb) {}

    /* virtual methods from class Istream */

    off_t GetAvailable(gcc_unused bool partial) override {
        return reader.Available();
    }

    void Read() override {
        /* this loop is required to cross the buffer borders */
        while (true) {
            auto src = reader.Read();
            if (src.IsNull()) {
                assert(reader.IsEOF());
                DestroyEof();
                return;
            }

            assert(!reader.IsEOF());

            size_t nbytes = InvokeData(src.data, src.size);
            if (nbytes == 0)
                /* growing_buffer has been closed */
                return;

            reader.Consume(nbytes);
            if (nbytes < src.size)
                return;
        }
    }
};

struct istream *
istream_gb_new(struct pool *pool, const GrowingBuffer *gb)
{
    assert(gb != nullptr);

    return NewIstream<GrowingBufferIstream>(*pool, *gb);
}
