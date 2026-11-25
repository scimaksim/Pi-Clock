import sys
import threading
import types
import unittest
from unittest.mock import patch

from pchelper import common, server


class _Stop:
    """Stops the job loop after a fixed number of waits, recording them."""
    def __init__(self, n):
        self.n, self.waits = n, []

    def is_set(self):
        return len(self.waits) >= self.n

    def wait(self, s):
        self.waits.append(s)


class JobLoopTests(unittest.TestCase):
    def test_upstream_gap_retries_steadily_without_claiming_success(self):
        mod = types.ModuleType("pchelper.fakejob")
        mod.run = lambda cfg: (_ for _ in ()).throw(common.RetryLater("radar 45 min old", 120))
        stop = _Stop(4)
        with patch.dict(sys.modules, {"pchelper.fakejob": mod}):
            server.job_loop({}, "fakejob", stop)
        self.assertEqual(stop.waits, [120, 120, 120, 120])
        self.assertEqual(server.STATUS["fakejob"]["state"], "error")
        self.assertIn("45 min old", server.STATUS["fakejob"]["error"])

    def test_real_failures_still_back_off(self):
        mod = types.ModuleType("pchelper.failjob")
        mod.run = lambda cfg: 1 / 0
        stop = _Stop(4)
        with patch.dict(sys.modules, {"pchelper.failjob": mod}), patch.object(server.log, "exception"):
            server.job_loop({}, "failjob", stop)
        self.assertEqual(stop.waits, [60, 120, 240, 480])
