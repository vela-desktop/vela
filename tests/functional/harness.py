# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""A display-less Vela session, driven by the tests as a person would.

The compositor runs on the wlroots headless backend, in temporary
configuration directories: nothing it does touches the real session. The
windows are vela-pattern's; keyboard and mouse are vela-input's virtual
ones; the state is read with the "state" request on the command socket
(state_json in command.c).

Python standard library only. Needs a GPU with Vulkan 1.4 (Vela's renderer
has no software fallback): without one, run.py skips the tests."""

import atexit
import json
import os
import shutil
import struct
import signal
import socket
import subprocess
import tempfile
import threading
import time
import zlib

# Pillow uses a native PNG decoder and is preflight-checked on GPU runners.
# Keep the original standard-library decoder for developer/CPU environments.
try:
    from PIL import Image as PillowImage
except ImportError:
    PillowImage = None

_PROFILE = os.environ.get("VELA_FUNCTIONAL_PROFILE") == "1"
_METRICS = {}


def _profile(metric, duration):
    if _PROFILE:
        count, seconds = _METRICS.get(metric, (0, 0.0))
        _METRICS[metric] = (count + 1, seconds + duration)


@atexit.register
def _print_profile():
    if _PROFILE:
        for metric, (count, seconds) in sorted(_METRICS.items()):
            print(f"test-profile {metric}: {seconds:.3f}s / {count} calls", flush=True)


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.environ.get("VELA_BUILD", os.path.join(ROOT, "build"))

from renderer_identity import verify_renderer_vendor


def tool(name):
    return os.path.join(BUILD, "tools", name)


class TimeoutError(AssertionError):
    pass


class Session:
    """A headless compositor with its command socket.

    with Session(scale=1.25) as vela:
        window = vela.open_window()
        vela.keys("super+Left")
        vela.wait_for(lambda s: s.window(window)["snap"] == [0, 0, 6, 12])
    """

    def __init__(self, scale=1.0, size="1920x1080@60", supervise=False, lock_hold_ms=None, env=None, startup=None,
                 nofile=None, expected_exit=0):
        self.scale = scale
        self.size = size
        self.supervise = supervise
        self.lock_hold_ms = lock_hold_ms
        self.extra_env = env or {}
        self.startup = startup # the startup command (-s), like the shell
        self.nofile = nofile
        self.expected_exit = expected_exit
        self.process = None
        self.clients = []
        self.display = None
        self.directory = None
        self.shell = None
        self.log = None

    # ---------------------------------------------------------- startup --

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, kind, value, traceback):
        self.stop(keep_log=kind is not None)
        return False

    def start(self):
        """Start a session and clean up partial startup on failure."""
        began = time.perf_counter()
        try:
            try:
                self._start()
            except BaseException:
                self.stop(keep_log=True)
                raise
        finally:
            _profile("session-start", time.perf_counter() - began)

    def _start(self):
        self.directory = tempfile.mkdtemp(prefix="vela-functional-")
        env = {key: value for key, value in os.environ.items()
               if key not in ("WAYLAND_DISPLAY", "DISPLAY", "QT_WAYLAND_RECONNECT", "WAYLAND_SOCKET")}
        env.update({
            "VELA_TEST_MARK": "menu",
            "WLR_BACKENDS": "headless",
            "WLR_LIBINPUT_NO_DEVICES": "1",
            "VELA_DEBUG_INPUT": "1",
            "VELA_OUTPUT_SIZE": self.size,
            "VELA_SCALE": str(self.scale),
            "XDG_CONFIG_HOME": os.path.join(self.directory, "config"),
            "XDG_DATA_HOME": os.path.join(self.directory, "data"),
            "XDG_CACHE_HOME": os.path.join(self.directory, "cache"),
        })
        if self.lock_hold_ms is not None:
            env["VELA_LOCK"] = f"{tool('vela-testlock')} {self.lock_hold_ms}"
        env.update(self.extra_env)
        self.env = env
        command = [os.path.join(BUILD, "compositor", "vela-compositor")]
        if self.supervise:
            command.append("--supervise")
        if self.startup:
            command += ["-s", self.startup]
        if self.nofile is not None:
            # prlimit applies only to this isolated test session, including
            # its compositor after a restart; no parent limits are changed.
            command = ["prlimit", f"--nofile={self.nofile}:{self.nofile}", "--", *command]
        self.log_path = os.path.join(self.directory, "vela.log")
        self.log = open(self.log_path, "w")
        self.process = subprocess.Popen(command, env=env, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        self.display = self.wait_for_log(r"WAYLAND_DISPLAY=", lambda line: line.split("WAYLAND_DISPLAY=")[1].split()[0])
        # On hardware CI, prove which DRM render node Vela itself opened.
        if expected_vendor := os.environ.get("VELA_GPU_EXPECT_VENDOR"):
            nodes = verify_renderer_vendor(self.log_text(), expected_vendor)
            print(f"Vela renderer PCI {expected_vendor} confirmed: {', '.join(nodes)}", flush=True)
        runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        self.socket_path = os.path.join(runtime, f"vela-{self.display}.sock")
        self.wait_for(lambda _: os.path.exists(self.socket_path), what="the command socket", read_state=False)
        self.client_env = dict(env, WAYLAND_DISPLAY=self.display)
        # The first frame: the output exists.
        self.wait_for(lambda s: s["outputs"], what="an output")

    def stop(self, keep_log=False):
        validation_errors = []
        if self.shell:
            self.shell.close()
        for client in self.clients:
            if client.poll() is None:
                client.terminate()
        for client in self.clients:
            try:
                client.wait(2)
            except subprocess.TimeoutExpired:
                client.kill()
                client.wait()
        status = self.process.poll() if self.process else 0
        if status is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass # It exited between poll() and the signal; wait still records its status.
            try:
                status = self.process.wait(5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.wait()
                status = "a hang"
        if self.directory:
            if self.log:
                self.log.close()
            if self.process and self.env.get("VELA_VULKAN_VALIDATION") == "1":
                with open(self.log_path, errors="replace") as log:
                    text = log.read()
                if "Vulkan validation layers enabled" not in text:
                    validation_errors.append("Requested Vulkan validation layers were not enabled")
                validation_errors.extend(line for line in text.splitlines()
                                         if "[ERROR]" in line and "Vulkan:" in line)
            if keep_log or status != self.expected_exit:
                print(f"\n--- Vela log ({self.log_path}) ---")
                with open(self.log_path, errors="replace") as log:
                    print("".join(log.readlines()[-40:]))
            shutil.rmtree(self.directory, ignore_errors=True)
        # Asked to stop, Vela closes cleanly: a crash on the way out would look
        # like a real one to the supervisor.
        if self.process and status != self.expected_exit:
            raise AssertionError(f"vela-compositor stopped with {status} (expected {self.expected_exit})")
        if validation_errors:
            raise AssertionError("Vulkan validation failed:\n" + "\n".join(validation_errors))

    def log_text(self):
        self.log.flush()
        with open(self.log_path, errors="replace") as log:
            return log.read()

    def wait_for_log(self, needle, extract=lambda line: line, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.log_text().splitlines():
                if needle in line:
                    return extract(line)
            if self.process.poll() is not None:
                raise AssertionError(f"the compositor exited (code {self.process.returncode}):\n{self.log_text()[-3000:]}")
            time.sleep(0.05)
        raise TimeoutError(f"the log is missing «{needle}»")

    # ------------------------------------------------------- commands --

    def command(self, line):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(self.socket_path)
            connection.sendall((line + "\n").encode())

    def query(self, line):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.connect(self.socket_path)
            connection.sendall((line + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = connection.recv(65536)
                if not chunk:
                    break
                data += chunk
        return json.loads(data)

    def state(self):
        return State(self.query("state"))

    def wait_for(self, condition, timeout=5.0, what="the condition", read_state=True):
        """Waits until condition(state) is true; returns the state."""
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.state() if read_state else None
            try:
                if condition(last):
                    return last
            except (KeyError, IndexError, TypeError):
                pass
            time.sleep(0.05)
        raise TimeoutError(f"after {timeout} s this still isn't true: {what}\nlast state: {last}\n"
                           f"--- end of the Vela log ---\n{self.log_text()[-2500:]}")

    def config_path(self, name="vela.conf"):
        return os.path.join(self.env["XDG_CONFIG_HOME"], "vela", name)

    def fake_shell(self):
        """From here on Vela's messages to the shell reach a FakeShell."""
        self.shell = FakeShell(self.display)
        return self.shell

    # ------------------------------------------------ keyboard and mouse --

    def input(self, *actions):
        subprocess.run([tool("vela-input"), *map(str, actions)], env=self.client_env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def randr(self, *args):
        """Configures outputs as Settings > Display does (wlr-output-management)."""
        return subprocess.run([tool("vela-randr"), *map(str, args)], env=self.client_env, check=True,
                              capture_output=True, text=True).stdout

    def keys(self, *combos):
        actions = []
        for combo in combos:
            actions += ["key", combo, "sleep", "60"]
        self.input(*actions)

    # -------------------------------------------------------- windows --

    def open_window(self, width=400, height=300, command=None, timeout=5.0, app_id=None, decorated=False):
        """Opens a window (vela-pattern, or `command`); returns its identifier.

        decorated: with Vela's title bar (xdg-decoration)."""
        before = {w["id"] for w in self.state().windows}
        pattern = ([tool("vela-pattern")] + (["--app-id", app_id] if app_id else [])
                   + (["--decorated"] if decorated else []) + [str(width), str(height)])
        # Preserve per-client output: GPU test tools can fail before their
        # first Wayland surface is mapped, and DEVNULL hides the actual cause.
        client_log_path = os.path.join(self.directory, f"client-{len(self.clients) + 1}.log")
        with open(client_log_path, "w") as client_log:
            client = subprocess.Popen(command or pattern, env=self.client_env,
                                      stdout=client_log, stderr=subprocess.STDOUT)
        self.clients.append(client)

        def client_output():
            with open(client_log_path, errors="replace") as output:
                return output.read()[-6000:] or "(no client output)"

        def new_window(state):
            new_ids = {w["id"] for w in state.windows} - before
            if new_ids:
                return new_ids
            status = client.poll()
            if status is not None:
                raise AssertionError(
                    f"client {command or pattern!r} exited with status {status} before mapping a window\n"
                    f"--- client output ---\n{client_output()}")
            return False

        try:
            state = self.wait_for(new_window, timeout=timeout, what="a new window")
        except TimeoutError as error:
            raise TimeoutError(
                f"{error}\nclient {command or pattern!r} (pid={client.pid}, exit={client.poll()})"
                f"\n--- client output ---\n{client_output()}") from error
        identifier = next(iter({w["id"] for w in state.windows} - before))
        self.wait_still(identifier) # the opening animation is over (it rises a few pixels)
        return identifier

    def wait_still(self, identifier, timeout=3.0):
        """Waits until the window's geometry stays still for a while."""
        def geometry():
            window = self.state().window(identifier)
            return (window["x"], window["y"], window["w"], window["h"])
        deadline = time.monotonic() + timeout
        last = geometry()
        still_since = time.monotonic()
        while time.monotonic() < deadline:
            time.sleep(0.05)
            current = geometry()
            if current != last:
                last = current
                still_since = time.monotonic()
            elif time.monotonic() - still_since >= 0.3:
                return current
        raise TimeoutError(f"window {identifier} doesn't settle")

    def screenshot(self, path, output=0, region=None, cursor=False):
        """Capture upright output pixels; an optional region is (x, y, width, height)."""
        began = time.perf_counter()
        try:
            env = dict(self.client_env, VELA_SCREEN_DUMP_OUTPUT=str(output),
                       VELA_SCREEN_DUMP_CURSOR="1" if cursor else "0")
            args = [tool("vela-screen-dump"), path]
            if region is not None:
                args.extend(str(value) for value in region)
            subprocess.run(args, env=env, check=True)
        finally:
            _profile("screen-dump", time.perf_counter() - began)

    def pixels(self, output=0):
        """The chosen output now, as an Image (vela-screen-dump)."""
        path = os.path.join(self.directory, "shot.png")
        self.screenshot(path, output=output)
        return Image.read(path)


class FakeShell:
    """The fake Qt shell: listens on its socket and collects the lines
    Vela sends it ("toggle-start", "accessibility {...}", ...)."""

    def __init__(self, display):
        runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        self.path = os.path.join(runtime, f"vela-shell-{display}.sock")
        if os.path.exists(self.path):
            os.unlink(self.path)
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(self.path)
        self.server.listen(16)
        self.lines = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while True:
            try:
                connection, _ = self.server.accept()
            except OSError:
                return
            data = b""
            with connection:
                while chunk := connection.recv(65536):
                    data += chunk
            with self.lock:
                self.lines += data.decode(errors="replace").splitlines()

    def received(self, prefix=""):
        with self.lock:
            return [line for line in self.lines if line.startswith(prefix)]

    def wait_for(self, prefix, timeout=5.0, containing=""):
        """The last line starting with `prefix` (and `containing` that), as
        soon as it arrives."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            lines = [line for line in self.received(prefix) if containing in line]
            if lines:
                return lines[-1]
            time.sleep(0.05)
        raise TimeoutError(f"the shell never received «{prefix}…{containing}»; it got: {self.received()[-10:]}")

    def clear(self):
        with self.lock:
            self.lines = []

    def close(self):
        try:
            self.server.shutdown(socket.SHUT_RDWR) # wakes accept()
        except OSError:
            pass
        self.server.close()
        if os.path.exists(self.path):
            os.unlink(self.path)


class Image:
    """An 8-bit RGB PNG from screen-dump or Qt (standard library only)."""

    def __init__(self, width, height, rows):
        self.width = width
        self.height = height
        self.rows = rows

    @staticmethod
    def read(path):
        began = time.perf_counter()
        try:
            return Image._read_impl(path)
        finally:
            _profile("png-decode", time.perf_counter() - began)

    @staticmethod
    def _read_impl(path):
        if PillowImage is not None:
            try:
                with PillowImage.open(path) as png:
                    assert png.format == "PNG" and png.mode == "RGB", "expected 8-bit RGB PNG"
                    assert not png.info.get("interlace", 0), "interlaced PNG not supported"
                    width, height = png.size
                    pixels = png.tobytes()
                stride = width * 3
                assert len(pixels) == height * stride, "invalid PNG scanline size"
                return Image(width, height,
                             [pixels[offset:offset + stride]
                              for offset in range(0, len(pixels), stride)])
            except (OSError, ValueError) as error:
                raise AssertionError(f"invalid PNG: {error}") from error
        with open(path, "rb") as file:
            data = file.read()
        assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
        position = 8
        width = height = 0
        packed = b""
        while position < len(data):
            length, kind = struct.unpack(">I4s", data[position:position + 8])
            chunk = data[position + 8:position + 8 + length]
            if kind == b"IHDR":
                width, height = struct.unpack(">II", chunk[:8])
                assert chunk[8:] == b"\x08\x02\x00\x00\x00", "expected non-interlaced 8-bit RGB"
            elif kind == b"IDAT":
                packed += chunk
            position += 12 + length
        raw = zlib.decompress(packed)
        stride = 1 + width * 3
        assert len(raw) == height * stride, "invalid PNG scanline size"
        rows = []
        for y in range(height):
            filter_type = raw[y * stride]
            row = bytearray(raw[y * stride + 1:(y + 1) * stride])
            assert filter_type <= 4, "invalid PNG filter"
            if filter_type:
                previous = rows[-1] if rows else bytes(width * 3)
                for x in range(len(row)):
                    left = row[x - 3] if x >= 3 else 0
                    above = previous[x]
                    upper_left = previous[x - 3] if x >= 3 else 0
                    if filter_type == 1:
                        prediction = left
                    elif filter_type == 2:
                        prediction = above
                    elif filter_type == 3:
                        prediction = (left + above) // 2
                    else:
                        p = left + above - upper_left
                        distances = (abs(p - left), abs(p - above), abs(p - upper_left))
                        prediction = (left, above, upper_left)[distances.index(min(distances))]
                    row[x] = (row[x] + prediction) & 255
            rows.append(bytes(row))
        return Image(width, height, rows)

    def at(self, x, y):
        row = self.rows[y]
        return tuple(row[x * 3:x * 3 + 3])

    def changed_bounds(self, other, height=None):
        """Half-open bounds of changed RGB pixels, or None; optionally omit bottom rows."""
        assert (self.width, self.height) == (other.width, other.height), "image dimensions differ"
        left, top, right, bottom = self.width, self.height, 0, 0
        for y in range(self.height if height is None else height):
            actual, old = self.rows[y], other.rows[y]
            if actual == old:
                continue
            first = next(i for i, (a, b) in enumerate(zip(actual, old)) if a != b) // 3
            last = self.width - next(i for i, (a, b) in enumerate(zip(reversed(actual), reversed(old)))
                                     if a != b) // 3
            left, top = min(left, first), min(top, y)
            right, bottom = max(right, last), y + 1
        return (left, top, right, bottom) if right else None

    def colors(self, x0, y0, x1, y1):
        """I colori diversi nel rettangolo [x0, x1) x [y0, y1)."""
        return {self.at(x, y) for y in range(y0, y1) for x in range(x0, x1)}


class State(dict):
    """The result of "state", with a few conveniences."""

    @property
    def windows(self):
        return self["windows"]

    def window(self, identifier):
        for window in self["windows"]:
            if window["id"] == identifier:
                return window
        raise KeyError(identifier)

    def has(self, identifier):
        return any(window["id"] == identifier for window in self["windows"])

    @property
    def output(self):
        return self["outputs"][0]


def gpu_available():
    """Vela's renderer wants a real GPU (DRM node and Vulkan 1.4)."""
    return any(name.startswith("renderD") for name in os.listdir("/dev/dri")) if os.path.isdir("/dev/dri") else False
