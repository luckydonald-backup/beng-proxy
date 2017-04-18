/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_SSL_CTX_HXX
#define BENG_PROXY_SSL_CTX_HXX

#include "Error.hxx"

#include <openssl/ssl.h>

#include <utility>

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#include <memory>
#endif

/**
 * A wrapper for SSL_CTX which takes advantage of OpenSSL's reference
 * counting.
 */
class SslCtx {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    SSL_CTX *ssl_ctx = nullptr;
#else
    struct Deleter {
        void operator()(SSL_CTX *p) {
            SSL_CTX_free(p);
        }
    };

    std::shared_ptr<SSL_CTX> ssl_ctx;
#endif

public:
    SslCtx() = default;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    explicit SslCtx(const SSL_METHOD *meth)
        :ssl_ctx(SSL_CTX_new(meth)) {
        if (ssl_ctx == nullptr)
            throw SslError("SSL_CTX_new() failed");
    }

    SslCtx(const SslCtx &src)
        :ssl_ctx(src.ssl_ctx) {
        SSL_CTX_up_ref(ssl_ctx);
    }

    SslCtx(SslCtx &&src)
        :ssl_ctx(std::exchange(src.ssl_ctx, nullptr)) {}

    ~SslCtx() {
        if (ssl_ctx != nullptr)
            SSL_CTX_free(ssl_ctx);
    }

    SslCtx &operator=(const SslCtx &src) {
        /* this check is important because it protects against
           self-assignment */
        if (ssl_ctx != src.ssl_ctx) {
            if (ssl_ctx != nullptr)
                SSL_CTX_free(ssl_ctx);

            ssl_ctx = src.ssl_ctx;
            if (ssl_ctx != nullptr)
                SSL_CTX_up_ref(ssl_ctx);
        }

        return *this;
    }

    SslCtx &operator=(SslCtx &&src) {
        std::swap(ssl_ctx, src.ssl_ctx);
        return *this;
    }

    operator bool() const {
        return ssl_ctx != nullptr;
    }

    SSL_CTX *get() const {
        return ssl_ctx;
    }
#else
    explicit SslCtx(const SSL_METHOD *meth)
        :ssl_ctx(SSL_CTX_new(meth), Deleter()) {
        if (ssl_ctx == nullptr)
            throw SslError("SSL_CTX_new() failed");
    }

    operator bool() const {
        return (bool)ssl_ctx;
    }

    SSL_CTX *get() const {
        return ssl_ctx.get();
    }
#endif

    SSL_CTX &operator*() const {
        return *ssl_ctx;
    }

    void reset() {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        if (ssl_ctx != nullptr)
            SSL_CTX_free(ssl_ctx);
        ssl_ctx = nullptr;
#else
        ssl_ctx.reset();
#endif
    }
};

#endif