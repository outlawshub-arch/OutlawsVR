#pragma once

#include "CoreMinimal.h"

/** (BUG-FOVCLIP) what the eyes can see, for our own "is it in view" tests: the union of both eyes' frusta as the headset
 *  reports them (asymmetric, the Quest's reaching much further down than up), plus a margin; a generous fixed cone when
 *  no headset answers.  Half-angles in degrees, measured from the camera's forward. */
struct FOutlawsViewCone
{
	float Left = 60.f, Right = 60.f, Up = 60.f, Down = 60.f;
	bool bFromHmd = false;

	/** This frame's cone (cheap: two projection matrices).  Logs an OLRT line whenever its source changes, or always
	 *  with bLog. */
	static FOutlawsViewCone Get(bool bLog = false);

	/** Whether a world point lies inside the cone of a camera at Eye looking along Rot. */
	bool Contains(const FVector& Eye, const FQuat& Rot, const FVector& Point) const;

	/** Bounds for a small part that rides on a hand or the wrist (thin planes a few cm across): widened, so the
	 *  frustum and occlusion tests never drop it while it is on screen.  Cheap: a handful of components. */
	static void WidenHandBounds(class UPrimitiveComponent* C, float Scale = 8.f);
	static int32 WidenedCount;

	/** One OLRT line at level start: the near plane against the depths the held gear and text plates are pulled to,
	 *  and the cone the eyes see. */
	static void LogClipDiagnostics();
};
