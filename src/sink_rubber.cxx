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

#include "sink_rubber.hxx"
#include "istream/Sink.hxx"
#include "rubber.hxx"
#include "pool.hxx"
#include "util/Cancellable.hxx"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

class RubberSink final : IstreamSink, Cancellable {
    Rubber &rubber;
    unsigned rubber_id;

    const size_t max_size;
    size_t position = 0;

    RubberSinkHandler &handler;

public:
    RubberSink(Rubber &_rubber, unsigned _rubber_id, size_t _max_size,
               RubberSinkHandler &_handler,
               Istream &_input,
               CancellablePointer &cancel_ptr)
        :IstreamSink(_input, FD_ANY),
         rubber(_rubber), rubber_id(_rubber_id), max_size(_max_size),
         handler(_handler) {
        cancel_ptr = *this;
    }

private:
    void FailTooLarge();
    void InvokeEof();

    /* virtual methods from class Cancellable */
    void Cancel() noexcept override;

    /* virtual methods from class IstreamHandler */
    size_t OnData(const void *data, size_t length) override;
    ssize_t OnDirect(FdType type, int fd, size_t max_length) override;
    void OnEof() noexcept override;
    void OnError(std::exception_ptr ep) noexcept override;
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
    rubber.Remove(rubber_id);

    if (input.IsDefined())
        input.ClearAndClose();

    handler.RubberTooLarge();
}

void
RubberSink::InvokeEof()
{
    if (input.IsDefined())
        input.ClearAndClose();

    if (position == 0) {
        /* the stream was empty; remove the object from the rubber
           allocator */
        rubber.Remove(rubber_id);
        rubber_id = 0;
    } else
        rubber.Shrink(rubber_id, position);

    handler.RubberDone(rubber_id, position);
}

/*
 * istream handler
 *
 */

inline size_t
RubberSink::OnData(const void *data, size_t length)
{
    assert(position <= max_size);

    if (position + length > max_size) {
        /* too large, abort and invoke handler */

        FailTooLarge();
        return 0;
    }

    uint8_t *p = (uint8_t *)rubber.Write(rubber_id);
    memcpy(p + position, data, length);
    position += length;

    return length;
}

inline ssize_t
RubberSink::OnDirect(FdType type, int fd, size_t max_length)
{
    assert(position <= max_size);

    size_t length = max_size - position;
    if (length == 0) {
        /* already full, see what the file descriptor says */

        uint8_t dummy;
        ssize_t nbytes = fd_read(type, fd, &dummy, sizeof(dummy));
        if (nbytes > 0) {
            FailTooLarge();
            return ISTREAM_RESULT_CLOSED;
        }

        if (nbytes == 0) {
            InvokeEof();
            return ISTREAM_RESULT_CLOSED;
        }

        return ISTREAM_RESULT_ERRNO;
    }

    if (length > max_length)
        length = max_length;

    uint8_t *p = (uint8_t *)rubber.Write(rubber_id);
    p += position;

    ssize_t nbytes = fd_read(type, fd, p, length);
    if (nbytes > 0)
        position += (size_t)nbytes;

    return nbytes;
}

void
RubberSink::OnEof() noexcept
{
    assert(input.IsDefined());
    input.Clear();

    InvokeEof();
}

void
RubberSink::OnError(std::exception_ptr ep) noexcept
{
    assert(input.IsDefined());
    input.Clear();

    rubber.Remove(rubber_id);
    handler.RubberError(ep);
}

/*
 * async operation
 *
 */

void
RubberSink::Cancel() noexcept
{
    rubber.Remove(rubber_id);

    if (input.IsDefined())
        input.ClearAndClose();
}

/*
 * constructor
 *
 */

void
sink_rubber_new(struct pool &pool, Istream &input,
                Rubber &rubber, size_t max_size,
                RubberSinkHandler &handler,
                CancellablePointer &cancel_ptr)
{
    const off_t available = input.GetAvailable(true);
    if (available > (off_t)max_size) {
        input.CloseUnused();
        handler.RubberTooLarge();
        return;
    }

    const off_t size = input.GetAvailable(false);
    assert(size == -1 || size >= available);
    assert(size <= (off_t)max_size);
    if (size == 0) {
        input.CloseUnused();
        handler.RubberDone(0, 0);
        return;
    }

    const size_t allocate = size == -1
        ? max_size
        : (size_t)size;

    unsigned rubber_id = rubber.Add(allocate);
    if (rubber_id == 0) {
        input.CloseUnused();
        handler.RubberOutOfMemory();
        return;
    }

    NewFromPool<RubberSink>(pool, rubber, rubber_id, allocate,
                            handler,
                            input, cancel_ptr);
}
