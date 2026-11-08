#!/usr/bin/env python3
"""Cut an elevation patch around a rover for the Mars close-up view.

The renderer traces each column of the rover's latest picture over this
patch to outline the ground actually in the picture.

Inputs: a USGS/JPL GeoTIFF elevation model in Mars equirectangular
metres (sphere R = 3396190 m, optional standard parallel), e.g.
  Gale:   MSL_Gale_DEM_Mosaic_1m_v3.tif   (1 m, LZW, 3.6 GB)
  Jezero: M20_JezeroCrater_CTXDEM_20m.tif (20 m, 90 MB)
Only the rows needed are read, so the big file is fine.

Output "PDEM": 4-byte magic, int32 w, h, float64 centre lat, lon, step (m),
then w*h float32 metres, row 0 = north edge, grid in local north/east
metres around the centre. Copy it to the clock as
/data/cache/pi-clock-worlds/<MSL|M20>_dem.bin

Usage: dem_patch.py dem.tif lat lon out.bin [size_m=6000] [step_m=4]
"""
import math, struct, sys
import numpy as np
import tifffile

R = 3396190.0


def main():
    path, lat, lon, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    size = float(sys.argv[5]) if len(sys.argv) > 5 else 6000.0
    step = float(sys.argv[6]) if len(sys.argv) > 6 else 4.0
    with tifffile.TiffFile(path) as t:
        p = t.pages[0]
        tags = {tag.code: tag.value for tag in p.tags.values()}
        sx, sy = tags[33550][0], tags[33550][1]
        x0, y0 = tags[33922][3], tags[33922][4]
        dbl = tags.get(34736, ())
        ts = next((d for d in dbl if 0.5 < abs(d) < 89.5), 0.0)       # standard parallel, if any
        H, W = p.shape
        offs, cnts = p.dataoffsets, p.databytecounts
        rows_per = len(offs) and math.ceil(H / len(offs))
        if p.is_tiled or rows_per != 1:
            raise SystemExit('expected one row per strip')
        lzw = p.compression == 5
        if lzw:
            import imagecodecs

        def x_of(lo): return R * math.cos(math.radians(ts)) * math.radians(lo)
        def y_of(la): return R * math.radians(la)

        n = int(size / step)
        coslat = math.cos(math.radians(lat))
        north = (np.arange(n) - n / 2 + 0.5)[::-1] * step               # row 0 = north
        east = (np.arange(n) - n / 2 + 0.5) * step
        lats = lat + np.degrees(north / R)
        lons = lon + np.degrees(east / (R * coslat))
        rows = np.clip(((y0 - np.array([y_of(a) for a in lats])) / sy).astype(int), 0, H - 1)
        cols = np.clip(((np.array([x_of(o) for o in lons]) - x0) / sx).astype(int), 0, W - 1)
        fh = t.filehandle
        grid = np.empty((n, n), np.float32)
        cache = {}
        for i, r in enumerate(rows):
            if r not in cache:
                fh.seek(offs[r])
                raw = fh.read(cnts[r])
                if lzw:
                    raw = imagecodecs.lzw_decode(raw)
                cache.clear()
                cache[r] = np.frombuffer(raw[:W * 4], dtype=p.dtype.newbyteorder(t.byteorder))
            grid[i] = cache[r][cols]
    bad = (grid < -20000) | (grid > 30000) | ~np.isfinite(grid)
    if bad.all():
        raise SystemExit('no elevation data there')
    grid[bad] = np.nan
    with open(out, 'wb') as f:
        f.write(b'PDEM' + struct.pack('<ii', n, n) + struct.pack('<ddd', lat, lon, step))
        f.write(grid.astype('<f4').tobytes())
    print('%s: %dx%d at %.0f m, %.0f%% filled, %.0f..%.0f m' %
          (out, n, n, step, 100 * (1 - bad.mean()), np.nanmin(grid), np.nanmax(grid)))


if __name__ == '__main__':
    main()
