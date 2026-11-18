"""Shared helpers: config, downloads, products (frames + manifest), palettes.

A product is a directory under the output folder with a manifest.json and
one raw frame file per time step. Frames are 8-bit palette indices
(width x height bytes, row 0 = north edge), so the Pi can keep many of
them in memory and draw them without decoding images. Index 0 is
transparent; the palette (up to 256 RGBA entries) is in the manifest.
"""

import json
import logging
import math
import os
import tempfile
import threading
import time

import numpy as np
import requests

log = logging.getLogger("pchelper")

DEFAULTS = {
    "home_lat": 35.69,
    "home_lon": -78.71,
    "radar": "KRAX",            # nearest NEXRAD for bird migration
    "port": 8765,
    "out_dir": os.path.join(os.path.dirname(__file__), "..", "out"),
    "cache_dir": os.path.join(os.path.dirname(__file__), "..", "cache"),
    "mistnet_model": os.path.join(os.path.dirname(__file__), "..", "models",
                                  "mistnet_nexrad.pt"),
    "jobs": ["nowcast", "hrrr", "birds"],
}


def load_config(path):
    cfg = dict(DEFAULTS)
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            cfg.update(json.load(f))
    for k in ("out_dir", "cache_dir", "mistnet_model"):
        cfg[k] = os.path.abspath(cfg[k])
    os.makedirs(cfg["out_dir"], exist_ok=True)
    os.makedirs(cfg["cache_dir"], exist_ok=True)
    return cfg


_session = threading.local()


def http():
    s = getattr(_session, "s", None)
    if s is None:
        s = requests.Session()
        s.headers["User-Agent"] = "pi-clock-helper/1.0 (+https://github.com/scimaksim/Pi-Clock)"
        _session.s = s
    return s


def get(url, timeout=60, tries=3, headers=None):
    """GET with retries; returns bytes, or None for 404."""
    last = None
    for i in range(tries):
        try:
            r = http().get(url, timeout=timeout, headers=headers)
            if r.status_code == 404:
                return None
            r.raise_for_status()
            return r.content
        except requests.RequestException as e:   # network or 5xx
            last = e
            time.sleep(2 + 3 * i)
    raise last


def s3_list(bucket, prefix, max_keys=1000):
    """Keys under a prefix in a public S3 bucket (one page is plenty here)."""
    import re
    url = (f"https://{bucket}.s3.amazonaws.com/?list-type=2&prefix={prefix}"
           f"&max-keys={max_keys}")
    body = get(url, timeout=30)
    if not body:
        return []
    return re.findall(r"<Key>([^<]+)</Key>", body.decode("utf-8", "replace"))


# ── Palettes ──────────────────────────────────────────────────────────

def hex_rgba(h, a=255):
    h = h.lstrip("#")
    return [int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a]


class Palette:
    """Maps a value range onto palette indices 1..n (0 = transparent).

    stops: [(value, "#rrggbb", alpha 0-255), ...] ascending. Values below
    `vmin_show` are transparent. Colours are interpolated between stops
    over n steps.
    """

    def __init__(self, stops, vmin_show, n=200, log=False):
        self.log = log
        self.stops = stops
        # interpolation happens in log10 space for log palettes
        self._s = [(math.log10(v) if log else v, c, a) for v, c, a in stops]
        self.vmin_show = vmin_show
        self.v0 = self._s[0][0]
        self.v1 = self._s[-1][0]
        self.n = n
        vals = np.linspace(self.v0, self.v1, n)
        self.colours = [[0, 0, 0, 0]]
        for v in vals:
            self.colours.append(self._colour(v))

    def _colour(self, v):
        s = self._s
        for i in range(1, len(s)):
            if v <= s[i][0] or i == len(s) - 1:
                a, b = s[i - 1], s[i]
                t = 0 if b[0] == a[0] else min(1, max(0, (v - a[0]) / (b[0] - a[0])))
                ca, cb = hex_rgba(a[1], a[2]), hex_rgba(b[1], b[2])
                return [round(ca[k] + (cb[k] - ca[k]) * t) for k in range(4)]
        return hex_rgba(s[-1][1], s[-1][2])

    def index(self, values):
        v = np.asarray(values, dtype=np.float32)
        with np.errstate(all="ignore"):
            lv = np.log10(np.maximum(v, 1e-9)) if self.log else v
        idx = np.rint((lv - self.v0) / (self.v1 - self.v0) * (self.n - 1)) + 1
        idx = np.clip(np.nan_to_num(idx, nan=0), 1, self.n).astype(np.uint8)
        idx[~(v >= self.vmin_show)] = 0          # also NaN
        return idx

    def legend(self, title, ticks, unit=""):
        return {
            "title": title,
            "unit": unit,
            "stops": [[float(v), c, int(a)] for v, c, a in self.stops if v >= self.vmin_show
                      or v == self.stops[-1][0]],
            "ticks": [float(t) for t in ticks],
            "log": self.log,
        }


# ── Products ──────────────────────────────────────────────────────────

def _atomic_write(path, data):
    d = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tmp-")
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def publish(cfg, product, frames, palette, bbox, *, note="", note_rgb=None,
            legend=None, animate=True, frame_ms=600, hold_ms=2500,
            label="clock", now_index=None, extra=None, expires_s=3600):
    """Write a product: frames = [(epoch, uint8 array HxW), ...].

    bbox = (lon0, lat0, lon1, lat1) of the frame edges (row 0 = lat1).
    note may contain {t:EPOCH} (local clock time) and {in:EPOCH}
    ("in 35 min") tokens, formatted on the Pi so they never go stale.
    """
    pdir = os.path.join(cfg["out_dir"], product)
    os.makedirs(pdir, exist_ok=True)
    gen = int(time.time())
    files = []
    h = w = 0
    for i, (t, arr) in enumerate(frames):
        arr = np.ascontiguousarray(arr, dtype=np.uint8)
        h, w = arr.shape
        name = f"{gen}_{i:02d}.u8"
        _atomic_write(os.path.join(pdir, name), arr.tobytes())
        files.append({"file": name, "time": int(t)})
    man = {
        "product": product,
        "generated": gen,
        "expires": gen + int(expires_s),
        "bbox": [round(float(x), 5) for x in bbox],
        "width": w,
        "height": h,
        "palette": palette.colours if palette else [[0, 0, 0, 0]],
        "frames": files,
        "animate": bool(animate and len(files) > 1),
        "frame_ms": frame_ms,
        "hold_ms": hold_ms,
        "label": label,            # "clock" | "relative" | "none"
        "now_index": now_index,
        "legend": legend,
        "note": note,
        "note_rgb": note_rgb or [0.85, 0.9, 1.0],
    }
    if extra:
        man.update(extra)
    _atomic_write(os.path.join(pdir, "manifest.json"),
                  json.dumps(man, separators=(",", ":")).encode())
    # Drop older generations (keep the previous one a little while for
    # a Pi that is mid-download)
    for f in os.listdir(pdir):
        if f.endswith(".u8") and not f.startswith(str(gen)):
            try:
                g = int(f.split("_")[0])
                if gen - g > 600:
                    os.remove(os.path.join(pdir, f))
            except (ValueError, OSError):
                pass
    log.info("%s: published %d frames %dx%d", product, len(files), w, h)


# ── Geometry and time ─────────────────────────────────────────────────

def sun_altitude(lat, lon, t):
    """Solar altitude in degrees (low precision, fine for dusk/dawn)."""
    n = t / 86400.0 + 2440587.5 - 2451545.0
    L = math.radians((280.460 + 0.9856474 * n) % 360)
    g = math.radians((357.528 + 0.9856003 * n) % 360)
    lam = L + math.radians(1.915) * math.sin(g) + math.radians(0.020) * math.sin(2 * g)
    eps = math.radians(23.439 - 0.0000004 * n)
    ra = math.atan2(math.cos(eps) * math.sin(lam), math.cos(lam))
    dec = math.asin(math.sin(eps) * math.sin(lam))
    gmst = (18.697374558 + 24.06570982441908 * n) % 24
    ha = math.radians(gmst * 15 + lon) - ra
    la = math.radians(lat)
    return math.degrees(math.asin(math.sin(la) * math.sin(dec) +
                                  math.cos(la) * math.cos(dec) * math.cos(ha)))


def dist_km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    c = math.sin(p1) * math.sin(p2) + math.cos(p1) * math.cos(p2) * math.cos(dl)
    return 6371.0 * math.acos(max(-1.0, min(1.0, c)))


def compass(deg):
    pts = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
           "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
    return pts[int((deg % 360) / 22.5 + 0.5) % 16]
