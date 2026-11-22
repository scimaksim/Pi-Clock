import datetime as dt
import sys
import unittest
from unittest.mock import Mock, patch

import numpy as np

from pchelper import birds, nowcast


class NowcastTests(unittest.TestCase):
    def test_irregular_scans_across_midnight(self):
        latest = dt.datetime(2026, 1, 2, 0, 51, tzinfo=dt.timezone.utc)
        keys = []
        for minutes in (60, 50, 40, 30, 25, 19, 15, 11, 5, 0):
            scan = latest - dt.timedelta(minutes=minutes)
            keys.append(f"scan/{scan:%Y%m%d}/mrms_{scan:%Y%m%d-%H%M%S}.grib2.gz")

        def listing(bucket, prefix):
            return [key for key in keys if prefix[-9:] in key]

        def radar(cfg, key):
            return np.full((16, 16), (latest.timestamp() - nowcast._key_time(key)) / 60,
                           dtype=np.float32)

        def flow(a, b):
            return np.zeros((*a.shape, 2), dtype=np.float32)

        cfg = {"home_lat": 35, "home_lon": -79}
        with patch.object(nowcast, "s3_list", side_effect=listing), \
             patch.object(nowcast, "_load", side_effect=radar), \
             patch.object(nowcast, "_flow", side_effect=flow) as motion, \
             patch.object(nowcast, "_home_max", return_value=25):
            obs, forecast = nowcast.run(cfg, latest.timestamp() + 90, publish_out=False)
        self.assertEqual(len(obs), 10)
        self.assertEqual(obs[0][0], latest.timestamp() - 3600)
        self.assertEqual([t for t, _ in forecast], [latest.timestamp() + m * 60 for m in nowcast.LEADS])
        self.assertEqual([(a[0, 0], b[0, 0]) for (a, b), _ in motion.call_args_list],
                         [(19, 11), (11, 0)])

    def test_old_scans_do_not_start_a_new_forecast(self):
        key = "scan_20260101-120000.grib2.gz"
        with patch.object(nowcast, "s3_list", return_value=[key]):
            with self.assertRaisesRegex(RuntimeError, "no recent MRMS"):
                nowcast.run({}, nowcast._key_time(key) + 3600)


class BirdTests(unittest.TestCase):
    def test_cached_volume_still_ages_out(self):
        key = "2026/01/01/KRAX/KRAX20260101_120000_V06"
        start = dt.datetime(2026, 1, 1, 12, tzinfo=dt.timezone.utc).timestamp()
        cfg = {"radar": "KRAX", "home_lat": 35, "home_lon": -79}
        with patch.dict(sys.modules, pyart=Mock()), patch.object(birds, "_stats", return_value={}), \
             patch.object(birds, "s3_list", return_value=[key]), patch.object(birds, "_last_key", key), \
             patch.object(birds, "sun_altitude", return_value=-20), \
             patch.object(birds.time, "time", side_effect=[start + 30, start + 1801]):
            self.assertEqual(birds.run(cfg), 120)
            with self.assertRaisesRegex(RuntimeError, "newest volume"):
                birds.run(cfg)
