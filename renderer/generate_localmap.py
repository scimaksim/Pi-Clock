#!/usr/bin/env python3
"""
generate_localmap.py - Build geodata/localmap.dat for the Local Detail layer

Inputs (Natural Earth 1:10m, public domain), GeoJSON from
https://github.com/nvkelso/natural-earth-vector/tree/master/geojson :
  ne_10m_coastline.geojson
  ne_10m_admin_0_boundary_lines_land.geojson
  ne_10m_admin_1_states_provinces_lines.geojson
  ne_10m_populated_places_simple.geojson

Usage: generate_localmap.py <dir with the geojson files> <out.dat>

Lines are simplified (Ramer-Douglas-Peucker, 0.003 deg ~ 300 m) so the
file stays a few MB. Format (little-endian):

  char     magic[8]   "PCLMAP1\\0"
  uint32   nlines
  per line:
    uint8  kind       0 coastline, 1 country border, 2 state/province
    uint8  pad[3]
    float  lon0, lat0, lon1, lat1   bounding box
    uint32 npts
    float  lon, lat   * npts
  uint32   nplaces
  per place:
    float  lon, lat
    uint32 pop
    uint8  rank       Natural Earth scalerank (0 = most important)
    uint8  flags      1 = national capital, 2 = state capital
    uint8  namelen
    char   name[namelen]   UTF-8, no terminator
"""

import json
import os
import struct
import sys

TOL = 0.003


def rdp(pts, tol):
    """Iterative Ramer-Douglas-Peucker on a list of (x, y)."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    tol2 = tol * tol
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        dd = dx * dx + dy * dy
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if dd == 0:
                d = (px - ax) ** 2 + (py - ay) ** 2
            else:
                t = ((px - ax) * dx + (py - ay) * dy) / dd
                t = max(0.0, min(1.0, t))
                d = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2
            if d > best:
                best, bi = d, i
        if best > tol2:
            keep[bi] = True
            stack.append((a, bi))
            stack.append((bi, b))
    return [p for p, k in zip(pts, keep) if k]


def lines_of(path):
    with open(path) as f:
        d = json.load(f)
    for ft in d["features"]:
        g = ft.get("geometry")
        if not g:
            continue
        if g["type"] == "LineString":
            yield g["coordinates"]
        elif g["type"] == "MultiLineString":
            yield from g["coordinates"]


def main():
    src, out = sys.argv[1], sys.argv[2]
    lines = []
    for kind, name in ((0, "ne_10m_coastline"),
                       (1, "ne_10m_admin_0_boundary_lines_land"),
                       (2, "ne_10m_admin_1_states_provinces_lines")):
        for coords in lines_of(os.path.join(src, name + ".geojson")):
            pts = rdp([(float(c[0]), float(c[1])) for c in coords], TOL)
            if len(pts) < 2:
                continue
            lines.append((kind, pts))

    with open(os.path.join(src, "ne_10m_populated_places_simple.geojson")) as f:
        places = json.load(f)["features"]

    with open(out, "wb") as f:
        f.write(b"PCLMAP1\0")
        f.write(struct.pack("<I", len(lines)))
        npts = 0
        for kind, pts in lines:
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            f.write(struct.pack("<B3x4fI", kind, min(xs), min(ys),
                                max(xs), max(ys), len(pts)))
            f.write(struct.pack("<%df" % (2 * len(pts)),
                                *[v for p in pts for v in p]))
            npts += len(pts)
        f.write(struct.pack("<I", len(places)))
        for ft in places:
            p = ft["properties"]
            name = (p.get("name") or "").encode("utf-8")[:63]
            flags = (1 if p.get("adm0cap") else 0) | \
                    (2 if "Admin-1 capital" in (p.get("featurecla") or "") else 0)
            lon, lat = ft["geometry"]["coordinates"][:2]
            f.write(struct.pack("<2fIBBB", lon, lat,
                                max(0, int(p.get("pop_max") or 0)),
                                int(p.get("scalerank") or 10), flags, len(name)))
            f.write(name)
    print("%s: %d lines, %d points, %d places, %d bytes" %
          (out, len(lines), npts, len(places), os.path.getsize(out)))


if __name__ == "__main__":
    main()
