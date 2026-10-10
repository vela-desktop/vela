# Security policy

Vela is alpha software. The compositor, authentication agent, screen locker and session supervisor are security-sensitive components; passing current tests does not establish that every hardware and software configuration is secure.

## Reporting a vulnerability

Please **do not disclose unpatched vulnerabilities, credentials, exploit code or private diagnostic logs in a public GitHub issue or pull request**.

If the repository's **Security** tab offers **Report a vulnerability**, use GitHub's private vulnerability reporting to share the details with maintainers. If that option is unavailable, contact the maintainers privately before publishing sensitive details. A public issue may request a private contact method but should not contain technical exploitation information.

Include the affected Vela commit, distribution, kernel, driver, steps to reproduce and expected security boundary. Minimize personal data and redact diagnostic archives before sharing them.

There is no promised response or fix timeframe while Vela is in alpha. Maintainers should coordinate disclosure and release fixes before publicizing actionable details.

## Scope

Reports involving the lock screen, polkit/PAM, privilege boundaries, clipboard access, command sockets, compositor crashes that expose protected content, or credential leakage are particularly valuable. Functional defects without a security impact can use the normal [bug report form](.github/ISSUE_TEMPLATE/bug_report.yml).

## Supported versions

Development currently occurs on `main` and test branches rather than stable, maintained releases. Security fixes are expected to land on the active development branch; older builds and experimental branches may not receive backports. Do not treat Vela as a hardened production security boundary solely because a particular test passes.
