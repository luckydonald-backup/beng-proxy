/*
 * istream implementation which reads a file from a NFS server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Istream.hxx"
#include "Client.hxx"
#include "Handler.hxx"
#include "istream/istream.hxx"
#include "pool.hxx"
#include "GException.hxx"
#include "util/Cast.hxx"
#include "util/ForeignFifoBuffer.hxx"

#include <glib.h>

#include <assert.h>
#include <string.h>

static constexpr size_t NFS_BUFFER_SIZE = 32768;

class NfsIstream final : public Istream, NfsClientReadFileHandler {
    NfsFileHandle *handle;

    /**
     * The offset of the next "pread" call on the NFS server.
     */
    uint64_t offset;

    /**
     * The number of bytes that are remaining on the NFS server, not
     * including the amount of data that is already pending.
     */
    uint64_t remaining;

    /**
     * The number of bytes currently scheduled by nfs_pread_async().
     */
    size_t pending_read = 0;

    /**
     * The number of bytes that shall be discarded from the
     * nfs_pread_async() result.  This is non-zero if istream_skip()
     * has been called while a read call was pending.
     */
    size_t discard_read = 0;

    ForeignFifoBuffer<uint8_t> buffer;

public:
    NfsIstream(struct pool &p, NfsFileHandle &_handle,
               uint64_t start, uint64_t end)
        :Istream(p), handle(&_handle),
         offset(start), remaining(end - start),
         buffer(nullptr) {}

private:
    void ScheduleRead();

    /**
     * Check for end-of-file, and if there's more data to read, schedule
     * another read call.
     *
     * The input buffer must be empty.
     */
    void ScheduleReadOrEof();

    void Feed(const void *data, size_t length);

    void ReadFromBuffer();

    /* virtual methods from class Istream */

    off_t _GetAvailable(gcc_unused bool partial) override {
        return remaining + pending_read - discard_read +
            buffer.GetAvailable();
    }

    off_t _Skip(off_t length) override;

    void _Read() override {
        if (!buffer.IsEmpty())
            ReadFromBuffer();
        else
            ScheduleReadOrEof();
    }

    void Close() {
        nfs_client_close_file(handle);
        Istream::_Close();
    }

    /* virtual methods from class NfsClientReadFileHandler */
    void OnNfsRead(const void *data, size_t length) override;
    void OnNfsReadError(std::exception_ptr ep) override;
};

void
NfsIstream::ScheduleReadOrEof()
{
    assert(buffer.IsEmpty());

    if (pending_read > 0)
        return;

    if (remaining > 0) {
        /* read more */

        ScheduleRead();
    } else {
        /* end of file */

        nfs_client_close_file(handle);
        DestroyEof();
    }
}

inline void
NfsIstream::Feed(const void *data, size_t length)
{
    assert(length > 0);

    if (buffer.IsNull()) {
        const uint64_t total_size = remaining + length;
        const size_t buffer_size = total_size > NFS_BUFFER_SIZE
            ? NFS_BUFFER_SIZE
            : (size_t)total_size;
        buffer.SetBuffer(PoolAlloc<uint8_t>(GetPool(), buffer_size),
                         buffer_size);
    }

    auto w = buffer.Write();
    assert(w.size >= length);

    memcpy(w.data, data, length);
    buffer.Append(length);
}

void
NfsIstream::ReadFromBuffer()
{
    assert(buffer.IsDefined());

    size_t buffer_remaining = ConsumeFromBuffer(buffer);
    if (buffer_remaining == 0 && pending_read == 0)
        ScheduleReadOrEof();
}

/*
 * NfsClientReadFileHandler
 *
 */

void
NfsIstream::OnNfsRead(const void *data, size_t _length)
{
    assert(pending_read > 0);
    assert(discard_read <= pending_read);
    assert(_length <= pending_read);

    if (_length < pending_read) {
        nfs_client_close_file(handle);
        GError *error = g_error_new_literal(g_file_error_quark(), 0,
                                            "premature end of file");
        DestroyError(error);
        return;
    }

    const size_t discard = discard_read;
    const size_t length = pending_read - discard;
    pending_read = 0;
    discard_read = 0;

    if (length > 0)
        Feed((const char *)data + discard, length);
    ReadFromBuffer();
}

void
NfsIstream::OnNfsReadError(std::exception_ptr ep)
{
    assert(pending_read > 0);

    nfs_client_close_file(handle);
    DestroyError(ToGError(ep));
}

inline void
NfsIstream::ScheduleRead()
{
    assert(pending_read == 0);

    const size_t max = buffer.IsDefined()
        ? buffer.Write().size
        : NFS_BUFFER_SIZE;
    size_t nbytes = remaining > max
        ? max
        : (size_t)remaining;

    const uint64_t read_offset = offset;

    offset += nbytes;
    remaining -= nbytes;
    pending_read = nbytes;

    nfs_client_read_file(handle, read_offset, nbytes, *this);
}

/*
 * istream implementation
 *
 */

off_t
NfsIstream::_Skip(off_t _length)
{
    assert(discard_read <= pending_read);

    uint64_t length = _length;

    uint64_t result = 0;

    if (buffer.IsDefined()) {
        const uint64_t buffer_available = buffer.GetAvailable();
        const uint64_t consume = length < buffer_available
            ? length
            : buffer_available;
        buffer.Consume(consume);
        result += consume;
        length -= consume;
    }

    const uint64_t pending_available =
        pending_read - discard_read;
    uint64_t consume = length < pending_available
        ? length
        : pending_available;
    discard_read += consume;
    result += consume;
    length -= consume;

    if (length > remaining)
        length = remaining;

    remaining -= length;
    offset += length;
    result += length;

    Consumed(result);
    return result;
}

/*
 * constructor
 *
 */

Istream *
istream_nfs_new(struct pool &pool, NfsFileHandle &handle,
                uint64_t start, uint64_t end)
{
    assert(start <= end);

    return NewIstream<NfsIstream>(pool, handle, start, end);
}