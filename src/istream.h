/*
 * Asynchronous input stream API.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_ISTREAM_H
#define __BENG_ISTREAM_H

#include "pool.h"

#include <sys/types.h>

typedef struct istream *istream_t;

struct istream_handler {
    size_t (*data)(const void *data, size_t length, void *ctx);
    ssize_t (*direct)(int fd, size_t max_length, void *ctx);
    void (*eof)(void *ctx);
    void (*free)(void *ctx);
};

struct istream {
    pool_t pool;
    const struct istream_handler *handler;
    void *handler_ctx;

    void (*read)(istream_t istream);
    void (*direct)(istream_t istream);
    void (*close)(istream_t istream);
};

static inline void
istream_read(istream_t istream)
{
    istream->read(istream);
}

static inline void
istream_direct(istream_t istream)
{
    if (istream->direct == NULL)
        istream->read(istream);
    else
        istream->direct(istream);
}

static inline void
istream_close(istream_t istream)
{
    istream->close(istream);
}

static inline void
istream_free(istream_t *istream_r)
{
    istream_t istream = *istream_r;
    *istream_r = NULL;
    istream_close(istream);
}

static inline size_t
istream_invoke_data(istream_t istream, const void *data, size_t length)
{
    return istream->handler->data(data, length, istream->handler_ctx);
}

static inline ssize_t
istream_invoke_direct(istream_t istream, int fd, size_t max_length)
{
    return istream->handler->direct(fd, max_length, istream->handler_ctx);
}

static inline void
istream_invoke_eof(istream_t istream)
{
    if (istream->handler->eof != NULL)
        istream->handler->eof(istream->handler_ctx);
}

static inline void
istream_invoke_free(istream_t istream)
{
    if (istream->handler != NULL && istream->handler->free != NULL) {
        const struct istream_handler *handler = istream->handler;
        void *handler_ctx = istream->handler_ctx;
        istream->handler = NULL;
        istream->handler_ctx = NULL;
        handler->free(handler_ctx);
    }
}


istream_t attr_malloc
istream_memory_new(pool_t pool, const void *data, size_t length);

istream_t attr_malloc
istream_string_new(pool_t pool, const char *s);

istream_t attr_malloc
istream_file_new(pool_t pool, const char *path);

#endif
