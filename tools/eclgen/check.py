import json, math, datetime as dt
E = json.load(open('elements.json'))
F = 1/298.257223563; E2 = F*(2-F); B_A = 1-F
def ev(c, h): return sum(ci*h**i for i, ci in enumerate(c))
def el_at(e, t):
    h = (t - e['t0'])/3600.0
    return ev(e['x'],h), ev(e['y'],h), math.radians(ev(e['d'],h)), ev(e['mu'],h), ev(e['l1'],h), ev(e['l2'],h)
def local(e, t, lat, lon):
    x,y,d,mu,l1,l2 = el_at(e,t)
    u = math.atan(B_A*math.tan(math.radians(lat)))
    rs, rc = B_A*math.sin(u), math.cos(u)
    H = math.radians(mu + lon)
    xi = rc*math.sin(H); eta = rs*math.cos(d) - rc*math.cos(H)*math.sin(d)
    zeta = rs*math.sin(d) + rc*math.cos(H)*math.cos(d)
    L1 = l1 - zeta*e['tf1']; L2 = l2 - zeta*e['tf2']
    m = math.hypot(x-xi, y-eta)
    return m, L1, L2, zeta
def obscuration(m, L1, L2):
    if m >= L1: return 0.0
    A = (L1-L2)/(L1+L2)                     # moon/sun diameter ratio
    s = (m/(L1+L2))*2                        # separation in sun radii
    R, r = 1.0, A
    if s <= abs(R-r): return min(1.0, (r/R)**2)
    a1 = math.acos((s*s+R*R-r*r)/(2*s*R)); a2 = math.acos((s*s+r*r-R*R)/(2*s*r))
    area = R*R*a1 + r*r*a2 - 0.5*math.sqrt(max(0,(-s+R+r)*(s+R-r)*(s-R+r)*(s+R+r)))
    return area/(math.pi*R*R)
def central(e, t):
    x,y,d,mu,l1,l2 = el_at(e,t)
    rho1 = math.sqrt(1-E2*math.cos(d)**2)
    y1 = y/rho1; sd1 = math.sin(d)/rho1; cd1 = math.sqrt(1-E2)*math.cos(d)/rho1
    B = 1-x*x-y1*y1
    if B < 0: return None
    B = math.sqrt(B)
    sp1 = B*sd1 + y1*cd1
    th = math.degrees(math.atan2(x, B*cd1 - y1*sd1))
    phi = math.degrees(math.atan(math.tan(math.asin(sp1))/B_A))
    lon = (th - mu + 180) % 360 - 180
    return phi, lon
def ut(s): return dt.datetime.fromisoformat(s).replace(tzinfo=dt.timezone.utc).timestamp()
def find(date): return [e for e in E['solar'] if e['date']==date][0]
def hm(t): return dt.datetime.utcfromtimestamp(t).strftime('%H:%M:%S')

def main():
    for date, ref in [('2024-04-08','GE 18:17:16 25.29N 104.14W 4m28s'),('2024-10-02','GE 18:45 22.0S 114.5W ann 7m25s'),
                      ('2026-08-12','GE 17:46:06 65.2N 25.2W 2m18s'),('2027-08-02','GE 10:07:50 25.5N 33.2E 6m23s')]:
        e = find(date); t = e['tmax']
        c = central(e, t); m,L1,L2,z = local(e, t, *c)
        # duration on the centre line at GE: step 1 s while inside umbra
        a = t; 
        while local(e,a,*c)[0] < abs(local(e,a,*c)[2]): a -= 1
        b = t
        while local(e,b,*c)[0] < abs(local(e,b,*c)[2]): b += 1
        print(date, 'GE', hm(t), 'at %.2f %.2f' % c, 'dur %dm%02ds' % divmod(int(b-a),60), 'gamma %.4f'%e['gamma'], 'type', 'T' if L2<0 else 'A', '| ref', ref)
    
    # Raleigh / Lake Wheeler 2024-04-08
    e = find('2024-04-08'); best=(0,0)
    for k in range(0, 4*3600, 20):
        t = e['t0'] - 2*3600 + k; m,L1,L2,z = local(e,t,35.69,-78.71)
        o = obscuration(m,L1,L2)
        if o > best[0]: best=(o,t)
    print('Raleigh 2024: max obsc %.1f%% at %s UT (ref ~78-80%%, 19:16 UT)' % (best[0]*100, hm(best[1])))
    for e in E['solar'][:12]:
        t=e['tmax']; c=central(e,t)
        typ = 'P' if c is None else ('T' if local(e,t,*c)[2]<0 else 'A')
        print(e['date'], typ, 'gamma %.4f' % e['gamma'])
    def lunar_umag(e):
        best=-9
        for k in range(-14400,14400,30):
            h=k/3600; x=ev(e['x'],h); y=ev(e['y'],h); ru=ev(e['ru'],h); sm=ev(e['sm'],h)
            best=max(best,(ru+sm-math.hypot(x,y))/(2*sm))
        return best
    for e in E['lunar'][:8]:
        print(e['date'], 'kind', e['kind'], 'umag ours %.3f skyfield %.3f' % (lunar_umag(e), e['umag']))
    
if __name__ == "__main__":
    main()
