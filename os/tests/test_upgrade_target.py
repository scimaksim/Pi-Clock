"""Run the upgrade preflight with fixture files and no storage or network tools."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class UpgradeTargetTests(unittest.TestCase):
    def test_installer_stops_before_touching_storage(self):
        source = Path("os/overlay/usr/local/bin/pi-clock-upgrade").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for path in ("/boot/firmware", "/etc", "/data/tmp", "/sys/class/block", "/proc/self"):
                source = source.replace(path, str(root) + path)
                (root / path[1:]).mkdir(parents=True, exist_ok=True)
            script = root / "upgrade"
            script.write_text(source)
            (root / "etc/pi-clock-os.conf").write_text(
                'UPDATE_URL="https://github.com/MW0MWZ/Pi-Clock/releases/latest/download"\n'
                'MANIFEST_FILE=manifest.txt\n')
            for part in (2, 3):
                dev = root / f"sys/class/block/mmcblk0p{part}/dev"
                dev.parent.mkdir()
                dev.write_text(f"179:{part}\n")
            tools = root / "bin"
            tools.mkdir()
            for name in ("awk", "cat", "grep", "mkdir"):
                (tools / name).symlink_to(shutil.which(name))
            for name, body in [("id", "echo 0"), ("wget", "exit 1")]:
                tool = tools / name
                tool.write_text("#!/bin/sh\n" + body + "\n")
                tool.chmod(0o755)

            for slot, pending, mounted, version, expected in (
                ("A", "false", 2, "fork-review", "this is a fork image"),
                ("B", "true", 2, "stock", "still pending"),
                ("B", "false", 2, "stock", "mounted root disagrees"),
                ("B", "false", 3, "stock", "failed to fetch manifest"),
                ("A", "false", 2, "stock", "failed to fetch manifest"),
            ):
                with self.subTest(slot=slot, pending=pending, mounted=mounted, version=version):
                    (root / "etc/pi-clock-os-release").write_text(f"Version: {version}\n")
                    (root / "boot/firmware/slot.conf").write_text(f"ACTIVE_SLOT={slot}\nPENDING={pending}\n")
                    (root / "proc/self/mountinfo").write_text(
                        f"17 0 179:{mounted} / / rw - ext4 /dev/root rw\n")
                    result = subprocess.run(["/bin/sh", str(script), "--install"], text=True,
                                            capture_output=True, env={**os.environ, "PATH": str(tools)})
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected, result.stderr)
                    self.assertNotIn("STAGE:format", result.stdout)
