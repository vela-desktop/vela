# Compositor security model

This page describes the trust boundaries of `vela-compositor`: who may talk
to it, which Wayland protocols each client may use, how the screen lock
survives crashes, and what the renderer refuses from clients. To report a
vulnerability, follow [SECURITY.md](../SECURITY.md).

## Who is trusted

| Peer | Trust |
|---|---|
| Processes of the session user, outside any sandbox | Full: the shell, Settings, the CLI, screenshot tools, the screen-sharing portal. Wayland cannot tell these apart. |
| Sandboxed clients (Flatpak, or any sandbox using `wp_security_context_v1`) | Ordinary windows only: no privileged protocols, no command socket. |
| Xwayland | Ordinary windows only: X11 apps capture the screen through the portal. |
| Other users | None. |

## Command socket

`$XDG_RUNTIME_DIR/vela-$WAYLAND_DISPLAY.sock` acts on the whole session
(logout, lock, window actions, end task), so:

- it is created only inside a private `XDG_RUNTIME_DIR` (mode `0700`, owned by
  the user), so no other user can reach it, and is then set to mode `0600`; a
  pre-existing file with that name is replaced only if it is a socket owned by
  the user;
- every connection is checked with `SO_PEERCRED`: another user's process, or a
  process inside a Flatpak sandbox, is refused;
- at most 32 connections are open at once;
- `test-output`, `test-power` and `test-shell` exist only for the functional
  tests: they are refused unless the compositor runs on the headless backend,
  outside a real session, with `VELA_TEST_COMMANDS=1`
  ([testing](testing.md)).

## Privileged protocols

These globals are withheld from sandboxed clients and from Xwayland
(`compositor/src/security_policy.c`):

| Purpose | Globals |
|---|---|
| Reading the screen or other windows | `zwlr_screencopy_manager_v1`, `ext_image_copy_capture_manager_v1`, `ext_output_image_capture_source_manager_v1`, `ext_foreign_toplevel_image_capture_source_manager_v1` |
| Listing and driving other windows | `ext_foreign_toplevel_list_v1`, `zwlr_foreign_toplevel_manager_v1` |
| The clipboard without focus | `zwlr_data_control_manager_v1`, `ext_data_control_manager_v1` |
| Synthetic input (only with `VELA_DEBUG_INPUT`) | `zwlr_virtual_pointer_manager_v1`, `zwp_virtual_keyboard_manager_v1` |
| Outputs and the desktop | `zwlr_output_manager_v1`, `vela_output_preview_manager_v1`, `zwlr_layer_shell_v1`, `ext_session_lock_manager_v1` |
| Whether the user is away | `ext_idle_notifier_v1` |
| Minting sandbox sockets | `wp_security_context_manager_v1` |

A client is sandboxed when it connected through a `wp_security_context_v1`
socket (Flatpak 1.15 and later create one per app) or, for older sandboxes,
when its process has `/.flatpak-info` in its root. Only a client positively
verified to be outside a sandbox counts as unsandboxed. These cases count as
sandboxed:

- the peer's process can't be inspected, for example a non-dumpable process
  whose `/proc/<pid>/root` is unreadable;
- the peer is in another pid namespace;
- the peer has already exited, or its pid can't be tied to the connection.
  `SO_PEERPIDFD` (Linux 6.5 and later) provides that tie: the inspected pid
  must be the pidfd's own process, still alive after the inspection.

Vela therefore needs Linux 6.5 or later. On older kernels every peer counts as
sandboxed, so the shell can't use the command socket.

The same rule applies to the command socket. The global filter hides the
globals from the registry and libwayland refuses to bind them, so a client
cannot reach them by guessing names.

## Screen lock across crashes

- The locked state lives in a sealed shared page owned by the supervisor and
  in a marker file. Anything other than a page saying "unlocked" with a marker
  positively absent restarts the compositor locked; an unreadable marker counts
  as locked.
- A restarted compositor engages the lock (black backdrop, desktop layers
  disabled, focus revoked) before the backend starts and before any client is
  served, so every output appears already behind the backdrop.
- `vela-lock` (or `VELA_LOCK`) is launched with a Wayland connection created
  for it (`WAYLAND_SOCKET`). While Vela holds the screen locked (Win+L,
  inactivity, a restart, or a locker that crashed) only that connection may
  register an `ext_session_lock_v1`; any other client's lock is refused,
  because whoever holds the lock can unlock it. While the session is unlocked a
  client may still lock it (for example `swaylock`), and can only unlock its own
  lock.
- A locker that crashes leaves the screen black and is relaunched with a new
  connection; the old one is no longer trusted.

## Client buffers

- Shared-memory buffers: every upload checks that rows hold their pixels
  (`stride >= width * bytes per pixel`) and that the last byte read is
  addressable, on the first upload, on damage updates and on deferred uploads.
- Dma-bufs: before Vulkan sees an explicit plane layout, the compositor checks
  it against the dma-buf's size (`lseek(SEEK_END)`): plane 0's rows must fit,
  auxiliary planes must start inside the buffer, and the image's memory
  requirement must not exceed the buffer. This covers client textures and
  client buffers used as capture targets, which the GPU writes.
- Commits wait for their fences in the event loop before they are applied
  (`compositor/src/scene/ready.c`). A CPU wait for the GPU is bounded
  (`VELA_RENDERER_WAIT_TIMEOUT_NS`, 10 s, longer than the drivers' hang
  detection): past it the compositor ends and the supervisor restarts it,
  locked if it was, instead of freezing the session.
