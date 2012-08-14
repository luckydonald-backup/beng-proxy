/*
 * Remember which servers (socket addresses) failed recently.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "failure.h"
#include "expiry.h"
#include "address-envelope.h"
#include "pool.h"

#include <daemon/log.h>

#include <assert.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct failure {
    struct failure *next;

    time_t expires;

    enum failure_status status;

    struct address_envelope envelope;
};

#define FAILURE_SLOTS 64

struct failure_list {
    struct pool *pool;

    struct failure *slots[FAILURE_SLOTS];
};

static struct failure_list fl;

static inline unsigned
calc_hash(const struct sockaddr *addr, size_t addrlen)
{
    const char *p = (const char*)addr;
    unsigned hash = 5381;

    assert(p != NULL);

    while (addrlen-- > 0)
        hash = (hash << 5) + hash + *p++;

    return hash;
}

void
failure_init(struct pool *pool)
{
    fl.pool = pool_new_libc(pool, "failure_list");
    memset(fl.slots, 0, sizeof(fl.slots));
}

void
failure_deinit(void)
{
    pool_unref(fl.pool);
}

gcc_const
static inline bool
failure_status_can_expire(enum failure_status status)
{
    return status != FAILURE_MONITOR;
}

gcc_pure
static inline bool
failure_is_expired(const struct failure *failure)
{
    assert(failure != NULL);

    return failure_status_can_expire(failure->status) &&
        is_expired(failure->expires);
}

static bool
failure_override_status(struct failure *failure, time_t now,
                        enum failure_status status, unsigned duration)
{
    if (status < failure->status && !failure_is_expired(failure))
        /* don't update if the current status is more serious than the
           new one */
        return false;

    failure->expires = now + duration;
    failure->status = status;
    return true;
}

void
failure_set(const struct sockaddr *addr, size_t addrlen,
            enum failure_status status, unsigned duration)
{
    unsigned slot = calc_hash(addr, addrlen) % FAILURE_SLOTS;
    struct failure *failure;
    struct timespec now;
    int ret;

    assert(addr != NULL);
    assert(addrlen >= sizeof(failure->envelope.address));
    assert(status > FAILURE_OK);

    ret = clock_gettime(CLOCK_MONOTONIC, &now);
    if (ret < 0) {
        daemon_log(1, "clock_gettime(CLOCK_MONOTONIC) failed: %s\n",
                   strerror(errno));
        return;
    }

    for (failure = fl.slots[slot]; failure != NULL; failure = failure->next) {
        if (failure->envelope.length == addrlen &&
            memcmp(&failure->envelope.address, addr, addrlen) == 0) {
            failure_override_status(failure, now.tv_sec, status, duration);
            return;
        }
    }

    /* insert new failure object into the linked list */

    failure = p_malloc(fl.pool, sizeof(*failure)
                       - sizeof(failure->envelope.address) + addrlen);
    failure->expires = now.tv_sec + duration;
    failure->status = status;
    failure->envelope.length = addrlen;
    memcpy(&failure->envelope.address, addr, addrlen);

    failure->next = fl.slots[slot];
    fl.slots[slot] = failure;
}

static bool
match_status(enum failure_status current, enum failure_status match)
{
    /* FAILURE_OK is a catch-all magic value */
    return match == FAILURE_OK || current == match;
}

void
failure_unset(const struct sockaddr *addr, size_t addrlen,
              enum failure_status status)
{
    unsigned slot = calc_hash(addr, addrlen) % FAILURE_SLOTS;
    struct failure **failure_r, *failure;

    assert(addr != NULL);
    assert(addrlen >= sizeof(failure->envelope.address));

    for (failure_r = &fl.slots[slot], failure = *failure_r;
         failure != NULL;
         failure_r = &failure->next, failure = *failure_r) {
        if (failure->envelope.length == addrlen &&
            memcmp(&failure->envelope.address, addr, addrlen) == 0) {
            /* found it: remove it */

            if (!match_status(failure->status, status) &&
                !failure_is_expired(failure))
                /* don't update if the current status is more serious
                   than the one to be removed */
                return;

            *failure_r = failure->next;
            p_free(fl.pool, failure);
            return;
        }
    }
}

gcc_pure
static enum failure_status
failure_get_status2(const struct failure *failure)
{
    return !failure_is_expired(failure)
        ? failure->status
        : FAILURE_OK;
}

enum failure_status
failure_get_status(const struct sockaddr *address, size_t length)
{
    unsigned slot = calc_hash(address, length) % FAILURE_SLOTS;
    struct failure *failure;

    assert(address != NULL);
    assert(length >= sizeof(failure->envelope.address));

    for (failure = fl.slots[slot]; failure != NULL; failure = failure->next)
        if (failure->envelope.length == length &&
            memcmp(&failure->envelope.address, address, length) == 0)
            return failure_get_status2(failure);

    return FAILURE_OK;
}
