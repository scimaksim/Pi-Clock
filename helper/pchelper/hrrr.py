"""HRRR forecast maps: tonight's clouds, smoke, wind gusts, frost.

NOAA's High-Resolution Rapid Refresh (3 km, hourly runs, 18-hour
forecasts) from AWS. Only the four needed fields are downloaded, using
the .idx files and HTTP byte ranges (~3.5 MB per forecast hour instead
of ~130 MB). Each field is resampled from the Lambert grid to a lat/lon
image around home (nearest neighbour; the index map is computed once).

Products: hrrr_clouds, hrrr_smoke, hrrr_gust (18 hourly frames each) and
hrrr_frost (tonight's lowest temperature, one frame), each with a note
about home.
"""

import datetime as dt
import logging
import math
import os
import time

import eccodes
import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

from .common import Palette, get, publish, sun_altitude

log = logging.getLogger("pchelper.hrrr")

BASE = "https://noaa-hrrr-bdp-pds.s3.amazonaws.com"
HOURS = range(1, 19)
STEP = 0.03                    # output grid, degrees (~HRRR's 3 km)
HALF_LON, HALF_LAT = 12.0, 7.0  # around home: covers the local views
FIELDS = {
    "clouds": "TCDC:entire atmosphere",
    "smoke": "MASSDEN:8 m above ground",
    "gust": "GUST:surface",
    "temp": "TMP:2 m above ground",
}

PAL_CLOUD = Palette([(15, "#d8dde6", 40), (40, "#e3e7ee", 110),
                     (70, "#eef1f5", 170), (100, "#ffffff", 215)], vmin_show=15)
PAL_SMOKE = Palette([(4, "#b8a888", 60), (12, "#e8d44a", 120), (35, "#f29a2e", 170),
                     (55, "#e5402a", 200), (150, "#9b2d8f", 220), (250, "#5a1030", 235)],
                    vmin_show=4)
PAL_GUST = Palette([(20, "#7fd3e6", 70), (25, "#59c46b", 130), (35, "#f1d54a", 170),
                    (45, "#f08a2e", 200), (55, "#de3b2f", 220), (70, "#b02cc0", 235)],
                   vmin_show=20)
# Icy blues (birds own the violet/pink family)
PAL_FROST = Palette([(20, "#1f5fb8", 220), (28, "#3f9be0", 200), (32, "#7fd0f5", 180),
                     (36, "#cdeefc", 90)], vmin_show=-40)

_map = {}


def _box(cfg):
    lat, lon = cfg["home_lat"], cfg["home_lon"]
    lat0 = max(21.5, lat - HALF_LAT)
    lat1 = min(52.5, lat + HALF_LAT)
    lon0 = max(-134.0, lon - HALF_LON)
    lon1 = min(-61.0, lon + HALF_LON)
    return lon0, lat0, lon1, lat1


def _index_map(cfg, lats, lons):
    """Flat HRRR index for each output pixel (-1 = outside the model)."""
    box = _box(cfg)
    key = (box, lats.shape)
    if key in _map:
        return _map[key]
    path = os.path.join(cfg["cache_dir"], "hrrr_map_%.2f_%.2f_%.2f_%.2f.npz" % box)
    if os.path.exists(path):
        d = np.load(path)
        if tuple(d["shape"]) == lats.shape:
            _map[key] = (d["idx"], box)
            return _map[key]

    def xyz(la, lo):
        la, lo = np.radians(la), np.radians(lo)
        return np.stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)], -1)

    tree = cKDTree(xyz(lats.ravel(), ((lons.ravel() + 180) % 360) - 180))
    lon0, lat0, lon1, lat1 = box
    w = int(round((lon1 - lon0) / STEP))
    h = int(round((lat1 - lat0) / STEP))
    glat = lat1 - (np.arange(h) + 0.5) * STEP
    glon = lon0 + (np.arange(w) + 0.5) * STEP
    LO, LA = np.meshgrid(glon, glat)
    dist, idx = tree.query(xyz(LA.ravel(), LO.ravel()))
    idx = idx.astype(np.int32)
    idx[dist * 6371.0 > 3.0] = -1                # beyond the model domain
    idx = idx.reshape(h, w)
    np.savez_compressed(path, idx=idx, shape=np.array(lats.shape))
    _map[key] = (idx, box)
    return _map[key]


def _latest_run(now):
    for back in range(1, 6):
        t = dt.datetime.fromtimestamp(now, dt.timezone.utc) - dt.timedelta(hours=back)
        url = f"{BASE}/hrrr.{t:%Y%m%d}/conus/hrrr.t{t:%H}z.wrfsfcf18.grib2.idx"
        if get(url, timeout=20):
            return t.replace(minute=0, second=0, microsecond=0)
    raise RuntimeError("no complete HRRR run in the last 5 hours")


def _fetch_fields(run, fh):
    base = f"{BASE}/hrrr.{run:%Y%m%d}/conus/hrrr.t{run:%H}z.wrfsfcf{fh:02d}.grib2"
    idx = get(base + ".idx", timeout=30)
    if not idx:
        raise RuntimeError(f"missing idx f{fh:02d}")
    lines = idx.decode().strip().split("\n")
    offs = [int(l.split(":")[1]) for l in lines]
    out = {}
    for name, field in FIELDS.items():
        for i, l in enumerate(lines):
            if f":{field}:" in l and (" fcst" in l or "anl" in l):
                end = offs[i + 1] - 1 if i + 1 < len(offs) else ""
                msg = get(base, timeout=60, headers={"Range": f"bytes={offs[i]}-{end}"})
                gid = eccodes.codes_new_from_message(msg)
                try:
                    vals = eccodes.codes_get_values(gid)
                    shape = (eccodes.codes_get(gid, "Nj"), eccodes.codes_get(gid, "Ni"))
                    if "lats" not in out:
                        out["lats"] = eccodes.codes_get_array(gid, "latitudes").reshape(shape)
                        out["lons"] = eccodes.codes_get_array(gid, "longitudes").reshape(shape)
                    out[name] = vals.reshape(shape)
                finally:
                    eccodes.codes_release(gid)
                break
        else:
            raise RuntimeError(f"field {field} not in f{fh:02d}")
    return out


def _home_value(cfg, idx_map, field_img):
    lon0, lat0, lon1, lat1 = _box(cfg)
    r = int((lat1 - cfg["home_lat"]) / STEP)
    c = int((cfg["home_lon"] - lon0) / STEP)
    return float(field_img[r, c])


def _clear_note(cfg, times, cover_home):
    lat, lon = cfg["home_lat"], cfg["home_lon"]
    dark = [sun_altitude(lat, lon, t) < -12 for t in times]
    if not any(dark):
        return ""
    # first dark stretch in the forecast = tonight
    i0 = dark.index(True)
    i1 = i0
    while i1 + 1 < len(dark) and dark[i1 + 1]:
        i1 += 1
    best, cur = None, None
    for i in range(i0, i1 + 1):
        if cover_home[i] < 25:
            cur = (cur[0], i) if cur else (i, i)
            if not best or cur[1] - cur[0] > best[1] - best[0]:
                best = cur
        else:
            cur = None
    full = i1 == len(dark) - 1
    if best is None:
        return "Tonight at home: cloudy, no clear spell" + ("" if full else "")
    a, b = times[best[0]], times[best[1]] + 3600
    if best[0] == i0 and best[1] == i1:
        return f"Tonight at home: clear from {{t:{a}}}" + ("" if full else f" to {{t:{b}}}")
    return f"Tonight at home: clear {{t:{a}}}–{{t:{b}}}"


def run(cfg):
    now = time.time()
    run_t = _latest_run(now)
    times, imgs = [], {k: [] for k in FIELDS}
    idx_map = None
    for fh in HOURS:
        d = _fetch_fields(run_t, fh)
        if idx_map is None:
            idx_map, box = _index_map(cfg, d["lats"], d["lons"])
            valid = idx_map >= 0
            flat = np.where(valid, idx_map, 0)
        for k in FIELDS:
            img = d[k].ravel()[flat].astype(np.float32)
            # Soften the 3 km cells (no staircase edges when zoomed in);
            # smoothing is normalised so the model edge doesn't darken
            w = ndimage.gaussian_filter(valid.astype(np.float32), 1.2)
            v = ndimage.gaussian_filter(np.where(valid, img, 0).astype(np.float32), 1.2)
            with np.errstate(all="ignore"):
                img = np.where(valid & (w > 0.3), v / w, np.nan).astype(np.float32)
            imgs[k].append(img)
        times.append(int(run_t.timestamp()) + fh * 3600)

    cloud = imgs["clouds"]
    smoke = [a * 1e9 for a in imgs["smoke"]]                 # kg/m3 -> ug/m3
    gust = [a * 2.23694 for a in imgs["gust"]]               # m/s -> mph
    temp = [(a - 273.15) * 9 / 5 + 32 for a in imgs["temp"]]  # K -> F

    home = lambda arr: [_home_value(cfg, idx_map, a) for a in arr]
    cover_h, smoke_h, gust_h, temp_h = home(cloud), home(smoke), home(gust), home(temp)

    publish(cfg, "hrrr_clouds", [(t, PAL_CLOUD.index(a)) for t, a in zip(times, cloud)],
            PAL_CLOUD, box, note=_clear_note(cfg, times, cover_h),
            note_rgb=[0.8, 0.85, 1.0], legend=PAL_CLOUD.legend("Cloud cover %, HRRR",
                                                               [25, 50, 75, 100]),
            label="clock", frame_ms=700, expires_s=4 * 3600)

    note = ""
    i = int(np.argmax(smoke_h))
    if smoke_h[i] >= 12:
        note = f"Smoke at home up to {smoke_h[i]:.0f} µg/m³ around {{t:{times[i]}}}"
    publish(cfg, "hrrr_smoke", [(t, PAL_SMOKE.index(a)) for t, a in zip(times, smoke)],
            PAL_SMOKE, box, note=note, note_rgb=[0.95, 0.8, 0.5],
            legend=PAL_SMOKE.legend("Near-surface smoke µg/m³, HRRR",
                                    [12, 35, 55, 150, 250]),
            label="clock", frame_ms=700, expires_s=4 * 3600)

    note = ""
    i = int(np.argmax(gust_h))
    if gust_h[i] >= 20:
        note = f"Gusts at home up to {gust_h[i]:.0f} mph around {{t:{times[i]}}}"
    publish(cfg, "hrrr_gust", [(t, PAL_GUST.index(a)) for t, a in zip(times, gust)],
            PAL_GUST, box, note=note, note_rgb=[0.6, 0.9, 0.7],
            legend=PAL_GUST.legend("Wind gusts mph, HRRR", [25, 35, 45, 55, 70]),
            label="clock", frame_ms=700, expires_s=4 * 3600)

    # Frost: lowest temperature over tonight's hours (sun below the horizon)
    lat, lon = cfg["home_lat"], cfg["home_lon"]
    night = [i for i, t in enumerate(times) if sun_altitude(lat, lon, t) < 0]
    if night:
        # tonight = the first run of night hours
        run_end = night[0]
        while run_end + 1 in night:
            run_end += 1
        sel = list(range(night[0], run_end + 1))
        with np.errstate(all="ignore"):
            st = np.stack([temp[i] for i in sel])
            tmin = np.where(np.isfinite(st).any(0), np.nanmin(np.where(np.isfinite(st), st, 999), 0), np.nan)
        low = min(temp_h[i] for i in sel)
        when = times[sel[int(np.argmin([temp_h[i] for i in sel]))]]
        if low <= 33:
            note = f"Tonight's low at home ~{low:.0f}°F around {{t:{when}}}: frost likely"
        elif low <= 36:
            note = f"Tonight's low at home ~{low:.0f}°F: patchy frost possible"
        else:
            note = f"Tonight's low at home ~{low:.0f}°F, no frost"
        img = np.where(tmin <= 36, tmin, np.nan)
        publish(cfg, "hrrr_frost", [(times[sel[0]], PAL_FROST.index(img))], PAL_FROST, box,
                note=note, note_rgb=[0.7, 0.85, 1.0],
                legend=PAL_FROST.legend("Tonight's low °F (36 and below), HRRR",
                                        [20, 28, 32, 36]),
                label="none", animate=False, expires_s=6 * 3600)
    log.info("hrrr run %s: home cloud %s smoke max %.0f gust max %.0f",
             run_t.strftime("%HZ"), [round(c) for c in cover_h], max(smoke_h), max(gust_h))
    return 3 * 3600
