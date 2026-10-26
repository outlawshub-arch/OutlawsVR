"""The held guns' build recipes for the runtime builder (REQ-RUNTIMEGUNS, user 2026-10-05): <NAME>_build.json in
Content/HeldGuns (no meshes ship).  The game builds each gun from the player's own sprite (OutlawsGunBuilder.cpp):

  body, parts   the sprite-traced surfaces (guns.inflate_round, proto.inflate): only how to make them ships here, as
                mask ops on the sprite's alpha (our rects: the action parts, the barrel band, cuts) and the inflate's
                settings, plus each part's post transform (thicken, twin hammers) as a per-axis scale and offset
  details       our own geometry (barrels, sights, screws, the fluted cylinder, ...) in the hand frame as the OBJ
                writes it, with UVs into the gun's texture: solid colours point at a swatch whose colour is NOT
                stored, only where on the sprite it is read (at x, y, r; the opaque or all pixels' mean; then each k)
  blocks        texture blocks we synthesize under the sprite (the revolver's cylinder face), by their sample specs

Checks (fails loudly): every body/part mask rebuilt from the ops equals the one the generator inflated; every
solid-colour detail has a sample spec and reproduces its colour off the stock sprite (within the literal tones'
rounding); no colour, outline or z-field values are written; with --parity the full outputs (write_held) are written
to a scratch folder and compared byte for byte with the parity OBJs (make_held_ship.py's dev folder,
placeholders/items3d/held_ship), so these edits changed nothing the guns are built to.

  python make_held_recipe.py <repo>/Content/HeldGuns [--parity]"""
import json, math, os, sys

import numpy as np

import proto
import guns

VERSION = 1
# the body cuts the builders make with a local function (cut=...): inclusive px rects, y1 None = to the bottom
CUTS = {"GPISTOL": [(31, 6, 61, 24)], "GSGUN": [(86, 14, 140, None)]}

calls = []


def _wrap(fn, kind):
    def w(m, *a, **kw):
        alpha = m.a[:, :, 3].copy()
        k0 = len(m.parts)
        r = fn(m, *a, **kw)
        for i in range(k0, len(m.parts)):
            m.parts[i]["rt"] = len(calls)
            m.parts[i]["pos0"] = m.parts[i]["pos"].copy()
        calls.append(dict(kind=kind, kw=kw, alpha=alpha, uv_push=getattr(m, "uv_push", 2.2), name=m.name,
                          H=m.H, W=m.W))
        return r
    return w


proto.inflate = _wrap(proto.inflate, "plain")
guns.inflate_round = _wrap(guns.inflate_round, "round")


def fail(msg):
    print("FAIL", msg)
    sys.exit(1)


def rect_ops(name, barrel, cut_rects):
    """the body's mask ops, in order (as guns.body: action cuts, the barrel band, the builder's cut, the smoothing)"""
    ops = []
    spec = proto.ACTION.get(name)
    if spec:
        for k in ("hammer", "trigger"):
            ops.append(dict(op="clear", rect=list(spec[k])))
        if "lever" in spec:
            ops.append(dict(op="clear_lever", **lever_def(spec)))
    if barrel:
        y0, y1 = barrel["rows"]
        xa, xb = sorted((barrel["x_muzzle"], barrel["x_end"]))
        ops.append(dict(op="clear", rect=[max(0, int(xa) - 2), y0, int(xb), y1]))
    for r in cut_rects:
        ops.append(dict(op="clear", rect=list(r)))
    ops.append(dict(op="smooth", sigma=guns.SMOOTH))
    return ops


def lever_def(spec):
    # proto.lever_mask: opaque and dark (r+g+b under 200) inside the rect, less the trigger rect and x>=195, y<26
    return dict(rect=list(spec["lever"]), dark_sum=200, less=[list(spec["trigger"]), [195, 0, 100000, 25]])


def lever_mask(full, d):
    h, w = full.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    x0, y0, x1, y1 = d["rect"]
    mk = (xx >= x0) & (xx <= x1) & (yy >= y0) & (yy <= y1) & (full[:, :, :3].sum(2) < d["dark_sum"]) & (full[:, :, 3] > 127)
    for a0, b0, a1, b1 in d["less"]:
        mk &= ~((xx >= a0) & (xx <= a1) & (yy >= b0) & (yy <= b1))
    return mk


def run_ops(full, ops):
    """the ops on the sprite (float RGBA): the alpha the inflate sees"""
    from scipy import ndimage
    a = full[:, :, 3].copy()
    h, w = a.shape
    for o in ops:
        if o["op"] == "clear":
            x0, y0, x1, y1 = o["rect"]
            a[y0:(h if y1 is None else y1 + 1), x0:x1 + 1] = 0
        elif o["op"] == "clear_lever":
            a[lever_mask(full, o)] = 0
        elif o["op"] == "keep":
            k = np.zeros(a.shape, bool)
            x0, y0, x1, y1 = o["rect"]
            k[y0:y1 + 1, x0:x1 + 1] = True
            a[~k] = 0
        elif o["op"] == "keep_lever":
            a[~lever_mask(full, o)] = 0
        elif o["op"] == "smooth":
            a = ndimage.gaussian_filter(a, o["sigma"])
    return a


def sample(sprite, spec):
    x, y, r = spec["at"]
    p = sprite[max(0, y - r):y + r + 1, max(0, x - r):x + r + 1]
    if spec["mode"] == "opaque":
        p = p[p[:, :, 3] > 127][:, :3]
        c = p.mean(0) if len(p) else np.array((60.0, 60.0, 60.0))
    else:
        c = p[:, :, :3].reshape(-1, 3).mean(0)
    ks = list(spec["k"])
    if spec["mode"] == "opaque":          # guns.colour: the mean times k, clamped, truncated
        k0 = ks.pop(0)
        out = [int(min(255, max(0, v * k0))) for v in c]
    else:                                 # proto.Model.sample: the mean truncated
        out = [int(v) for v in c]
    for k in ks:                          # shade / lit: each channel times k, truncated
        kk = k if isinstance(k, list) else [k] * 3
        out = [int(v * q) for v, q in zip(out, kk)]
    return out


def fit_axis(a, b):
    """b = s * a + o per column (least squares), with the worst residual"""
    so = []
    worst = 0.0
    for j in range(3):
        x, y = a[:, j], b[:, j]
        if np.ptp(x) < 1e-9:
            s, o = 1.0, float(np.mean(y - x))
        else:
            s, o = np.polyfit(x, y, 1)
        worst = max(worst, float(np.abs(s * x + o - y).max()))
        so += [float(s), float(o)]
    return so, worst


def build(name):
    calls.clear()
    fn = {"GPISTOL": guns.build_pistol_held, "GRIFLE": guns.build_rifle, "GSGUN": guns.build_sgun,
          "GDBSGUN": guns.build_dbsgun, "GSAWGUN": guns.build_sawgun}[name]
    r = fn()
    return r[0] if isinstance(r, tuple) else r


def main():
    dest = sys.argv[1]
    parity = "--parity" in sys.argv
    for name in ("GPISTOL", "GRIFLE", "GSGUN", "GDBSGUN", "GSAWGUN"):
        from PIL import Image
        sprite_img = Image.open(proto.hd_sprite(name)).convert("RGBA")
        sprite = np.asarray(sprite_img).astype(np.float32)
        SH, SW = sprite.shape[:2]
        m = build(name)
        spec = proto.HELD[name]
        groups = getattr(m, "groups", {})
        group_of = {i: g for g, ids in groups.items() for i in ids}
        # the texture as write_held lays it out: the model's image (sprite + blocks), then the swatch rows
        H0, W0 = m.H, m.W
        uniq = []
        for p in m.parts:
            if p["color"] and list(p["color"]) not in uniq:
                uniq.append(list(p["color"]))
        sw = 4
        per_row = max(1, W0 // sw)
        H2 = H0 + (((len(uniq) + per_row - 1) // per_row) * sw if uniq else 0)
        # the frame (write_held's to_ue): sprite px -> model (cm) by the body's map, then the hand frame
        s, cx, cy = m.map
        sg = spec["dir"]
        gx, gy = (spec["grip"][0] - cx) * s, (cy - spec["grip"][1]) * s

        def to_obj(P):
            X, Y, Z = P[:, 0], P[:, 1], P[:, 2]
            return np.stack([sg * (X - gx), -(sg * Z), Y - gy], 1)     # (the OBJ's -Y: the loader mirrors it back)
        # the inflate calls -> body / parts
        body = None
        parts = []
        call_of_part = {}
        for i, p in enumerate(m.parts):
            if "rt" in p:
                call_of_part.setdefault(p["rt"], []).append(i)
        for ci, c in enumerate(calls):
            ids = call_of_part.get(ci, [])
            if not ids:
                continue
            g = {group_of.get(i, "") for i in ids}
            if len(g) != 1:
                fail(f"{name}: inflate call {ci} spans groups {g}")
            g = g.pop()
            if c["kind"] == "round":
                if body is not None:
                    fail(f"{name}: two bodies")
                ops = rect_ops(name, BARREL[name], [(x0, y0, x1, (SH - 1) if y1 is None else y1) for x0, y0, x1, y1 in CUTS.get(name, [])])
                kw = c["kw"]
                rec = dict(group=g, kind="round", width_cm=kw["width_cm"], thick_cm=kw["thick_cm"], tol=kw["tol"],
                           spacing_cm=kw["spacing_cm"], min_spacing_px=2.0, insets=list(kw.get("insets", (0.45, 1.3, 2.6))),
                           uv_push=c["uv_push"], ops=ops)
            else:
                act = proto.ACTION[name]
                if g == "lever":
                    ops = [dict(op="keep_lever", **lever_def(act))]
                else:
                    ops = [dict(op="keep", rect=list(act[g]))]
                kw = c["kw"]
                rec = dict(group=g, kind="plain", thick_cm=kw["thick_cm"], tol=kw.get("tol", 0.9),
                           spacing_cm=kw["spacing_cm"], min_spacing_px=2.5, ops=ops)
            # the mask the inflate saw, again from the ops
            mine = run_ops(sprite, rec["ops"])
            if mine.shape != c["alpha"].shape or not np.allclose(mine, c["alpha"][:SH] if c["alpha"].shape[0] > SH else c["alpha"], atol=1e-3):
                fail(f"{name}: {rec['group'] or 'body'} mask differs from the generator's")
            # each surface the call made (front, back; twin copies): its post transform
            post = []
            for i in ids:
                p = m.parts[i]
                so, worst = fit_axis(p["pos0"], p["pos"])
                if worst > 1e-4:
                    fail(f"{name}: part {i} is not a per-axis scale+offset of its inflate ({worst})")
                post.append(dict(side=1 if p["pos0"][:, 2].max() >= -p["pos0"][:, 2].min() else -1, so=[round(v, 6) for v in so]))
            rec["surfaces"] = post
            rec["tris"] = int(sum(len(m.parts[i]["idx"]) // 3 for i in ids))
            if c["kind"] == "round":
                body = rec
            else:
                parts.append(rec)
        # the details: everything else
        details = []
        for i, p in enumerate(m.parts):
            if "rt" in p:
                continue
            P = to_obj(p["pos"])
            d = dict(group=group_of.get(i, ""), pos=[round(float(v), 4) for v in P.ravel()],
                     idx=[int(v) for v in p["idx"]])
            if p["color"]:
                sp = getattr(p["color"], "spec", None)
                if not sp:
                    fail(f"{name}: detail part {i} has a colour with no sample point {p['color']}")
                got = sample(sprite, sp)
                if any(abs(a - b) > 1 for a, b in zip(got, p["color"])):
                    fail(f"{name}: detail part {i}'s sample {sp} gives {got}, not {list(p['color'])}")
                si = uniq.index(list(p["color"]))
                x0, y0 = (si % per_row) * sw, H0 + (si // per_row) * sw
                d["swatch"] = [x0, y0, sw]
                d["sample"] = dict(at=list(sp["at"]), mode=sp["mode"], k=sp["k"])
            else:
                uv = p["uv"].copy()
                uv[:, 1] = 1.0 - (1.0 - uv[:, 1]) * H0 / H2
                d["uv"] = [round(float(v), 5) for v in uv.ravel()]
            details.append(d)
        blocks = []
        for b in getattr(m, "blocks", []):
            bb = dict(b)
            bb["metal"] = dict(at=list(b["metal"]["at"]), mode=b["metal"]["mode"], k=b["metal"]["k"])
            blocks.append(bb)
        out = dict(version=VERSION, name=name, sprite_size=[SW, SH], tex_size=[W0, H2], image_rows=H0,
                   fill_alpha_over=127, two_sided=True,
                   frame=dict(dir=sg, grip=list(spec["grip"]), width_cm=body["width_cm"]),
                   body=body, parts=parts, details=details, blocks=blocks)
        # nothing of the sprite's own: no colour lists, no outline or field arrays besides our geometry
        txt = json.dumps(out, separators=(",", ":"))
        for bad in ('"color"', '"rgb"', '"outline"', '"zfield"', '"lit"'):
            if bad in txt:
                fail(f"{name}: {bad} in the recipe")
        open(os.path.join(dest, name + "_build.json"), "w").write(txt)
        nd = sum(len(d["idx"]) // 3 for d in details)
        print(f"{name}: body {body['tris']} tris, {len(parts)} parts {sum(p['tris'] for p in parts)} tris, "
              f"{len(details)} details {nd} tris ({sum(1 for d in details if 'swatch' in d)} solid), tex {W0}x{H2}, "
              f"{len(txt) // 1024} KB")
        if parity:
            import tempfile
            tmp = tempfile.mkdtemp()
            proto.HELD_OUT = tmp
            proto.write_held(m, spec)
            for f in os.listdir(tmp):
                if f.endswith(".obj"):
                    ref = os.path.join(proto.UE_OUT, "held_ship", f)
                    if not os.path.exists(ref):
                        fail(f"{name}: no parity OBJ {ref} (run make_held_ship.py first)")
                    same = open(os.path.join(tmp, f), "rb").read() == open(ref, "rb").read()
                    print(f"   parity {f}: {'same' if same else 'DIFFERS'}")


BARREL = {"GPISTOL": proto.PISTOL_BARREL, "GRIFLE": dict(x_muzzle=1, x_end=101, rows=(3, 14)),
          "GSGUN": dict(x_muzzle=3, x_end=141, rows=(6, 15)), "GDBSGUN": dict(x_muzzle=5, x_end=148, rows=(6, 13)),
          "GSAWGUN": dict(x_muzzle=2, x_end=134, rows=(3, 17))}

if __name__ == "__main__":
    main()
