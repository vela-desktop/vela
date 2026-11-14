# Temporary credential backend compatibility

**Remove this entire compatibility layer when Vela Secrets is ready.**

This is a narrowly scoped workaround for #222, not a credential-provider
framework. No credential files are read or migrated.

- `secret-backend=kwallet` (default, preserving Vela's pre-existing behavior)
  exports `XDG_CURRENT_DESKTOP=Vela:KDE`.
- `secret-backend=gnome` exports `XDG_CURRENT_DESKTOP=Vela:GNOME`
  and leaves `KDE_SESSION_VERSION` unset.
- The setting lives in `~/.config/vela/vela.conf` and is edited via
  Settings > System. The selection is made in `vela-session` **before**
  the compositor/supervisor starts. Apply changes by logging out and back in.
- The session hook starts a Vela-owned compatibility unit exactly once per
  session; service shutdown follows `vela-session.target`. GNOME's `--start`
  should reuse an existing PAM-unlocked daemon when available. The unit
  cannot promise automatic unlocking without appropriate distro PAM setup.
- `DesktopNames=Vela;` prevents desktop-manager startup from eagerly
  selecting KWallet before this user choice is applied.
- Neither switching providers nor uninstalling Vela Secrets migrates the
  encrypted data in KWallet or GNOME Keyring.

## Outstanding acceptance checks (draft)

- Live login under Fedora Workstation 44 GDM with an existing GNOME login
  keyring (Brave/Chromium, Electron, `gh auth status`, Secret Service owner).
- Live login under Arch/Plasma SDDM with an existing KWallet (same clients).
- Sign out / sign in with another DE; compositor crash + supervised restart;
  ensure the session does not kill a PAM-owned daemon or steal a bus name.
- First-login guidance when backend selection is absent; GNOME installations
  currently start with the previous `kwallet` default until changed in Settings.

## Removal plan

Delete `session/credential-compat/`, the two `*-compat.service` units,
their install entries, session hook branches and `VELA_SECRET_BACKEND` handling.
Delete the Settings preference and System card. Restore a Vela-only desktop
identity after checking Chromium uses libsecret with Vela Secrets.
