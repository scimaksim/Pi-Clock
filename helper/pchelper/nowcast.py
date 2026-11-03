"""Rain nowcast: MRMS radar extrapolated 0-90 minutes ahead.

Every 5 minutes: the latest MRMS composite reflectivity (CONUS, 1 km,
every 2 min, NOAA on AWS) and the ones 10, 20 and 30 minutes before,
cropped to the area around home. Motion comes from dense optical flow
(OpenCV Farneback) between consecutive frames, smoothed; the newest
frame is then advected along it (semi-Lagrangian, as in pysteps'
extrapolation nowcast) in 10-minute steps to +90 min.

Product "nowcast": the 4 observed frames then 9 forecast frames, and a
note on rain at home: when it arrives or eases.
"""

import datetime as dt
import gzip
import logging
import time

import cv2
import eccodes
import numpy as np
from scipy import ndimage

from .common import Palette, dist_km, publish, s3_list, get

log = logging.getLogger("pchelper.nowcast")

BUCKET = "noaa-mrms-pds"
PRODUCT = "CONUS/MergedReflectivityQCComposite_00.50"
LAT0, LON0, STEP = 54.995, -129.995, 0.01   # MRMS cell centres, row 0 north
HALF_LAT, HALF_LON = 3.5, 4.5               # area around home
RAIN_DBZ = 20.0                             # light rain at the ground
LEADS = list(range(10, 100, 10))            # minutes

PAL = Palette([
    (15, "#04e9e7", 150), (20, "#019ff4", 170), (25, "#0300f4", 180),
    (30, "#02fd02", 190), (35, "#01c501", 200), (40, "#008e00", 205),
    (45, "#fdf802", 210), (50, "#e5bc00", 215), (55, "#fd9500", 220),
    (60, "#fd0000", 225), (65, "#d40000", 230), (70, "#bc0000", 235),
    (75, "#f800fd", 240),
], vmin_show=15)

_cache = {}     # key -> (epoch, dBZ crop)


def _key_time(key):
    s = key.rsplit("_", 1)[1].split(".")[0]          # 20261103-184039
    return dt.datetime.strptime(s, "%Y%m%d-%H%M%S").replace(
        tzinfo=dt.timezone.utc).timestamp()


def _window(cfg):
    lat, lon = cfg["home_lat"], cfg["home_lon"]
    r0 = int(round((LAT0 - (lat + HALF_LAT)) / STEP))
    r1 = int(round((LAT0 - (lat - HALF_LAT)) / STEP))
    c0 = int(round(((lon - HALF_LON) - LON0) / STEP))
    c1 = int(round(((lon + HALF_LON) - LON0) / STEP))
    bbox = (LON0 + (c0 - 0.5) * STEP, LAT0 - (r1 - 0.5) * STEP,
            LON0 + (c1 - 0.5) * STEP, LAT0 - (r0 - 0.5) * STEP)
    return r0, r1, c0, c1, bbox


def _load(cfg, key):
    if key in _cache:
        return _cache[key][1]
    raw = get(f"https://{BUCKET}.s3.amazonaws.com/{key}", timeout=90)
    if raw is None:
        return None
    gid = eccodes.codes_new_from_message(gzip.decompress(raw))
    try:
        ni = eccodes.codes_get(gid, "Ni")
        nj = eccodes.codes_get(gid, "Nj")
        la1 = eccodes.codes_get(gid, "latitudeOfFirstGridPointInDegrees")
        lo1 = eccodes.codes_get(gid, "longitudeOfFirstGridPointInDegrees")
        vals = eccodes.codes_get_values(gid).reshape(nj, ni)
    finally:
        eccodes.codes_release(gid)
    if abs(la1 - LAT0) > 0.001 or abs(((lo1 + 180) % 360 - 180) - LON0) > 0.001:
        raise RuntimeError(f"unexpected MRMS grid origin {la1},{lo1}")
    r0, r1, c0, c1, _ = _window(cfg)
    crop = vals[r0:r1, c0:c1].astype(np.float32)
    crop[crop < -900] = np.nan          # outside radar coverage
    crop[crop < 0] = 0                  # no echo
    _cache[key] = (_key_time(key), crop)
    for k in sorted(_cache, key=lambda k: _cache[k][0])[:-8]:
        del _cache[k]
    return crop


def _pick(keys, t):
    best = min(keys, key=lambda k: abs(_key_time(k) - t))
    return best if abs(_key_time(best) - t) <= 150 else None


def _flow(a, b):
    """Displacement (px per 10 min) of echoes from frame a to frame b."""
    def u8(x):
        x = np.nan_to_num(x, nan=0.0)
        return np.clip((x - 10) * 4, 0, 255).astype(np.uint8)
    f = cv2.calcOpticalFlowFarneback(u8(a), u8(b), None, 0.5, 5, 31, 4, 7, 1.5, 0)
    return f


def _home_max(crop, cfg, km=4.0):
    r0, r1, c0, c1, _ = _window(cfg)
    hr = int(round((LAT0 - cfg["home_lat"]) / STEP)) - r0
    hc = int(round((cfg["home_lon"] - LON0) / STEP)) - c0
    k = max(1, int(km / 1.11))
    win = crop[max(0, hr - k):hr + k + 1, max(0, hc - k):hc + k + 1]
    return float(np.nanmax(win)) if np.isfinite(win).any() else 0.0


def run(cfg, t_ref=None, publish_out=True):
    now = t_ref or time.time()
    keys = []
    for d in {dt.datetime.fromtimestamp(now - s, dt.timezone.utc).strftime("%Y%m%d")
              for s in (0, 3000)}:
        keys += s3_list(BUCKET, f"{PRODUCT}/{d}/")
    keys = [k for k in keys if k.endswith(".grib2.gz") and 0 <= now - _key_time(k) < 3600]
    if not keys:
        raise RuntimeError("no recent MRMS files")
    newest = max(keys, key=_key_time)
    t0 = _key_time(newest)
    obs = []
    for m in (30, 20, 10, 0):
        k = _pick(keys, t0 - m * 60)
        if k is None:
            raise RuntimeError(f"no MRMS file near t-{m} min")
        crop = _load(cfg, k)
        if crop is None:
            raise RuntimeError(f"MRMS file vanished: {k}")
        obs.append((_key_time(k), crop))

    # Motion: average of the last two 10-minute flows, smoothed hard
    # (storm-scale motion, not every wobble)
    f1 = _flow(obs[1][1], obs[2][1])
    f2 = _flow(obs[2][1], obs[3][1])
    flow = (f1 + 2 * f2) / 3
    for c in range(2):
        flow[..., c] = ndimage.gaussian_filter(flow[..., c], 12)

    cur = np.nan_to_num(obs[3][1], nan=0.0)
    h, w = cur.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    fc = []
    for lead in LEADS:
        k = lead / 10.0
        src = [yy - flow[..., 1] * k, xx - flow[..., 0] * k]
        f = ndimage.map_coordinates(cur, src, order=1, mode="constant", cval=0.0)
        fc.append((t0 + lead * 60, f.astype(np.float32)))

    # Rain at home
    now_dbz = _home_max(obs[3][1], cfg)
    lead_dbz = [_home_max(f, cfg) for _, f in fc]
    note = ""
    if now_dbz >= RAIN_DBZ:
        stop = next((fc[i][0] for i in range(len(fc) - 1)
                     if lead_dbz[i] < RAIN_DBZ and lead_dbz[i + 1] < RAIN_DBZ), None)
        note = (f"Raining at home now, easing ~{{t:{int(stop)}}} ({{in:{int(stop)}}})"
                if stop else "Raining at home now, for at least the next 90 min")
    else:
        start = next((fc[i][0] for i in range(len(fc)) if lead_dbz[i] >= RAIN_DBZ), None)
        if start:
            note = f"Rain reaching home ~{{t:{int(start)}}} ({{in:{int(start)}}})"
        else:
            # Anything within ~100 km?
            r0, r1, c0, c1, _ = _window(cfg)
            hr = int(round((LAT0 - cfg["home_lat"]) / STEP)) - r0
            hc = int(round((cfg["home_lon"] - LON0) / STEP)) - c0
            k = 90
            win = obs[3][1][max(0, hr - k):hr + k, max(0, hc - k):hc + k]
            if np.nanmax(np.nan_to_num(win)) >= RAIN_DBZ:
                note = "Rain nearby, not reaching home in the next 90 min"

    if not publish_out:
        return obs, fc
    frames = [(t, PAL.index(a)) for t, a in obs] + [(t, PAL.index(a)) for t, a in fc]
    _, _, _, _, bbox = _window(cfg)
    publish(cfg, "nowcast", frames, PAL, bbox, note=note, note_rgb=[0.6, 0.85, 1.0],
            legend=PAL.legend("Radar dBZ (observed, then forecast)",
                              [20, 30, 40, 50, 60, 70]),
            label="relative", now_index=3, frame_ms=650, hold_ms=2500,
            expires_s=1200, extra={"t0": int(t0)})
    log.info("nowcast t0=%s home now %.0f dBZ, leads %s", time.strftime(
        "%H:%MZ", time.gmtime(t0)), now_dbz, [round(x) for x in lead_dbz])
    return 300
