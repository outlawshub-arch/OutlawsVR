"""Held guns, detail pass (user 2026-09-30: "twice as much detail", from the pickup sprites' side view and the first-person
sprites' view from behind).  The body is proto's inflated sprite, meshed twice as finely; on it go the parts the sprites
show that a flat inflation cannot: round (or octagonal) barrels with crowned muzzles, sights, a fluted cylinder, screw
heads, hinge pins, the top lever, breech fences, the rib, a saddle ring, buttplates, swivels, twin triggers.

All in proto's model space (three.js axes, cm: X along the sprite, Y up, Z through the sprite's thickness); sprite px
points go through the model's map.  Solid-colour parts take colours sampled off the sprite, so they keep its palette."""
import math

import numpy as np

import proto
from proto import Model, hd_sprite, inflate, action_cut, action_add, PISTOL_BARREL, Col, shade, lit

FINE = 0.5            # mesh spacing against proto's (thick x 0.45): twice as fine
TOL = 0.8             # silhouette simplification (px; proto's 0.9), on an outline smoothed first (SMOOTH)
SMOOTH = 0.75         # px: the alpha softened before the outline is traced, so the pixel steps round off


# ---------------------------------------------------------------- the body and its surface
def zfield(mask, s, thick_cm):
    """the inflated body's half-thickness (cm) at every sprite pixel, as proto.inflate shapes it"""
    from scipy import ndimage
    dt = np.clip(ndimage.distance_transform_edt(mask) - 0.5, 0, None)
    cap = np.full(dt.shape, thick_cm / 2 / s)
    win = 2 * int(math.ceil(min(cap.max(), dt.max()))) + 1
    ridge = ndimage.grey_dilation(dt, size=(win, win))
    rr = np.maximum(np.minimum(ridge, cap), 0.5)
    dd = np.minimum(dt, rr)
    z = rr * np.sqrt(np.clip(1 - (1 - dd / rr) ** 2, 0, 1))
    z[~mask] = 0
    return z * s


def inflate_round(m, width_cm, thick_cm, tol=0.9, spacing_cm=None, mapping=None, insets=(0.45, 1.3, 2.6)):
    """proto.inflate with the edges rounded: besides the silhouette and the interior grid, rings of points inset from
    every outline (px) take the circular profile's height there, so an edge curls over instead of dropping from the
    first grid point straight to the rim (seen from behind, the grip's back edge was a dark sawtooth)"""
    from scipy import ndimage
    from scipy.spatial import Delaunay
    from shapely.prepared import prep
    from shapely.geometry import LineString, Polygon, Point
    from skimage import measure
    alpha = m.a[:, :, 3]
    mask = alpha > 127
    ys, xs = np.nonzero(mask)
    s = width_cm / (xs.max() - xs.min() + 1) if not mapping else mapping[0]
    cx, cy = ((xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2) if not mapping else mapping[1:]
    dt = np.clip(ndimage.distance_transform_edt(mask) - 0.5, 0, None)
    cap = np.full(dt.shape, thick_cm / 2 / s)
    win = 2 * int(math.ceil(min(cap.max(), dt.max()))) + 1
    ridge = ndimage.grey_dilation(dt, size=(win, win))
    rr = np.maximum(np.minimum(ridge, cap), 0.5)
    dd = np.minimum(dt, rr)
    zpx = rr * np.sqrt(np.clip(1 - (1 - dd / rr) ** 2, 0, 1))
    zpx[~mask] = 0
    pad = np.pad(alpha, 1)
    loops = []
    for c in measure.find_contours(pad, 127.5):
        pts = [(p[1] - 1 + 0.5, p[0] - 1 + 0.5) for p in c]
        poly = Polygon(pts).buffer(0)
        if poly.area >= 6:
            loops.append((poly, list(LineString(pts).simplify(tol).coords)[:-1]))
    shape = None
    for pi, li in loops:
        depth = sum(1 for pj, _ in loops if pj is not pi and pj.area > pi.area and pj.contains(Point(li[0])))
        shape = (pi if shape is None else (shape.union(pi) if depth % 2 == 0 else shape.difference(pi)))
    inside = prep(shape)
    sp = max(2.0, (spacing_cm or thick_cm * 0.45) / s)

    def zat(x, y):
        return float(ndimage.map_coordinates(zpx, [[y - 0.5], [x - 0.5]], order=1)[0])
    pts, zs_ = [], []
    for _, ring in loops:
        n = len(ring)
        for i in range(n):
            (x0, y0), (x1, y1) = ring[i], ring[(i + 1) % n]
            k = max(1, int(math.ceil(math.hypot(x1 - x0, y1 - y0) / sp)))
            for j in range(k):
                pts.append((x0 + (x1 - x0) * j / k, y0 + (y1 - y0) * j / k)); zs_.append(0.0)
    # the inset rings: the outline pushed inward (a shapely negative buffer), sampled at the grid's spacing
    for d in insets:
        inner = shape.buffer(-d, join_style=1)
        geoms = getattr(inner, "geoms", [inner])
        for g in geoms:
            if g.is_empty:
                continue
            for lr in [g.exterior] + list(g.interiors):
                L = lr.length
                k = max(3, int(math.ceil(L / (sp * 0.8))))
                for j in range(k):
                    q = lr.interpolate(L * j / k)
                    pts.append((q.x, q.y)); zs_.append(zat(q.x, q.y))
    for gy in np.arange(ys.min() + sp / 2, ys.max() + 1, sp):
        for gx in np.arange(xs.min() + sp / 2, xs.max() + 1, sp):
            ix, iy = int(gx), int(gy)
            if mask[iy, ix] and dt[iy, ix] >= max(0.4 * sp, insets[-1] + 0.5):
                pts.append((gx, gy)); zs_.append(zat(gx, gy))
    pts = np.array(pts); zs_ = np.array(zs_) * s
    tri = Delaunay(pts).simplices
    keep = []
    for a, b, c in tri:
        cxy = (pts[a] + pts[b] + pts[c]) / 3
        if inside.contains(Point(cxy)):
            area = (pts[b][0] - pts[a][0]) * (pts[c][1] - pts[a][1]) - (pts[c][0] - pts[a][0]) * (pts[b][1] - pts[a][1])
            keep.append((a, c, b) if area > 0 else (a, b, c))
    keep = np.array(keep)
    m.map = (s, cx, cy)
    # the texture read a little inside the outline near the edge: the sprite's edge pixels are its black outline,
    # which curled round the rounded edge as a dark seam (seen from behind); within 2.2 px of the edge the UV moves
    # inward (down the distance field's slope) to where the colour is
    dts = ndimage.gaussian_filter(dt, 1.0)
    gy, gx = np.gradient(dts)
    uvs = []
    for x, y in pts:
        d = float(ndimage.map_coordinates(dt, [[y - 0.5], [x - 0.5]], order=1)[0])
        push = max(0.0, getattr(m, "uv_push", 2.2) - d)
        if push > 0:
            ix, iy = min(max(int(x), 0), dt.shape[1] - 1), min(max(int(y), 0), dt.shape[0] - 1)
            nx, ny = gx[iy, ix], gy[iy, ix]
            ln = math.hypot(nx, ny)
            if ln > 1e-6:
                x, y = x + nx / ln * push, y + ny / ln * push
        uvs.append(m.uv(x, y))
    for sign in (1, -1):
        pos = [((x - cx) * s, (cy - y) * s, sign * z) for (x, y), z in zip(pts, zs_)]
        m.part(pos, uvs, keep if sign > 0 else keep[:, ::-1])
    return s


def body(m, width_cm, thick_cm, barrel=None, cut=None):
    """proto.build_gun's body, finer: the action parts and the barrel band cut out, inflated; keeps m.zf, m.full"""
    full = action_cut(m)
    if barrel:
        y0, y1 = barrel["rows"]
        xa, xb = sorted((barrel["x_muzzle"], barrel["x_end"]))
        m.a[y0:y1 + 1, max(0, int(xa) - 2):int(xb) + 1, 3] = 0
    if cut:
        cut(m)
    m.bmask = m.a[:, :, 3] > 127
    from scipy import ndimage
    m.a[:, :, 3] = ndimage.gaussian_filter(m.a[:, :, 3], SMOOTH)
    ys, xs = np.nonzero(full[:, :, 3] > 127)
    s0 = width_cm / (xs.max() - xs.min() + 1)
    mapping = (s0, (xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2)
    inflate_round(m, width_cm=width_cm, thick_cm=thick_cm, tol=TOL, spacing_cm=thick_cm * 0.45 * FINE, mapping=mapping)
    m.zf = zfield(m.a[:, :, 3] > 127, m.map[0], thick_cm)
    m.full = full
    m.thick = thick_cm
    return full


def P(m, x, y):
    """sprite px -> model X, Y (cm)"""
    s, cx, cy = m.map
    return (x - cx) * s, (cy - y) * s


def zs(m, x, y):
    """the body's half-thickness (cm) at a sprite px (its surface there is at +-this)"""
    x, y = int(round(x)), int(round(y))
    y = min(max(y, 0), m.zf.shape[0] - 1)
    x = min(max(x, 0), m.zf.shape[1] - 1)
    return float(m.zf[y, x])


def snap_in(m, x, y, reach=8):
    """the nearest point of the body's outline-inside to (x, y) up or down its column (a lock plate's corner drawn a
    pixel off the receiver floated in the air)"""
    mk = m.bmask if hasattr(m, "bmask") else m.full[:, :, 3] > 127
    xi = int(x)
    for d in range(reach + 1):
        for yy in (y + d, y - d):
            yi = int(yy)
            if 0 <= yi < mk.shape[0] and mk[yi, xi] and m.zf[yi, xi] > 0.3:
                return x, yy
    return x, y


def top(m, x):
    """the body's top edge at column x (its own outline: not a hammer standing on it; else the whole sprite's)"""
    col = m.bmask[:, int(x)] if hasattr(m, "bmask") and m.bmask[:, int(x)].any() else m.full[:, int(x), 3] > 127
    return int(np.nonzero(col)[0].min())


def bottom(m, x):
    col = m.full[:, int(x), 3] > 127
    return int(np.nonzero(col)[0].max())


def colour(m, x, y, r=1, k=1.0):
    img = m.full
    x, y = int(x), int(y)
    p = img[max(0, y - r):y + r + 1, max(0, x - r):x + r + 1]
    p = p[p[:, :, 3] > 127][:, :3]
    c = p.mean(0) if len(p) else np.array((60, 60, 60))
    return Col([int(min(255, max(0, v * k))) for v in c], dict(at=(x, y, r), mode="opaque", k=[k]))


# ---------------------------------------------------------------- primitives (solid colour)
def _frame(axis):
    a = np.asarray(axis, float)
    a /= np.linalg.norm(a)
    h = np.array((0, 1, 0)) if abs(a[1]) < 0.9 else np.array((1, 0, 0))
    u = np.cross(a, h)
    u /= np.linalg.norm(u)
    v = np.cross(a, u)
    return a, u, v


def cyl(m, a, b, r, color, segs=16, caps=True, r2=None, phase=0.0, profile=None):
    """a (truncated) cylinder from a to b; profile(t) scales the radius round the axis (flutes, flats)"""
    a, b = np.asarray(a, float), np.asarray(b, float)
    ax, u, v = _frame(b - a)
    r2 = r if r2 is None else r2
    pos, idx = [], []
    for end, rad in ((a, r), (b, r2)):
        for j in range(segs):
            t = phase + 2 * math.pi * j / segs
            k = profile(t) if profile else 1.0
            pos.append(tuple(end + rad * k * (math.cos(t) * u + math.sin(t) * v)))
    for j in range(segs):
        j2 = (j + 1) % segs
        idx += [j, segs + j, segs + j2, j, segs + j2, j2]
    if caps:
        pos += [tuple(a), tuple(b)]
        ca, cb = 2 * segs, 2 * segs + 1
        for j in range(segs):
            j2 = (j + 1) % segs
            idx += [ca, j2, j, cb, segs + j, segs + j2]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color)


def loft(m, x0, x1, yc, hy, hz, step=2.0, segs=20):
    """a solid of elliptical sections along X from sprite column x0 to x1: centre yc(x) px, half-height hy(x) px,
    half-width hz(x) cm; textured from the sprite column (a fore-end hugging the barrel)"""
    s, cx, cy = m.map
    xs = list(np.arange(x0, x1, step)) + [x1]
    pos, uv, idx = [], [], []
    for x in xs:
        for j in range(segs + 1):
            t = 2 * math.pi * j / segs
            y = yc(x) - hy(x) * math.sin(t)
            pos.append(((x - cx) * s, (cy - y) * s, hz(x) * math.cos(t)))
            uv.append(m.uv(x, yc(x) - hy(x) * math.sin(t) * 0.8))
    for i in range(len(xs) - 1):
        for j in range(segs):
            a, b = i * (segs + 1) + j, (i + 1) * (segs + 1) + j
            idx += [a, b, b + 1, a, b + 1, a + 1]
    m.part(pos, uv, idx)
    for x, k in ((xs[0], 0), (xs[-1], len(xs) - 1)):
        c = len(pos)
        pos.append(((x - cx) * s, (cy - yc(x)) * s, 0.0))
        uv.append(m.uv(x, yc(x)))
        ring = [k * (segs + 1) + j for j in range(segs)]
        tri = []
        for j in range(segs):
            tri += [c, ring[j], ring[(j + 1) % segs]]
        m.part([pos[i] for i in [c] + ring], [uv[i] for i in [c] + ring],
               [0 if v == c else 1 + ring.index(v) for v in tri])


def band(m, X, Yc, hy, hz, width, color, segs=24):
    """an elliptical band round the body at X (a ferrule, a wrist ring): half-height hy, half-thickness hz (cm)"""
    pos, idx = [], []
    for dx in (-width / 2, width / 2):
        for j in range(segs):
            t = 2 * math.pi * j / segs
            pos.append((X + dx, Yc + hy * math.sin(t), hz * math.cos(t)))
    for j in range(segs):
        j2 = (j + 1) % segs
        idx += [j, segs + j, segs + j2, j, segs + j2, j2]
    for e in (0, segs):                     # the band's two rims, a sliver of face each
        for j in range(segs):
            t = 2 * math.pi * j / segs
            pos.append((X + (-width / 2 if e == 0 else width / 2), Yc + hy * 0.9 * math.sin(t), hz * 0.9 * math.cos(t)))
    base = 2 * segs
    for k, e in enumerate((0, segs)):
        o = base + k * segs
        for j in range(segs):
            j2 = (j + 1) % segs
            idx += [e + j, o + j, o + j2, e + j, o + j2, e + j2]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color)


def dome(m, c, n, r, color, h=None, segs=12, rings=3):
    """a low dome (a screw head, a bead, a fence): base circle radius r at c, bulging h along n"""
    c = np.asarray(c, float)
    ax, u, v = _frame(n)
    h = r * 0.45 if h is None else h
    pos, idx = [tuple(c + ax * h)], []
    for i in range(1, rings + 1):
        f = i / rings
        rad, lift = r * math.sin(f * math.pi / 2), h * math.cos(f * math.pi / 2)
        for j in range(segs):
            t = 2 * math.pi * j / segs
            pos.append(tuple(c + ax * lift + rad * (math.cos(t) * u + math.sin(t) * v)))
    for j in range(segs):
        idx += [0, 1 + j, 1 + (j + 1) % segs]
    for i in range(rings - 1):
        b0, b1 = 1 + i * segs, 1 + (i + 1) * segs
        for j in range(segs):
            j2 = (j + 1) % segs
            idx += [b0 + j, b1 + j, b1 + j2, b0 + j, b1 + j2, b0 + j2]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color)


def box(m, c, size, color, yaw=0.0, pitch=0.0):
    """a box centred at c, size (x, y, z) cm, turned pitch degrees about Z (nose up) then yaw about Y"""
    c = np.asarray(c, float)
    sx, sy, sz = (v / 2 for v in size)
    corners = [(x, y, z) for x in (-sx, sx) for y in (-sy, sy) for z in (-sz, sz)]
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cyw, syw = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    pos = []
    for x, y, z in corners:
        x, y = x * cp - y * sp, x * sp + y * cp
        x, z = x * cyw + z * syw, -x * syw + z * cyw
        pos.append(tuple(c + (x, y, z)))
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    idx = []
    for a, b, cc, d in faces:
        idx += [a, b, cc, a, cc, d]
    m.part(pos, [(0, 0)] * 8, idx, color=color)


def sweep(m, path, r, color, segs=8, closed=False):
    """a round wire along a polyline of 3D points"""
    path = [np.asarray(p, float) for p in path]
    n = len(path)
    pos, idx = [], []
    for i in range(n):
        if closed:
            d = path[(i + 1) % n] - path[i - 1]
        else:
            d = path[min(i + 1, n - 1)] - path[max(i - 1, 0)]
        ax, u, v = _frame(d)
        for j in range(segs):
            t = 2 * math.pi * j / segs
            pos.append(tuple(path[i] + r * (math.cos(t) * u + math.sin(t) * v)))
    rings = n if closed else n - 1
    for i in range(rings):
        i2 = (i + 1) % n
        for j in range(segs):
            j2 = (j + 1) % segs
            idx += [i * segs + j, i2 * segs + j, i2 * segs + j2, i * segs + j, i2 * segs + j2, i * segs + j2]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color)


def ring(m, c, n, R, r, color, segs=20, tube=6):
    """a torus: centre c, axis n, radius R, wire r"""
    c = np.asarray(c, float)
    ax, u, v = _frame(n)
    sweep(m, [c + R * (math.cos(t) * u + math.sin(t) * v) for t in np.linspace(0, 2 * math.pi, segs, endpoint=False)],
          r, color, segs=tube, closed=True)


def screws(m, pts, color, r_cm=0.22):
    """screw heads on both faces at sprite px points"""
    for x, y in pts:
        X, Y = P(m, x, y)
        z = zs(m, x, y)
        for sg in (1, -1):
            dome(m, (X, Y, sg * (z - 0.02)), (0, 0, sg), r_cm, color, h=0.07, segs=10, rings=2)
            box(m, (X, Y, sg * (z + 0.045)), (r_cm * 1.5, 0.035, 0.02), shade(color, 0.45))   # its slot


def top_strip(m, x0, x1, width_cm, color, lift=0.06, thick=0.08, widths=None):
    """a flat plate lying along the body's top edge from sprite column x0 to x1 (a tang, a top strap's channel);
    widths(x) -> cm overrides the width along it (the sawed-off's T)"""
    xs = list(np.arange(x0, x1, 1.0)) + [x1]
    pos, idx = [], []
    for x in xs:
        X, Y = P(m, x, top(m, x))
        w = (widths(x) if widths else width_cm) / 2
        pos += [(X, Y + lift, -w), (X, Y + lift, w), (X, Y + lift + thick, w), (X, Y + lift + thick, -w)]
    for i in range(len(xs) - 1):
        a0, b0 = 4 * i, 4 * (i + 1)
        for k in range(4):
            k2 = (k + 1) % 4
            idx += [a0 + k, b0 + k, b0 + k2, a0 + k, b0 + k2, a0 + k2]
    for e in (0, 4 * (len(xs) - 1)):
        idx += [e, e + 1, e + 2, e, e + 2, e + 3]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color)


def top_screw(m, x, color, r_cm=0.2, lift=0.14):
    X, Y = P(m, x, top(m, x))
    dome(m, (X, Y + lift - 0.02, 0), (0, 1, 0), r_cm, color, h=0.06, segs=10, rings=2)
    box(m, (X, Y + lift + 0.035, 0), (0.035, 0.02, r_cm * 1.5), shade(color, 0.45))


def thicken(m, group, k):
    """a moving part made thicker across (Z) about its middle plane (the first-person sprites draw them wide)"""
    for i in getattr(m, "groups", {}).get(group, []):
        m.parts[i]["pos"] = m.parts[i]["pos"] * np.array((1.0, 1.0, k))


# ---------------------------------------------------------------- barrels
def barrels(m, x_muzzle, x_end, specs, segs=20, crown=True):
    """round (or, n_flats, faceted) barrels along X: specs = (centre y px, radius px, lateral z cm, open bore, n_flats)
    each textured from the sprite band it replaces; a crowned muzzle ring and a dark bore"""
    s, cx, cy = m.map
    x0, x1 = sorted((x_muzzle, x_end))
    for by, r_px, zc, bore_open, flats in specs:
        R = r_px * s
        n = flats or segs
        ph = math.pi / n if flats else 0.0
        pos, uv, idx = [], [], []
        xs = np.linspace(x0, x1, 6)
        for x in xs:
            for j in range(n + 1):
                t = ph + 2 * math.pi * j / n
                pos.append(((x - cx) * s, (cy - by) * s + R * math.sin(t), zc + R * math.cos(t)))
                uv.append(m.uv(x, by - r_px * math.sin(t) * 0.9))
        for i in range(len(xs) - 1):
            for j in range(n):
                a, b = i * (n + 1) + j, (i + 1) * (n + 1) + j
                idx += [a, b, b + 1, a, b + 1, a + 1]
        m.part(pos, uv, idx)
        X = (x_muzzle - cx) * s
        C = np.array((X, (cy - by) * s, zc))
        out = -1.0 if x_muzzle < x_end else 1.0                 # the way the muzzle faces along X
        metal = colour(m, x_muzzle + 3 * -out, by, 1, 1.15)
        if crown:
            # the crown: a slightly proud ring at the very muzzle, and the bore's dark
            cyl(m, C - (out * 0.25 * R, 0, 0), C + (out * 0.02, 0, 0), R * 1.04, metal, segs=n, phase=ph)
        rin = R * (0.62 if bore_open else 0.0)
        if bore_open:
            cyl(m, C + (out * 0.03, 0, 0), C - (out * R * 1.6, 0, 0), rin, lit([18, 16, 14], metal), segs=14, caps=True)
        else:
            dome(m, C + (out * 0.02, 0, 0), (out, 0, 0), R * 0.9, metal, h=R * 0.25, segs=n)


# ---------------------------------------------------------------- the revolver
PISTOL_TUBE = [(7.5, 5.0, 0.0, True, 0)]


def pistol_details(m, held):
    s = m.map[0]
    metal = colour(m, 46, 31, 1, 1.0)
    dark = colour(m, 46, 31, 1, 0.55)
    bright = colour(m, 100, 8, 1, 1.1)
    # front sight: a blade on the barrel's top near the muzzle
    X, Y = P(m, 114, 2.5)
    box(m, (X, Y + 0.25, 0), (0.7, 0.5, 0.16), dark, pitch=8)
    # rear sight: the notch cut in the top strap (two ears either side of a groove)
    X, Y = P(m, 37, top(m, 37))
    for sg in (1, -1):
        box(m, (X, Y + 0.08, sg * 0.2), (0.5, 0.16, 0.18), dark)
    # the channel along the top strap to it (the first-person frames show the strap's dark groove), and the hammer's
    # spur broad from behind: half as wide again as the sprite's cut-out
    top_strip(m, 38, 62, 0.3, shade(dark, 0.6), lift=0.0, thick=0.03)
    thicken(m, "hammer", 1.6)
    # the ejector rod's button at the front of its housing, under the barrel
    X, Y = P(m, 94.5, 14.3)
    cyl(m, (X, Y, 0), (X + 0.35, Y, 0), 0.34, bright, segs=12)
    dome(m, (X + 0.35, Y, 0), (1, 0, 0), 0.34, bright, h=0.12, segs=12)
    # frame and grip screws; the grip's escutcheon
    screws(m, [(29, 29), (47, 31), (57, 27)], metal)
    screws(m, [(12, 42)], shade(bright, 1.2), r_cm=0.28)
    # the butt cap: a metal band under the grip's foot
    xa, ya = P(m, 3, 58.5)
    xb, _ = P(m, 21, 58.5)
    zz = zs(m, 12, 56)
    box(m, ((xa + xb) / 2, ya + 0.02, 0), (xb - xa - 0.2, 0.12, 2 * zz * 0.85), dark)
    # a lanyard ring at the butt
    ring(m, ((xa + xb) / 2, ya - 0.45, 0), (0, 0, 1), 0.38, 0.07, bright, segs=14)
    if held:
        # the recoil shield (seen when the cylinder swings out), and the loading gate on the right of it
        X0, Cy = P(m, 31.0, 15.0)
        cyl(m, (X0 - 0.35, Cy, 0), (X0 - 0.02, Cy, 0), 9.0 * s * 0.98, metal, segs=24)
        Xg, Yg = P(m, 30.5, 17)
        box(m, (Xg - 0.1, Yg, zs(m, 29, 17) + 0.06), (0.55, 1.1, 0.12), bright)


def fluted_cylinder(m, rings_px, cyl_y, cyl_r, D_top, D):
    """proto's held cylinder with its six flutes between the chambers, 36 facets round"""
    s, cx, cy = m.map
    Cy = (cy - cyl_y) * s
    R = cyl_r * s
    segs = 36

    def flute(t):                                  # six shallow grooves, centred between the chambers
        d = min(abs(((t - math.radians(90 + 30)) % (math.pi / 3)) - math.pi / 6), math.pi / 6)
        return 1.0 - 0.1 * max(0.0, 1 - d / 0.2) ** 2

    pos, uv, idx = [], [], []
    for xpx, f, fl in rings_px:
        for j in range(segs + 1):
            t = 2 * math.pi * j / segs
            k = f * (flute(t) if fl else 1.0)
            pos.append(((xpx - cx) * s, Cy + k * R * math.sin(t), k * R * math.cos(t)))
            uv.append(m.uv(xpx, cyl_y - k * cyl_r * math.sin(t) * 0.95))
    for i in range(len(rings_px) - 1):
        for j in range(segs):
            a, b = i * (segs + 1) + j, (i + 1) * (segs + 1) + j
            idx += [a, b, b + 1, a, b + 1, a + 1]
    m.part(pos, uv, idx)
    for xpx in (rings_px[0][0], rings_px[-1][0]):
        f = rings_px[0][1]
        pos = [((xpx - cx) * s, Cy, 0.0)]
        uv = [m.uv(D / 2, D_top + D / 2)]
        for j in range(segs):
            t = 2 * math.pi * j / segs
            pos.append(((xpx - cx) * s, Cy + f * R * math.sin(t), f * R * math.cos(t)))
            uv.append(m.uv(D / 2 + (D / 2 - 2) * math.cos(t), D_top + D / 2 - (D / 2 - 2) * math.sin(t)))
        m.part(pos, uv, [v for j in range(segs) for v in (0, 1 + j, 1 + (j + 1) % segs)])


def build_pistol_floor():
    m = Model("GPISTOL", hd_sprite("GPISTOL"))
    b = PISTOL_BARREL
    full = body(m, 30.0, 3.6, barrel=b)
    m.a = full
    barrels(m, b["x_muzzle"], b["x_end"], PISTOL_TUBE)
    action_add(m, full)
    pistol_details(m, held=False)
    return m, "inflate+parts", "Detail pass 2026-09-30: fine body, round crowned barrel, sights, screws, ejector, butt cap"


def build_pistol_held():
    """proto.build_pistol_held, detailed: the frame with the cylinder's window, the cylinder fluted"""
    from PIL import Image
    m = Model("GPISTOL", hd_sprite("GPISTOL"))
    cyl_x0, cyl_x1, cyl_y, cyl_r = 31.5, 60.5, 15.0, 9.0
    b = PISTOL_BARREL

    def window(mm):
        mm.a[6:25, 31:62, 3] = 0
    full = body(m, 30.0, 3.6, barrel=b, cut=window)
    m.a = full
    barrels(m, b["x_muzzle"], b["x_end"], PISTOL_TUBE)
    action_add(m, full)
    s, cx, cy = m.map
    k0 = len(m.parts)
    D = 64
    metal_c = m.sample(46, 15, 2)
    metal = np.array(metal_c, np.float32)
    yy, xx = np.mgrid[0:D, 0:D] + 0.5
    rr = np.hypot(xx - D / 2, yy - D / 2) / (D / 2)
    img = np.zeros((D, D, 3), np.float32) + metal * 0.9
    img[rr > 0.92] = metal * 0.6
    for c in range(6):
        a = math.radians(90 + 60 * c)
        px, py = D / 2 + 0.55 * (D / 2) * math.cos(a), D / 2 - 0.55 * (D / 2) * math.sin(a)
        d = np.hypot(xx - px, yy - py) / (D / 2)
        img[d < 0.24] = (28, 25, 22)
    img[rr < 0.10] = metal * 0.5
    top_y = m.add_block(Image.fromarray(np.dstack([img.clip(0, 255), np.full((D, D), 255)]).astype(np.uint8), "RGBA"), rescale=True)
    m.full = np.asarray(m.img).astype(np.float32)
    # (make_held_recipe.py: the runtime paints this face again from the player's sprite; the holes' literal as a tone
    # of the metal, per channel)
    m.blocks = [dict(kind="cylface", top=top_y, size=D, metal=metal_c.spec, rim=0.6, face=0.9, hub=0.5,
                     hole=lit([28, 25, 22], metal_c).spec["k"][-1])]
    rings_px = [(cyl_x0, 0.86, False), (cyl_x0 + 0.8, 0.97, False), (cyl_x0 + 1.6, 1.0, False), (cyl_x0 + 6.0, 1.0, True),
                (cyl_x1 - 1.6, 1.0, True), (cyl_x1 - 0.8, 0.97, False), (cyl_x1, 0.86, False)]
    fluted_cylinder(m, rings_px, cyl_y, cyl_r, top_y, D)
    # the cylinder's axis pin, proud of the front face
    Cy = (cy - cyl_y) * s
    Xf = (cyl_x1 - cx) * s
    cyl(m, (Xf, Cy, 0), (Xf + 0.25, Cy, 0), 0.16 * cyl_r * s, shade(metal_c, 0.6), segs=10)
    m.groups["cyl"] = list(range(k0, len(m.parts)))
    m.cyl = dict(centre=(((cyl_x0 + cyl_x1) / 2 - cx) * s, Cy, 0.0), radius=cyl_r * s, length=(cyl_x1 - cyl_x0) * s,
                 chamber_r=0.55 * 0.86 * cyl_r * s, hole_r=0.24 * 0.86 * cyl_r * s)
    pistol_details(m, held=True)
    return m


# ---------------------------------------------------------------- the rifle (the Henry: brass frame, octagon barrel)
def build_rifle():
    m = Model("GRIFLE", hd_sprite("GRIFLE"))
    bar = dict(x_muzzle=1, x_end=101, rows=(3, 14))
    full = body(m, 110.0, 4.4, barrel=bar)
    m.a = full
    s = m.map[0]
    barrels(m, 1, 101, [(5.6, 2.8, 0.0, True, 8), (11.6, 2.7, 0.0, False, 0)])
    action_add(m, full)
    metal = colour(m, 40, 5, 1, 1.0)
    dark = colour(m, 40, 5, 1, 0.5)
    brass = colour(m, 165, 10, 2, 0.95)
    brass_d = colour(m, 165, 10, 2, 0.7)
    silver = colour(m, 101, 10, 1, 1.0)
    # the barrel band / magazine collar where barrel and tube meet the wood
    for by, r in ((5.6, 2.8), (11.6, 2.7)):
        X0, Y = P(m, 97.5, by)
        X1, _ = P(m, 101.5, by)
        cyl(m, (X0, Y, 0), (X1, Y, 0), r * s * 1.12, silver, segs=20)
    Xa, Ya = P(m, 99.5, 8.6)
    box(m, (Xa, Ya, 0), (abs(X1 - X0), 1.1, 2 * 2.7 * s * 1.05), silver)
    # the magazine tube's slot and follower stud
    X, Y = P(m, 70, 11.6)
    for sg in (1, -1):
        box(m, (X, Y, sg * 2.7 * s * 0.97), (26 * s, 0.14, 0.05), dark)
    # front sight (a brass blade in a dovetail) and the rear sight's leaf and base
    X, Y = P(m, 4, 5.6 - 2.8)
    box(m, (X, Y + 0.05, 0), (1.0, 0.18, 0.5), dark)
    box(m, (X, Y + 0.35, 0), (0.6, 0.5, 0.12), brass, pitch=-10)
    X, Y = P(m, 92, 5.6 - 2.8)
    box(m, (X, Y + 0.08, 0), (2.0, 0.22, 0.9), dark)
    box(m, (X + 0.35, Y + 0.45, 0), (0.12, 0.7, 0.75), dark, pitch=12)
    for sg in (1, -1):
        box(m, (X + 0.35, Y + 0.85, sg * 0.22), (0.12, 0.18, 0.28), dark, pitch=12)
    # the receiver: screws, the loading gate (right), the saddle ring (left), the top tang and its screw
    screws(m, [(163, 15), (184, 19), (196, 11)], brass_d, r_cm=0.25)
    Xg, Yg = P(m, 175, 12.5)
    box(m, (Xg, Yg, -(zs(m, 175, 12.5) + 0.05)), (14 * s, 4 * s, 0.1), brass_d)
    box(m, (Xg - 1.0, Yg, -(zs(m, 175, 12.5) + 0.1)), (0.5, 0.18, 0.04), dark)      # the gate's spring slot
    Xr, Yr = P(m, 193, 15)
    zr = zs(m, 193, 15) + 0.25
    cyl(m, (Xr, Yr, zr - 0.3), (Xr, Yr, zr), 0.18, metal, segs=8)
    ring(m, (Xr, Yr - 1.0, zr + 0.05), (0, 0, 1), 1.0, 0.1, metal, segs=18)
    Xt, Yt = P(m, 205, top(m, 205))
    box(m, (Xt, Yt - 0.02, 0), (4.5, 0.12, 1.1), brass, pitch=-14)
    dome(m, (Xt + 0.5, Yt + 0.08, 0), (0, 1, 0), 0.22, brass_d, h=0.06, segs=10, rings=2)
    # the buttplate: a brass crescent over the butt's end
    butt_plate(m, 262, brass, 0.28)
    # the fore-end's front cap is the band above; the wood meets the receiver under a thin brass lip
    Xl, Yl = P(m, 156, 13)
    box(m, (Xl, Yl, 0), (0.25, (21 - 5) * s, 2 * zs(m, 150, 13) + 0.12), brass)
    return m, "inflate+parts", "Detail pass 2026-09-30: octagon barrel, collar, sights, gate, saddle ring, screws, tang, brass buttplate"


def butt_plate(m, x_from, color, t_cm, wrap=0.35):
    """a plate over the butt's end: along the stock's last column of each row, both faces a little wrapped"""
    a = m.full[:, :, 3] > 127
    rows = [y for y in range(a.shape[0]) if a[y, x_from:].any()]
    pts = []
    for y in [rows[0] - 0.5] + rows + [rows[-1] + 0.5]:
        yi = min(max(int(round(y)), rows[0]), rows[-1])
        xr = x_from + int(np.nonzero(a[yi, x_from:])[0].max())
        pts.append((xr, y))
    # (outward of the smoothed, simplified outline by a pixel and the plate's thickness, so no rim pokes through)
    pos, idx = [], []
    for i, (x, y) in enumerate(pts):
        X, Y = P(m, x + 1.2, y)
        z = max(zs(m, x - 3, y), 0.4) + 0.06
        pos += [(X + t_cm, Y, -z), (X + t_cm, Y, z), (X - wrap - 0.4, Y, z), (X - wrap - 0.4, Y, -z)]
    for i in range(len(pts) - 1):
        a0, b0 = 4 * i, 4 * (i + 1)
        for k in range(4):
            k2 = (k + 1) % 4
            idx += [a0 + k, b0 + k, b0 + k2, a0 + k, b0 + k2, a0 + k2]
    for e in (0, 4 * (len(pts) - 1)):
        idx += [e, e + 1, e + 2, e, e + 2, e + 3]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color)


# ---------------------------------------------------------------- the shotgun (a single barrel, a break action)
def build_sgun():
    m = Model("GSGUN", hd_sprite("GSGUN"))
    bar = dict(x_muzzle=3, x_end=141, rows=(6, 15))
    FE0, FE1 = 86, 140                               # the fore-end, under the barrel

    def no_foreend(mm):
        mm.a[14:, FE0:FE1 + 1, 3] = 0
    full = body(m, 105.0, 4.4, barrel=bar, cut=no_foreend)
    m.a = full
    s = m.map[0]
    barrels(m, 3, 141, [(10.6, 4.4, 0.0, True, 0)])
    # the fore-end: wood round the barrel's underside, as deep as the sprite draws it, a little wider than the barrel
    def fe_bottom(x):
        return float(bottom(m, int(min(max(x, FE0), FE1))))
    loft(m, FE0 - 2, FE1, yc=lambda x: (10.6 + max(fe_bottom(x), 13.0)) / 2 + 0.5,
         hy=lambda x: max(2.0, (max(fe_bottom(x), 13.0) - 10.6) / 2 + 0.5),
         hz=lambda x: 4.4 * s * min(1.08, 0.55 + 0.53 * (x - FE0 + 2) / 14.0))
    action_add(m, full)
    metal = colour(m, 150, 20, 1, 1.0)
    dark = colour(m, 150, 20, 1, 0.45)
    light = colour(m, 150, 20, 1, 1.25)
    # the front bead and a plain rib along the barrel's top
    X, Y = P(m, 7, 10.6 - 4.4)
    dome(m, (X, Y - 0.05, 0), (0, 1, 0), 0.2, lit([220, 210, 170], light), h=0.2, segs=10)
    Xa, Ya = P(m, 10, 10.6 - 4.4)
    Xb, _ = P(m, 138, 10.6 - 4.4)
    box(m, ((Xa + Xb) / 2, Ya - 0.02, 0), (abs(Xb - Xa), 0.14, 0.55), light)
    # the hinge pin knuckle, the fore-end's latch, the action's screws
    Xh, Yh = P(m, 142, 21)
    zh = zs(m, 146, 21)
    cyl(m, (Xh, Yh, -zh - 0.08), (Xh, Yh, zh + 0.08), 0.55, light, segs=16)
    Xf, Yf = P(m, 126, bottom(m, 126))
    box(m, (Xf, Yf + 0.05, 0), (2.6, 0.2, 1.2), dark)
    screws(m, [(152, 26), (164, 22), (171, 18)], metal)
    # the top lever: a flat thumb piece on the tang behind the hammer, pointing back
    top_strip(m, 166, 206, 1.1, metal, lift=0.02, thick=0.12)
    for x in (182, 200):
        top_screw(m, x, light, lift=0.14)
    Xt, Yt = P(m, 168, top(m, 168))
    box(m, (Xt, Yt + 0.22, 0), (2.6, 0.2, 1.3), light, pitch=-6)
    # the wrist's two ferrule rings (the dark band on the side view), a swivel under the butt, the buttplate
    for x in (179, 198):
        mid = (top(m, x) + bottom(m, x)) / 2
        Xw, Yw = P(m, x, mid)
        hh = (bottom(m, x) - top(m, x) + 1) * s / 2
        band(m, Xw, Yw, hh + 0.08, zs(m, x + 4, mid) + 0.1, 0.55, dark)
    Xs, Ys = P(m, 252, bottom(m, 252))
    cyl(m, (Xs, Ys + 0.2, 0), (Xs, Ys - 0.25, 0), 0.14, metal, segs=8)
    ring(m, (Xs, Ys - 0.7, 0), (0, 0, 1), 0.5, 0.08, metal, segs=16)
    butt_plate(m, 272, dark, 0.3)
    return m, "inflate+parts", "Detail pass 2026-09-30: round crowned barrel, bead, rib, hinge pin, top lever, latch, screws, rings, buttplate"


# ---------------------------------------------------------------- the double (side by side, twin hammers and triggers)
def build_dbsgun():
    m = Model("GDBSGUN", hd_sprite("GDBSGUN"))
    bar = dict(x_muzzle=5, x_end=148, rows=(6, 13))
    full = body(m, 112.0, 4.6, barrel=bar)
    m.a = full
    s = m.map[0]
    R = 4.0 * s
    twin = 0.97 * 4.0 * s
    barrels(m, 5, 148, [(9.5, 4.0, -twin, True, 0), (9.5, 4.0, twin, True, 0)])
    # proto's twin hammers sit at +-0.85 of the barrels' spacing
    action_add(m, full, twin_z=twin)
    metal = colour(m, 165, 21, 1, 1.0)
    dark = colour(m, 165, 21, 1, 0.45)
    light = colour(m, 165, 21, 1, 1.2)
    # the second trigger, behind the first (both move with the trigger group)
    if "trigger" in m.groups:
        for i in list(m.groups["trigger"]):
            q = dict(m.parts[i])
            q["pos"] = m.parts[i]["pos"] + np.array((4.5 * s, -0.2, 0.0))
            m.parts.append(q)
            m.groups["trigger"].append(len(m.parts) - 1)
    # the top rib in the valley between the barrels, the bead on it, and the rib below ahead of the fore-end
    Xa, Y = P(m, 9, 9.5)
    Xb, _ = P(m, 147, 9.5)
    box(m, ((Xa + Xb) / 2, Y + R * 0.72, 0), (abs(Xb - Xa), 0.16, 0.7), light)
    dome(m, (Xa - 0.3, Y + R * 0.8, 0), (0, 1, 0), 0.2, lit([220, 210, 170], light), h=0.2, segs=10)
    Xc, _ = P(m, 62, 9.5)
    box(m, ((Xa + Xc) / 2, Y - R * 0.72, 0), (abs(Xc - Xa), 0.16, 0.6), metal)
    # the breech fences: the rounded shoulders of the action behind each barrel (the first-person sprites show them)
    Xf, Yf = P(m, 150, 9.5)
    for sg in (1, -1):
        dome(m, (Xf, Yf, sg * twin), (1, 0, 0), R * 1.12, metal, h=R * 0.8, segs=16, rings=4)
    # the top lever over the tang, the hinge pin, the fore-end latch, the action's screws
    Xt, Yt = P(m, 180, top(m, 180))
    box(m, (Xt, Yt + 0.15, 0), (3.4, 0.24, 1.0), metal, pitch=-8)
    box(m, (Xt + 1.4, Yt + 0.34, 0), (0.8, 0.22, 1.4), metal, pitch=-8)
    Xh, Yh = P(m, 150, 21)
    zh = zs(m, 156, 21)
    cyl(m, (Xh, Yh, -zh - 0.08), (Xh, Yh, zh + 0.08), 0.6, light, segs=16)
    Xl, Yl = P(m, 104, bottom(m, 104))
    box(m, (Xl, Yl + 0.05, 0), (2.8, 0.2, 1.3), dark)
    screws(m, [(160, 22), (172, 17), (188, 21)], metal)
    for x in (188, 197):
        top_screw(m, x, light, lift=0.12)
    thicken(m, "hammer", 1.3)
    # the side plates' engraved border: a raised line round each lock plate
    for sg in (1, -1):
        pts = [(157, 14), (190, 14), (195, 20), (190, 24), (157, 24)]
        path = []
        for x, y in pts + pts[:1]:
            x, y = snap_in(m, x, y)
            X, Y = P(m, x, y)
            path.append((X, Y, sg * (zs(m, x, y) + 0.02)))
        sweep(m, path, 0.05, light, segs=4)
    # a swivel under the butt and the buttplate
    Xs, Ys = P(m, 300, bottom(m, 300))
    cyl(m, (Xs, Ys + 0.2, 0), (Xs, Ys - 0.25, 0), 0.14, metal, segs=8)
    ring(m, (Xs, Ys - 0.7, 0), (0, 0, 1), 0.5, 0.08, metal, segs=16)
    butt_plate(m, 318, dark, 0.3)
    return m, "inflate+parts", "Detail pass 2026-09-30: fine barrels, rib + bead, fences, twin triggers, top lever, hinge, lock plates, buttplate"


# ---------------------------------------------------------------- the sawed-off (a cut-down double, a pistol grip)
def build_sawgun():
    """from the side sprite and the first-person frames (ijsawgun): twin short barrels with a rib and bead, the
    breech fences, lock plates, twin hammers and triggers, the copper T-tang with its two screws down the wrist's
    top, a capped pistol grip"""
    m = Model("GSAWGUN", hd_sprite("GSAWGUN"))
    m.uv_push = 4.0                                # (past the grip's light rim line: dark wood from behind, as ijsawgun)
    bar = dict(x_muzzle=2, x_end=134, rows=(3, 17))
    full = body(m, 58.0, 4.2, barrel=bar)
    m.a = full
    s = m.map[0]
    R = 5.5 * s
    twin = 0.97 * R
    barrels(m, 2, 134, [(10.0, 5.5, -twin, True, 0), (10.0, 5.5, twin, True, 0)])
    action_add(m, full, twin_z=twin)
    metal = colour(m, 125, 24, 1, 1.0)
    dark = colour(m, 125, 24, 1, 0.45)
    light = colour(m, 125, 24, 1, 1.2)
    copper = lit([178, 112, 74], light)                      # the tang plate's copper (ijsawgun)
    if "trigger" in m.groups:
        for i in list(m.groups["trigger"]):
            q = dict(m.parts[i])
            q["pos"] = m.parts[i]["pos"] + np.array((4.0 * s, -0.15, 0.0))
            m.parts.append(q)
            m.groups["trigger"].append(len(m.parts) - 1)
    thicken(m, "hammer", 1.3)
    # the rib in the barrels' top valley and its bead; the lower rib ahead of the fore-end
    Xa, Y = P(m, 5, 10.0)
    Xb, _ = P(m, 133, 10.0)
    box(m, ((Xa + Xb) / 2, Y + R * 0.72, 0), (abs(Xb - Xa), 0.14, 0.55), light)
    dome(m, (Xa - 0.2, Y + R * 0.8, 0), (0, 1, 0), 0.17, lit([220, 210, 170], light), h=0.17, segs=10)
    Xc, _ = P(m, 76, 10.0)
    box(m, ((Xa + Xc) / 2, Y - R * 0.72, 0), (abs(Xc - Xa), 0.14, 0.5), metal)
    # the breech fences behind each barrel (round, toward the shooter) and the dark stud between them
    Xf, Yf = P(m, 135, 10.0)
    for sg in (1, -1):
        dome(m, (Xf, Yf, sg * twin), (1, 0, 0), R * 1.1, metal, h=R * 0.75, segs=16, rings=4)
    dome(m, (Xf + 0.1, Yf + R * 0.75, 0), (1, 0, 0), 0.22, dark, h=0.18, segs=10)
    # the copper T-tang down the wrist's top: a broad crossbar at the action, a narrow stem back, two slotted screws
    top_strip(m, 146, 170, 1.0, copper, lift=0.05, thick=0.1, widths=lambda x: 1.9 if x < 150 else 1.0)
    for x in (148, 164):
        top_screw(m, x, shade(copper, 0.8), r_cm=0.24, lift=0.15)
    # the hinge pin, the fore-end latch, the action's screws, the lock plates' borders
    Xh, Yh = P(m, 118, 23)
    zh = zs(m, 124, 23)
    cyl(m, (Xh, Yh, -zh - 0.08), (Xh, Yh, zh + 0.08), 0.5, light, segs=16)
    Xl, Yl = P(m, 96, bottom(m, 96))
    box(m, (Xl, Yl + 0.05, 0), (2.0, 0.18, 1.2), dark)
    screws(m, [(128, 24), (140, 20), (160, 26)], metal)
    for sg in (1, -1):
        pts = [(126, 17), (160, 17), (166, 23), (160, 28), (126, 28)]
        path = []
        for x, y in pts + pts[:1]:
            x, y = snap_in(m, x, y)
            X, Yp = P(m, x, y)
            path.append((X, Yp, sg * (zs(m, x, y) + 0.02)))
        sweep(m, path, 0.045, light, segs=4)
    return m, "inflate+parts", "Detail pass 2026-09-30: twin crowned barrels, rib + bead, fences, T-tang, twin triggers, lock plates"


BUILDERS = [build_pistol_floor, build_rifle, build_sgun, build_dbsgun, build_sawgun]


def held_model(m):
    return build_pistol_held() if m.name == "GPISTOL" else m
