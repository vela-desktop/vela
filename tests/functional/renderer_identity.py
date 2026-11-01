# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later

"""Verify that Vela actually opened the GPU requested by the hardware CI."""

from pathlib import Path
import re
import sys

# Emitted by the compositor after it opens the selected Vulkan DRM render node.
_RENDERER_NODE = re.compile(r"Vela renderer: [^\n]*?\((/dev/dri/renderD\d+)\)")


def verify_renderer_vendor(log, expected_vendor, sysfs_root=Path("/sys/class/drm")):
    """Return selected render nodes or fail if any GPU isn't from the expected vendor."""
    expected = int(expected_vendor, 0)
    nodes = _RENDERER_NODE.findall(log)
    if not nodes:
        raise AssertionError("No Vela renderer selection found in compositor log")

    for node in nodes:
        vendor_path = Path(sysfs_root) / Path(node).name / "device" / "vendor"
        try:
            actual = int(vendor_path.read_text(encoding="ascii").strip(), 0)
        except (OSError, ValueError) as exc:
            raise AssertionError(f"Cannot determine PCI vendor for selected GPU {node}: {exc}") from exc
        if actual != expected:
            raise AssertionError(
                f"Vela selected {node} with PCI vendor 0x{actual:04x}; expected 0x{expected:04x}"
            )
    return nodes


def main():
    if len(sys.argv) != 3:
        print("usage: renderer_identity.py COMPOSITOR_LOG EXPECTED_PCI_VENDOR", file=sys.stderr)
        return 2
    try:
        log = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
        nodes = verify_renderer_vendor(log, sys.argv[2])
    except (OSError, ValueError, AssertionError) as exc:
        print(f"::error::Renderer identity check failed: {exc}", file=sys.stderr)
        return 1
    print("Vela renderer verified on " + ", ".join(nodes) + f" (PCI {sys.argv[2]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
