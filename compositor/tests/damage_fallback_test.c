// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// The damage fallback of the frame path (docs/memory-allocation-policy.md,
// "Design for (4)") on the CPU: the real, private functions of frame.c,
// with pixman made to fail for real. While `failing` is set, this
// executable's malloc returns NULL, and pixman, whose allocations resolve to
// it, takes its genuine failure path (a broken region, a FALSE result).
// Function sections let the linker discard the rendering paths. Not built
// with ASan, whose allocator this malloc would replace.

#include "../src/scene/frame.c"

#include <errno.h>
#include <stdio.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);

static bool failing;

void *malloc(size_t size)
{
    if (failing) {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size)
{
    if (failing) {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_calloc(count, size);
}

void *realloc(void *pointer, size_t size)
{
    if (failing) {
        errno = ENOMEM;
        return NULL;
    }
    return __libc_realloc(pointer, size);
}

static unsigned scheduled;

void vela_output_schedule_frame(struct vela_output *output)
{
    (void)output;
    ++scheduled;
}

bool vela_texture_is_opaque(struct wlr_texture *texture)
{
    (void)texture;
    return true;
}

// CTest builds with NDEBUG in RelWithDebInfo, so assert() would skip checks.
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAILED: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

// A region pixman has broken: two disjoint rectangles need memory.
static void make_broken(pixman_region32_t *region)
{
    pixman_region32_init_rect(region, 0, 0, 10, 10);
    failing = true;
    pixman_bool_t ok = pixman_region32_union_rect(region, region, 50, 50, 10, 10);
    failing = false;
    CHECK(!ok);
    CHECK(!pixman_region32_selfcheck(region));
}

static bool is_rect(pixman_region32_t *region, int x, int y, int width, int height)
{
    int count = 0;
    const pixman_box32_t *boxes = pixman_region32_rectangles(region, &count);
    return count == 1 && boxes[0].x1 == x && boxes[0].y1 == y && boxes[0].x2 == x + width
        && boxes[0].y2 == y + height;
}

static void add_element(struct vela_elements *list, int x, int y, int width, int height)
{
    struct vela_element *e = push_element(list);
    e->box = (struct wlr_box) { x, y, width, height };
    e->color = (struct wlr_render_color) { 0.2f, 0.3f, 0.4f, 1.0f };
    e->opacity = 1.0f;
}

static struct wlr_output output = { .name = "TEST-1" };

static void frame_init(struct vela_output_frame *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->output = &output;
    wlr_damage_ring_init(&frame->ring);
    pixman_region32_init(&frame->scene_damage);
    frame->region_failures_seen = vela_region_failures;
}

static void frame_finish(struct vela_output_frame *frame)
{
    wlr_damage_ring_finish(&frame->ring);
    pixman_region32_fini(&frame->scene_damage);
}

// Occlusion culling drops what opaque elements cover; when its regions
// fail it must keep everything instead.
static void test_culling(void)
{
    for (int fail = 0; fail < 2; ++fail) {
        struct vela_elements list = { 0 };
        add_element(&list, 60, 10, 10, 10); // below the right one: covered
        add_element(&list, 50, 0, 50, 100);
        add_element(&list, 0, 0, 30, 30); // top: with the right one, two rectangles
        unsigned long before = vela_region_failures;
        failing = fail;
        cull_occluded(&list);
        failing = false;
        CHECK(list.items[2].visible && list.items[1].visible);
        if (fail) {
            CHECK(vela_region_failures > before);
            CHECK(list.items[0].visible); // kept: drawing too much is correct
            CHECK(list.items[0].visible_area == 100);
        } else {
            CHECK(vela_region_failures == before);
            CHECK(!list.items[0].visible); // culled
        }
        free(list.items);
    }
}

static void test_before_drawing(void)
{
    const struct wlr_box whole = { 0, 0, 640, 480 };
    struct vela_output_frame frame;

    // Nothing failed: the damage is left as it is, and no frame is added.
    frame_init(&frame);
    pixman_region32_union_rect(&frame.scene_damage, &frame.scene_damage, 5, 5, 10, 10);
    unsigned before = scheduled;
    CHECK(check_damage_before_drawing(&frame, &whole) == vela_region_failures);
    CHECK(!frame.damage_unknown && is_rect(&frame.scene_damage, 5, 5, 10, 10));
    CHECK(scheduled == before);
    frame_finish(&frame);

    // A failure since the last frame, even one that broke no region of ours.
    // It may have hit the elements just built (a blur region): one more frame
    // builds them again, and only one.
    frame_init(&frame);
    CHECK(!vela_region_ok(0));
    before = scheduled;
    unsigned long checked = check_damage_before_drawing(&frame, &whole);
    CHECK(frame.damage_unknown && is_rect(&frame.scene_damage, 0, 0, 640, 480));
    CHECK(scheduled == before + 1);
    check_damage_after_drawing(&frame, checked); // drawn whole
    check_damage_before_drawing(&frame, &whole);
    CHECK(scheduled == before + 1);
    frame_finish(&frame);

    // A broken scene damage (wlroots' calls do not count failures).
    frame_init(&frame);
    pixman_region32_fini(&frame.scene_damage);
    make_broken(&frame.scene_damage);
    check_damage_before_drawing(&frame, &whole);
    CHECK(frame.damage_unknown && is_rect(&frame.scene_damage, 0, 0, 640, 480));
    frame_finish(&frame);

    // A broken ring.
    frame_init(&frame);
    pixman_region32_fini(&frame.ring.current);
    make_broken(&frame.ring.current);
    check_damage_before_drawing(&frame, &whole);
    CHECK(frame.damage_unknown && is_rect(&frame.scene_damage, 0, 0, 640, 480));
    CHECK(pixman_region32_selfcheck(&frame.ring.current));
    frame_finish(&frame);

    // The previous frame was not drawn whole.
    frame_init(&frame);
    frame.damage_unknown = true;
    check_damage_before_drawing(&frame, &whole);
    CHECK(is_rect(&frame.scene_damage, 0, 0, 640, 480));
    frame_finish(&frame);
}

static void test_after_ring(void)
{
    struct vela_output_frame frame;
    pixman_region32_t frame_damage, buffer_damage;

    // Nothing failed.
    frame_init(&frame);
    pixman_region32_init_rect(&frame_damage, 1, 1, 2, 2);
    pixman_region32_init_rect(&buffer_damage, 3, 3, 4, 4);
    unsigned long checked = vela_region_failures;
    CHECK(check_damage_after_ring(&frame, checked, &frame_damage, &buffer_damage, 640, 480) == checked);
    CHECK(is_rect(&frame_damage, 1, 1, 2, 2) && is_rect(&buffer_damage, 3, 3, 4, 4));
    pixman_region32_fini(&frame_damage);
    pixman_region32_fini(&buffer_damage);
    frame_finish(&frame);

    // A failure counted since the previous check, or a region wlroots broke.
    for (int which = 0; which < 3; ++which) {
        frame_init(&frame);
        pixman_region32_init_rect(&frame_damage, 1, 1, 2, 2);
        pixman_region32_init_rect(&buffer_damage, 3, 3, 4, 4);
        checked = vela_region_failures;
        if (which == 0) {
            CHECK(!vela_region_ok(0));
        } else {
            pixman_region32_t *broken = which == 1 ? &frame_damage : &buffer_damage;
            pixman_region32_fini(broken);
            make_broken(broken);
        }
        check_damage_after_ring(&frame, checked, &frame_damage, &buffer_damage, 640, 480);
        CHECK(is_rect(&frame_damage, 0, 0, 640, 480) && is_rect(&buffer_damage, 0, 0, 640, 480));
        pixman_region32_fini(&frame_damage);
        pixman_region32_fini(&buffer_damage);
        frame_finish(&frame);
    }
}

static void test_after_drawing(void)
{
    struct vela_output_frame frame;

    // Drawn whole: safe, nothing more to schedule.
    frame_init(&frame);
    unsigned before = scheduled;
    check_damage_after_drawing(&frame, vela_region_failures);
    CHECK(vela_output_frame_safe(&frame) && scheduled == before);
    frame_finish(&frame);

    // A clip region failed while drawing: not safe, and the redraw is
    // scheduled. The next frame's first check then redraws whole.
    frame_init(&frame);
    unsigned long checked = vela_region_failures;
    CHECK(!vela_region_ok(0));
    check_damage_after_drawing(&frame, checked);
    CHECK(!vela_output_frame_safe(&frame) && scheduled == before + 1);
    CHECK(frame.region_failures_seen == vela_region_failures);
    const struct wlr_box whole = { 0, 0, 640, 480 };
    check_damage_before_drawing(&frame, &whole);
    CHECK(is_rect(&frame.scene_damage, 0, 0, 640, 480));
    frame_finish(&frame);
}

int main(void)
{
    test_culling();
    test_before_drawing();
    test_after_ring();
    test_after_drawing();
    puts("damage fallback passed");
    return 0;
}
