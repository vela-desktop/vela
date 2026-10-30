# Updates

Design of Vela's updates in the desktop: Settings > System > Updates, the
background check and the notification. It is the 0.1 release blocker in the
[roadmap](roadmap.md), on top of the release channels of
[release-channels.md](release-channels.md). This document comes before the
code: decisions are made here, and the code follows them.

> **Status:** U0 (this design), U1 (`vela-update`'s report), U2 (the
> daemon) and U3 (the Qt client) are done. See §11.

## 1. What there is today

- **`vela-update`** (`packaging/vela-update`) updates Vela from the command
  line. The installed package picks the channel: `vela` is Beta, `vela-git`
  (or nothing) is Experimental.
  - Beta (`packaging/vela-update-beta`) installs the newest signed `v0.x.y`
    release. `--check --json` prints a schema-versioned status (v1).
  - Experimental fetches a Git mirror, then downloads a signed package for
    that commit or builds one, and installs it. `--check` prints only text.
- **PR #133** (Lowell137) added a first Updates page: Settings runs
  `vela-update --check`, reads its text, and opens a terminal to install.
  It came before the channels; this design supersedes it and keeps its
  good parts (the terminal, what's new, the source).

Problems this design fixes:

1. `--check` exits with 1 both when Vela is up to date and when the check
   fails: a caller cannot tell them apart.
2. Experimental has no machine-readable output, and the page would have to
   read log lines.
3. Nothing checks while Settings is closed, so nobody learns that an update
   exists. The roadmap requires a notification.
4. An update started from the command line is invisible to the desktop, and
   the desktop cannot tell how an update it started ended.
5. On Arch, `pacman -Q vela` also answers for `vela-git`, which provides
   `vela`: an Experimental installation was taken for Beta, and
   `vela-update` without `--channel` went to the Beta channel. Packages are
   now matched by their own name.

## 2. Goals

1. **One source of truth.** `vela-update` stays the only code that knows
   how to check, verify and install. The rest of Vela runs it and reads
   what it reports, never its log lines.
2. **The user learns about updates without looking.** One notification per
   new version, never repeated, never on a metered connection.
3. **The password goes where the user can see what asks for it.** The
   update always runs in a terminal: the package manager asks for the
   password there, and the build and its errors stay readable.
4. **Updates from anywhere are seen.** One started from a terminal by hand
   shows in Settings like one started from Settings.
5. **Nothing gets stuck.** Every check and every update ends as done,
   failed (with a reason in plain words) or cancelled, also when the
   terminal is closed or the script is killed.
6. **It can be tested without a network or a package manager.**

## 3. Scope

In 0.1:

- the Updates page: state, last check, what's new, install, channel, the
  Experimental advanced options;
- automatic checks (on by default) and the notification;
- "Sign out to finish" after an update;
- the `vela-update` changes of §5.

Not in 0.1: installing without a terminal, automatic installation, pausing
updates, update history and rollback from the page (the cache keeps the
last packages for rollback by hand), a "Terminal" entry in Default apps.

## 4. Decisions taken

| Question | Decision | Why |
|---|---|---|
| Where the logic lives | a session daemon, `vela-update-daemon`, serving `org.vela.Updates` | checks run while Settings is closed; one owner for state, schedule and the running update |
| How the daemon is started | by the compositor, supervised, like `vela-bluetooth` | it lives exactly as long as the session; no unit files to install |
| Install | always in a terminal | the password prompt and the build output are visible; no privileged helper to audit |
| Which terminal | `xdg-terminal-exec`, else `VELA_TERMINAL`, else `konsole` | the freedesktop proposal, packaged by Arch, Fedora and Ubuntu; the rest as the shell does today |
| How the desktop follows an update | state files written by `vela-update` itself (§5.3) | they work for updates started by hand too, and survive the terminal |
| JSON in C | `sd-json` from libsystemd (257 or later) | already a dependency; no new library |
| Who notifies | the shell, when the daemon says so (§6.4) | notification texts are translated with Qt; the daemon has no translations |
| What's new | Experimental: the commits; Beta: the release texts | commit subjects mean something only on the development channel |

## 5. `vela-update`

### 5.1 `--check --json`, schema 2

Both channels print the same object. Schema 1 (Beta only) has no caller
outside this repository yet, so it is replaced, not kept beside 2.

```json
{
  "schema_version": 2,
  "channel": "beta",
  "status": "update-available",
  "installed_package": "vela",
  "installed_version": "0.1.10-1",
  "version": "0.1.11",
  "tag": "v0.1.11",
  "commit": "<40 hex>",
  "source": "https://github.com/vela-desktop/vela",
  "changes": [],
  "changes_total": 0,
  "notes": [{ "version": "0.1.11", "text": "..." }],
  "checked_at": 1793289600,
  "error": null
}
```

- `status`: `up-to-date`, `update-available`, `downgrade-available`
  (Beta: the installed release is newer than the newest published one),
  `not-installed` (no Vela package: a development build), or `error`.
- `installed_package`: `vela`, `vela-git` or empty, whatever the channel
  checked: a channel transition shows what will be replaced.
- `version`: Beta, the release; Experimental, the project version at that
  commit (`CMakeLists.txt`). `tag` is empty for Experimental.
- `changes`: Experimental only, `[{commit, summary}]`, newest first, at
  most 100; `changes_total` counts them all. Empty when the installed
  commit is not an ancestor of the new one (another branch).
- `notes`: Beta only, the text of every release newer than the installed
  one up to the target, newest first, at most 20 and 64 KiB each.
- `error`: `{code, message}` when `status` is `error` (§5.4).

Exit codes with `--json`: **0** when the check completed, whatever it
found; **1** when it failed (the JSON then has `status: "error"`); **2**
for wrong arguments (no JSON). Without `--json` the human output and its
exit codes stay as they are, for existing scripts.

With `--json`, stdout carries only the JSON; everything else goes to
stderr.

### 5.2 Phases

An install goes through `resolving` (fetch, select the revision),
`downloading`, `verifying`, `building`, `installing` (the package manager,
which may ask for the password). Beta skips `building`. Arch's local build
installs from makepkg, so it goes from `building` straight to the end.

### 5.3 State files

In `${XDG_STATE_HOME:-~/.local/state}/vela-update/`, always written, by a
check or an install, from Settings or by hand; each one replaced with a
rename, never written in place:

| File | Written | Content |
|---|---|---|
| `run.json` | while a check or an install runs; removed at its end | `action` (`check`, `install`), `channel`, `phase`, `pid`, `start_time` (the process's start time in clock ticks, from `/proc/PID/stat`), `started_at` |
| `check.json` | at the end of every check | the §5.1 object |
| `install.json` | at the end of every install | `channel`, `result` (`success`, `failed`, `cancelled`), `error`, `installed_version`, `finished_at` |

If `run.json` names a process that no longer exists (the PID is gone, or
its start time differs), that run was killed without its exit trap: the
daemon treats it as `cancelled`.

The files are written by a small helper, `vela-update-state` (Python, in
libexec next to `verify-attestation.py`), so that both scripts produce the
same JSON and none of it is built by hand in shell.

### 5.4 Errors

Every failure goes through one function that prints the message, records
a stable code and exits. An unexpected exit (a command failing under
`set -e`) is recorded as `failed`; a signal as `cancelled`.

| Code | Meaning |
|---|---|
| `busy` | another `vela-update` holds the lock |
| `unsupported` | distribution or release not supported |
| `missing-tool` | a required program is missing (cosign, rpmbuild, flock...) |
| `network` | the repository or GitHub could not be reached |
| `rate-limited` | GitHub refused with 403 or 429 |
| `not-found` | no such revision, branch or Beta release |
| `downgrade-refused` | Beta would go back without `--tag` |
| `verification-failed` | checksum or signed provenance rejected |
| `build-failed` | the local build failed |
| `install-failed` | the package manager failed or was refused |
| `cancelled` | interrupted (Ctrl+C, the terminal closed) |
| `failed` | anything else |

The page translates the codes; the message is shown under them.

## 6. `vela-update-daemon`

C17 with sd-bus, sd-event and sd-json, like `vela-bluetooth`. Installed
in libexec. The compositor starts it in the real (DRM) session and restarts
it if it crashes; `VELA_UPDATE_DAEMON` names another command, `0` none.
Exit code 2: another instance owns the name.

### 6.1 What it knows

- The last check, from `check.json` (also after a restart).
- The running check or install, from `run.json`, followed with inotify on
  the state directory.
- The last install, from `install.json`.
- Whether the session must be restarted: the running compositor's
  executable was replaced (`/proc/PID/exe` ends in ` (deleted)`), which is
  what a package upgrade does. Walking up from the daemon's parent finds
  the compositor.
- `update-check=yes|no` in `vela.conf` (automatic checks, default yes).
- The last version announced (§6.4), in `announced` in the state
  directory.

### 6.2 Checking

The daemon runs `vela-update --check --json` and reads stdout (at most 1
MiB). Outcomes are taken in the order they come, from that output or from
a new `check.json` (a check run by hand); `check.json` is taken again only
when its atomic replacement changes the file identity (device, inode and
nanosecond modification time). `checked_at` is only a user-facing timestamp:
two checks may finish in the same second. An old report must not hide an
outcome that never reached the file (`busy`, or no report at all). A failed check keeps the
last completed one shown, with the failure beside it. A check never
starts while another check or an install runs, from anyone. A check
taking more than 15 minutes is stopped (SIGTERM, then SIGKILL).

Automatic checks:

- the first 10 minutes after the daemon starts, or 24 hours after the
  last check if that is later;
- then every 24 hours;
- skipped while NetworkManager says the connection is metered (`Metered`
  is yes or guess-yes) or not fully connected (`Connectivity` below
  full, including the unknown state `0`); retried as soon as that changes.
  Without NetworkManager, always allowed.
- after a `network` or `rate-limited` error, retried after 1, 2, 4... hours,
  at most 24.

After the daemon's own check, the next one is timed on the monotonic
clock; only the first after the start uses the dates in the reports, and
a date in the future waits one interval at most. A wrong clock never
makes checks run back to back.

### 6.3 Installing

`Install(options)` opens the terminal with:

```sh
sh -c 'vela-update "$@"; s=$?; echo; [ $s = 0 ] || echo "vela-update failed ($s)"; printf "Press Enter to close. "; read -r _' sh [options]
```

The daemon then waits for `run.json`. If no run starts within 60 seconds,
it reports that the terminal did not start the update (`terminal`), and
the page shows it. From then on, an update started here and one started by
hand look the same.

Options (all optional): `channel` (`beta`, `experimental`: a transition
when it differs from the installed package), `branch`, `commit`, `source`
(Experimental only), `tag` (Beta only, to go back to an older release).
They are checked like the script checks them, so mistakes come back at
once as `InvalidArgs`.

### 6.4 Announcing

`Announce` is the commit of the update to tell the user about: set when a
check finds an update whose commit is not the one in `announced`, cleared
once announced. The shell notifies and calls `Announced(commit)`; opening
the Updates page also counts. An update is announced once, also across
sessions.

After a successful install, `NeedsRelogin` becomes true and the shell
notifies once: "Sign out to finish updating".

## 7. `org.vela.Updates`

Object `/org/vela/Updates`, interface `org.vela.Updates`, on the session
bus. Properties change with `PropertiesChanged`.

| Property | Type | |
|---|---|---|
| `Activity` | s | `idle`, `checking`, `starting` (terminal opened, the script not yet running), `installing` |
| `Phase` | s | §5.2, while installing |
| `Status` | s | the last check's `status`, or `unknown` |
| `Channel` | s | the last check's channel |
| `InstalledPackage`, `InstalledVersion` | s | |
| `Version`, `Tag`, `Commit` | s | the update found |
| `Changes` | a(ss) | commit, summary |
| `ChangesTotal` | u | |
| `Notes` | a(ss) | version, text |
| `Source` | s | Experimental's source (`updates.conf`), or the default |
| `LastCheck` | x | Unix time, 0 if never |
| `Error` | (ss) | the last check's code and message, or empty |
| `InstallResult` | s | the last install's `result`, or empty |
| `InstallError` | (ss) | |
| `NeedsRelogin` | b | |
| `AutoCheck` | b | |
| `Announce` | s | §6.4 |

| Method | |
|---|---|
| `Check()` | starts a check; `Busy` if one or an install is running |
| `Install(a{sv} options)` | §6.3; `Busy`, `InvalidArgs`, `NoTerminal` |
| `SetAutoCheck(b)` | writes `vela.conf` |
| `Announced(s commit)` | §6.4 |

Errors are `org.vela.Updates.Error.*`: `NotAllowed`, `Busy`, `InvalidArgs`,
`NoTerminal`, `Failed`.

**Who may call:** reading needs the same user. Methods also need a caller
that is not sandboxed (Flatpak, Snap): a sandboxed app must not be able to
open a terminal that asks for the user's password. Any unsandboxed process
of the user can already run `vela-update` itself, so no more is checked.
The caller check is the one of `vela-bluetooth` (docs/bluetooth.md §7.3),
moved to a small shared library.

## 8. `common/updates`

`vela-updates-client`: a Qt client of `org.vela.Updates` for Settings and
the shell, like `vela-bluetooth-client`. Without the daemon `available` is
false and the page says the update service is not running.

It gives the daemon's properties as QML reads them: `changes` and `notes`
as lists of objects, `lastCheck` as a date, `checkError` and
`installError` as `{code, message, reason}` with the code in plain words.
The automatic checks switch shows what the user asked for at once, until
the daemon reports it. Refusals of `Check`, `Install` and `SetAutoCheck`
are in `error`, in plain words; those of `Announced`, which the user did
not ask for, only in the log.

## 9. UI

### 9.1 Settings > System > Updates

Top card, after Windows Update:

- the state in words: "You're up to date", "Vela 0.1.11 is available",
  "Checking for updates...", "Updating in the terminal: building",
  "Couldn't check for updates" with the reason, "Development build: not
  installed from a package";
- "Last checked: today, 10:24";
- the button: "Check for updates", or "Install now" (accent) when there is
  an update; disabled while busy;
- "Sign out to finish updating" when `NeedsRelogin`;
- the last install's failure, if it was the last thing that happened.

"What's new": Beta's release texts, or Experimental's commits (the first
10, then "and N more").

Options:

- **Check for updates automatically**, a switch: "Not on metered
  connections".
- **Update channel**: Beta or Experimental. Changing it asks first: what
  the other channel is, that the package is replaced, that the version may
  go back, and that the update runs in a terminal. Confirming calls
  `Install({channel})`.
- **Advanced** (Experimental only, collapsed): source (repository or
  folder), branch or commit, "Install this revision".
- The installed version and channel.

### 9.2 Shell

Notifications only, as the client library tells them:

- "Vela 0.1.11 is available": "Open Updates to see what's new and install
  it." Clicking opens `vela-settings --page updates`.
- "Sign out to finish updating", with a "Sign out" action.

## 10. Tests

- **Script** (`packaging/tests`): schema 2 for both channels, exit codes,
  the state files for check, install, failure and signal, errors' codes,
  Beta notes and Experimental changes, stdout clean with `--json`.
- **Daemon** (`tests/updates`, on a private bus): a fake `vela-update`
  that prints chosen JSON and writes chosen state files; a fake terminal
  that records its command; a fake NetworkManager. Check, busy, timeout,
  install start and the terminal not starting, a run started by hand, a
  killed run, automatic checks with short timers, metered connections,
  `Announce` once per version (with private marker permissions), successive
  reports in the same second, full-connectivity gating, the caller check.
- **Client** (`tests/updates`, on the daemon and the same fakes): the
  properties as QML reads them, failures and their reasons, an install,
  the switch shown at once, refusals, the daemon leaving and coming back.
- **Page** (QML tests): every state of the top card, the channel
  confirmation, the advanced options, no daemon.
- **Shell**: the notifications, once per version.
- **By hand**: a real Beta and a real Experimental update on Arch, Fedora
  44 and Ubuntu 26.10; closing the terminal halfway; signing out after.

## 11. Stages

| Stage | Content | State |
|---|---|---|
| U0 | this design | done |
| U1 | `vela-update`: schema 2, exit codes, state files, error codes, `vela-update-state` | done |
| U2 | `vela-update-daemon` and `org.vela.Updates`; the caller check shared with `vela-bluetooth` | done |
| U3 | `common/updates`, the Qt client | done |
| U4 | the Settings page | |
| U5 | the shell's notifications | |
| U6 | packaging (`xdg-terminal-exec`, libexec files), docs, roadmap, release-channels | |
| U7 | real updates on real systems | |

## 12. Open questions and risks

- **The Beta channel has no release yet.** Until `v0.1.0` exists the
  Beta check ends with `not-found`; the page says that no Beta release
  is published yet.
- **xdg-terminal-exec is still a proposal.** Its command line (`-e` or
  not) could change; Vela passes the command as plain arguments, which
  every version accepts.
- **Terminals that hand off to a running instance** (Konsole) return at
  once: the daemon never relies on the terminal process, only on
  `run.json`.
