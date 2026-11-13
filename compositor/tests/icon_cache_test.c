// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The icon cache's budget and eviction rule (icon_cache.h;
// docs/memory-allocation-policy.md, "Aggregate growth"): never more entries
// than the budget, whatever the keys; the least recently used entry goes
// first; an evicted entry can be remembered again; nothing is left allocated
// after the cache is destroyed (the sanitizer build checks that).

#include "icon_cache.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CTest builds with NDEBUG in RelWithDebInfo, so assert() would skip checks.
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

// A size x size image whose every pixel is `value`. From malloc directly:
// this is test code, and the cache frees it with free().
static struct vela_image image_of(int size, uint32_t value)
{
    struct vela_image image = { size, size, malloc((size_t)size * (size_t)size * sizeof(uint32_t)) };
    CHECK(image.pixels);
    for (int i = 0; i < size * size; ++i) {
        image.pixels[i] = value;
    }
    return image;
}

static void insert(struct vela_icon_cache *cache, const char *app_id, int size, uint32_t value)
{
    struct vela_image image = image_of(size, value);
    const struct vela_image *stored = vela_icon_cache_insert(cache, app_id, size, &image);
    // The cache took the pixels over.
    CHECK(!image.pixels && image.width == 0 && image.height == 0);
    CHECK(stored && stored->pixels && stored->width == size && stored->height == size);
}

static void insert_miss(struct vela_icon_cache *cache, const char *app_id, int size)
{
    struct vela_image none = { 0 };
    const struct vela_image *stored = vela_icon_cache_insert(cache, app_id, size, &none);
    CHECK(stored && !stored->pixels);
}

// Whether (app_id, size) is remembered with every pixel equal to `value`.
// Looking counts as a use.
static bool holds(struct vela_icon_cache *cache, const char *app_id, int size, uint32_t value)
{
    const struct vela_image *image = vela_icon_cache_find(cache, app_id, size);
    if (!image || !image->pixels || image->width != size || image->height != size) {
        return false;
    }
    for (int i = 0; i < size * size; ++i) {
        if (image->pixels[i] != value) {
            return false;
        }
    }
    return true;
}

static bool absent(struct vela_icon_cache *cache, const char *app_id, int size)
{
    return vela_icon_cache_find(cache, app_id, size) == NULL;
}

// A client that keeps changing its app_id, on outputs of several scales,
// with icons found and not found: the count never passes the budget.
static void test_budget(int budget)
{
    struct vela_icon_cache *cache = vela_icon_cache_create(budget);
    CHECK(vela_icon_cache_count(cache) == 0);
    static const int sizes[] = { 16, 20, 24, 32 };
    int total = budget * 10 + 7;
    for (int i = 0; i < total; ++i) {
        char app_id[64];
        snprintf(app_id, sizeof(app_id), "org.example.app%d", i / 2);
        int size = sizes[i % 4];
        if (i % 3 == 0) {
            insert_miss(cache, app_id, size);
        } else {
            insert(cache, app_id, size, (uint32_t)i);
        }
        int expected = i + 1 < budget ? i + 1 : budget;
        CHECK(vela_icon_cache_count(cache) == expected);
    }
    // The last `budget` insertions are the ones kept, the one before is not.
    for (int i = total - budget - 1; i < total; ++i) {
        char app_id[64];
        snprintf(app_id, sizeof(app_id), "org.example.app%d", i / 2);
        bool kept = vela_icon_cache_find(cache, app_id, sizes[i % 4]) != NULL;
        CHECK(kept == (i >= total - budget));
    }
    CHECK(vela_icon_cache_count(cache) == budget);
    vela_icon_cache_destroy(cache);
}

// The entry used least recently is the one evicted; finding an entry is a
// use, so a recently found entry survives.
static void test_least_recently_used(void)
{
    struct vela_icon_cache *cache = vela_icon_cache_create(4);
    insert(cache, "a", 16, 1);
    insert(cache, "b", 16, 2);
    insert(cache, "c", 16, 3);
    insert(cache, "d", 16, 4);
    CHECK(vela_icon_cache_count(cache) == 4);
    // "a" is the oldest insertion but now the most recent use.
    CHECK(holds(cache, "a", 16, 1));
    insert(cache, "e", 16, 5);
    CHECK(vela_icon_cache_count(cache) == 4);
    CHECK(absent(cache, "b", 16)); // the least recently used went
    CHECK(holds(cache, "c", 16, 3));
    CHECK(holds(cache, "d", 16, 4));
    CHECK(holds(cache, "a", 16, 1));
    CHECK(holds(cache, "e", 16, 5));
    // Uses, oldest first, are now c, d, a, e: two insertions evict c and d.
    insert(cache, "f", 16, 6);
    CHECK(absent(cache, "c", 16));
    insert_miss(cache, "g", 16);
    CHECK(absent(cache, "d", 16));
    CHECK(holds(cache, "a", 16, 1));
    CHECK(holds(cache, "e", 16, 5));
    CHECK(holds(cache, "f", 16, 6));
    // A remembered miss is an entry like the others: found, empty.
    const struct vela_image *miss = vela_icon_cache_find(cache, "g", 16);
    CHECK(miss && !miss->pixels);
    CHECK(vela_icon_cache_count(cache) == 4);
    vela_icon_cache_destroy(cache);
}

// The same app at another size is another entry, and an evicted entry can be
// remembered again.
static void test_sizes_and_reinsertion(void)
{
    struct vela_icon_cache *cache = vela_icon_cache_create(3);
    insert(cache, "app", 16, 0x10);
    insert(cache, "app", 32, 0x20);
    CHECK(vela_icon_cache_count(cache) == 2);
    CHECK(holds(cache, "app", 32, 0x20));
    CHECK(holds(cache, "app", 16, 0x10));
    CHECK(absent(cache, "app", 24));
    CHECK(absent(cache, "ap", 16));
    CHECK(absent(cache, "", 16));
    // Uses, oldest first: (app, 32), (app, 16).
    insert(cache, "other", 16, 0x30);
    insert(cache, "another", 16, 0x40);
    CHECK(absent(cache, "app", 32));
    CHECK(holds(cache, "app", 16, 0x10));
    // Evicted, then remembered again with what was drawn the second time.
    insert(cache, "app", 32, 0x21);
    CHECK(holds(cache, "app", 32, 0x21));
    CHECK(vela_icon_cache_count(cache) == 3);
    // The same key again replaces the entry: one entry, the new pixels.
    insert(cache, "app", 32, 0x22);
    CHECK(vela_icon_cache_count(cache) == 3);
    CHECK(holds(cache, "app", 32, 0x22));
    insert_miss(cache, "app", 32);
    CHECK(vela_icon_cache_count(cache) == 3);
    const struct vela_image *miss = vela_icon_cache_find(cache, "app", 32);
    CHECK(miss && !miss->pixels);
    vela_icon_cache_destroy(cache);
}

// With a budget of one the table has two buckets, so chains are shared and
// every insertion evicts.
static void test_budget_of_one(void)
{
    struct vela_icon_cache *cache = vela_icon_cache_create(1);
    for (int i = 0; i < 100; ++i) {
        char app_id[32];
        snprintf(app_id, sizeof(app_id), "app%d", i);
        insert(cache, app_id, 16, (uint32_t)i);
        CHECK(vela_icon_cache_count(cache) == 1);
        CHECK(holds(cache, app_id, 16, (uint32_t)i));
        if (i > 0) {
            snprintf(app_id, sizeof(app_id), "app%d", i - 1);
            CHECK(absent(cache, app_id, 16));
        }
    }
    vela_icon_cache_destroy(cache);
    // A budget below one is one.
    cache = vela_icon_cache_create(0);
    insert(cache, "a", 16, 1);
    insert(cache, "b", 16, 2);
    CHECK(vela_icon_cache_count(cache) == 1);
    vela_icon_cache_destroy(cache);
    vela_icon_cache_destroy(NULL);
}

// Entries leave from the middle of their bucket's chain and of the use
// list, in an order unrelated to insertion, and the cache stays consistent
// with a model of it kept here.
static void test_against_a_model(void)
{
    enum { BUDGET = 8, KEYS = 40, STEPS = 20000 };
    struct vela_icon_cache *cache = vela_icon_cache_create(BUDGET);
    // The model: for each key its value and the step of its last use (0: not
    // in the cache).
    uint32_t value[KEYS] = { 0 };
    long used[KEYS] = { 0 };
    int count = 0;
    unsigned state = 12345;
    for (long step = 1; step <= STEPS; ++step) {
        state = state * 1103515245u + 12345u;
        int key = (int)((state >> 16) % KEYS);
        char app_id[32];
        snprintf(app_id, sizeof(app_id), "key%d", key / 2);
        int size = key % 2 ? 16 : 32;
        if (used[key]) {
            CHECK(holds(cache, app_id, size, value[key]));
            used[key] = step;
            continue;
        }
        CHECK(absent(cache, app_id, size));
        if (count == BUDGET) {
            int oldest = -1;
            for (int k = 0; k < KEYS; ++k) {
                if (used[k] && (oldest < 0 || used[k] < used[oldest])) {
                    oldest = k;
                }
            }
            used[oldest] = 0;
            --count;
        }
        value[key] = (uint32_t)step;
        used[key] = step;
        ++count;
        insert(cache, app_id, size, value[key]);
        CHECK(vela_icon_cache_count(cache) == count);
    }
    // Every key is where the model says. Checked without disturbing the
    // order any further: nothing follows.
    for (int key = 0; key < KEYS; ++key) {
        char app_id[32];
        snprintf(app_id, sizeof(app_id), "key%d", key / 2);
        int size = key % 2 ? 16 : 32;
        CHECK(used[key] ? holds(cache, app_id, size, value[key]) : absent(cache, app_id, size));
    }
    vela_icon_cache_destroy(cache);
}

int main(void)
{
    test_budget(1);
    test_budget(5);
    test_budget(VELA_ICON_CACHE_BUDGET);
    test_least_recently_used();
    test_sizes_and_reinsertion();
    test_budget_of_one();
    test_against_a_model();
    puts("icon cache passed");
    return 0;
}
