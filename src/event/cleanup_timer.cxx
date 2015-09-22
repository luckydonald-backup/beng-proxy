/*
 * Wrapper for event.h which aims to simplify installing recurring
 * events.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "cleanup_timer.hxx"
#include "Callback.hxx"

#include <inline/compiler.h>

#include <stddef.h>

inline void
CleanupTimer::OnTimer()
{
    if (callback(callback_ctx))
        Enable();
}

void
CleanupTimer::Init(unsigned delay_s, bool (*_callback)(void *ctx), void *_ctx)
{
    event.SetTimer(MakeSimpleEventCallback(CleanupTimer, OnTimer), this);

    delay.tv_sec = delay_s;
    delay.tv_usec = 0;

    callback = _callback;
    callback_ctx = _ctx;
}

void
CleanupTimer::Enable()
{
    if (!event.IsTimerPending())
        event.Add(&delay);
}

void
CleanupTimer::Disable()
{
    event.Delete();
}
