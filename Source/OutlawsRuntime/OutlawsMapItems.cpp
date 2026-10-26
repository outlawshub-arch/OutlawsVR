// REQ-MAPITEMS (user 16 Oct; 20 Oct: "build it with your own placement, I'll adjust after"): the keys, stones, gears,
// crowbars, shovels and the other quest and utility items go on the map (the wheel's and the held one) once the player has
// seen them: within 5 m of the eyes, inside 45 degrees of where they look, nothing in the way.  Each is drawn as the
// game's own inventory sprite for it (ibrsskey.nwx, HD when Remastered is on; else its ground sprite, else the wheel's
// card) with a dark rim, its longer side 3.5 page cm (about 1.9 cm on the wheel's resting page, a little over the arrow),
// at its place on the page, over the walls and under the arrow and the others' marks; two close together are eased
// apart.  Taken, it leaves the map (it is in the inventory then); dropped again it shows once seen again.  A lock item
// seen loses its hint blob to its picture.  Per level, kept in saves ("map_seen" on the pickup; an older save has none
// seen); in co-op each machine keeps its own player's (a client taking the host's world keeps none of the host's).
// Test: -OLShots -OLLevel=hideout -OLNoFrontEnd -OLMapItemsTest[=vr] (the seen rule, save and load, pickup; the shots).
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "CanvasItem.h"
#include "GameFramework/PlayerController.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"

#if WITH_OUTLAWS_TESTS
bool AOutlawsLevelActor::bDebugMapItemsOff = false;
#endif

bool AOutlawsLevelActor::IsMapItem(const FPickup& P) const
{
	// the wheel's own item row (an inventory ITM, no weapon, ammunition or counted stock) or one a lock asks for
	if (P.bInstant || P.Entries.Num() == 0) return false;
	if (IsListedItem(P.Item)) return true;
	if (P.UseId < 0 || !Inf) return false;
	TSet<int32> Ids;
	Inf->RequiredObjects(Ids);
	return Ids.Contains(P.UseId);
}

void AOutlawsLevelActor::UpdateMapSeen(const FVector& Eye, const FVector& Forward, float Dt)
{
	MapSeenWait -= Dt;
	if (MapSeenWait > 0.f) return;
	MapSeenWait = 0.1f;
	const double ConeCos = FMath::Cos(FMath::DegreesToRadians(MapSeenConeDeg));
	const FVector Fw = Forward.GetSafeNormal();
	for (FPickup& P : Pickups)
	{
		if (P.bMapSeen || P.bHeld) continue;
		const AActor* A = P.Actor.Get();
		if (!A) continue;
		const FVector Base = A->GetActorLocation();
		// the item's middle a little over its foot point, and a higher point (a tall one seen over a low sill)
		const FVector Low = Base + FVector(0.0, 0.0, 15.0), High = Base + FVector(0.0, 0.0, 35.0);
		const FVector To = Low - Eye;
		const double D = To.Size();
		if (D > MapSeenCm || D < 1.0) continue;
		if (FVector::DotProduct(To / D, Fw) < ConeCos) continue;
		if (!IsMapItem(P)) continue;
		if (!CanSee(Eye, Low, A) && !CanSee(Eye, High, A)) continue;
		P.bMapSeen = true;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT map item seen: %s (%s) %.0f cm off, %.0f degrees from the view's middle; on the map"), *P.Item, *P.Ground, D,
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(To / D, Fw), -1.0, 1.0))));
	}
}

void AOutlawsLevelActor::MapItemMarks(TArray<FMapItemMark>& Out) const
{
	Out.Reset();
	for (const FPickup& P : Pickups)
	{
		const AActor* A = P.Actor.Get();
		if (!A || !P.bMapSeen || P.bHeld) continue;
		FMapItemMark M;
		M.Where = A->GetActorLocation();
		M.Item = P.Item;
		if (const AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(A); S && S->SpriteTexture)
		{
			const int32 Cols = FMath::Max(1, S->AtlasCols), Rows = FMath::Max(1, S->AtlasRows);
			const int32 Cell = S->Grid.Num() > 0 ? FMath::Clamp(S->Grid[0], 0, Cols * Rows - 1) : 0;
			M.Tex = S->SpriteTexture;
			M.UV0 = FVector2D(double(Cell % Cols) / Cols, double(Cell / Cols) / Rows);
			M.UV1 = M.UV0 + FVector2D(1.0 / Cols, 1.0 / Rows);
			const double W = M.Tex->GetSurfaceWidth() / Cols, H = M.Tex->GetSurfaceHeight() / Rows;
			M.Aspect = H > 0.0 ? float(W / H) : 1.f;
		}
		Out.Add(M);
	}
}

int32 AOutlawsLevelActor::MapMarksSignature() const
{
	TArray<FVector> Spots;
	KeyItemSpots(Spots);
	uint32 H = GetTypeHash(Spots.Num());
	for (const FVector& S : Spots) H = HashCombine(H, GetTypeHash(FIntVector(S / 25.0)));
	TArray<FMapItemMark> Marks;
	MapItemMarks(Marks);
	H = HashCombine(H, GetTypeHash(Marks.Num()));
	for (const FMapItemMark& M : Marks) H = HashCombine(HashCombine(H, GetTypeHash(FIntVector(M.Where / 25.0))), GetTypeHash(M.Tex));
#if WITH_OUTLAWS_TESTS
	H = HashCombine(H, bDebugMapItemsOff ? 1u : 0u);
#endif
	return int32(H & 0x7fffffff);          // (never the -1 of "not drawn yet")
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::DebugMapItems(TArray<FMapItemDebug>& Out) const
{
	Out.Reset();
	for (int32 i = 0; i < Pickups.Num(); ++i)
	{
		const FPickup& P = Pickups[i];
		AActor* A = P.Actor.Get();
		if (!A || P.bHeld || !IsMapItem(P)) continue;
		FMapItemDebug D;
		D.Index = i;
		D.Where = A->GetActorLocation();
		D.Item = P.Item;
		D.bSeen = P.bMapSeen;
		D.Actor = A;
		Out.Add(D);
	}
}

void AOutlawsLevelActor::DebugSetMapSeen(int32 Index, bool bSeen)
{
	if (Pickups.IsValidIndex(Index)) Pickups[Index].bMapSeen = bSeen;
}

void AOutlawsLevelActor::DebugClearMapSeen()
{
	for (FPickup& P : Pickups) P.bMapSeen = false;
}

bool AOutlawsLevelActor::DebugIsMapItemSeen(const AActor* A) const
{
	for (const FPickup& P : Pickups) if (P.Actor.Get() == A) return P.bMapSeen;
	return false;
}
#endif

// ---- the player's page ------------------------------------------------------------------------------------------------

void AOutlawsPlayer::RefreshMapMarks(float Dt)
{
	MapSpotsWait -= Dt;
	if (MapSpotsWait > 0.f) return;
	MapSpotsWait = 0.5f;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !MapCanvas || MapDrawnFor.Get() != L) return;
	if (L->MapMarksSignature() != MapSpotsDrawn) MapCanvas->UpdateResource();
}

void AOutlawsPlayer::DrawMapItems(UCanvas* Canvas, int32 Width, int32 Height, double K)
{
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	if (!L || !Lv || !Canvas) return;
	MapSpotsDrawn = L->MapMarksSignature();
#if WITH_OUTLAWS_TESTS
	if (AOutlawsLevelActor::bDebugMapItemsOff) return;
#endif
	TArray<AOutlawsLevelActor::FMapItemMark> Marks;
	L->MapItemMarks(Marks);
	if (Marks.Num() == 0) return;
	// the picture's longer side 3.5 page cm: on the wheel's 15 cm page (28 page cm long) about 1.9 cm, a little over the
	// arrow's 1.5 (the sprites carry some clear border), so the item reads beside it at rest and plainly when zoomed
	constexpr double IconCm = 3.5;
	const double IconPx = IconCm * K;
	struct FPlaced { FVector2D At; FVector2D Size; float Alpha; UTexture* Tex; FVector2D UV0, UV1; const TCHAR* Source; };
	TArray<FPlaced> Placed;
	for (const AOutlawsLevelActor::FMapItemMark& M : Marks)
	{
		// on the drawing only: a sector it shows, the layer underfoot full, another one fainter (as its walls are)
		const int32 Si = L->CullSectorOf(M.Where + FVector(0.0, 0.0, 10.0));
		const FOutlawsSector* S = Lv->Sector(Si);
		if (!S || !MapSectors.Contains(Si)) continue;
		const FVector2D Q((M.Where.Y - MapCentre.Y) * MapScale, (M.Where.X - MapCentre.X) * MapScale);
		FPlaced P;
		P.At = FVector2D(Width * 0.5 + Q.X * K, Height * 0.5 - Q.Y * K);
		if (P.At.X < 0.0 || P.At.Y < 0.0 || P.At.X > Width || P.At.Y > Height) continue;
		// the game's own inventory sprite for it (ibrsskey.nwx: upright, as the original's inventory drew it; HD with
		// Remastered), else its ground sprite (a key lying flat reads as a thin sliver), else the wheel's card
		double Asp = 1.0;
		FWristIconSrc Inv;
		if (WristIcon(TEXT("nwx:") + M.Item, Inv) && Inv.Tex && Inv.Tex->GetResource())
		{
			P.Tex = Inv.Tex;
			P.UV0 = Inv.Offset;
			P.UV1 = Inv.Offset + Inv.Scale;
			Asp = Inv.Aspect;
		}
		else if (M.Tex)
		{
			P.Tex = M.Tex;
			P.UV0 = M.UV0;
			P.UV1 = M.UV1;
			Asp = M.Aspect;
		}
		else
		{
			P.Tex = ItemCard(M.Item, true);
			P.UV0 = FVector2D::ZeroVector;
			P.UV1 = FVector2D(1.0, 1.0);
			Asp = 152.0 / 216.0;
		}
		if (!P.Tex || !P.Tex->GetResource()) continue;
		P.Source = P.Tex == Inv.Tex ? TEXT("inventory sprite") : P.Tex == M.Tex ? TEXT("ground sprite") : TEXT("wheel card");
		P.Size = Asp >= 1.0 ? FVector2D(IconPx, IconPx / Asp) : FVector2D(IconPx * Asp, IconPx);
		P.Alpha = S->Layer == MapLayer ? 1.f : 0.55f;
		Placed.Add(P);
	}
	// two close together eased apart (each kept on the page), so neither hides the other
	const double Gap = IconPx * 0.85;
	for (int32 Pass = 0; Pass < 6; ++Pass)
	{
		for (int32 a = 0; a < Placed.Num(); ++a)
		{
			for (int32 b = a + 1; b < Placed.Num(); ++b)
			{
				FVector2D D = Placed[b].At - Placed[a].At;
				const double Len = D.Size();
				if (Len >= Gap) continue;
				D = Len < 1e-3 ? FVector2D(1.0, 0.0) : D / Len;
				const FVector2D Push = D * ((Gap - Len) * 0.5);
				Placed[a].At -= Push;
				Placed[b].At += Push;
			}
		}
		for (FPlaced& P : Placed) P.At = FVector2D(FMath::Clamp(P.At.X, IconPx * 0.5, Width - IconPx * 0.5), FMath::Clamp(P.At.Y, IconPx * 0.5, Height - IconPx * 0.5));
	}
	for (const FPlaced& P : Placed)
	{
		// a dark rim round it (its own shape in black, a few pixels out each way), so it stands off the pale page and
		// the walls' lines
		const double Rim = FMath::Max(2.0, IconPx * 0.03);
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector2D Off = FVector2D(FMath::Cos(k * PI / 4.0), FMath::Sin(k * PI / 4.0)) * Rim;
			FCanvasTileItem Edge(P.At - P.Size * 0.5 + Off, P.Tex->GetResource(), P.Size, P.UV0, P.UV1, FLinearColor(0.f, 0.f, 0.f, 0.75f * P.Alpha));
			Edge.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(Edge);
		}
		FCanvasTileItem I(P.At - P.Size * 0.5, P.Tex->GetResource(), P.Size, P.UV0, P.UV1, FLinearColor(1.f, 1.f, 1.f, P.Alpha));
		I.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(I);
	}
	FString Sources;
	for (const FPlaced& P : Placed) Sources += FString::Printf(TEXT("%s%s"), Sources.IsEmpty() ? TEXT("") : TEXT(", "), P.Source);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT map items drawn: %d of %d seen, %.0f px each (%.1f page cm): %s"), Placed.Num(), Marks.Num(), IconPx, IconCm, *Sources);
}

// ---- -OLShots -OLLevel=hideout -OLNoFrontEnd -OLMapItemsTest[=vr] ------------------------------------------------------
// The seen rule (desktop; skipped with =vr): every map item's seen mark cleared; one item picked with three stands round it
// (one 6.5 to 9 m off in plain view, one within 5 m with a wall between, one within 5 m in plain view); from the far one,
// facing it: not seen; behind the wall, facing it: not seen; near, facing away: not seen; near, facing it: seen, on the
// map's marks; turned away and gone far: still seen.  Saved to slot 9, every mark cleared, slot 9 loaded: that item seen
// again and no other; the same save with its "map_seen" taken out (an older save) loaded: none seen; slot 9 again, the item
// taken: off the marks and in the inventory.  Then the shots (desktop, or VR with =vr): every map item marked seen, the
// wheel opened, its map at rest and zoomed photographed (ol_mapitems_<level>_<mode>_after_rest/zoom), then the items off
// (the map as it was before) and the same two (..._before_...).

namespace
{
	struct FMapItemsTest
	{
		int32 Step = 0;
		double At = 0.0;
		int32 Pass = 0, Fail = 0;
		FString Mode;
		bool bVRMode = false;
		AActor* Target = nullptr;
		FVector TargetAt = FVector::ZeroVector;
		FString TargetItem;
		TArray<FVector> Far, Wall, Near;         // the stands (floor points) found round the target, best first
		int32 Try = 0;
		FString SaveText;
		int32 OtherSeen = 0;
		int32 Shot = 0;
		double ClearAt = 0.0;                    // (a stand's first frames may still look the old way: its marks cleared after them)
	};
	FMapItemsTest MI;
	constexpr int32 MapItemsSlot = 9;
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::MapItemsTestTick()
{
	AOutlawsLevelActor* L = Level.Get();
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!L || !PC || L->IsFrontEnd() || L->PendingLoad.IsValid() || MI.Step < 0 || !Camera) return;
	const double Now = FPlatformTime::Seconds();
	if (MI.At == 0.0)
	{
		MI.At = Now + 5.0;
		OLFlags::Value(TEXT("OLMapItemsTest"), MI.Mode);
		MI.bVRMode = MI.Mode == TEXT("vr");
		return;
	}
	if (MI.ClearAt > 0.0 && Now >= MI.ClearAt)
	{
		L->DebugClearMapSeen();
		MI.ClearAt = 0.0;
	}
	if (Now < MI.At) return;
	if (MI.bVRMode) bVR = true;            // (the wheel as the headset has it; this run only)
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? MI.Pass : MI.Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const FString LevelTag = L->LevelName.ToLower();
	const FString ModeTag = MI.bVRMode ? TEXT("vr") : TEXT("desktop");
	auto Shot = [&](const FString& Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_mapitems_%s_%s_%s.png"), *LevelTag, *ModeTag, *Name);
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS shot %s"), *Path);
	};
	const double Half = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 90.0;
	auto Seen = [&]() { return L->DebugIsMapItemSeen(MI.Target); };
	auto InMarks = [&]()
	{
		TArray<AOutlawsLevelActor::FMapItemMark> M;
		L->MapItemMarks(M);
		for (const AOutlawsLevelActor::FMapItemMark& K : M) if (FVector::Dist(K.Where, MI.TargetAt) < 5.0) return true;
		return false;
	};
	auto StandAt = [&](const FVector& Floor, bool bFacing)
	{
		TeleportTo(Floor + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
		const FVector Eye = Floor + FVector(0.0, 0.0, 160.0);
		FRotator R = (MI.TargetAt + FVector(0.0, 0.0, 15.0) - Eye).Rotation();
		if (!bFacing) R.Yaw += 180.0;
		R.Pitch = bFacing ? R.Pitch : 0.0;
		PC->SetControlRotation(R);
		MI.ClearAt = FPlatformTime::Seconds() + 0.3;
		MI.At = FPlatformTime::Seconds() + 0.9;
	};
	// the eyes now against the target: how far, how far off the view's middle, a clear line
	auto Geometry = [&](double& OutD, double& OutDeg, bool& OutClear)
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Low = MI.TargetAt + FVector(0.0, 0.0, 15.0), High = MI.TargetAt + FVector(0.0, 0.0, 35.0);
		OutD = FVector::Dist(Eye, Low);
		OutDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((Low - Eye).GetSafeNormal(), Camera->GetForwardVector()), -1.0, 1.0)));
		OutClear = L->DebugCanSee(Eye, Low, MI.Target) || L->DebugCanSee(Eye, High, MI.Target);
		return FString::Printf(TEXT("%.0f cm, %.0f degrees off, line %s"), OutD, OutDeg, OutClear ? TEXT("clear") : TEXT("blocked"));
	};
	auto OpenMapWheel = [&](bool bZoom)
	{
		CloseWheel();
		const int32 One = OneHand();
		OpenWheel(One >= 0 ? One : (LeftDominant() ? 0 : 1));
		WheelMapTestHover = bZoom ? 1 : -1;
		bWheelMapZoom = bZoom;
	};
	switch (MI.Step)
	{
	case 0:
	{
		TArray<AOutlawsLevelActor::FMapItemDebug> Items;
		L->DebugMapItems(Items);
		FString List;
		for (const AOutlawsLevelActor::FMapItemDebug& D : Items) List += FString::Printf(TEXT("%s%s"), List.IsEmpty() ? TEXT("") : TEXT(", "), *D.Item);
		UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS %s: %d map items lying about: %s"), *LevelTag, Items.Num(), *List);
		Check(Items.Num() > 0, FString::Printf(TEXT("%s has map items (%d)"), *LevelTag, Items.Num()));
		if (MI.bVRMode || Items.Num() == 0) { MI.Step = 20; return; }
		L->DebugClearMapSeen();
		// the stands round each item in turn until one has all three kinds
		UWorld* W = GetWorld();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsMapItemsTest), true);
		Q.AddIgnoredActor(this);
		for (const AOutlawsLevelActor::FMapItemDebug& D : Items)
		{
			TArray<FVector> Far, Wall, Near;
			for (int32 k = 0; k < 24; ++k)
			{
				const FVector Dir = FRotator(0.0, k * 15.0, 0.0).Vector();
				for (const double Dist : { 220.0, 280.0, 340.0, 380.0, 650.0, 750.0, 850.0 })
				{
					const FVector Over = D.Where + Dir * Dist + FVector(0.0, 0.0, 120.0);
					FHitResult H;
					if (!W->LineTraceSingleByChannel(H, Over, Over - FVector(0.0, 0.0, 420.0), ECC_Visibility, Q)) continue;
					const FVector Floor = H.ImpactPoint;
					if (FMath::Abs(Floor.Z - D.Where.Z) > 60.0 || L->CullSectorOf(Floor + FVector(0.0, 0.0, 20.0)) < 0) continue;
					FHitResult Up;
					if (W->LineTraceSingleByChannel(Up, Floor + FVector(0.0, 0.0, 20.0), Floor + FVector(0.0, 0.0, 200.0), ECC_Visibility, Q)) continue;
					const FVector Eye = Floor + FVector(0.0, 0.0, 160.0);
					const FVector Low = D.Where + FVector(0.0, 0.0, 15.0), High = D.Where + FVector(0.0, 0.0, 35.0);
					const bool bClear = L->DebugCanSee(Eye, Low, D.Actor) || L->DebugCanSee(Eye, High, D.Actor);
					const double Dd = FVector::Dist(Eye, Low);
					if (Dd > 640.0 && Dd < 950.0 && bClear) Far.Add(Floor);
					else if (Dd < 440.0 && !bClear) Wall.Add(Floor);
					else if (Dd < 440.0 && bClear) Near.Add(Floor);
				}
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS %s at %s: %d far, %d behind a wall, %d near stands"), *D.Item, *D.Where.ToString(), Far.Num(), Wall.Num(), Near.Num());
			if (Far.Num() && Wall.Num() && Near.Num())
			{
				MI.Target = D.Actor;
				MI.TargetAt = D.Where;
				MI.TargetItem = D.Item;
				MI.Far = Far;
				MI.Wall = Wall;
				MI.Near = Near;
				break;
			}
		}
		// (the hideout always has them; a level like the train, a narrow strip of cars, may have no walled stand at all)
		if (MI.Target || LevelTag == TEXT("hideout")) Check(MI.Target != nullptr, FString::Printf(TEXT("an item with far, walled and near stands round it (%s)"), *MI.TargetItem));
		else UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS %s: no item with far, walled and near stands round it; the seen rule is the hideout run's"), *LevelTag);
		if (!MI.Target) { MI.Step = 20; return; }
		MI.Try = 0;
		StandAt(MI.Far[0], true);
		MI.Step = 1;
		return;
	}
	case 1:
	case 2:
	case 3:
	{
		// 1 far, facing; 2 behind a wall, facing; 3 near, facing away: never seen
		double D, Deg;
		bool bClear;
		const FString G = Geometry(D, Deg, bClear);
		TArray<FVector>& Stands = MI.Step == 1 ? MI.Far : MI.Step == 2 ? MI.Wall : MI.Near;
		const bool bAsMeant = MI.Step == 1 ? (D > AOutlawsLevelActor::MapSeenCm && bClear && Deg < 20.0) : MI.Step == 2 ? (D < AOutlawsLevelActor::MapSeenCm && !bClear && Deg < 20.0)
			: (D < AOutlawsLevelActor::MapSeenCm && bClear && Deg > 90.0);
		if (!bAsMeant && ++MI.Try < Stands.Num())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS stand %d of step %d not as meant (%s): the next"), MI.Try - 1, MI.Step, *G);
			StandAt(Stands[MI.Try], MI.Step != 3);
			return;
		}
		static const TCHAR* const What[] = { TEXT(""), TEXT("out of range (in plain view, facing it)"), TEXT("behind a wall within 5 m (facing it)"), TEXT("within 5 m in plain view, facing away") };
		Check(bAsMeant && !Seen() && !InMarks(), FString::Printf(TEXT("%s %s: not seen, not on the map (%s)"), *MI.TargetItem, What[MI.Step], *G));
		MI.Try = 0;
		++MI.Step;
		StandAt(MI.Step == 2 ? MI.Wall[0] : MI.Near[0], MI.Step != 3);      // (2 behind the wall, 3 near facing away, 4 near facing it)
		return;
	}
	case 4:
	{
		double D, Deg;
		bool bClear;
		const FString G = Geometry(D, Deg, bClear);
		const bool bAsMeant = D < AOutlawsLevelActor::MapSeenCm && bClear && Deg < AOutlawsLevelActor::MapSeenConeDeg;
		if (!bAsMeant && ++MI.Try < MI.Near.Num())
		{
			StandAt(MI.Near[MI.Try], true);
			return;
		}
		Check(bAsMeant && Seen() && InMarks(), FString::Printf(TEXT("%s within 5 m in plain view, facing it: seen, on the map (%s)"), *MI.TargetItem, *G));
		// the page drawn with it
		OpenMapWheel(true);
		MI.Step = 5;
		MI.At = Now + 0.8;
		return;
	}
	case 5:
	{
		Check(MapSpotsDrawn == L->MapMarksSignature(), TEXT("the wheel's map drawn again with it"));
		Shot(TEXT("seen_zoom"));
		MI.Step = 6;
		MI.At = Now + 0.4;
		return;
	}
	case 6:
	{
		CloseWheel();
		WheelMapTestHover = -1;
		bWheelMapZoom = false;
		// turned away and gone far off
		StandAt(MI.Far[0], false);
		MI.ClearAt = 0.0;                // (no clearing here: what was seen stays)
		MI.Step = 7;
		return;
	}
	case 7:
	{
		double D, Deg;
		bool bClear;
		const FString G = Geometry(D, Deg, bClear);
		Check(Seen() && InMarks(), FString::Printf(TEXT("looked away and gone %.0f cm off: still on the map (%s)"), D, *G));
		Check(L->SaveGame(MapItemsSlot), TEXT("saved to slot 9"));
		FFileHelper::LoadFileToString(MI.SaveText, *AOutlawsLevelActor::SavePath(MapItemsSlot));
		Check(MI.SaveText.Contains(TEXT("\"map_seen\"")), TEXT("the save names the seen item (map_seen)"));
		L->DebugClearMapSeen();
		Check(!Seen(), TEXT("every mark cleared before the load"));
		Check(L->LoadGame(MapItemsSlot), TEXT("slot 9 loading"));
		MI.Step = 8;
		MI.At = Now + 3.0;
		return;
	}
	case 8:
	{
		// (the level is built again by the load: the item's actor is a new one, found by its place)
		TArray<AOutlawsLevelActor::FMapItemDebug> Items;
		L->DebugMapItems(Items);
		int32 SeenN = 0, TargetSeen = 0;
		for (const AOutlawsLevelActor::FMapItemDebug& D : Items)
		{
			if (FVector::Dist(D.Where, MI.TargetAt) < 5.0) { MI.Target = D.Actor; TargetSeen = D.bSeen ? 1 : 0; }
			SeenN += D.bSeen ? 1 : 0;
		}
		Check(TargetSeen == 1 && SeenN == 1, FString::Printf(TEXT("after the load: %s seen again (%d), %d seen in all (the load's own frames may add none: the player stands far off)"), *MI.TargetItem, TargetSeen, SeenN));
		// the same save as an older one, no "map_seen": loaded, none seen
		FString Old = MI.SaveText.Replace(TEXT("\"map_seen\""), TEXT("\"map_seen_x\""));
		Check(Old != MI.SaveText, TEXT("the older save made (map_seen taken out)"));
		TSharedPtr<FJsonObject> O;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Old), O);
		if (O.IsValid()) L->LoadGameFrom(O, TEXT("an older save"));
		MI.Step = 9;
		MI.At = Now + 3.0;
		return;
	}
	case 9:
	{
		TArray<AOutlawsLevelActor::FMapItemDebug> Items;
		L->DebugMapItems(Items);
		int32 SeenN = 0;
		for (const AOutlawsLevelActor::FMapItemDebug& D : Items) SeenN += D.bSeen ? 1 : 0;
		Check(SeenN == 0, FString::Printf(TEXT("an older save (no map_seen) loads with none seen (%d)"), SeenN));
		L->LoadGame(MapItemsSlot);
		MI.Step = 10;
		MI.At = Now + 3.0;
		return;
	}
	case 10:
	{
		TArray<AOutlawsLevelActor::FMapItemDebug> Items;
		L->DebugMapItems(Items);
		for (const AOutlawsLevelActor::FMapItemDebug& D : Items) if (FVector::Dist(D.Where, MI.TargetAt) < 5.0) MI.Target = D.Actor;
		Check(Seen() && InMarks(), TEXT("slot 9 again: on the map"));
		const int32 Before = L->GetCount(MI.TargetItem);
		const bool bTaken = L->CollectPickupActor(MI.Target, true);
		MI.Target = nullptr;
		Check(bTaken && !InMarks() && L->GetCount(MI.TargetItem) > Before, FString::Printf(TEXT("picked up: off the map, in the inventory (%s %d to %d)"), *MI.TargetItem, Before, L->GetCount(MI.TargetItem)));
		IFileManager::Get().Delete(*AOutlawsLevelActor::SavePath(MapItemsSlot));
		MI.Step = 20;
		MI.At = Now + 0.5;
		return;
	}
	case 20:
	{
		// the shots: every map item seen, the wheel opened
		TArray<AOutlawsLevelActor::FMapItemDebug> Items;
		L->DebugMapItems(Items);
		for (const AOutlawsLevelActor::FMapItemDebug& D : Items) L->DebugSetMapSeen(D.Index, true);
		AOutlawsLevelActor::bDebugMapItemsOff = false;
		OpenMapWheel(false);
		MI.Shot = 0;
		MI.Step = 21;
		MI.At = Now + 0.8;
		return;
	}
	case 21:
	{
		// after_rest, after_zoom, before_rest, before_zoom
		static const TCHAR* const Names[] = { TEXT("after_rest"), TEXT("after_zoom"), TEXT("before_rest"), TEXT("before_zoom") };
		if (MI.Shot >= 4)
		{
			CloseWheel();
			AOutlawsLevelActor::bDebugMapItemsOff = false;
			UE_LOG(LogOutlaws, Display, TEXT("OLMAPITEMS %s %s done: %d PASS, %d FAIL%s"), *LevelTag, *ModeTag, MI.Pass, MI.Fail, MI.Fail == 0 ? TEXT(" ALL PASS") : TEXT(""));
			MI.Step = 22;
			MI.At = Now + 1.0;
			return;
		}
		if (MI.Shot == 0)
		{
			TArray<AOutlawsLevelActor::FMapItemMark> M;
			L->MapItemMarks(M);
			Check(bWheelOpen && WheelMapPage && WheelMapPage->IsVisible() && MapSpotsDrawn == L->MapMarksSignature(), FString::Printf(TEXT("the wheel's map open, drawn with the %d seen items"), M.Num()));
		}
		Shot(Names[MI.Shot]);
		++MI.Shot;
		MI.Step = 23;
		MI.At = Now + 0.3;           // (the shot is taken at this frame's end)
		return;
	}
	case 23:
	{
		AOutlawsLevelActor::bDebugMapItemsOff = MI.Shot >= 2;
		OpenMapWheel(MI.Shot % 2 == 1);
		MapSpotsWait = 0.f;
		RefreshMapMarks(0.f);
		MI.Step = 21;
		MI.At = Now + 0.8;
		return;
	}
	default:
		MI.Step = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}

OL_SHOT_MATCH(OLMapItemsTest) { return OLFlags::Has(TEXT("OLMapItemsTest")); }
OL_SHOT_TEST(OLMapItemsTest, 285)
{
	MapItemsTestTick();              // (REQ-MAPITEMS: OutlawsMapItems.cpp)
}
#endif // WITH_OUTLAWS_TESTS
