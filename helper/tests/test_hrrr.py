"""HRRR home notes when the model has no value at home (offline fixtures)."""
import math
import unittest
from unittest.mock import patch

import numpy as np

from pchelper import hrrr

NAN = float("nan")
T0 = 1_800_000_000


class FrostNoteTests(unittest.TestCase):
    def test_no_value_at_home_gives_no_claim(self):
        self.assertEqual(hrrr._frost_note([T0, T0 + 3600], [NAN, NAN], partial=False), "")

    def test_complete_night(self):
        self.assertIn("no frost", hrrr._frost_note([T0, T0 + 3600], [41.0, 39.6], partial=False))
        self.assertIn("frost likely", hrrr._frost_note([T0, T0 + 3600], [35.0, 31.2], partial=False))

    def test_partial_night_never_promises_no_frost(self):
        # some hours missing: the true low can only be lower than what we saw
        self.assertEqual(hrrr._frost_note([T0, T0 + 3600], [41.0, NAN], partial=True), "")
        note = hrrr._frost_note([T0, T0 + 3600], [NAN, 31.0], partial=True)
        self.assertIn("frost likely", note)
        self.assertIn("part of the night", note)
        self.assertNotIn("nan", note.lower())


class PeakTests(unittest.TestCase):
    def test_peak_ignores_missing_hours(self):
        self.assertEqual(hrrr._peak([NAN, 3.0, 25.0, NAN]), 2)
        self.assertIsNone(hrrr._peak([NAN, NAN]))


class ClearNoteTests(unittest.TestCase):
    def test_missing_cover_is_not_reported_as_cloudy(self):
        times = [T0 + h * 3600 for h in range(4)]
        with patch.object(hrrr, "sun_altitude", return_value=-30):
            self.assertEqual(hrrr._clear_note({"home_lat": 35, "home_lon": -79}, times,
                                              [NAN, NAN, NAN, NAN]), "")
            self.assertEqual(hrrr._clear_note({"home_lat": 35, "home_lon": -79}, times,
                                              [10.0, NAN, 10.0, 10.0]), "")


class RunTests(unittest.TestCase):
    """The whole job with home on a masked cell (ocean or outside HRRR)."""

    def _run(self, home_valid):
        cfg = {"home_lat": 35.0, "home_lon": -79.0, "cache_dir": "/tmp"}
        h, w = 20, 30
        idx = np.arange(h * w, dtype=np.int32).reshape(h, w)
        lon0, lat0, lon1, lat1 = hrrr._box(cfg)
        r = int((lat1 - cfg["home_lat"]) / hrrr.STEP)
        c = int((cfg["home_lon"] - lon0) / hrrr.STEP)
        big = np.full((int(round((lat1 - lat0) / hrrr.STEP)),
                       int(round((lon1 - lon0) / hrrr.STEP))), -1, dtype=np.int32)
        big[:h, :w] = idx
        if home_valid:
            big[r - 3:r + 4, c - 3:c + 4] = 0   # home sits on a model cell
        fields = {"lats": np.zeros((h, w)), "lons": np.zeros((h, w)),
                  "clouds": np.full((h, w), 5.0), "smoke": np.full((h, w), 1e-9),
                  "gust": np.full((h, w), 3.0), "temp": np.full((h, w), 272.0)}
        notes = {}

        def publish(cfg, product, frames, pal, box, note="", **kw):
            notes[product] = note

        run_t = hrrr.dt.datetime.fromtimestamp(T0, hrrr.dt.timezone.utc)
        with patch.object(hrrr, "_latest_run", return_value=run_t), \
             patch.object(hrrr, "_fetch_fields", return_value=fields), \
             patch.object(hrrr, "_index_map", return_value=(big, hrrr._box(cfg))), \
             patch.object(hrrr, "sun_altitude", return_value=-30), \
             patch.object(hrrr, "publish", side_effect=publish), \
             patch.object(hrrr.time, "time", return_value=T0 + 1800):
            wait = hrrr.run(cfg)
        return wait, notes

    def test_home_off_the_model_publishes_maps_without_home_claims(self):
        wait, notes = self._run(home_valid=False)
        self.assertEqual(wait, 3 * 3600)
        self.assertEqual(set(notes), {"hrrr_clouds", "hrrr_smoke", "hrrr_gust", "hrrr_frost"})
        self.assertTrue(all(n == "" for n in notes.values()), notes)

    def test_home_on_the_model_keeps_its_notes(self):
        _, notes = self._run(home_valid=True)
        self.assertIn("frost likely", notes["hrrr_frost"])   # 272 K = 30 °F
        self.assertIn("part of the night", notes["hrrr_frost"])  # night runs past f18
        self.assertIn("clear", notes["hrrr_clouds"])
