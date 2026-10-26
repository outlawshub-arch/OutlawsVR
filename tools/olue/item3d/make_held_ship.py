"""The five held guns' parity reference (REQ-RUNTIMEGUNS step B): no meshes or images ship any more; the game builds
each gun from the player's own sprite (make_held_recipe.py's <NAME>_build.json).  From the held folder
(proto.write_held) it writes, per gun, into the dev folder placeholders/items3d/held_ship (never the repo or the pak;
the game holds these OBJs only with -OLShippedGuns, and measures the runtime build against them when they are there):

  <NAME>.obj, <NAME>_<part>.obj, <NAME>.mtl, <NAME>.json   copied unchanged
  <NAME>_template.png   the gun's texture with the sprite rows blank (RGBA 0) and every other texel at or under
                        alpha 127 RGB 0 (its colour is the fill's): only our own rows are left (the blocks under
                        the sprite, e.g. the revolver's cylinder face, and the 4x4 colour swatches)
  <NAME>_recipe.json    where the player's HD sprite goes and how the texture is finished at runtime

The game (OutlawsPlayerWeapons.cpp, HeldGunTexture) decodes the player's own HD pickup sprite (<NAME>.SPX frame 0,
BC7 mip 0, 4x the NWX cell), pastes it over rows 0..SH-1, then fills the RGB of every texel at or under alpha 127
in rows 0..dilate_rows-1 from its nearest texel over alpha 127 (Euclidean), as write_held does with scipy before
it appends the swatch rows.  Without the HD sprite it holds a flat card of the low-res pickup sprite ("card").

With --repo <OutlawsUE>/Content/HeldGuns it also copies the shippable part there, <NAME>.json and <NAME>_recipe.json
(our own points and layout), and fails if that folder holds anything but JSON.

Checks (any failure exits non-zero and nothing is left half written):
  - the sprite rows of every shipped PNG are blank, and no other PNG is in the folder
  - the recipe rebuilt here from sprites_hd/<NAME>.png (this PC's export of the same SPX frame) equals the dev
    texture exactly, every texel

  python make_held_ship.py [--out <dir>] [--repo <OutlawsUE>/Content/HeldGuns]
"""
import argparse, json, os, shutil, sys
import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proto                                   # noqa: E402  (HELD: the guns' grip and muzzle in sprite pixels)

HELD_DIR = proto.HELD_OUT
SPRITES_HD = os.path.join(HERE, "sprites_hd")
OUT = os.path.join(proto.UE_OUT, "held_ship")
GUNS = ["GPISTOL", "GRIFLE", "GSGUN", "GDBSGUN", "GSAWGUN"]
PARTS = ["cyl", "hammer", "trigger", "lever"]
HD_SCALE = 4                                   # the remaster's SPX cells are 4x the NWX cell
SWATCH = 4                                     # write_held's swatch size


def fail(msg):
    sys.exit("make_held_ship: FAIL: " + msg)


def swatch_top(a):
    """the first row of write_held's swatch band (the texture's height when it has none): the bottom rows, a
    multiple of 4 high, whose texels are transparent black or opaque 4x4 squares on the 4 px grid"""
    H, W = a.shape[:2]
    best = H
    for top in range(H - SWATCH, -1, -SWATCH):
        band = a[top:top + SWATCH]
        ok = True
        for x0 in range(0, W - W % SWATCH, SWATCH):
            blk = band[:, x0:x0 + SWATCH]
            if (blk[:, :, 3] == 255).all():
                continue
            if not (blk == 0).all():
                ok = False
                break
        if W % SWATCH and not (band[:, W - W % SWATCH:] == 0).all():
            ok = False
        if not ok:
            break
        best = top
    return best


def finish(tpl, sprite, sh, dilate_rows):
    """the runtime's recipe in numpy: the sprite into rows 0..sh-1, then the nearest-opaque fill over the
    rows above the swatches"""
    a = tpl.copy()
    a[:sh, :sprite.shape[1]] = sprite
    top = a[:dilate_rows]
    opaque = top[:, :, 3] > 127
    if (~opaque).any():
        _, (iy, ix) = ndimage.distance_transform_edt(~opaque, return_indices=True)
        top[:, :, :3] = top[iy, ix, :3]
    return a


def card_of(name, points, sw, sh):
    """the flat card's corners in the hand frame (cm, +X to the muzzle, +Z up, grip at the origin): the sprite's
    scale from the grip and the muzzle (proto.HELD, HD px) against the muzzle in <NAME>.json"""
    spec = proto.HELD[name]
    sg = spec["dir"]
    gx, gy = spec["grip"]
    mx, my = spec["muzzle"]
    s = points["muzzle"][0] / (sg * (mx - gx))
    # the same scale must put the muzzle's height and the front sight where the json has them
    for k in ("muzzle", "sight_front", "sight_rear"):
        px, py = spec[k]
        want = (sg * s * (px - gx), s * (gy - py))
        got = (points[k][0], points[k][2])
        if abs(want[0] - got[0]) > 0.02 or abs(want[1] - got[1]) > 0.02:
            fail(f"{name}: the card scale {s:.4f} puts {k} at {want}, the json has {got}")
    x_left, x_right = sg * s * (0 - gx), sg * s * (sw - gx)
    return dict(cm_per_hd_px=round(s, 6),
                # the sprite's left and right edges along the bore (the left edge is the muzzle end when the
                # barrel points left in the sprite), its top and bottom edges
                x_left=round(x_left, 4), x_right=round(x_right, 4),
                z_top=round(s * gy, 4), z_bottom=round(s * (gy - sh), 4))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--repo", help="the repo's Content/HeldGuns: the points and texture recipes only")
    args = ap.parse_args()
    stage = args.out + ".tmp"
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)
    total = 0
    for name in GUNS:
        dev = np.asarray(Image.open(os.path.join(HELD_DIR, name + ".png")).convert("RGBA"))
        sprite = np.asarray(Image.open(os.path.join(SPRITES_HD, name + ".png")).convert("RGBA"))
        H, W = dev.shape[:2]
        sh, sw = sprite.shape[:2]
        if sw > W or sh > H:
            fail(f"{name}: sprite {sw}x{sh} larger than the texture {W}x{H}")
        # the texture's top rows are the sprite as exported (opaque texels identical, alpha identical)
        top = dev[:sh, :sw]
        if not (top[:, :, 3] == sprite[:, :, 3]).all():
            fail(f"{name}: the texture's alpha in rows 0..{sh - 1} is not the HD sprite's")
        op = sprite[:, :, 3] > 127
        if (top[op][:, :3] != sprite[op][:, :3]).any():
            fail(f"{name}: the texture's opaque texels in rows 0..{sh - 1} are not the HD sprite's")
        if sw != W and (dev[:sh, sw:, 3] > 127).any():
            fail(f"{name}: opaque texels beside the sprite")
        dilate_rows = swatch_top(dev)
        # the template: sprite rows blank, the fill's colours gone (they come from the sprite)
        tpl = dev.copy()
        tpl[:sh] = 0
        low = tpl[:, :, 3] <= 127
        tpl[low, :3] = 0
        rebuilt = finish(tpl, sprite, sh, dilate_rows)
        bad = int((rebuilt != dev).any(2).sum())
        if bad:
            fail(f"{name}: the recipe rebuilds the texture with {bad} texels unlike the dev PNG (dilate_rows {dilate_rows})")
        if (tpl[:sh] != 0).any():
            fail(f"{name}: sprite rows not blank in the template")
        Image.fromarray(tpl, "RGBA").save(os.path.join(stage, name + "_template.png"))
        # the meshes and points, unchanged
        files = [name + ".obj", name + ".mtl", name + ".json"]
        files += [f"{name}_{p}.obj" for p in PARTS if os.path.exists(os.path.join(HELD_DIR, f"{name}_{p}.obj"))]
        for f in files:
            shutil.copyfile(os.path.join(HELD_DIR, f), os.path.join(stage, f))
        mtl = open(os.path.join(HELD_DIR, name + ".mtl")).read().split()
        tex_key = mtl[mtl.index("map_Kd") + 1] if "map_Kd" in mtl else name + ".png"
        points = json.load(open(os.path.join(HELD_DIR, name + ".json")))
        recipe = dict(
            name=name,
            texture=tex_key,                          # the name the .mtl gives it (no such file is shipped)
            size=[W, H],
            template=name + "_template.png",
            sprite=dict(nwx=name + ".NWX", spx=name + ".SPX", frame=0, hd_scale=HD_SCALE,
                        size=[sw, sh], at=[0, 0], rows=[0, sh]),
            fill=dict(rows=[0, dilate_rows], opaque_alpha_over=127,
                      rule="RGB of every texel at or under the alpha from its nearest texel over it (Euclidean), alpha kept"),
            card=card_of(name, points, sw, sh),
            parts=[f[len(name) + 1:-4] for f in files if f.startswith(name + "_")],
        )
        json.dump(recipe, open(os.path.join(stage, name + "_recipe.json"), "w"), indent=1)
        print(f"{name}: texture {W}x{H}, sprite {sw}x{sh} at rows 0..{sh - 1}, fill rows 0..{dilate_rows - 1}, "
              f"parts {recipe['parts']}, card {recipe['card']}")
    # the folder as shipped: nothing but templates among its images, every one blank in its sprite rows
    for f in sorted(os.listdir(stage)):
        p = os.path.join(stage, f)
        total += os.path.getsize(p)
        if f.lower().endswith((".png", ".bmp", ".tga", ".jpg", ".jpeg", ".dds")):
            if not f.endswith("_template.png"):
                fail(f"image {f} is not a template")
            r = json.load(open(os.path.join(stage, f[:-len("_template.png")] + "_recipe.json")))
            a = np.asarray(Image.open(p).convert("RGBA"))
            y0, y1 = r["sprite"]["rows"]
            if (a[y0:y1] != 0).any():
                fail(f"{f}: non-blank pixels in the sprite rows {y0}..{y1 - 1}")
    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.replace(stage, args.out)
    print(f"wrote {len(os.listdir(args.out))} files, {total} bytes, to {args.out}")
    if args.repo:
        for name in GUNS:
            for f in (name + ".json", name + "_recipe.json"):
                shutil.copyfile(os.path.join(args.out, f), os.path.join(args.repo, f))
        # the folder as shipped: our JSON only (the recipes, the points), no meshes or images
        extra = [f for f in os.listdir(args.repo) if not f.endswith(".json")]
        if extra:
            fail(f"{args.repo} holds more than JSON: {extra}")
        print(f"copied the points and recipes to {args.repo}")


if __name__ == "__main__":
    main()
