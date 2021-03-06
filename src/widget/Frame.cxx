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

#include "Frame.hxx"
#include "Request.hxx"
#include "Error.hxx"
#include "Widget.hxx"
#include "Class.hxx"
#include "Approval.hxx"
#include "Resolver.hxx"
#include "LookupHandler.hxx"
#include "penv.hxx"
#include "HttpResponseHandler.hxx"
#include "istream/istream.hxx"
#include "bp/session/Session.hxx"
#include "util/Exception.hxx"
#include "util/StringFormat.hxx"

#include <assert.h>

void
frame_top_widget(struct pool &pool, Widget &widget,
                 struct processor_env &env,
                 HttpResponseHandler &handler,
                 CancellablePointer &cancel_ptr)
{
    assert(widget.cls != nullptr);
    assert(widget.HasDefaultView());
    assert(widget.from_request.frame);

    if (!widget_check_approval(&widget)) {
        WidgetError error(*widget.parent, WidgetErrorCode::FORBIDDEN,
                          StringFormat<256>("widget '%s' is not allowed to embed widget '%s'",
                                            widget.parent->GetLogName(),
                                            widget.GetLogName()));
        widget.Cancel();
        handler.InvokeError(std::make_exception_ptr(error));
        return;
    }

    try {
        widget.CheckHost(env.untrusted_host, env.site_name);
    } catch (...) {
        WidgetError error(widget, WidgetErrorCode::FORBIDDEN,
                          "Untrusted host");
        widget.Cancel();
        handler.InvokeError(NestException(std::current_exception(), error));
        return;
    }

    if (widget.session_sync_pending) {
        auto session = env.GetRealmSession();
        if (session)
            widget.LoadFromSession(*session);
        else
            widget.session_sync_pending = false;
    }

    widget_http_request(pool, widget, env,
                        handler, cancel_ptr);
}

void
frame_parent_widget(struct pool &pool, Widget &widget, const char *id,
                    struct processor_env &env,
                    WidgetLookupHandler &handler,
                    CancellablePointer &cancel_ptr)
{
    assert(widget.cls != nullptr);
    assert(widget.HasDefaultView());
    assert(!widget.from_request.frame);
    assert(id != nullptr);

    try {
        if (!widget.IsContainer()) {
            /* this widget cannot possibly be the parent of a framed
               widget if it is not a container */

            widget.Cancel();

            throw WidgetError(WidgetErrorCode::NOT_A_CONTAINER,
                              "frame within non-container requested");
        }

        if (!widget_check_approval(&widget)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "widget '%s' is not allowed to embed widget '%s'",
                     widget.parent->GetLogName(),
                     widget.GetLogName());

            widget.Cancel();

            throw WidgetError(WidgetErrorCode::FORBIDDEN, msg);
        }
    } catch (...) {
        handler.WidgetLookupError(std::current_exception());
        return;
    }

    if (widget.session_sync_pending) {
        auto session = env.GetRealmSession();
        if (session)
            widget.LoadFromSession(*session);
        else
            widget.session_sync_pending = false;
    }

    widget_http_lookup(pool, widget, id, env,
                       handler, cancel_ptr);
}
