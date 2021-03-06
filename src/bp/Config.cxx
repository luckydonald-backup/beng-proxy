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

#include "Config.hxx"
#include "avahi/Check.hxx"
#include "net/Parser.hxx"
#include "util/StringView.hxx"
#include "util/StringParser.hxx"

#include <string.h>

void
BpConfig::HandleSet(StringView name, const char *value)
{
    if (name.Equals("max_connections")) {
        max_connections = ParsePositiveLong(value, 1024 * 1024);
    } else if (name.Equals("tcp_stock_limit")) {
        tcp_stock_limit = ParseUnsignedLong(value);
    } else if (name.Equals("fastcgi_stock_limit")) {
        fcgi_stock_limit = ParseUnsignedLong(value);
    } else if (name.Equals("fcgi_stock_max_idle")) {
        fcgi_stock_max_idle = ParseUnsignedLong(value);
    } else if (name.Equals("was_stock_limit")) {
        was_stock_limit = ParseUnsignedLong(value);
    } else if (name.Equals("was_stock_max_idle")) {
        was_stock_max_idle = ParseUnsignedLong(value);
    } else if (name.Equals("http_cache_size")) {
        http_cache_size = ParseSize(value);
        http_cache_size_set = true;
    } else if (name.Equals("filter_cache_size")) {
        filter_cache_size = ParseSize(value);
    } else if (name.Equals("nfs_cache_size")) {
        nfs_cache_size = ParseSize(value);
    } else if (name.Equals("translate_cache_size")) {
        translate_cache_size = ParseUnsignedLong(value);
    } else if (name.Equals("translate_stock_limit")) {
        translate_stock_limit = ParseUnsignedLong(value);
    } else if (name.Equals("stopwatch")) {
        stopwatch = ParseBool(value);
    } else if (name.Equals("dump_widget_tree")) {
        dump_widget_tree = ParseBool(value);
    } else if (name.Equals("verbose_response")) {
        verbose_response = ParseBool(value);
    } else if (name.Equals("session_cookie")) {
        if (*value == 0)
            throw std::runtime_error("Invalid value");

        session_cookie = value;
    } else if (name.Equals("dynamic_session_cookie")) {
        dynamic_session_cookie = ParseBool(value);
    } else if (name.Equals("session_idle_timeout")) {
        session_idle_timeout = ParsePositiveDuration(value);
    } else if (name.Equals("session_save_path")) {
        session_save_path = value;
    } else
        throw std::runtime_error("Unknown variable");
}
