// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the production output source without a GPU. Observe emitted damage
// and scheduled output frames, including two asynchronous consumers finishing
// at different times and a final change with no later desktop activity.
#undef NDEBUG
#include "../src/scene/output_capture.c"

static int scheduled;

void wlr_output_update_needs_frame(struct wlr_output *output)
{
    ++scheduled;
}

// Sanitizers may retain the source vtables. These paths need real renderer
// buffers and must never be entered by this protocol scheduling test.
void vela_capture_copy_frame(struct wlr_ext_image_capture_source_v1 *source,
    struct wlr_ext_image_copy_capture_frame_v1 *frame, struct wlr_buffer *buffer,
    struct vela_renderer *renderer, struct wl_event_loop *loop,
    enum wl_output_transform transform, const struct timespec *when) { abort(); }
struct wlr_renderer *vela_renderer_wlr(struct vela_renderer *renderer) { abort(); }

struct observer {
    struct wl_listener frame;
    pixman_region32_t damage;
    int frames;
};

static void observe_frame(struct wl_listener *listener, void *data)
{
    struct observer *observer = wl_container_of(listener, observer, frame);
    struct vela_output_capture_source_frame_event *event = data;
    ++observer->frames;
    pixman_region32_copy(&observer->damage, event->base.damage);
}

static void commit(struct vela_output_capture_source *source, int x, int y, int width, int height)
{
    struct wlr_buffer buffer = { .width = 128, .height = 96 };
    struct wlr_output_state state = { .committed = WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_DAMAGE,
        .buffer = &buffer };
    pixman_region32_init_rect(&state.damage, x, y, width, height);
    struct wlr_output_event_commit event = { .state = &state };
    source_handle_output_commit(&source->output_commit, &event);
    pixman_region32_fini(&state.damage);
}

static void terminate_frame(struct vela_output_capture_source *source)
{
    struct output_capture_frame *frame = calloc(1, sizeof(*frame));
    assert(frame);
    frame->source = source;
    wl_list_init(&frame->destroy.link);
    output_capture_frame_destroy(&frame->destroy, NULL);
}

int main(void)
{
    struct wlr_output output = { 0 };
    struct vela_output_capture_source source = { .output = &output };
    struct observer observer = { .frame.notify = observe_frame };
    wl_signal_init(&source.base.events.frame);
    wl_signal_add(&source.base.events.frame, &observer.frame);
    pixman_region32_init(&source.damage);
    pixman_region32_init(&observer.damage);

    // An idle persistent session does not wake presentation. A new session's
    // initial full damage can still request its first buffer.
    output_source_request_frame(&source.base, false);
    assert(scheduled == 0);
    output_source_request_frame(&source.base, true);
    assert(scheduled == 1);
    commit(&source, 2, 3, 4, 5);
    assert(observer.frames == 1);
    pixman_region32_t expected;
    pixman_region32_init_rect(&expected, 2, 3, 4, 5);
    assert(pixman_region32_equal(&observer.damage, &expected));

    // These changes must not reach wlroots' session damage before both old
    // ready() callbacks have cleared that session damage.
    source.pending = 2;
    commit(&source, 10, 11, 12, 13);
    commit(&source, 20, 21, 22, 23);
    assert(observer.frames == 1);
    output_source_request_frame(&source.base, false);
    assert(scheduled == 1);
    terminate_frame(&source);
    assert(source.pending == 1 && scheduled == 1);
    terminate_frame(&source);
    assert(source.pending == 0 && scheduled == 2);
    // The next output image need not itself have new scene damage: the last
    // change already happened while the old capture was in flight.
    commit(&source, 0, 0, 0, 0);
    pixman_region32_clear(&expected);
    pixman_region32_union_rect(&expected, &expected, 10, 11, 12, 13);
    pixman_region32_union_rect(&expected, &expected, 20, 21, 22, 23);
    assert(observer.frames == 2 && pixman_region32_equal(&observer.damage, &expected));
    assert(!pixman_region32_not_empty(&source.damage));
    output_source_request_frame(&source.base, false);
    assert(scheduled == 2);

    // If no client has requested the next frame yet, keep the final damage
    // until it does, without generating a gratuitous output commit.
    source.pending = 1;
    commit(&source, 1, 2, 3, 4);
    terminate_frame(&source);
    assert(scheduled == 2 && observer.frames == 2);
    output_source_request_frame(&source.base, false);
    assert(scheduled == 3);
    commit(&source, 0, 0, 0, 0);
    pixman_region32_clear(&expected);
    pixman_region32_union_rect(&expected, &expected, 1, 2, 3, 4);
    assert(observer.frames == 3 && pixman_region32_equal(&observer.damage, &expected));

    source.pending = 1;
    source.frame_requested = source.finishing = true;
    terminate_frame(&source);
    assert(source.pending == 0 && scheduled == 3);
    wl_list_remove(&observer.frame.link);
    pixman_region32_fini(&expected);
    pixman_region32_fini(&observer.damage);
    pixman_region32_fini(&source.damage);
    return 0;
}
