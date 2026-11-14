// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

#include "icon_cache.h"
#include "alloc/alloc.h"

#include <stdint.h>
#include <string.h>

// One remembered icon. It is in two lists at once: its hash bucket's chain,
// and the list of all entries by last use.
struct entry {
    char *app_id;
    int size;
    uint32_t hash;
    struct vela_image image;
    struct entry *chain; // the next entry in the same bucket
    struct entry *newer; // by last use; NULL: the most recently used
    struct entry *older; // NULL: the least recently used
};

// The budget and the eviction rule are in icon_cache.h, with the reasons for
// the compositor's value.
struct vela_icon_cache {
    int budget;
    int count;
    // A power of two, at least twice the budget, fixed at creation. The hash
    // is not keyed, so a client can choose app_ids that share a bucket; the
    // budget bounds that chain too, and a lookup compares at most `budget`
    // keys, each first by hash and size.
    struct entry **buckets;
    uint32_t mask;
    struct entry *newest;
    struct entry *oldest;
};

// FNV-1a over the app_id's bytes, then the size's.
static uint32_t hash_of(const char *app_id, int size)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *c = (const unsigned char *)app_id; *c; ++c) {
        hash = (hash ^ *c) * 16777619u;
    }
    uint32_t bits = (uint32_t)size;
    for (int i = 0; i < 4; ++i) {
        hash = (hash ^ (bits & 0xffu)) * 16777619u;
        bits >>= 8;
    }
    return hash;
}

struct vela_icon_cache *vela_icon_cache_create(int budget)
{
    struct vela_icon_cache *cache = VELA_NEW(struct vela_icon_cache);
    cache->budget = budget < 1 ? 1 : budget;
    uint32_t buckets = 2;
    while (buckets < (uint32_t)cache->budget * 2u && buckets < (1u << 30)) {
        buckets *= 2;
    }
    cache->buckets = VELA_NEW_N(struct entry *, buckets);
    cache->mask = buckets - 1;
    return cache;
}

static void unlink_use(struct vela_icon_cache *cache, struct entry *entry)
{
    if (entry->newer) {
        entry->newer->older = entry->older;
    } else {
        cache->newest = entry->older;
    }
    if (entry->older) {
        entry->older->newer = entry->newer;
    } else {
        cache->oldest = entry->newer;
    }
    entry->newer = entry->older = NULL;
}

static void link_newest(struct vela_icon_cache *cache, struct entry *entry)
{
    entry->newer = NULL;
    entry->older = cache->newest;
    if (cache->newest) {
        cache->newest->newer = entry;
    } else {
        cache->oldest = entry;
    }
    cache->newest = entry;
}

static void free_entry(struct entry *entry)
{
    free(entry->app_id);
    vela_image_finish(&entry->image);
    free(entry);
}

// Out of the table and of the use list, and freed.
static void remove_entry(struct vela_icon_cache *cache, struct entry *entry)
{
    struct entry **link = &cache->buckets[entry->hash & cache->mask];
    while (*link != entry) {
        link = &(*link)->chain;
    }
    *link = entry->chain;
    unlink_use(cache, entry);
    --cache->count;
    free_entry(entry);
}

static struct entry *find_entry(const struct vela_icon_cache *cache, const char *app_id, int size, uint32_t hash)
{
    for (struct entry *entry = cache->buckets[hash & cache->mask]; entry; entry = entry->chain) {
        if (entry->hash == hash && entry->size == size && strcmp(entry->app_id, app_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

void vela_icon_cache_destroy(struct vela_icon_cache *cache)
{
    if (!cache) {
        return;
    }
    struct entry *entry = cache->newest;
    while (entry) {
        struct entry *older = entry->older;
        free_entry(entry);
        entry = older;
    }
    free(cache->buckets);
    free(cache);
}

const struct vela_image *vela_icon_cache_find(struct vela_icon_cache *cache, const char *app_id, int size)
{
    struct entry *entry = find_entry(cache, app_id, size, hash_of(app_id, size));
    if (!entry) {
        return NULL;
    }
    if (entry != cache->newest) {
        unlink_use(cache, entry);
        link_newest(cache, entry);
    }
    return &entry->image;
}

const struct vela_image *vela_icon_cache_insert(struct vela_icon_cache *cache, const char *app_id, int size,
    struct vela_image *image)
{
    uint32_t hash = hash_of(app_id, size);
    struct entry *same = find_entry(cache, app_id, size, hash);
    if (same) {
        remove_entry(cache, same);
    }
    // The budget is enforced here, before the new entry exists: the count
    // never passes it, not even for the length of this call, so one
    // eviction always makes room.
    if (cache->count >= cache->budget) {
        remove_entry(cache, cache->oldest);
    }
    struct entry *entry = VELA_NEW(struct entry);
    entry->app_id = vela_strdup(app_id);
    entry->size = size;
    entry->hash = hash;
    entry->image = *image;
    *image = (struct vela_image) { 0 };
    struct entry **bucket = &cache->buckets[hash & cache->mask];
    entry->chain = *bucket;
    *bucket = entry;
    link_newest(cache, entry);
    ++cache->count;
    return &entry->image;
}

int vela_icon_cache_count(const struct vela_icon_cache *cache)
{
    return cache->count;
}
