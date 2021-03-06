/*
 * Copyright 2007-2019 Content Management AG
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

#include "IstreamFilterTest.hxx"
#include "FailingResourceLoader.hxx"
#include "istream/istream_string.hxx"
#include "istream/istream.hxx"
#include "pool/pool.hxx"
#include "widget/Widget.hxx"
#include "widget/Class.hxx"
#include "bp/XmlProcessor.hxx"
#include "penv.hxx"
#include "bp/session/Manager.hxx"
#include "bp/session/Session.hxx"
#include "widget/Inline.hxx"
#include "widget/Registry.hxx"
#include "bp/Global.hxx"
#include "crash.hxx"
#include "util/ScopeExit.hxx"

#include <stdlib.h>
#include <stdio.h>

class EventLoop;

const Event::Duration inline_widget_body_timeout = std::chrono::seconds(10);

void
widget_class_lookup(gcc_unused struct pool &pool,
                    gcc_unused struct pool &widget_pool,
                    gcc_unused TranslationService &service,
                    gcc_unused const char *widget_type,
                    WidgetRegistryCallback callback,
                    gcc_unused CancellablePointer &cancel_ptr)
{
    callback(nullptr);
}

UnusedIstreamPtr
embed_inline_widget(struct pool &pool, gcc_unused struct processor_env &env,
                    gcc_unused bool plain_text,
                    Widget &widget) noexcept
{
    return istream_string_new(pool, p_strdup(&pool, widget.class_name));
}

class IstreamProcessorTestTraits {
public:
    static constexpr const char *expected_result =
        "foo &c:url; <script><c:widget id=\"foo\" type=\"bar\"/></script> bar";

    static constexpr bool call_available = true;
    static constexpr bool got_data_assert = true;
    static constexpr bool enable_blocking = true;
    static constexpr bool enable_abort_istream = true;

    UnusedIstreamPtr CreateInput(struct pool &pool) const noexcept {
        return istream_string_new(pool, "foo &c:url; <script><c:widget id=\"foo\" type=\"bar\"/></script> <c:widget id=\"foo\" type=\"bar\"/>");
    }

    UnusedIstreamPtr CreateTest(EventLoop &event_loop, struct pool &pool,
                                UnusedIstreamPtr input) const noexcept {
        /* HACK, processor.c will ignore c:widget otherwise */
        global_translation_service = (TranslationService *)(size_t)1;

        auto *widget = NewFromPool<Widget>(pool, pool, &root_widget_class);

        crash_global_init();
        AtScopeExit() { crash_global_deinit(); };

        session_manager_init(event_loop, std::chrono::minutes(30), 0, 0);
        AtScopeExit() { session_manager_deinit(); };

        auto *session = session_new();

        static struct processor_env env;
        FailingResourceLoader resource_loader;
        env = processor_env(event_loop, resource_loader, resource_loader,
                            nullptr, nullptr,
                            "localhost:8080",
                            "localhost:8080",
                            "/beng.html",
                            "http://localhost:8080/beng.html",
                            "/beng.html",
                            nullptr,
                            "bp_session", session->id, "foo",
                            nullptr);
        session_put(session);

        return processor_process(pool, std::move(input), *widget, env, PROCESSOR_CONTAINER);
    }
};

INSTANTIATE_TYPED_TEST_CASE_P(Processor, IstreamFilterTest,
                              IstreamProcessorTestTraits);
