"""Prototype: low-poly 3D items built from Outlaws item sprites.

Three builders, all driven by the sprite's own pixels (UVs point back into the sprite, so the
colour scheme is the sprite's by construction):
  extrude  - alpha contour -> simplified polygon, front/back faces + side walls (flat items)
  lathe    - per-row silhouette width -> radius profile -> surface of revolution (bottles, jars)
  recipe   - hand-fitted primitives (box, drum) whose faces are projected from sprite quads/ellipses
Writes viewer.html (three.js) and one OBJ+MTL+PNG per item.
"""
import base64, io, json, math, os
import numpy as np
from PIL import Image
from skimage import measure
from shapely.geometry import LineString, Polygon, Point
import mapbox_earcut as earcut

HERE = os.path.dirname(os.path.abspath(__file__))
HD = os.path.join(HERE, "sprites_hd")      # G*.png from olue_export.py <level> --hd (sprites_hd/)
OUT = os.path.join(HERE, "out")
# the local item folder (private, never in the repo): OUTLAWSVR_ITEMDIR, else tools/olue/placeholders beside this folder
ITEM_DIR = os.environ.get("OUTLAWSVR_ITEMDIR") or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "placeholders")
UE_OUT = os.path.join(ITEM_DIR, "items3d")


class Col(list):
    """a solid colour [r, g, b] that remembers where on the sprite it was read (make_held_recipe.py: the runtime
    builder reads it again off the player's own sprite).  spec: dict(at=(x, y, r), mode="opaque"|"all", k=[...]):
    the mean of the window's pixels (opaque ones, or all), then each k in turn, truncated to int (and clamped)."""
    def __init__(self, rgb, spec=None):
        super().__init__(rgb)
        self.spec = spec


def shade(c, k):
    """c's channels times k, truncated (as [int(v * k) for v in c]), keeping its sample spec"""
    out = [int(v * k) for v in c]
    sp = getattr(c, "spec", None)
    return Col(out, dict(sp, k=list(sp["k"]) + [k]) if sp else None)


def lit(rgb, like):
    """a literal colour standing for a tone of the sprite colour `like`: the runtime reads `like`'s point and scales
    each channel by the literal's over `like`'s (+0.5 so it truncates back), so the stock sprite gives the literal"""
    sp = getattr(like, "spec", None)
    if not sp:
        return Col(rgb)
    k = [(c + 0.5) / max(1, l) for c, l in zip(rgb, like)]
    return Col(rgb, dict(sp, k=list(sp["k"]) + [k], lit=list(rgb)))


class Model:
    def __init__(self, name, tex_path, cell=None):
        self.name = name
        im = Image.open(tex_path).convert("RGBA")
        if cell:
            im = im.crop(cell)
        self.src = tex_path
        self.cell = cell
        self.img = im
        self.a = np.asarray(im).astype(np.float32)
        self.W, self.H = im.size
        self.SH = self.H          # the sprite's own height (the texture may grow below it)
        self.parts = []   # dict(pos, uv, idx, tex|color, alpha)

    def add_block(self, block, rescale=False):
        """append a synthesized image under the sprite; returns its top y.  Parts already built keep their
        pixels when rescale is set (their V is squeezed into the taller texture)"""
        assert rescale or not self.parts
        top = self.H
        w = max(self.W, block.width)
        im = Image.new("RGBA", (w, self.H + block.height), (0, 0, 0, 0))
        im.paste(self.img, (0, 0)); im.paste(block, (0, top))
        self.img = im
        self.a = np.asarray(im).astype(np.float32)
        H0 = self.H
        self.W, self.H = im.size
        for part in self.parts:
            part["uv"][:, 1] = 1.0 - (1.0 - part["uv"][:, 1]) * H0 / self.H
        return top

    def uv(self, x, y):
        return (x / self.W, 1.0 - y / self.H)

    def sample(self, x, y, r=1):
        x, y = int(x), int(y)
        p = self.a[max(0, y - r):y + r + 1, max(0, x - r):x + r + 1, :3].reshape(-1, 3)
        return Col([int(c) for c in p.mean(0)], dict(at=(x, y, r), mode="all", k=[]))

    def part(self, pos, uv, idx, color=None, alpha=False, xf=None):
        pos = np.asarray(pos, np.float64).reshape(-1, 3)
        if xf is not None:
            pos = pos @ xf[:3, :3].T + xf[:3, 3]
        self.parts.append(dict(pos=pos, uv=np.asarray(uv, np.float64).reshape(-1, 2),
                               idx=np.asarray(idx, np.int64).reshape(-1), color=color, alpha=alpha))

    def quad(self, P, UVpx, xf=None):
        """four 3D corners, four sprite pixel corners (same order)"""
        self.part(P, [self.uv(*q) for q in UVpx], [0, 1, 2, 0, 2, 3], xf=xf)

    def tris(self):
        return sum(len(p["idx"]) // 3 for p in self.parts)


def rot(axis, deg, t=(0, 0, 0)):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    x, y, z = axis
    R = np.array([[c + x*x*(1-c), x*y*(1-c) - z*s, x*z*(1-c) + y*s],
                  [y*x*(1-c) + z*s, c + y*y*(1-c), y*z*(1-c) - x*s],
                  [z*x*(1-c) - y*s, z*y*(1-c) + x*s, c + z*z*(1-c)]])
    M = np.eye(4); M[:3, :3] = R; M[:3, 3] = t
    return M


# ---------------------------------------------------------------- extrude
def extrude(m, width_cm, thick_cm, tol=0.9, mapping=None):
    alpha = m.a[:, :, 3]
    ys, xs = np.nonzero(alpha > 127)
    s = width_cm / (xs.max() - xs.min() + 1) if not mapping else mapping[0]
    cx, cy = ((xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2) if not mapping else mapping[1:]
    pad = np.pad(alpha, 1)
    loops = []
    for c in measure.find_contours(pad, 127.5):
        pts = [(p[1] - 1 + 0.5, p[0] - 1 + 0.5) for p in c]   # pixel-centre space
        poly = Polygon(pts)
        if not poly.is_valid:
            poly = poly.buffer(0)
        if poly.area < 6:
            continue
        simp = LineString(pts).simplify(tol)
        loops.append((poly, list(simp.coords)[:-1]))
    # nesting depth: odd = hole
    rings = []
    for i, (pi, li) in enumerate(loops):
        depth = sum(1 for j, (pj, _) in enumerate(loops) if j != i and pj.area > pi.area and pj.contains(Point(li[0])))
        rings.append((depth, pi, li))
    outers = [r for r in rings if r[0] % 2 == 0]
    holes = [r for r in rings if r[0] % 2 == 1]
    z = thick_cm / 2
    P = lambda x, y, zz: ((x - cx) * s, (cy - y) * s, zz)
    for d, po, lo in outers:
        hs = [h for h in holes if h[0] == d + 1 and po.contains(Point(h[2][0]))]
        ring_pts = [lo] + [h[2] for h in hs]
        flat = np.array([p for r in ring_pts for p in r], np.float32)
        ends = np.cumsum([len(r) for r in ring_pts]).astype(np.uint32)
        tri = earcut.triangulate_float32(flat, ends).reshape(-1, 3)
        for zz, flip in ((z, False), (-z, True)):
            pos = [P(x, y, zz) for x, y in flat]
            uv = [m.uv(x, y) for x, y in flat]
            idx = tri[:, ::-1] if flip else tri
            m.part(pos, uv, idx, alpha=True)
        # side walls: UV nudged 1.5 px inside so the wall takes the edge colour, not the fringe
        for r in ring_pts:
            n = len(r)
            area = sum(r[i][0] * r[(i+1) % n][1] - r[(i+1) % n][0] * r[i][1] for i in range(n)) / 2
            sgn = 1 if area > 0 else -1
            pos, uv, idx = [], [], []
            for i in range(n):
                (x0, y0), (x1, y1) = r[i], r[(i+1) % n]
                ex, ey = x1 - x0, y1 - y0
                L = math.hypot(ex, ey) or 1
                nx, ny = -ey / L * sgn, ex / L * sgn    # inward in image space
                k = len(pos)
                pos += [P(x0, y0, z), P(x1, y1, z), P(x1, y1, -z), P(x0, y0, -z)]
                u0 = m.uv(x0 + nx * 1.5, y0 + ny * 1.5); u1 = m.uv(x1 + nx * 1.5, y1 + ny * 1.5)
                uv += [u0, u1, u1, u0]
                idx += [k, k+1, k+2, k, k+2, k+3]
            m.part(pos, uv, idx)
    return s


# ---------------------------------------------------------------- lathe
def lathe(m, height_cm, segs=14, tol=1.2, y_from=None):
    alpha = m.a[:, :, 3]
    rows = []
    for y in range(m.H):
        xs = np.nonzero(alpha[y] > 127)[0]
        if len(xs):
            rows.append((y, xs.min(), xs.max() + 1))
    cx = float(np.median([(l + r) / 2 for _, l, r in rows]))
    ytop, ybot = rows[0][0], rows[-1][0] + 1
    s = height_cm / (ybot - ytop)
    prof = [(y + 0.5, (r - l) / 2) for y, l, r in rows]
    prof = [(ytop, prof[0][1])] + prof + [(ybot, prof[-1][1])]
    prof = list(LineString(prof).simplify(tol).coords)
    pos, uv, idx = [], [], []
    for yi, rad in prof:
        for k in range(segs + 1):
            t = 2 * math.pi * k / segs
            pos.append((rad * math.sin(t) * s, (ybot - yi) * s, rad * math.cos(t) * s))
            uv.append(m.uv(cx + rad * math.sin(t) * 0.97, yi))
    for i in range(len(prof) - 1):
        for k in range(segs):
            a, b = i * (segs + 1) + k, (i + 1) * (segs + 1) + k
            idx += [a, b, b + 1, a, b + 1, a + 1]
    m.part(pos, uv, idx)
    for yi, rad in (prof[0], prof[-1]):      # caps
        c = len(pos)
        cap_pos = [(0, (ybot - yi) * s, 0)] + [(rad * math.sin(2*math.pi*k/segs) * s, (ybot - yi) * s,
                                               rad * math.cos(2*math.pi*k/segs) * s) for k in range(segs)]
        cap_uv = [m.uv(cx, yi + (2 if yi == prof[0][0] else -2))] * (segs + 1)
        m.part(cap_pos, cap_uv, [v for k in range(segs) for v in (0, 1 + k, 1 + (k+1) % segs)])
    return s


# ---------------------------------------------------------------- drum: cylinder whose cap is an ellipse in the sprite
def drum(m, cx, cy, rx, ry, band_px, radius, height, dome=0.0, segs=16, xf=None, side_color=None, bottom_color=None, band_inset=1.0, cap=None):
    # cap (t=0 faces the viewer = lower edge of the sprite ellipse); cap=(ccx, ccy, crx, cry) maps it elsewhere
    ccx, ccy, crx, cry = cap or (cx, cy, rx, ry)
    pos, uv, idx = [(0, height + dome, 0)], [m.uv(ccx, ccy)], []
    for f, lift in ((0.55, dome * 0.75), (1.0, 0.0)):
        for k in range(segs):
            t = 2 * math.pi * k / segs
            pos.append((radius * f * math.sin(t), height + lift, radius * f * math.cos(t)))
            uv.append(m.uv(ccx + crx * f * math.sin(t), ccy + cry * f * math.cos(t)))
    for k in range(segs):
        idx += [0, 1 + k, 1 + (k+1) % segs]
        a0, a1, b0, b1 = 1 + k, 1 + (k+1) % segs, 1 + segs + k, 1 + segs + (k+1) % segs
        idx += [a0, b0, b1, a0, b1, a1]
    m.part(pos, uv, idx, xf=xf)
    # side band: front half read straight off the sprite, back half mirrored
    pos, uv, idx = [], [], []
    for k in range(segs + 1):
        t = 2 * math.pi * k / segs
        x, z = radius * math.sin(t), radius * math.cos(t)
        ux, uy = cx + rx * band_inset * math.sin(t), cy + ry * abs(math.cos(t))
        pos += [(x, height, z), (x, 0, z)]
        uv += [m.uv(ux, uy + 1), m.uv(ux, uy + band_px - 1)]
    for k in range(segs):
        a = 2 * k
        idx += [a, a + 1, a + 3, a, a + 3, a + 2]
    if side_color:
        m.part(pos, uv, idx, color=side_color, xf=xf)
    else:
        m.part(pos, uv, idx, xf=xf)
    bc = bottom_color or m.sample(cx, cy + ry + band_px - 2)
    pos = [(0, 0, 0)] + [(radius * math.sin(2*math.pi*k/segs), 0, radius * math.cos(2*math.pi*k/segs)) for k in range(segs)]
    m.part(pos, [(0, 0)] * (segs + 1), [v for k in range(segs) for v in (0, 1 + (k+1) % segs, 1 + k)], color=bc, xf=xf)


def box_solid(m, x0, x1, y0, y1, z0, z1, color, xf=None):
    c = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),(x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    faces = [(4,5,6,7),(1,0,3,2),(5,1,2,6),(0,4,7,3),(7,6,2,3),(0,1,5,4)]
    pos, idx = [], []
    for f in faces:
        k = len(pos); pos += [c[i] for i in f]; idx += [k, k+1, k+2, k, k+2, k+3]
    m.part(pos, [(0, 0)] * len(pos), idx, color=color, xf=xf)


# ---------------------------------------------------------------- inflate: extrusion with a rounded cross-section
def inflate(m, width_cm, thick_cm, tol=0.9, spacing_cm=None, cap_at=None, warp=None, mapping=None):
    """Front/back surfaces meet at the silhouette; height from the edge follows a circle whose radius is the
    local half-width (thin parts come out round, e.g. barrels) capped at the half-thickness (wide parts get
    rounded edges and flat faces).  cap_at(x_px, y_px) -> half-thickness cm overrides per region;
    warp(X, Y, Z, x_px, y_px) -> (X, Y, Z) bends the result (cm, three.js axes)."""
    from scipy import ndimage
    from scipy.spatial import Delaunay
    from shapely.prepared import prep
    alpha = m.a[:, :, 3]
    mask = alpha > 127
    ys, xs = np.nonzero(mask)
    s = width_cm / (xs.max() - xs.min() + 1) if not mapping else mapping[0]
    cx, cy = ((xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2) if not mapping else mapping[1:]
    dt = np.clip(ndimage.distance_transform_edt(mask) - 0.5, 0, None)
    if cap_at:
        gy, gx = np.mgrid[0:m.H, 0:m.W]
        cap = np.vectorize(cap_at)(gx + 0.5, gy + 0.5) / s
    else:
        cap = np.full(dt.shape, thick_cm / 2 / s)
    win = 2 * int(math.ceil(min(cap.max(), dt.max()))) + 1
    ridge = ndimage.grey_dilation(dt, size=(win, win))
    rr = np.maximum(np.minimum(ridge, cap), 0.5)
    dd = np.minimum(dt, rr)
    zpx = rr * np.sqrt(np.clip(1 - (1 - dd / rr) ** 2, 0, 1))
    zpx[~mask] = 0
    # the silhouette, holes included
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
    sp = max(2.5, (spacing_cm or thick_cm * 0.45) / s)
    pts, zs = [], []
    for _, ring in loops:
        n = len(ring)
        for i in range(n):
            (x0, y0), (x1, y1) = ring[i], ring[(i + 1) % n]
            k = max(1, int(math.ceil(math.hypot(x1 - x0, y1 - y0) / sp)))
            for j in range(k):
                pts.append((x0 + (x1 - x0) * j / k, y0 + (y1 - y0) * j / k)); zs.append(0.0)
    for gy in np.arange(ys.min() + sp / 2, ys.max() + 1, sp):
        for gx in np.arange(xs.min() + sp / 2, xs.max() + 1, sp):
            ix, iy = int(gx), int(gy)
            if mask[iy, ix] and dt[iy, ix] >= 0.4 * sp:
                pts.append((gx, gy))
                zs.append(float(ndimage.map_coordinates(zpx, [[gy - 0.5], [gx - 0.5]], order=1)[0]))
    pts = np.array(pts); zs = np.array(zs) * s
    tri = Delaunay(pts).simplices
    keep = []
    for a, b, c in tri:
        cxy = (pts[a] + pts[b] + pts[c]) / 3
        if inside.contains(Point(cxy)):
            area = (pts[b][0] - pts[a][0]) * (pts[c][1] - pts[a][1]) - (pts[c][0] - pts[a][0]) * (pts[b][1] - pts[a][1])
            keep.append((a, c, b) if area > 0 else (a, b, c))   # image y runs down: CCW in the model's X/Y
    keep = np.array(keep)
    m.map = (s, cx, cy)
    for sign in (1, -1):
        pos = []
        for (x, y), z in zip(pts, zs):
            X, Y, Z = (x - cx) * s, (cy - y) * s, sign * z
            if warp:
                X, Y, Z = warp(X, Y, Z, x, y)
            pos.append((X, Y, Z))
        m.part(pos, [m.uv(x, y) for x, y in pts], keep if sign > 0 else keep[:, ::-1])
    return s


def build_shovel():
    # the sprite is a top view, blade pointing right and drawn foreshortened (about 10 cm wide at 115 cm
    # long): the blade is widened to a real 23 cm and dished, the shaft stays a round rod
    m = Model("GSHOVEL", hd_sprite("GSHOVEL"))
    cyc = 24.0                          # the shaft's centre line in the sprite

    def cap(x, y):
        return 1.1 if x < 345 else (1.4 if x < 378 else 0.35)

    def warp(X, Y, Z, x, y):
        k = min(1.0, max(0.0, (x - 360) / 28.0))          # 0 at the neck, 1 across the blade
        v = (cyc - y)                                      # sprite px across the blade
        Yw = Y * (1.0 + 1.2 * k)
        edge = min(1.0, abs(v) / 24.0)
        return X, Yw, Z + k * 3.2 * edge * edge           # the blade's sides curl up into a dish
    inflate(m, width_cm=115.0, thick_cm=2.2, cap_at=cap, warp=warp, spacing_cm=1.6)
    return m, "inflate+recipe", "Top-view sprite: round shaft, blade widened to 23 cm and dished"


# ---------------------------------------------------------------- torus / shell helpers
def torus(m, centre3, e_rad, e_norm, img_centre, img_rad, R, r, n_ring=24, n_tube=6):
    """a ring of wire: centre3(phi) the centreline point (cm), e_rad(phi) its outward radial, e_norm the ring
    plane's normal; img_centre(phi) the matching wire pixel in the sprite, img_rad(phi) the radial there"""
    pos, uv, idx = [], [], []
    for i in range(n_ring + 1):
        ph = 2 * math.pi * i / n_ring
        c, er = np.array(centre3(ph)), np.array(e_rad(ph))
        ix, iy = img_centre(ph)
        rx, ry = img_rad(ph)
        for j in range(n_tube + 1):
            ps = 2 * math.pi * j / n_tube
            pos.append(tuple(c + r * (math.cos(ps) * er + math.sin(ps) * np.array(e_norm))))
            uv.append(m.uv(ix + rx * math.cos(ps) * 0.7, iy + ry * math.cos(ps) * 0.7))
    for i in range(n_ring):
        for j in range(n_tube):
            a, b = i * (n_tube + 1) + j, (i + 1) * (n_tube + 1) + j
            idx += [a, b, b + 1, a, b + 1, a + 1]
    m.part(pos, uv, idx)


def build_key(name, width_cm, bit_end, shaft, bow, ring):
    """the key and the big ring through its bow: bit extruded flat, shaft a rod, bow and ring tori (the sprite's
    wide ellipse is a round ring lying tipped toward the viewer)"""
    m = Model(name, hd_sprite(name))
    ys, xs = np.nonzero(m.a[:, :, 3] > 127)
    s = width_cm / (xs.max() - xs.min() + 1)
    cx, cy = (xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2
    P = lambda x, y, z=0.0: np.array(((x - cx) * s, (cy - y) * s, z * s))
    # the bit: only the pixels left of bit_end, extruded
    full = m.a.copy()
    m.a[:, bit_end:, 3] = 0
    extrude(m, width_cm=0, thick_cm=shaft[3] * s * 0.9, tol=0.8, mapping=(s, cx, cy))
    m.a = full
    # the shaft: a rod along x
    x0, x1, sy, sr = shaft
    pos, uv, idx = [], [], []
    for k in range(9):
        t = 2 * math.pi * k / 8
        for x in (x0, x1):
            pos.append(tuple(P(x, sy - sr * math.cos(t), sr * math.sin(t))))
            uv.append(m.uv(x, sy - sr * math.cos(t) * 0.7))
    for k in range(8):
        a = 2 * k
        idx += [a, a + 1, a + 3, a, a + 3, a + 2]
    m.part(pos, uv, idx)
    # the bow: a ring in the key's own plane
    bx, by, bR, br = bow
    torus(m, lambda ph: P(bx + bR * math.cos(ph), by - bR * math.sin(ph)),
          lambda ph: (math.cos(ph), math.sin(ph), 0.0), (0.0, 0.0, 1.0),
          # (the tube's round reads a narrower band of the drawn line than its 3D radius: the drawn line is thinner and
          # the masked material cuts what strays off it)
          lambda ph: (bx + bR * math.cos(ph), by - bR * math.sin(ph)), lambda ph: (0.45 * br * math.cos(ph), -0.45 * br * math.sin(ph)),
          R=bR * s, r=br * s, n_ring=20, n_tube=6)
    # the key ring: radius Rx, tipped so its outline is the sprite's ellipse (Ry = Rx sin a)
    rx_, ry_, rR, rRy, rr = ring
    # lying on the floor (the game lays keys flat) the ring lies nearly in the key's plane, 15 degrees out of it
    # (tipped to the sprite's ellipse it stood on edge once the key was laid down; user 2026-09-28)
    sa = math.cos(math.radians(15.0))
    ca = math.sqrt(1 - sa * sa)
    Tv = np.array((0.0, -sa, ca))
    C = P(rx_, ry_)
    torus(m, lambda ph: C + rR * s * (math.cos(ph) * np.array((1.0, 0, 0)) + math.sin(ph) * Tv),
          lambda ph: math.cos(ph) * np.array((1.0, 0, 0)) + math.sin(ph) * Tv, (0.0, -ca, -sa),
          lambda ph: (rx_ + rR * math.cos(ph), ry_ + rRy * math.sin(ph)),
          lambda ph: (0.45 * rr * math.cos(ph), 0.45 * rr * math.sin(ph)),
          R=rR * s, r=rr * s, n_ring=36, n_tube=6)
    m.map = (s, cx, cy)
    m.lay = "flat"                                         # the bows would stand a key on edge
    return m, "recipe", "Recipe: flat bit, round shaft, torus bow, tipped torus key ring"


def build_ironkey():
    return build_key("GIRONKEY", 11.0, 27, (25, 82, 30.3, 4.0), (98, 37.7, 16.5, 3.5), (166, 46, 73, 18, 3.0))


def build_brasskey():
    return build_key("GBRSSKEY", 9.0, 28, (25, 86, 31.7, 4.5), (103.5, 37, 17.5, 4.0), (175, 45.5, 74.5, 19, 3.5))


def shotshell(m, xf, brass, hull, r=1.0, hull_len=5.4, head_len=1.3):
    """one 12-gauge shell along its local +Y: paper hull, then the brass head with the primer end (drawn from the
    loose shell's end in the sprite) on top; the same for the box's shells and the loose one"""
    pos, idx = [], []
    for k in range(13):
        t = 2 * math.pi * k / 12
        pos += [(0.97 * r * math.sin(t), hull_len, 0.97 * r * math.cos(t)), (0.97 * r * math.sin(t), 0, 0.97 * r * math.cos(t))]
    for k in range(12):
        a = 2 * k
        idx += [a, a + 1, a + 3, a, a + 3, a + 2]
    m.part(pos, [(0, 0)] * len(pos), idx, color=hull, xf=xf)
    pos = [(0, 0, 0)] + [(0.97 * r * math.sin(2 * math.pi * k / 12), 0, 0.97 * r * math.cos(2 * math.pi * k / 12)) for k in range(12)]
    m.part(pos, [(0, 0)] * 13, [v for k in range(12) for v in (0, 1 + (k + 1) % 12, 1 + k)], color=hull, xf=xf)
    drum(m, 84, 156, 9, 10, 4, radius=r * 1.02, height=head_len, segs=12, side_color=brass, bottom_color=brass,
         xf=xf @ rot((0, 1, 0), 0, (0, hull_len, 0)))


def build_pistol_held():
    """the held revolver: the frame with the cylinder's window cut out, and the cylinder on its own (a drum below
    the bore, fluted sides from the sprite, the ends drawn with six loaded chambers) so it can swing out on the
    crane and turn"""
    m = Model("GPISTOL", hd_sprite("GPISTOL"))
    cyl_x0, cyl_x1, cyl_y, cyl_r = 31.5, 60.5, 15.0, 9.0
    full = action_cut(m)
    m.a[6:25, 31:62, 3] = 0
    b = PISTOL_BARREL
    m.a[b["rows"][0]:b["rows"][1] + 1, b["x_end"]:int(b["x_muzzle"]) + 1, 3] = 0
    ys, xs = np.nonzero(full[:, :, 3] > 127)          # the scale from the whole sprite
    s0 = 30.0 / (xs.max() - xs.min() + 1)
    inflate(m, width_cm=30.0, thick_cm=3.6, mapping=(s0, (xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2))
    m.a = full
    tubes(m, b["x_muzzle"], b["x_end"], [(by, r, 0.0, op) for by, r, _, op in b["tubes"]])
    action_add(m, full)
    s, cx, cy = m.map
    k0 = len(m.parts)
    D = 64
    metal = np.array(m.sample(46, 15, 2), np.float32)
    yy, xx = np.mgrid[0:D, 0:D] + 0.5
    rr = np.hypot(xx - D / 2, yy - D / 2) / (D / 2)
    img = np.zeros((D, D, 3), np.float32) + metal * 0.9
    img[rr > 0.92] = metal * 0.6
    for c in range(6):
        a = math.radians(90 + 60 * c)
        px, py = D / 2 + 0.55 * (D / 2) * math.cos(a), D / 2 - 0.55 * (D / 2) * math.sin(a)
        d = np.hypot(xx - px, yy - py) / (D / 2)
        img[d < 0.24] = (28, 25, 22)               # an empty chamber (the game shows the rounds loaded in it)
    img[rr < 0.10] = metal * 0.5                   # the axis pin
    top = m.add_block(Image.fromarray(np.dstack([img.clip(0, 255), np.full((D, D), 255)]).astype(np.uint8), "RGBA"), rescale=True)
    Cy = (cy - cyl_y) * s
    R = cyl_r * s
    segs = 12
    rings = [(cyl_x0, 0.86), (cyl_x0 + 1.5, 1.0), (cyl_x1 - 1.5, 1.0), (cyl_x1, 0.86)]
    pos, uv, idx = [], [], []
    for xpx, f in rings:
        for j in range(segs + 1):
            t = 2 * math.pi * j / segs
            pos.append(((xpx - cx) * s, Cy + f * R * math.sin(t), f * R * math.cos(t)))
            uv.append(m.uv(xpx, cyl_y - f * cyl_r * math.sin(t) * 0.95))
    for i in range(len(rings) - 1):
        for j in range(segs):
            a, b = i * (segs + 1) + j, (i + 1) * (segs + 1) + j
            idx += [a, b, b + 1, a, b + 1, a + 1]
    m.part(pos, uv, idx)
    for xpx in (cyl_x0, cyl_x1):
        pos = [((xpx - cx) * s, Cy, 0.0)]
        uv = [m.uv(D / 2, top + D / 2)]
        for j in range(segs):
            t = 2 * math.pi * j / segs
            pos.append(((xpx - cx) * s, Cy + 0.86 * R * math.sin(t), 0.86 * R * math.cos(t)))
            uv.append(m.uv(D / 2 + (D / 2 - 2) * math.cos(t), top + D / 2 - (D / 2 - 2) * math.sin(t)))
        m.part(pos, uv, [v for j in range(segs) for v in (0, 1 + j, 1 + (j + 1) % segs)])
    m.groups["cyl"] = list(range(k0, len(m.parts)))
    m.cyl = dict(centre=(((cyl_x0 + cyl_x1) / 2 - cx) * s, Cy, 0.0), radius=R, length=(cyl_x1 - cyl_x0) * s,
                 chamber_r=0.55 * 0.86 * R, hole_r=0.24 * 0.86 * R)
    return m


# ---------------------------------------------------------------- moving parts: hammers and triggers
# sprite px rects (x0, y0, x1, y1, inclusive) and pivots; twin = the doubles' two hammers side by side
# (the first-person sprites show both); fired / pulled poses are set by the game, the sprite pose is at rest
ACTION = {
    "GPISTOL": dict(hammer=(26, 0, 35, 8), hammer_pivot=(34, 9), trigger=(35, 35, 37, 40), trigger_pivot=(36, 34)),
    "GSAWGUN": dict(hammer=(142, 0, 151, 8), hammer_pivot=(148, 9), trigger=(152, 30, 156, 34), trigger_pivot=(154, 29),
                    twin=True, tang=(150, 166), selector=(140, 20)),
    "GSGUN": dict(hammer=(156, 2, 164, 8), hammer_pivot=(157, 9), trigger=(159, 29, 162, 37), trigger_pivot=(161, 28)),
    "GRIFLE": dict(hammer=(186, 2, 194, 8), hammer_pivot=(189, 9), trigger=(192, 18, 194, 22), trigger_pivot=(193, 17),
                   lever=(178, 17, 199, 31), lever_pivot=(181, 17)),
    "GDBSGUN": dict(hammer=(174, 2, 181, 6), hammer_pivot=(176, 7), trigger=(183, 25, 186, 30), trigger_pivot=(184, 24),
                    twin=True, tang=(182, 200), selector=(165, 17)),
}


def lever_mask(m, spec, full):
    """the lever's pixels: dark metal inside its rect, less the trigger and the stock's outline"""
    x0, y0, x1, y1 = spec["lever"]
    h, w = full.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    dark = full[:, :, :3].sum(2) < 200
    mask = (xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1) & dark & (full[:, :, 3] > 127)
    tx0, ty0, tx1, ty1 = spec["trigger"]
    mask &= ~((xx >= tx0) & (xx <= tx1) & (yy >= ty0) & (yy <= ty1))
    mask &= ~((xx >= 195) & (yy < 26))
    return mask


def action_cut(m):
    """take the hammer, trigger (and lever) pixels out of the body before it is built; returns the full alpha"""
    full = m.a.copy()
    spec = ACTION.get(m.name)
    if spec:
        for k in ("hammer", "trigger"):
            x0, y0, x1, y1 = spec[k]
            m.a[y0:y1 + 1, x0:x1 + 1, 3] = 0
        if "lever" in spec:
            m.a[lever_mask(m, spec, full), 3] = 0
    return full


def action_add(m, full, twin_z=0.0):
    """the hammer(s) and trigger as their own parts (groups 'hammer', 'trigger'), inflated thin"""
    spec = ACTION.get(m.name)
    m.a = full
    if not spec:
        return
    if not hasattr(m, "groups"):
        m.groups = {}
    for k, thick in (("hammer", 0.8), ("trigger", 0.5), ("lever", 0.7)):
        if k not in spec:
            continue
        only = full.copy()
        if k == "lever":
            keep = lever_mask(m, spec, full)
        else:
            x0, y0, x1, y1 = spec[k]
            keep = np.zeros(only.shape[:2], bool)
            keep[y0:y1 + 1, x0:x1 + 1] = True
        only[~keep, 3] = 0
        if not (only[:, :, 3] > 127).any():
            continue
        m.a = only
        k0 = len(m.parts)
        inflate(m, width_cm=0, thick_cm=thick, mapping=m.map, spacing_cm=0.25)
        new = list(range(k0, len(m.parts)))
        if k == "hammer" and spec.get("twin") and twin_z:
            for i in list(new):
                q = dict(m.parts[i])
                q["pos"] = m.parts[i]["pos"] + np.array((0.0, 0.0, twin_z * 0.85))
                m.parts[i]["pos"] = m.parts[i]["pos"] - np.array((0.0, 0.0, twin_z * 0.85))
                m.parts.append(q)
                new.append(len(m.parts) - 1)
        m.groups[k] = new
        m.a = full
    m.a = full
    if spec.get("tang"):
        tang(m, *spec["tang"], width_cm=max(0.9, twin_z * 0.8))


def tang(m, x0, x1, width_cm=1.0):
    """the metal tang plate along the top of the wrist, behind the action (the first-person sprites show it)"""
    s, cx, cy = m.map
    a = m.a[:, :, 3] > 127
    xs = [x for x in range(x0, x1 + 1, 2) if a[:, x].any()]
    col = m.sample(x0 - 8, int(np.nonzero(a[:, x0 - 8])[0].min()) + 6, 1)
    pos, idx = [], []
    for i, x in enumerate(xs):
        top = np.nonzero(a[:, x])[0].min()
        Y = (cy - top) * s + 0.08
        X = (x - cx) * s
        pos += [(X, Y, -width_cm / 2), (X, Y, width_cm / 2)]
        if i:
            b = 2 * (i - 1)
            idx += [b, b + 2, b + 3, b, b + 3, b + 1]
    m.part(pos, [(0, 0)] * len(pos), idx, color=col)


def tubes(m, x_muzzle, x_end, specs):
    """barrels and tubes along x: specs = (centre_y px, radius px, lateral z cm, open bore?)"""
    s, cx, cy = m.map
    x0, x1 = sorted((x_muzzle, x_end))
    dark = [22, 20, 18]
    segs = 10
    for by, r_px, zc, bore_open in specs:
        R = r_px * s
        pos, uv, idx = [], [], []
        for x in (x0, x1):
            for j in range(segs + 1):
                t = 2 * math.pi * j / segs
                pos.append(((x - cx) * s, (cy - by) * s + R * math.sin(t), zc + R * math.cos(t)))
                uv.append(m.uv(x, by - r_px * math.sin(t) * 0.9))
        for j in range(segs):
            idx += [j, segs + 1 + j, segs + 2 + j, j, segs + 2 + j, j + 1]
        m.part(pos, uv, idx)
        X = (x_muzzle - cx) * s
        ring = [(X, (cy - by) * s + R * math.sin(2 * math.pi * j / segs), zc + R * math.cos(2 * math.pi * j / segs)) for j in range(segs)]
        inner = [(X, (cy - by) * s + 0.72 * R * math.sin(2 * math.pi * j / segs), zc + 0.72 * R * math.cos(2 * math.pi * j / segs)) for j in range(segs)]
        idx = []
        for j in range(segs):
            a, b = j, (j + 1) % segs
            idx += [a, b, segs + b, a, segs + b, segs + a]
        m.part(ring + inner, [m.uv(x_muzzle + 1, by)] * (2 * segs), idx)
        centre = (X, (cy - by) * s, zc)
        if bore_open:
            m.part([centre] + inner, [(0, 0)] * (segs + 1), [v for j in range(segs) for v in (0, 1 + j, 1 + (j + 1) % segs)], color=dark)
        else:
            m.part([centre] + inner, [m.uv(x_muzzle + 1, by)] * (segs + 1), [v for j in range(segs) for v in (0, 1 + j, 1 + (j + 1) % segs)])


def build_gun(name, width_cm, thick_cm, barrel=None):
    """a held gun: body inflated with the barrel band and the moving parts cut out, then the barrels (twin
    side by side, or barrel over magazine tube), the hammer(s), trigger and tang"""
    m = Model(name, hd_sprite(name))
    full = action_cut(m)
    if barrel:
        y0, y1 = barrel["rows"]
        xa, xb = sorted((barrel["x_muzzle"], barrel["x_end"]))
        m.a[y0:y1 + 1, max(0, int(xa) - 2):int(xb) + 1, 3] = 0
    # the scale from the whole sprite (the cut barrel band would shrink its box and grow the gun)
    ys, xs = np.nonzero(full[:, :, 3] > 127)
    s0 = width_cm / (xs.max() - xs.min() + 1)
    inflate(m, width_cm=width_cm, thick_cm=thick_cm,
            mapping=(s0, (xs.min() + xs.max() + 1) / 2, (ys.min() + ys.max() + 1) / 2))
    s = m.map[0]
    twin_z = 0.0
    if barrel:
        specs = []
        for by, r_px, lateral, bore_open in barrel["tubes"]:
            z = lateral * r_px * s
            twin_z = max(twin_z, abs(z))
            specs.append((by, r_px, z, bore_open))
        m.a = full
        tubes(m, barrel["x_muzzle"], barrel["x_end"], specs)
    action_add(m, full, twin_z=twin_z)
    return m, "inflate+parts", "Inflated body; barrels, hammer(s), trigger and tang as parts (back-view detail from the first-person sprites)"


def build_sawgun():
    return build_gun("GSAWGUN", 58.0, 4.2, barrel=dict(x_muzzle=2, x_end=134, rows=(3, 17),
                     tubes=[(10.0, 5.5, -0.97, True), (10.0, 5.5, 0.97, True)]))


def build_dbsgun():
    return build_gun("GDBSGUN", 112.0, 4.6, barrel=dict(x_muzzle=5, x_end=148, rows=(6, 13),
                     tubes=[(9.5, 4.0, -0.97, True), (9.5, 4.0, 0.97, True)]))


def build_rifle():
    # the Henry: barrel on top, the magazine tube under it (rows 3-8 over 9-14), both to the fore-end
    return build_gun("GRIFLE", 110.0, 4.4, barrel=dict(x_muzzle=1, x_end=101, rows=(3, 14),
                     tubes=[(5.6, 2.8, 0.0, True), (11.6, 2.7, 0.0, False)]))


def build_sgun():
    return build_gun("GSGUN", 105.0, 4.4)


PISTOL_BARREL = dict(x_muzzle=119.5, x_end=72, rows=(3, 12), tubes=[(7.5, 5.0, 0.0, True)])     # a round barrel with an open bore (user 2026-09-28: it ended flat)


def build_pistol_floor():
    return build_gun("GPISTOL", 30.0, 3.6, barrel=PISTOL_BARREL)


# ================================================================ the items
def build_badge():
    m = Model("GBADGE", os.path.join(HD, "GBADGE.png"), cell=(0, 0, 216, 256))
    extrude(m, width_cm=7.0, thick_cm=0.35, tol=1.0)
    # the sheriff's star twirls in the air, as the game's sprite does (user 2026-09-28): standing, off the floor
    m.lay, m.spin, m.hover_cm = "up", 180, 20
    return m, "extrude", "Silhouette extrusion: alpha contour simplified to a polygon, sprite on both faces"


def build_elixir():
    m = Model("GELIXIR", os.path.join(HD, "GELIXIR.png"))
    lathe(m, height_cm=18.0, segs=14)
    return m, "lathe", "Lathe: each sprite row's width becomes a radius, the sprite wraps round the body"


def build_canteen():
    m = Model("GCANTEEN", os.path.join(HD, "GCANTEEN.png"))
    # the sprite's top is foreshortened 5.5x and its cross is cut off by the sprite edge, so the cap is a
    # clean top-down texture drawn from colours sampled off the sprite: cloth, a stitched rim, the red cross
    rng = np.random.default_rng(7)
    D = 128
    cloth = np.array(m.sample(165, 22, 3), np.float32)
    rim = np.array(m.sample(30, 30, 1), np.float32)
    red = np.array(m.sample(107, 6, 2), np.float32)
    yy, xx = np.mgrid[0:D, 0:D] + 0.5
    r = np.hypot(xx - D / 2, yy - D / 2) / (D / 2)
    img = cloth * (1.0 - 0.10 * r ** 2)[..., None] + rng.normal(0, 5, (D, D, 1))
    ring = (r > 0.86) & (r < 0.93)
    img[ring] = img[ring] * 0.55 + rim * 0.45
    stitch = (r > 0.89) & (r < 0.905) & ((np.degrees(np.arctan2(yy - D / 2, xx - D / 2)) % 6) < 3)
    img[stitch] *= 0.7
    arm, half = 0.18 * D, 0.24 * D                        # a squat bold cross (user 2026-09-29: thicker, a little smaller)
    c = D / 2
    cross = ((abs(xx - c) < arm / 2) & (abs(yy - c) < half)) | ((abs(yy - c) < arm / 2) & (abs(xx - c) < half))
    img[cross] = red + rng.normal(0, 4, (int(cross.sum()), 3))
    block = np.dstack([np.clip(img, 0, 255), np.full((D, D), 255)]).astype(np.uint8)
    top = m.add_block(Image.fromarray(block, "RGBA"))
    # side ellipse centre (107,17) radii 104x19; side band 24 px; spout at the left
    drum(m, 107, 17, 104, 19, 24, radius=8.0, height=1.9, dome=0.9, segs=20, band_inset=0.88,
         cap=(D / 2, top + D / 2, D / 2 - 1, D / 2 - 1))
    spout = m.sample(8, 32, 2)
    drum(m, 8, 30, 6, 3, 8, radius=0.8, height=1.4, segs=8, side_color=spout, bottom_color=spout,
         xf=rot((0, 0, 1), 90, (-8.0, 1.1, 0)))
    return m, "recipe", "Recipe: domed drum, band from the sprite's side, cap redrawn top-down from sampled colours"


def build_ammobox():
    m = Model("GAMBOXS", os.path.join(HD, "GAMBOXS.png"))
    W, H, D = 11.0, 8.6, 6.0
    x0, x1, y0, y1, z0, z1 = -W/2, W/2, 0, H, -D/2, D/2
    front = [(1, 55), (111, 52), (108, 143), (2, 137)]
    side = [(111, 52), (123, 48), (121, 138), (108, 143)]
    m.quad([(x0,y1,z1),(x1,y1,z1),(x1,y0,z1),(x0,y0,z1)], front)                  # front
    m.quad([(x1,y1,z1),(x1,y1,z0),(x1,y0,z0),(x1,y0,z1)], side)                   # right
    m.quad([(x0,y1,z0),(x0,y1,z1),(x0,y0,z1),(x0,y0,z0)], [side[1], side[0], side[3], side[2]])   # left = mirrored right
    m.quad([(x1,y1,z0),(x0,y1,z0),(x0,y0,z0),(x1,y0,z0)], front)                  # back
    card = m.sample(60, 125, 2)
    box_solid(m, x0, x1, 0.0, 0.05, z0, z1, [int(c * 0.7) for c in card])        # bottom
    box_solid(m, x0 + 0.1, x1 - 0.1, y1 - 1.2, y1 - 1.0, z0 + 0.1, z1 - 0.1, [int(c * 0.5) for c in card])   # shell tray
    # lid standing up off the back edge, leaning back 18 degrees
    lid = [(32, 5), (130, 1), (129, 23), (31, 23)]      # the band above the sprite's shells only
    xf = rot((1, 0, 0), -18, (0, y1, z0))
    m.quad([(x0, 7.5, 0), (x1, 7.5, 0), (x1, 0, 0), (x0, 0, 0)], lid, xf=xf)
    # 2 x 5 shells standing head up, 1.5 cm proud of the box, and one lying in front: all the same shell
    red, brass = m.sample(45, 140, 2), m.sample(100, 150, 1)
    for row, zc in enumerate((-1.3, 1.3)):
        for i in range(5):
            xc = x0 + 1.1 + i * 2.2
            shotshell(m, rot((0, 1, 0), 0, (xc, y1 + 1.5 - 6.7 + (0.25 if row == 0 else 0), zc)), brass, red)
    shotshell(m, rot((0, 1, 0), 35, (1.5, 1.0, D / 2 + 2.0)) @ rot((0, 0, 1), -90), brass, red)
    return m, "recipe", "Recipe: box faces projected from hand-picked sprite quads, primitives for the shells"


# ================================================================ output
def png_b64(im):
    b = io.BytesIO(); im.save(b, "PNG"); return base64.b64encode(b.getvalue()).decode()


def write_obj(m, d):
    os.makedirs(d, exist_ok=True)
    m.img.save(os.path.join(d, m.name + ".png"))
    mtl = [f"newmtl tex\nmap_Kd {m.name}.png\nmap_d {m.name}.png\n"]
    lines, vo = [f"mtllib {m.name}.mtl"], 1
    for i, p in enumerate(m.parts):
        if p["color"]:
            mtl.append(f"newmtl col{i}\nKd {p['color'][0]/255:.3f} {p['color'][1]/255:.3f} {p['color'][2]/255:.3f}\n")
        lines.append(f"usemtl {'col%d' % i if p['color'] else 'tex'}")
        for v in p["pos"]:
            lines.append(f"v {v[0]:.4f} {v[1]:.4f} {v[2]:.4f}")
        for t in p["uv"]:
            lines.append(f"vt {t[0]:.5f} {t[1]:.5f}")
        for a, b, c in p["idx"].reshape(-1, 3):
            lines.append(f"f {a+vo}/{a+vo} {b+vo}/{b+vo} {c+vo}/{c+vo}")
        vo += len(p["pos"])
    open(os.path.join(d, m.name + ".obj"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(d, m.name + ".mtl"), "w").write("\n".join(mtl))


def hd_sprite(name):
    import glob
    return glob.glob(os.path.join(HD, name + ".png"))[0]


AUTO = [   # name, builder, size cm (width for extrude, height for lathe), thickness cm
    ("GPOTION", "lathe", 24.0, 0),
]


def auto_builders():
    for name, kind, size, thick in AUTO:
        def fn(name=name, kind=kind, size=size, thick=thick):
            m = Model(name, hd_sprite(name))
            if kind == "inflate":
                inflate(m, width_cm=size, thick_cm=thick)
                return m, kind, "Automatic inflated extrusion (rounded cross-section)"
            lathe(m, height_cm=size, segs=14)
            return m, kind, "Automatic lathe"
        yield fn


def write_ue_obj(m, d):
    """runtime copy for OutlawsUE -OLItemTest: right-handed Z-up cm (the loader mirrors Y), one texture per item:
    solid-colour parts point at swatches packed under the sprite, edge colours bled into the transparent
    pixels so the masked world material shows no dark fringe"""
    from scipy import ndimage
    os.makedirs(d, exist_ok=True)
    a = np.asarray(m.img).copy()
    rgb, al = a[:, :, :3].astype(np.float32), a[:, :, 3] > 127
    iy = ix = None
    if (~al).any() and al.any():
        _, (iy, ix) = ndimage.distance_transform_edt(~al, return_indices=True)
        rgb = rgb[iy, ix]
    uniq = []
    for p in m.parts:
        if p["color"] and p["color"] not in uniq:
            uniq.append(p["color"])
    sw = 4
    per_row = max(1, m.W // sw)
    extra = ((len(uniq) + per_row - 1) // per_row) * sw if uniq else 0
    H2 = m.H + extra
    tex = np.zeros((H2, m.W, 4), np.uint8)
    tex[:m.H, :, :3] = rgb.astype(np.uint8)
    tex[:m.H, :, 3] = a[:, :, 3]
    swatch_uv = {}
    for i, c in enumerate(uniq):
        x0, y0 = (i % per_row) * sw, m.H + (i // per_row) * sw
        tex[y0:y0 + sw, x0:x0 + sw, :3] = c
        tex[y0:y0 + sw, x0:x0 + sw, 3] = 255
        swatch_uv[tuple(c)] = ((x0 + sw / 2) / m.W, 1.0 - (y0 + sw / 2) / H2)
    Image.fromarray(tex, "RGBA").save(os.path.join(d, m.name + ".png"))
    mtls = ["tex"] + sorted({p["mtl"] for p in m.parts if p.get("mtl")})
    open(os.path.join(d, m.name + ".mtl"), "w").write("".join(f"newmtl {n}\nmap_Kd {m.name}.png\n" for n in mtls))
    lines, vo = [f"mtllib {m.name}.mtl", "usemtl tex"], 1
    cur = "tex"
    for p in m.parts:
        want = p.get("mtl") or "tex"                       # (a part may name its material: "glass" is drawn see-through)
        if want != cur:
            lines.append(f"usemtl {want}")
            cur = want
        for x, y, z in p["pos"]:
            lines.append(f"v {x:.4f} {-z:.4f} {y:.4f}")        # three.js Y-up -> right-handed Z-up
        for u, v in p["uv"]:
            if p["color"]:
                u, v = swatch_uv[tuple(p["color"])]
            else:
                # the masked material cuts where the texture is clear: only the flat cut-outs (alpha parts) mean it.  A
                # solid part (a rod, a ring, a turned body) that reads a clear pixel takes the nearest opaque one instead
                # (user 2026-09-28: the steel key's thin ring was cut away where its UVs strayed off the drawn ring)
                if not p["alpha"] and iy is not None:
                    px = min(m.W - 1, max(0, int(u * m.W)))
                    py = min(m.H - 1, max(0, int((1.0 - v) * m.H)))
                    if not al[py, px]:
                        u, v = (ix[py, px] + 0.5) / m.W, 1.0 - (iy[py, px] + 0.5) / m.H
                v = 1.0 - (1.0 - v) * m.H / H2
            lines.append(f"vt {u:.5f} {v:.5f}")
        for i0, i1, i2 in p["idx"].reshape(-1, 3):
            lines.append(f"f {i0+vo}/{i0+vo} {i1+vo}/{i1+vo} {i2+vo}/{i2+vo}")
        vo += len(p["pos"])
    open(os.path.join(d, m.name + ".obj"), "w").write("\n".join(lines) + "\n")
    write_sizes(m, d)


def write_sizes(m, d):
    """<name>.json beside the OBJ: the model's real size (for the hands) and the scale that makes it as big
    as the original's sprite in the world (for items lying about), from the SD sprite's world size"""
    import glob
    allp = np.vstack([p["pos"] for p in m.parts])
    ext = allp.max(0) - allp.min(0)                        # three.js axes: x along, y up, z toward the viewer
    out = dict(name=m.name, real_size_cm=[round(float(ext[0]), 2), round(float(ext[2]), 2), round(float(ext[1]), 2)])
    if getattr(m, "lay", None):
        out["lay"] = m.lay                                 # "flat" / "up": how it lies on the floor (the game reads it)
    for flag in ("any_yaw", "keep_origin"):               # scenery: turned by where it stands; stood on its own origin
        if getattr(m, flag, False):
            out[flag] = True
    if getattr(m, "spin", 0):
        out["spin"] = m.spin                               # degrees a second about the vertical (a pickup that twirls)
        out["hover_cm"] = getattr(m, "hover_cm", 0)        # held that far off the floor (world cm)
    sd = glob.glob(os.path.join(HERE, "..", "out", "*", "sprites", m.name + ".json"))
    if sd:
        j = json.load(open(sd[0]))
        cm_per_px = j["w_cm"] / j["px_w"] / (Image.open(m.src).width / j["atlas_w"])
        ys, xs = np.nonzero(np.asarray(m.img)[:m.SH, :, 3] > 127)
        world = max(xs.max() - xs.min() + 1, ys.max() - ys.min() + 1) * cm_per_px
        out["sprite_world_cm"] = round(float(world), 1)
        out["world_scale"] = round(float(world / max(ext[0], ext[1])), 3)
    if getattr(m, "world_scale", None):
        out["world_scale"] = m.world_scale                 # built at its real size (a box drawn three-quarter on: the
                                                           # sprite's width is front and side together)
    if getattr(m, "extra_json", None):
        out.update(m.extra_json)                           # the game's animation keys (anim_catalogue, 2026-10-01)
    import anim_keys                                       # (the prop sprites' animations as keyframes)
    out.update(anim_keys.ANIMS.get(m.name, {}))
    import traits                                          # (what the hands do with it: hold, breaks; backlog 4)
    traits.merged(m.name, out)
    json.dump(out, open(os.path.join(d, m.name + ".json"), "w"), indent=1)
    print(f"   sizes {out}")


# ---------------------------------------------------------------- held weapons (OutlawsUE hands)
HELD_OUT = os.path.join(UE_OUT, "held")
# sprite pixel points (HD px): grip = the hand's centre (becomes the origin), muzzle = bore centre at the
# tip; sights on the top edge; +1 = the barrel points right in the sprite, -1 = left (turned round)
HELD = {
    "GPISTOL": dict(dir=1, grip=(17, 40), muzzle=(119.5, 7.5), sight_rear=(40, 2), sight_front=(116, 0.5),
                    hammer=(32, 1.5), load=(47, 14), eject=(47, 14)),
    "GSAWGUN": dict(dir=-1, grip=(170, 25), muzzle=(2, 9.7), sight_rear=(133, 3), sight_front=(4, 3),
                    hammer=(143, 5), load=(135, 10), eject=(135, 10)),
    "GSGUN": dict(dir=-1, grip=(183, 22), muzzle=(3, 11), sight_rear=(150, 8), sight_front=(4, 8),
                  hammer=(152, 5), load=(150, 12), eject=(150, 12)),
    "GRIFLE": dict(dir=-1, grip=(208, 16), muzzle=(1, 5.3), sight_rear=(100, 2.5), sight_front=(3, 2),
                   hammer=(192, 4), load=(175, 12), eject=(170, 6)),
    "GDBSGUN": dict(dir=-1, grip=(198, 17), muzzle=(5, 9.5), sight_rear=(170, 6), sight_front=(7, 6.5),
                    hammer=(173, 7), load=(165, 10), eject=(165, 10)),
}


SEL_SIDE = {"GSAWGUN": 2.25, "GDBSGUN": 2.45}      # half the receiver's thickness + a little


def write_held(m, spec=None, tool_tip=None):
    """the model in its hand frame (Unreal axes, cm): +X toward the muzzle (or the tool's business end),
    +Z up, grip at the origin; both faces of every triangle (the held material is one-sided and the left
    hand is a mirror image).  <name>.json beside it carries the gun's points in the same frame."""
    s, cx, cy = m.map
    if spec:
        sg = spec["dir"]
        gx, gy = (spec["grip"][0] - cx) * s, (cy - spec["grip"][1]) * s

        def to_ue(X, Y, Z):                      # three.js model space -> hand frame
            return (sg * (X - gx), sg * Z, Y - gy)
    else:                                        # the shovel: a top view, blade width across the sprite
        # held by the end of the shaft (the fist 4 cm in from it), the blade forward (user 2026-09-28)
        xmin = min(float(part["pos"][:, 0].min()) for part in m.parts)

        def to_ue(X, Y, Z):
            return (X - xmin - 4.0, Y, Z)
    os.makedirs(HELD_OUT, exist_ok=True)
    a = np.asarray(m.img).copy()
    from scipy import ndimage
    al = a[:, :, 3] > 127
    if (~al).any():
        _, (iy, ix) = ndimage.distance_transform_edt(~al, return_indices=True)
        a[:, :, :3] = a[iy, ix, :3]
    # solid-colour parts (sights, screws, bands, the bore) point at swatches packed under the sprite, as write_ue_obj
    # does (2026-09-30: they read the texture's corner pixel, all one wrong colour)
    uniq = []
    for p in m.parts:
        if p["color"] and list(p["color"]) not in uniq:
            uniq.append(list(p["color"]))
    H0, W0 = a.shape[:2]
    sw = 4
    per_row = max(1, W0 // sw)
    extra = ((len(uniq) + per_row - 1) // per_row) * sw if uniq else 0
    H2 = H0 + extra
    if extra:
        a = np.concatenate([a, np.zeros((extra, W0, 4), a.dtype)], 0)
    swatch_uv = {}
    for i, c in enumerate(uniq):
        x0, y0 = (i % per_row) * sw, H0 + (i // per_row) * sw
        a[y0:y0 + sw, x0:x0 + sw, :3] = c
        a[y0:y0 + sw, x0:x0 + sw, 3] = 255
        swatch_uv[tuple(c)] = ((x0 + sw / 2) / W0, 1.0 - (y0 + sw / 2) / H2)

    def uv_of(part, u, v):
        if part["color"]:
            return swatch_uv[tuple(part["color"])]
        return u, 1.0 - (1.0 - v) * H0 / H2
    Image.fromarray(a, "RGBA").save(os.path.join(HELD_OUT, m.name + ".png"))
    open(os.path.join(HELD_OUT, m.name + ".mtl"), "w").write(f"newmtl tex\nmap_Kd {m.name}.png\n")
    groups = getattr(m, "groups", {})
    grouped = {i for g in groups.values() for i in g}

    def obj_of(parts, fname):
        lines, vo = [f"mtllib {m.name}.mtl", "usemtl tex"], 1
        for part in parts:
            for X, Y, Z in part["pos"]:
                ux, uy, uz = to_ue(X, Y, Z)
                lines.append(f"v {ux:.4f} {-uy:.4f} {uz:.4f}")       # the loader mirrors Y back
            for u, v in part["uv"]:
                u, v = uv_of(part, u, v)
                lines.append(f"vt {u:.5f} {v:.5f}")
            for i0, i1, i2 in part["idx"].reshape(-1, 3):
                a_, b_, c_ = i0 + vo, i1 + vo, i2 + vo
                lines.append(f"f {a_}/{a_} {b_}/{b_} {c_}/{c_}")
                lines.append(f"f {a_}/{a_} {c_}/{c_} {b_}/{b_}")
            vo += len(part["pos"])
        open(os.path.join(HELD_OUT, fname), "w").write("\n".join(lines) + "\n")
    obj_of([part for i, part in enumerate(m.parts) if i not in grouped], m.name + ".obj")
    for g, ids in groups.items():
        if ids:
            obj_of([m.parts[i] for i in ids], f"{m.name}_{g}.obj")
    for f in os.listdir(HELD_OUT):                   # parts this gun no longer has
        stem = os.path.splitext(f)[0]
        if f.endswith(".obj") and stem.startswith(m.name + "_") and stem[len(m.name) + 1:] not in groups:
            os.remove(os.path.join(HELD_OUT, f))
    out = dict(name=m.name)
    if spec:
        for k in ("muzzle", "sight_rear", "sight_front", "hammer", "load", "eject"):
            px, py = spec[k]
            out[k] = [round(v, 2) for v in to_ue((px - cx) * s, (cy - py) * s, 0.0)]
    act = ACTION.get(m.name)
    if act and spec:
        for k in ("hammer", "trigger", "lever"):
            if groups.get(k):
                px, py = act[k + "_pivot"]
                out[k + "_pivot"] = [round(v, 3) for v in to_ue((px - cx) * s, (cy - py) * s, 0.0)]
    if act and spec and "selector" in act:
        # the barrel selector's hub on the receiver's side: x, the side's distance out, z (the game puts it on the
        # gun's left as held)
        px, py = act["selector"]
        sx, _, sz = to_ue((px - cx) * s, (cy - py) * s, 0.0)
        out["selector"] = [round(sx, 3), round(SEL_SIDE.get(m.name, 2.3), 3), round(sz, 3)]
    if hasattr(m, "cyl"):
        c = np.array(to_ue(*m.cyl["centre"]))
        R = m.cyl["radius"]
        out["cyl_centre"] = c.round(3).tolist()
        out["cyl_pivot"] = (c + np.array((0.0, -0.5 * R, -1.05 * R))).round(3).tolist()   # the crane, low on the gun's left
        out["cyl_radius"] = round(R, 3)
        out["cyl_length"] = round(m.cyl["length"], 3)
        out["chamber_r"] = round(m.cyl["chamber_r"], 3)     # the chambers' ring on the cylinder's faces
        out["hole_r"] = round(m.cyl["hole_r"], 3)
    allp = np.array([to_ue(*v) for part in m.parts for v in part["pos"]])
    out["bounds_min"] = allp.min(0).round(2).tolist()
    out["bounds_max"] = allp.max(0).round(2).tolist()
    if not spec:
        out["tip"] = round(float(allp[:, 0].max()), 2)
    json.dump(out, open(os.path.join(HELD_OUT, m.name + ".json"), "w"), indent=1)
    print(f"   held {out}")


def main(only=None):
    import sys
    import pickups                                         # the rest of the pickups and the barrels (2026-09-28)
    import ammo                                            # the ammo boxes, second pass (2026-09-29)
    import scenery                                         # trees, rocks, sacks, pots and the like (2026-09-29)
    import guns                                            # the held guns' detail pass (2026-09-30)
    items = []
    fns = (build_badge, guns.build_sawgun, build_elixir, build_canteen, ammo.build_amboxs, build_shovel, build_ironkey, build_brasskey, guns.build_dbsgun, guns.build_pistol_floor, guns.build_sgun, guns.build_rifle) + tuple(auto_builders()) + tuple(pickups.BUILDERS) + tuple(scenery.SCENERY)
    for fn in fns:
        m, kind, note = fn()
        write_obj(m, OUT)
        write_ue_obj(m, UE_OUT)
        if m.name in HELD:
            write_held(guns.held_model(m) if m.name in ("GPISTOL", "GRIFLE", "GSGUN", "GDBSGUN", "GSAWGUN") else m, HELD[m.name])
        elif m.name == "GSHOVEL":
            write_held(m, tool_tip=(508, 24))
        items.append(dict(name=m.name, kind=kind, note=note, tris=m.tris(), tex=png_b64(m.img),
                          parts=[dict(pos=p["pos"].round(4).ravel().tolist(), uv=p["uv"].round(5).ravel().tolist(),
                                      idx=p["idx"].tolist(), color=p["color"], alpha=p["alpha"]) for p in m.parts]))
        print(f"{m.name:10s} {kind:8s} {m.tris():5d} tris  sprite {m.W}x{m.H}")
    tpl = open(os.path.join(HERE, "viewer_tpl.html"), encoding="utf-8").read()
    open(os.path.join(HERE, "viewer.html"), "w", encoding="utf-8").write(tpl.replace("__ITEMS__", json.dumps(items)))


if __name__ == "__main__":
    main()
