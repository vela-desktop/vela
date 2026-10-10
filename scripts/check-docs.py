#!/usr/bin/env python3
"""Offline checks for documentation paths, fragment anchors and build-dependency drift."""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
DOCS = [ROOT / "README.md", *sorted((ROOT / "docs").glob("*.md")),
        ROOT / "CONTRIBUTING.md", ROOT / "SECURITY.md"]
LINK = re.compile(r'\]\(([^)]+)\)|(?:src|href)="([^"]+)"')
HEADING = re.compile(r'^#{1,6}\s+(.+?)\s*#*\s*$', re.M)
EXPLICIT = re.compile(r'<a\s+(?:[^>]*?\s)?(?:name|id)="([^"]+)"')
TAG = re.compile(r'<[^>]+>')
CODE = re.compile(r'`([^\`]+)`')


def anchors(text: str) -> set[str]:
    found: set[str] = set(EXPLICIT.findall(text))
    count: dict[str, int] = {}
    for match in HEADING.finditer(text):
        heading = CODE.sub(r'\1', TAG.sub('', match.group(1)))
        heading = heading.strip().lower()
        heading = re.sub(r'[^\w\- ]', '', heading, flags=re.UNICODE)
        slug = heading.replace(' ', '-')  # GitHub preserves repeated spaces as repeated hyphens
        suffix = count.get(slug, 0)
        count[slug] = suffix + 1
        found.add(f'{slug}-{suffix}' if suffix else slug)
    return found


def check_links() -> list[str]:
    failures = []
    for doc in DOCS:
        if not doc.is_file():
            failures.append(f'missing docs file: {doc.relative_to(ROOT)}')
            continue
        text = doc.read_text(encoding='utf-8')
        for match in LINK.finditer(text):
            raw = match.group(1) or match.group(2)
            if raw.startswith(('http:', 'https:', 'mailto:', 'data:')):
                continue
            parsed = urlsplit(raw)
            local = unquote(parsed.path)
            if local.startswith('/'):
                continue
            target = (doc.parent / local).resolve() if local else doc
            if not target.is_relative_to(ROOT):
                failures.append(f'{doc.relative_to(ROOT)}: path outside repo: {raw}')
                continue
            if not target.exists():
                failures.append(f'{doc.relative_to(ROOT)}: missing local target: {raw}')
                continue
            if parsed.fragment and target.suffix.lower() == '.md':
                fragment = unquote(parsed.fragment)
                if fragment not in anchors(target.read_text(encoding='utf-8')):
                    failures.append(f'{doc.relative_to(ROOT)}: missing heading anchor: {raw}')
    return failures


def package_section(content: str, prefix: str, suffix: str) -> set[str]:
    if prefix not in content or suffix not in content:
        raise ValueError(f'build dependency section changed: {prefix!r}')
    chunk = content.split(prefix, 1)[1].split(suffix, 1)[0]
    return set(re.findall(r'[A-Za-z0-9_+.-]+', chunk.replace('\\\n', ' ')))


def build_checks() -> list[str]:
    failures = []
    readme = (ROOT / 'README.md').read_text()
    arch = (ROOT / '.github/workflows/build.yml').read_text()
    fedora = (ROOT / '.github/workflows/fedora.yml').read_text()
    try:
        arch_required = package_section(arch, '  DEPENDENCIES: >-', '\n\njobs:')
        fedora_required = package_section(fedora, '          dnf install -y', '\n\n      - uses:')
        arch_documented = package_section(readme, 'sudo pacman -S --needed ', '\n```')
        fedora_documented = package_section(readme, 'sudo dnf install ', '\n```')
    except ValueError as e:
        return [str(e)]
    for name, required, documented in [
        ('Arch', arch_required, arch_documented),
        ('Fedora', fedora_required, fedora_documented),
    ]:
        missing = required - documented
        # CI-only tools are not required for a user's build.
        missing -= {'ccache', 'dbus-daemon'}
        if missing:
            failures.append(f'{name}: README dependency list omits CI build dependencies: {sorted(missing)}')
    return failures


def main() -> int:
    failures = check_links() + build_checks()
    for issue in failures:
        print(f'ERROR: {issue}', file=sys.stderr)
    if failures:
        print(f'{len(failures)} documentation check(s) failed', file=sys.stderr)
        return 1
    print(f'Documentation checks passed ({len(DOCS)} Markdown files).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
