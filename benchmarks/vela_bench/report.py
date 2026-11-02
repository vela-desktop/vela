# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline archive validation and a self-contained, escaped HTML report."""

import html
import json
from pathlib import Path

from .archive import atomic_json, inspect_archive


def render_report(directory):
    directory = Path(directory)
    manifest, plan, summary = inspect_archive(directory)
    atomic_json(directory / "summary.json", summary)
    escaped = lambda value: html.escape(str(value), quote=True)
    document = """<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Vela benchmark preflight</title><style>
body{font:16px/1.5 system-ui,sans-serif;max-width:70rem;margin:3rem auto;padding:0 1rem;color:#18232b;background:#fafbfc}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#edf1f4;padding:1rem;border-radius:.4rem}
table{border-collapse:collapse;width:100%}td,th{padding:.5rem;text-align:left;border-bottom:1px solid #ccd4dc}
</style><h1>Vela benchmark preflight</h1>
<p>This archive contains host-side checks only. It contains no performance measurements.</p>"""
    document += f"<p>Run: {escaped(manifest['run_id'])}. Check: {escaped(summary['check_status'])}.</p>"
    document += "<table><thead><tr><th>Property</th><th>Value</th></tr></thead><tbody>"
    for key in ("execution_complete", "selection_complete", "full_suite_complete", "planned_repetitions", "valid_measurement_repetitions"):
        document += f"<tr><td>{escaped(key)}</td><td>{escaped(summary[key])}</td></tr>"
    document += "</tbody></table>"
    for title, value in (("Selection and provisional budgets", plan), ("Observed environment", manifest["environment"]),
                         ("Suite identity", manifest["suite"])):
        document += f"<h2>{escaped(title)}</h2><pre>{escaped(json.dumps(value, indent=2, sort_keys=True))}</pre>"
    document += "</html>\n"
    # Reports are derived output; they never modify the canonical evidence.
    (directory / "report.html").write_text(document, encoding="utf-8")
    return summary


def differences(left, right, prefix=""):
    if isinstance(left, dict) and isinstance(right, dict):
        rows = []
        for key in sorted(left.keys() | right.keys()):
            path = f"{prefix}.{key}" if prefix else key
            if key not in left or key not in right:
                rows.append({"field": path, "left": left.get(key), "right": right.get(key)})
            else:
                rows.extend(differences(left[key], right[key], path))
        return rows
    return [] if left == right else [{"field": prefix, "left": left, "right": right}]


def compare_archives(left_dir, right_dir):
    left, left_plan, left_summary = inspect_archive(left_dir)
    right, right_plan, right_summary = inspect_archive(right_dir)
    changes = []
    for name, a, b in (("environment", left["environment"], right["environment"]),
                       ("suite", left["suite"], right["suite"]),
                       ("targets", left_plan["targets"], right_plan["targets"]),
                       ("display_policy", left_plan["display_policy"], right_plan["display_policy"]),
                       ("scenario_revision", left_plan["scenario_revision"], right_plan["scenario_revision"])):
        changes.extend(differences(a, b, name))
    return {"left": left_summary["run_id"], "right": right_summary["run_id"], "differences": changes,
            "observed_metadata_match": not changes, "performance_comparison_available": False,
            "reason": "Preflight archives contain no performance measurements; matching inventory is not runtime qualification."}
