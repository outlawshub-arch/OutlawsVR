"""REQ-SKYTHRU (user 2026-11-03): M_OLR_WorldSkyThru, a copy of M_OLR_World for the outer walls whose index-0 texels show the sky.
MaskTex (the map's own 8-bit texture, alpha 0 on index 0, whatever HD art Tex holds) is sampled at the wall's UVs; a new Custom
node OLSkyThru takes the wall's colour (the OLWaterTint output) and the mask, returns the wall where the mask is solid and
otherwise runs M_OLR_SkySurface's own SkyByDirection code on a copy of its parameters (SkyTex for its Tex), so the same picture,
projection and scroll.  Its output feeds what OLWaterTint fed: the feature switch's Default and OLQuestLook's Col, so the sky
part takes the headset look as the sky surfaces do.  Opaque, no extra geometry.  M_OLR_World itself is not touched.

Run again after the sky's code changes: the copy's code is taken again from M_OLR_SkySurface, nothing else moves.

  UnrealEditor-Cmd.exe OutlawsUE.uproject -ExecutePythonScript="sky_thru_material.py" -unattended -nullrhi
"""
import traceback
import unreal

SRC = "/Game/Outlaws/Runtime/M_OLR_World"
DST = "/Game/Outlaws/Runtime/M_OLR_WorldSkyThru"
SKY = "/Game/Outlaws/Runtime/M_OLR_SkySurface"
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

GATE = "[branch] if (MaskA >= 0.5) return Wall;      // (REQ-SKYTHRU) the wall's solid texels; the rest is the sky surfaces' code\n"


def walk(mat):
    seen, todo, out = set(), [], []
    e = MEL.get_material_property_input_node(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if e:
        todo.append(e)
    while todo:
        e = todo.pop()
        if e is None or e.get_name() in seen:
            continue
        seen.add(e.get_name())
        out.append(e)
        todo += [x for x in (MEL.get_inputs_for_material_expression(mat, e) or []) if x]
    return out


def custom(mat, desc):
    hit = [e for e in walk(mat) if isinstance(e, unreal.MaterialExpressionCustom) and str(e.get_editor_property("description")) == desc]
    if len(hit) != 1:
        raise RuntimeError("%d %s Custom nodes (expected 1)" % (len(hit), desc))
    return hit[0]


def copy_source(mat, s, x, y):
    """A node in mat doing what the sky's input s does: the same parameter under the same name (the texture as SkyTex)."""
    if isinstance(s, unreal.MaterialExpressionTextureObjectParameter):
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureObjectParameter, x, y)
        n.set_editor_property("parameter_name", "SkyTex")
        n.set_editor_property("texture", s.get_editor_property("texture"))
        return n
    if isinstance(s, unreal.MaterialExpressionScalarParameter):
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
        n.set_editor_property("parameter_name", s.get_editor_property("parameter_name"))
        n.set_editor_property("default_value", s.get_editor_property("default_value"))
        return n
    if isinstance(s, unreal.MaterialExpressionVectorParameter):
        n = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
        n.set_editor_property("parameter_name", s.get_editor_property("parameter_name"))
        n.set_editor_property("default_value", s.get_editor_property("default_value"))
        return n
    if isinstance(s, unreal.MaterialExpressionCameraVectorWS):
        return MEL.create_material_expression(mat, unreal.MaterialExpressionCameraVectorWS, x, y)
    if isinstance(s, unreal.MaterialExpressionTime):
        return MEL.create_material_expression(mat, unreal.MaterialExpressionTime, x, y)
    raise RuntimeError("a sky input of an unexpected kind: %s" % type(s).__name__)


def main():
    sky_mat = unreal.load_asset(SKY)
    sky = custom(sky_mat, "SkyByDirection")
    sky_code = str(sky.get_editor_property("code"))
    if EAL.does_asset_exist(DST):
        mat = unreal.load_asset(DST)
        node = custom(mat, "OLSkyThru")
        node.set_editor_property("code", GATE + sky_code)
        MEL.recompile_material(mat)
        EAL.save_asset(DST)
        unreal.log("OLSKYTHRU the code taken again from the sky (%d chars)" % len(sky_code))
        return
    if not EAL.duplicate_asset(SRC, DST):
        raise RuntimeError("no copy of M_OLR_World made")
    mat = unreal.load_asset(DST)
    tint = custom(mat, "OLWaterTint")
    look = custom(mat, "OLQuestLook")
    swirl = custom(mat, "OLSwirl")
    stoch = custom(mat, "OLStochastic")
    fls = MEL.get_material_property_input_node(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if not isinstance(fls, unreal.MaterialExpressionFeatureLevelSwitch):
        raise RuntimeError("emissive is not the feature switch: %s" % type(fls).__name__)
    tex = MEL.get_inputs_for_material_expression(mat, stoch)[0]
    if MEL.get_inputs_for_material_expression(mat, look)[0] != tint or MEL.get_inputs_for_material_expression(mat, fls)[0] != tint:
        raise RuntimeError("OLWaterTint does not feed the switch and the look as expected")
    x, y = -1800, 1600
    mask = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    mask.set_editor_property("parameter_name", "MaskTex")
    mask.set_editor_property("texture", tex.get_editor_property("texture"))
    if not MEL.connect_material_expressions(swirl, "", mask, "UVs"):
        raise RuntimeError("no link to MaskTex's UVs")
    names = [str(i.get_editor_property("input_name")) for i in sky.get_editor_property("inputs")]
    srcs = list(MEL.get_inputs_for_material_expression(sky_mat, sky) or [])
    if len(names) != len(srcs) or any(s is None for s in srcs):
        raise RuntimeError("a sky input without a link")
    unreal.log("OLSKYTHRU sky inputs: %s" % ", ".join("%s<-%s" % (n, type(s).__name__) for n, s in zip(names, srcs)))
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, x + 500, y)
    node.set_editor_property("description", "OLSkyThru")
    node.set_editor_property("output_type", sky.get_editor_property("output_type"))
    node.set_editor_property("code", GATE + sky_code)
    inputs = []
    for n in ["Wall", "MaskA"] + names:
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    node.set_editor_property("inputs", inputs)
    if not MEL.connect_material_expressions(tint, "", node, "Wall") or not MEL.connect_material_expressions(mask, "A", node, "MaskA"):
        raise RuntimeError("no link to OLSkyThru's Wall or MaskA")
    for i, (n, s) in enumerate(zip(names, srcs)):
        c = copy_source(mat, s, x - 400, y + 75 * i)
        if not MEL.connect_material_expressions(c, "", node, n):
            raise RuntimeError("no link to %r" % n)
    if not MEL.connect_material_expressions(node, "", look, "Col") or not MEL.connect_material_expressions(node, "", fls, "Default"):
        raise RuntimeError("no link from OLSkyThru to the look or the switch")
    if MEL.get_inputs_for_material_expression(mat, look)[0] != node or MEL.get_inputs_for_material_expression(mat, fls)[0] != node:
        raise RuntimeError("the look or the switch not on OLSkyThru after the edit")
    MEL.recompile_material(mat)
    EAL.save_asset(DST)
    unreal.log("OLSKYTHRU done: %d inputs" % len(inputs))


try:
    main()
    unreal.log("OLSKYTHRU finished")
except Exception:
    unreal.log_error("OLSKYTHRU FAILED\n" + traceback.format_exc())
    raise
