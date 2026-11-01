#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Actual private KWin/DBus/socket preflight; no VT, login or systemd takeover.

The user-manager environment and NSS identity are fixture boundaries. KWin,
DBus ownership, Wayland peer credentials and KScreen queries are real.
"""

import json
import copy
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from vela_bench.conditions import compare_outputs, greeter_environment, normalized_outputs


def child(adapter, base):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    inherited = dict(os.environ)
    # Reproduce direct kwin_wayland: a live socket exists, but the manager
    # did not import WAYLAND_DISPLAY. The socket deliberately isn't wayland-0.
    clean = {key: value for key, value in inherited.items() if key not in ("WAYLAND_DISPLAY", "DISPLAY")}
    def native(*arguments, env=clean):
        return subprocess.run([str(adapter), *arguments], env=env, capture_output=True, text=True, timeout=12)
    first = native("--resolve-wayland")
    if first.returncode:
        raise RuntimeError(first.stderr)
    found = json.loads(first.stdout)
    assert found["wayland_display"] == "wayland-vela-greeter-check", found
    assert found["uid"] == os.getuid() and found["pid"] != os.getpid(), found
    checks = {"missing_display_variable": True, "real_kwin_peer_identity": True}
    # An owned live Unix socket from another process is not a KWin endpoint.
    with socket.socket(socket.AF_UNIX) as decoy:
        decoy.bind(str(runtime / "wayland-decoy"))
        decoy.listen(4)
        again = native("--resolve-wayland")
        assert again.returncode == 0 and json.loads(again.stdout) == found, again.stderr
    checks["foreign_listener_ignored"] = True
    alias = runtime / "wayland-alias"
    os.link(runtime / found["wayland_display"], alias)
    try:
        ambiguous = native("--resolve-wayland")
        assert ambiguous.returncode != 0 and "expected one" in ambiguous.stderr, ambiguous.stderr
    finally:
        alias.unlink()
    checks["ambiguous_kwin_endpoints_rejected"] = True

    # Exercise the Python preflight against that actual KWin/socket. Only
    # identity and privileged process creation are replaced: the test cannot
    # create a real greeter account or drop groups as an unprivileged process.
    class FixtureHost:
        def run(self, command, **kwargs):
            assert command[:4] == ["setpriv", f"--reuid={os.getuid()}", f"--regid={os.getgid()}", "--clear-groups"]
            if command[-3:] == ["/usr/bin/systemctl", "--user", "show-environment"]:
                return SimpleNamespace(returncode=0, stderr="", stdout="")
            assert command[-2:] == [str(adapter), "--resolve-wayland"], command
            # This fixture's bus is private, not /run/user/UID/bus. The
            # native adapter still validates its actual org.kde.KWin owner.
            return native("--resolve-wayland")
    user = SimpleNamespace(pw_name="plasmalogin", pw_uid=os.getuid(), pw_gid=os.getgid(), pw_dir=str(base / "home"))
    sessions = [{"Id": "fixture", "User": str(user.pw_uid), "VTNr": "1", "Class": "greeter",
                 "Type": "wayland", "Remote": "no", "Seat": "seat0", "State": "online"}]
    proc = base / "empty-proc"
    proc.mkdir()
    # The policy validates a canonical bus socket; this extra socket does
    # not serve requests. All native DBus operations use the real private bus.
    with socket.socket(socket.AF_UNIX) as fixture_bus:
        fixture_bus.bind(str(runtime / "bus"))
        with patch("vela_bench.conditions.pwd.getpwuid", return_value=user):
            resolved = greeter_environment(sessions, proc, host=FixtureHost(), run=base / "run", desktop=adapter)
    assert resolved["environment"]["WAYLAND_DISPLAY"] == found["wayland_display"]
    checks["python_preflight_without_manager_display_variable"] = True
    display_env = dict(clean, WAYLAND_DISPLAY=found["wayland_display"])
    subprocess.run(["dbus-update-activation-environment", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR"],
                   env=display_env, check=True, capture_output=True, timeout=5)
    output = native("--outputs", env=display_env)
    if output.returncode:
        raise RuntimeError(output.stderr)
    inventory = normalized_outputs(output.stdout, "plasma")
    assert len(inventory) == 2 and all(row["enabled"] for row in inventory), inventory
    checks["native_kscreen_query_after_resolution"] = True
    # Query and apply are distinct native processes. Deliberately replace all
    # mode IDs to reproduce backend-local IDs changing across that boundary.
    restored = copy.deepcopy(inventory)
    restored[0]["priority"], restored[1]["priority"] = 1, 0
    for row in restored:
        original = row["current_mode_id"]
        for mode in row["modes"]:
            saved = mode["id"]
            mode["id"] = "snapshot-" + saved
            if saved == original:
                row["current_mode_id"] = mode["id"]
    applied = native("--apply", json.dumps(restored), env=display_env)
    assert applied.returncode == 0, applied.stderr
    observed = native("--outputs", env=display_env)
    assert observed.returncode == 0, observed.stderr
    compare_outputs(normalized_outputs(observed.stdout, "plasma"), restored, priorities=True)
    checks["restore_resolves_physical_mode_across_processes"] = True
    checks["enabled_unordered_secondary_remains_enabled"] = True
    # The benchmark disables all secondary heads. Exercise the real
    # SetConfig transition back to a nonzero two-head layout, not just a
    # repeated application of an already-enabled layout.
    arranged = copy.deepcopy(restored)
    arranged[0]["x"], arranged[1]["x"] = 800, 0
    applied = native("--apply", json.dumps(arranged), env=display_env)
    assert applied.returncode == 0, applied.stderr
    controlled = copy.deepcopy(arranged)
    controlled[0]["x"] = 0
    controlled[1]["enabled"], controlled[1]["priority"] = False, 0
    applied = native("--apply", json.dumps(controlled), env=display_env)
    assert applied.returncode == 0, applied.stderr
    compare_outputs(normalized_outputs(native("--outputs", env=display_env).stdout, "plasma"), controlled, priorities=True)
    applied = native("--apply", json.dumps(arranged), env=display_env)
    assert applied.returncode == 0, applied.stderr
    compare_outputs(normalized_outputs(native("--outputs", env=display_env).stdout, "plasma"), arranged, priorities=True)
    checks["primary_only_to_original_two_head_layout"] = True
    unavailable = copy.deepcopy(restored)
    for mode in unavailable[0]["modes"]:
        if mode["id"] == unavailable[0]["current_mode_id"]:
            mode["refresh_mhz"] += 12345
    rejected = native("--apply", json.dumps(unavailable), env=display_env)
    assert rejected.returncode != 0 and "physical mode is not advertised" in rejected.stderr, rejected.stderr
    assert "output-configuration-failed" not in rejected.stdout, rejected.stdout
    unchanged = native("--outputs", env=display_env)
    assert unchanged.returncode == 0, unchanged.stderr
    compare_outputs(normalized_outputs(unchanged.stdout, "plasma"), arranged, priorities=True)
    checks["unavailable_physical_mode_refused_without_fallback"] = True
    scene = adapter.parent / "vela-bench-scene"
    if scene.is_file():
        applied = native("--apply", json.dumps(controlled), env=display_env)
        assert applied.returncode == 0, applied.stderr
        process = subprocess.Popen([str(scene), "--scene"], env=display_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            import time
            time.sleep(.3)
            captured = native("--capture", controlled[0]["name"], "1", env=display_env)
            assert captured.returncode == 0, captured.stderr
            record = json.loads(captured.stdout)
            assert len(record["tiles"]) == 576, record.keys()
            assert any(marker["frame"] == 0x715000 for marker in record["markers"]), record["markers"]
            checks["native_scene_identified_through_declared_kwin_capture"] = True
        finally:
            process.terminate()
            process.communicate(timeout=5)
    pattern = adapter.parent / 'vela-pattern'
    if pattern.is_file():
        import base64
        import select
        import time
        from vela_bench.calibration import Lines
        from vela_bench.pixels import pattern_metrics
        scaled = copy.deepcopy(controlled)
        scaled[0]['scale'] = 1.25
        applied = native('--apply',json.dumps(scaled),env=display_env)
        assert applied.returncode == 0, applied.stderr
        compare_outputs(normalized_outputs(native('--outputs',env=display_env).stdout,'plasma'),scaled)
        process = subprocess.Popen([str(pattern),'--benchmark','401','301'],env=display_env,
                                   stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0)
        try:
            lines=Lines(process);meta=lines.next(time.monotonic()+5)
            time.sleep(.3)
            while lines.pending or select.select([process.stdout],[],[],.1)[0]:meta=lines.next(time.monotonic()+5)
            assert meta['scale120']==150 and meta['logical_width']==401,meta
            captured=native('--capture',scaled[0]['name'],'1',env=display_env)
            assert captured.returncode==0,captured.stderr
            record=json.loads(captured.stdout)
            path=base/'fractional-pattern.png';path.write_bytes(base64.b64decode(record['png_base64'],validate=True))
            metrics=pattern_metrics(path,meta,[800,600],1.25)
            assert metrics['checked_pixels']>100000,metrics
            checks['fractional_pattern_native_kwin_capture_and_scale_restore']=True
        finally:
            process.terminate();process.communicate(timeout=5)
            applied=native('--apply',json.dumps(controlled),env=display_env)
            assert applied.returncode==0,applied.stderr
            compare_outputs(normalized_outputs(native('--outputs',env=display_env).stdout,'plasma'),controlled)
    print("PLASMA_PREFLIGHT_EVIDENCE " + json.dumps({"schema": 1, "scope": "isolated-virtual-kwin-no-hardware-qualification",
           "checks": checks, "endpoint": found, "outputs": inventory}, sort_keys=True), flush=True)


def main(adapter, output=None):
    if os.getuid() == 0:
        raise RuntimeError("run the isolated integration test unprivileged")
    adapter = Path(adapter).resolve()
    with tempfile.TemporaryDirectory(prefix="vela-kwin-preflight-") as temporary:
        base = Path(temporary)
        runtime = base / "run/user" / str(os.getuid())
        runtime.mkdir(parents=True, mode=0o700)
        (base / "home").mkdir(mode=0o700)
        env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "HOME": str(base / "home"), "XDG_RUNTIME_DIR": str(runtime),
               "XDG_CONFIG_HOME": str(base / "home/config"), "XDG_DATA_HOME": str(base / "home/data"),
               "XDG_STATE_HOME": str(base / "home/state"), "XDG_CACHE_HOME": str(base / "home/cache"),
               "QT_QPA_PLATFORM": "wayland", "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=" + str(base / "no-system-bus")}
        command = ["dbus-run-session", "--", "kwin_wayland", "--virtual", "--output-count", "2", "--width", "800", "--height", "600",
                   "--socket", "wayland-vela-greeter-check", "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities",
                   "--exit-with-session", shlex.join([sys.executable, str(Path(__file__).resolve()), "--child", str(adapter), str(base)])]
        process = subprocess.Popen(command, env=env, start_new_session=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            try:
                log, _ = process.communicate(timeout=35)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("Isolated KWin integration deadline; partial log: " + str(exc.output)[-4000:]) from exc
            records = [line.removeprefix("PLASMA_PREFLIGHT_EVIDENCE ") for line in log.splitlines() if line.startswith("PLASMA_PREFLIGHT_EVIDENCE ")]
            if len(records) != 1:
                raise RuntimeError("Private KWin/preflight failed: " + log[-7000:])
            evidence = json.loads(records[0])
            if output:
                path = Path(output)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(evidence, indent=2) + "\n")
            print(json.dumps(evidence, indent=2))
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)


if __name__ == "__main__":
    if sys.argv[1] == "--child":
        child(Path(sys.argv[2]), Path(sys.argv[3]))
    else:
        main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
