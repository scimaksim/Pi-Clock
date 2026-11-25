"""Besselian elements (solar) and shadow elements (lunar) for 2024-2040,
from JPL DE421 via Skyfield. Polynomials are in UT hours from t0 (UT), so
the renderer needs no Delta-T. Validated in check.py."""
import numpy as np, json, sys
from skyfield.api import load
from skyfield import almanac, eclipselib

ts = load.timescale()
eph = load('de421.bsp')
earth, sun, moon = eph['earth'], eph['sun'], eph['moon']
RE = 6378.137                 # km
AU = 149597870.7
K_PEN = 0.272488              # Moon radius / Earth radius (penumbra, NASA)
K_UMB = 0.272281              # smaller value used for umbra/central (NASA)
RS = 696000.0 / RE            # solar radius in Earth radii (959.63")

def vec(body, t):
    """apparent geocentric equatorial-of-date position in Earth radii"""
    ra, dec, dist = earth.at(t).observe(body).apparent().radec(epoch='date')
    r = dist.km / RE
    a, d = ra.radians, dec.radians
    return np.array([r*np.cos(d)*np.cos(a), r*np.cos(d)*np.sin(a), r*np.sin(d)]), a, d, r

def bessel(t):
    S, _, _, _ = vec(sun, t)
    M, am, dm, rm = vec(moon, t)
    G = S - M
    gl = np.linalg.norm(G)
    g = G / gl
    d = np.arcsin(g[2]); a = np.arctan2(g[1], g[0])
    x = rm*np.cos(dm)*np.sin(am - a)
    y = rm*(np.sin(dm)*np.cos(d) - np.cos(dm)*np.sin(d)*np.cos(am - a))
    z = rm*(np.sin(dm)*np.sin(d) + np.cos(dm)*np.cos(d)*np.cos(am - a))
    f1 = np.arcsin((RS + K_PEN) / gl)
    f2 = np.arcsin((RS - K_UMB) / gl)
    l1 = z*np.tan(f1) + K_PEN/np.cos(f1)
    l2 = z*np.tan(f2) - K_UMB/np.cos(f2)
    mu = (t.gast*15.0 - np.degrees(a)) % 360.0     # GAST at UT -> no Delta-T term
    return x, y, np.degrees(d), mu, l1, l2, np.tan(f1), np.tan(f2)

def fit(ts_h, vals, deg=3):
    return [float(c) for c in np.polyfit(ts_h, vals, deg)[::-1]]

def solar(t_nm):
    # minimise sqrt(x^2+y^2) around the new moon
    jd0 = t_nm.ut1
    def m_at(jd):
        b = bessel(ts.ut1_jd(jd)); return np.hypot(b[0], b[1])
    lo, hi = jd0 - 0.25, jd0 + 0.25
    for _ in range(60):
        m1, m2 = lo + (hi-lo)/3, hi - (hi-lo)/3
        if m_at(m1) < m_at(m2): hi = m2
        else: lo = m1
    jdm = (lo+hi)/2
    b = bessel(ts.ut1_jd(jdm))
    gamma = np.hypot(b[0], b[1]) * (1 if b[1] >= 0 else -1)
    if abs(gamma) > 1.0 + b[4] + 0.02:     # shadow misses Earth
        return None
    # t0 = nearest whole UT hour
    t0 = np.round(jdm*24)/24
    hs = np.linspace(-4, 4, 97)
    B = np.array([bessel(ts.ut1_jd(t0 + h/24)) for h in hs])
    mu = np.degrees(np.unwrap(np.radians(B[:, 3])))
    el = dict(t0=float((t0 - 2440587.5)*86400.0),
              x=fit(hs, B[:,0]), y=fit(hs, B[:,1]), d=fit(hs, B[:,2], 2),
              mu=fit(hs, mu, 1), l1=fit(hs, B[:,4], 2), l2=fit(hs, B[:,5], 2),
              tf1=float(B[48,6]), tf2=float(B[48,7]), gamma=float(gamma),
              tmax=float((jdm - 2440587.5)*86400.0))
    # residuals of the fit
    def ev(c, h): return sum(ci*h**i for i, ci in enumerate(c))
    el['fit_err'] = float(max(np.max(abs(ev(el['x'],hs)-B[:,0])), np.max(abs(ev(el['y'],hs)-B[:,1])),
                              np.max(abs(ev(el['l2'],hs)-B[:,5]))))
    el['fit_err_mu'] = float(np.max(abs(ev(el['mu'],hs)-mu)))
    return el

def lunar_elems(t):
    _, asun, dsun, rs = vec(sun, t)
    _, am, dm, rm = vec(moon, t)
    a0, d0 = asun + np.pi, -dsun
    # Moon relative to the shadow centre, on the sky (degrees)
    x = np.degrees(np.arctan2(np.sin(am - a0)*np.cos(dm), 1))  # ~ (am-a0)cos d
    x = np.degrees(((am - a0 + np.pi) % (2*np.pi) - np.pi) * np.cos(dm))
    y = np.degrees(dm - d0)
    pim = np.degrees(np.arcsin(1/rm)); pis = np.degrees(np.arcsin(1/rs))
    ss = np.degrees(np.arcsin(RS/rs))
    sm = np.degrees(np.arcsin(K_PEN/rm))
    ru = 1.02*(0.998340*pim - ss + pis)
    rp = 1.02*(0.998340*pim + ss + pis)
    gha = (t.gast*15.0 - np.degrees(am)) % 360.0
    return x, y, ru, rp, sm, np.degrees(dm), gha

def lunar(t_max):
    t0 = np.round(t_max.ut1*24)/24
    hs = np.linspace(-4, 4, 97)
    L = np.array([lunar_elems(ts.ut1_jd(t0 + h/24)) for h in hs])
    gha = np.degrees(np.unwrap(np.radians(L[:,6])))
    return dict(t0=float((t0 - 2440587.5)*86400.0), x=fit(hs, L[:,0]), y=fit(hs, L[:,1]),
                ru=fit(hs, L[:,2], 2), rp=fit(hs, L[:,3], 2), sm=fit(hs, L[:,4], 2),
                dec=fit(hs, L[:,5], 2), gha=fit(hs, gha, 1))

out = dict(solar=[], lunar=[])
import os
t1 = ts.utc(int(os.environ.get('Y1', 1900)), 1, 1); t2 = ts.utc(int(os.environ.get('Y2', 2051)), 1, 1)
tt, ph = almanac.find_discrete(t1, t2, almanac.moon_phases(eph))
for t, p in zip(tt, ph):
    if p == 0:
        e = solar(t)
        if e:
            e['date'] = t.utc_strftime('%Y-%m-%d'); out['solar'].append(e)
lt, ly, ld = eclipselib.lunar_eclipses(t1, t2, eph)
for t, y in zip(lt, ly):
    e = lunar(t); e['date'] = t.utc_strftime('%Y-%m-%d'); e['tmax'] = float((t.ut1 - 2440587.5)*86400.0)
    e['kind'] = int(y)   # 0 penumbral, 1 partial, 2 total
    e['umag'] = float(ld['umbral_magnitude'][list(lt.tt).index(t.tt)])
    out['lunar'].append(e)
json.dump(out, open('elements.json', 'w'))
print(len(out['solar']), 'solar', len(out['lunar']), 'lunar')
print('max fit err', max(e['fit_err'] for e in out['solar']), max(e['fit_err_mu'] for e in out['solar']))
