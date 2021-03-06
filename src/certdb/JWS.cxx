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

#include "JWS.hxx"
#include "ssl/Base64.hxx"

#include "openssl/evp.h"
#include "openssl/rsa.h"

#include <stdexcept>

std::string
MakeJwk(EVP_PKEY &key)
{
    if (EVP_PKEY_base_id(&key) != EVP_PKEY_RSA)
        throw std::runtime_error("RSA key expected");

    const BIGNUM *n, *e;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    RSA_get0_key(EVP_PKEY_get0_RSA(&key), &n, &e, nullptr);
#else
    n = key.pkey.rsa->n;
    e = key.pkey.rsa->e;
#endif

    const auto exponent = UrlSafeBase64(*e);
    const auto modulus = UrlSafeBase64(*n);
    std::string jwk("{\"e\":\"");
    jwk += exponent.c_str();
    jwk += "\",\"kty\":\"RSA\",\"n\":\"";
    jwk += modulus.c_str();
    jwk += "\"}";
    return jwk;
}
