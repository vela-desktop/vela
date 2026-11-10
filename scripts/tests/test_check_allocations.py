# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""scripts/check-allocations.py: every rejected form is reported, none of the
allowed ones is, and the baseline only shrinks."""
import importlib.util
from collections import Counter
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("allocations", Path(__file__).parents[1] / "check-allocations.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

# One call of every rejected form; the comment names what must be reported.
REJECTED = r"""
#include <stdio.h>
#include <stdlib.h>

void rejected(FILE *file, const char *path, int count)
{
    struct item *one = malloc(sizeof(*one)); // reject: malloc
    struct item *many = calloc(count, sizeof(*many)); // reject: calloc
    many = realloc(many, 2 * count * sizeof(*many)); // reject: realloc
    many = reallocarray (many, count, sizeof(*many)); // reject: reallocarray
    char *a = strdup(path); // reject: strdup
    char *b = strndup(path, 4); // reject: strndup
    char *c;
    asprintf(&c, "%s", path); // reject: asprintf
    vasprintf(&c, "%s", args); // reject: vasprintf
    void *aligned;
    posix_memalign(&aligned, 64, 4096); // reject: posix_memalign
    aligned = aligned_alloc(64, 4096); // reject: aligned_alloc
    FILE *memory = open_memstream(&c, &size); // reject: open_memstream
    struct dirent **names;
    scandir(path, &names, NULL, alphasort); // reject: scandir
    char *resolved = realpath(path, NULL); // reject: realpath
    resolved = realpath(path,
        NULL); // reject: realpath
    char *line = NULL;
    size_t capacity = 0;
    // No errno = 0 before the call.
    while (getline(&line, &capacity, file) > 0) { // reject: getline
    }
    // Cleared once before a loop condition, not before each call.
    errno = 0;
    while (getdelim(&line, &capacity, ';', file) > 0) { // reject: getdelim
    }
    if (errno == ENOMEM) {
        abort();
    }
}

void cleared_but_not_checked(FILE *file)
{
    char *line = NULL;
    size_t capacity = 0;
    for (;;) {
        errno = 0;
        ssize_t n = getline(&line, &capacity, file); // reject: getline
        if (n < 0) {
            break;
        }
    }
}
"""

# The allowed forms: the helpers, realpath with a buffer, the policy's
# getline loop, members and longer names, and the names in comments and
# strings.
ALLOWED = r"""
#include "alloc/alloc.h"

// malloc(1), calloc(1, 2), realloc(p, 3) and strdup(s) in a comment.
/* A block comment: getline(&line, &capacity, file);
   realpath(path, NULL); */

static void *my_malloc(size_t size);
struct allocator { void *(*malloc)(size_t); void *(*realloc)(void *, size_t); };

void allowed(FILE *file, const char *path, struct allocator *allocator, struct allocator table)
{
    char *copy = vela_strdup(path);
    copy = vela_strndup(path, 3);
    copy = vela_asprintf("%s %s", "strdup(", "malloc(4)");
    void *block = vela_alloc(4, 4);
    block = my_malloc(8);
    block = allocator->malloc(8);
    block = table.realloc(block, 16);
    char resolved[PATH_MAX];
    realpath(path, resolved);
    char quote = '"';
    const char *text = "calloc(1, 1) \" realloc(p, 2)";
    char *line = NULL;
    size_t capacity = 0;
    for (;;) {
        errno = 0;
        ssize_t n = getline(&line, &capacity, file);
        if (n < 0) {
            break;
        }
    }
    int error = errno;
    if (error == ENOMEM) {
        vela_oom(capacity, 1, __FILE__, __LINE__);
    }
}
"""


def expected(text: str) -> list[tuple[int, str]]:
    """The (line, call) pairs the fixture's comments ask for. A marker on a
    continuation line belongs to the call that starts on an earlier line."""
    found = []
    lines = text.split("\n")
    for number, line in enumerate(lines, 1):
        marker = re.search(r"// reject: (\w+)", line)
        if not marker:
            continue
        call = marker[1]
        start = number
        while not re.search(rf"\b{call}\s*\(", lines[start - 1]):
            start -= 1
        found.append((start, call))
    return found


class Scan(unittest.TestCase):
    def test_every_rejected_form_is_reported(self):
        wanted = expected(REJECTED)
        self.assertEqual(len(wanted), 17)
        self.assertEqual(sorted(m.scan(REJECTED)), sorted(wanted))
        # Every rejected name has a fixture.
        names = {call for _, call in wanted}
        self.assertEqual(names, set(m.ALLOCATORS) | {"realpath", "getline", "getdelim"})

    def test_no_allowed_form_is_reported(self):
        self.assertEqual(m.scan(ALLOWED), [])

    def test_blanking_keeps_lines(self):
        text = 'a /* x\ny */ "q\\"uote" // c\nb'
        blanked = m.blank(text)
        self.assertEqual(len(blanked), len(text))
        self.assertEqual(blanked.count("\n"), 2)
        self.assertNotIn("uote", blanked)


class Baseline(unittest.TestCase):
    SITES = {"compositor/src/a.c": [(3, "calloc"), (9, "calloc"), (12, "strdup")]}

    def test_matching_counts_pass(self):
        baseline = Counter({("compositor/src/a.c", "calloc"): 2, ("compositor/src/a.c", "strdup"): 1})
        self.assertEqual(m.check(self.SITES, baseline), [])

    def test_a_new_site_fails_with_its_lines(self):
        baseline = Counter({("compositor/src/a.c", "calloc"): 1, ("compositor/src/a.c", "strdup"): 1})
        failures = m.check(self.SITES, baseline)
        self.assertEqual(len(failures), 1)
        self.assertIn("compositor/src/a.c: 2 calloc (lines 3, 9), 1 allowed", failures[0])

    def test_a_migrated_site_asks_to_lower_the_baseline(self):
        baseline = Counter({("compositor/src/a.c", "calloc"): 2, ("compositor/src/a.c", "strdup"): 1,
                            ("compositor/src/gone.c", "malloc"): 1})
        failures = m.check(self.SITES, baseline)
        self.assertEqual(failures, ["compositor/src/gone.c: 0 malloc, 1 in the baseline: "
                                    "lower it in common/alloc/legacy-sites.txt"])

    def test_the_baseline_round_trips(self):
        found = Counter({("b.c", "strdup"): 2, ("a.c", "calloc"): 1})
        self.assertEqual(m.read_baseline(m.write_baseline(found)), found)

    def test_a_malformed_baseline_is_an_error(self):
        for text in ("a.c calloc\n", "a.c calloc 0\n", "a.c calloc 1\na.c calloc 2\n"):
            with self.assertRaises(ValueError):
                m.read_baseline(text)


class Repository(unittest.TestCase):
    def test_the_sources_match_the_baseline(self):
        baseline = m.read_baseline(m.BASELINE.read_text(encoding="utf-8"))
        self.assertEqual(m.check(m.collect(), baseline), [])

    def test_scope_skips_tests_and_the_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("compositor/src/a.c", "compositor/tests/b.c", "common/alloc/alloc.c",
                             "updates/src/c.h", "tools/d.c", "compositor/src/e.cpp"):
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                (root / relative).write_text("void f(void) { free(malloc(1)); }\n")
            self.assertEqual(sorted(m.collect(root)), ["compositor/src/a.c", "updates/src/c.h"])


if __name__ == "__main__":
    unittest.main()
