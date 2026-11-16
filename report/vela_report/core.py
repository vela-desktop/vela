# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

import dataclasses
import contextlib
import datetime as dt
import getpass
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import shutil
import socket
import stat
import subprocess
import tempfile
import time
import uuid
import zipfile

from . import VERSION

TEXT_LIMIT = 2 * 1024 * 1024
LOG_LIMIT = 64 * 1024 * 1024
ATTACHMENT_LIMIT = 256 * 1024 * 1024
ARCHIVE_LIMIT = 1024 * 1024 * 1024
SECRET_KEY = r"(?:password|passwd|(?:access[_-]?|refresh[_-]?|auth[_-]?)?token|secret|api[_-]?key|authorization)"


def timestamp():
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


class Redactor:
    """Redact exported copies, including report metadata; never modify sources."""

    def __init__(self, home=None, user=None, host=None):
        self.home = str(home or Path.home())
        self.user = user if user is not None else getpass.getuser()
        self.host = host if host is not None else socket.gethostname()

    def text(self, value):
        text = str(value)
        if self.home != "/":
            text = text.replace(self.home, "<home>")
        for personal, replacement in ((self.user, "<user>"), (self.host, "<host>")):
            if personal:
                text = re.sub(r"(?<![\w-])" + re.escape(personal) + r"(?![\w-])",
                              replacement, text)
        text = re.sub(r'(?i)("' + SECRET_KEY + r'"\s*:\s*)"(?:\\.|[^"\\])*"',
                      r'\1"<redacted>"', text)
        text = re.sub(r"(?i)(?<![\w\"])(" + SECRET_KEY + r")"
                      r"(\s*[:=]\s*)([^\n,;]+)", r"\1\2<redacted>", text)
        text = re.sub(r"(?i)\bBearer\s+[A-Za-z0-9._~+/=-]+", "Bearer <redacted>", text)
        text = re.sub(r"(https?://)[^/\s:@]+:[^/\s@]+@", r"\1<redacted>@", text)
        text = re.sub(r"(?im)^(\s*(?:serial(?: number)?|uuid)\s*[:=]\s*).+$",
                      r"\1<redacted>", text)
        return text

    def value(self, value):
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, dict):
            return {self.text(k): "<redacted>" if re.fullmatch(SECRET_KEY, k, re.I)
                    else self.value(v) for k, v in value.items()}
        if isinstance(value, (list, tuple)):
            return [self.value(v) for v in value]
        return value


@dataclasses.dataclass
class Result:
    status: str
    output: str = ""
    reason: str = ""
    returncode: int | None = None


def run_command(args, timeout=10, limit=TEXT_LIMIT, cwd=None):
    """Bound both pipes and execution time, including forked vendor helpers."""
    try:
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   cwd=cwd, start_new_session=True)
    except FileNotFoundError:
        return Result("unavailable", reason=f"{args[0]} is not installed.")
    except PermissionError:
        return Result("permission_denied", reason=f"Cannot execute {args[0]}.")
    output = bytearray()
    status, reason = "collected", ""
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            selector.register(process.stderr, selectors.EVENT_READ)
            deadline = time.monotonic() + timeout
            while selector.get_map():
                if time.monotonic() >= deadline:
                    status, reason = "timed_out", f"Command exceeded {timeout} seconds."
                    break
                for key, _ in selector.select(min(0.1, max(0, deadline - time.monotonic()))):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    available = limit - len(output)
                    output.extend(chunk[:available])
                    if len(chunk) > available:
                        status, reason = "partial", f"Command output exceeded {limit} bytes."
                        break
                if status != "collected":
                    break
            if status == "collected":
                try:
                    process.wait(timeout=max(0.01, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    status, reason = "timed_out", f"Command exceeded {timeout} seconds."
    finally:
        # Terminate descendants too, even if the immediate child has exited.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        process.stdout.close()
        process.stderr.close()
    text = output.decode("utf-8", errors="replace")
    if status == "collected" and process.returncode != 0:
        denied = re.search(r"permission denied|not permitted|access denied", text, re.I)
        status = "permission_denied" if denied else "failed"
        reason = f"Command exited with status {process.returncode}."
    return Result(status, text, reason, process.returncode)


def read_regular(path, limit=TEXT_LIMIT, binary=False, bookends=False):
    """Open a regular file without following a substituted symlink or a FIFO."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("Only regular files can be included; symlinks are not followed.")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            if bookends and before.st_size > limit:
                marker = b"\n[vela-report: middle of oversized log omitted; startup and latest messages retained]\n"
                head_size = min(65536, (limit - len(marker)) // 4)
                tail_size = limit - head_size - len(marker)
                if head_size < 0 or tail_size < 0:
                    raise ValueError("Log limit is too small for the omission marker.")
                head = stream.read(head_size)
                stream.seek(before.st_size - tail_size)
                data = head + marker + stream.read(tail_size)
                truncated = True
            else:
                data = stream.read(limit + 1)
                truncated = len(data) > limit
        after = os.fstat(fd)
        changed = (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns)
        metadata = {"captured_at": timestamp(), "size_at_start": before.st_size,
                    "mtime_ns": before.st_mtime_ns, "changed_during_copy": changed,
                    "truncated": truncated, "byte_limit": limit,
                    "device": before.st_dev, "inode": before.st_ino,
                    "retained": "head_and_tail" if bookends and truncated else "complete" if not truncated else "head"}
        data = data[:limit]
        return (data if binary else data.decode("utf-8", errors="replace")), metadata
    finally:
        os.close(fd)


@dataclasses.dataclass
class Environment:
    proc: Path = dataclasses.field(default_factory=lambda: Path("/proc"))
    sys: Path = dataclasses.field(default_factory=lambda: Path("/sys"))
    etc: Path = dataclasses.field(default_factory=lambda: Path("/etc"))
    home: Path = dataclasses.field(default_factory=Path.home)
    env: dict = dataclasses.field(default_factory=lambda: dict(os.environ))
    uid: int = dataclasses.field(default_factory=os.getuid)

    def xdg(self, key, fallback):
        return Path(self.env.get(key) or self.home / fallback)

    def state_dir(self):
        return self.xdg("XDG_STATE_HOME", ".local/state") / "vela"

    def config_dir(self):
        return self.xdg("XDG_CONFIG_HOME", ".config") / "vela"


class Report:
    def __init__(self, environment=None, build=None):
        self.environment = environment or Environment()
        self.redactor = Redactor(home=self.environment.home)
        self.temporary = tempfile.TemporaryDirectory(prefix="vela-report-")
        self.directory = Path(self.temporary.name)
        self.id = uuid.uuid4().hex[:12]
        self.created = timestamp()
        self.build = build or {}
        self.description = {}
        self.context = {}
        self.plan = {}
        self.results = {}
        self.artifacts = {}
        self.observations = []
        self.attachment_index = 0

    def close(self):
        self.temporary.cleanup()

    @contextlib.contextmanager
    def _artifact_parent(self, name, create=False):
        """Walk archive components from the private staging directory, never symlinks."""
        if not isinstance(name, str):
            raise ValueError("Unsafe archive path.")
        parts = name.split("/")
        if any(part in ("", ".", "..") or any(ord(c) < 32 or ord(c) == 127 for c in part)
               for part in parts):
            raise ValueError("Unsafe archive path.")
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        parent = os.open(self.directory, flags)
        try:
            for component in parts[:-1]:
                if create:
                    try:
                        os.mkdir(component, mode=0o700, dir_fd=parent)
                    except FileExistsError:
                        pass
                child = os.open(component, flags, dir_fd=parent)
                os.close(parent)
                parent = child
            yield parent, parts[-1]
        finally:
            os.close(parent)

    @contextlib.contextmanager
    def _open_artifact(self, name):
        with self._artifact_parent(name) as (parent, leaf):
            fd = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC,
                         dir_fd=parent)
        try:
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                raise ValueError("Staged evidence must be a regular file.")
            with os.fdopen(fd, "rb", closefd=False) as stream:
                yield stream
        finally:
            os.close(fd)

    def add_text(self, collection, name, text, source=None, metadata=None):
        return self.add_bytes(collection, name, self.redactor.text(text).encode(),
                              "text", False, source, metadata)

    def add_json(self, collection, name, value, source=None, metadata=None):
        data = (json.dumps(self.redactor.value(value), indent=2) + "\n").encode()
        return self.add_bytes(collection, name, data, "text", False, source, metadata)

    def add_bytes(self, collection, name, data, kind, sensitive, source=None, metadata=None):
        if sum(item["size"] for key, item in self.artifacts.items() if key != name) + len(data) > ARCHIVE_LIMIT:
            raise ValueError("Included evidence exceeds the 1 GiB report limit. Remove a file first.")
        with self._artifact_parent(name, create=True) as (parent, leaf):
            fd = os.open(leaf, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW
                         | os.O_NONBLOCK | os.O_CLOEXEC, 0o600, dir_fd=parent)
        with os.fdopen(fd, "wb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise ValueError("Staged evidence must be a regular file.")
            os.fchmod(stream.fileno(), 0o600)
            stream.write(data)
        self.artifacts[name] = {"collection": collection, "size": len(data), "kind": kind,
                                "sensitive": sensitive,
                                "source": self.redactor.text(source) if source else None,
                                "metadata": self.redactor.value(metadata or {})}
        return name

    def attach(self, path, kind="other"):
        if kind not in {"text", "update", "screenshot", "video", "core", "vendor", "other"}:
            raise ValueError("Unknown attachment type.")
        source = Path(path).expanduser().absolute()
        # Never silently truncate a dump/video: either include the whole file or reject it.
        info = source.lstat()
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Attachments must be regular files, not symlinks.")
        if info.st_size > ATTACHMENT_LIMIT:
            raise ValueError(f"Attachment exceeds the {ATTACHMENT_LIMIT // (1024 * 1024)} MiB limit.")
        text = kind in ("text", "update")
        data, metadata = read_regular(source, ATTACHMENT_LIMIT, binary=not text)
        if metadata["truncated"] or metadata["changed_during_copy"]:
            raise ValueError("Attachment changed during copying; select it again when it is stable.")
        # Names deliberately avoid exporting personal filenames.
        self.attachment_index += 1
        index = self.attachment_index
        allowed = {".txt", ".log", ".json", ".jsonl", ".yaml", ".yml", ".ini", ".conf", ".csv",
                   ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".mp4", ".mkv", ".webm", ".mov",
                   ".avi", ".core", ".dump", ".gz", ".zip", ".tgz", ".xz", ".zst", ".bz2", ".bin", ".dmp"}
        suffix = source.suffix.lower() if source.suffix.lower() in allowed else ""
        for compound in (".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".log.gz"):
            if source.name.lower().endswith(compound):
                suffix = compound
                break
        name = f"attachments/{index:03d}-{kind}{suffix}"
        if text:
            self.add_text("attachments", name, data, str(source), metadata)
        else:
            self.add_bytes("attachments", name, data, kind, True, str(source), metadata)
        self.results["attachments"] = {"status": "collected", "reason": "",
                                        "captured_at": timestamp()}
        self.plan["attachments"] = True
        return name

    def remove(self, name):
        item = self.artifacts[name]
        with self._artifact_parent(name) as (parent, leaf):
            os.unlink(leaf, dir_fd=parent)
        del self.artifacts[name]
        collection = item["collection"]
        if not any(a["collection"] == collection for a in self.artifacts.values()):
            self.mark_excluded(collection)

    def mark_excluded(self, collection):
        self.plan[collection] = False
        self.results[collection] = {"status": "excluded", "reason": "Removed during review."}
        fields = {"logs": ["log_path", "log_session"], "runtime": ["socket"], "resources": ["processes", "duration"]}
        for key in fields.get(collection, []):
            self.context.pop(key, None)
        if collection == "resources":
            self.observations = [o for o in self.observations if o.startswith("Session log:")]
        if collection == "logs":
            self.observations = [o for o in self.observations if not o.startswith("Session log:")]

    def remove_collection(self, collection):
        for name in list(self.artifacts):
            if self.artifacts[name]["collection"] == collection:
                self.remove(name)
        self.mark_excluded(collection)

    def preview(self, name):
        if name == "report.md":
            return self.markdown()
        if name == "manifest.json":
            return json.dumps(self.manifest(), indent=2)
        item = self.artifacts[name]
        if item["kind"] != "text":
            return ("Binary attachment. Automatic text redaction does not apply.\n"
                    "Review the original file before sharing.\n" +
                    json.dumps(item, indent=2))
        with self._open_artifact(name) as stream:
            data = stream.read(65537)
        return data[:65536].decode("utf-8", errors="replace") + (
            "\n[Preview limited to 64 KiB; the archive includes the collected file.]\n"
            if len(data) > 65536 else "")

    def manifest(self):
        artifacts = {}
        for name, item in self.artifacts.items():
            digest = hashlib.sha256()
            with self._open_artifact(name) as stream:
                for block in iter(lambda: stream.read(65536), b""):
                    digest.update(block)
            artifacts[name] = dict(item, sha256=digest.hexdigest())
        return self.redactor.value({
            "schema_version": 1, "reporter_version": VERSION, "reporter_build": self.build,
            "report_id": self.id, "created_at": self.created, "exported_at": timestamp(),
            "description": self.description, "incident": self.context, "collection_plan": self.plan,
            "collectors": self.results, "artifacts": artifacts, "observations": self.observations,
            "limits": {"text_bytes": TEXT_LIMIT, "log_bytes": LOG_LIMIT, "attachment_bytes": ATTACHMENT_LIMIT,
                       "evidence_bytes": ARCHIVE_LIMIT},
            "redaction": {"applied": ["home path", "user and host names", "recognized secret assignments",
                                    "basic serial/UUID fields", "credentials in HTTP URLs"],
                          "limitations": "Best effort; review free-form text and binary attachments. "
                                         "No original-value mapping is exported."}})

    def markdown(self):
        text = ["# Vela diagnostic report", "", f"Report ID: {self.id}",
                f"Collection started: {self.created}", "", "## User description", ""]
        for key, value in self.description.items():
            text += [f"### {key.replace('_', ' ').capitalize()}", "", str(value or "Unknown"), ""]
        text += ["## Incident and provenance", "",
                 "Current system/configuration data may differ from the incident. "
                 "Unavailable historical data is not reconstructed.", "",
                 json.dumps(self.redactor.value(self.context), indent=2), "",
                 "## Collector results", ""]
        for collection, result in self.results.items():
            text.append(f"- {collection}: {result['status']}. {result.get('reason', '')}")
        text += ["", "## Observations", ""]
        text += [f"- {item}" for item in self.observations] or ["No automatic conclusions."]
        text += ["", "## Included evidence", ""]
        for name, item in self.artifacts.items():
            text.append(f"- [{name}]({name}) — {item['size']} bytes" +
                        ("; sensitive attachment, review separately" if item["sensitive"] else ""))
        text += ["", "## Privacy", "",
                 "Text copies have best-effort redaction. Images, videos, dumps and vendor "
                 "archives are not automatically redacted. Review before sharing. "
                 "Nothing was uploaded. See manifest.json for provenance and limitations.", ""]
        return self.redactor.text("\n".join(text))

    def summary(self):
        return {"report_id": self.id, "results": self.redactor.value(self.results),
                "artifacts": self.redactor.value(self.artifacts),
                "observations": self.observations,
                "default_output": f"vela-report-{dt.datetime.now():%Y%m%d-%H%M%S}-{self.id}.zip"}

    def export(self, destination, overwrite=False):
        destination = Path(destination).expanduser().absolute()
        if destination.exists() and not overwrite:
            raise FileExistsError("The destination already exists. Choose another path or confirm replacement.")
        fd, temporary = tempfile.mkstemp(prefix=".vela-report-", suffix=".zip", dir=destination.parent)
        try:
            with os.fdopen(fd, "w+b") as stream:
                with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                    archive.writestr("report.md", self.markdown())
                    archive.writestr("manifest.json", json.dumps(self.manifest(), indent=2) + "\n")
                    for name in self.artifacts:
                        with self._open_artifact(name) as source, archive.open(name, "w") as target:
                            shutil.copyfileobj(source, target)
                stream.flush()
                os.fsync(stream.fileno())
            if overwrite:
                os.replace(temporary, destination)
            else:
                # Atomic no-clobber publication, even if another process creates the destination.
                os.link(temporary, destination)
                os.unlink(temporary)
            return {"path": str(destination), "size": destination.stat().st_size}
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
