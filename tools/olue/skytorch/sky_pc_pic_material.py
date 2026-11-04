"""REQ-SKYPCBLUE (user 4 Nov ~21:4x: "yes scale those rows too"): M_OLR_SkySurface changed IN PLACE: a new Custom node OLSkyPcPic
sits between SkyByDirection and the feature switch's Default pin, the PC's pin.  It takes the picture toward its own grey
(PcPicSat, a scalar) and scales it (PcPicScale, a colour), so a sky can land on its own colours under the PC's exposure and
gamut expansion; 1 and 1 (the defaults, every sky but armad) leave it as it came.  The switch's other pins are not touched: the
headset's OLQuestLook still takes SkyByDirection directly, so its shader is as it was.  SkyByDirection's own code is not touched
either, so M_OLR_WorldSkyThru's copy of it stays as it is (armad has no sky-through walls).  Each link is checked.

  UnrealEditor-Cmd.exe OutlawsUE.uproject -ExecutePythonScript="sky_pc_pic_material.py" -unattended -nullrhi
"""
import traceback
import unreal

PATH = "/Game/Outlaws/Runtime/M_OLR_SkySurface"
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary

CODE = """// (REQ-SKYPCBLUE, user 4 Nov ~21:4x: "yes scale those rows too") the PC's pin alone: see AOutlawsLevelActor::Build
float L = dot(Col, float3(0.2126, 0.7152, 0.0722));
return PcPicScale.rgb * lerp(float3(L, L, L), Col, PcPicSat);"""


def main():
    mat = unreal.load_asset(PATH)
    fls = MEL.get_material_property_input_node(mat, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if not isinstance(fls, unreal.MaterialExpressionFeatureLevelSwitch):
        raise RuntimeError("emissive is not the feature switch: %s" % type(fls).__name__)
    before = list(MEL.get_inputs_for_material_expression(mat, fls) or [])
    unreal.log("OLSKYPCPIC switch before: %s" % ", ".join(str(s.get_name()) if s else "-" for s in before))
    sky = before[0]
    if isinstance(sky, unreal.MaterialExpressionCustom) and str(sky.get_editor_property("description")) == "OLSkyPcPic":
        unreal.log("OLSKYPCPIC already there: nothing to do")
        return
    if not isinstance(sky, unreal.MaterialExpressionCustom) or str(sky.get_editor_property("description")) != "SkyByDirection":
        raise RuntimeError("the switch's Default is not SkyByDirection")
    x = int(sky.get_editor_property("material_expression_editor_x")) + 320
    y = int(sky.get_editor_property("material_expression_editor_y")) + 260
    scale = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x - 300, y + 60)
    scale.set_editor_property("parameter_name", "PcPicScale")
    scale.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    sat = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x - 300, y + 260)
    sat.set_editor_property("parameter_name", "PcPicSat")
    sat.set_editor_property("default_value", 1.0)
    node = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", "OLSkyPcPic")
    node.set_editor_property("output_type", sky.get_editor_property("output_type"))
    node.set_editor_property("code", CODE)
    inputs = []
    for n in ("Col", "PcPicScale", "PcPicSat"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", n)
        inputs.append(ci)
    node.set_editor_property("inputs", inputs)
    for s, pin in ((sky, "Col"), (scale, "PcPicScale"), (sat, "PcPicSat")):
        if not MEL.connect_material_expressions(s, "", node, pin):
            raise RuntimeError("no link to %r" % pin)
    if not MEL.connect_material_expressions(node, "", fls, "Default"):
        raise RuntimeError("no link to the switch's Default")
    after = list(MEL.get_inputs_for_material_expression(mat, fls) or [])
    want = [node.get_name()] + [s.get_name() if s else None for s in before[1:]]
    got = [s.get_name() if s else None for s in after]
    if got != want:
        raise RuntimeError("the switch after the edit %s, wanted %s" % (got, want))
    MEL.recompile_material(mat)
    EAL.save_asset(PATH)
    unreal.log("OLSKYPCPIC done: the switch %s" % ", ".join(str(g) for g in got))


try:
    main()
    unreal.log("OLSKYPCPIC finished")
except Exception:
    unreal.log_error("OLSKYPCPIC FAILED\n" + traceback.format_exc())
    raise
