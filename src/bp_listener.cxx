/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "bp_listener.hxx"
#include "bp_instance.hxx"
#include "bp_connection.hxx"
#include "util/Error.hxx"
#include "net/SocketAddress.hxx"

#include <daemon/log.h>

BPListener::BPListener(BpInstance &_instance, const char *_tag)
    :ServerSocket(_instance.event_loop), instance(_instance), tag(_tag)
{
}

void
BPListener::OnAccept(SocketDescriptor &&_fd, SocketAddress address)
{
    new_connection(instance, std::move(_fd), address, tag);
}

void
BPListener::OnAcceptError(Error &&error)
{
    daemon_log(2, "%s\n", error.GetMessage());
}
