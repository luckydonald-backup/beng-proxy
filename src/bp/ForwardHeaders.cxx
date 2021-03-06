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

#include "ForwardHeaders.hxx"
#include "http/Upgrade.hxx"
#include "strmap.hxx"
#include "session/Session.hxx"
#include "pool/pool.hxx"
#include "product.h"
#include "http/HeaderName.hxx"
#include "http/CookieClient.hxx"
#include "http/CookieServer.hxx"
#include "util/CharUtil.hxx"
#include "util/StringCompare.hxx"

#ifndef NDEBUG
#include "io/Logger.hxx"
#endif

#include <string.h>

using namespace BengProxy;

gcc_pure
static bool
IsIfCacheHeader(const char *name) noexcept
{
    const char *i = StringAfterPrefix(name, "if-");
    return i != nullptr &&
        (StringIsEqual(i, "modified-since") ||
         StringIsEqual(i, "unmodified-since") ||
         StringIsEqual(i, "match") ||
         StringIsEqual(i, "none-match") ||
         StringIsEqual(i, "range"));
}

/**
 * @return HeaderGroup::ALL for headers which must be copied
 * unconditionally; HeaderGroup::MAX for headers with special handling
 * (to be forwarded/handled by special code); or one of the "real"
 * HeaderGroup values for the given group
 */
gcc_pure
static HeaderGroup
ClassifyRequestHeader(const char *name, const bool with_body,
                      const bool is_upgrade) noexcept
{
    switch (*name) {
    case 'a':
        if (auto accept = StringAfterPrefix(name, "accept")) {
            if (StringIsEmpty(accept))
                /* "basic" */
                return HeaderGroup::ALL;

            if (StringIsEqual(accept, "-language") ||
                StringIsEqual(accept, "-charset") ||
                StringIsEqual(accept, "-encoding"))
                /* special handling */
                return HeaderGroup::MAX;
        }

        if (auto acr = StringAfterPrefix(name, "access-control-request-")) {
            if (StringIsEqual(acr, "method") || StringIsEqual(acr, "headers"))
                /* see http://www.w3.org/TR/cors/#syntax */
                return HeaderGroup::CORS;
        }

        if (StringIsEqual(name, "authorization"))
            return HeaderGroup::AUTH;

        break;

    case 'c':
        if (StringIsEqual(name, "cache-control"))
            /* "basic" */
            return HeaderGroup::ALL;

        if (StringIsEqual(name, "cookie") || StringIsEqual(name, "cookie2"))
            return HeaderGroup::COOKIE;

        if (auto content = StringAfterPrefix(name, "content-")) {
            if (StringIsEqual(content, "encoding") ||
                StringIsEqual(content, "language") ||
                StringIsEqual(content, "md5") ||
                StringIsEqual(content, "range") ||
                StringIsEqual(content, "type") ||
                StringIsEqual(content, "disposition"))
                /* "body" */
                return with_body ? HeaderGroup::ALL : HeaderGroup::MAX;
        }

        break;

    case 'f':
        if (StringIsEqual(name, "from"))
            /* "basic" */
            return HeaderGroup::ALL;

        break;

    case 'i':
        if (IsIfCacheHeader(name))
            /* "cache" */
            return HeaderGroup::MAX;

        break;

    case 'h':
        if (StringIsEqual(name, "host"))
            return HeaderGroup::MAX;
        break;

    case 'o':
        if (StringIsEqual(name, "origin"))
            /* see http://www.w3.org/TR/cors/#syntax */
            return is_upgrade
                /* always forward for "Upgrade" requests */
                ? HeaderGroup::ALL
                /* only forward if CORS forwarding is enabled */
                : HeaderGroup::CORS;
        break;

    case 'r':
        if (StringIsEqual(name, "referer"))
            return HeaderGroup::LINK;
        if (StringIsEqual(name, "range"))
                /* special handling */
            return HeaderGroup::MAX;
        break;

    case 's':
        if (is_upgrade && StringStartsWith(name, "sec-websocket-"))
            /* "upgrade" */
            return HeaderGroup::ALL;
        break;

    case 'u':
        if (is_upgrade && StringIsEqual(name, "upgrade"))
            /* "upgrade" */
            return HeaderGroup::ALL;

        if (StringIsEqual(name, "user-agent"))
            return HeaderGroup::MAX;
        break;

    case 'v':
        if (StringIsEqual(name, "via"))
            /* TODO: use HeaderGroup::IDENTITY */
            return HeaderGroup::MAX;
        break;

    case 'x':
        if (auto c4 = StringAfterPrefix(name, "x-cm4all-")) {
            if (auto b = StringAfterPrefix(c4, "beng-")) {
                return StringIsEqual(b, "peer-subject") ||
                    StringIsEqual(b, "peer-issuer-subject")
                    ? HeaderGroup::SSL
                    : HeaderGroup::SECURE;
            } else if (StringIsEqual(c4, "https"))
                return HeaderGroup::SSL;
            else if (StringIsEqual(c4, "docroot"))
                /* this header is used by apache-lhttpd to set the
                   per-request DocumentRoot, and should never be forwarded
                   from the outside to apache-lhttpd */
                return HeaderGroup::MAX;
        } else if (StringIsEqual(name, "x-forwarded-for"))
            /* TODO: use HeaderGroup::IDENTITY */
            return HeaderGroup::MAX;

        break;
    }

    if (http_header_is_hop_by_hop(name))
        return HeaderGroup::MAX;

    return HeaderGroup::OTHER;
}

static void
forward_user_agent(StringMap &dest, const StringMap &src,
                   bool mangle) noexcept
{
    const char *p;

    p = !mangle
        ? src.Get("user-agent")
        : nullptr;
    if (p == nullptr)
        p = PRODUCT_TOKEN;

    dest.Add("user-agent", p);
}

static void
forward_via(struct pool &pool, StringMap &dest, const StringMap &src,
            const char *local_host, bool mangle) noexcept
{
    const char *p = src.Get("via");
    if (p == nullptr) {
        if (local_host != nullptr && mangle)
            dest.Add("via", p_strcat(&pool, "1.1 ", local_host, nullptr));
    } else {
        if (local_host == nullptr || !mangle)
            dest.Add("via", p);
        else
            dest.Add("via", p_strcat(&pool, p, ", 1.1 ", local_host, nullptr));
    }
}

static void
forward_xff(struct pool &pool, StringMap &dest, const StringMap &src,
            const char *remote_host, bool mangle) noexcept
{
    const char *p;

    p = src.Get("x-forwarded-for");
    if (p == nullptr) {
        if (remote_host != nullptr && mangle)
            dest.Add("x-forwarded-for", remote_host);
    } else {
        if (remote_host == nullptr || !mangle)
            dest.Add("x-forwarded-for", p);
        else
            dest.Add("x-forwarded-for",
                     p_strcat(&pool, p, ", ", remote_host, nullptr));
    }
}

static void
forward_identity(struct pool &pool,
                 StringMap &dest, const StringMap &src,
                 const char *local_host, const char *remote_host,
                 bool mangle) noexcept
{
    forward_via(pool, dest, src, local_host, mangle);
    forward_xff(pool, dest, src, remote_host, mangle);
}

gcc_pure
static bool
compare_set_cookie_name(const char *set_cookie, const char *name) noexcept
{
    auto suffix = StringAfterPrefix(set_cookie, name);
    return suffix != nullptr && !IsAlphaNumericASCII(*suffix);
}

StringMap
forward_request_headers(struct pool &pool, const StringMap &src,
                        const char *local_host, const char *remote_host,
                        bool exclude_host,
                        bool with_body, bool forward_charset,
                        bool forward_encoding,
                        bool forward_range,
                        const HeaderForwardSettings &settings,
                        const char *session_cookie,
                        const RealmSession *session,
                        const char *host_and_port, const char *uri) noexcept
{
#ifndef NDEBUG
    if (session != nullptr && CheckLogLevel(10)) {
        LogFormat(10, "forward_request_headers",
                  "remote_host='%s' "
                  "host='%s' uri='%s' session=%s user='%s' cookie='%s'",
                  remote_host, host_and_port, uri,
                  session->parent.id.Format().c_str(),
                  session->user.c_str(),
                  host_and_port != nullptr && uri != nullptr
                  ? cookie_jar_http_header_value(session->cookies,
                                                 host_and_port, uri, pool)
                  : nullptr);
    }
#endif

    const bool is_upgrade = with_body && http_is_upgrade(src);

    StringMap dest(pool);
    bool found_accept_charset = false;

    for (const auto &i : src) {
        const char *const key = i.key;
        const char *value = i.value;

        const auto group = ClassifyRequestHeader(key, with_body, is_upgrade);
        if (group == HeaderGroup::ALL) {
            dest.Add(key, value);
            continue;
        } else if (group == HeaderGroup::MAX) {
            if (StringIsEqual(key, "host")) {
                if (!exclude_host)
                    dest.Add(key, value);
                if (settings[HeaderGroup::FORWARD] == HeaderForwardMode::MANGLE)
                    dest.Add("x-forwarded-host", value);
            } else if (forward_charset &&
                       StringIsEqual(key, "accept-charset")) {
                dest.Add(key, value);
                found_accept_charset = true;
            } else if (forward_encoding &&
                       StringIsEqual(key, "accept-encoding")) {
                dest.Add(key, value);
            } else if ((session == nullptr ||
                        session->parent.language == nullptr) &&
                       StringIsEqual(key, "accept-language")) {
                dest.Add(key, value);
            } else if (forward_range &&
                       (StringIsEqual(key, "range") ||
                        // TODO: separate parameter for cache headers
                        IsIfCacheHeader(key))) {
                dest.Add(key, value);
            }

            continue;
        }

        const auto mode = settings[group];
        switch (mode) {
        case HeaderForwardMode::NO:
            continue;

        case HeaderForwardMode::YES:
            break;

        case HeaderForwardMode::BOTH:
            if (group == HeaderGroup::COOKIE) {
                if (StringIsEqual(key, "cookie2"))
                    break;

                if (StringIsEqual(key, "cookie")) {
                    value = cookie_exclude(i.value, session_cookie, &pool);
                    if (value != nullptr)
                        break;
                }
            }

            continue;

        case HeaderForwardMode::MANGLE:
            continue;
        }

        dest.Add(key, value);
    }

    if (!found_accept_charset)
        dest.Add("accept-charset", "utf-8");

    if (settings[HeaderGroup::COOKIE] == HeaderForwardMode::MANGLE &&
        session != nullptr && host_and_port != nullptr && uri != nullptr)
        cookie_jar_http_header(session->cookies, host_and_port, uri,
                               dest, pool);

    if (session != nullptr && session->parent.language != nullptr)
        dest.Add("accept-language",
                  p_strdup(&pool, session->parent.language));

    if (session != nullptr && session->user != nullptr)
        dest.Add("x-cm4all-beng-user", p_strdup(&pool, session->user));

    if (settings[HeaderGroup::CAPABILITIES] != HeaderForwardMode::NO)
        forward_user_agent(dest, src,
                           settings[HeaderGroup::CAPABILITIES] == HeaderForwardMode::MANGLE);

    if (settings[HeaderGroup::IDENTITY] != HeaderForwardMode::NO)
        forward_identity(pool, dest, src, local_host, remote_host,
                         settings[HeaderGroup::IDENTITY] == HeaderForwardMode::MANGLE);

    return dest;
}

/**
 * @return HeaderGroup::ALL for headers which must be copied
 * unconditionally; HeaderGroup::MAX for headers with special handling
 * (to be forwarded/handled by special code); or one of the "real"
 * HeaderGroup values for the given group
 */
gcc_pure
static HeaderGroup
ClassifyResponseHeader(const char *name, const bool is_upgrade) noexcept
{
    switch (*name) {
    case 'a':
        if (StringIsEqual(name, "accept-ranges") ||
            StringIsEqual(name, "age") ||
            StringIsEqual(name, "allow"))
            /* "basic" */
            return HeaderGroup::ALL;

        if (auto acr = StringAfterPrefix(name, "access-control-")) {
            if (StringIsEqual(acr, "allow-origin") ||
                StringIsEqual(acr, "allow-credentials") ||
                StringIsEqual(acr, "expose-headers") ||
                StringIsEqual(acr, "max-age") ||
                StringIsEqual(acr, "allow-methods") ||
                StringIsEqual(acr, "allow-headers"))
                /* see http://www.w3.org/TR/cors/#syntax */
                return HeaderGroup::CORS;
        }

        if (StringIsEqual(name, "authentication-info"))
            return HeaderGroup::AUTH;

        break;

    case 'c':
        if (auto content = StringAfterPrefix(name, "content-")) {
            if (StringIsEqual(content, "encoding") ||
                StringIsEqual(content, "language") ||
                StringIsEqual(content, "md5") ||
                StringIsEqual(content, "range") ||
                StringIsEqual(content, "type") ||
                StringIsEqual(content, "disposition"))
                /* "body" */
                return HeaderGroup::ALL;

            if (StringIsEqual(content, "location"))
                /* "link" */
                return HeaderGroup::LINK;
        } else if (StringIsEqual(name, "cache-control"))
            /* "basic" */
            return HeaderGroup::ALL;

        break;

    case 'd':
        if (StringIsEqual(name, "date"))
            /* "exclude" */
            return HeaderGroup::MAX;

        break;

    case 'e':
        if (StringIsEqual(name, "etag") || StringIsEqual(name, "expires"))
            /* "basic" */
            return HeaderGroup::ALL;
        break;

    case 'l':
        if (StringIsEqual(name, "last-modified"))
            /* "basic" */
            return HeaderGroup::ALL;

        if (StringIsEqual(name, "location"))
            /* "link" */
            return HeaderGroup::LINK;

        break;

    case 'r':
        if (StringIsEqual(name, "retry-after"))
            /* "basic" */
            return HeaderGroup::ALL;
        break;

    case 's':
        if (is_upgrade && StringStartsWith(name, "sec-websocket-"))
            /* "upgrade" */
            return HeaderGroup::ALL;

        if (StringIsEqual(name, "server"))
            /* RFC 2616 3.8: Product Tokens */
            return HeaderGroup::CAPABILITIES;

        if (StringIsEqual(name, "set-cookie") ||
            StringIsEqual(name, "set-cookie2"))
            return HeaderGroup::COOKIE;

        break;

    case 'u':
        if (is_upgrade && StringIsEqual(name, "upgrade"))
            /* "upgrade" */
            return HeaderGroup::ALL;
        break;

    case 'v':
        if (StringIsEqual(name, "vary"))
            /* "basic" */
            return HeaderGroup::ALL;

        if (StringIsEqual(name, "via"))
            /* TODO: use HeaderGroup::IDENTITY */
            return HeaderGroup::MAX;

        break;

    case 'w':
        if (StringIsEqual(name, "www-authenticate"))
            return HeaderGroup::AUTH;

        break;

    case 'x':
        if (auto c4 = StringAfterPrefix(name, "x-cm4all-")) {
            if (auto b = StringAfterPrefix(c4, "beng-")) {
                return StringIsEqual(b, "peer-subject") ||
                    StringIsEqual(b, "peer-issuer-subject")
                    /* note: HeaderGroup::SSL doesn't exist for response
                       headers */
                    ? HeaderGroup::OTHER
                    : HeaderGroup::SECURE;
            } else if (StringIsEqual(c4, "https"))
                return HeaderGroup::SSL;
            else if (StringIsEqual(c4, "view"))
                return HeaderGroup::TRANSFORMATION;
        }

        break;
    }

    if (http_header_is_hop_by_hop(name))
        return HeaderGroup::MAX;

    return HeaderGroup::OTHER;
}

StringMap
forward_response_headers(struct pool &pool, http_status_t status,
                         const StringMap &src,
                         const char *local_host,
                         const char *session_cookie,
                         const char *(*relocate)(const char *uri, void *ctx),
                         void *relocate_ctx,
                         const HeaderForwardSettings &settings) noexcept
{
    const bool is_upgrade = http_is_upgrade(status, src);

    StringMap dest(pool);

    for (const auto &i : src) {
        const char *const key = i.key;
        const char *value = i.value;

        const auto group = ClassifyResponseHeader(key, is_upgrade);
        if (group == HeaderGroup::ALL) {
            dest.Add(key, value);
            continue;
        } else if (group == HeaderGroup::MAX) {
            continue;
        }

        const auto mode = settings[group];
        switch (mode) {
        case HeaderForwardMode::NO:
            continue;

        case HeaderForwardMode::YES:
            break;

        case HeaderForwardMode::BOTH:
            if (group == HeaderGroup::COOKIE &&
                (session_cookie == nullptr ||
                 !compare_set_cookie_name(value, session_cookie)))
                /* if this is not our session cookie, forward it */
                break;

            continue;

        case HeaderForwardMode::MANGLE:
            if (relocate != nullptr && group == HeaderGroup::LINK) {
                value = relocate(value, relocate_ctx);
                if (value != nullptr)
                    break;
            }

            continue;
        }

        dest.Add(key, value);
    }

    if (settings[HeaderGroup::IDENTITY] != HeaderForwardMode::NO)
        forward_via(pool, dest, src, local_host,
                    settings[HeaderGroup::IDENTITY] == HeaderForwardMode::MANGLE);

    return dest;
}

void
forward_reveal_user(StringMap &headers,
                    const RealmSession *session) noexcept
{
    headers.SecureSet("x-cm4all-beng-user",
                      session != nullptr && session->user != nullptr
                      ? p_strdup(&headers.GetPool(), session->user)
                      : nullptr);
}
