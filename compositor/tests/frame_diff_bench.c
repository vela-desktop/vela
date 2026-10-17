// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_diff_driver.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum scenario {
    STABLE, MOVE, ROTATE, REVERSE, CHURN, REPLACE,
    INSERT_FRONT, INSERT_MIDDLE, REMOVE_FRONT, RESTACK_WINDOW, POPUP, ANIMATION_CHURN,
    DUPLICATE_KEYS, SCENARIO_COUNT
};
static const char *names[] = {
    "stable", "move", "rotate", "reverse", "churn-quarter", "replace-all",
    "insert-front", "insert-middle", "remove-front", "restack-window", "popup-open-close", "animation-churn",
    "duplicate-keys"
};

struct fixture {
    struct vela_output_frame frame;
    char *keys;
};

#ifdef VELA_DIFF_ALLOCATION_TEST
static bool track_allocations;
static unsigned allocations, frees;
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void __real_free(void *ptr);
void *__wrap_calloc(size_t count, size_t size)
{
    if (track_allocations) ++allocations;
    return __real_calloc(count, size);
}
void *__wrap_realloc(void *ptr, size_t size)
{
    if (track_allocations) ++allocations;
    return __real_realloc(ptr, size);
}
void __wrap_free(void *ptr)
{
    if (track_allocations && ptr) ++frees;
    __real_free(ptr);
}
#endif

static void init_fixture(struct fixture *f, int count, enum scenario scenario)
{
    memset(f, 0, sizeof(*f));
    // Room for temporary scene nodes, including a popup and animation layers.
    f->keys = calloc((size_t)(count * 2 + 8), 1);
    f->frame.last.items = calloc((size_t)(count + 5), sizeof(struct vela_element));
    f->frame.current.items = calloc((size_t)(count + 5), sizeof(struct vela_element));
    if (!f->keys || !f->frame.last.items || !f->frame.current.items) abort();
    f->frame.last.count = count;
    f->frame.last.capacity = f->frame.current.capacity = count + 5;
    wlr_damage_ring_init(&f->frame.ring);
    pixman_region32_init(&f->frame.scene_damage);
    for (int i = 0; i < count; ++i) {
        struct vela_element e = {
            .key = &f->keys[i], .opacity = 1,
            .box = { (i % 64) * 16, (i / 64) * 16, 16, 16 }, .order = i,
        };
        f->frame.last.items[i] = e;
    }

    // Deliberately non-unique keys for stress-testing the circular match rule.
    if (scenario == DUPLICATE_KEYS) {
        for (int i = 0; i < count; ++i) {
            f->frame.last.items[i].key = &f->keys[i % 8];
        }
    }

    int inserts = scenario == INSERT_FRONT || scenario == INSERT_MIDDLE ? 1
        : scenario == POPUP ? 2 : scenario == ANIMATION_CHURN ? 4 : 0;
    int new_count = count + inserts - (scenario == REMOVE_FRONT && count > 0 ? 1 : 0);
    f->frame.current.count = new_count;
    int insert_at[4] = { -1, -1, -1, -1 };
    if (scenario == INSERT_FRONT) insert_at[0] = 0;
    if (scenario == INSERT_MIDDLE) insert_at[0] = count / 2;
    if (scenario == POPUP) {
        insert_at[0] = count / 2;
        insert_at[1] = count / 2 + 1;
    }
    if (scenario == ANIMATION_CHURN) {
        insert_at[0] = 0;
        insert_at[1] = (count + 4) / 3;
        insert_at[2] = (count + 4) * 2 / 3;
        insert_at[3] = count + 3;
    }

    int inserted = 0;
    int old_cursor = scenario == REMOVE_FRONT && count > 0 ? 1 : 0;
    for (int i = 0; i < new_count; ++i) {
        struct vela_element e;
        if (inserted < inserts && i == insert_at[inserted]) {
            int n = count + inserted;
            e = (struct vela_element) {
                .key = &f->keys[n],
                .opacity = scenario == ANIMATION_CHURN ? 0.5f + 0.1f * inserted : 1.0f,
                .box = { (n % 64) * 16, (n / 64) * 16, 16, 16 },
            };
            ++inserted;
        } else {
            int old = old_cursor++;
            if (scenario == ROTATE && count > 0) old = (i + 1) % count;
            if (scenario == REVERSE || scenario == DUPLICATE_KEYS) old = count - i - 1;
            if (scenario == RESTACK_WINDOW && count > 1) {
                int selected = count / 2;
                old = i == 0 ? selected : i <= selected ? i - 1 : i;
            }
            e = f->frame.last.items[old];
            if (scenario == MOVE) ++e.box.x;
            if (scenario == REPLACE || (scenario == CHURN && i % 4 == 0)) e.key = &f->keys[count + i];
        }
        e.order = i;
        f->frame.current.items[i] = e;
    }
    if (inserted != inserts) abort();
}

static void finish_fixture(struct fixture *f)
{
    wlr_damage_ring_finish(&f->frame.ring);
    pixman_region32_fini(&f->frame.scene_damage);
    free(f->frame.last.items);
    free(f->frame.current.items);
    free(f->frame.found);
    free(f->frame.match_heads);
    free(f->frame.match_next);
    free(f->keys);
}

static void tick(struct fixture *f)
{
    pixman_region32_clear(&f->frame.scene_damage);
    // A compatible reference driver may still accumulate damage in the ring.
    pixman_region32_clear(&f->frame.ring.current);
    vela_test_diff(&f->frame);
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

static bool damage_is(struct fixture *f, int x, int y, unsigned width, unsigned height)
{
    pixman_region32_t expected;
    pixman_region32_init_rect(&expected, x, y, width, height);
    bool equal = pixman_region32_equal(&expected, &f->frame.scene_damage);
    pixman_region32_fini(&expected);
    return equal;
}

// Independent reference for the original O(N^2) circular key search. Synthetic
// fixtures only vary boxes and opacity; every other visual field is identical.
static void reference_add_box(pixman_region32_t *region, const struct wlr_box *box)
{
    pixman_region32_union_rect(region, region, box->x, box->y,
        (unsigned)(box->width > 0 ? box->width : 0),
        (unsigned)(box->height > 0 ? box->height : 0));
}

static bool reference_same_look(const struct vela_element *a, const struct vela_element *b)
{
    return a->box.x == b->box.x && a->box.y == b->box.y
        && a->box.width == b->box.width && a->box.height == b->box.height
        && a->opacity == b->opacity;
}

static void reference_diff_damage(const struct vela_elements *last, const struct vela_elements *current,
    pixman_region32_t *damage)
{
    bool *found = calloc((size_t)last->count + 1, sizeof(*found));
    if (!found) abort();
    int highest_old_order = 0;
    for (int i = 0; i < current->count; ++i) {
        const struct vela_element *e = &current->items[i];
        const struct vela_element *old = NULL;
        for (int k = 0; k < last->count; ++k) {
            int j = (i + k) % last->count;
            if (last->items[j].key == e->key) {
                old = &last->items[j];
                found[j] = true;
                break;
            }
        }
        if (!old) {
            reference_add_box(damage, &e->box);
            continue;
        }
        if (!reference_same_look(e, old)) {
            reference_add_box(damage, &old->box);
            reference_add_box(damage, &e->box);
        } else if (old->order < highest_old_order) {
            reference_add_box(damage, &e->box);
        }
        if (old->order > highest_old_order) highest_old_order = old->order;
    }
    for (int j = 0; j < last->count; ++j) {
        if (!found[j]) reference_add_box(damage, &last->items[j].box);
    }
    free(found);
}

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static int test_diff(void)
{
    struct fixture f;
    init_fixture(&f, 2, STABLE);
    struct vela_element *built = f.frame.current.items;
    tick(&f);
    CHECK(f.frame.last.items == built);
    CHECK(!pixman_region32_not_empty(&f.frame.scene_damage));
    f.frame.current.items[0].opacity = 0.5f;
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 16, 16));
    finish_fixture(&f);

    init_fixture(&f, 1, MOVE);
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 17, 16));
    finish_fixture(&f);

    init_fixture(&f, 2, REVERSE);
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 16, 16));
    CHECK(f.frame.last.items[0].order == 0 && f.frame.last.items[1].order == 1);
    finish_fixture(&f);

    // A reused match bit must not hide a removal on the next frame.
    init_fixture(&f, 8, STABLE);
    tick(&f);
    unsigned forgotten = vela_test_forgotten();
    f.frame.last.items[7].surface = vela_test_surface();
    f.frame.current.count = 7;
    tick(&f);
    CHECK(damage_is(&f, 112, 0, 16, 16));
    CHECK(vela_test_forgotten() == forgotten + 1);
    f.frame.current.count = 7;
    tick(&f);
    CHECK(!pixman_region32_not_empty(&f.frame.scene_damage));
    f.frame.current.count = 0;
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 112, 16));
    f.frame.current.count = 0;
    tick(&f);
    CHECK(!pixman_region32_not_empty(&f.frame.scene_damage));
    f.frame.current.count = 8;
    // Clear the fake surface before reintroducing the element.
    f.frame.current.items[7].surface = NULL;
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 128, 16));
    f.frame.current.count = 8;
    f.frame.current.items[7].surface = NULL;
    tick(&f);
    CHECK(!pixman_region32_not_empty(&f.frame.scene_damage));
    finish_fixture(&f);

    // A front insertion shifts every surviving element's position.
    init_fixture(&f, 3, INSERT_FRONT);
    tick(&f);
    CHECK(f.frame.last.count == 4 && damage_is(&f, 48, 0, 16, 16));
    memcpy(f.frame.current.items, f.frame.last.items, 4 * sizeof(struct vela_element));
    f.frame.current.count = 4;
    tick(&f);
    CHECK(!pixman_region32_not_empty(&f.frame.scene_damage));
    finish_fixture(&f);

    init_fixture(&f, 4, INSERT_MIDDLE);
    tick(&f);
    CHECK(f.frame.last.count == 5 && damage_is(&f, 64, 0, 16, 16));
    finish_fixture(&f);

    init_fixture(&f, 4, REMOVE_FRONT);
    forgotten = vela_test_forgotten();
    f.frame.last.items[0].surface = vela_test_surface();
    tick(&f);
    CHECK(f.frame.last.count == 3 && damage_is(&f, 0, 0, 16, 16));
    CHECK(vela_test_forgotten() == forgotten + 1);
    finish_fixture(&f);

    // A middle window is raised above its predecessors.
    init_fixture(&f, 4, RESTACK_WINDOW);
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 32, 16));
    finish_fixture(&f);

    init_fixture(&f, 4, POPUP);
    tick(&f);
    CHECK(f.frame.last.count == 6 && damage_is(&f, 64, 0, 32, 16));
    // The fixture's original list is now current: close both popup layers.
    tick(&f);
    CHECK(f.frame.last.count == 4 && damage_is(&f, 64, 0, 32, 16));
    finish_fixture(&f);

    init_fixture(&f, 4, ANIMATION_CHURN);
    struct vela_element before_animation[4];
    memcpy(before_animation, f.frame.last.items, sizeof(before_animation));
    tick(&f);
    CHECK(f.frame.last.count == 8 && damage_is(&f, 64, 0, 64, 16));
    memcpy(f.frame.current.items, f.frame.last.items, 8 * sizeof(struct vela_element));
    f.frame.current.count = 8;
    f.frame.current.items[0].opacity -= 0.25f;
    tick(&f);
    CHECK(damage_is(&f, 64, 0, 16, 16));
    // Removing transient layers must restore the actual original scene,
    // not merely truncate the animated list (which still contains layers).
    memcpy(f.frame.current.items, before_animation, sizeof(before_animation));
    f.frame.current.count = 4;
    tick(&f);
    CHECK(damage_is(&f, 64, 0, 64, 16));
    finish_fixture(&f);

    // Match the original circular search even if two old elements share a key.
    init_fixture(&f, 3, STABLE);
    forgotten = vela_test_forgotten();
    f.frame.last.items[2].key = f.frame.last.items[0].key;
    f.frame.last.items[0].surface = vela_test_surface();
    f.frame.current.items[0] = f.frame.last.items[1];
    f.frame.current.items[1] = f.frame.last.items[2];
    f.frame.current.count = 2;
    tick(&f);
    CHECK(damage_is(&f, 0, 0, 16, 16));
    CHECK(vela_test_forgotten() == forgotten + 1);
    finish_fixture(&f);

    // Differential regression against the pre-index circular search. Vary list
    // lengths, duplicate-key multiplicities, permutations and appearance. A
    // fixed seed makes failures reproducible on every CI environment.
    uint32_t random_state = UINT32_C(0x86a5e11a);
    for (int trial = 0; trial < 1000; ++trial) {
        int count = (int)(next_random(&random_state) % 24);
        init_fixture(&f, count, STABLE);
        int distinct_keys = count < 6 ? count : 6;
        for (int j = 0; j < count; ++j) {
            int key_index = (int)(next_random(&random_state) % (unsigned)distinct_keys);
            f.frame.last.items[j].key = &f.keys[key_index];
        }
        int new_count = (int)(next_random(&random_state) % (unsigned)(count + 5));
        f.frame.current.count = new_count;
        for (int i = 0; i < new_count; ++i) {
            struct vela_element e = { 0 };
            if (count > 0 && next_random(&random_state) % 4 != 0) {
                e = f.frame.last.items[next_random(&random_state) % (unsigned)count];
            } else {
                int key_index = count + (int)(next_random(&random_state) % 4);
                e.key = &f.keys[key_index];
                e.box = (struct wlr_box) { 256 + i * 16, 0, 16, 16 };
                e.opacity = 1.0f;
            }
            if (next_random(&random_state) % 5 == 0) e.box.x += 3;
            if (next_random(&random_state) % 7 == 0) e.opacity = 0.75f;
            e.order = i;
            f.frame.current.items[i] = e;
        }
        pixman_region32_t expected;
        pixman_region32_init(&expected);
        reference_diff_damage(&f.frame.last, &f.frame.current, &expected);
        tick(&f);
        bool matches = f.frame.last.count == new_count
            && pixman_region32_equal(&f.frame.scene_damage, &expected);
        pixman_region32_fini(&expected);
        finish_fixture(&f);
        if (!matches) {
            fprintf(stderr, "scene diff mismatch in randomized case %d (old=%d, new=%d)\n",
                trial, count, new_count);
            return 1;
        }
    }

    // Empty scenes also swap safely.
    init_fixture(&f, 0, STABLE);
#ifdef VELA_DIFF_ALLOCATION_TEST
    allocations = frees = 0;
    track_allocations = true;
#endif
    tick(&f);
#ifdef VELA_DIFF_ALLOCATION_TEST
    track_allocations = false;
    CHECK(allocations == 0 && frees == 0);
    CHECK(f.frame.found == NULL && f.frame.found_capacity == 0);
#endif
    CHECK(!pixman_region32_not_empty(&f.frame.scene_damage));
    finish_fixture(&f);

#ifdef VELA_DIFF_ALLOCATION_TEST
    // Growth allocates once; hundreds of comparisons and shrinking scenes
    // must reuse that capacity. Each output owns its own scratch buffer.
    struct fixture other;
    init_fixture(&f, 32, STABLE);
    init_fixture(&other, 8, STABLE);
    allocations = frees = 0;
    track_allocations = true;
    tick(&f);
    tick(&other);
    track_allocations = false;
    CHECK(allocations == 2 && frees == 0);
    CHECK(f.frame.found_capacity >= 32 && other.frame.found_capacity >= 8);
    CHECK(f.frame.found != other.frame.found);
    bool *scratch = f.frame.found;
    allocations = frees = 0;
    track_allocations = true;
    for (int i = 0; i < 240; ++i) {
        tick(&f);
        tick(&other);
    }
    f.frame.current.count = 4;
    tick(&f);
    f.frame.current.count = 4;
    tick(&f);
    track_allocations = false;
    CHECK(allocations == 0 && frees == 0);
    CHECK(f.frame.found == scratch && f.frame.found_capacity >= 32);
    finish_fixture(&f);
    // Grow the actual scene beyond the warmed capacity.
    init_fixture(&f, 32, STABLE);
    f.frame.last.count = f.frame.current.count = 4;
    tick(&f);
    CHECK(f.frame.found_capacity == 8);
    f.frame.current.count = 32;
    tick(&f); // compares against four old elements, then swaps
    allocations = frees = 0;
    track_allocations = true;
    tick(&f);
    track_allocations = false;
    CHECK(allocations == 1 && frees == 0);
    CHECK(f.frame.found_capacity >= 32);
    finish_fixture(&f);
    finish_fixture(&other);
#endif
    // A reordered scene builds its index only on demand and reuses it.
#ifdef VELA_DIFF_ALLOCATION_TEST
    init_fixture(&f, 32, INSERT_FRONT);
    tick(&f);
    tick(&f); // warm both list lengths before tracking steady allocations
    CHECK(f.frame.match_heads && f.frame.match_next);
    int *heads = f.frame.match_heads;
    int *links = f.frame.match_next;
    allocations = frees = 0;
    track_allocations = true;
    for (int i = 0; i < 240; ++i) tick(&f);
    track_allocations = false;
    CHECK(allocations == 0 && frees == 0);
    CHECK(f.frame.match_heads == heads && f.frame.match_next == links);
    finish_fixture(&f);
#endif
    puts("scene diff: damage, reorder, animation, differential matching and scratch reuse passed");
    return 0;
}

static double now_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) abort();
    return (double)time.tv_sec * 1e9 + time.tv_nsec;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double measure(struct fixture *fixtures, int outputs, int ticks)
{
    double start = now_ns();
    for (int i = 0; i < ticks; ++i) {
        for (int j = 0; j < outputs; ++j) tick(&fixtures[j]);
    }
    return now_ns() - start;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--test") == 0) return test_diff();
    if (argc != 4) {
        fprintf(stderr, "usage: %s ELEMENTS OUTPUTS SCENARIO\nscenarios: stable move rotate reverse churn-quarter replace-all insert-front insert-middle remove-front restack-window popup-open-close animation-churn\n", argv[0]);
        return 1;
    }
    char *end;
    long count = strtol(argv[1], &end, 10);
    if (*end || count < 0 || count > 16384) return 1;
    long outputs = strtol(argv[2], &end, 10);
    if (*end || outputs < 1 || outputs > 16) return 1;
    int scenario;
    for (scenario = 0; scenario < SCENARIO_COUNT; ++scenario) if (strcmp(names[scenario], argv[3]) == 0) break;
    if (scenario == SCENARIO_COUNT) return 1;
    struct fixture *fixtures = calloc((size_t)outputs, sizeof(*fixtures));
    if (!fixtures) abort();
    for (int i = 0; i < outputs; ++i) init_fixture(&fixtures[i], (int)count, scenario);
    int ticks = 1;
    // Warm up and choose batches of at least 2 ms to amortize the clock cost.
    while (ticks < 65536 && measure(fixtures, (int)outputs, ticks) < 2e6) ticks *= 2;
    enum { SAMPLES = 21 };
    double samples[SAMPLES];
    for (int i = 0; i < SAMPLES; ++i) samples[i] = measure(fixtures, (int)outputs, ticks) / ticks;
    qsort(samples, SAMPLES, sizeof(*samples), compare_double);
    // Per tick includes all outputs. Percentiles describe batch averages,
    // not individual-frame latency or the GPU/render loop.
    puts("scenario,elements,outputs,ticks_per_sample,samples,median_ns_per_tick,p95_batch_ns_per_tick");
    printf("%s,%ld,%ld,%d,%d,%.1f,%.1f\n", names[scenario], count, outputs, ticks, SAMPLES,
        samples[SAMPLES / 2], samples[(SAMPLES * 95 / 100)]);
    for (int i = 0; i < outputs; ++i) finish_fixture(&fixtures[i]);
    free(fixtures);
    return 0;
}
