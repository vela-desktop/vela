# Post-crash reporting notification

> Status: proposed. Target: 0.2, subject to review before feature freeze.

## Goal

After Vela recovers from an unexpected compositor or shell failure, offer a
one-click path to the existing [Vela Report](reporting.md) workflow. Recovery
must remain independent of reporting. No new diagnostic collector or upload
service is required.

## User experience

Once the recovered shell is ready and the session is unlocked, show a
non-critical notification:

- **Vela recovered from an unexpected error**
- “You can help improve Vela by creating a diagnostic report.”
- **Create report** opens the guided reporter; dismissal does nothing.

Identify the affected component when known, without claiming a root cause.
Use translated shell strings. Respect notification preferences and DND; retain
the notification in the notification center when banners are suppressed.
Coalesce repeated failures into one pending notification per session, and
never open the reporter automatically.

## Design

- The compositor supervisor (`compositor/src/supervisor.c`) is the authority
  for unexpected compositor exits, including device-loss recovery. The
  existing shell child lifecycle owner records unexpected shell exits.
  Exclude normal logout, intentional shutdown/restart and unsupported GPU
  startup failures. Preserve existing secure-lock and restart-limit behavior.
- Publish a small, versioned incident record through a private, session-scoped
  handoff surviving child restarts. Include incident ID, component, timestamp
  with offset, boot/session identity and available exit classification.
  Use bounded storage, atomic updates and user-only permissions; never put
  arbitrary logs, command lines or credentials in the record. Isolate nested
  and concurrent sessions. Failure to record an incident must not delay or
  prevent recovery.
- The shell consumes pending incidents only after notification handling is
  ready and the session is unlocked. Use the existing notification model and
  a trusted internal action, not a subprocess in the crash/restart path.
  Acknowledge successful delivery and deduplicate by incident ID across shell
  restarts; bound retries and expire stale session records. A crash between
  delivery and acknowledgement may cause a duplicate, never an endless loop.
- Launch the installed `vela-report --gui --incident-time <timestamp>` using
  an argument array. Supply a verified matching retained log via `--log`
  only when its identity is known; otherwise leave incident selection to the
  reporter. Do not guess a rotated log or boot. Validate record fields and
  prevent arbitrary executable or argument injection. If the reporter is
  unavailable, keep recovery working and provide a concise manual-report hint.
- Preserve the reporter's existing collection consent, review, redaction and
  export confirmation. Clicking the action is not consent to collect data.
  No automatic collection, archive creation, upload or privileged operation.

For 0.2, only successful recovery within the same session is in scope.
Reporting after a terminal crash or logout remains available through the
standalone reporter. Cross-login notifications and persistent crash history
are deferred. The exact handoff transport is an implementation review decision.

## Acceptance criteria

- Inject unexpected compositor and shell exits; each successful recovery
  produces the appropriate notification and launches the guided reporter with
  the correct incident time when requested.
- Normal logout and deliberate restart produce no notification. Device loss
  retains existing bounded recovery behavior.
- Locked sessions never expose a notification or launch a reporter before
  unlock. DND, dismissal and repeated shell restarts behave as described.
- Repeated crashes, stale/malformed records, concurrent sessions, missing
  reporter and log rotation do not block recovery or misattribute evidence.
- Automated tests cover classification, handoff, deduplication and action
  arguments; a real-session smoke test verifies recovery and the consent flow.
  Existing supervisor, lock, notification and reporting tests must pass.

## Implementation sequence

1. Add incident classification and bounded handoff with lifecycle tests.
2. Integrate shell notification and trusted reporter action with tests.
3. Validate real-session recovery and update the roadmap/reporting documentation.

This proposal changes no runtime behavior until the implementation is reviewed.
