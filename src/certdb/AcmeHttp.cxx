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

#include "AcmeHttp.hxx"
#include "AcmeChallenge.hxx"
#include "JWS.hxx"
#include "ssl/Base64.hxx"
#include "io/FileWriter.hxx"
#include "util/ConstBuffer.hxx"

#include <sys/stat.h>

std::string
MakeHttp01(const AcmeChallenge &challenge, EVP_PKEY &account_key)
{
    return challenge.token + "." +
        UrlSafeBase64SHA256(MakeJwk(account_key)).c_str();
}

static void
CreateFile(const char *path, ConstBuffer<void> contents)
{
    FileWriter file(path);

    /* force the file to be world-readable so our web server can
       deliver it to the ACME server's HTTP client */
    fchmod(file.GetFileDescriptor().Get(), 0644);

    file.Write(contents.data, contents.size);
    file.Commit();
}

static void
CreateFile(const char *path, const std::string &contents)
{
    CreateFile(path, ConstBuffer<void>(contents.data(), contents.length()));
}

std::string
MakeHttp01File(const char *directory, const AcmeChallenge &challenge,
               EVP_PKEY &account_key)
{
    std::string path = std::string(directory) + "/" + challenge.token;
    CreateFile(path.c_str(), MakeHttp01(challenge, account_key));
    return path;
}
