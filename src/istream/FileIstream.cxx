/*
 * Copyright 2007-2018 Content Management AG
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

#include "FileIstream.hxx"
#include "istream.hxx"
#include "New.hxx"
#include "Result.hxx"
#include "io/Buffered.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "pool/pool.hxx"
#include "fb_pool.hxx"
#include "SliceFifoBuffer.hxx"
#include "system/Error.hxx"
#include "event/TimerEvent.hxx"
#include "util/RuntimeError.hxx"

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/**
 * If EAGAIN occurs (on NFS), we try again after 100ms.  We can't
 * check SocketEvent::READ, because the kernel always indicates VFS files as
 * "readable without blocking".
 */
static constexpr Event::Duration file_retry_timeout = std::chrono::milliseconds(100);

class FileIstream final : public Istream {
    FileDescriptor fd;

    FdType fd_type;

    /**
     * A timer to retry reading after EAGAIN.
     */
    TimerEvent retry_event;

    off_t rest;
    SliceFifoBuffer buffer;
    const char *path;

public:
    FileIstream(struct pool &p, EventLoop &event_loop,
                UniqueFileDescriptor &&_fd, FdType _fd_type, off_t _length,
                const char *_path) noexcept
        :Istream(p),
         fd(_fd.Steal()), fd_type(_fd_type),
         retry_event(event_loop, BIND_THIS_METHOD(EventCallback)),
         rest(_length),
         path(_path) {}

    ~FileIstream() noexcept {
        retry_event.Cancel();
    }

    FileDescriptor GetFileDescriptor() const noexcept {
        assert(fd.IsDefined());
        return fd;
    }

    bool SetRange(off_t start, off_t end) noexcept {
        assert(start >= 0);
        assert(end >= start);
        assert(fd.IsDefined());
        assert(rest >= 0);
        assert(buffer.IsNull());
        assert(end <= rest);

        if (start > 0 && fd.Skip(start) < 0)
            return false;

        rest = end - start;
        return true;
    }

private:
    void CloseHandle() noexcept {
        if (!fd.IsDefined())
            return;

        retry_event.Cancel();

        fd.Close();

        buffer.FreeIfDefined();
    }

    void Abort(std::exception_ptr ep) noexcept {
        CloseHandle();
        DestroyError(ep);
    }

    /**
     * @return the number of bytes still in the buffer
     */
    size_t SubmitBuffer() noexcept {
        return ConsumeFromBuffer(buffer);
    }

    void EofDetected() noexcept {
        assert(fd.IsDefined());

        CloseHandle();
        DestroyEof();
    }

    gcc_pure
    size_t GetMaxRead() const noexcept {
        if (rest != (off_t)-1 && rest < (off_t)INT_MAX)
            return (size_t)rest;
        else
            return INT_MAX;
    }

    void TryData() noexcept;
    void TryDirect() noexcept;

    void TryRead() noexcept {
        if (CheckDirect(fd_type))
            TryDirect();
        else
            TryData();
    }

    void EventCallback() noexcept {
        TryRead();
    }

    /* virtual methods from class Istream */

    off_t _GetAvailable(bool partial) noexcept override;
    off_t _Skip(gcc_unused off_t length) noexcept override;

    void _Read() noexcept override {
        retry_event.Cancel();
        TryRead();
    }

    int _AsFd() noexcept override;
    void _Close() noexcept override {
        CloseHandle();
        Destroy();
    }
};

inline void
FileIstream::TryData() noexcept
{
    size_t buffer_rest = 0;

    if (buffer.IsNull()) {
        if (rest != 0)
            buffer.Allocate(fb_pool_get());
    } else {
        const size_t available = buffer.GetAvailable();
        if (available > 0) {
            buffer_rest = SubmitBuffer();
            if (buffer_rest == available)
                /* not a single byte was consumed: we may have been
                   closed, and we must bail out now */
                return;
        }
    }

    if (rest == 0) {
        if (buffer_rest == 0)
            EofDetected();
        return;
    }

    ssize_t nbytes = read_to_buffer(fd.Get(), buffer, GetMaxRead());
    if (nbytes == 0) {
        if (rest == (off_t)-1) {
            rest = 0;
            if (buffer_rest == 0)
                EofDetected();
        } else {
            Abort(std::make_exception_ptr(FormatRuntimeError("premature end of file in '%s'",
                                                             path)));
        }
        return;
    } else if (nbytes == -1) {
        Abort(std::make_exception_ptr(FormatErrno("Failed to read from '%s'",
                                                  path)));
        return;
    } else if (nbytes > 0 && rest != (off_t)-1) {
        rest -= (off_t)nbytes;
        assert(rest >= 0);
    }

    assert(!buffer.empty());

    buffer_rest = SubmitBuffer();
    if (buffer_rest == 0 && rest == 0)
        EofDetected();
}

inline void
FileIstream::TryDirect() noexcept
{
    /* first consume the rest of the buffer */
    if (SubmitBuffer() > 0)
        return;

    if (rest == 0) {
        EofDetected();
        return;
    }

    ssize_t nbytes = InvokeDirect(fd_type, fd.Get(), GetMaxRead());
    if (nbytes == ISTREAM_RESULT_CLOSED)
        /* this stream was closed during the direct() callback */
        return;

    if (nbytes > 0 || nbytes == ISTREAM_RESULT_BLOCKING) {
        /* -2 means the callback wasn't able to consume any data right
           now */
        if (nbytes > 0 && rest != (off_t)-1) {
            rest -= (off_t)nbytes;
            assert(rest >= 0);
            if (rest == 0)
                EofDetected();
        }
    } else if (nbytes == ISTREAM_RESULT_EOF) {
        if (rest == (off_t)-1) {
            EofDetected();
        } else {
            Abort(std::make_exception_ptr(FormatRuntimeError("premature end of file in '%s'",
                                                             path)));
        }
    } else if (errno == EAGAIN) {
        /* this should only happen for splice(SPLICE_F_NONBLOCK) from
           NFS files - unfortunately we cannot use SocketEvent::READ
           here, so we just install a timer which retries after
           100ms */

        retry_event.Schedule(file_retry_timeout);
    } else {
        /* XXX */
        Abort(std::make_exception_ptr(FormatErrno("Failed to read from '%s'",
                                                  path)));
    }
}

/*
 * istream implementation
 *
 */

off_t
FileIstream::_GetAvailable(bool partial) noexcept
{
    off_t available;
    if (rest != (off_t)-1)
        available = rest;
    else if (!partial)
        return (off_t)-1;
    else
        available = 0;

    available += buffer.GetAvailable();
    return available;
}

off_t
FileIstream::_Skip(off_t length) noexcept
{
    retry_event.Cancel();

    if (rest == (off_t)-1)
        return (off_t)-1;

    if (length == 0)
        return 0;

    const size_t buffer_available = buffer.GetAvailable();
    if (length < off_t(buffer_available)) {
        buffer.Consume(length);
        Consumed(length);
        return length;
    }

    length -= buffer_available;
    buffer.Clear();

    if (length >= rest) {
        /* skip beyond EOF */

        length = rest;
        rest = 0;
    } else {
        /* seek the file descriptor */

        if (fd.Skip(length) < 0)
            return -1;
        rest -= length;
    }

    off_t result = buffer_available + length;
    Consumed(result);
    return result;
}

int
FileIstream::_AsFd() noexcept
{
    int result_fd = fd.Steal();

    Destroy();

    return result_fd;
}

/*
 * constructor and public methods
 *
 */

Istream *
istream_file_fd_new(EventLoop &event_loop, struct pool &pool,
                    const char *path,
                    UniqueFileDescriptor fd, FdType fd_type, off_t length) noexcept
{
    assert(fd.IsDefined());
    assert(length >= -1);

    return NewIstream<FileIstream>(pool, event_loop,
                                   std::move(fd), fd_type,
                                   length, path);
}

Istream *
istream_file_stat_new(EventLoop &event_loop, struct pool &pool,
                      const char *path, struct stat &st)
{
    assert(path != nullptr);

    auto fd = OpenReadOnly(path);

    if (fstat(fd.Get(), &st) < 0)
        throw FormatErrno("Failed to stat %s", path);

    FdType fd_type = FdType::FD_FILE;
    off_t size = st.st_size;

    if (S_ISCHR(st.st_mode)) {
        fd_type = FdType::FD_CHARDEV;
        size = -1;
    }

    return istream_file_fd_new(event_loop, pool, path,
                               std::move(fd), fd_type, size);
}

Istream *
istream_file_new(EventLoop &event_loop, struct pool &pool,
                 const char *path, off_t length)
{
    assert(length >= -1);

    return istream_file_fd_new(event_loop, pool, path,
                               OpenReadOnly(path), FdType::FD_FILE, length);
}

FileDescriptor
istream_file_fd(Istream &istream) noexcept
{
    auto &file = (FileIstream &)istream;
    return file.GetFileDescriptor();
}

bool
istream_file_set_range(Istream &istream, off_t start, off_t end) noexcept
{
    auto &file = (FileIstream &)istream;
    return file.SetRange(start, end);
}
