import importlib.util
import sys
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("fetch_gfs", Path(__file__).with_name("fetch-gfs.py"))
gfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gfs)


class GfsTests(unittest.TestCase):
    def test_fallback_restarts_both_wind_components(self):
        latest = datetime(2026, 1, 2, 0, tzinfo=timezone.utc)
        previous = latest - timedelta(hours=6)

        def fetch(run, tmpdir, variable, *args):
            if (run == latest and variable == "VGRD") or variable == "TCDC":
                return None
            return [run.hour]

        with patch.object(sys, "argv", ["fetch-gfs.py", "unused.gz"]), \
             patch.object(gfs, "get_latest_cycle", return_value=latest), \
             patch.object(gfs, "fetch_field", side_effect=fetch), \
             patch.object(gfs, "write_gfs_binary", return_value=True) as write:
            gfs.main()
        write.assert_called_once_with([("UGRD", [18]), ("VGRD", [18]), ("APCP", [18])],
                                      previous, "unused.gz")
