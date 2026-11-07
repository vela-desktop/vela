# Security policy

Vela is beta software and includes security-sensitive components such as the compositor,
screen locker, authentication and polkit integration, session supervisor, updater, and
diagnostic tooling. Passing the current test suite does not establish that every hardware
and software configuration is secure.

## Supported versions

Vela does not maintain parallel stable release branches. Security fixes land on `main`
and ship in the next Beta release.

| Version | Security support |
|---|---|
| Latest Beta release (`vela` package) | Yes: fixes ship in a new Beta release |
| Older Beta releases | No backports: update to the latest Beta |
| Current `main` (Experimental, `vela-git`) | Yes |
| Older development commits, historical alpha builds and test branches | No guaranteed security backports |

When a fix is released, update with `vela-update` (or Settings → System → Updates) rather
than expect an older release or snapshot to be patched in place.

## Reporting a vulnerability

**Do not open a public issue, discussion, or pull request for an unpatched
vulnerability.** Do not publish exploit code, credentials, sensitive logs, or details
that would make an unresolved vulnerability easier to exploit.

Use GitHub's private vulnerability reporting:

**[Report a vulnerability privately](https://github.com/vela-desktop/vela/security/advisories/new)**

A CWE assignment is not required before submitting a report. If you are unsure whether
a finding has security impact, report it privately and let the maintainers triage it.

A useful report includes, when relevant:

- the affected Vela release or commit;
- distribution, kernel, GPU and driver versions;
- the security boundary or assumption that is being violated;
- attack prerequisites and expected impact;
- clear reproduction steps and a minimal proof of concept;
- sanitized logs, screenshots, or diagnostics needed to reproduce the issue.

Please minimize personal data and secrets. Review diagnostic archives before attaching
them and redact tokens, credentials, private paths, user content, and unrelated logs.

## Security-sensitive areas

Examples of reports that are especially useful include:

- lock-screen bypasses or disclosure of content while the session is locked;
- authentication, PAM, or polkit bypasses;
- privilege escalation or unsafe privileged file operations;
- cross-session or cross-user exposure of clipboard, credentials, files, or screen
  contents;
- authorization flaws in Vela IPC or command interfaces;
- vulnerabilities in update, package-integrity, or installation paths;
- memory-safety issues that can be triggered by an untrusted Wayland client, file,
  device, or other input;
- diagnostic or crash-reporting behavior that exposes secrets without informed user
  consent.

Crashes, rendering defects, performance regressions, and hardware compatibility problems
without a security impact should use the normal
[bug report form](.github/ISSUE_TEMPLATE/bug_report.yml).

## Coordinated disclosure

Vela does not promise a fixed response or remediation timeframe while the project is in
beta. Maintainers will evaluate private reports, reproduce them where possible, prepare
a fix, and coordinate disclosure based on the severity and practical exploitability of
the issue.

Please give the maintainers a reasonable opportunity to release a fix before publishing
actionable technical details. When appropriate, the final GitHub Security Advisory can
document affected versions, remediation, credit, and a CVE.

## Safe research

Only test systems, accounts, and data that you own or are explicitly authorized to test.
Avoid unnecessary persistence, disruption, or access to real user data. If testing exposes
credentials or private information, stop once the issue is demonstrated and include only
the minimum sanitized evidence needed to reproduce it.
