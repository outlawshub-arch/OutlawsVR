"""BUG-ES31MATS (9 Nov): M_OLR_Underwater, M_OLR_Backdrop and M_OLR_InkOutline did not compile for the mobile renderer
(the Quest's SF_VULKAN_ES31_ANDROID and the PC's ES3_1 preview): their Custom nodes call SceneTextureLookup(UV, Id, bFiltered),
which the engine's MaterialTemplate.ush has only on the deferred (desktop) path; the mobile path has
MobileSceneTextureLookup(Parameters, Id, UV).  The cook then put the Default Material in their place.

This edits the three assets in place (no rebuild, so every later script's change on them stays): in each Custom node whose
code calls SceneTextureLookup, the code becomes

    #if SHADING_PATH_MOBILE
    <the same code, each SceneTextureLookup(UV, Id, F) as MobileSceneTextureLookup(Parameters, Id, UV)>
    #else
    <the old code, unchanged>
    #endif

so the desktop shader is the old one.  On the mobile path the custom stencil is read at the pixel itself, whatever UV is asked
(the engine's own MobileSceneTextureLookup); these are post process passes and the project has r.MobileHDR=False, so the
headset does not draw them at all: this is for a clean cook (no Default Material) and a preview that compiles.

  UnrealEditor-Cmd.exe OutlawsUE.uproject -ExecutePythonScript="<this file>" -unattended -nosplash -nosound -nullrhi
  (add " inspect" inside the quotes after the path to log the nodes and change nothing)
"""
import sys
import traceback

import unreal

RT = "/Game/Outlaws/Runtime"
PATHS = [RT + "/M_OLR_Underwater", RT + "/M_OLR_Backdrop", RT + "/M_OLR_InkOutline"]
EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
MARK = "#if SHADING_PATH_MOBILE // (BUG-ES31MATS)"
CALL = "SceneTextureLookup("
PROPS = [unreal.MaterialProperty.MP_EMISSIVE_COLOR, unreal.MaterialProperty.MP_OPACITY, unreal.MaterialProperty.MP_OPACITY_MASK,
         unreal.MaterialProperty.MP_BASE_COLOR]


def calls(code):
    """The places of bare SceneTextureLookup( calls (not MobileSceneTextureLookup)."""
    out, i = [], code.find(CALL)
    while i >= 0:
        if i == 0 or not (code[i - 1].isalnum() or code[i - 1] == "_"):
            out.append(i)
        i = code.find(CALL, i + 1)
    return out


def args_at(code, open_at):
    """The arguments of the call whose "(" is at open_at, split at top level commas, and the index after its ")"."""
    depth, start, parts, i = 0, open_at + 1, [], open_at
    while i < len(code):
        c = code[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
            if depth == 0:
                parts.append(code[start:i].strip())
                return parts, i + 1
        elif c == "," and depth == 1:
            parts.append(code[start:i].strip())
            start = i + 1
        i += 1
    raise RuntimeError("unclosed call at %d" % open_at)


def mobile(code):
    out, pos, n = [], 0, 0
    for at in calls(code):
        if at < pos:
            continue
        a, end = args_at(code, at + len(CALL) - 1)
        if len(a) != 3:
            raise RuntimeError("SceneTextureLookup with %d arguments: %r" % (len(a), code[at:end]))
        out.append(code[pos:at])
        out.append("MobileSceneTextureLookup(Parameters, %s, %s)" % (a[1], a[0]))
        pos, n = end, n + 1
    out.append(code[pos:])
    return "".join(out), n


def nodes(mat):
    seen, todo, found = set(), [], []
    for p in PROPS:
        todo.append(MEL.get_material_property_input_node(mat, p))
    while todo:
        e = todo.pop()
        if e is None or e.get_name() in seen:
            continue
        seen.add(e.get_name())
        if isinstance(e, unreal.MaterialExpressionCustom):
            found.append(e)
        todo.extend(MEL.get_inputs_for_material_expression(mat, e) or [])
    return found


def fix(path, inspect):
    mat = unreal.load_asset(path)
    if mat is None:
        raise RuntimeError("no " + path)
    changed = 0
    for e in nodes(mat):
        code = str(e.get_editor_property("code"))
        desc = str(e.get_editor_property("description"))
        if MARK in code:
            unreal.log("OLES31MATS %s [%s]: already fixed" % (path, desc))
            continue
        n = len(calls(code))
        unreal.log("OLES31MATS %s [%s]: %d SceneTextureLookup calls" % (path, desc, n))
        if n == 0 or inspect:
            continue
        mob, m = mobile(code)
        if m != n or calls(mob):
            raise RuntimeError("%s [%s]: %d of %d calls rewritten" % (path, desc, m, n))
        body = code if code.endswith("\n") else code + "\n"
        mob = mob if mob.endswith("\n") else mob + "\n"
        e.set_editor_property("code", MARK + "\n" + mob + "#else\n" + body + "#endif\n")
        changed += 1
    if changed and not inspect:
        MEL.recompile_material(mat)
        EAL.save_asset(path)
    unreal.log("OLES31MATS %s: %d nodes changed" % (path, changed))


def main():
    inspect = "inspect" in [a.lower() for a in sys.argv[1:]]
    for p in PATHS:
        fix(p, inspect)
    unreal.log("OLES31MATS done" + (" (inspect)" if inspect else ""))


try:
    main()
except Exception:
    unreal.log_error("OLES31MATS FAILED\n" + traceback.format_exc())
    raise
