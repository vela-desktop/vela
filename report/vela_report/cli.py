# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from .collectors import CATEGORIES, Collector, SPECS, discover, recommended
from .core import Report, read_regular

PRIVACY = ("Text redaction is best effort. Review logs and descriptions before sharing. "
           "Images, videos, dumps and vendor archives are not automatically redacted. "
           "Nothing is uploaded; no settings are changed.")


def display(text):
    # Untrusted log contents must not issue terminal control sequences.
    return "".join(c if c in "\n\t" or c.isprintable()
                   else f"[0x{ord(c):02x}]" for c in str(text))


def ask(prompt, default=""):
    answer = input(display(prompt) + (f" [{default}]" if default else "") + ": ").strip()
    return answer or default


def yes(prompt, default=False):
    while True:
        answer = ask(prompt + " (yes/no)", "yes" if default else "no").lower()
        if answer in ("yes", "y"):
            return True
        if answer in ("no", "n"):
            return False
        print("Please enter yes or no.")


def multiline(prompt):
    print(prompt + " Finish with a line containing only a dot; a dot immediately skips.")
    lines = []
    while (line := input()) != ".":
        lines.append(line)
    return "\n".join(lines)


def choose(prompt, choices, default=0):
    print(display(prompt))
    for index, label in enumerate(choices):
        print(display(f"  {index + 1}. {label}"))
    while True:
        try:
            result = int(ask("Choice", str(default + 1))) - 1
            if 0 <= result < len(choices):
                return result
        except ValueError:
            pass
        print("Please choose one of the listed numbers.")


def description(initial=None):
    data = initial or {}
    data["title"] = ask("Short title", data.get("title", ""))
    data["category"] = CATEGORIES[choose("What kind of problem is this?", CATEGORIES,
                                        CATEGORIES.index(data.get("category", "Other")))]
    data["what_happened"] = multiline("What happened?")
    data["expected"] = multiline("What did you expect?")
    data["steps"] = multiline("How can it be reproduced? Include the applications/actions involved.")
    data["frequency"] = ask("How often does it happen?", data.get("frequency", "I don't know"))
    data["changed_since_incident"] = ask("Have you updated Vela or changed settings since the incident?",
                                        data.get("changed_since_incident", "I don't know"))
    return data


def attachment(report):
    path = ask("File to include (empty cancels)")
    if not path:
        return
    kinds = ["text", "update", "screenshot", "video", "core", "vendor", "other"]
    kind = kinds[choose("What does this file contain? Choose text only for a plain-text file.",
                        kinds, default=len(kinds) - 1)]
    print(PRIVACY)
    if kind == "core":
        print("Core dumps contain process memory and may include documents, messages or credentials.")
    if not yes("Include this specific file?", False):
        return
    try:
        name = report.attach(path, kind)
        print(f"Added {name}.")
    except (OSError, ValueError) as error:
        print(display(f"Cannot include file: {error}"))


def review(report, output=None):
    while True:
        print("\nReview before export\n" + PRIVACY)
        summary = report.summary()
        for key, result in summary["results"].items():
            print(display(f"  {key}: {result['status']} — {result.get('reason', '')}"))
        names = ["report.md", "manifest.json", *report.artifacts]
        for index, name in enumerate(names, 1):
            item = report.artifacts.get(name, {})
            print(f"  {index}. {name} ({item.get('size', 'generated')} bytes)" +
                  (" — sensitive; review original" if item.get("sensitive") else ""))
        print("Commands: view NUMBER, remove NUMBER, exclude COLLECTION, edit, attach, export, cancel")
        action = ask("Review action", "view 1").split(maxsplit=1)
        if not action:
            continue
        if action[0] == "cancel":
            raise KeyboardInterrupt
        if action[0] == "edit":
            report.description = description(dict(report.description))
        elif action[0] == "attach":
            attachment(report)
        elif action[0] in ("view", "remove") and len(action) == 2:
            try:
                index = int(action[1])
                if not 1 <= index <= len(names):
                    raise ValueError
                name = names[index - 1]
                if action[0] == "view":
                    print(display(report.preview(name)))
                elif name in report.artifacts:
                    report.remove(name)
                else:
                    print("The report and manifest are required. Remove a collection instead.")
            except (ValueError, KeyError):
                print("Choose a valid file number.")
        elif action[0] == "exclude" and len(action) == 2:
            if action[1] in report.results:
                report.remove_collection(action[1])
            else:
                print("Unknown collection.")
        elif action[0] == "export":
            destination = ask("ZIP output path", output or summary["default_output"])
            overwrite = False
            if Path(destination).expanduser().exists():
                overwrite = yes("This file exists. Replace it?", False)
                if not overwrite:
                    continue
            if not yes("Create the ZIP with the reviewed contents?", False):
                continue
            try:
                result = report.export(destination, overwrite)
            except (OSError, ValueError) as error:
                print(display(f"Export failed: {error}. You can choose another path or cancel."))
                continue
            print(display(f"Report created: {result['path']} ({result['size']} bytes).\n"
                          "You can send this file to the Vela maintainer. Nothing was uploaded."))
            return 0
        else:
            print("Choose a listed review command.")


def interactive(options, build):
    report = Report(build=build)
    try:
        print("Vela Report\nCreate a local diagnostic ZIP with the evidence you choose.\n" + PRIVACY)
        report.description = description()
        if options.description_file:
            text, _ = read_regular(Path(options.description_file).expanduser())
            report.description["additional_description"] = text
        info = discover(report.environment, options.state_dir)
        paths = [None, *[item["path"] for item in info["logs"]]]
        labels = ["No retained log / unknown session",
                  *[f"{item['label']} — modified {item['modified']}, {item['bytes']} bytes: {item['path']}"
                    for item in info["logs"]], "Choose a different log file"]
        if options.log:
            log_path = str(Path(options.log).expanduser().absolute())
        else:
            index = choose("Which Vela session are you reporting?", labels, 1 if info["logs"] else 0)
            log_path = ask("Incident log path") if index == len(labels) - 1 else paths[index]
        context = {"log_path": log_path, "live_during_incident": ask("Is the affected Vela session still running?",
                                                                     "I don't know")}
        while True:
            incident = ask("Incident time: ISO date/time with timezone, or empty if unknown",
                           options.incident_time or "")
            try:
                if incident and dt.datetime.fromisoformat(incident).tzinfo is None:
                    raise ValueError
                context["incident_time"] = incident
                break
            except ValueError:
                print("Example: 2026-10-08T13:20:00+02:00. Include a timezone offset.")
        context["boot"] = ask("Journal boot: auto=match selected session, 0=current, -1=previous, or boot ID",
                              options.boot)
        context["minutes_before"] = getattr(options, "minutes_before", 10)
        context["minutes_after"] = getattr(options, "minutes_after", 5)
        defaults = recommended(report.description["category"])
        for key, title, help_text, privacy, seconds in SPECS:
            marker = "recommended" if key in defaults else "optional"
            print(f"  {title} ({marker}): {help_text}\n    Privacy: {privacy}")
        selected = set(defaults)
        if yes("Customize the proposed collections?", False):
            selected = set()
            for key, title, help_text, privacy, seconds in SPECS:
                if key == "vendor" and not info["vendor_available"]:
                    continue
                print(f"\n{title}\n{help_text}\nPrivacy: {privacy}")
                if yes(f"Include {title}?", key in defaults):
                    selected.add(key)
        if "vendor" in selected:
            print("NVIDIA's report is unredacted and may contain additional system details. "
                  "It runs as your user; inaccessible data will be missing. You can instead attach an existing report.")
            if not yes("Generate and review this NVIDIA report?", False):
                selected.remove("vendor")
        if selected & {"session_journal", "kernel_journal", "crashes"}:
            print("Journal window: " + str(context["minutes_before"]) + " minutes before and " +
                  str(context["minutes_after"]) + " minutes after the incident, capped at now. "
                  "Unknown incident time uses the selected log's last write, or the whole selected boot.")
        if selected & {"runtime", "brightness"}:
            index = choose("Select the live Vela instance; the host desktop is not assumed to be Vela.",
                           ["No live instance", *info["sockets"]])
            context["socket"] = info["sockets"][index - 1] if index else None
        if "resources" in selected:
            print("Select process numbers separated by commas. Each process is recorded separately.")
            for index, process in enumerate(info["processes"], 1):
                print(f"  {index}. {process['role']} — PID {process['pid']}")
            while True:
                try:
                    choice = ask("Processes (empty skips)")
                    indices = [int(i.strip()) - 1 for i in choice.split(",")] if choice else []
                    if any(i < 0 or i >= len(info["processes"]) for i in indices):
                        raise ValueError
                    context["processes"] = [info["processes"][i] for i in indices]
                    duration = int(ask("Recording duration in seconds (1–300)", str(options.duration)))
                    if not 1 <= duration <= 300:
                        raise ValueError
                    context["duration"] = duration
                    break
                except ValueError:
                    print("Choose valid process numbers and a duration from 1 to 300.")
        print("\nSelected: " + (", ".join(sorted(selected)) or "description and manifest only"))
        if not yes("Accept this collection plan and start?", False):
            return 130
        if "resources" in selected:
            print("Repeat the steps that trigger the problem during the recording countdown. "
                  "Press Ctrl+C to stop recording and keep the samples.")
        Collector(report).collect(selected, context, report.description,
                                  progress=lambda message: print(message, flush=True))
        return review(report, options.output)
    except (KeyboardInterrupt, EOFError):
        print("\nReport cancelled. Temporary data removed.")
        return 130
    finally:
        report.close()


def worker(build):
    """Private JSON-lines bridge. Only fixed read-only collectors are exposed."""
    report = Report(build=build)

    def send(value):
        print(json.dumps(value), flush=True)

    try:
        for line in sys.stdin:
            try:
                request = json.loads(line)
                action = request["action"]
                if action == "discover":
                    result = discover(report.environment, request.get("state_dir"))
                elif action == "collect":
                    if report.results:
                        report.close()
                        report = Report(build=build)
                    if request.get("description_file"):
                        text, _ = read_regular(Path(request["description_file"]).expanduser())
                        request["description"]["additional_description"] = text
                    result = Collector(report).collect(request["selected"], request["context"],
                              request["description"], progress=lambda message: send({"event": "progress", "message": message}))
                elif action == "preview":
                    result = {"text": report.preview(request["name"])}
                elif action == "remove":
                    report.remove(request["name"])
                    result = report.summary()
                elif action == "exclude":
                    report.remove_collection(request["collection"])
                    result = report.summary()
                elif action == "edit":
                    report.description = request["description"]
                    result = report.summary()
                elif action == "attach":
                    report.attach(request["path"], request["kind"])
                    result = report.summary()
                elif action == "export":
                    result = report.export(request["path"], request.get("overwrite", False))
                elif action == "close":
                    break
                else:
                    raise ValueError("Unknown action.")
                send({"event": "result", "action": action, "result": result})
            except (OSError, ValueError, KeyError, TypeError) as error:
                send({"event": "error", "message": report.redactor.text(str(error))})
    finally:
        report.close()
    return 0


def main(argv=None, gui_path=None, build=None):
    parser = argparse.ArgumentParser(description="Create a reviewed local Vela diagnostic ZIP. Nothing is uploaded.")
    frontend = parser.add_mutually_exclusive_group()
    frontend.add_argument("--cli", action="store_true", help="Use the complete guided terminal interface.")
    frontend.add_argument("--gui", action="store_true", help="Use the standalone graphical interface.")
    parser.add_argument("--output", help="Destination ZIP path; existing files require confirmation.")
    parser.add_argument("--state-dir", help="Directory containing retained vela.log and vela.log.old.")
    parser.add_argument("--log", help="Select an incident log file explicitly.")
    parser.add_argument("--description-file", help="Include a text description file in the reviewed report.")
    parser.add_argument("--incident-time", help="Incident ISO timestamp including its timezone offset.")
    parser.add_argument("--boot", default="auto", help="Journal boot: auto matches the selected session; also accepts 0, a negative index, or a boot ID.")
    parser.add_argument("--duration", type=int, default=60, help="Resource recording duration (1–300 seconds).")
    parser.add_argument("--minutes-before", type=int, default=10, help="Journal window before the incident (0–120 minutes).")
    parser.add_argument("--minutes-after", type=int, default=5, help="Journal window after the incident (0–120 minutes).")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    options = parser.parse_args(argv)
    if not 1 <= options.duration <= 300:
        parser.error("--duration must be between 1 and 300 seconds.")
    if not 0 <= options.minutes_before <= 120 or not 0 <= options.minutes_after <= 120:
        parser.error("Journal windows must be between 0 and 120 minutes.")
    build = build or json.loads(os.environ.get("VELA_REPORT_BUILD", "{}"))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(130))
    if options.worker:
        return worker(build)
    graphical = bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))
    helper = gui_path or os.environ.get("VELA_REPORT_GUI", "")
    if not options.cli and (options.gui or graphical) and helper and Path(helper).is_file():
        try:
            probe = subprocess.run([helper, "--probe"], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, timeout=10)
            if probe.returncode == 0:
                env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parent.parent),
                           VELA_REPORT_BUILD=json.dumps(build))
                process = subprocess.Popen([helper, "--python", sys.executable, "--options-json",
                                            json.dumps(vars(options))], env=env)
                try:
                    return process.wait()
                except (KeyboardInterrupt, SystemExit):
                    process.terminate()
                    try:
                        process.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    return 130
        except (OSError, subprocess.TimeoutExpired):
            pass
    if options.gui:
        print("The graphical reporter could not start. Run vela-report --cli in a terminal.", file=sys.stderr)
        return 2
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("An interactive terminal is required. Run vela-report --cli in a terminal, "
              "or vela-report --gui in a graphical session.", file=sys.stderr)
        return 2
    try:
        return interactive(options, build)
    except (OSError, ValueError) as error:
        print(display(f"Cannot create report: {error}"), file=sys.stderr)
        return 1
