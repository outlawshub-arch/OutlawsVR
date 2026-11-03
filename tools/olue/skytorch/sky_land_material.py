"""REQ-SKYDOME (user 2026-11-03): M_OLR_SkySurface changed IN PLACE: its SkyByDirection Custom node takes two more inputs,
LandV (the land's foot, 0..1 down the picture; 0 = off, every sky as before) and LandDrop (how far under the horizon the foot
sits, quarter turns), from two new scalar parameters, and its code the branch that uses them.  Every other input is linked
again by name to the expression it had (setting the inputs array drops the links), each link checked; nothing else in the
graph is touched (ue_olue_quest_look.py's additions stay).  The same edit belongs in
tools/olue/ue_olue_sky_surface_material.py's CODE for a rebuild from scratch.

  UnrealEditor-Cmd.exe OutlawsUE.uproject -ExecutePythonScript="sky_land_material.py" -unattended -nullrhi
"""
import traceback
import unreal

PATH = "/Game/Outlaws/Runtime/M_OLR_SkySurface"
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

OLD = """float v = Wrap > 0.5 ? frac(1.0 - t) : 1.0 - min(t, 0.998);
float3 C = Texture2DSampleLevel(Tex, TexSampler, float2(u, v), 0).rgb;
float f = Wrap > 0.5 ? 0.0 : saturate((t - 1.0) * 4.0);         // past the top row: its colour takes over
C = lerp(C, FadeColour.rgb, f);"""
NEW = """float v = Wrap > 0.5 ? frac(1.0 - t) : 1.0 - min(t, 0.998);
float f = Wrap > 0.5 ? 0.0 : saturate((t - 1.0) * 4.0);         // past the top row: its colour takes over
// (REQ-SKYDOME, user 2026-11-03) a picture with its land part way up it (LandV: the land's foot, 0..1 down the picture): the
// foot LandDrop under the horizon (quarter turns), the rows under it below the horizon; over its top sixteenth its top colour
// comes in, alone from its top row up (no row drawn out up the dome)
if (LandV > 0.0)
{
    float tl = RepV * (asin(clamp(D.z, -1.0, 1.0)) / 1.5707963 + LandDrop);
    v = clamp(LandV - tl, 0.002, 0.998);
    f = saturate((tl - LandV) * 16.0 + 1.0);
}
float3 C = Texture2DSampleLevel(Tex, TexSampler, float2(u, v), 0).rgb;
C = lerp(C, FadeColour.rgb, f);"""


def find_custom(mat):
    seen, todo, hit = set(), [], []
    e = MEL.get_material_property_input_node(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if e:
        todo.append(e)
    while todo:
        e = todo.pop()
        if e is None or e.get_name() in seen:
            continue
        seen.add(e.get_name())
        if isinstance(e, unreal.MaterialExpressionCustom) and "CloudOpacity" in str(e.get_editor_property("code")):
            hit.append(e)
        todo += [x for x in (MEL.get_inputs_for_material_expression(mat, e) or []) if x]
    if len(hit) != 1:
        raise RuntimeError("%d sky Custom nodes found (expected 1)" % len(hit))
    return hit[0]


def main():
    mat = unreal.load_asset(PATH)
    custom = find_custom(mat)
    code = str(custom.get_editor_property("code"))
    names = [str(i.get_editor_property("input_name")) for i in custom.get_editor_property("inputs")]
    srcs = list(MEL.get_inputs_for_material_expression(mat, custom) or [])
    unreal.log("OLSKYLAND inputs before: %s" % ", ".join("%s<-%s" % (n, s.get_name() if s else None) for n, s in zip(names, srcs)))
    if "LandV" in names:
        # (the first edit's fade, a quarter picture over the top row, drew the top row out up the dome: the code alone again)
        old_fade = "f = saturate((tl - LandV) * 4.0);"
        if old_fade in code:
            i0, i1 = code.index("// (REQ-SKYDOME"), code.index("float3 C = Texture2DSampleLevel")
            new_block = NEW[NEW.index("// (REQ-SKYDOME"):NEW.index("float3 C = Texture2DSampleLevel")]
            custom.set_editor_property("code", code[:i0] + new_block + code[i1:])
            MEL.recompile_material(mat)
            EAL.save_asset(PATH)
            unreal.log("OLSKYLAND fade updated in place (LandV inputs kept)")
        else:
            unreal.log("OLSKYLAND already has LandV and the fade: nothing to do")
        return
    if code.count(OLD) != 1:
        raise RuntimeError("the code's sampling lines are not as expected")
    if len(srcs) != len(names) or any(s is None for s in srcs):
        raise RuntimeError("an input without a link before the edit")
    y = max(int(s.get_editor_property("material_expression_editor_y")) for s in srcs) + 75
    land = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, y)
    land.set_editor_property("parameter_name", "LandV")
    land.set_editor_property("default_value", 0.0)
    drop = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -700, y + 75)
    drop.set_editor_property("parameter_name", "LandDrop")
    drop.set_editor_property("default_value", 0.0)
    inputs = []
    for n in names + ["LandV", "LandDrop"]:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    custom.set_editor_property("inputs", inputs)
    custom.set_editor_property("code", code.replace(OLD, NEW))
    for n, s in list(zip(names, srcs)) + [("LandV", land), ("LandDrop", drop)]:
        if not MEL.connect_material_expressions(s, "", custom, n):
            raise RuntimeError("no link to %r" % n)
    after = list(MEL.get_inputs_for_material_expression(mat, custom) or [])
    got = [a.get_name() if a else None for a in after]
    want = [s.get_name() for s in srcs] + [land.get_name(), drop.get_name()]
    if got != want:
        raise RuntimeError("links after the edit %s, wanted %s" % (got, want))
    MEL.recompile_material(mat)
    EAL.save_asset(PATH)
    unreal.log("OLSKYLAND done: %d inputs, every link as before plus LandV and LandDrop" % len(after))


try:
    main()
except Exception:
    unreal.log_error("OLSKYLAND FAILED\n" + traceback.format_exc())
    raise
