/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Error.hxx"
#include "util/StringFormat.hxx"

extern "C" {
#include <nfsc/libnfs.h>
}

#include <assert.h>
#include <string.h>

static StringBuffer<char, 256>
FormatNfsClientError(struct nfs_context *nfs, const char *msg)
{
    assert(msg != nullptr);

    const char *msg2 = nfs_get_error(nfs);
    return StringFormat<256>("%s: %s", msg, msg2);
}

NfsClientError::NfsClientError(struct nfs_context *nfs, const char *msg)
    :std::runtime_error(FormatNfsClientError(nfs, msg).c_str()),
     code(0) {}

static StringBuffer<char, 256>
FormatNfsClientError(int err, struct nfs_context *nfs, void *data,
                     const char *msg)
{
    assert(msg != nullptr);
    assert(err < 0);

    const char *msg2 = (const char *)data;
    if (data == nullptr || *(const char *)data == 0) {
        msg2 = nfs_get_error(nfs);
        if (msg2 == nullptr)
            msg2 = strerror(-err);
    }

    return StringFormat<256>("%s: %s", msg, msg2);
}

NfsClientError::NfsClientError(int err, struct nfs_context *nfs, void *data,
                               const char *msg)
    :std::runtime_error(FormatNfsClientError(err, nfs, data, msg).c_str()),
     code(-err) {}