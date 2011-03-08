/*
 * Store a URI along with a list of socket addresses.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_ADDRESS_LIST_H
#define BENG_PROXY_ADDRESS_LIST_H

#include <inline/list.h>

#include <sys/socket.h>
#include <stdbool.h>

struct pool;

struct address_list {
    struct list_head addresses;
};

static inline void
address_list_init(struct address_list *list)
{
    list_init(&list->addresses);
}

void
address_list_copy(struct pool *pool, struct address_list *dest,
                  const struct address_list *src);

void
address_list_add(struct pool *pool, struct address_list *list,
                 const struct sockaddr *address, socklen_t length);

const struct sockaddr *
address_list_first(const struct address_list *list, socklen_t *length_r);

const struct sockaddr *
address_list_next(struct address_list *list, socklen_t *length_r);

static inline bool
address_list_is_empty(const struct address_list *list)
{
    return list_empty(&list->addresses);
}

/**
 * Is there no more than one address?
 */
bool
address_list_is_single(const struct address_list *list);

/**
 * Generates a unique string which identifies this object in a hash
 * table.  This string stored in a statically allocated buffer.
 */
const char *
address_list_key(const struct address_list *list);

#endif
