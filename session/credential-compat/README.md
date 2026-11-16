# Temporary credential backend compatibility

**Delete this shim when Vela Secrets is ready.** It is not a password-provider
framework: it never opens, reads, deletes, migrates, or copies credential files.

## Choice and first login

- `secret-backend=kwallet` exports `XDG_CURRENT_DESKTOP=Vela:KDE` and
  `KDE_SESSION_VERSION=6` for Chromium/Electron compatibility with Plasma.
- `secret-backend=gnome` exports `XDG_CURRENT_DESKTOP=Vela:GNOME` and removes
  `KDE_SESSION_VERSION`, so Chromium/Electron select GNOME's password store.
- `DesktopNames=Vela;` in the session entry avoids overriding the choice
  before `vela-session` reads `~/.config/vela/vela.conf`.
- When the key is **not configured**, the previous KWallet behavior is retained
  for upgrades. This is *not* a decision on the user's behalf: on the first
  graphical login (and each later login while unset), Vela automatically opens
  a **choice dialog** in Settings, even if the implicit fallback works. The
  user can choose KWallet, GNOME Keyring, or defer until the next login.
  Providers whose daemon is not installed are disabled in the choice dialog.
  The session retries opening Settings for a bounded period and cancels a
  pending prompt when its original session ends (even if the display is reused).
  Settings > System also shows an unselected choice until one is saved.
  Choosing even the already-used KWallet persists the decision explicitly.
  Supervised compositor restarts never relaunch the dialog.
- The choice is fixed before the compositor/supervisor and autostarts launch.
  Changing it requires a full logout/login. No password migration takes place.
- `VELA_SECRET_CONFIGURED` only communicates whether the choice was explicit
  during this session; it does not change the selected provider.

## Secret Service verification and lifecycle

On the first compositor start in a real session, `vela-session-env` sources
`ensure-backend`, which queries the *actual* `org.freedesktop.secrets` bus
owner using `busctl --user` and `GetConnectionUnixProcessID`:

1. If the **correct** daemon already owns Secret Service (for example one
   unlocked by PAM), do not launch or manage it.
2. If **another** daemon owns the bus name, report a conflict. Do not replace,
   terminate, or reconfigure someone else's password store.
3. For KWallet, **also inspect `org.kde.kwalletd6`**, the native KDE API:
   `kwalletd6` is the compatibility wrapper, while `ksecretd` owns
   `org.freedesktop.secrets`. Starting `kwalletd6` does not start the
   Secret Service provider reliably. If only the native API is running,
   check for an existing **ksecretd** before starting another provider;
   leave the native daemon untouched.
   Report `limited` only if Secret Service is still unavailable, because
   libsecret clients (including `gh`) need it.
4. A PAM-started KWallet can be alive without owning any D-Bus name. Inspect
   current-user process metadata and the live listener named by
   `PAM_KWALLET5_LOGIN`, which also covers the interval before `ksecretd` execs.
   The inherited variable or a stale socket pathname alone is insufficient.
   Wait for the provider to register or exit, with ten bounded 200 ms retries.
   If it remains pending, or metadata cannot be verified, report `unverified`
   and leave it alone; do not start or stop a compatibility unit. This reads
   only `/proc` metadata: never process arguments/environment, PAM socket
   contents, file descriptors or credentials.
5. If no compatible Secret Service owner or pending KWallet exists, check the selected daemon
   executable explicitly (for KWallet, `/usr/bin/ksecretd`, not
   `/usr/bin/kwalletd6`). Start the matching Vela-owned systemd unit,
   then verify the bus owner.
   `ConditionFileIsExecutable` and a successful `systemctl start` on their
   own **do not** prove that the provider is usable.
6. Recheck Secret Service after the final verification wait, before native-only
   cleanup. A matching owner that registered during that wait is accepted;
   it must not be stopped or reported as limited.
7. If the bus response is unreadable, no owner appears, or the selected
   executable is missing, report the failure without claiming success.

The result is written to `$XDG_RUNTIME_DIR/vela-credential-compat.status`
with a private umask: `ok`, `limited`, `missing`, `conflict`, `unverified`, or `failed`.
Settings > System displays persistent failure guidance; notification delivery
for actual provider failures is best effort after Vela's notification server
starts. The missing-choice dialog is separate from those warnings. Logs also explain
failures. This state is removed at session teardown.

Only the two Vela-owned compatibility units are stopped at logout; pre-existing
PAM/user daemon processes are not stopped. A supervised compositor restart
keeps the initial selection and does not re-run the provider startup. The
GNOME `--start` command can reuse a daemon initialized or unlocked by PAM.
KWallet's `ksecretd` can be started by PAM before Vela; if it already owns
Secret Service, or is still awaiting its PAM handoff, Vela does not launch or
stop another instance. The native
`kwalletd6` wrapper is left to KDE's normal D-Bus activation.
Automatic unlocking itself depends on the host distribution's PAM configuration
and is not promised by this workaround. `busctl` is provided by systemd on
supported distributions.

Startup ordering was checked against upstream
[KWallet 6.14](https://github.com/KDE/kwallet/blob/v6.14.0/src/runtime/ksecretd/main.cpp)
and [6.31](https://github.com/KDE/kwallet/blob/v6.31.0/src/runtime/ksecretd/main.cpp),
and [kwallet-pam 6.8.2](https://github.com/KDE/kwallet-pam/blob/v6.8.2/pam_kwallet.c).
Both KWallet versions receive PAM data before registering the unique service;
6.31 additionally lets a PAM instance replace an earlier non-PAM instance.
The guard does not depend on that newer replacement behavior. These source
checks do not establish automatic unlocking on a particular display manager.

**Caveat:** `XDG_CURRENT_DESKTOP=Vela:GNOME` intentionally influences more
than Chromium. It also matches `OnlyShowIn=GNOME;` XDG autostarts and some
desktop-specific integrations. Vela needs to audit those autostarts on each
supported distribution; the compatibility shim never claims to isolate GNOME
autostarts. The Vela-specific portal configuration must remain the preferred
configuration. Avoid adding GNOME-only units to Vela's package dependencies.

## Validation

CPU-only deterministic tests:

```sh
ctest --test-dir build -R '^session\.credential-(compat|pending)$' --output-on-failure
```

They cover explicit/unset/invalid selection, selection precedence, a
pre-existing PAM daemon, a wrong bus owner, a missing daemon, failed systemd
start, an unrecognizable D-Bus response, native-only KWallet and ksecretd
ownership, pending PAM registration/exit/timeout, read-only process and listener
metadata, final-wait registration, successful startup, and prompt launch only on the first start
of an unconfigured session.

**Manual acceptance before merge:**

- Fedora 44/45 GDM + a real existing GNOME login keyring: select GNOME,
  login again, check stored Brave/Chromium/Electron sessions and `gh auth status`,
  and confirm that the provider's process owns `org.freedesktop.secrets`.
- Arch SDDM + an existing KWallet: select KWallet, login again, check the
  same clients and confirm both `org.kde.kwalletd6` and, when enabled,
  `org.freedesktop.secrets` (often owned by `ksecretd`). Verify that a
  native-only KWallet causes Vela to attempt starting `ksecretd` only after
  ruling out a pending PAM instance,
  and that a failure is reported as `limited` rather than claiming success.
  Verify that a missing `ksecretd` is clearly reported.
- Verify the first-login modal appears regardless of the implicit KWallet
  fallback and repeats on a new login when dismissed; choosing either provider
  persists `secret-backend` in `vela.conf`. After saving, verify that it
  does not return, including after a supervised compositor restart. Verify that
  forwarding to an already-open Settings instance still displays the modal.
  Also verify that a delayed prompt cannot open after logout, and unavailable
  provider choices are visibly disabled.
- With GNOME and with Plasma active on another VT, confirm that Vela neither
  steals their bus name nor stops their daemon. Switch desktop environments
  and confirm previous credentials remain available.
- Crash and restart the supervised compositor: the chosen backend and its
  bus owner must remain unchanged; there must be no second provider start.
- Inspect `/etc/xdg/autostart` and `~/.config/autostart` for entries with
  `OnlyShowIn=GNOME;` or `OnlyShowIn=KDE;` and verify no unwanted autostart
  side-effects under the chosen desktop alias. Check portal selection as well.
- Confirm that no passwords are migrated and no empty wallet is silently
  treated as the originally selected one.

For a failing session, inspect:

```sh
cat "${XDG_RUNTIME_DIR}/vela-credential-compat.status"
busctl --user status org.freedesktop.secrets
busctl --user status org.kde.kwalletd6 # Native KWallet API; may be inactive until first use
systemctl --user status vela-gnome-keyring-compat.service vela-kwallet-compat.service
```

## Removal plan

Delete `session/credential-compat/`, both `*-compat.service` units,
their CMake and packaging entries, session hook branches, and
`VELA_SECRET_BACKEND` / `VELA_SECRET_CONFIGURED` handling. Remove the
Settings preferences and cards, the `secret-backend` config documentation,
and the reporting test fixture. Restore Vela-only desktop identity after
verifying Chromium uses libsecret with Vela Secrets.

Provider verification runs in a worker after the synchronous environment hook
returns, so a KWallet GUI can complete its Wayland initialization. The first
session target starts after verification; a supervised restart reimports the
environment and does not start a second provider. The worker checks the login
token before changing status or stopping its own unit, so an older login cannot
report or tear down a later login's provider.

Login-state changes and credential side effects share a private runtime `flock`
(util-linux, already a package dependency). Provider discovery never holds it.
Status replacement, nonblocking unit requests and session-target activation
hold it through token validation and the action; logout/new login use the same
lock. An in-flight old action therefore completes before logout cleans it up,
and cannot run after the next login begins. The lock file is never unlinked.
