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

#include "ChildErrorLog.hxx"
#include "spawn/Prepared.hxx"
#include "event/net/log/PipeAdapter.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "system/Error.hxx"

#include <assert.h>

ChildErrorLog::ChildErrorLog() = default;
ChildErrorLog::~ChildErrorLog() noexcept = default;
ChildErrorLog &ChildErrorLog::operator=(ChildErrorLog &&) = default;

ChildErrorLog::ChildErrorLog(PreparedChildProcess &p,
                             EventLoop &event_loop, SocketDescriptor socket)
{
    if (socket.IsDefined())
        EnableClient(p, event_loop, socket);
}

UniqueFileDescriptor
ChildErrorLog::EnableClient(EventLoop &event_loop, SocketDescriptor socket)
{
    assert(!adapter);

    if (!socket.IsDefined())
        return UniqueFileDescriptor();

    UniqueFileDescriptor r, w;
    if (!UniqueFileDescriptor::CreatePipe(r, w))
        throw MakeErrno("Failed to create pipe");

    adapter = std::make_unique<Net::Log::PipeAdapter>(event_loop, std::move(r),
                                                      socket);
    return w;
}

void
ChildErrorLog::EnableClient(PreparedChildProcess &p,
                            EventLoop &event_loop, SocketDescriptor socket)
{
    assert(!adapter);

    if (p.stderr_fd >= 0)
        /* already set */
        return;

    auto w = EnableClient(event_loop, socket);
    if (w.IsDefined())
        p.SetStderr(std::move(w));
}

Net::Log::Datagram &
ChildErrorLog::GetDatagram() noexcept
{
    assert(adapter);

    return adapter->GetDatagram();
}

void
ChildErrorLog::SetSite(const char *_site) noexcept
{
    if (!adapter)
        return;

    if (_site == nullptr) {
        if (site.empty())
            return;

        site.clear();
    } else {
        if (site == _site)
            return;

        site = _site;
        _site = site.c_str();
    }

    GetDatagram().site = _site;
}

void
ChildErrorLog::SetUri(const char *_uri) noexcept
{
    if (!adapter)
        return;

    if (_uri == nullptr) {
        if (uri.empty())
            return;

        uri.clear();
    } else {
        if (uri == _uri)
            return;

        uri = _uri;
        _uri = uri.c_str();
    }

    GetDatagram().http_uri = _uri;
}
