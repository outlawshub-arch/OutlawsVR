#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;
class UMaterialInterface;

/**
 * The ink line round held gear and world items on the headset.  The PC draws it in the post process
 * (M_OLR_InkOutline over custom stencils 1 and 4); the Quest's mobile renderer has no post process, so there it is a
 * shell, as the hands have (UOutlawsBoxHand): every section again, pushed out along its normals (averaged over the
 * vertices at one place, so hard edges do not open), wound the other way, in near-black on a one-sided material:
 * only the shell's far side shows, round the thing's edge.
 */
namespace OutlawsInk
{
	/** The mobile renderer (the headset): no post process, so shells instead. */
	OUTLAWSRUNTIME_API bool Wanted();
	/** Shell M's sections (again, if it was shelled before: the old shell goes). Thickness in cm, world. */
	OUTLAWSRUNTIME_API void AddShell(UProceduralMeshComponent* M, float Thickness, UMaterialInterface* Ink);
	/** A mesh cleared and built again: its shells forgotten, so the next AddShell counts all its sections as its own. */
	OUTLAWSRUNTIME_API void Forget(UProceduralMeshComponent* M);
	/** M's sections copied from one already shelled (its first Own its own, the rest the shell): known as shelled. */
	OUTLAWSRUNTIME_API void NoteShelled(UProceduralMeshComponent* M, int32 Own);
	/** The same shell as a child component of Src (its own sections only, not its ink shell), in Mat: the grab glow's
	 *  rim round a 3D thing (user 2026-09-30).  Shown and hidden by the caller. */
	OUTLAWSRUNTIME_API UProceduralMeshComponent* MakeHalo(UProceduralMeshComponent* Src, float Thickness, UMaterialInterface* Mat);
}
