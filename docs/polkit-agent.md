# The polkit agent

Design of Vela's own polkit authentication agent: the dialog that asks for an
administrator password when an app or a command needs one (`pkexec`, `run0`,
renaming the PC, changing the date, mounting a disk...). It replaces KDE's
agent (`plasma-polkit-agent.service`). This document comes before the code:
decisions are made here, and the code follows them.

> **Status:** design approved (§2). Stages **P0–P3 are done** (§11): the
> agent, the dialog, `Vela.Controls` and the compositor changes, with their
> tests (§10). Next: P4, trying it in the real session.

## 1. Goals

1. **It looks like Windows 11's User Account Control.** The whole desktop
   dims and blurs, and a single dialog in the middle asks the question. It
   says who is asking and what for. Nothing else can be clicked until the
   user answers.
2. **It is always correct about the session.** It registers for the
   session Vela is running in. That also holds when Vela is started from a
   TTY while Plasma is running on another console.
3. **It costs nothing while idle.** The agent runs all day and is needed a
   few times a week. The process that stays alive has no Qt, no QML and no
   GPU context.
4. **Requests never get lost or refused.** Two requests at the same time are
   queued and shown one after the other, as on Windows. A request withdrawn
   by polkit disappears, whether it is on screen or still waiting.
5. **It can be tested without touching PAM.** Wrong passwords sent to the
   real PAM stack can lock the user's account (`pam_faillock`, see
   [testing](testing.md)). The tests never get anywhere near it.

## 2. Decisions taken

| Decision | Choice | Consequence |
|---|---|---|
| Look | **UAC-like, full screen**: dimmed and live-blurred desktop, modal dialog | layer-shell overlay surfaces on every output (§6.1) |
| Processes | **two**: `vela-polkit-agent` (always running, no GUI) and `vela-polkit-prompt` (Qt Quick, one per request) | they talk over a pipe with lines of text (§5) |
| Who starts the agent | **the compositor, as a supervised child**, like `vela-shell` | it inherits the logind session (§4.1); no systemd unit |
| Language | **C++**, like the rest of Vela | no Rust: it would bring no real security gain (the password is typed in the Qt prompt anyway) and would add a second toolchain |
| polkit library | **libpolkit-agent-1 directly** (GLib), not polkit-qt6 | polkit-qt6's `cancelAuthentication()` doesn't say *which* request is cancelled. KDE's agent therefore refuses a second request while one is open. With a `GCancellable` per request we can queue (§4.3) |
| Shared QML | a new **`Vela.Controls`** module (Theme, buttons, text fields, blur) | used by the prompt first, then by the shell, Settings and Files (§8) |
| Tests | the prompt is driven **by the test itself**, which plays the agent; the agent's core runs on a **fake backend**, the whole agent against a **fake polkitd** | no polkit and no PAM in automated tests (§10) |

## 3. Overview

```
polkitd (system bus)
   │  BeginAuthentication / CancelAuthentication (D-Bus)
   ▼
vela-polkit-agent ──── PolkitAgentSession ────► polkit-agent-helper-1 ──► PAM ("polkit-1")
   │        (via /run/polkit/agent-helper.socket, or the setuid helper)
   │ fork + exec, two pipes (§5)
   ▼
vela-polkit-prompt (Qt Quick, layer-shell overlay) ──► the compositor
```

- `vela-compositor` starts `vela-polkit-agent` and restarts it if it crashes.
- `vela-polkit-agent` registers with polkitd for the current session.
  - For each request it picks the identity and runs a `PolkitAgentSession`.
  - It starts one `vela-polkit-prompt` and passes the PAM conversation
    through to it.
- `vela-polkit-prompt` draws the dimmed screens and the dialog. It sends back
  what the user types, then fades out and exits.

## 4. `vela-polkit-agent`

A small C++ program on GLib/GIO. It subclasses `PolkitAgentListener`
(`initiate_authentication` / `initiate_authentication_finish`); GObject
boilerplate is kept to one file. Qt is not linked.

### 4.1 Registration

- Subject: `polkit_unix_session_new_for_process_sync(getpid())`.
  - The process is a child of the compositor, so it is inside the compositor's
    logind session: the one SDDM opened, or the TTY's.
  - A systemd user service would be outside every session. polkit would fall
    back to the user's "display" session, which is Plasma's when Vela is tried
    from a TTY.
  - If logind can't name the session, `XDG_SESSION_ID` is used.
- Object path: `/org/vela/PolkitAgent`.
- **Another agent already registered for this session:** registration fails.
  The agent logs it and exits with a distinct code (2). The compositor does
  not restart it in that case, to avoid a restart loop.
- **polkitd restarted:** libpolkit-agent-1 doesn't re-register by itself.
  The agent watches the `org.freedesktop.PolicyKit1` name owner and registers
  again when it comes back.

### 4.2 A request

`initiate_authentication(action_id, message, icon_name, details, cookie,
identities, cancellable)`:

1. **Identity.** Among `identities` (unix users; groups are already expanded
   by polkit):
   - the current user if present, with no choice shown, as on Windows;
   - otherwise the first one, and the prompt shows a picker. `root` goes
     last.
   - Display name: from the GECOS field, falling back to the login name.
   - Avatar: the same source as vela-lock
     (`/var/lib/AccountsService/icons/<user>`, then `~/.face.icon`).
2. **Who is asking.** polkit puts `polkit.subject-pid` and `polkit.caller-pid`
   in `details`.
   - The agent resolves the subject pid (falling back to the caller pid) to a
     program from `/proc/<pid>/exe`, and to an installed app (`.desktop`
     entry) whose `Exec` runs it. Shells and interpreters (`sh`, `python3`...)
     never match an app by themselves; then the script in their arguments is
     tried. With no app, the name the program was started with is shown
     (`run0`, not `systemd-run`).
   - For `pkexec` the program to run (`program` detail) counts, not the shell
     that started `pkexec`.
   - That gives the name and icon shown at the top of the dialog ("Do you
     want to allow *Settings* to make changes to your device?").
   - For `pkexec` the `command_line` detail is shown under "Show more
     details". For `run0` and other systemd actions, the `unit` and `verb`
     details are shown there.
   - *To verify on the real polkit 127: which `polkit.*` details actually
     arrive.*
3. Start the prompt (§5), then a `PolkitAgentSession` for the chosen identity.
4. Pass the session's signals through to the prompt:
   - `request(text, echo)`, `show-error(text)`, `show-info(text)`;
   - `completed(gained)`.
5. **`completed(true)`:** the agent tells the prompt `done` and completes the
   request.
6. **`completed(false)`:** a finished `PolkitAgentSession` can't be reused,
   so a new one starts for the same identity.
   - If the user had answered, the answer was wrong: the prompt gets `retry`.
     Retries have no limit; only the user's "No" ends the request.
   - If not, the session ended by itself (the helper doesn't start, PAM
     refuses at once). It is tried once more, silently; a second time the
     prompt gets `failed` and says authentication isn't available, instead
     of looping.
   - A PAM lockout arrives as `show-error`, with PAM's own text, and is shown
     as is.
7. **The user answers "No" or presses Esc:** the session is cancelled and the
   request completes with `POLKIT_ERROR_CANCELLED`.

### 4.3 Queue and cancellation

- Requests are kept in a FIFO; only the head has a prompt and a session.
- Every request keeps its own `GCancellable`.
  - Cancelled at the head: the session is cancelled, the prompt gets `cancel`
    and the next request starts.
  - Cancelled while waiting: it is just removed from the queue.
- **The prompt crashes or closes its pipe:** this counts as "No" for the
  current request. A crashing UI never leaves polkitd waiting.
- A request withdrawn by polkit completes with `G_IO_ERROR_CANCELLED` (GTask
  does that for a cancelled `GCancellable`); polkitd doesn't look at the
  answer to a request it withdrew.

### 4.4 Finding the prompt

The same rule the compositor uses for `vela-session-env`:

1. next to the agent's own executable (the build directory);
2. then `${libexecdir}/vela-polkit-prompt`.

`VELA_POLKIT_PROMPT` overrides it, for the tests.

## 5. Agent ↔ prompt protocol

- **Transport.** The prompt's stdin carries agent → prompt; its stdout
  carries prompt → agent. stderr is inherited and goes to the session log.
- **Format.** One message per line: a word, a space, an argument
  (`polkit/protocol.hpp`, shared by both programs).
  - In the argument, `\` and newline are escaped as `\\` and `\n`.
  - Unknown words are ignored, so either side can grow.

Agent → prompt:

| Line | Meaning |
|---|---|
| `action <id>` | polkit action id |
| `message <text>` | the action's message, already translated by polkitd (the agent registers with the user's locale) |
| `icon <name>` | the action's icon name, possibly empty |
| `app <name>` / `app-icon <name or path>` | who is asking (§4.2) |
| `detail <key>=<value>` | one per detail, for "Show more details" |
| `identity <uid> <login> <display name>` | one per identity; the first is selected |
| `avatar <uid> <path>` | optional |
| `show` | everything above has been sent; the prompt appears |
| `request <0\|1> <text>` | PAM asks something; `1` = echo on (e.g. a user name), `0` = password |
| `info <text>` / `error <text>` | PAM messages (fingerprint reader, lockout...) |
| `retry` | wrong answer: clear the field, shake it, say so |
| `failed` | authentication isn't available (§4.2): say so; only "No" is left |
| `done` | authorized: fade out and exit 0 |
| `cancel` | withdrawn by polkit: fade out and exit 0 |

Prompt → agent:

| Line | Meaning |
|---|---|
| `identity <uid>` | the user picked another identity; the agent restarts the session for it |
| `response <text>` | the answer to the last `request` |
| `cancel` | "No", Esc or the window was closed |

The password travels once, in a `response` line, over a pipe between two of
the user's own processes. It then goes into `polkit_agent_session_response`.
Both sides overwrite their copy afterwards, as best they can (§9).

## 6. `vela-polkit-prompt`

Qt Quick, with LayerShellQt and `Vela.Controls`. It runs only while a request
is on screen.

### 6.1 Surfaces

- **A veil on every other output.** A layer-shell surface on the `overlay`
  layer (namespace `vela-polkit-veil`), anchored to all edges, with
  `exclusive_zone = -1`.
  - It has no keyboard (`KEYBOARD_INTERACTIVITY_NONE`) but takes every
    pointer event, so nothing underneath can be clicked.
  - It is a translucent dark fill over a full-screen blur
    (`ext-background-effect-v1`): the desktop stays visible and alive behind
    it, but out of reach.
  - Outputs added or removed while the prompt is open get or lose their veil.
- **The dialog.** Another full-screen `overlay` surface (namespace
  `vela-polkit`) with `KEYBOARD_INTERACTIVITY_EXCLUSIVE`: its own veil, and
  the dialog centered on it.
  - It goes on the output under the pointer: the prompt doesn't name an
    output, and the compositor chooses.
  - The prompt learns which output that is when the surface enters it
    (`wl_surface.enter`); the veils then go on all the others. A veil
    created on the wrong guess disappears within a frame or two, while it is
    still almost transparent.
- **Animation.** The veils fade in, and the dialog fades in and grows
  slightly, using the shell's curves and durations. Both fade out on `done`,
  `cancel` or "No", and the process exits only once the fade is over.
  Durations are in milliseconds and don't depend on the refresh rate.

### 6.2 The dialog

The layout follows Windows 11's UAC:

- **Header** with the title "User Account Control" (*Controllo dell'account
  utente*).
- **The question:** "Do you want to allow this app to make changes to your
  device?"
- **The app:** icon and name (§4.2). Under them, polkit's message for the
  action, e.g. "Authentication is required to change the system time".
- **"Show more details"** expands to:
  - the action id (polkit doesn't give agents the vendor);
  - the command line (pkexec);
  - the program path;
  - any other details polkit sent.
- **Identity:**
  - the avatar and name of the selected administrator;
  - a picker if there are several identities and the current user is not one
    of them.
- **The PAM prompt**, under the identity:
  - a password field for `request 0`, a plain text field for `request 1`;
  - PAM's own label, if it isn't just "Password:";
  - a Caps Lock warning;
  - `info` and `error` texts.
- **Buttons:** **Yes** (primary, disabled while the field is empty) and
  **No**.

Behavior:

- Enter = Yes, Esc = No, and Tab moves through the dialog.
- The field has focus as soon as the dialog appears.
- While the helper is checking, the buttons are disabled and a small
  indeterminate bar runs.
- A `retry` clears the field, shakes it and says "The password is incorrect.
  Try again."
- A `request` with no password field on screen (e.g. after a fingerprint
  timeout) shows one.
- The dialog never grows past its output. The band with Yes and No stays in
  view and the content above it scrolls. Identities show three and a half at
  most and details 140 px at most, and both scroll. The password field
  scrolls into view when it takes the focus.
- Every text is plain text: app names, polkit's message and PAM's words
  come from outside, and markup in them must not change the dialog.
- Text follows Vela's rules: English in the code, Italian in
  `i18n/vela-polkit-prompt_it.ts`. polkit's own message is already in the
  user's language.

### 6.3 Light and dark, scale

The prompt uses the same Theme as the shell, so it follows light/dark and the
accent color. It is a layer-shell client with fractional-scale and
viewporter, like the shell's panels, and is drawn at the exact scale of its
output. Checked on screenshots at 100% and 125%; an automated sharpness check
of the dialog is still to do (§12).

## 7. Compositor changes

1. **Supervised children** (`struct vela_child`, `process.c`): the shell and
   the agent are restarted by the same code.
   - Exit code 2 from the agent ("another agent is registered") is not
     restarted.
   - The agent gets `SIGTERM`, not `SIGKILL`, when the compositor dies, so it
     unregisters from polkitd.
2. **Who starts the agent.** `VELA_POLKIT_AGENT` names another command, or
   `0` for none. Without it, the agent starts only in a real (DRM) session:
   nested or headless, the logind session belongs to someone else (Plasma
   already has its agent, and a test must never answer polkit for the user).
   `polkit-agent=no` in `vela.conf` leaves the job to another agent. The
   agent is found next to the compositor, then in `libexecdir`.
3. **The modal dialog** (`vela_focus_modal_layer`, `focus.c`): a mapped
   `overlay` surface with exclusive keyboard interactivity.
   - Global bindings are off while it is there: Super, Alt+Tab, Alt+F4,
     Win+D, snapping, desktops... **Exceptions:** switching VT, the
     keyboard's media and volume keys (they act on the player and the volume,
     never on windows, and muting a loud video while typing the password is
     useful), Win+L (locking is always allowed; after unlocking, the dialog
     has the keyboard again) and the developer exit (Alt+Shift+Esc).
   - A keyboard Move/Size in progress ends as with Esc when the dialog takes
     the keyboard, and none can start under it: its keys would swallow the
     password.
   - Neither a window nor a shell panel opening meanwhile takes the keyboard;
     a newer `overlay` surface can.
   - `vela_focus_refocus()` gives the keyboard back to the dialog first (after
     unlocking, or when a panel under it closes).
4. **Stacking.** The `overlay` tree already sits above fullscreen windows and
   above the shell's `top` panels raised over fullscreen.
5. **Tests.** `state` lists the layer-shell surfaces (`layers`) and the one
   with the keyboard (`focusedLayer`).
6. `vela_view_is_system_prompt()` keeps recognizing KDE's agent, for people who disable
   ours.

## 8. `Vela.Controls`

One theme and one set of controls for every Qt Quick interface of Vela.
Before it, `Theme.qml` existed in three copies (shell, Settings, Files) that
had drifted apart; Files also compiles `MenuPanel.qml` straight from
`shell/qml`, which it still does (its shadow is now `Vela.Controls`' `Shadow`).

- **The module.** A static QML module, `Vela.Controls`, in `common/`:
  - `Theme` (singleton): Windows 11's Fluent colors and sizes, named after
    WinUI's resources, plus what only one interface needs (the shell's
    acrylic surfaces, Settings' cards, Files' selection and scroll bars) and
    the veil behind system dialogs;
  - `Button` (standard, accent or subtle, with text and/or an icon) and
    `TextBox` (with `password: true`), since joined by `Fill`, `Switch`,
    `Slider`, `Shadow`, `PanelMotion` and `Dialog`
    ([development](development.md#controls));
  - the C++ singletons `Style` (`Appearance`: light or dark, accent, Mica,
    icon mode; system UI follows the shell's mode, apps their own) and
    `Effects` (`BackgroundEffects`: blur), plus the `image://icon` provider,
    installed with `vela::controls::install()`.
- **The prompt** was its first user; the shell, Settings and Files followed
  (P5). The shell took the Fluent values too: text, secondary text, hover,
  corner radii and dialogs now match the apps'.

## 9. Security notes

What this design protects:

- **The session is the right one** (§4.1).
- **Requests can't be misattributed.** One prompt per request, one session
  per prompt, cancellation by cookie.
- **No other client can take the keyboard while the dialog is up.** That
  means exclusive interactivity plus suppressed shortcuts. Wayland already
  prevents other clients from reading keystrokes.
- **The idle process is small.** No Qt, no QML, no GPU driver. Its inputs
  come from polkitd (root) and from its own child.
- **Test mode can't replace the real agent.** `VELA_POLKIT_TEST_PASSWORD` is
  refused unless the authority on the bus reports itself as the tests' fake
  polkitd (`BackendName` `vela-fake-polkitd`): an address alone could name
  the real system bus.

What it doesn't protect, as with every Linux agent:

- **Another program can draw a look-alike dialog.** The real defense against
  phishing would be a compositor-attested "secure" surface. That is not
  planned; noted as a possible future step.
- **Password memory hygiene is best effort.** The prompt and the agent
  overwrite their buffers. Qt strings, though, can be copied internally, and
  nothing is `mlock`ed. This is the same level as KDE's and GNOME's agents.

## 10. Tests

None of them uses polkitd or PAM.

- **Agent core (GoogleTest, `polkit.*`).** `Agent` runs on a fake backend:
  the prompt is a list of received lines, the PAM session is driven by the
  test. Covered: what the prompt is told, PAM messages, a wrong answer
  retried with a new session, sessions that fail by themselves (no loop),
  "No", a crashed prompt counting as "No", a prompt that doesn't start,
  queueing, polkit withdrawing the active or a waiting request, late events
  from finished requests, choosing another identity, escaping, identity
  order, which program is "asking".
- **Prompt (functional, `test_polkit.py`).** The test starts
  `vela-polkit-prompt` and plays the agent over its stdin/stdout. Covered:
  the answer sent and `done` closing it, `retry`, no second answer while PAM
  is checking, Esc, `cancel` from the agent, a dead agent, `failed`,
  choosing another identity from the keyboard, clicks outside going nowhere,
  a window opening without taking the keyboard, shortcuts off (snap, Alt+F4,
  Alt+Tab) and back on afterwards, a keyboard Move ended by the dialog and
  refused under it, dialog under the pointer and veils on the other outputs.
- **Whole agent (functional, `test_polkit.py`).** The compositor starts the
  real `vela-polkit-agent` (`VELA_POLKIT_AGENT`), which registers with
  `fake_polkitd.py` on a private bus given as `DBUS_SYSTEM_BUS_ADDRESS`.
  `VELA_POLKIT_TEST_PASSWORD` replaces PAM with a session accepting one
  password; the agent refuses it unless the authority is the fake one
  (§9). Covered: registration for a `unix-session` in Vela's language,
  authorized after a wrong password, "No", withdrawn by polkit, a crashed
  dialog counting as "No" with the agent going on, two requests queued,
  restarted after a crash, not restarted when another agent owns the
  session, test mode refused against an authority that isn't the fake.
- The whole agent also ran under AddressSanitizer and UBSan through the
  functional tests: no reports.
- **By hand, by the user** (with the correct password only):
  - `pkexec true`;
  - Terminal (Admin), i.e. `run0`;
  - renaming the PC and changing the date in Settings;
  - two `pkexec` at once (queue);
  - Vela from a TTY with Plasma running (right session).

## 11. Stages

| Stage | Content | |
|---|---|---|
| P0 | `Vela.Controls` with what the prompt needs (Theme, buttons, fields, blur); the shell keeps its own files for now | done |
| P1 | `vela-polkit-prompt` and the protocol (§5, §6), with its functional tests driven by the harness | done |
| P2 | `vela-polkit-agent`: registration, requests, queue, cancellation, re-registration after a polkitd restart, unit tests | done |
| P3 | Compositor: list of supervised children, the agent started with the session, the modal dialog keeps keyboard and shortcuts; `vela-session.target` drops `Wants=plasma-polkit-agent.service`; PKGBUILD depends on `polkit` instead of suggesting `polkit-kde-agent` | done |
| P4 | The user tries it for real (§10, "by hand") | |
| P5 | Shell, Settings and Files move to `Vela.Controls` | done |

## 12. Open questions and risks

- **Which `polkit.*` details polkitd really sends** (§4.2): to check in P4.
  If the subject pid is missing, the dialog falls back to the action's icon,
  as KDE's does.
- **Re-registration after a polkitd restart** (§4.1) is not covered by the
  tests yet.
- **Sharpness of the dialog** at fractional scales is not checked
  automatically yet (§6.3).
- **Qt Quick cold start.** Starting the prompt costs a few hundred
  milliseconds, partly hidden by the veil's fade-in. If it is too slow, the
  agent can keep one prompt started and hidden. That gives up part of goal 3,
  so only if measurements say so.
- **Several requests from the same app in a row** (e.g. a package manager
  asking for three actions). polkit's `auth_admin_keep` already avoids
  repeated passwords where the action allows it. Nothing to do on our side.
