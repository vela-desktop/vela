# Bluetooth pairing and device lifecycle — design draft

> **Status:** proposal only. No implementation and no claim of test coverage.
> Target: stabilization of beta 0.1. Final interface/ownership decisions pending review.

## Problem

Vela's Settings uses BlueZ over the system D-Bus (`settings/src/bluetooth.cpp`) for discovery, pairing and connecting devices, but does not currently implement/register a BlueZ `org.bluez.Agent1` pairing agent. `pairAndConnect()` stops discovery, calls `Device1.Pair`, then asynchronously sets `Trusted` and connects. This implicitly relies on another registered/default pairing agent for confirmations, PIN/passkey exchange and authorization. In real use a Bluetooth mouse could not be paired from Vela but could be paired from KDE Plasma.

Further problems identified in the current code:

- `StartDiscovery`, `StopDiscovery`, `RemoveDevice`, and `Trusted` use fire-and-forget D-Bus calls: failures are not surfaced, and state may differ from the UI.
- `refresh()` returns silently on ObjectManager errors and does synchronous D-Bus I/O in the UI process.
- The device `busy` flag has no formal operation/timeout/restart lifecycle; discovery is stopped before pairing without a guaranteed resumption path.
- D-Bus `PropertiesChanged` and adapter disappearance need explicitly tested state invalidation and hotplug behavior.
- `bluedevil` is currently an optional Bluetooth file-transfer integration, not an acceptable hidden requirement for native pairing. BlueZ remains the system stack.

## Proposed architectural direction (to discuss)

1. Introduce a session-scoped Vela Bluetooth service/controller responsible for BlueZ availability, agent registration, operation state, and device models. Settings/quick settings become clients rather than each owning the lifecycle.
2. Implement `org.bluez.Agent1`: at minimum `RequestConfirmation`, `RequestPinCode`, `RequestPasskey`, `DisplayPinCode`, `DisplayPasskey`, `RequestAuthorization`, `AuthorizeService`, `Cancel`, and `Release` according to supported capabilities. Register with `AgentManager1`; decide whether and when to request default-agent status. Never silently auto-approve authentication.
3. Expose confirmation/passkey UI from the Vela shell, including timeout, cancellation, accessible keyboard focus, and behavior when Settings is closed or the shell restarts.
4. Model device operations as explicit async states (discovering, pairing, authenticating, connecting, connected, failed/cancelled), preserving original BlueZ error details for diagnostics.
5. Handle adapter loss, BlueZ daemon restart, rfkill, devices disappearing during pairing, multiple adapters, reconnect, trust failure and removal explicitly.
6. Keep BlueZ; audit whether `bluedevil` is still needed for the separate OBEX/file-sharing path rather than forcing a KDE dependency for pairing.

## Acceptance criteria

- Clean Vela session with BlueZ but **no KDE/BlueDevil pairing agent**: pair and connect a Bluetooth mouse/keyboard; test both Just Works and confirmation/passkey flows with appropriate hardware/emulation.
- Reject, cancel, timeout, device shutdown and unexpected D-Bus failure present accurate, actionable UI state; no stuck `busy` device.
- Adapter off/on, Bluetooth service restart, reconnection and session switch do not leave stale device state or orphaned agent registrations.
- Unit tests use a fake BlueZ D-Bus service and cover pairing state transitions; at least one real-hardware manual pairing check is recorded before beta.
- Document required BlueZ service/packages separately from optional Bluetooth file-sharing features.

## Scope / non-goals

This draft changes **only** documentation. Do not reimplement BlueZ or fold Bluetooth OBEX file transfer into the initial pairing fix. Final architecture and release classification require review.
