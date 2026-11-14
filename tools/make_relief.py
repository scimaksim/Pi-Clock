#!/usr/bin/env python3
"""Shaded-relief factor maps for the Moon and Mars planet views.

Inputs (public domain, NASA):
  Moon: LRO LOLA ldem_16.tif (SVS CGI Moon Kit, 16 px/deg, km)
        https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/ldem_16.tif
  Mars: MGS MOLA MEGDR megt90n000eb.img (16 px/deg, m, MSB int16, 0E at left)
        https://pds-geosciences.wustl.edu/mgs/mgs-m-mola-5-megdr-l3-v1/mgsl_300x/meg016/megt90n000eb.img
Output: renderer/worlds/<world>_relief.jpg, 3840x1920 grey, 128 = unchanged,
lit from the north-west at 45 degrees; the renderer multiplies it into the
colour map (VIEW_WORLD_RELIEF).
Usage: make_relief.py ldem_16.tif megt90n000eb.img outdir
"""
import math, sys
import numpy as np
import tifffile
from PIL import Image

def relief(dem_km, R, gain, lo=0.45, hi=1.6):
    H, W = dem_km.shape
    lat = np.radians(90 - (np.arange(H) + 0.5) * 180 / H)[:, None]
    dx = 2 * math.pi * R / W * np.maximum(np.cos(lat), 0.02); dy = math.pi * R / H
    gx = (np.roll(dem_km, -1, 1) - np.roll(dem_km, 1, 1)) / (2 * dx)
    gy = np.zeros_like(dem_km); gy[1:-1] = (dem_km[:-2] - dem_km[2:]) / (2 * dy)
    n = np.stack([-gx, -gy, np.ones_like(gx)]); n /= np.linalg.norm(n, axis=0)
    az, el = math.radians(315), math.radians(45)
    le, ln, lu = math.sin(az) * math.cos(el), math.cos(az) * math.cos(el), math.sin(el)
    return np.clip(1 + gain * (n[0] * le + n[1] * ln + n[2] * lu - lu), lo, hi)

moon = tifffile.imread(sys.argv[1]).astype(np.float64)                      # -180 at left
mars = np.fromfile(sys.argv[2], dtype='>i2').reshape(2880, 5760).astype(np.float64) / 1000
mars = np.roll(mars, -2880, axis=1)                                          # 0E -> -180 at left
for name, f in (('moon', relief(moon, 1737.4, 1.6)), ('mars', relief(mars, 3389.5, 2.6))):
    Image.fromarray(np.clip(f * 128, 0, 255).astype(np.uint8)).resize((3840, 1920), Image.LANCZOS) \
         .save('%s/%s_relief.jpg' % (sys.argv[3], name), quality=92)
