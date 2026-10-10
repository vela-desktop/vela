#!/bin/sh
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

# Keep the five most recently published Vela alpha releases. Only releases
# created by the per-commit publisher match all filters below; stable, manual
# and unrelated prereleases are never touched. Removing an alpha release also
# removes its Git tag, so it can be rebuilt from source if requested later.

set -eu

: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
: "${GITHUB_SHA:?GITHUB_SHA is required}"

case "$GITHUB_SHA" in
    *[!0-9a-f]*|"") echo "Invalid commit SHA" >&2; exit 1 ;;
esac
[ "${#GITHUB_SHA}" -eq 40 ] || { echo "Expected a full 40-character SHA" >&2; exit 1; }

current="alpha-$GITHUB_SHA"
# GitHub's --paginate handles repositories with more than 100 releases.
# Compare ISO 8601 UTC publication timestamps lexically (newest first).
# Filter on type, exact tag shape and title to leave other releases alone.
releases=$(gh api --paginate "repos/$GITHUB_REPOSITORY/releases?per_page=100" \
    --jq '.[] | select(.prerelease == true and .draft == false and
        (.tag_name | test("^alpha-[0-9a-f]{40}$")) and
        (.name | startswith("Vela alpha ")) and
        .published_at != null) | [.published_at, .tag_name] | @tsv')

# Never delete anything if GitHub fails to list the release just published.
if ! printf '%s\n' "$releases" | cut -f2 | grep -Fqx "$current"; then
    echo "Current alpha release $current not found; skipping cleanup" >&2
    exit 1
fi

printf '%s\n' "$releases" | LC_ALL=C sort -r | awk -F '\t' 'NR > 5 { print $2 }' |
while IFS= read -r old; do
    [ -n "$old" ] || continue
    # Defense in depth: never remove the release from this run.
    [ "$old" != "$current" ] || { echo "Refusing to delete current release" >&2; exit 1; }
    echo ":: Pruning old Vela alpha release $old"
    gh release delete "$old" -R "$GITHUB_REPOSITORY" --yes --cleanup-tag
done
