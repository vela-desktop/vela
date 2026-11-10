#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Direct allocations in the host C code (docs/memory-allocation-policy.md,
"Enforcement"). Outside common/alloc/alloc.c, the in-scope sources call the
helpers of common/alloc/alloc.h, never the libc allocators. Rejected:

- malloc, calloc, realloc, reallocarray, strdup, strndup, asprintf,
  vasprintf, posix_memalign, aligned_alloc, open_memstream and scandir;
- realpath with NULL as its second argument (with a buffer it allocates
  nothing);
- getline and getdelim not in the policy's form: `errno = 0;` as the
  statement right before the call, and ENOMEM checked later in the same
  function.

The legacy sites still to migrate are counted per file and call in
common/alloc/legacy-sites.txt. A count above the baseline fails (a new
site), and so does one below it, so the baseline only shrinks: lower it in
the same change, or rewrite it with --write-baseline.

A scan of the sources without the preprocessor: comments and string
literals are blanked first, and calls through a macro are not seen."""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "common/alloc/legacy-sites.txt"
# The C code of the session and the update daemon. Test programs and tools
# are out of scope.
SCOPE = ("compositor", "bluetooth", "brightness", "capture", "updates", "common/alloc", "common/caller")
EXEMPT = {"common/alloc/alloc.c"}

ALLOCATORS = ("malloc", "calloc", "realloc", "reallocarray", "strdup", "strndup", "asprintf",
              "vasprintf", "posix_memalign", "aligned_alloc", "open_memstream", "scandir")
# A call, not a member (x.malloc, x->malloc) or part of a longer name.
CALL = re.compile(r"(?<![\w.$])(?<!->)\b(" + "|".join(ALLOCATORS + ("realpath", "getline", "getdelim"))
                  + r")\s*\(")
NULL_ARGUMENT = {"NULL", "0", "nullptr", "(void *)0", "((void *)0)"}


def blank(text: str) -> str:
    """The text with comments, string literals and character literals
    replaced by spaces, newlines kept, so offsets and lines do not move."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + (c if j - 1 > i else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def arguments(code: str, open_paren: int) -> tuple[list[str], int]:
    """The top-level arguments of the call whose '(' is at `open_paren`,
    and the offset just past its ')'."""
    depth, start, parts = 0, open_paren + 1, []
    for i in range(open_paren, len(code)):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                parts.append(code[start:i].strip())
                return parts, i + 1
        elif c == "," and depth == 1:
            parts.append(code[start:i].strip())
            start = i + 1
    return parts, len(code)


def statement_before(code: str, offset: int) -> str:
    """The statement that ends just before the one containing `offset`."""
    begin = max(code.rfind(c, 0, offset) for c in ";{}")
    if begin < 0 or code[begin] != ";":
        return ""
    previous = max(code.rfind(c, 0, begin) for c in ";{}")
    return " ".join(code[previous + 1:begin].split())


def function_end(code: str, offset: int) -> int:
    """The offset of the '}' that closes the top-level block around `offset`."""
    depth = code.count("{", 0, offset) - code.count("}", 0, offset)
    for i in range(offset, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(code)


def getline_follows_policy(code: str, start: int, end: int) -> bool:
    # In a loop condition, an `errno = 0;` before the loop clears it once,
    # not before each call.
    begin = max(code.rfind(c, 0, start) for c in ";{}")
    if re.match(r"\s*(while|for|do)\b", code[begin + 1:start]):
        return False
    if statement_before(code, start) != "errno = 0":
        return False
    return re.search(r"\bENOMEM\b", code[end:function_end(code, end)]) is not None


def scan(text: str) -> list[tuple[int, str]]:
    """(line, call) for each rejected call in one source file."""
    code = blank(text)
    found = []
    for match in CALL.finditer(code):
        name = match[1]
        args, end = arguments(code, match.end() - 1)
        if name == "realpath":
            if len(args) < 2 or args[1] not in NULL_ARGUMENT:
                continue
        elif name in ("getline", "getdelim"):
            if getline_follows_policy(code, match.start(), end):
                continue
        found.append((code.count("\n", 0, match.start()) + 1, name))
    return found


def sources(root: Path = ROOT) -> list[Path]:
    files = []
    for top in SCOPE:
        for path in sorted((root / top).rglob("*")):
            relative = path.relative_to(root)
            if path.suffix not in (".c", ".h") or "tests" in relative.parts:
                continue
            if relative.as_posix() not in EXEMPT:
                files.append(path)
    return files


def collect(root: Path = ROOT) -> dict[str, list[tuple[int, str]]]:
    sites = {}
    for path in sources(root):
        found = scan(path.read_text(encoding="utf-8", errors="replace"))
        if found:
            sites[path.relative_to(root).as_posix()] = found
    return sites


def counts(sites: dict[str, list[tuple[int, str]]]) -> Counter:
    return Counter((path, name) for path, found in sites.items() for _, name in found)


def read_baseline(text: str) -> Counter:
    baseline = Counter()
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3 or not parts[2].isdigit() or int(parts[2]) <= 0:
            raise ValueError(f"legacy-sites.txt:{number}: expected '<path> <call> <count>', got {line!r}")
        if (parts[0], parts[1]) in baseline:
            raise ValueError(f"legacy-sites.txt:{number}: {parts[0]} {parts[1]} listed twice")
        baseline[(parts[0], parts[1])] = int(parts[2])
    return baseline


def write_baseline(found: Counter) -> str:
    lines = [
        "# Direct allocations still to migrate to common/alloc/alloc.h",
        "# (docs/memory-allocation-policy.md, \"Enforcement\"): <path> <call> <count>.",
        "# Checked by scripts/check-allocations.py. It only shrinks, down to empty.",
    ]
    lines += [f"{path} {name} {count}" for (path, name), count in sorted(found.items())]
    return "\n".join(lines) + "\n"


def check(sites: dict[str, list[tuple[int, str]]], baseline: Counter) -> list[str]:
    failures = []
    found = counts(sites)
    for key in sorted(set(found) | set(baseline)):
        path, name = key
        now, allowed = found[key], baseline[key]
        if now > allowed:
            where = ", ".join(str(line) for line, call in sites[path] if call == name)
            failures.append(f"{path}: {now} {name} (lines {where}), {allowed} allowed by the baseline: "
                            f"use the helpers of common/alloc/alloc.h")
        elif now < allowed:
            failures.append(f"{path}: {now} {name}, {allowed} in the baseline: lower it in "
                            f"{BASELINE.relative_to(ROOT)}")
    return failures


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--write-baseline", action="store_true",
                        help="rewrite the baseline from the current sources")
    options = parser.parse_args(argv)
    sites = collect()
    if options.write_baseline:
        BASELINE.write_text(write_baseline(counts(sites)), encoding="utf-8")
        return 0
    try:
        baseline = read_baseline(BASELINE.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"check-allocations: {error}", file=sys.stderr)
        return 1
    failures = check(sites, baseline)
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
