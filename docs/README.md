# Documentation directory

This directory contains Vela's documentation and supporting assets, including design notes, technical explanations, screenshots, and diagrams. Subdirectories may be used to organize these materials.

## Repository policy

Everything under `docs/` is treated as **documentation-only** by the path-aware pull request branch-freshness check. Changes here do not, by themselves, require an open pull request to update its branch from `main`.

Keep executable code, build inputs, CI configuration, generated files consumed by the build or tests, and other runtime-affecting resources **outside** `docs/`. Even if a file looks like documentation, it must not live here if changing it could affect how Vela builds, tests, installs, or runs.

If that assumption ever changes, update the freshness-check policy before introducing the dependency. This exemption only relaxes the branch-update requirement; normal reviews and other required checks still apply.
