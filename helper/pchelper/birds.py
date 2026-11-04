"""Night bird migration from the local NEXRAD (default KRAX, Raleigh).

At night, each new radar volume (Level II, every 5-10 min, Unidata on
AWS) is run through MistNet (Lin et al. 2019), the deep-learning model
that separates birds from rain in US weather radar, used by vol2bird
and BirdCast. Its input is replicated from vol2bird: the 0.5-4.5 degree
tilts rendered to a 608x608, 500 m grid (reflectivity, radial velocity,
spectrum width for each). Pixels it calls weather (mean probability
over the tilts > 0.45, plus a 5 km margin) are dropped.

From the bird pixels:
  * a map of bird density at the lowest tilt (birds/km^3, from
    reflectivity with vol2bird's 11 cm^2 per bird), and
  * a vertical profile 0-3 km within 5-35 km of the radar: density and
    a VAD wind fit per 200 m layer, giving birds per km^2 aloft, the
    migration traffic rate (birds crossing 1 km per hour), heading and
    ground speed.

Product "birds": the last hour of scans as frames and a note. By day,
no frames and a summary of last night's peak.
"""

import datetime as dt
import json
import logging
import math
import os
import tempfile
import time

import numpy as np
from scipy import ndimage

from .common import Palette, compass, get, publish, s3_list, sun_altitude

log = logging.getLogger("pchelper.birds")

BUCKET = "unidata-nexrad-level2"
ELEVS = [0.5, 1.5, 2.5, 3.5, 4.5]
DIM, RES = 608, 500.0
LAMBDA_CM = 10.7                          # NEXRAD S band
DBZ_FACTOR = (0.964 ** 2) * 1000 * math.pi ** 5 / LAMBDA_CM ** 4
SIGMA_BIRD = 11.0                         # cm^2 per bird
ETA_MAX = 36000.0                         # vol2bird default: above = not birds
RE = 6371200.0 * 4 / 3                    # effective earth radius, m
MAP_STEP = 0.005                          # output map, degrees
MAP_KM = 150.0

PAL = Palette([(5, "#3fa7c9", 35), (20, "#58c98a", 140), (50, "#d9d84a", 180),
               (100, "#f2a33a", 200), (200, "#ea5a35", 215), (500, "#c9369a", 230)],
              vmin_show=5, log=True)

_model = None
_last_key = None
_frames = []        # [(epoch, uint8 map)]
_stats_path = None


def _load_model(cfg):
    global _model
    if _model is None:
        import torch
        dev = "cuda" if torch.cuda.is_available() else "cpu"
        _model = (torch.jit.load(cfg["mistnet_model"], map_location=dev).eval(), dev)
        log.info("MistNet loaded on %s", dev)
    return _model


def _radar_site(cfg, radar):
    return radar.latitude["data"][0], radar.longitude["data"][0]


def _sweep_for(radar, field, elev):
    """Sweep at the tilt closest to elev (as vol2bird picks them: clear-air
    VCPs have e.g. 0.48/1.27/2.42/3.08/4.0) with the most valid gates of
    `field` (split cuts keep reflectivity and velocity in separate
    sweeps), or None."""
    angles = radar.fixed_angle["data"]
    if field not in radar.fields or not len(angles):
        return None
    near = min(angles, key=lambda a: abs(a - elev))
    if abs(near - elev) > 0.8:
        return None
    best, best_n = None, 0
    for s in range(radar.nsweeps):
        if abs(angles[s] - near) > 0.05:
            continue
        d = radar.get_field(s, field)
        n = int(np.ma.count(d))
        if n > best_n:
            best, best_n = s, n
    return best


def _to_grid(radar, sweep, field, elev_deg):
    """vol2bird's polarScanToCartesian: 608x608, 500 m, [x=north][y=east]."""
    out = np.full((DIM, DIM), np.nan, np.float32)
    if sweep is None:
        return out
    data = radar.get_field(sweep, field)
    vals = np.ma.filled(data.astype(np.float32), np.nan)
    az = radar.get_azimuth(sweep)
    rng = radar.range["data"]
    order = np.argsort(az)
    az_s = az[order]
    i = (np.arange(DIM) - DIM // 2) * RES
    xx, yy = np.meshgrid(i, i, indexing="ij")            # xx north, yy east
    bearing = (np.degrees(np.arctan2(yy, xx)) + 360) % 360
    s = np.hypot(xx, yy)
    el = math.radians(elev_deg)
    gamma = s / RE
    beta = math.pi - (math.pi / 2 + el) - gamma
    r = RE * np.sin(gamma) / np.sin(beta)
    # nearest ray (with wrap) and gate
    k = np.searchsorted(az_s, bearing) % len(az_s)
    km1 = (k - 1) % len(az_s)
    d1 = np.abs((az_s[k] - bearing + 180) % 360 - 180)
    d0 = np.abs((az_s[km1] - bearing + 180) % 360 - 180)
    ray = order[np.where(d0 < d1, km1, k)]
    dr = rng[1] - rng[0]
    gate = np.rint((r - rng[0]) / dr).astype(int)
    ok = (gate >= 0) & (gate < len(rng))
    out[ok] = vals[ray[ok], gate[ok]]
    return out


def _tensor(radar):
    chans = np.full((15, DIM, DIM), np.nan, np.float32)
    for e, elev in enumerate(ELEVS):
        sz = _sweep_for(radar, "reflectivity", elev)
        sv = _sweep_for(radar, "velocity", elev)
        if sz is None or sv is None:
            raise RuntimeError(f"no {elev} deg tilt")
        ez = radar.fixed_angle["data"][sz]
        ev = radar.fixed_angle["data"][sv]
        chans[e] = _to_grid(radar, sz, "reflectivity", ez)
        chans[5 + e] = _to_grid(radar, sv, "velocity", ev)
        chans[10 + e] = _to_grid(radar, sv, "spectrum_width", ev)
        # vol2bird: velocity and width only where reflectivity exists
        miss = np.isnan(chans[e])
        chans[5 + e][miss] = np.nan
        chans[10 + e][miss] = np.nan
    return chans


def _segment(cfg, chans):
    import torch
    model, dev = _load_model(cfg)
    with torch.no_grad():
        x = torch.from_numpy(chans[None]).to(dev)
        y = model(x)[0].float().cpu().numpy()          # 3 x 5 x 608 x 608
    weather = y[2].mean(axis=0) > 0.45
    weather = ndimage.binary_dilation(weather, iterations=10)   # 5 km margin
    return weather


def _profile(chans, weather):
    """Layer density and VAD winds 0-3 km from the 5 tilts, 5-35 km out."""
    i = (np.arange(DIM) - DIM // 2) * RES
    xx, yy = np.meshgrid(i, i, indexing="ij")
    s = np.hypot(xx, yy)
    bearing = np.arctan2(yy, xx)
    ring = (s >= 5000) & (s <= 35000) & ~weather
    layers = []
    for top in range(200, 3001, 200):
        layers.append({"h": top - 100, "eta_sum": 0.0, "n": 0, "A": [], "b": []})
    for e, elev in enumerate(ELEVS):
        el = math.radians(elev)
        h = s * math.sin(el) + s ** 2 / (2 * RE)
        dbz, vr = chans[e], chans[5 + e]
        valid = ring & ~np.isnan(dbz)
        eta = DBZ_FACTOR * 10 ** (np.nan_to_num(dbz, nan=-30) / 10)
        bird = valid & (eta <= ETA_MAX)
        for li, L in enumerate(layers):
            m = valid & (h >= L["h"] - 100) & (h < L["h"] + 100)
            if not m.any():
                continue
            L["n"] += int(m.sum())
            L["eta_sum"] += float(eta[m & bird].sum())
            v = m & bird & ~np.isnan(vr) & (dbz > -5)
            if v.any():
                b = bearing[v]
                L["A"].append(np.stack([np.sin(b) * math.cos(el), np.cos(b) * math.cos(el)], 1))
                L["b"].append(vr[v])
    vid = mtr = 0.0
    u_w = v_w = w_sum = ff_w = 0.0
    for L in layers:
        if L["n"] < 200:
            continue
        dens = L["eta_sum"] / L["n"] / SIGMA_BIRD          # birds/km^3
        vid += dens * 0.2
        if L["A"]:
            A = np.concatenate(L["A"])
            b = np.concatenate(L["b"])
            if len(b) >= 100:
                (u, v), *_ = np.linalg.lstsq(A, b, rcond=None)
                ff = math.hypot(u, v)
                if ff < 30:                                  # plausible flight speed
                    mtr += dens * ff * 3.6 * 0.2             # birds / km / h
                    u_w += dens * u
                    v_w += dens * v
                    ff_w += dens * ff
                    w_sum += dens
    heading = (math.degrees(math.atan2(u_w, v_w)) + 360) % 360 if w_sum else None
    speed = ff_w / w_sum if w_sum else None           # density-weighted mean speed
    return vid, mtr, heading, speed


def _map(chans, weather, lat0, lon0):
    """Density map at the lowest tilt, as a lat/lon image around the radar."""
    dlat = MAP_KM / 110.57
    dlon = MAP_KM / (111.32 * math.cos(math.radians(lat0)))
    h = int(2 * dlat / MAP_STEP)
    w = int(2 * dlon / MAP_STEP)
    lats = lat0 + dlat - (np.arange(h) + 0.5) * MAP_STEP
    lons = lon0 - dlon + (np.arange(w) + 0.5) * MAP_STEP
    LO, LA = np.meshgrid(lons, lats)
    north = (LA - lat0) * 110.57e3
    east = (LO - lon0) * 111.32e3 * math.cos(math.radians(lat0))
    gi = np.rint(north / RES).astype(int) + DIM // 2
    gj = np.rint(east / RES).astype(int) + DIM // 2
    ok = (gi >= 0) & (gi < DIM) & (gj >= 0) & (gj < DIM)
    dist = np.hypot(north, east)
    dens = np.full((h, w), np.nan, np.float32)
    dbz = chans[0]
    eta = DBZ_FACTOR * 10 ** (np.nan_to_num(dbz, nan=-30) / 10)
    # Measured gates outside weather count (zero where no bird echo);
    # a 1.5 km Gaussian (normalised by coverage) turns the 500 m speckle
    # into the familiar smooth "bloom" around the radar
    measured = ~np.isnan(dbz) & ~weather
    val = np.where(measured & (eta <= ETA_MAX), eta / SIGMA_BIRD, 0.0)
    wsm = ndimage.gaussian_filter(measured.astype(np.float32), 3)
    vsm = ndimage.gaussian_filter((val * measured).astype(np.float32), 3)
    with np.errstate(all="ignore"):
        grid = np.where(wsm > 0.25, vsm / wsm, np.nan)
    sel = ok & (dist >= 5000) & (dist <= MAP_KM * 1000)
    dens[sel] = grid[gi[sel], gj[sel]]
    bbox = (lon0 - dlon, lat0 - dlat, lon0 + dlon, lat0 + dlat)
    return dens, bbox


def _stats(cfg):
    global _stats_path
    _stats_path = os.path.join(cfg["cache_dir"], "birds_night.json")
    try:
        with open(_stats_path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _save_stats(st):
    with open(_stats_path, "w") as f:
        json.dump(st, f)


def run(cfg):
    global _last_key, _frames
    import pyart

    radar_id = cfg["radar"]
    now = time.time()
    st = _stats(cfg)
    site = st.get("site")
    lat, lon = (site or (cfg["home_lat"], cfg["home_lon"]))
    night = sun_altitude(lat, lon, now) < -6

    if not night:
        _frames = []
        note = ""
        if st.get("peak_mtr", 0) >= 20 and now - st.get("peak_t", 0) < 20 * 3600:
            note = (f"Last night ({radar_id}): peak ~{st['peak_mtr']:,.0f} birds/km/h around "
                    f"{{t:{st['peak_t']}}}, heading {st.get('peak_dir', '?')}")
        publish(cfg, "birds", [], PAL, (lon - 1, lat - 1, lon + 1, lat + 1), note=note,
                note_rgb=[0.75, 0.95, 0.75], legend=None, animate=False,
                label="none", expires_s=3600)
        return 600

    # Start of a new night: reset the peak
    if now - st.get("night_start", 0) > 14 * 3600:
        st = {"night_start": now, "site": site}

    keys = []
    for back in (0, 1):
        d = dt.datetime.fromtimestamp(now - back * 86400, dt.timezone.utc)
        keys += [k for k in s3_list(BUCKET, f"{d:%Y/%m/%d}/{radar_id}/")
                 if k.endswith("_V06")]
    if not keys:
        raise RuntimeError(f"no {radar_id} volumes")
    key = max(keys)
    vt = dt.datetime.strptime(key.rsplit("/", 1)[1][4:19], "%Y%m%d_%H%M%S").replace(
        tzinfo=dt.timezone.utc).timestamp()
    if key == _last_key:
        return 120
    if now - vt > 1800:
        raise RuntimeError(f"{radar_id} newest volume is {int((now - vt) / 60)} min old")
    raw = get(f"https://{BUCKET}.s3.amazonaws.com/{key}", timeout=120)
    fd, path = tempfile.mkstemp(suffix=".ar2v", dir=cfg["cache_dir"])
    with os.fdopen(fd, "wb") as f:
        f.write(raw)
    try:
        radar = pyart.io.read_nexrad_archive(path)
    finally:
        os.remove(path)
    rlat, rlon = _radar_site(cfg, radar)
    st["site"] = [float(rlat), float(rlon)]

    chans = _tensor(radar)
    weather = _segment(cfg, chans)
    vid, mtr, heading, speed = _profile(chans, weather)
    dens, bbox = _map(chans, weather, rlat, rlon)
    _frames = [f for f in _frames if vt - f[0] < 3600 and f[0] != vt] + [(vt, PAL.index(dens))]
    _last_key = key

    if mtr >= 20 and heading is not None:
        note = (f"Birds aloft ({radar_id} {{t:{int(vt)}}}): ~{mtr:,.0f} birds/km/h heading "
                f"{compass(heading)} at {speed * 2.237:.0f} mph, {vid:,.0f}/km²")
    else:
        note = f"Little bird migration over {radar_id} ({{t:{int(vt)}}})"
    if mtr > st.get("peak_mtr", 0):
        st.update(peak_mtr=float(mtr), peak_t=int(vt),
                  peak_dir=compass(heading) if heading is not None else "?")
    _save_stats(st)

    publish(cfg, "birds", _frames, PAL, bbox, note=note, note_rgb=[0.75, 0.95, 0.75],
            legend=PAL.legend(f"Birds aloft per km³ ({radar_id}, MistNet)",
                              [5, 20, 50, 100, 200, 500]),
            label="clock", frame_ms=900, hold_ms=2500, expires_s=1800,
            extra={"vid": round(vid, 1), "mtr": round(mtr, 1),
                   "heading": heading, "speed_ms": speed})
    log.info("birds %s: vid %.0f/km2 mtr %.0f/km/h heading %s speed %s", key.rsplit("/", 1)[1],
             vid, mtr, heading and round(heading), speed and round(speed, 1))
    return 180
