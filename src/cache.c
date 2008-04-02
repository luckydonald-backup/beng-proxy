/*
 * Generic cache class.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "cache.h"
#include "hashmap.h"

#include <assert.h>
#include <time.h>

struct cache {
    const struct cache_class *class;
    size_t size;
    struct hashmap *items;
};

struct cache *
cache_new(pool_t pool, const struct cache_class *class)
{
    struct cache *cache = p_malloc(pool, sizeof(*cache));

    assert(class != NULL);

    cache->class = class;
    cache->size = 0;
    cache->items = hashmap_new(pool, 1024);

    /* event, auto-expire */

    return cache;
}

void
cache_close(struct cache *cache)
{
    const struct hashmap_pair *pair;

    if (cache->class->destroy != NULL) {
        hashmap_rewind(cache->items);
        while ((pair = hashmap_next(cache->items)) != NULL) {
            struct cache_item *item = pair->value;

            assert(cache->size >= item->size);
            cache->size -= item->size;

            cache->class->destroy(item);
        }

        assert(cache->size == 0);
    }
}

static void
cache_destroy_item(struct cache *cache, struct cache_item *item)
{
    assert(cache->size >= item->size);
    cache->size -= item->size;

    if (cache->class->destroy != NULL)
        cache->class->destroy(item);
}

struct cache_item *
cache_get(struct cache *cache, const char *key)
{
    struct cache_item *item = hashmap_get(cache->items, key);
    if (item == NULL)
        return NULL;

    if (time(NULL) < item->expires &&
        (cache->class->validate == NULL || cache->class->validate(item))) {
        item->last_accessed = time(NULL);
        return item;
    }

    hashmap_remove(cache->items, key);
    cache_destroy_item(cache, item);
    return NULL;
}

void
cache_put(struct cache *cache, const char *key,
          struct cache_item *item)
{
    /* XXX size constraints */
    struct cache_item *old = hashmap_put(cache->items, key, item, 1);

    if (old != NULL)
        cache_destroy_item(cache, old);

    cache->size += item->size;
    item->last_accessed = time(NULL);
}
