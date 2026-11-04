# Vela themes: declarative Level 2.5 design

> Status: design contract for draft PR #206. Items marked **planned** are
> not implemented. The old PR #205 is superseded, not discarded.

## Product contract

A user installs or selects **one complete look** and receives a consistent,
predictable desktop in one action. They can then override individual
components in **Customize** without modifying the installed package. The
default view never requires understanding different toolkit styles or panel
configuration files.

The author supplies **data**, never executable QML, JavaScript, native modules
or install-time scripts. Vela owns and tests every control and shell variant.
No theme may bypass common application models, accessibility, input handling,
or the compositor's frame scheduling.

## Three orthogonal axes

1. **Tokens:** semantic color, spacing, shape, typography, material, and
   motion values. Typed, validated and clamped; accessibility preferences
   retain priority.
2. **Variants:** stable identifiers selecting official Vela implementations
   (for example `startmenu-variant=seven`). The interface contract, not the
   exact QML file structure, is public.
3. **Inheritance:** one parent per package, recursively resolved before
   local overrides, without cycles or paths outside theme roots.

### Supported today in PR #206

- Legacy INI-style `.conf` themes, custom user packages, layered per-surface
  styles, reactive QML tokens, compositor blur values, and built-in layouts.
- `schema-version=1` and `inherits=<package-name>`; parent lookup within
  the same package family; maximum depth 16; no cycles.
- Typed registry for known tokens and explicit allowlist of official variants.
  Unknown fields and wrong value types are ignored with diagnostics.
- User-provided per-surface styles cannot override unrelated surfaces.
- Candidate preview of colors and basic taskbar geometries using the existing
  `DesktopPreview`, without changing the active theme. Apply is explicit.
- Detailed per-surface styling remains available only under Customize.
- Backward compatibility for the old 80 preset files from #205.

The preview is currently **schematic**, not an accurate rendering of every
surface or a full-desktop rollback mechanism.

## Layer resolution

For each semantic property, the intended order is:

```
Vela built-in defaults
  -> inherited theme chain
    -> selected complete theme
      -> explicit per-surface user override
        -> accessibility policy / mandatory platform constraints
```

The selected package is immutable; a per-user override never edits it.
An **unset** property inherits from its parent rather than being confused
with an explicit transparent/zero value.

### Compatibility policy

Existing `theme=<name>` and `taskbar-style=<name>`,
`startmenu-style=<name>`, `notifications-style=<name>` remain valid during
migration. The former defines a complete theme, while the latter are
individual user overrides. New packages can define all surfaces in one file.
The legacy numeric layout switches must keep working but should not be
documented as the public variant API.

Unknown future-schema versions must not activate silently. Missing parents,
unknown variants, missing assets and malformed numeric or color values need
actionable, localized diagnostics (Settings UI: **planned**).

## Official variant contract

Each official variant must define:

- Stable versioned ID, supported property schema, and platform requirements.
- Accessible keyboard, mouse and touch behavior and common data/services.
- Supported fallback when a capability is unavailable (for example blur).
- Consistent performance constraints and an automated functional test.
- Deprecation/migration policy; do not repurpose an existing variant ID.

Rendering implementation details remain private. For example, a Seven Start
menu may have its own QML delegate tree, but it must use the same app data
source, launch/search behavior and focus semantics as Vela Default.

### Current draft identifiers

| Surface | IDs |
| --- | --- |
| Taskbar | `default`, `floating`, `floating-split`, `dock` |
| Start menu | `default`, `seven` |
| Quick settings | `default`, `glass`, `classic` |
| Notifications | `default`, `aero` |

These are **initial curated combinations**, not evidence that every component
already has an independent implementation.

## Runtime architecture (planned final form)

- **Theme resolver:** a shared C++ library used by Settings, shell, apps and
  compositor. Resolve and validate once into a versioned immutable snapshot.
- **Theme API:** QML receives stable typed properties and variant IDs rather
  than parsing arbitrary files. The compositor receives resolved numerical
  values, never author-authored scripts or QML.
- **Transaction:** validate the whole candidate before activation; publish a
  generation ID and apply consistent values across consumers. If application
  fails, restore the previous generation. A compositor restart must never be
  required simply to switch a theme.
- **Cache:** key previews and resolved themes by package version, ancestry,
  mode, accessibility and override generation; invalidate on file changes.
- **No permanent daemon required** solely for parsing themes.

The current header-only resolver and scattered `Theme.themed()` lookups are
migration-compatible foundations, **not yet** the final shared snapshot API.

## Preview and user interaction

Required UX:

1. Gallery displays a complete-theme thumbnail, title, author and
   compatibility.
2. Selecting a thumbnail only updates a candidate preview. No session state
   or config files are written.
3. Apply activates the theme; user overrides and accessibility settings
   remain independent.
4. Customize permits optional changes to individual surfaces, saved separately.
5. Test on desktop offers a time-limited preview with Keep/Revert, where
   rollback is independent of Settings staying responsive.

Preview tiers:

- **Fast:** reuse existing Vela Settings miniature components and token
  resolver (partly implemented).
- **Faithful:** render real shell controls in an isolated/nested surface or
  headless test session with deterministic mock data. Use Vela's existing
  capture tooling to generate cached images (**planned**).
- **In-session:** transactional temporary apply + rollback (**planned**).

Never use an untrusted author's screenshot as proof of the resulting theme.

## Safety, packaging and maintainability

- Data files have size/depth/range limits, no arbitrary imports, code,
  installation hooks or untrusted process execution.
- Theme assets must resolve inside allowed package roots, with symlinks and
  packaged image limits handled explicitly (**planned hardening**).
- Theme IDs, schema and variant IDs are stable interfaces; implementation
  details remain private.
- A compatibility matrix separates Vela-owned controls, third-party Qt, GTK,
  and portal-exposed appearance preferences. Do not promise universal theming.
- Community themes need clear provenance, author/license metadata and a safe
  update path. Reused Windhawk/Aero assets require a provenance review before
  the draft can be merged.

## Merge gate

- [x] Reuse the working theme/renderer integration from #205.
- [x] Versioned keys, type validation, guarded inheritance and official IDs.
- [x] Runtime surface isolation and explicit candidate selection.
- [ ] Build, CTest, QML lint, GPU functional tests and real desktop check.
- [ ] Finalize immutable snapshot / cross-process consistency boundaries.
- [ ] Render faithful candidate previews and transactional rollback.
- [ ] Validate third-party resources, schema diagnostics and package metadata.
- [ ] Review source licenses, assets and translations.
- [ ] Ensure visual changes respect personal preferences and accessibility.

Do not publish schema 1 as a long-term public promise before the remaining
architecture and migration issues are settled.
