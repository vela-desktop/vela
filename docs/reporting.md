# Vela Report

A guided diagnostic report that users can share with a Vela maintainer.

**Status: implemented.** The standalone Python core and guided CLI live in
`report/vela_report/`; the graphical frontend in `report/gui/` uses Qt Widgets
and the same collection worker. This document records the design and the
current implementation's boundaries. It does not introduce a renderer fix.

## Quick start

```sh
vela-report                 # normal desktop window when it can start; otherwise CLI
vela-report --cli           # complete guided workflow in a TTY or terminal
vela-report --gui           # graphical interface in Vela or another desktop
vela-report --cli --log ./vela.log --output ./vela-bug.zip
vela-report --cli --boot -1 --incident-time 2026-10-08T13:20:00+02:00
```

Both interfaces explain collection choices, require acceptance of the plan,
allow preview, removal, description edits and explicitly chosen attachments,
then ask before exporting. Nothing is uploaded. Text redaction is best effort;
binary attachments and NVIDIA archives require separate review.

Additional options are `--state-dir`, `--description-file`, `--duration`
(1–300 seconds), `--minutes-before` and `--minutes-after` (0–120 minutes).
Run `vela-report --help` for details. Options prefill the guided workflow;
they do not bypass collection or export confirmation.

The installed desktop entry is **Vela Report**. Python 3.10 or newer is the
only CLI dependency. The normal build includes the Qt Widgets frontend; a
CLI-only build does not require Qt, Wayland, X11 or wlroots:

```sh
cmake -S . -B build-report-cli \
  -DVELA_BUILD_COMPOSITOR=OFF -DVELA_BUILD_SHELL=OFF \
  -DVELA_BUILD_POLKIT=OFF -DVELA_BUILD_TOOLS=OFF -DVELA_REPORT_GUI=OFF
build-report-cli/report/vela-report --cli
```

Current limits are 64 MiB for the selected session log, 2 MiB for other text
sources/commands, 256 MiB per attachment and
1 GiB of included evidence. Ordinary commands have a ten-second timeout;
the read-only Vela socket has a two-second timeout, and the NVIDIA tool has a
120-second timeout; the displays' brightness probe has 30 seconds. Live sampling runs once per second with a baseline and
final sample. Stopping recording retains available samples.

Current evidence includes package versions and executable build IDs,
distribution/kernel information, PCI graphics and NVIDIA driver queries,
one explicitly chosen retained log, current configuration and an environment
allowlist, title-free Vela runtime state, the displays' brightness probe, bounded journals, current-user crash
metadata, resource samples, and separately selected NVIDIA reports or
attachments. Other-desktop and older-boot reports keep incident selectors
separate from current machine information. A full installed Vela commit is
not inferred from the reporter build; unavailable identity is recorded as
unknown. Previous boot indices are resolved from accessible journal metadata.

Session logs up to 64 MiB are retained completely. Larger logs retain startup
and the latest messages, with an omission marker and truncation metadata.
During live resource recording the log is refreshed afterwards if it is still
the same file; rotation keeps the original incident snapshot. Journals are
collected newest first so their size cap preserves the end of the incident.

The default boot selector is `auto`: use the boot ID in the selected session
log, or match an older log's last-write timestamp to accessible journal boot
metadata. Unknown incident boots are reported as unavailable; the current
boot is never silently substituted. Without an incident time or log timestamp,
an explicitly selected boot is queried in full, subject to the output cap.

New `vela-session` logs include the start timestamp and boot ID. The supervisor
records compositor FD counts and types, the open-file limit and resident memory
every two seconds, even while the compositor is hung. These historical samples
stay in the selected log across logout/reboot and are also exported as
`logs/resource-history.jsonl`; the last 10000 samples are retained in that
derived file. Excluding session logs removes the derived evidence too. Live
sampling of additional selected processes remains optional.

The session-log summary counts descriptor exhaustion, shell restarts after
errors and layer-shell buffer commits before configuration. Observations
explain that restarting the shell can reset the wallpaper and desktop panels;
they do not attribute the underlying fault to a driver or component. The
summary is refreshed with the log and removed when session logs are excluded.
Package evidence includes `layer-shell-qt`; the environment allowlist includes
Qt's platform, shell integration and scene-graph rendering selections.

Known boundaries: rotations and changes during copying are recorded rather than
reconstructed. Older logs without supervisor samples cannot retroactively
provide descriptor history. The reporter records current process instances and marks exited
or reused PIDs; newly restarted processes need a new recording. It does not
decode GPU memory, generate stack traces, inspect image contents, or redact
binary files. A vendor tool that requires root may produce incomplete evidence;
the reporter never elevates permissions automatically.

## 1. Purpose

`vela-report` creates a local ZIP archive containing a description of a problem
and the diagnostic evidence the user chooses to include. Its purpose is to
make a report useful even when the maintainer cannot reproduce the problem on
their own hardware.

The tool should explain what each collection can reveal, help the user choose
relevant evidence, and produce a report that can be understood without opening
every raw log. Collecting more data is useful only when its relevance and
privacy implications are clear.

All interface text, prompts, help, generated headings, field names and collector
status messages must be in English. Preserve the user's description and
external command output in their original language; do not translate evidence.

## 2. Required environments

The same reporting workflow must work in all of these environments:

| Environment | Required behavior |
|---|---|
| Inside a running Vela session | Collect selected live Vela data and retained logs. Offer graphical review when available. |
| Pure CLI: a TTY, SSH connection or terminal without a display | Provide the complete guided workflow through text prompts. Do not require Wayland, X11, a graphical toolkit or a running compositor. |
| Another desktop environment | Collect retained Vela evidence and ordinary system information. Identify the current desktop separately from the Vela session being reported. |
| After Vela has crashed or the user has rebooted | Produce a useful report from retained files, accessible journal entries and crash metadata. Mark live incident data as unavailable. |
| Nested or headless Vela, including multiple instances | Let the user select the relevant instance and avoid mixing its data with the host or another Vela instance. |

A missing GPU, driver tool, display server, journal, session bus or Vela process
must not prevent creation of an otherwise useful report. The reporter must
never start Vela, switch the user's desktop or restart a service to collect
evidence.

## 3. Architecture and entry points

Use a standalone collection core with a CLI frontend and a graphical frontend.
Both frontends use the same collection plan, explanations, consent choices,
redaction rules and archive writer. The CLI remains usable when the graphical
frontend is unavailable or cannot start.

The graphical frontend must work in Vela and other desktop environments, on
Wayland or X11 where available. It must not require Vela shell integration or a
layer-shell surface. Any integration in Vela Settings or the application menu
launches this standalone reporter.

The core uses Python's standard library; the graphical frontend is a small Qt
Widgets application communicating with that core over a private JSON-lines
pipe. CLI startup does not initialize Qt or graphical services.

Entry points:

```sh
vela-report                          # guided interface; graphical if available
vela-report --cli                    # complete guided text interface
vela-report --gui                    # explicitly request the graphical interface
vela-report --output ./vela-bug.zip   # choose the archive location
```

Automatic mode checks whether a graphical frontend can actually start, rather
than relying only on desktop environment variables. If it cannot, use the CLI
when an interactive terminal is available. An explicit `--gui` failure should
explain how to run `--cli`. Never collect with implied consent because a
frontend failed.

Without an interactive terminal or a usable graphical frontend, show how to
start an interactive report and exit. Do not silently select defaults. An
unattended mode is outside the initial scope; any future mode must receive an
explicit collection plan.

## 4. Guided workflow

### 4.1 Describe the problem

Ask for:

1. A short title and the kind of problem: crash, freeze, rendering, performance,
   input, installation/update, or other.
2. What happened and what the user expected.
3. Steps to reproduce it, including the applications or actions involved.
4. Whether it happens every time, occasionally, or only once so far.
5. When it happened and whether Vela is still running.

Support multiline answers in the CLI, keyboard-only navigation in the GUI,
and an optional description file for longer answers. Let users skip questions
or answer "I don't know". Do not require them to identify the faulty component.

### 4.2 Select the incident

Offer the current Vela session, the previous session, or an explicitly selected
log file. Show timestamps and known session details to help the user choose.
Allow reporting from a different desktop without assuming that desktop caused
the incident.

Record the collection time and incident time separately, including timezone
offsets. Retain boot and session identifiers when available. For live samples,
record elapsed monotonic time as well as wall-clock time.

Present uncertain attribution honestly. A package installed now may differ
from the one that produced an older log, and current configuration may differ
from the configuration active during the incident. Ask whether the user
updated Vela or changed settings afterward; do not label current data as
historical evidence.

### 4.3 Choose collections

Offer a short recommended plan based on the problem category, with a
"Customize" option exposing every collection. Each choice explains its
diagnostic value, potential personal information, expected duration and
approximate size when known.

Ordinary diagnostic collections may be selected in the proposed plan, but the
user must accept or edit that plan before collection starts. Sensitive
collections are excluded by default and need their own explicit selection.
Users may disable any collection; a description and collection manifest are
enough to create a minimal report.

Do not repeatedly request consent for a collection the user already selected.
Ask again only if the scope changes, for example when adding a core dump or
running a privileged vendor tool.

### 4.4 Collect and optionally reproduce

Show progress per collector. For a live recording, ask the user to reproduce
the problem while the reporter samples selected metrics. Provide a visible
countdown and a stop action.

Keep this recording separate from the initial snapshot. Capture a baseline
before reproduction and a final sample afterward. Preserve available samples
if the target process exits or the user stops recording early.

### 4.5 Review and export

Before creating the final archive, show:

- Included collections and files, their sizes, and the reason for including them.
- Excluded, unavailable, failed and incomplete collections.
- The redactions applied and limitations of automatic redaction.
- Any sensitive attachments that still need the user's attention.
- The output location and estimated total size.

Allow users to view collected text, remove a file or an entire collection,
edit their description, or cancel. The CLI must support these actions too;
review cannot depend on a graphical file browser or editor.

Create the archive only after this review. Show its path and size, and explain
that the user can send it to the maintainer. No automatic upload, email,
issue creation or account login belongs in this workflow.

## 5. Collections and explanations

| Collection | Contents and diagnostic value | Proposed selection |
|---|---|---|
| Vela version | Installed package version, executable identity and exact commit when verifiable. Distinguishes regressions and experimental builds. | Recommended for every report. |
| System and runtime versions | Distribution, kernel, architecture and relevant runtime package versions, including wlroots, Qt, Wayland and graphics libraries where identifiable. Helps compare environments. | Recommended for every report. |
| Graphics hardware and driver | GPU model, active driver and available driver version information. Helps isolate GPU and driver differences. | Recommended for rendering, performance, crashes and freezes. |
| Display setup | Relevant output resolution, refresh rate, scale, position and transform; HDR, VRR and backend state when observable. Helps with scaling, presentation and multiple-monitor issues. | Recommended for rendering and performance. |
| Vela logs | Selected current or previous session log, with its provenance and truncation status. Shows startup, failure sequence and recovery. | Recommended for every report when available. |
| Vela configuration | Selected Vela settings, display settings and an allowlist of relevant overrides. Helps reproduce the same behavior. | Recommended, with review and redaction. |
| Session journal | Entries associated with the selected Vela session, relevant processes and services. Helps correlate application crashes and service failures. | Recommended for crashes and freezes. |
| Kernel journal | Entries from the relevant boot and incident time window. Can expose GPU resets, driver errors, OOM events and device failures. | Recommended for crashes, freezes and device problems. |
| Live process sampling | Selected processes' descriptor counts and types, resource limits, memory and CPU metrics. Reveals growth and exhaustion before a failure. | Optional; recommended for leaks, freezes and performance. |
| Existing Vela diagnostics | Read-only runtime state and already recorded timing statistics where available. Helps explain frame scheduling and current compositor state. | Recommended when relevant and available. |
| Display brightness | `vela-brightness --probe`: each connector, its I²C bus, the transfer mode and one read of the brightness with every attempt, its bytes and latency; and the compositor's brightness state (each output's state, value and monitor name). Only reads, under the same lock as the session's helpers ([brightness](brightness.md#11-not-in-scope)). Shows why a slider is missing, locked or wrong. | Optional: it opens every display's bus. |
| Installation/update evidence | Package metadata and user-selected build or update logs. Helps investigate dependency failures and failed installation. | Recommended for installation/update problems. |
| Crash metadata | Available executable identity, signal, crash time and stored dump availability. Helps locate a crash without exporting process memory. | Recommended for crashes. |
| Vendor diagnostics | A separately selected GPU vendor report, such as an NVIDIA bug report. Adds driver-specific evidence. | Optional; offered for the matching hardware. |
| Screenshot or recording | A user-selected image or video of the affected session. Makes visible defects easier to understand. | Excluded by default. |
| Core dump and derived debugging output | Selected process memory and explicitly requested backtrace data. Can reveal the exact crash location, but may expose application data. | Excluded by default; separate consent. |
| Additional attachments | Specific files the user selects and describes. Allows evidence the standard collectors cannot obtain. | Explicit selection only. |

Never substitute current host output information for unavailable historical
Vela output information. A screenshot captured from another desktop must be
identified as such.

The collector registry should be extensible, but new collectors must declare
their purpose, data scope, required permissions, sensitivity and execution
limits before a frontend can offer them.

## 6. Example prompts

These explanations should be short enough for both a terminal and a dialog.

```text
Include kernel logs around the incident?
They can reveal GPU resets and driver errors that do not appear in Vela's log.
They may include device identifiers and details about your machine.
[Include — recommended] [Exclude] [More details]
```

```text
Record resource usage for 60 seconds?
Repeat the steps that trigger the problem while this runs. Descriptor and
memory counts can show which resources accumulate before a freeze or crash.
This records resource metrics, not keystrokes or application contents.
[Record] [Skip] [More details]
```

```text
Include an NVIDIA diagnostic report?
It can help investigate driver-specific problems. NVIDIA's reporting tool may
collect additional system details and may require administrator privileges.
You can review the generated report before adding it to the archive.
[Exclude] [Generate and review] [Select an existing report]
```

```text
Include a core dump?
It can help locate the exact crash, but it contains process memory and may
include documents, messages, credentials or other application data.
[Exclude — default] [More details] [Select and include]
```

Selecting "More details" must not enable a collection. Explain the proposed
command or source there, without requiring users to understand it to make an
informed choice.

## 7. Session discovery and diagnostic accuracy

Discovery must be independent of the current desktop. Inspect only the
reporting user's relevant processes and retained state. Use process start
times as well as PIDs so that a reused PID cannot be mistaken for the original
process during sampling.

Distinguish the Vela supervisor from the compositor. Identify shell and client
processes separately; do not aggregate their descriptor or memory counts into
one apparent compositor leak.

Live compositor queries are optional, read-only and bounded by a timeout. Use
an explicitly selected Vela instance rather than assuming the reporter's
`WAYLAND_DISPLAY` points to Vela. In another desktop it usually describes the
host session.

The usual retained session logs are under
`${XDG_STATE_HOME:-$HOME/.local/state}/vela/vela.log` and `vela.log.old`. Support
an explicitly chosen state directory or file, since the reporting environment
may have different XDG paths from the failed session.

Record each source's path in redacted form, capture time and relevant file
metadata. Snapshot logs early; rotation or new writes must not silently change
which session the report describes. Record if a source changed during copying.

Do not infer a full source commit from a version prefix unless it can be
verified. Record an unknown commit as unknown. Preserve build identifiers when
available so a future backtrace can be matched to the correct executable and
debug symbols.

## 8. Live sampling

The initial recording default is 60 seconds at one sample per second, with
options to stop early or explicitly choose a different duration. Apply bounded
duration and output limits. Sampling is observational: do not change resource
limits, scheduling, driver settings or renderer behavior.

For each selected process, collect when permitted:

- PID, process role, start time, capture time and availability.
- Total open descriptors, highest descriptor number, and soft/hard
  `RLIMIT_NOFILE` limits.
- Descriptor categories, including `sync_file`, syncobj, DMA-BUF, `eventfd`,
  timers, sockets, pipes and other files. Include unlinked files in the counts.
- Resident memory and available memory breakdowns.
- CPU-time counters and sample intervals, so usage can be calculated correctly.

Do not include arbitrary file targets or full process command lines by
default. Descriptor classification should minimize personal path exposure.
Unknown descriptor types stay visible as unknown; do not silently discard them.

Distinguish a growth trend from a one-time high count. The report may summarize
observations such as "descriptor count increased from 120 to 980", but must
not assert that the driver or Vela is responsible without evidence.

Internal renderer counters are collected only if an existing diagnostic
interface exposes them. Missing counters are a documented limitation, not a
reason to modify the compositor as part of this reporter design.

## 9. Privacy, permissions and collection boundaries

Run as the ordinary user. A missing permission becomes a recorded limitation.
Offer narrowly scoped privileged collection only when the user explicitly
chooses it; never rerun the entire reporter as root automatically.

Do not collect a complete environment dump, home directory, browser profile,
shell history, SSH keys, authentication tokens, clipboard contents, input
events, or arbitrary application documents. Configuration collection uses
known files and an allowlist of relevant variables; application command lines
are excluded by default.

Apply best-effort redaction to collected text copies: replace the user's home
path, user and host names, hardware serials and recognized secrets where
practical. Keep replacements consistent within a report so that related
events can still be correlated. Do not alter the original files.

Explain that free-form logs, descriptions and attachments can contain personal
data that automatic redaction does not recognize. Images, core dumps and
compressed vendor reports cannot be assumed safe because text redaction is
enabled. A derived backtrace may also contain sensitive strings or arguments;
it needs review and the same explicit collection boundary.

Preserve diagnostically useful error codes, timestamps, GPU models and driver
versions. Record which transformations were applied. Do not include a mapping
back to the original personal values in the exported archive.

Use a private temporary directory and user-only file permissions. Invoke
collectors with argument arrays, not commands constructed from user text.
Treat descriptions, logs and attachments as data and never execute them.
Archive paths must be relative and must not traverse directories; do not
follow attachment symlinks into unrelated files.

## 10. Execution limits and failure handling

Each collector has a timeout, output size limit and declared permission scope.
Long-running vendor tools show progress and allow cancellation. Cap journal
collection to the selected boot and time window; propose ten minutes before
and five minutes after a known incident, restricted to data already available.
Users can adjust the window using the GUI fields or CLI options, or choose
retained logs when the incident time is unknown.

Record truncation, time windows and dropped samples. Explain when a limit means
the evidence may be incomplete; do not describe a capped log as the full log.
Do not automatically install missing tools, enable verbose logging, restart a
session or perform a reproduction that changes system settings.

Collector results use explicit statuses: `collected`, `excluded`, `unavailable`,
`permission_denied`, `timed_out`, `failed` and `partial`. Include a concise
reason and any usable partial evidence. One failed collector must not discard
other collections.

If the user cancels a collector, offer to continue to review with the evidence
already collected. If they cancel the whole report, remove temporary data.
Export to a temporary archive in the destination directory, then rename it
after completion. Do not overwrite an existing report without an explicit
choice. Clean up temporary collection data after successful export.

Exit codes:

| Code | Meaning |
|---|---|
| `0` | An archive was exported, including a usable report with incomplete collectors. |
| `1` | Collection or export failed so no usable archive could be produced. |
| `2` | Invalid arguments or an interactive interface is required but unavailable. |
| `130` | The user cancelled the whole report. |

## 11. Archive format

Use ZIP for the initial format because recipients can inspect it with common
archive tools. Compression must not require network access. Large sensitive
attachments remain optional; do not silently change formats or split the
report without explaining the result.

Example layout, with only selected and available collections present:

```text
vela-report-<timestamp>-<report-id>.zip
  report.md
  manifest.json
  system/
    reporter-version.json
    packages.txt
    runtime.json
    os-release.txt
    pci-graphics.txt
    nvidia-smi.txt
  session/
    runtime-state.json
  logs/
    vela.log
    session-journal.txt
    kernel-journal.txt
  config/
    vela.conf
    outputs.conf
  samples/
    resources.jsonl
  crashes/
    metadata.txt
  vendor/
    <selected vendor report>
  attachments/
    <selected files>
```

`report.md` is the maintainer's entry point. It contains the user's description,
reproduction steps, incident and collection times, selected session, version
summary, observations, collection results, redaction summary and links to
included evidence. Keep the user's statements separate from tool observations
and inferred conclusions.

`manifest.json` has a versioned schema independent of the Vela release. Include
reporter version, report identifier, collection plan, per-collector status,
source provenance, timestamps, duration, size limits, redactions, artifact
sizes and SHA-256 hashes. Do not include omitted content or secret values in
failure messages. The archive must be understandable without a custom viewer.

## 12. Validation and acceptance criteria

Validate the reporter with fixture logs and a fake process/journal environment
so most checks do not require a GPU or privileged access. Tests should prove
collection boundaries and report integrity, not merely mirror implementation.

Required scenarios:

- Pure CLI with no display, session bus or running Vela.
- Reporting a previous Vela incident from another desktop and after reboot.
- A running Vela instance, multiple instances, and a supervisor restart during
  sampling.
- Missing packages, vendor tools, journal access and GPU devices.
- A process exiting or its PID being reused while recording.
- Permission failures, collector timeouts, log rotation and size limits.
- Refusing sensitive data, removing a collection at review, and verifying that
  excluded content is absent from both archive and manifest.
- Redaction without changing originals or losing timestamps and error codes.
- Cancellation, archive write failure, an existing destination file, and safe
  attachment paths.
- Equivalent collection decisions and output from the CLI and GUI.

The first release is acceptable when a user can generate and inspect a report
in each required environment, explain what they chose to include, and deliver
an archive that gives the maintainer enough provenance to interpret the data.
No collector may be required solely because the user lacks another GPU or the
maintainer cannot reproduce the problem locally.

## 13. Running the checks

```sh
PYTHONPATH=report python3 -m unittest discover -s report/tests -v
ctest --test-dir build -R '^reporting' --output-on-failure
```

The core suite uses isolated logs, process fixtures, simulated journals and an
actual terminal without a display. The graphical smoke check runs the real Qt
frontend with the offscreen platform and exercises discovery, the shared
worker, review and ZIP export. Both suites run in CI with `VELA_BUILD_TOOLS=ON`;
the GUI check also requires `VELA_REPORT_GUI=ON`. These checks do not replace a
manual visual check in Vela and another desktop, or an NVIDIA hardware run.
