# Library results

How each library call that can return a failure is treated in the host C
code ([memory-allocation-policy.md](../../docs/memory-allocation-policy.md),
point 2b). Each call is classified once, here. A new call takes its
family's row or adds one.

- **OOM-only:** wrapped in `VELA_MUST`, which aborts like a failed helper.
  No fallback branch follows it.
- **Other reasons:** the failure is real and is handled where it happens.
  Code that must keep running takes a local path (drop the client, skip the
  feature, fall back) and logs it.
- **Safer fallback:** the failure has a documented fallback.

Checked against wlroots 0.20.2, libwayland 1.26.0 and systemd 262.

## OOM-only (`VELA_MUST`)

| Call | Why |
|---|---|
| `wl_event_loop_add_idle` | Only allocates the source. |
| `wl_resource_create` | Called with an id libwayland has already validated: what is left is the allocation and the map insert. |
| `wl_global_create` | Allocation, or a version outside the interface's range, which would be a programming error (Vela passes constants). |
| wlroots protocol managers: `wlr_compositor_create`, `wlr_subcompositor_create`, `wlr_data_device_manager_create`, `wlr_primary_selection_v1_device_manager_create`, `wlr_data_control_manager_v1_create`, `wlr_ext_data_control_manager_v1_create`, `wlr_viewporter_create`, `wlr_single_pixel_buffer_manager_v1_create`, `wlr_fractional_scale_manager_v1_create`, `wlr_screencopy_manager_v1_create`, `wlr_presentation_create`, `wlr_xdg_output_manager_v1_create`, `wlr_relative_pointer_manager_v1_create`, `wlr_pointer_constraints_v1_create`, `wlr_keyboard_shortcuts_inhibit_v1_create`, `wlr_pointer_gestures_v1_create`, `wlr_seat_create`, `wlr_cursor_shape_manager_v1_create`, `wlr_virtual_pointer_manager_v1_create`, `wlr_virtual_keyboard_manager_v1_create`, `wlr_layer_shell_v1_create`, `wlr_session_lock_manager_v1_create`, `wlr_idle_notifier_v1_create`, `wlr_idle_inhibit_v1_create`, `wlr_xdg_shell_create`, `wlr_foreign_toplevel_manager_v1_create`, `wlr_ext_foreign_toplevel_list_v1_create`, `wlr_ext_image_copy_capture_manager_v1_create`, `wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create`, `wlr_xdg_decoration_manager_v1_create`, `wlr_xdg_activation_v1_create`, `wlr_output_manager_v1_create` | `calloc` and `wl_global_create`, nothing else. |
| `wlr_output_layout_create`, `wlr_cursor_create`, `wlr_xcursor_manager_create` | Only allocate. The cursor themes load later, in `wlr_xcursor_manager_load`. |
| `wlr_output_configuration_v1_create`, `wlr_output_configuration_head_v1_create`, `wlr_foreign_toplevel_handle_v1_create` | Only allocate. |

## Other reasons (handled at the call site)

| Call | Why, and the local path |
|---|---|
| `wl_event_loop_add_fd`, `_signal` | Duplicate the descriptor (`wl_os_dupfd_cloexec`) or create one (`signalfd`) and add it to epoll: `EMFILE`, `ENFILE`, `ENOSPC`. A new IPC connection is closed and dropped; the brightness helper and a spawned child are stopped and reaped. |
| `wl_event_loop_add_timer` | All of a loop's timers share one `timerfd`, created by the first call (`wl_timer_heap_ensure_timerfd`); later calls only allocate. The compositor's first timer is made at startup, so afterwards only memory can fail, but each caller keeps the path of a timer that cannot be armed (no repeat, no paste, no snap layouts), which costs nothing. |
| `timerfd_create` (Vela's own: an output's latch and virtual vblank) | Descriptor exhaustion. Without its latch an output draws at the backend's frame event; without its virtual vblank a headless output follows the backend's frames. |
| `sd_event_add_io`, `_time`, `_time_relative`, `_child`, `_signal`, `_defer` | Negative errno, for descriptors (the first timer of a clock creates a `timerfd`, `pidfd`, `signalfd`), a finished or forked loop, and memory. Each caller logs and continues without the source, or takes the source's action at once (the check's `SIGKILL`, the deferred `PropertiesChanged`). |
| `sd_bus_*` | Negative errno from the bus, the peer, the message or memory; handled as D-Bus errors, as before. |
| `wlr_ext_foreign_toplevel_handle_v1_create` | Reads its identifier from `/dev/urandom`, opened on first use: descriptor exhaustion. The window then has no ext-foreign-toplevel handle, which every user of `ext_handle` allows. |
| `wlr_xwayland_create`, `wlr_swapchain_create`, `wlr_drm_syncobj_timeline_create`, `wlr_linux_drm_syncobj_manager_v1_create`, `wlr_linux_dmabuf_v1_create_with_renderer`, backends and renderers | Start processes, open devices, query the kernel or the GPU. Each caller already falls back (no Xwayland, the software cursor, implicit sync). |
| `gbm_create_device` | Device and driver errors. |
| FreeType (`FT_Init_FreeType`, `FT_New_Face`, `FT_Load_*`), fontconfig | Missing or broken fonts: no text, as before. |
| cairo | Errors are kept in the object's status (`cairo_surface_status`), which callers check. |
| `xkb_*_new*` | An invalid keymap or context. |

## Safer fallback

| Call | Fallback |
|---|---|
| `pixman_region32_*` | The damage fallback ("The premise", item 4): the output is redrawn whole. |
