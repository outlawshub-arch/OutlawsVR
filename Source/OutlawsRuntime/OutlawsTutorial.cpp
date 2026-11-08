// REQ-TUTORIAL (PLANS.md, rewritten 10 Oct against the real controls) and REQ-TUTORIAL2 (user 14 Oct: cards you step through,
// bottles first, a better order): New Game > Tutorial.  Single player in town_2p (Sanctuary), about four minutes: one street,
// each card ONE short action on a small panel with Back and Next (AOutlawsTutorialCard, the join bubble's look, where the
// notice lines stood).  A card never moves on by itself: once its action is seen (counted where it happens:
// AOutlawsPlayer::TutNotes) it shows a tick and "Good." with a short sound and Next lights up.  Next always works (a player
// may skip ahead); Back shows the card before and sets its station up again (the bottles stand, a bad guy comes back).
// "Step N of M" on each.  A red arrow floats over the card's target (the post, the bottles, the box, the barrel, the
// crates, the door, the horse).
//   1 walk            the moving stick to the post under the arrow
//   2 turn            the turning stick or the body
//   3 take_revolver   the main hand's wheel (no holsters), the revolver taken
//   4 fire            five bottles on a rail, all five broken (any shots)
//   5 fan             REQ-TUTFAN: the other hand, empty, swiped back across the revolver's hammer: one fanned shot
//   6 reload          B / Y (the Classic layout and one controller: a tap of the wheel button)
//   7 pickup          a box of bullets on the ground: a hand's grip on it, or walked over
//   8 take_knife      the other hand's wheel, the knife
//   9 throw_knife     aimed by the hand, thrown higher to go further (THROWAIM), at the post
//  10 take_dynamite   a stick from the other hand's wheel
//  11 throw_dynamite  lit with the trigger, thrown at the barrel (the game's barrel sprite, ahead on the right)
//  12 climb           the crates, by their top edge (one hand is enough)
//  13 jump            a crouch (the turning stick held down, or the body) and a jump (pushed up, or a dip and rise), both
//                     waited for; one controller: the jump
//  14 door            the two front doors of the building by the well, across the street from the jail (REQ-TUTDOORS), each
//                     pushed open by hand; not found: the street's nearest door (left out when the level has none near)
//  15 lantern         the lantern from the other hand's wheel, lit
//  16 grab            a bad guy rushes in and holds still (frozen) until a hand takes him (a gun hand too)
//  17 punch           the other hand's fist
//  18 grab_shoot      another: taken, then shot
//  19 fight           two more at range, no freeze; the player is left hurt (health 6) for the next card
//  20 heal            the Medical Bag from a wheel: only using it finishes the card
//  21 pause           the menu button: the pause menu opened (Options has comfort and speed)
//  22 horse           "You're ready. Ride out!": a hand's grip on the horse ends it; back to the main menu, "Tutorial
//                     complete" (Finish on the card does the same)
// One controller (REQ-ONECTRL): no fan, no punch and no grab_shoot (no other hand), the grab card asks for a throw instead.
// Nothing is saved (no autosave, no level opening), no best time, no unlock (CompleteLevel is never called), health stops at
// 1 (DamagePlayer).  "Skip tutorial" heads the pause page (OutlawsPlayerMenu.cpp).  The props are the engine cube in the
// level's own wood (the post, the rail, the crates), the game's own bottles, box of bullets, barrel and horse sprite; the
// stations are laid along the street the start faces (the longest clear line from the start).
// -OLTutorialTest=walk|skip|menu (scratchpad gate_popin\tutorial_checks.ps1): from the title menu through New Game.
//   walk  every card driven as the player would (the wheel's buttons, the trigger, the sticks, the grips; the punch's own
//         blow, a desktop having no swinging fist), each card's "Good." and its Next pressed, each card's step and line
//         checked and photographed; Back once from three stations (the bottles stand again, the knife is back in the hand,
//         the bad guy comes back); then the front menu again and its "Tutorial complete" line
//   skip  Next pressed on the first card before its action (it still moves on), three cards in the pause page: "Skip
//         tutorial" its top row, pressed: the front menu, no tutorial
//   menu  New Game's rows (Main Game, Historical Missions, Multiplayer, Tutorial), each opening its page; REQ-MPMENU: the
//         walk Multiplayer > Co-op > Host / Join and > Competitive > Host / Join, each page's rows, title and fit, Back up;
//         then Historical Missions > Ice Caves > Good builds the Handful mission (no tutorial; REQ-HMCAREER: the five
//         original missions only through Marshal Training, -OLHmCareerTest)
//   float REQ-TUTCARDFLOAT: a new card's first seconds, stage by stage (phase 30 below)
//   doors REQ-TUTDOORS: the pair by name, a push on each, Back shuts them (phase 31 below)
//   fan   REQ-TUTFAN: its station, and a hand really swiped past the hammer (phase 32 below)
//   grabfist  REQ-TUTGRABFIST: the other hand given a lit stick, a knife, the lantern, the revolver (phase 33 below)
//   gunfire   REQ-TUTGUNFIRE: a gun through the low card's buttons fires, an empty hand and a fist press, the risen card as ever (phase 34 below)
// Prints "OLTUT test check PASS|FAIL: ..." and "OLTUT test RESULT ...".  Screenshots: Saved/OLShots/ol_tut_<name>.png.

#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsAchievements.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsCoop.h"
#include "OutlawsTextures.h"
#include "OutlawsTutorialCard.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "MotionControllerComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/IConsoleManager.h"
#include "UnrealClient.h"

namespace OutlawsTutorialLocal
{
	enum ECard : int32
	{
		Walk, Turn, TakeRevolver, Fire, Fan, Reload, Pickup, TakeKnife, ThrowKnife, TakeDynamite, ThrowDynamite, Climb, Jump,
		Door, Lantern, Grab, Punch, GrabShoot, Fight, Heal, Pause, Horse, NumCards
	};
	const TCHAR* const CardIds[NumCards] = { TEXT("walk"), TEXT("turn"), TEXT("take_revolver"), TEXT("fire"), TEXT("fan"), TEXT("reload"), TEXT("pickup"),
		TEXT("take_knife"), TEXT("throw_knife"), TEXT("take_dynamite"), TEXT("throw_dynamite"), TEXT("climb"), TEXT("jump"), TEXT("door"),
		TEXT("lantern"), TEXT("grab"), TEXT("punch"), TEXT("grab_shoot"), TEXT("fight"), TEXT("heal"), TEXT("pause"), TEXT("horse") };
	constexpr int32 RevolverIdx = 0, DynamiteIdx = 6, KnifeIdx = 7;
	constexpr int32 NumBottles = 5;
	constexpr int32 FanRounds = 3;           // (REQ-TUTFAN: the fewest the fan card leaves in the cylinder)
	// the line the front menu shows once a finished tutorial is back at it
	FString GToast;
	// (REQ-TUTDOORS) the door card is about the pair (TutorialCardLines has no run to look at)
	bool GTwoDoors = false;
	bool OldTutDoors()
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldTutDoors"));
		return bOld;
	}
	bool OldTutFan()
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldTutFan"));
		return bOld;
	}
	bool OldTutGrabFist()
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldTutGrabFist"));
		return bOld;
	}
}

struct FOutlawsTutorialRun
{
	int32 Card = -1;                         // -1: the level is not up yet; NumCards: done
	int32 Done = 0;                          // cards whose action was seen
	bool bGood = false;                      // this card's action seen (the tick, Next lit)
	FString Line, Hint;                      // the card's words now
	double CardAt = 0.0;                     // real seconds the card went up
	double Began = 0.0;                      // real seconds the first card went up
	AOutlawsPlayer::FTutorialNotes At;       // the counts when the card went up
	FVector Start = FVector::ZeroVector;     // the start's feet
	FVector Dir = FVector::ForwardVector;    // along the street
	FVector Side = FVector::RightVector;
	double Scale = 1.0;
	float StreetYaw = 0.f;
	FVector KnifePost, Barrel, Marker, Crates, HorseAt, ManFrom, FightA, FightB, Rail, BoxAt;
	double CrateTop = 0.0, CrateHalf = 75.0, RailTop = 0.0;
	TWeakObjectPtr<AActor> Horse;
	TWeakObjectPtr<AOutlawsEnemy> Men[4];
	bool bHeld[4] = { false, false, false, false };
	bool bFrozen[4] = { false, false, false, false };
	TWeakObjectPtr<AOutlawsSpriteActor> Bottles[OutlawsTutorialLocal::NumBottles];
	FVector BottleAt[OutlawsTutorialLocal::NumBottles];
	TWeakObjectPtr<AActor> Box;              // the pickup card's box of bullets
	int32 DoorSector = -1;                   // the door card's door (its master sector), -1: none near
	int32 DoorSector2 = -1;                  // (REQ-TUTDOORS) the pair's other leaf, -1: one door
	FVector DoorAt = FVector::ZeroVector;    // its middle at the floor
	FVector DoorOut = FVector::ZeroVector;   // the way out from it, to the side the player comes from
	FVector DoorArrow = FVector::ZeroVector; // what the arrow floats over
	float TurnSum = 0.f, LastYaw = 0.f;
	bool bCrouchSeen = false, bJumpSeen = false;   // (the jump card's two actions)
	float JumpHead0 = 0.f;
	bool bMenuSeen = false;                  // (the pause card)
	TWeakObjectPtr<AActor> BarrelActor;      // (the dynamite's barrel: the game's own barrel sprite where the install has one)
	TWeakObjectPtr<AOutlawsTutorialCard> CardActor;
	TWeakObjectPtr<AActor> Arrow;            // the floating marker
};

namespace OutlawsTutorialLocal
{
	// (one controller: no other hand to punch or shoot with while holding, or to fan with; no door near: no door card)
	bool Skipped(const AOutlawsPlayer* P, const FOutlawsTutorialRun* R, int32 Card)
	{
		if (P && P->OneHand() >= 0 && (Card == Punch || Card == GrabShoot || Card == Fan)) return true;
		if (Card == Fan && OldTutFan()) return true;
		return Card == Door && R && R->DoorSector < 0;
	}
	int32 StepsOf(const AOutlawsPlayer* P, const FOutlawsTutorialRun* R, int32 Upto)
	{
		int32 N = 0;
		for (int32 c = 0; c < Upto && c < NumCards; ++c) if (!Skipped(P, R, c)) ++N;
		return N;
	}
}

FString AOutlawsLevelActor::TutorialCardId() const
{
	using namespace OutlawsTutorialLocal;
	if (!bTutorial || !TutorialRun.IsValid() || TutorialRun->Card < 0 || TutorialRun->Card >= NumCards) return FString();
	return CardIds[TutorialRun->Card];
}

void AOutlawsLevelActor::TutorialCardLines(const AOutlawsPlayer* P, const FString& CardId, FString& Line, FString& Hint)
{
	using namespace OutlawsTutorialLocal;
	int32 Card = NumCards;
	for (int32 i = 0; i < NumCards; ++i) if (CardId == CardIds[i]) Card = i;
	Line.Reset();
	Hint.Reset();
	// (user 2026-10-14: each card says exactly which button, on which controller, and where to look, in short plain
	// sentences; REQ-TUTORIAL2: one action a card, the hand named every time, by Options' main hand and the one-controller
	// mapping.  REQ-BUTTONGLYPHS: the face buttons are drawn under the card, marked; the line says "the marked button"
	// instead of its letter, which a remap or the Classic layout could make wrong)
	const int32 One = P->OneHand();
	const bool bLeft = One >= 0 ? One == 0 : P->LeftDominant();
	const TCHAR* Btn = TEXT("the marked button");
	auto On = [One](bool bLeftHand) { return One >= 0 ? FString(TEXT("on your controller")) : FString::Printf(TEXT("on your %s controller"), bLeftHand ? TEXT("left") : TEXT("right")); };
	const FString OnDom = On(bLeft), OnOff = On(!bLeft);
	const FString OnMove = On(!bLeft), OnTurn = On(bLeft);         // (the moving stick is the off hand's, turning the main hand's)
	switch (Card)
	{
	case Walk: Line = FString::Printf(TEXT("Push the stick %s forward to walk to the post under the red arrow."), *OnMove); break;
	case Turn:
		Line = One >= 0 ? FString(TEXT("Push the stick on your controller hard left or right to turn."))
			: FString::Printf(TEXT("Push the stick %s left or right to turn."), *OnTurn);
		Hint = TEXT("Or just turn your body.");
		break;
	case TakeRevolver:
		Line = FString::Printf(TEXT("Hold %s %s to open its weapon wheel."), Btn, *OnDom);
		Hint = TEXT("Point at the revolver, then let go of the button.");
		break;
	case Fire:
		Line = FString::Printf(TEXT("Shoot the five bottles on the rail: pull the trigger %s."), *OnDom);
		break;
	// REQ-TUTFAN (user 2026-11-04): what really fans (UpdateFan): the other hand empty, its palm not up, swept back or down past
	// the hammer, over 80 cm/s and within 13 cm of the gun's back; the trigger plays no part
	case Fan:
		Line = FString::Printf(TEXT("Now fan it: swipe your empty %s hand back across the hammer, palm down."), bLeft ? TEXT("right") : TEXT("left"));
		Hint = TEXT("A quick swipe under the arrow. No trigger needed.");
		break;
	case Reload:
		Line = P->bFaceReload && One < 0 ? FString::Printf(TEXT("Press %s %s to reload."), Btn, *OnDom)
			: FString::Printf(TEXT("Tap %s %s to reload."), Btn, *OnDom);
		break;
	case Pickup:
		Line = FString::Printf(TEXT("Reach down to the box of bullets and squeeze the grip %s."), *OnOff);
		Hint = TEXT("Or just walk over it.");
		break;
	case TakeKnife:
		Line = One >= 0 ? FString::Printf(TEXT("Hold %s on your controller again and take the knife."), Btn)
			: FString::Printf(TEXT("Your other hand has its own wheel. Hold %s %s."), Btn, *OnOff);
		Hint = TEXT("Point at the knife, then let go of the button.");
		break;
	case ThrowKnife:
		Line = FString::Printf(TEXT("Hold the trigger %s, point at the post under the arrow, then let go."), *OnOff);
		Hint = TEXT("Raise your hand to throw further.");
		break;
	case TakeDynamite:
		Line = FString::Printf(TEXT("Hold %s %s and take the dynamite from the wheel."), Btn, *OnOff);
		break;
	case ThrowDynamite:
		Line = FString::Printf(TEXT("Hold the trigger %s to light the fuse. Aim at the barrel under the arrow, then let go."), *OnOff);
		Hint = TEXT("Raise your hand to throw further.");
		break;
	case Climb:
		Line = One >= 0 ? FString(TEXT("Climb the crates: reach the top edge, squeeze the grip on your controller and pull yourself up."))
			: FString(TEXT("Climb the crates: reach the top edge, squeeze the grip on either controller and pull yourself up."));
		break;
	case Jump:
		// (two actions: the card waits for both, a crouch and a jump, in either order; one controller: the jump)
		Line = One >= 0 ? FString::Printf(TEXT("Press %s on your controller to jump down."), Btn)
			: FString::Printf(TEXT("Hold the stick %s down to crouch. Then push it up to jump down."), *OnTurn);
		Hint = One >= 0 ? TEXT("Or dip down and spring up.") : TEXT("You can also crouch for real, and jump with a quick dip and rise.");
		break;
	case Door:
		if (GTwoDoors)
		{
			Line = One >= 0 ? FString(TEXT("Walk to the two doors under the arrow and push each one open with your hand."))
				: FString(TEXT("Walk to the two doors under the arrow and push each one open with either hand."));
			break;
		}
		Line = One >= 0 ? FString(TEXT("Walk to the door under the arrow and push it open with your hand."))
			: FString(TEXT("Walk to the door under the arrow and push it open with either hand."));
		break;
	case Lantern:
		Line = FString::Printf(TEXT("Hold %s %s and pick the lantern to light it."), Btn, *OnOff);
		Hint = TEXT("Pick it again to put it away.");
		break;
	case Grab:
		Line = FString::Printf(TEXT("Reach out to him and squeeze the grip %s to grab him."), *OnOff);
		if (One >= 0) Hint = TEXT("Then let go of the grip to throw him.");
		break;
	// (REQ-PUNCHGRIP, user 15 Oct: an open hand swung does not punch; the grip held makes the fist) the hand not holding him
	case Punch:
	{
		const int32 Dom = bLeft ? 0 : 1, Off = 1 - Dom;
		const int32 Fist = P->GrabbedEnemy[Dom].IsValid() && !P->GrabbedEnemy[Off].IsValid() ? Off : Dom;
		Line = FString::Printf(TEXT("Keep holding him. With your %s hand, hold the grip to make a fist, then punch him."), Fist == 0 ? TEXT("left") : TEXT("right"));
		break;
	}
	case GrabShoot:
		Line = FString::Printf(TEXT("Here comes another. Grab him with your %s hand, then shoot him with your %s."), bLeft ? TEXT("right") : TEXT("left"), bLeft ? TEXT("left") : TEXT("right"));
		break;
	case Fight: Line = FString::Printf(TEXT("Two more down the street. Shoot them both with the trigger %s."), *OnDom); break;
	case Heal: Line = FString::Printf(TEXT("Hurt? Hold %s %s and pick the Medical Bag."), Btn, *OnDom); break;
	case Pause:
	{
		// (the menu button is the one its binding names: the left controller's on a Quest)
		const OutlawsGlyphs::FGlyph G = P->TutorialGlyph(CardIds[Pause]).Glyph;
		const bool bMenuLeft = G.Marks.Num() > 0 ? G.Marks[0].Hand == 0 : true;
		Line = FString::Printf(TEXT("Press the menu button %s to pause."), *On(bMenuLeft));
		Hint = TEXT("Options has comfort and speed.");
		break;
	}
	case Horse:
		Line = TEXT("You're ready. Ride out! Walk to the horse and squeeze a grip on its back.");
		Hint = TEXT("See Extras on the main menu for what you can earn.");      // (REQ-EXTRAS)
		break;
	default: break;
	}
}

void AOutlawsLevelActor::StartTutorial()
{
	UE_LOG(LogOutlaws, Log, TEXT("OLTUT start: town_2p (Sanctuary), single player"));
	OutlawsTutorialLocal::GToast.Reset();
	// (a Then given: the build is a rebuild "with more to it than a level start", so it is made in place, never a map load
	// that would lose this actor's tutorial)
	StartMission(TEXT("town_2p"), EOutlawsDifficulty::Good, []() {});
	bHom = false;
	bTutorial = true;
	TutorialRun = MakeShared<FOutlawsTutorialRun>();
}

static void OlTutorialClear(FOutlawsTutorialRun* R)
{
	if (!R) return;
	if (AOutlawsTutorialCard* C = R->CardActor.Get()) { C->Hide(); C->Destroy(); }
	if (AActor* A = R->Arrow.Get()) A->Destroy();
}

void AOutlawsLevelActor::EndTutorial(bool bSkipped)
{
	if (!bTutorial) return;
	FOutlawsTutorialRun* R = TutorialRun.Get();
	UE_LOG(LogOutlaws, Log, TEXT("OLTUT end: %s after %d cards, %.0f s"), bSkipped ? TEXT("skipped") : TEXT("finished"), R ? R->Done : 0,
		R && R->Began > 0.0 ? FPlatformTime::Seconds() - R->Began : 0.0);
	OlTutorialClear(R);
	bTutorial = false;
	TutorialRun.Reset();
	OutlawsTutorialLocal::GToast = bSkipped ? FString() : FString(TEXT("Tutorial complete"));
	if (!bSkipped) OutlawsAchievements::NoteTutorialDone(this);      // (REQ-EXTRAS)
	ReturnToFrontEnd();
}

void AOutlawsLevelActor::TickTutorial(float Dt)
{
	using namespace OutlawsTutorialLocal;
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!GToast.IsEmpty() && bFrontEnd && P && P->bMenuOpen && P->MenuPage && P->MenuPage->File == TEXT("#front") && !bRebuildPending)
	{
		P->CoopToast(GToast);
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT the main menu says: %s"), *GToast);
		GToast.Reset();
	}
	if (!bTutorial || !TutorialRun.IsValid()) return;
	if (bFrontEnd && !bRebuildPending)
	{
		// (left another way: End Game, a load) nothing of the tutorial goes on
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT left for the main menu"));
		OlTutorialClear(TutorialRun.Get());
		bTutorial = false;
		TutorialRun.Reset();
		return;
	}
	if (!P || bNeedPlace || bRebuildPending || !Inf || bFrontEnd || !LevelName.Equals(TEXT("town_2p"), ESearchCase::IgnoreCase)) return;
	FOutlawsTutorialRun& R = *TutorialRun;
	const double Now = FPlatformTime::Seconds();
	const int32 One = P->OneHand();
	const int32 DomH = One >= 0 ? One : P->LeftDominant() ? 0 : 1;
	const int32 OffH = One >= 0 ? One : 1 - DomH;
	auto Feet = [P]() { return P->GetActorLocation() - FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
	auto Floor = [World](const FVector& At, double& OutZ) -> bool
	{
		FHitResult H;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutFloor), true);
		if (World->LineTraceSingleByChannel(H, At + FVector(0.0, 0.0, 250.0), At - FVector(0.0, 0.0, 600.0), ECC_Visibility, Q))
		{
			OutZ = H.ImpactPoint.Z;
			return true;
		}
		return false;
	};
	// a box of the engine's cube in the level's own wood, solid (the crates hold a ledge) unless bGhost
	auto Block = [this, World, &Floor](const FVector& At, const FVector& Size, float Yaw, bool bGhost, const TCHAR* What) -> FVector
	{
		double Z = At.Z;
		Floor(At, Z);
		const FVector Base(At.X, At.Y, Z);
		static UStaticMesh* Cube = nullptr;
		if (!Cube) Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.ObjectFlags |= RF_Transient;
		AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Base), SP);
		if (!A || !Cube) return Base;
		UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(A);
		M->SetStaticMesh(Cube);
		M->SetMobility(EComponentMobility::Movable);
		M->SetCollisionProfileName(bGhost ? TEXT("NoCollision") : TEXT("BlockAll"));
		M->SetCastShadow(false);
		A->SetRootComponent(M);
		M->RegisterComponent();
		A->SetActorLocationAndRotation(Base + FVector(0.0, 0.0, Size.Z * 0.5), FRotator(0.0, Yaw, 0.0));
		A->SetActorScale3D(Size / 100.0);
		// the level's wood (a crate's, a box's, a plank's), else its first texture
		static const TCHAR* Hints[] = { TEXT("crat"), TEXT("box"), TEXT("wood"), TEXT("plank"), TEXT("barl"), TEXT("brd") };
		FString Key;
		for (const TCHAR* H : Hints)
		{
			for (const TPair<FString, TObjectPtr<UTexture2D>>& Kv : Textures) if (Kv.Key.Contains(H)) { Key = Kv.Key; break; }
			if (!Key.IsEmpty()) break;
		}
		if (Key.IsEmpty()) for (const TPair<FString, TObjectPtr<UTexture2D>>& Kv : Textures) { Key = Kv.Key; break; }
		if (UMaterialInstanceDynamic* Mid = Key.IsEmpty() ? nullptr : MaterialFor(WorldMaterial, Key)) M->SetMaterial(0, Mid);
		SpawnedActors.Add(A);
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT prop %s at %s, %s cm, %s (%s)"), What, *Base.ToCompactString(), *Size.ToCompactString(), *Key, bGhost ? TEXT("not solid") : TEXT("solid"));
		return Base;
	};
	auto SpawnMan = [this](const FVector& At, float Yaw, float HealthPts) -> AOutlawsEnemy*
	{
		if (!AssetsRef) return nullptr;
		FString Itm, Func;
		TMap<FString, FString> Data;
		const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, TEXT("bgy1"), &Itm);
		OutlawsFormats::ParseItm(Itm, Func, Data);
		// (low accuracy and damage; the punch's one blow, or one shot, drops him; he drops nothing)
		Data.Add(TEXT("HEALTH"), FString::SanitizeFloat(HealthPts));
		Data.Add(TEXT("DAMAGE"), TEXT("0.3"));
		Data.Add(TEXT("WEAPON_INACCURACY"), TEXT("14"));
		Data.Add(TEXT("MOVE_INACCURACY"), TEXT("30"));
		Data.Add(TEXT("DROP_ITEM"), TEXT("null"));
		if (!SpawnEnemy(TEXT("bgy1"), Data, Anim, At + FVector(0.0, 0.0, 2.0), Yaw, LightAt(At + FVector(0.0, 0.0, 60.0)))) return nullptr;
		AOutlawsEnemy* E = Enemies.Last().Get();
		if (E) E->Wake(false);
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT a bad guy (%s) at %s"), E ? *E->GetName() : TEXT("none"), *At.ToCompactString());
		return E;
	};
	// one of the game's bottles (BOTTLE01..05: each a shootable prop, Inv_Object, its own glass flying when hit) standing at At
	auto SpawnBottle = [this, World](int32 i, const FVector& At, float Yaw) -> AOutlawsSpriteActor*
	{
		if (!AssetsRef) return nullptr;
		const FString Name = FString::Printf(TEXT("bottle%02d"), i % NumBottles + 1);
		FString Itm, Func;
		TMap<FString, FString> Data;
		const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, Name, &Itm);
		if (Anim.IsEmpty() || !AssetsRef->Has(Anim)) return nullptr;
		OutlawsFormats::ParseItm(Itm, Func, Data);
		const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(Anim, true);
		if (!Atlas || !Atlas->Texture || !SpriteQuad || !SpriteMaterial) return nullptr;
		const double Wcm = Atlas->CellW / 16.0 * FOutlawsGeometry::UnitsToCm;
		const double Hcm = Atlas->CellH / 16.0 * FOutlawsGeometry::UnitsToCm;
		const FTransform Xf(FRotator::ZeroRotator, At + FVector(0.0, 0.0, 0.5), FVector(1.0, Wcm, Hcm));
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.ObjectFlags |= RF_Transient;
		SP.bDeferConstruction = true;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AOutlawsSpriteActor* S = World->SpawnActor<AOutlawsSpriteActor>(AOutlawsSpriteActor::StaticClass(), Xf, SP);
		if (!S) return nullptr;
		S->OutlawsName = Name.ToUpper();
		S->OutlawsYaw = Yaw;
		S->LightLevel = LightAt(At + FVector(0.0, 0.0, 20.0));
		S->AtlasCols = Atlas->Cols;
		S->AtlasRows = Atlas->Rows;
		S->Directions = Atlas->Dirs;
		S->FrameCount = Atlas->Frames;
		S->FPS = Atlas->FPS;
		S->Grid = Atlas->Grid;
		S->SpriteTexture = Atlas->Texture;
		for (const OutlawsTextures::FSpriteAtlas::FChor& C : Atlas->Chors)
		{
			FOutlawsSpriteChor& D = S->Chors.AddDefaulted_GetRef();
			D.Dirs = C.Dirs;
			D.Frames = C.Frames;
			D.FPS = C.FPS;
			D.GridStart = C.GridStart;
			D.Length = C.Length;
			D.HeightPx = C.HeightPx;
			D.Events = C.Events;
		}
		S->ChorGrid = Atlas->ChorGrid;
		S->SetFeet(Atlas->BelowPx, Atlas->FootPx, Atlas->CellH);
		S->SetFeetAcross(Atlas->SidePx2, Atlas->CellW);
		S->Quad->SetStaticMesh(SpriteQuad);
		S->Quad->SetMaterial(0, SpriteMaterial);
		S->FinishSpawning(Xf);
		SpawnedActors.Add(S);
		UseItemModel(S, Anim, Yaw);
		AddProp(S, S->OutlawsName, Data, -1, false);
		return S;
	};
	auto BottleDown = [&R, this](int32 i) -> bool
	{
		AOutlawsSpriteActor* S = R.Bottles[i].Get();
		if (!S || S->IsHidden() || FVector::Dist(S->GetActorLocation(), R.BottleAt[i]) > 30.0) return true;
		const FProp* Pr = Props.FindByPredicate([S](const FProp& X) { return X.Sprite.Get() == S; });
		return !Pr || Pr->Current != 0;
	};
	auto SetBottles = [&]()
	{
		for (int32 i = 0; i < NumBottles; ++i)
		{
			if (AOutlawsSpriteActor* Old = R.Bottles[i].Get())
			{
				Props.RemoveAll([Old](const FProp& X) { return X.Sprite.Get() == Old; });
				SpawnedActors.Remove(Old);
				Old->Destroy();
			}
			R.Bottles[i] = SpawnBottle(i, R.BottleAt[i], R.StreetYaw + 180.f);
		}
		int32 Made = 0;
		for (const auto& B : R.Bottles) Made += B.IsValid() ? 1 : 0;
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT the bottles stand on the rail (%d of %d made)"), Made, NumBottles);
	};
	auto BoxTaken = [&R, this]() -> bool
	{
		AActor* A = R.Box.Get();
		if (!A || A->IsHidden()) return true;
		const FPickup* Pk = Pickups.FindByPredicate([A](const FPickup& X) { return X.Actor.Get() == A; });
		return !Pk || Pk->bHeld;
	};
	auto SetBox = [&]()
	{
		if (AActor* Old = R.Box.Get())
		{
			Pickups.RemoveAll([Old](const FPickup& X) { return X.Actor.Get() == Old; });
			Old->Destroy();
		}
		R.Box = SpawnGroundItem(TEXT("gamboxb"), R.BoxAt);
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT the box of bullets at %s (%s)"), *R.BoxAt.ToCompactString(), R.Box.IsValid() ? *R.Box->GetName() : TEXT("not made"));
	};
	auto LeafOpen = [this](int32 Sector) -> bool
	{
		const TArray<int32>* Es = Inf->ElevatorsBySector.Find(Sector);
		if (!Es) return true;
		for (int32 e : *Es)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			if ((E.IsMorph() || E.Class == EOutlawsInfClass::MoveCeiling) && (E.Stop != 0 || E.Target >= 0)) return true;
		}
		return false;
	};
	// (REQ-TUTDOORS: the pair's leaves are two elevators, each its own push: done when both have been)
	auto DoorOpen = [&]() -> bool
	{
		if (R.DoorSector < 0 || !Inf) return true;
		return LeafOpen(R.DoorSector) && (R.DoorSector2 < 0 || LeafOpen(R.DoorSector2));
	};
	// (REQ-TUTDOORS) shut again for the card, however it came up: Back, or a Next onto doors the player has already opened.  A
	// held-open door stays open for good otherwise and the card would tick at once.  Nothing else opens them: the level's other
	// bad guys sleep through the tutorial, the card's own have not come yet, and the INF has no trigger or key on them
	auto ShutDoors = [&]()
	{
		for (const int32 Sector : { R.DoorSector, R.DoorSector2 })
		{
			const TArray<int32>* Es = Sector >= 0 ? Inf->ElevatorsBySector.Find(Sector) : nullptr;
			if (!Es) continue;
			for (int32 e : *Es)
			{
				const FOutlawsElevator& E = Inf->Elevators[e];
				if ((E.IsMorph() || E.Class == EOutlawsInfClass::MoveCeiling) && (E.Stop != 0 || E.Target >= 0 || E.bWaiting)) Inf->BackToFirstStop(e);
			}
		}
	};
	auto KillMan = [&R, this](int32 m)
	{
		AOutlawsEnemy* E = R.Men[m].Get();
		if (E && !E->IsDead())
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLTUT %s taken away (his card left)"), *E->GetName());
			E->Destroy();
		}
		R.Men[m] = nullptr;
		R.bHeld[m] = R.bFrozen[m] = false;
	};
	const float Face = float(FMath::RadiansToDegrees(FMath::Atan2(-R.Dir.Y, -R.Dir.X)));       // (looking back down the street)
	auto ManFor = [&](int32 m, bool bFresh)
	{
		if (!bFresh && R.Men[m].IsValid() && !R.Men[m]->IsDead()) return;
		KillMan(m);
		const FVector From = m == 0 ? R.ManFrom : m == 1 ? R.ManFrom + R.Side * 120.0 : m == 2 ? R.FightA : R.FightB;
		R.Men[m] = SpawnMan(From, Face, m < 2 ? 0.4f : 0.5f);
	};
	// a card up (bReset: come back to with Back, its station set up again)
	auto Show = [&](int32 Card, bool bReset)
	{
		R.Card = Card;
		R.CardAt = Now;
		R.At = P->TutNotes;
		R.bGood = false;
		if (Card >= NumCards)
		{
			R.Line.Reset();
			return;
		}
		TutorialCardLines(P, CardIds[Card], R.Line, R.Hint);
		// (REQ-BUTTONGLYPHS: the card's control drawn under it)
		P->TutGlyph = P->TutorialGlyph(CardIds[Card]);
		P->TutGlyphLine.Reset();
		if (R.Began <= 0.0) R.Began = Now;
		if (AOutlawsTutorialCard* C = R.CardActor.Get()) C->SetCard(StepsOf(P, &R, Card) + 1, StepsOf(P, &R, NumCards), R.Line, R.Hint, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT card %d %s shown at %.1f s (step %d of %d%s): %s"), Card + 1, CardIds[Card], Now - R.Began, StepsOf(P, &R, Card) + 1,
			StepsOf(P, &R, NumCards), bReset ? TEXT(", back to it: its station set up again") : TEXT(""), *R.Line);
		// the bad guys a card is not about go (one skipped past with Next, still standing)
		const bool bNeeds[4] = { Card == Grab || Card == Punch, Card == GrabShoot, Card == Fight, Card == Fight };
		for (int32 m = 0; m < 4; ++m) if (!bNeeds[m] && R.Men[m].IsValid()) KillMan(m);
		// what the card brings with it
		switch (Card)
		{
		case Turn:
			R.TurnSum = 0.f;
			R.LastYaw = P->Camera ? float(P->Camera->GetComponentRotation().Yaw) : float(P->GetActorRotation().Yaw);
			break;
		case Fire: if (bReset) SetBottles(); break;
		case Fan:
		{
			// REQ-TUTFAN: its station, whichever way the card came up (the fire card's five bottles can leave one round, or none, and
			// Back can come from a hand holding anything): the revolver in the main hand, changing hands rounds and all as the wheel
			// does it; the other hand empty, as the fan wants; at least FanRounds in the cylinder from the reserve, so a swipe always
			// fires and the card after it still has an open chamber to reload; the bottles up again to fan at
			if (P->Equipped[DomH] != RevolverIdx)
			{
				if (P->Equipped[OffH] == RevolverIdx)
				{
					P->Equip(OffH, -1);
					P->SavedMag.Add(DomH * 16 + RevolverIdx, P->SavedMag.FindRef(OffH * 16 + RevolverIdx));
				}
				P->Equip(DomH, RevolverIdx);
			}
			P->Equip(OffH, -1);
			int32& Reserve = Counts.FindOrAdd(TEXT("ibullit"));
			const int32 Rounds = FMath::Clamp(FanRounds - P->Mag[DomH], 0, Reserve);
			P->Mag[DomH] += Rounds;
			P->Spent[DomH] = FMath::Max(0, P->Spent[DomH] - Rounds);
			Reserve -= Rounds;
			SetBottles();
			UE_LOG(LogOutlaws, Log, TEXT("OLTUT the fan card: %d rounds in the revolver (%d topped up), %d left"), P->Mag[DomH], Rounds, Reserve);
			break;
		}
		case Pickup: if (bReset || !R.Box.IsValid()) SetBox(); break;
		case TakeKnife: case ThrowKnife:
			if (bReset)
			{
				Counts.FindOrAdd(TEXT("iknife")) = 2;
				if (Card == ThrowKnife && P->Equipped[0] != KnifeIdx && P->Equipped[1] != KnifeIdx) P->Equip(OffH, KnifeIdx);
			}
			break;
		case TakeDynamite: case ThrowDynamite:
			if (bReset)
			{
				Counts.FindOrAdd(TEXT("idynam")) = 2;
				if (Card == ThrowDynamite && P->Equipped[0] != DynamiteIdx && P->Equipped[1] != DynamiteIdx) P->Equip(OffH, DynamiteIdx);
			}
			break;
		case Jump:
			R.bCrouchSeen = R.bJumpSeen = false;
			R.JumpHead0 = P->HeadHeight();
			break;
		case Door: if (!OldTutDoors()) ShutDoors(); break;
		case Lantern: if (bReset && bLampOn) ToggleLamp(); break;
		case Grab:
			ManFor(0, bReset);
			// REQ-TUTGRABFIST (user 2026-11-04: "On the tutorial step where it tells you to grab an enemy with your non-dominant hand,
			// it should switch that hand to fist automatically to prevent any confusion"): the hand the line names (OnOff) emptied
			// whichever way the card came up, Next, Back or a skip: Equip's own way, as the wheel's bare hand does it, the rounds
			// kept, a tool or the lantern put away, a lit fuse out and the stick still in the count.  One controller: that one hand
			// is the grab's and the gun's, as before
			if (One < 0 && !OldTutGrabFist() && (P->Equipped[OffH] >= 0 || !P->HeldTool[OffH].IsEmpty()))
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT the grab card: the %s hand put away what it held (weapon %d, tool \"%s\")"), OffH == 0 ? TEXT("left") : TEXT("right"), P->Equipped[OffH], *P->HeldTool[OffH]);
				P->Equip(OffH, -1);
				P->Pulse(OffH, 0.2f);
			}
			break;
		case Punch: ManFor(0, false); break;
		case GrabShoot:
			ManFor(1, bReset);
			// (REQ-TUTGRABFIST: the revolver put away from that hand is the one this card shoots with: back in the main hand, its
			// rounds with it, if neither hand has it now)
			if (One < 0 && !OldTutGrabFist() && P->Equipped[DomH] != RevolverIdx && P->Equipped[OffH] != RevolverIdx)
			{
				if (const int32* Rounds = P->SavedMag.Find(OffH * 16 + RevolverIdx)) P->SavedMag.Add(DomH * 16 + RevolverIdx, *Rounds);
				P->Equip(DomH, RevolverIdx);
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT the grab and shoot card: the revolver back in the main hand, %d rounds"), P->Mag[DomH]);
			}
			break;
		case Fight: ManFor(2, bReset); ManFor(3, bReset); break;
		case Heal:
			if (Health > 6.f)
			{
				Health = 6.f;                    // (hurt for the bag to mend: the fight's scratch, or a card skipped to)
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT health set to 6 for the Medical Bag card"));
			}
			if (Counts.FindRef(TEXT("idocbag")) <= 0) Counts.Add(TEXT("idocbag"), 1);
			break;
		case Pause: R.bMenuSeen = false; break;
		default: break;
		}
	};
	if (R.Card < 0)
	{
		// ---- set up: the street, the props, the kit ----
		const FVector S = Feet();
		R.Start = S;
		P->Equip(0, -1);
		P->Equip(1, -1);
		Counts.Reset();
		Counts.Add(TEXT("ipistol"), 1);
		Counts.Add(TEXT("ibullit"), 24);
		Counts.Add(TEXT("iknife"), 2);
		Counts.Add(TEXT("idynam"), 2);
		Counts.Add(TEXT("idocbag"), 3);
		Counts.Add(TEXT("ioil"), 150);
		Health = 10.f;
		Armour = 0;
		// the map's own pickups and anyone else gone from it (a multiplayer map: weapons and ammunition about)
		int32 Hidden = 0;
		for (FPickup& Pk : Pickups) if (AActor* A = Pk.Actor.Get()) { A->SetActorHiddenInGame(true); ++Hidden; }
		Pickups.Reset();
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies) if (AOutlawsEnemy* E = W.Get()) E->SetDormant(true);
		// the longest clear line from the start (eyes' height), the way the start faces first
		const float Yaw0 = float(P->GetActorRotation().Yaw);
		float BestYaw = Yaw0;
		double Best = -1e9, BestClear = 0.0;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutProbe), true, P);
		for (int32 k = 0; k < 24; ++k)
		{
			const float Off = (k % 2 == 0 ? 1.f : -1.f) * float((k + 1) / 2) * 15.f;
			const FVector D = FRotator(0.0, Yaw0 + Off, 0.0).Vector();
			FHitResult H;
			const FVector From = S + FVector(0.0, 0.0, 120.0);
			const double Clear = World->LineTraceSingleByChannel(H, From, From + D * 2600.0, ECC_Visibility, Q) ? H.Distance : 2600.0;
			const double Score = FMath::Min(Clear, 2200.0) - FMath::Abs(Off) * 2.0;
			if (Score > Best) { Best = Score; BestYaw = Yaw0 + Off; BestClear = Clear; }
		}
		R.Dir = FRotator(0.0, BestYaw, 0.0).Vector();
		R.Side = FRotator(0.0, BestYaw + 90.0, 0.0).Vector();
		R.Scale = FMath::Clamp(BestClear / 2000.0, 0.55, 1.0);
		R.StreetYaw = BestYaw;
		if (FMath::Abs(FMath::FindDeltaAngleDegrees(Yaw0, BestYaw)) > 1.f) P->TurnBy(FMath::FindDeltaAngleDegrees(Yaw0, BestYaw));
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT the street: from %s toward yaw %.0f (start faced %.0f), %.0f cm clear, scale %.2f; %d pickups hidden"),
			*S.ToCompactString(), BestYaw, Yaw0, BestClear, R.Scale, Hidden);
		const double K = R.Scale;
		auto At = [&](double Along, double Across) { return S + R.Dir * (Along * K) + R.Side * (Across * K); };
		const float StreetYaw = BestYaw;
		// (left of the bottles' line: the walk 16 Oct had the player stopped at it with the post straight between him and them)
		R.Marker = Block(At(500.0, -60.0), FVector(20.0, 20.0, 220.0), StreetYaw, true, TEXT("the walk's marker post"));
		// the bottles' rail, across the street ahead of the marker post, not solid (shots and throws pass under the bottles)
		R.Rail = Block(At(850.0, 120.0), FVector(12.0, 230.0, 100.0), StreetYaw, true, TEXT("the bottles' rail"));
		R.RailTop = R.Rail.Z + 100.0;
		for (int32 i = 0; i < NumBottles; ++i) R.BottleAt[i] = FVector(R.Rail.X, R.Rail.Y, R.RailTop) + R.Side * ((double(i) - 2.0) * 45.0 * K);
		SetBottles();
		R.BoxAt = At(640.0, 260.0);
		{ double Z = R.BoxAt.Z; if (Floor(R.BoxAt, Z)) R.BoxAt.Z = Z; }
		SetBox();
		// (the throws are made from about where the box lay, the card before: the post ahead of that spot and to its left, not
		// beside it, so that looking down the street it is in view; the walk 16 Oct had it 84 degrees off from there)
		R.KnifePost = Block(At(1000.0, 40.0), FVector(30.0, 30.0, 190.0), StreetYaw, false, TEXT("the knife's post"));
		{
			// the dynamite's barrel (user 2026-10-14, "no barrel to be seen": it stood 11 m off almost straight behind the knife's
			// post, and a plain wooden cube).  Now the game's own barrel, clear of the post and in plain sight from where it is
			// thrown (the box's spot, the card before the knife's): the first spot of a few that the eyes reach and that sits
			// well apart from the post
			const FVector Spot(R.BoxAt.X, R.BoxAt.Y, S.Z);
			const FVector Eye = Spot + FVector(0.0, 0.0, 150.0);
			const FVector PostWay = (R.KnifePost - Spot).GetSafeNormal2D();
			FCollisionQueryParams BQ(SCENE_QUERY_STAT(OutlawsTutBarrel), true, P);
			// (ahead and to the right of that spot, the post ahead and to its left: both within about 30 degrees of the street's
			// way from there, so a headset looking down the street has them both in view; the left side put it 60 degrees off)
			FVector BarrelAt = At(1150.0, 450.0);
			bool bFound = false;
			for (const double Along : { 1150.0, 1250.0, 1050.0 })
			{
				for (const double Across : { 450.0, 380.0, 520.0 })
				{
					FVector C = At(Along, Across);
					double Z = C.Z;
					if (!Floor(C, Z)) continue;
					C.Z = Z;
					FHitResult H;
					const FVector Mid = C + FVector(0.0, 0.0, 50.0);
					const bool bSeen = !World->LineTraceSingleByChannel(H, Eye, Mid, ECC_Visibility, BQ) || H.Distance > FVector::Dist(Eye, Mid) - 40.0;
					const double Apart = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(PostWay, (C - Spot).GetSafeNormal2D()), -1.0, 1.0)));
					if (bSeen && Apart > 12.0 && FMath::Abs(Z - S.Z) < 120.0) { BarrelAt = C; bFound = true; break; }
				}
				if (bFound) break;
			}
			R.Barrel = BarrelAt;
			for (const TCHAR* Name : { TEXT("barrel01"), TEXT("barrel02"), TEXT("barrel03") })
			{
				if (!AssetsRef) break;
				const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, Name);
				if (Anim.IsEmpty() || !AssetsRef->Has(Anim)) continue;
				if (AOutlawsSpriteActor* Bs = SpawnSceneSprite(FString(Name).ToUpper(), Anim, BarrelAt, StreetYaw, LightAt(BarrelAt + FVector(0.0, 0.0, 60.0))))
				{
					SpawnedActors.Add(Bs);
					R.BarrelActor = Bs;
					UE_LOG(LogOutlaws, Log, TEXT("OLTUT the dynamite's barrel (%s, %s) at %s, %s"), Name, *Anim, *BarrelAt.ToCompactString(), bFound ? TEXT("in sight from the box's spot") : TEXT("no clear spot found: the default"));
					break;
				}
			}
			if (!R.BarrelActor.IsValid()) Block(BarrelAt, FVector(70.0, 70.0, 95.0), StreetYaw, false, TEXT("the dynamite's barrel (no barrel sprite in this install)"));
		}
		R.Crates = Block(At(650.0, 420.0), FVector(150.0, 150.0, 165.0), StreetYaw, false, TEXT("the crates"));
		R.CrateTop = R.Crates.Z + 165.0;
		R.CrateHalf = 75.0;
		R.ManFrom = At(1050.0, 250.0);
		R.FightA = At(1700.0, -250.0);
		R.FightB = At(1800.0, 300.0);
		for (FVector* V : { &R.ManFrom, &R.FightA, &R.FightB }) { double Z = V->Z; if (Floor(*V, Z)) V->Z = Z; }
		// REQ-TUTDOORS (user 2026-11-04: "the tutorial takes me to open a jail cell door in the jail to open. i think it should take
		// us to open the two front doors on the opposite side of the street at the building next to the water well"): sectors
		// COURTHOUSEDOOR01 and 02 (the octagon beside that building is the well, its shaft 34 units down), named, not searched
		// for.  Each leaf is its own MORPH_SPIN2 elevator, no key, opened by a nudge from outside (EVENT_MASK 32), and their floors
		// stand at 10.25: a leaf is the wall under that raised floor, so the search below, within 150 cm of the start's floor,
		// passes them over, and the middle of the pair at the floor is read off the ground in front of them
		GTwoDoors = false;
		if (!OldTutDoors())
		{
			FVector Leaf[2];
			int32 Found = 0;
			for (const TCHAR* Name : { TEXT("COURTHOUSEDOOR01"), TEXT("COURTHOUSEDOOR02") })
			{
				for (const FOutlawsElevator& E : Inf->Elevators)
				{
					if (!E.IsMorph() || E.Sectors.Num() == 0 || !Inf->Live.Sectors.IsValidIndex(E.Sectors[0])) continue;
					const FOutlawsSector& Sec = Inf->Live.Sectors[E.Sectors[0]];
					if (!Sec.Name.Equals(Name, ESearchCase::IgnoreCase) || E.Stop != 0 || E.Target >= 0 || E.bTerminated || !E.bMasterOn || Sec.Verts.Num() < 3) continue;
					FVector2D C(0.0, 0.0);
					for (const FVector2D& V : Sec.Verts) C += V;
					C /= double(Sec.Verts.Num());
					Leaf[Found] = FOutlawsGeometry::ToUE(C, 0.0);
					(Found == 0 ? R.DoorSector : R.DoorSector2) = E.Sectors[0];
					++Found;
					break;
				}
			}
			if (Found == 2)
			{
				const FVector Mid = (Leaf[0] + Leaf[1]) * 0.5, Across = (Leaf[1] - Leaf[0]).GetSafeNormal2D();
				R.DoorOut = FVector(Across.Y, -Across.X, 0.0);
				if (FVector::DotProduct(R.DoorOut, S - Mid) < 0.0) R.DoorOut = -R.DoorOut;
				double Z = S.Z;
				Floor(FVector(Mid.X, Mid.Y, S.Z) + R.DoorOut * 120.0, Z);
				R.DoorAt = FVector(Mid.X, Mid.Y, Z);
				// (out past the frame, a leaf's face being set back from it, and over the door's top)
				R.DoorArrow = R.DoorAt + R.DoorOut * 60.0 + FVector(0.0, 0.0, 290.0);
				GTwoDoors = true;
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT the door card's doors: sectors %d and %d (%s, %s), the pair's middle at %s, %.0f cm from the start"), R.DoorSector, R.DoorSector2,
					*Inf->Live.Sectors[R.DoorSector].Name, *Inf->Live.Sectors[R.DoorSector2].Name, *R.DoorAt.ToCompactString(), FVector::Dist2D(R.DoorAt, S));
			}
			else
			{
				R.DoorSector = R.DoorSector2 = -1;
				UE_LOG(LogOutlaws, Warning, TEXT("OLTUT the two front doors are not in this level (%d of 2 found): the nearest door"), Found);
			}
		}
		// the door card's door: the street's nearest shut door (a swinging or sliding leaf, MORPH_*, or a rising one) on the
		// start's level, within 35 m; none, and the card is left out
		if (!GTwoDoors)
		{
			double BestD = 3500.0;
			for (const FOutlawsElevator& E : Inf->Elevators)
			{
				if ((!E.IsMorph() && E.Class != EOutlawsInfClass::MoveCeiling) || E.Sectors.Num() == 0 || !Inf->Live.Sectors.IsValidIndex(E.Sectors[0])) continue;
				if (E.Stop != 0 || E.Target >= 0 || E.bWaiting || E.bTerminated || !E.bMasterOn) continue;
				const FOutlawsSector& Sec = Inf->Live.Sectors[E.Sectors[0]];
				if (Sec.Verts.Num() < 3) continue;
				FVector2D C(0.0, 0.0);
				for (const FVector2D& V : Sec.Verts) C += V;
				C /= double(Sec.Verts.Num());
				const FVector W = FOutlawsGeometry::ToUE(C, Sec.Floor.Y);
				const double D = FVector::Dist2D(W, S);
				if (D < BestD && FMath::Abs(W.Z - S.Z) < 150.0) { BestD = D; R.DoorSector = E.Sectors[0]; R.DoorAt = W; }
			}
			R.DoorOut = (S - R.DoorAt).GetSafeNormal2D();
			R.DoorArrow = R.DoorAt + FVector(0.0, 0.0, 250.0);
			UE_LOG(LogOutlaws, Log, TEXT("OLTUT the door card's door: %s"), R.DoorSector >= 0 ? *FString::Printf(TEXT("sector %d (%s) at %s, %.0f cm from the start"), R.DoorSector,
				*Inf->Live.Sectors[R.DoorSector].Name, *R.DoorAt.ToCompactString(), BestD) : TEXT("none within 35 m: the card is left out"));
		}
		// the horse at the end of the street (a Historical Mission's, else the story's)
		R.HorseAt = At(1450.0, -60.0);
		{ double Z = R.HorseAt.Z; if (Floor(R.HorseAt, Z)) R.HorseAt.Z = Z; }
		for (const TCHAR* Name : { TEXT("horsend"), TEXT("hor1"), TEXT("hor2"), TEXT("hor3") })
		{
			if (!AssetsRef) break;
			const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, Name);
			if (Anim.IsEmpty() || !AssetsRef->Has(Anim)) continue;
			if (AOutlawsSpriteActor* Hs = SpawnSceneSprite(FString(Name).ToUpper(), Anim, R.HorseAt, 90.f, LightAt(R.HorseAt + FVector(0.0, 0.0, 100.0))))
			{
				SpawnedActors.Add(Hs);
				R.Horse = Hs;
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT the horse (%s, %s) at %s"), Name, *Anim, *R.HorseAt.ToCompactString());
				break;
			}
		}
		if (!R.Horse.IsValid())
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLTUT no horse sprite in this install: a stand-in block"));
			Block(R.HorseAt, FVector(220.0, 70.0, 160.0), StreetYaw + 90.f, false, TEXT("the horse's stand-in"));
		}
		// the card, and the floating marker (a red cone pointing down, in the lasers' full-bright red)
		{
			FActorSpawnParameters SP;
			SP.Owner = this;
			SP.ObjectFlags |= RF_Transient;
			R.CardActor = World->SpawnActor<AOutlawsTutorialCard>(AOutlawsTutorialCard::StaticClass(), FTransform::Identity, SP);
			if (R.CardActor.IsValid()) SpawnedActors.Add(R.CardActor.Get());
			static UStaticMesh* Cone = nullptr;
			if (!Cone) Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
			if (AActor* A = Cone ? World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(S), SP) : nullptr)
			{
				UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(A);
				M->SetStaticMesh(Cone);
				M->SetMobility(EComponentMobility::Movable);
				M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				M->SetCastShadow(false);
				A->SetRootComponent(M);
				M->RegisterComponent();
				if (UMaterialInterface* Red = P->LaserMaterial()) M->SetMaterial(0, Red);
				A->SetActorScale3D(FVector(0.22, 0.22, 0.30));
				A->SetActorHiddenInGame(true);
				SpawnedActors.Add(A);
				R.Arrow = A;
			}
		}
		Show(Walk, false);
		return;
	}
	if (R.Card >= NumCards) return;
	AOutlawsTutorialCard* CardA = R.CardActor.Get();
	// ---- Back and Next ----
	if (const int32 Pick = CardA ? CardA->TakePick() : -1; Pick >= 0)
	{
		if (Pick == 1)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLTUT Next on card %d %s (%s)"), R.Card + 1, CardIds[R.Card], R.bGood ? TEXT("done") : TEXT("not done: skipped ahead"));
			int32 Next = R.Card + 1;
			while (Next < NumCards && Skipped(P, &R, Next))
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT card %d %s left out (%s)"), Next + 1, CardIds[Next], Next == Door ? TEXT("no door near") : Next == Fan && One < 0 ? TEXT("-OLOldTutFan") : TEXT("one controller"));
				++Next;
			}
			if (Next >= NumCards)
			{
				Show(NumCards, false);
				UE_LOG(LogOutlaws, Log, TEXT("OLTUT all cards through, %d done, in %.0f s"), R.Done, Now - R.Began);
				EndTutorial(false);
				return;
			}
			Show(Next, false);
		}
		else
		{
			int32 Prev = R.Card - 1;
			while (Prev >= 0 && Skipped(P, &R, Prev)) --Prev;
			UE_LOG(LogOutlaws, Log, TEXT("OLTUT Back on card %d %s"), R.Card + 1, CardIds[R.Card]);
			if (Prev >= 0) Show(Prev, true);
		}
		return;
	}
	// the marker over the card's target, bobbing and turning
	if (AActor* Arrow = R.Arrow.Get())
	{
		FVector Over = FVector::ZeroVector;
		switch (R.Card)
		{
		case Walk: Over = R.Marker + FVector(0.0, 0.0, 220.0); break;
		case Fire: Over = FVector(R.Rail.X, R.Rail.Y, R.RailTop + 40.0); break;
		case Fan:
			// (REQ-TUTFAN: where the swipe goes: a small arrow riding the revolver's hammer, the gun's own hand or the one it is in)
			for (const int32 H : { DomH, OffH })
			{
				if (P->Equipped[H] == RevolverIdx && P->WeaponMesh[H]) { Over = P->WeaponMesh[H]->GetComponentTransform().TransformPosition(AOutlawsPlayer::WeaponFanZone(RevolverIdx).Hammer); break; }
			}
			break;
		case Pickup: Over = R.BoxAt + FVector(0.0, 0.0, 40.0); break;
		case TakeKnife: case ThrowKnife: Over = R.KnifePost + FVector(0.0, 0.0, 190.0); break;
		case TakeDynamite: case ThrowDynamite: Over = R.Barrel + FVector(0.0, 0.0, 130.0); break;
		case Climb: Over = R.Crates + FVector(0.0, 0.0, 165.0); break;
		case Door: Over = R.DoorArrow; break;
		case Horse: Over = R.HorseAt + FVector(0.0, 0.0, 230.0); break;
		default: break;
		}
		const bool bShow = !Over.IsZero() && !R.bGood;
		if (Arrow->IsHidden() == bShow) Arrow->SetActorHiddenInGame(!bShow);
		if (bShow)
		{
			const double T = Now - R.CardAt;
			const double Size = R.Card == Fan ? 0.3 : 1.0;
			Arrow->SetActorScale3D(FVector(0.22 * Size, 0.22 * Size, 0.30 * Size));
			Arrow->SetActorLocationAndRotation(Over + FVector(0.0, 0.0, (30.0 + 10.0 * FMath::Sin(T * 3.0)) * Size), FRotator(180.0, FMath::Fmod(T * 90.0, 360.0), 0.0));
		}
	}
	// the bad guy a card is about: held still once he is near (or has had 3.5 s to come), going again once taken
	for (int32 m = 0; m < 2; ++m)
	{
		AOutlawsEnemy* E = R.Men[m].Get();
		if (!E || E->IsDead()) continue;
		const bool bHeldNow = P->GrabbedEnemy[0].Get() == E || P->GrabbedEnemy[1].Get() == E;
		if (bHeldNow) R.bHeld[m] = true;
		if (!R.bFrozen[m] && !bHeldNow && (FVector::Dist2D(E->GetActorLocation(), P->GetActorLocation()) < 300.0 || Now - R.CardAt > 3.5))
		{
			R.bFrozen[m] = true;
			E->CustomTimeDilation = 0.0001f;
			UE_LOG(LogOutlaws, Log, TEXT("OLTUT %s holds still %.0f cm off"), *E->GetName(), FVector::Dist2D(E->GetActorLocation(), P->GetActorLocation()));
		}
		if (R.bFrozen[m] && bHeldNow)
		{
			R.bFrozen[m] = false;
			E->CustomTimeDilation = 1.f;
		}
	}
	if (R.bGood) return;
	const AOutlawsPlayer::FTutorialNotes& N = P->TutNotes;
	const AOutlawsPlayer::FTutorialNotes& A = R.At;
	auto Holds = [P](int32 Wi) { return P->Equipped[0] == Wi || P->Equipped[1] == Wi; };
	auto Dead = [&R](int32 m) { return !R.Men[m].IsValid() || R.Men[m]->IsDead(); };
	bool bDone = false;
	switch (R.Card)
	{
	case Walk: bDone = FVector::Dist2D(Feet(), R.Marker) < 160.0; break;
	case Turn:
	{
		const float Yaw = P->Camera ? float(P->Camera->GetComponentRotation().Yaw) : float(P->GetActorRotation().Yaw);
		R.TurnSum += FMath::Abs(FMath::FindDeltaAngleDegrees(R.LastYaw, Yaw));
		R.LastYaw = Yaw;
		bDone = R.TurnSum >= 60.f;
		break;
	}
	case TakeRevolver: bDone = N.WheelTakes[0] + N.WheelTakes[1] > A.WheelTakes[0] + A.WheelTakes[1] && Holds(RevolverIdx); break;
	case Fire:
	{
		bDone = true;
		for (int32 i = 0; i < NumBottles; ++i) bDone = bDone && BottleDown(i);
		break;
	}
	case Fan: bDone = N.FanShots > A.FanShots; break;           // (one is enough; the trigger's shots do not count)
	case Reload: bDone = P->bButtonReload[0] || P->bButtonReload[1] || P->bReloadingNow[0] || P->bReloadingNow[1]; break;
	case Pickup: bDone = BoxTaken(); break;
	case TakeKnife: bDone = Holds(KnifeIdx); break;
	case ThrowKnife: bDone = N.Throws > A.Throws; break;
	case TakeDynamite: bDone = Holds(DynamiteIdx); break;
	case ThrowDynamite: bDone = N.LitThrows > A.LitThrows; break;
	case Climb:
	{
		const FVector F = Feet();
		const FVector Rel = F - R.Crates;
		bDone = F.Z > R.CrateTop - 25.0 && FMath::Abs(FVector::DotProduct(Rel, R.Dir)) < R.CrateHalf + 30.0 && FMath::Abs(FVector::DotProduct(Rel, R.Side)) < R.CrateHalf + 30.0
			&& !P->IsClimbing();
		break;
	}
	case Jump:
	{
		// (user 2026-10-14: a card that asks for two things waits for both) two controllers: a crouch (the stick held down,
		// or the head 35 cm under where it was when the card came up) and a jump, either order; one controller: the jump
		if ((P->bCrouchHeld && !P->bAutoCrouched) || P->HeadHeight() < R.JumpHead0 - 35.f) R.bCrouchSeen = true;
		if (N.Jumps > A.Jumps) R.bJumpSeen = true;
		bDone = R.bJumpSeen && (One >= 0 || R.bCrouchSeen);
		break;
	}
	case Door: bDone = DoorOpen(); break;
	case Lantern: bDone = bLampOn; break;
	case Grab:
		// (one controller: taken, then let go of or thrown)
		bDone = One >= 0 ? (R.bHeld[0] && P->GrabbedEnemy[0].Get() != R.Men[0].Get() && P->GrabbedEnemy[1].Get() != R.Men[0].Get()) || Dead(0) : R.bHeld[0] || Dead(0);
		break;
	case Punch: bDone = N.HeldPunches > A.HeldPunches || Dead(0); break;
	case GrabShoot: bDone = Dead(1) && (R.bHeld[1] || N.ShotsHolding > A.ShotsHolding); break;
	case Fight: bDone = Dead(2) && Dead(3); break;
	case Heal: bDone = N.BagsUsed > A.BagsUsed; break;           // (only the bag: health coming back by itself does not count)
	case Pause:
		if (P->bMenuOpen) R.bMenuSeen = true;
		bDone = R.bMenuSeen;
		break;
	case Horse:
	{
		if (!R.Horse.IsValid() && R.HorseAt.IsZero()) { bDone = true; break; }
		for (int32 H = 0; H < 2 && !bDone; ++H)
		{
			const FVector G = P->GripOf(H)->GetComponentLocation();
			const bool bGrip = P->Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")) > 0.5f;
			bDone = bGrip && FVector::Dist2D(G, R.HorseAt) < 170.0 && G.Z > R.HorseAt.Z + 30.0 && G.Z < R.HorseAt.Z + 260.0;
		}
		break;
	}
	default: break;
	}
	if (!bDone) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLTUT card %d %s done after %.1f s (at %.1f s): Good."), R.Card + 1, CardIds[R.Card], Now - R.CardAt, Now - R.Began);
	++R.Done;
	R.bGood = true;
	if (CardA) CardA->SetGood(true);
	PlayMenuSound(TEXT("cool2"));
	if ((R.Card == Punch || (R.Card == Grab && One >= 0)) && R.Men[0].IsValid() && !R.Men[0]->IsDead())
	{
		// (a punch, or one controller's throw, that did not drop him: the card has done its job, he drops now)
		AOutlawsEnemy::GNextHitMelee = true;
		R.Men[0]->TakeHit(5.f, R.Men[0]->GetActorLocation(), R.Dir, R.Men[0]->GetActorLocation() - R.Dir * 30.0);
	}
	if (R.Card == Fight && Health > 6.f)
	{
		Health = 6.f;                            // (the fight leaves the player hurt, for the Medical Bag card)
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT the fight leaves the player hurt: health 6"));
	}
	if (R.Card == Horse)
	{
		// (riding out is the tutorial's end, as the level-end grip is everywhere)
		Show(NumCards, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT rode out: %d cards done in %.0f s"), R.Done, Now - R.Began);
		EndTutorial(false);
	}
}

// ---- the player's test hooks ---------------------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
bool AOutlawsPlayer::TutorialTestWheelTake(int32 Hand, int32 Weapon)
{
	const int32 H = Hand & 1;
	if (!bWheelOpen || !bWheelHeld[H]) OpenWheel(H);
	if (!bWheelOpen) return false;
	bWheelHeld[H] = true;
	const int32 Slot = WheelWeapon.IndexOfByKey(Weapon);
	if (Slot == INDEX_NONE)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT test: weapon %d is not on the wheel"), Weapon);
		CloseWheel();
		return false;
	}
	WheelPick[H] = Slot;
	ReleaseWheel(H);
	return Equipped[H] == Weapon;
}

bool AOutlawsPlayer::TutorialTestWheelItem(int32 Hand, const FString& Item)
{
	const int32 H = Hand & 1;
	if (!bWheelOpen || !bWheelHeld[H]) OpenWheel(H);
	if (!bWheelOpen) return false;
	bWheelHeld[H] = true;
	int32 Slot = INDEX_NONE;
	for (int32 i = 0; i < WheelWeapon.Num() && Slot == INDEX_NONE; ++i)
	{
		if (WheelWeapon[i] == -2 && WheelItem.IsValidIndex(i) && WheelItem[i].Equals(Item, ESearchCase::IgnoreCase)) Slot = i;
	}
	if (Slot == INDEX_NONE)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLTUT test: %s is not on the wheel"), *Item);
		CloseWheel();
		return false;
	}
	WheelPick[H] = Slot;
	ReleaseWheel(H);
	return true;
}

bool AOutlawsPlayer::TutorialTestPressLabel(const FString& Label)
{
	if (!bMenuOpen || !MenuPage) return false;
	const int32 i = MenuPage->Items.IndexOfByPredicate([&Label](const OutlawsMenu::FItem& I) { return I.Label.Equals(Label, ESearchCase::IgnoreCase); });
	if (i == INDEX_NONE) return false;
	MenuActivate(1, i);
	return true;
}

FString AOutlawsPlayer::TutorialTestRows() const
{
	FString Rows;
	if (!bMenuOpen || !MenuPage) return Rows;
	for (const OutlawsMenu::FItem& I : MenuPage->Items) Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(" | ")) + I.Label;
	return Rows;
}

bool AOutlawsPlayer::TutorialTestPunchHeld(int32 Hand)
{
	AOutlawsLevelActor* L = Level.Get();
	const int32 H = Hand & 1;
	AOutlawsEnemy* Held = GrabbedEnemy[1 - H].Get();
	if (!L || !Held || Held->IsDead()) return false;
	// (UpdatePunches' blow on the one held in the other hand: a fist from in front, into his middle)
	const FVector Fist = GripOf(1 - H)->GetComponentLocation() - FVector(0.0, 0.0, 20.0);
	const FVector Dir = Camera ? Camera->GetForwardVector() : GetActorForwardVector();
	const float Damage = 0.8f * FMath::FRandRange(0.9f, 1.1f);
	AOutlawsEnemy::GNextHitMelee = true;
	++TutNotes.HeldPunches;
	Held->TakeHit(Damage * L->PlayerDamageScale(), Fist, Dir, Fist - Dir * 30.f);
	L->PlaySoundAt(TEXT("connect"), Fist);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s punch (fist, held): %.2f"), H == 0 ? TEXT("left") : TEXT("right"), Damage);
	return true;
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLTutorialTest ----------------------------------------------------------------------------------------------------

namespace OutlawsTutorialLocal
{
	struct FTest
	{
		int32 Phase = 0;
		double At = 0.0;                    // real seconds: the next step not before
		double CardSeen = 0.0;
		FString CardId;                     // the card the test is working on
		int32 Tries = 0;
		int32 Passes = 0, Fails = 0;
		int32 MenuStep = 0;
		int32 LastStep = 0;                 // the step the card before showed
		bool bDone = false;
		bool bGoodSeen = false;
		FString PendingShot;      // (a card's shot, taken a step after the view was turned to its target: no smear)
		bool bSkipNext = false;             // (skip) Next pressed on the first card before its action
		FString BackFrom;                   // Back pressed on this card: the card before is checked for its station set up again
		TWeakObjectPtr<AActor> ManBefore;   // (the grab card's bad guy when Back was pressed)
		TSet<FString> Backed;
		TSet<FString> Shot;
		int32 Job = 0;                      // (the one-job modes, phase 30 on: the step they are at)
		int32 JobTick = 0;
	};
	FTest GTest;
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickTutorialTest(float Dt)
{
	using namespace OutlawsTutorialLocal;
	FString Mode;
	if (!OLFlags::Value(TEXT("OLTutorialTest"), Mode)) return;
	FTest& T = GTest;
	if (T.bDone) return;
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P) return;
	const double Now = FPlatformTime::Seconds();
	if (T.At == 0.0)
	{
		T.At = Now + 4.0;
		// (the test turns the view at once before a card's shot: the desktop's motion blur smeared the card; a headset has none)
		if (IConsoleVariable* MB = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MotionBlurQuality"))) MB->Set(0, ECVF_SetByCode);
	}
	if (Now < T.At) return;
	auto Check = [&T](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++T.Passes; else ++T.Fails;
	};
	auto Finish = [&T, &Mode]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test %s RESULT %s (%d passed, %d failed)"), *Mode, T.Fails == 0 && T.Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), T.Passes, T.Fails);
		T.bDone = true;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Shot = [&T](const FString& Name)
	{
		if (T.Shot.Contains(Name)) return;
		T.Shot.Add(Name);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_tut_%s.png"), *Name), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: screenshot ol_tut_%s.png"), *Name);
	};
	const bool bFrontMenu = bFrontEnd && !bRebuildPending && !bNeedPlace && P->bMenuOpen && P->MenuPage && P->MenuPage->File == TEXT("#front");
	const FString Page = P->bMenuOpen && P->MenuPage ? P->MenuPage->File : FString();
	auto Cam = [P]() { return P->Camera ? P->Camera->GetComponentLocation() : P->GetActorLocation(); };
	// a hand put in front of the eyes and turned to Target (a desktop has no tracked hand, as the co-op tests put it)
	auto Aim = [P, &Cam](int32 H, const FVector& Target)
	{
		const FVector From = Cam() + (P->Camera ? P->Camera->GetForwardVector() : P->GetActorForwardVector()) * 35.0 + FVector(0.0, 0.0, -12.0)
			+ (P->Camera ? P->Camera->GetRightVector() : P->GetActorRightVector()) * (H == 0 ? -14.0 : 14.0);
		P->GripOf(H)->SetWorldLocationAndRotation(From, (Target - From).Rotation());
	};
	// the gun fired at Target: what the shot did logged (the rounds, and what the bore's line meets)
	auto FireAt = [P, World](int32 H, const FVector& Target)
	{
		const int32 Mag0 = P->Mag[H], Shots0 = P->TutNotes.Shots;
		FString Meets = TEXT("nothing");
		if (UProceduralMeshComponent* Gun = P->WeaponMesh[H])
		{
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutAim), true, P);
			const FVector From = Gun->GetComponentLocation();
			if (World->LineTraceSingleByChannel(Hit, From, From + (Target - From).GetSafeNormal() * 3000.0, ECC_Visibility, Q))
			{
				Meets = FString::Printf(TEXT("%s at %.0f cm"), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?"), Hit.Distance);
			}
		}
		P->FireCooldown[H] = 0.f;            // (the desktop runs no hand's weapon tick: the last shot's cycle never runs down)
		P->FireGun(H, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: fired (rounds %d -> %d, shots +%d, cooldown %.2f); the line to the target meets %s"), Mag0, P->Mag[H], P->TutNotes.Shots - Shots0,
			P->FireCooldown[H], *Meets);
	};
	auto Face = [P](const FVector& Target)
	{
		if (AController* C = P->GetController()) C->SetControlRotation(FRotator(0.0, (Target - P->GetActorLocation()).Rotation().Yaw, 0.0));
	};
	auto PutFeet = [P](const FVector& FeetAt, const FVector& Look)
	{
		P->TeleportTo(FeetAt + FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, (Look - FeetAt).Rotation().Yaw, 0.0), false, true);
	};
	switch (T.Phase)
	{
	case 0:
		// the title menu up
		if (!bFrontMenu) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test %s: the title menu: %s"), *Mode, *P->TutorialTestRows());
		if (Mode == TEXT("menu"))
		{
			// (REQ-TITLEMENU) Load Game and New Game, then Options and Exit; Load Game greyed out with no save to load
			const FString TitleRows = P->TutorialTestRows();
			Check(TitleRows == TEXT("Load Game | New Game | Extras | Options | Exit"), FString::Printf(TEXT("the title menu reads Load Game, New Game, Extras, Options, Exit (%s)"), *TitleRows));
			const bool bGrey = P->MenuPage->Items.Num() > 0 && P->MenuPage->Items[0].Classes.Contains(TEXT("disabled"));
			Check(bGrey == !AOutlawsPlayer::AnySaveToLoad(), FString::Printf(TEXT("Load Game is %s (a save %s)"), bGrey ? TEXT("greyed out") : TEXT("pressable"), AOutlawsPlayer::AnySaveToLoad() ? TEXT("there") : TEXT("absent")));
		}
		Check(P->TutorialTestPressLabel(TEXT("New Game")), TEXT("New Game pressed on the title menu"));
		T.Phase = 1;
		T.At = Now + 1.0;
		return;
	case 1:
	{
		const FString Rows = P->TutorialTestRows();
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: New Game's rows: %s"), *Rows);
		Check(Page == TEXT("#newgame"), FString::Printf(TEXT("New Game's page is up (%s)"), *Page));
		// (REQ-MPMENU, user 15 Oct: Multiplayer third, the Tutorial last)
		Check(Rows == TEXT("Main Game | Historical Missions | Multiplayer | Tutorial"), FString::Printf(TEXT("New Game reads Main Game, Historical Missions, Multiplayer, Tutorial (%s)"), *Rows));
		Shot(TEXT("newgame"));
		T.Phase = Mode == TEXT("menu") ? 10 : 2;
		T.At = Now + 1.0;
		return;
	}
	case 2:
		Check(P->TutorialTestPressLabel(TEXT("Tutorial")), TEXT("Tutorial pressed"));
		T.Phase = 3;
		T.At = Now + 0.5;
		T.CardSeen = Now;
		return;
	case 3:
	{
		// the first card up in town_2p
		const FString Id = TutorialCardId();
		if (Id.IsEmpty())
		{
			if (Now - T.CardSeen > 90.0) { Check(false, TEXT("the tutorial's first card came up within 90 s")); Finish(); }
			return;
		}
		Check(bTutorial && LevelName.Equals(TEXT("town_2p"), ESearchCase::IgnoreCase) && !bFrontEnd, FString::Printf(TEXT("the tutorial is up in %s"), *LevelName));
		const AOutlawsTutorialCard* C = TutorialRun->CardActor.Get();
		const int32 Want = StepsOf(P, TutorialRun.Get(), NumCards);
		Check(Id == CardIds[0] && C && C->IsUp() && C->GetLine() == TutorialRun->Line && C->GetStep() == 1 && C->GetSteps() == Want,
			FString::Printf(TEXT("the first card (%s) is up: step %d of %d (%d wanted): \"%s\""), *Id, C ? C->GetStep() : 0, C ? C->GetSteps() : 0, Want, C ? *C->GetLine() : TEXT("")));
		Check(C && !C->IsGood(), TEXT("the first card waits for its action (no tick, Next not lit)"));
		T.CardId = Id;
		T.CardSeen = Now;
		T.Tries = 0;
		T.LastStep = 1;
		T.bGoodSeen = false;
		T.Phase = Mode == TEXT("float") ? 30 : Mode == TEXT("doors") ? 31 : Mode == TEXT("fan") ? 32 : Mode == TEXT("grabfist") ? 33 : Mode == TEXT("gunfire") ? 34 : 4;
		T.At = Now + 1.5;
		return;
	}
	case 33:
	{
		// REQ-TUTGRABFIST (-OLTutorialTest=grabfist): the card before the grab, the other hand given something, Next: a lit stick,
		// a knife, the lantern; each out of that hand, nothing dropped or spent, the fuse out, the main hand untouched; Back into
		// the card from the punch does the same; the revolver in that hand with the main hand bare: gone from it at the grab, in
		// the main hand with its rounds at grab_shoot.  -OLOneController= and -OLOldTutGrabFist: the hand keeps what it holds
		FOutlawsTutorialRun& R = *TutorialRun;
		AOutlawsTutorialCard* C = R.CardActor.Get();
		if (!C) { Check(false, TEXT("grabfist: the card is there")); Finish(); return; }
		const FString Id = TutorialCardId();
		const int32 One = P->OneHand();
		const int32 DomH = One >= 0 ? One : P->LeftDominant() ? 0 : 1;
		const int32 OffH = One >= 0 ? One : 1 - DomH;
		T.At = Now + 0.05;
		if (Now - T.CardSeen > 60.0) { Check(false, FString::Printf(TEXT("grabfist: stuck at step %d on %s"), T.Job, *Id)); Finish(); return; }
		auto Bare = [&](const TCHAR* What)
		{
			Check(P->Equipped[OffH] < 0 && P->HeldTool[OffH].IsEmpty() && R.Line.Contains(OffH == 0 ? TEXT("left") : TEXT("right")),
				FString::Printf(TEXT("%s: the hand the card names is empty (weapon %d, tool \"%s\"): \"%s\""), What, P->Equipped[OffH], *P->HeldTool[OffH], *R.Line));
		};
		auto MainKept = [&](const TCHAR* What)
		{
			Check(P->Equipped[DomH] == RevolverIdx && P->Mag[DomH] == 5, FString::Printf(TEXT("%s: the main hand untouched (weapon %d, %d rounds)"), What, P->Equipped[DomH], P->Mag[DomH]));
		};
		switch (T.Job)
		{
		case 0:
			if (Id != TEXT("lantern")) { C->Pick(1); T.At = Now + 0.15; return; }
			if (One >= 0 || OLFlags::Param(TEXT("OLOldTutGrabFist")))
			{
				P->Equip(OffH, KnifeIdx);
				C->Pick(1);
				T.Job = 1;
				return;
			}
			P->Equip(DomH, RevolverIdx);
			P->Mag[DomH] = 5;
			P->Equip(OffH, DynamiteIdx);
			P->bFuseLit[OffH] = true;
			P->Fuse[OffH] = 4.5f;
			T.Tries = P->Thrown.Num();
			C->Pick(1);
			T.Job = 2;
			return;
		case 1:
			if (Id != TEXT("grab")) return;
			Check(P->Equipped[OffH] == KnifeIdx, FString::Printf(TEXT("%s: the hand keeps its knife (weapon %d)"), One >= 0 ? TEXT("one controller") : TEXT("old"), P->Equipped[OffH]));
			Finish();
			return;
		case 2:
			if (Id != TEXT("grab")) return;
			Bare(TEXT("a lit stick"));
			Check(!P->bFuseLit[OffH] && Counts.FindRef(TEXT("idynam")) == 2 && P->Thrown.Num() == T.Tries, FString::Printf(TEXT("the fuse is out, the stick not lost (%d in the count, %d thrown)"), Counts.FindRef(TEXT("idynam")), P->Thrown.Num() - T.Tries));
			MainKept(TEXT("a lit stick"));
			C->Pick(0);
			T.Job = 3;
			return;
		case 3:
			if (Id != TEXT("lantern")) return;
			P->Equip(OffH, KnifeIdx);
			C->Pick(1);
			T.Job = 4;
			return;
		case 4:
			if (Id != TEXT("grab")) return;
			Bare(TEXT("a knife"));
			Check(Counts.FindRef(TEXT("iknife")) == 2, FString::Printf(TEXT("both knives still counted (%d)"), Counts.FindRef(TEXT("iknife"))));
			C->Pick(0);
			T.Job = 5;
			return;
		case 5:
			if (Id != TEXT("lantern")) return;
			P->ToggleGear(OffH, TEXT("ilantern"));
			Check(P->HeldTool[OffH] == TEXT("ilantern"), TEXT("the lantern in that hand"));
			T.Tries = Counts.FindRef(TEXT("ioil"));
			C->Pick(1);
			T.Job = 6;
			return;
		case 6:
			if (Id != TEXT("grab")) return;
			Bare(TEXT("the lantern"));
			Check(Counts.FindRef(TEXT("ioil")) == T.Tries, FString::Printf(TEXT("the oil kept (%d)"), Counts.FindRef(TEXT("ioil"))));
			MainKept(TEXT("the lantern"));
			C->Pick(1);
			T.Job = 7;
			return;
		case 7:
			if (Id != TEXT("punch")) return;
			P->Equip(OffH, KnifeIdx);
			C->Pick(0);
			T.Job = 8;
			return;
		case 8:
			if (Id != TEXT("grab")) return;
			Bare(TEXT("Back into the card"));
			C->Pick(0);
			T.Job = 9;
			return;
		case 9:
			if (Id != TEXT("lantern")) return;
			P->Equip(DomH, -1);
			P->Equip(OffH, RevolverIdx);
			P->Mag[OffH] = 4;
			C->Pick(1);
			T.Job = 10;
			return;
		case 10:
			if (Id != TEXT("grab")) return;
			Bare(TEXT("the revolver"));
			Check(P->Equipped[DomH] < 0, TEXT("the main hand left alone, bare"));
			C->Pick(1);
			T.Job = 11;
			return;
		case 11:
			if (Id != TEXT("punch")) return;
			C->Pick(1);
			T.Job = 12;
			return;
		case 12:
			if (Id != TEXT("grab_shoot")) return;
			Check(P->Equipped[DomH] == RevolverIdx && P->Mag[DomH] == 4 && P->Equipped[OffH] < 0, FString::Printf(TEXT("grab_shoot: the revolver in the main hand with its rounds (weapon %d, %d rounds), the other hand free"), P->Equipped[DomH], P->Mag[DomH]));
			Finish();
			return;
		default:
			return;
		}
	}
	case 32:
	{
		// REQ-TUTFAN (-OLTutorialTest=fan): Next to the fire card with no gun drawn, on to the fan card: its station (the revolver,
		// the other hand empty, the rounds, the bottles, the arrow on the hammer, the card straight after fire); Back and a
		// cylinder run dry with the knife in the free hand: the station again.  Then the headset's rules and a hand really swiped
		// past the hammer (UpdateFan): sideways nothing, palm up nothing, back across with the trigger left alone a fanned shot,
		// the card done; with the trigger held another (the manual's way works too, it just is not needed); the reload card
		// after it has rounds to put back; Back from it, the lantern in the free hand, puts the station up again.
		// -OLOldTutFan: no fan card, the steps as they were
		FOutlawsTutorialRun& R = *TutorialRun;
		AOutlawsTutorialCard* C = R.CardActor.Get();
		if (!C) { Check(false, TEXT("fan: the card is there")); Finish(); return; }
		const FString Id = TutorialCardId();
		const int32 One = P->OneHand();
		const int32 DomH = One >= 0 ? One : P->LeftDominant() ? 0 : 1;
		const int32 OffH = 1 - DomH;
		T.At = Now + 0.05;
		if (Now - T.CardSeen > 60.0) { Check(false, FString::Printf(TEXT("fan: stuck at step %d on %s"), T.Job, *Id)); Finish(); return; }
		auto BottlesUp = [&]()
		{
			int32 Up = 0;
			for (int32 i = 0; i < NumBottles; ++i)
			{
				const AOutlawsSpriteActor* B = R.Bottles[i].Get();
				const FProp* Pr = B ? Props.FindByPredicate([B](const FProp& X) { return X.Sprite.Get() == B; }) : nullptr;
				Up += B && !B->IsHidden() && Pr && Pr->Current == 0 ? 1 : 0;
			}
			return Up;
		};
		// the station's checks (What: how the card came up)
		auto Station = [&](const TCHAR* What, int32 RoundsWanted)
		{
			Check(Id == TEXT("fan") && P->Equipped[DomH] == RevolverIdx && P->Equipped[OffH] < 0 && P->HeldTool[OffH].IsEmpty() && P->Mag[DomH] == RoundsWanted && BottlesUp() == NumBottles,
				FString::Printf(TEXT("%s: the revolver in the main hand, %d rounds (%d wanted), the other hand empty, %d bottles up"), What, P->Mag[DomH], RoundsWanted, BottlesUp()));
		};
		// the gun held out in front, the free hand's palm start and step along Way each frame
		auto GunXf = [P, DomH]() { return P->WeaponMesh[DomH] ? P->WeaponMesh[DomH]->GetComponentTransform() : FTransform::Identity; };
		auto HoldGun = [&]()
		{
			const FVector Fwd = P->Camera->GetForwardVector();
			P->GripOf(DomH)->SetWorldLocationAndRotation(Cam() + Fwd * 40.0 - FVector(0.0, 0.0, 25.0), FRotator(0.0, Fwd.Rotation().Yaw, 0.0));
		};
		auto Hammer = [&]() { return GunXf().TransformPosition(AOutlawsPlayer::WeaponFanZone(RevolverIdx).Hammer); };
		// (one stroke, 8 frames still at its start for the hand's speed to settle, then 16 along Way through the point By at 150
		// cm/s; Roll turns the palm)
		auto Stroke = [&](const FVector& Way, const FVector& By, float Roll)
		{
			const double Step = FMath::Max(double(Dt), 0.008) * 150.0;
			const FVector At = By + Way * (double(FMath::Max(T.JobTick - 8, 0)) - 8.0) * Step;
			P->GripOf(OffH)->SetWorldLocationAndRotation(At, FRotator(0.0, P->GetActorRotation().Yaw, Roll));
			++T.JobTick;
			T.At = Now;
			return T.JobTick >= 24;
		};
		switch (T.Job)
		{
		case 0:
			if (Id != TEXT("fire")) { C->Pick(1); T.At = Now + 0.15; return; }
			Check(P->Equipped[0] < 0 && P->Equipped[1] < 0, TEXT("the fire card reached by Next, no gun drawn"));
			C->Pick(1);
			T.Job = 1;
			T.At = Now + 0.4;
			return;
		case 1:
		{
			if (OLFlags::Param(TEXT("OLOldTutFan")))
			{
				Check(Id == TEXT("reload") && C->GetSteps() == NumCards - 1, FString::Printf(TEXT("old: reload follows fire (%s), %d steps"), *Id, C->GetSteps()));
				Finish();
				return;
			}
			Check(Id == TEXT("fan") && C->GetStep() == 5 && C->GetSteps() == NumCards, FString::Printf(TEXT("the fan card straight after fire: step %d of %d: \"%s\" / \"%s\""), C->GetStep(), C->GetSteps(), *C->GetLine(), *C->GetHint()));
			Check(C->GetLine().Contains(DomH == 1 ? TEXT("left hand") : TEXT("right hand")), TEXT("it names the free hand"));
			Station(TEXT("no gun drawn before"), 6);
			Check(!C->IsGood(), TEXT("waiting for a fanned shot"));
			Face(R.Rail);
			T.At = Now + 0.6;
			T.Job = 2;
			return;
		}
		case 2:
		{
			const FVector To = R.Arrow.IsValid() ? R.Arrow->GetActorLocation() : FVector::ZeroVector;
			Check(R.Arrow.IsValid() && !R.Arrow->IsHidden() && FVector::Dist(To, Hammer()) < 20.0 && R.Arrow->GetActorScale3D().X < 0.1, FString::Printf(TEXT("a small arrow on the hammer (%.0f cm off it, scale %.2f)"), FVector::Dist(To, Hammer()), R.Arrow.IsValid() ? R.Arrow->GetActorScale3D().X : 0.0));
			C->Pick(0);
			T.Job = 3;
			T.At = Now + 0.3;
			return;
		}
		case 3:
			if (Id != TEXT("fire")) return;
			// (the fire card's misses: the cylinder dry, then the knife drawn in the free hand)
			P->Mag[DomH] = 0;
			P->Spent[DomH] = 6;
			P->Equip(OffH, KnifeIdx);
			T.Tries = Counts.FindRef(TEXT("ibullit"));
			C->Pick(1);
			T.Job = 4;
			T.At = Now + 0.4;
			return;
		case 4:
			if (Id != TEXT("fan")) return;
			Station(TEXT("a dry cylinder, the knife out"), FanRounds);
			Check(Counts.FindRef(TEXT("ibullit")) == T.Tries - FanRounds && P->Spent[DomH] == 6 - FanRounds && Counts.FindRef(TEXT("iknife")) == 2,
				FString::Printf(TEXT("the rounds came from the reserve (%d -> %d, %d cases), the knife kept (%d)"), T.Tries, Counts.FindRef(TEXT("ibullit")), P->Spent[DomH], Counts.FindRef(TEXT("iknife"))));
			// the headset's rules from here: a hand's weapon tick, the swipes
			P->bVR = true;
			HoldGun();
			T.Job = 5;
			T.At = Now + 0.5;
			return;
		case 5:
			Shot(TEXT("fan"));
			T.JobTick = 0;
			T.Tries = P->TutNotes.Shots;
			T.Job = 6;
			return;
		case 6:
		{
			// sideways past the hammer
			HoldGun();
			const FTransform X = GunXf();
			if (!Stroke(X.GetUnitAxis(EAxis::Y), Hammer(), 0.f)) return;
			Check(P->TutNotes.Shots == T.Tries, FString::Printf(TEXT("a sideways brush does not fan (%d shots)"), P->TutNotes.Shots - T.Tries));
			T.JobTick = 0;
			T.Job = 7;
			return;
		}
		case 7:
		{
			// back across it, the palm up (cupped under the gun, holding it up)
			HoldGun();
			const FTransform X = GunXf();
			if (!Stroke(-X.GetUnitAxis(EAxis::X), Hammer() - X.GetUnitAxis(EAxis::Z) * 2.0, OffH == 0 ? -90.f : 90.f)) return;
			Check(P->TutNotes.Shots == T.Tries, FString::Printf(TEXT("back across it palm up does not fan (palm %.2f up, %d shots)"), FVector::DotProduct(P->PalmDir(OffH), FVector::UpVector), P->TutNotes.Shots - T.Tries));
			Check(!C->IsGood(), TEXT("the card still waits"));
			T.JobTick = 0;
			T.Job = 8;
			return;
		}
		case 8:
		{
			// (the palm level again, a frame or two for the band to let it go)
			HoldGun();
			P->GripOf(OffH)->SetWorldLocationAndRotation(Hammer() + P->Camera->GetRightVector() * 60.0 + FVector(0.0, 0.0, 30.0), FRotator(0.0, P->GetActorRotation().Yaw, 0.f));
			T.JobTick = 0;
			T.Job = 9;
			T.At = Now + 0.3;
			return;
		}
		case 9:
		{
			// back across it, the trigger not touched
			HoldGun();
			const FTransform X = GunXf();
			if (!Stroke(-X.GetUnitAxis(EAxis::X), Hammer() - X.GetUnitAxis(EAxis::Z) * 2.0, 0.f) && !C->IsGood()) return;
			Check(P->TutNotes.FanShots == R.At.FanShots + 1 && P->Mag[DomH] == FanRounds - 1, FString::Printf(TEXT("back across the hammer, the trigger not touched: a fanned shot (%d, %d rounds left)"), P->TutNotes.FanShots - R.At.FanShots, P->Mag[DomH]));
			Check(C->IsGood(), TEXT("one is enough: the card is done"));
			T.JobTick = 0;
			T.Job = 10;
			T.At = Now + 0.4;
			return;
		}
		case 10:
		{
			// the manual's way, the trigger held through the stroke: a second one (the pull's own shot goes first)
			HoldGun();
			const FTransform X = GunXf();
			if (T.JobTick == 0) { P->TestAxes.Add(DomH == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"), 1.f); T.JobTick = 1; T.At = Now + 0.5; return; }
			if (!Stroke(-X.GetUnitAxis(EAxis::X), Hammer() - X.GetUnitAxis(EAxis::Z) * 2.0, 0.f)) return;
			P->TestAxes.Remove(DomH == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
			UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the trigger held through a stroke: fanned shots now %d, rounds %d"), P->TutNotes.FanShots - R.At.FanShots, P->Mag[DomH]);
			P->GripOf(OffH)->SetWorldLocation(Hammer() + P->Camera->GetRightVector() * 60.0);
			C->Pick(1);
			T.Job = 11;
			T.At = Now + 0.3;
			return;
		}
		case 11:
			if (Id != TEXT("reload")) return;
			Check(P->Mag[DomH] < 6 && !C->IsGood(), FString::Printf(TEXT("the reload card has chambers to fill (%d rounds) and waits"), P->Mag[DomH]));
			P->ToggleGear(OffH, TEXT("ilantern"));
			Check(!P->HeldTool[OffH].IsEmpty(), TEXT("the lantern in the free hand"));
			C->Pick(0);
			T.Job = 12;
			T.At = Now + 0.3;
			return;
		case 12:
			if (Id != TEXT("fan")) return;
			Check(!C->IsGood() && P->HeldTool[OffH].IsEmpty() && BottlesUp() == NumBottles && P->Mag[DomH] >= FanRounds,
				FString::Printf(TEXT("Back to the fan: the hand empty again, the bottles up, %d rounds, the card waiting"), P->Mag[DomH]));
			P->bVR = false;
			Finish();
			return;
		default:
			return;
		}
	}
	case 31:
	{
		// REQ-TUTDOORS (-OLTutorialTest=doors): Next up to the door card; the pair by name, the well beside it, the arrow, clear
		// ground there and on to the next stations, a shot from the street; a hand on each leaf finds it (DoorAtHand); one push
		// leaves the card waiting, the second does it, each leaf's way blocked till it opens; Back into the card shuts them, and a
		// Next onto a leaf opened early too.  -OLOldTutDoors: the jail cell, the old line, no more
		FOutlawsTutorialRun& R = *TutorialRun;
		AOutlawsTutorialCard* C = R.CardActor.Get();
		if (!C) { Check(false, TEXT("doors: the card is there")); Finish(); return; }
		const FString Id = TutorialCardId();
		T.At = Now + 0.05;
		if (Now - T.CardSeen > 60.0) { Check(false, FString::Printf(TEXT("doors: stuck at step %d on %s"), T.Job, *Id)); Finish(); return; }
		auto LeafAt = [this](int32 Sector) -> FVector
		{
			const FOutlawsSector& Sec = Inf->Live.Sectors[Sector];
			FVector2D Cn(0.0, 0.0);
			for (const FVector2D& V : Sec.Verts) Cn += V;
			return FOutlawsGeometry::ToUE(Cn / double(FMath::Max(1, Sec.Verts.Num())), 0.0);
		};
		auto Shut = [this](int32 Sector) -> bool
		{
			const TArray<int32>* Es = Inf->ElevatorsBySector.Find(Sector);
			if (!Es) return false;
			for (int32 e : *Es)
			{
				const FOutlawsElevator& E = Inf->Elevators[e];
				if (E.IsMorph() && (E.Stop != 0 || E.Target >= 0 || FMath::Abs(E.Current) > 0.01)) return false;
			}
			return true;
		};
		// the street's side of a leaf to inside the room, at a hand's height: true when a leaf (or anything) stands in the way
		auto Blocked = [&](int32 Sector) -> bool
		{
			const FVector L0 = LeafAt(Sector) + FVector(0.0, 0.0, R.DoorAt.Z + 130.0);
			FHitResult H;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutLeaf), true, P);
			return World->LineTraceSingleByChannel(H, L0 + R.DoorOut * 25.0, L0 - R.DoorOut * 60.0, ECC_Visibility, Q);
		};
		switch (T.Job)
		{
		case 0:
			if (Id != TEXT("door")) { C->Pick(1); T.At = Now + 0.15; return; }
			T.Job = 1;
			T.At = Now + 0.4;
			return;
		case 1:
		{
			const FString Name1 = R.DoorSector >= 0 ? Inf->Live.Sectors[R.DoorSector].Name : FString();
			const FString Name2 = R.DoorSector2 >= 0 ? Inf->Live.Sectors[R.DoorSector2].Name : FString();
			if (OLFlags::Param(TEXT("OLOldTutDoors")))
			{
				Check(Name1.StartsWith(TEXT("JAILDOOR")) && R.DoorSector2 < 0 && R.Line.Contains(TEXT("the door under the arrow")), FString::Printf(TEXT("old: the jail cell (%s), one door: \"%s\""), *Name1, *R.Line));
				Finish();
				return;
			}
			Check(Name1 == TEXT("COURTHOUSEDOOR01") && Name2 == TEXT("COURTHOUSEDOOR02"), FString::Printf(TEXT("the pair by name (%s, %s)"), *Name1, *Name2));
			Check(R.Line.Contains(TEXT("two doors")) && C->GetLine() == R.Line, FString::Printf(TEXT("the card says doors: \"%s\""), *C->GetLine()));
			Check(Shut(R.DoorSector) && Shut(R.DoorSector2) && !C->IsGood(), TEXT("both shut, the card waiting"));
			// (the well: the level's deep shaft, under -30, beside the pair)
			double WellD = 1e9;
			for (const FOutlawsSector& Sec : Inf->Live.Sectors)
			{
				if (Sec.Floor.Y > -30.0 || Sec.Verts.Num() == 0) continue;
				WellD = FMath::Min(WellD, FVector::Dist2D(FOutlawsGeometry::ToUE(Sec.Verts[0], 0.0), R.DoorAt));
			}
			Check(WellD < 1200.0, FString::Printf(TEXT("the well is beside them, %.0f cm off"), WellD));
			const FVector Over = R.Arrow.IsValid() ? R.Arrow->GetActorLocation() : FVector::ZeroVector;
			Check(R.Arrow.IsValid() && !R.Arrow->IsHidden() && FVector::Dist2D(Over, R.DoorAt) < 80.0 && Over.Z > R.DoorAt.Z + 300.0, FString::Printf(TEXT("the arrow over the middle of the pair (%s, the pair %s)"), *Over.ToCompactString(), *R.DoorAt.ToCompactString()));
			Check(FMath::Abs(R.DoorAt.Z - R.Start.Z) < 30.0, FString::Printf(TEXT("the pair's foot is on the street's ground (z %.0f, the start %.0f)"), R.DoorAt.Z, R.Start.Z));
			// the walk there from the crates' foot, and on to the lantern's and the grab's ground: nothing in the way at the knees
			const FVector Knee(0.0, 0.0, 60.0);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutWalk), true, P);
			FHitResult H;
			const FVector Door = R.DoorAt + R.DoorOut * 150.0 + Knee, Crate = R.Crates + (R.DoorAt - R.Crates).GetSafeNormal2D() * 130.0 + Knee;
			Check(!World->LineTraceSingleByChannel(H, Crate, Door, ECC_Visibility, Q), FString::Printf(TEXT("the street is clear from the crates to the doors (%.0f cm)"), FVector::Dist2D(Crate, Door)));
			Check(!World->LineTraceSingleByChannel(H, Door, R.ManFrom + Knee, ECC_Visibility, Q), FString::Printf(TEXT("and from the doors to where the grab's man comes from (%.0f cm)"), FVector::Dist2D(Door, R.ManFrom)));
			PutFeet(R.DoorAt + R.DoorOut * 800.0, R.DoorAt);
			Face(R.DoorAt);
			T.At = Now + 0.6;
			T.Job = 2;
			return;
		}
		case 2:
			Shot(TEXT("doors_far"));
			T.At = Now + 0.4;
			T.Job = 3;
			return;
		case 3:
			PutFeet(R.DoorAt + R.DoorOut * 75.0, R.DoorAt);
			Face(R.DoorAt);
			T.At = Now + 0.6;
			T.Job = 4;
			return;
		case 4:
		{
			Shot(TEXT("doors_near"));
			Check(Blocked(R.DoorSector) && Blocked(R.DoorSector2), TEXT("each leaf shut stands in the way"));
			// a fist at each leaf's face, from the eyes
			for (const int32 Sector : { R.DoorSector, R.DoorSector2 })
			{
				const FVector Eye = Cam();
				const FVector Hand = LeafAt(Sector) + FVector(0.0, 0.0, R.DoorAt.Z + 130.0) + R.DoorOut * 12.0;
				bool bClosed = false;
				const int32 Got = DoorAtHand(Eye, Hand, bClosed);
				Check(Got == Sector && bClosed, FString::Printf(TEXT("a hand at leaf %d finds it, shut (%d, %d)"), Sector, Got, bClosed ? 1 : 0));
			}
			Check(PushDoorSector(R.DoorSector), TEXT("the first leaf takes the push"));
			T.At = Now + 1.8;
			T.Job = 5;
			return;
		}
		case 5:
			Check(!C->IsGood() && !Shut(R.DoorSector) && Shut(R.DoorSector2), TEXT("one leaf open: the card still waits"));
			Check(!Blocked(R.DoorSector) && Blocked(R.DoorSector2), TEXT("that leaf's way is clear, the other's is not"));
			Shot(TEXT("doors_one"));
			T.At = Now + 0.4;
			T.Job = 6;
			return;
		case 6:
			Check(PushDoorSector(R.DoorSector2), TEXT("the second leaf takes the push"));
			T.At = Now + 1.8;
			T.Job = 7;
			return;
		case 7:
			Check(C->IsGood(), TEXT("both open: done, the tick and Good."));
			Check(!Blocked(R.DoorSector) && !Blocked(R.DoorSector2), TEXT("both ways clear"));
			PutFeet(R.DoorAt + R.DoorOut * 240.0, R.DoorAt);
			Face(R.DoorAt);
			T.At = Now + 0.6;
			T.Job = 8;
			return;
		case 8:
			Shot(TEXT("doors_both"));
			C->Pick(1);
			T.At = Now + 0.4;
			T.Job = 9;
			return;
		case 9:
			if (Id != TEXT("lantern")) return;
			Check(!Shut(R.DoorSector) && !Shut(R.DoorSector2), TEXT("the doors stay open behind the next card"));
			C->Pick(0);
			T.Job = 10;
			return;
		case 10:
			if (Id != TEXT("door")) return;
			Check(Shut(R.DoorSector) && Shut(R.DoorSector2) && !C->IsGood(), TEXT("Back into the card: both shut again at once, the card waiting"));
			T.At = Now + 0.6;
			T.Job = 11;
			return;
		case 11:
			Check(!C->IsGood() && Blocked(R.DoorSector) && Blocked(R.DoorSector2), TEXT("and solid: no tick by itself"));
			Shot(TEXT("doors_back"));
			C->Pick(0);
			T.At = Now + 0.3;
			T.Job = 12;
			return;
		case 12:
			// (a leaf opened while another card is up, a Next onto it)
			if (Id != TEXT("jump")) return;
			PushDoorSector(R.DoorSector);
			T.At = Now + 0.5;
			T.Job = 13;
			return;
		case 13:
			C->Pick(1);
			T.Job = 14;
			return;
		case 14:
			if (Id != TEXT("door")) return;
			Check(Shut(R.DoorSector) && Shut(R.DoorSector2) && !C->IsGood(), TEXT("Next onto a leaf caught moving: shut for the card, not a free tick"));
			Finish();
			return;
		default:
			return;
		}
	}
	case 34:
	{
		// REQ-TUTGUNFIRE (-OLTutorialTest=gunfire): the card held low (TestPinAge), the main hand's gun through Next, then Back: the trigger
		// fires and presses nothing, a fan the same, no laser on the card; the empty hand's trigger presses Next, its fist presses Back;
		// risen, the gun on a button presses and does not fire, a fan does not fire, off the buttons the gun fires; then guns in both hands
		// low (both fire) and risen (the other hand's presses).  Left-handed: DominantHand=Left.  -OLOldTutGunPress: a gun on a low button presses
		FOutlawsTutorialRun& R = *TutorialRun;
		AOutlawsTutorialCard* C = R.CardActor.Get();
		if (!C || !P->Camera) { Check(false, TEXT("gunfire: the card and the camera are there")); Finish(); return; }
		const bool bOld = OLFlags::Param(TEXT("OLOldTutGunPress"));
		const FString Id = TutorialCardId();
		const int32 DomH = P->MainHand();
		const int32 OffH = 1 - DomH;
		const FTransform CamX = P->Camera->GetComponentTransform();
		static bool bHover = false, bLaser = false, bFired = false, bPressed = false;
		T.At = Now + 0.03;
		if (Now - T.CardSeen > 60.0) { Check(false, FString::Printf(TEXT("gunfire: stuck at step %d on %s"), T.Job, *Id)); Finish(); return; }
		auto Lay = [P, &CamX](int32 H, const FVector& To)
		{
			const FVector From = CamX.GetLocation() + CamX.TransformVectorNoScale(FVector(25.0, H == 0 ? -15.0 : 15.0, -20.0));
			P->AimOf(H)->SetWorldLocationAndRotation(From, (To - From).Rotation());
		};
		auto TrigOf = [](int32 H) { return FString(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger")); };
		auto GripOf = [](int32 H) { return FString(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")); };
		// a trial: hand H's laser held on the point, its trigger pulled and let go; true when it has been read
		auto Trial = [&](int32 H, const FVector& To)
		{
			Lay(H, To);
			if (++T.JobTick == 6)
			{
				bHover = AOutlawsTutorialCard::HandOnCard(H);
				bLaser = C->TestLaserShown(H);
				T.Tries = P->TutNotes.Shots;
				T.CardId = Id;
				P->Mag[H] = FMath::Max(P->Mag[H], 3);
				P->FireCooldown[H] = 0.f;
				P->TestAxes.Add(TrigOf(H), 1.f);
			}
			if (T.JobTick < 16) return false;
			P->TestAxes.Remove(TrigOf(H));
			T.JobTick = 0;
			bFired = P->TutNotes.Shots > T.Tries;
			bPressed = Id != T.CardId;
			return true;
		};
		auto Said = [&]() { return FString::Printf(TEXT("fired %d, pressed %d (%s to %s), its button lit %d, its laser on the card %d, rise %.2f, hands %d %d, a gun %d %d, open %d %d"), bFired, bPressed, *T.CardId, *Id, bHover, bLaser, C->GetRise(), P->Equipped[0], P->Equipped[1], P->HandHoldsGun(0), P->HandHoldsGun(1), P->bBreakOpen[0], P->bBreakOpen[1]); };
		// a fan: the same call the rake past the hammer makes (UpdateFan), the laser laid first; true when it has been read
		auto Fan = [&](int32 H, const FVector& To)
		{
			Lay(H, To);
			if (++T.JobTick < 6) return false;
			T.Tries = P->TutNotes.Shots;
			P->Mag[H] = FMath::Max(P->Mag[H], 3);
			P->FireCooldown[H] = 0.f;
			P->FireGun(H, true);
			T.JobTick = 0;
			bFired = P->TutNotes.Shots > T.Tries;
			return true;
		};
		const FVector NextAt = C->TestButtonAt(1), BackAt = C->TestButtonAt(0);
		switch (T.Job)
		{
		case 0:
			Check(P->TutorialTestWheelTake(DomH, RevolverIdx), FString::Printf(TEXT("gunfire: the revolver in the main hand (%d)"), DomH));
			P->bVR = true;      // (the headset's rules from here: a hand's weapon tick, the lasers)
			T.At = Now + 0.5;
			T.Job = 1;
			return;
		case 1:
			T.CardId = Id;
			C->Pick(1);
			T.Job = 2;
			return;
		case 2:
			if (Id == T.CardId) return;
			C->TestPinAge(0.6f);
			T.JobTick = 0;
			T.Job = 3;
			return;
		case 3:
			if (!Trial(DomH, NextAt)) return;
			Check(bOld ? bPressed && !bFired : bFired && !bPressed && !bHover && !bLaser, FString::Printf(TEXT("the gun through low Next: %s"), *Said()));
			T.Job = 4;
			return;
		case 4:
			if (!Trial(DomH, BackAt)) return;
			Check(bOld ? bPressed && !bFired : bFired && !bPressed && !bHover && !bLaser, FString::Printf(TEXT("the gun through low Back: %s"), *Said()));
			T.Job = 5;
			return;
		case 5:
			if (!Fan(DomH, NextAt)) return;
			Check(bOld ? !bFired : bFired, FString::Printf(TEXT("a fan through low Next: fired %d"), bFired));
			T.Job = 6;
			return;
		case 6:
			if (!Fan(DomH, BackAt)) return;
			Check(bOld ? !bFired : bFired, FString::Printf(TEXT("a fan through low Back: fired %d"), bFired));
			T.Job = 7;
			return;
		case 7:
			Check(P->Equipped[OffH] < 0, FString::Printf(TEXT("the other hand is empty (%d)"), P->Equipped[OffH]));
			T.Job = 8;
			return;
		case 8:
			if (!Trial(OffH, NextAt)) return;
			Check(bPressed && !bFired && bHover && bLaser, FString::Printf(TEXT("the empty hand presses low Next: %s"), *Said()));
			T.Job = 9;
			return;
		case 9:
			// (a fist clear of the gun hand: squeezed against it the gun changes hands, and the test's hands start together)
			P->GripOf(OffH)->SetWorldLocation(P->GripOf(DomH)->GetComponentLocation() + CamX.TransformVectorNoScale(FVector(0.0, OffH == 0 ? -1.0 : 1.0, 0.0)) * (P->SwapReach() + 30.0));
			P->TestAxes.Add(GripOf(OffH), 1.f);
			if (!Trial(OffH, BackAt)) return;
			P->TestAxes.Remove(GripOf(OffH));
			Check(bPressed && !bFired && bHover && bLaser, FString::Printf(TEXT("a fist presses low Back: %s"), *Said()));
			C->TestPinAge(10.f);
			T.Job = 10;
			return;
		case 10:
			if (C->GetRise() < 1.f) return;
			Lay(DomH, NextAt);
			T.Job = 11;
			return;
		case 11:
			if (!Trial(DomH, NextAt)) return;
			Check(bPressed && !bFired && bHover && bLaser, FString::Printf(TEXT("risen: the gun on Next presses, as today: %s"), *Said()));
			T.Job = 12;
			return;
		case 12:
			if (C->GetRise() < 1.f) return;
			if (!Fan(DomH, BackAt)) return;
			Check(!bFired, FString::Printf(TEXT("risen: a fan on Back does not fire, as today: fired %d"), bFired));
			T.Job = 13;
			return;
		case 13:
			if (!Trial(DomH, C->GetActorLocation())) return;
			Check(bFired && !bPressed && !bHover, FString::Printf(TEXT("risen: the gun on the card off its buttons fires, as today: %s"), *Said()));
			T.Job = 14;
			return;
		case 14:
			P->bVR = false;
			AddCount(TEXT("ipistol"), 1);      // (the tutorial gives one revolver: the wheel would take it out of the main hand)
			if (P->Equipped[OffH] != RevolverIdx) P->TutorialTestWheelTake(OffH, RevolverIdx);
			Check(P->Equipped[OffH] == RevolverIdx && P->Equipped[DomH] == RevolverIdx, FString::Printf(TEXT("a gun in each hand (%d, %d)"), P->Equipped[0], P->Equipped[1]));
			P->bVR = true;
			C->TestPinAge(0.6f);
			T.At = Now + 0.3;
			T.Job = 15;
			return;
		case 15:
			if (C->GetRise() > 0.f || !Trial(OffH, NextAt)) return;
			Check(bOld ? bPressed && !bFired : bFired && !bPressed, FString::Printf(TEXT("guns in both hands, low: the other hand's on Next: %s"), *Said()));
			T.Job = 16;
			return;
		case 16:
			if (!Trial(DomH, NextAt)) return;
			Check(bOld ? bPressed && !bFired : bFired && !bPressed, FString::Printf(TEXT("guns in both hands, low: the main hand's on Next: %s"), *Said()));
			C->TestPinAge(10.f);
			T.Job = 17;
			return;
		case 17:
			if (C->GetRise() < 1.f) return;
			T.Job = 18;
			return;
		default:
			if (!Trial(OffH, NextAt)) return;
			Check(bPressed && !bFired && bHover, FString::Printf(TEXT("guns in both hands, risen: the other hand's on Next presses: %s"), *Said()));
			Finish();
			return;
		}
	}
	case 30:
	{
		// REQ-TUTCARDFLOAT (-OLTutorialTest=float): the card's first seconds, read off the card and the camera: Next's new card
		// fading in at the middle of the view, its hold, the rise part way, the top where it always stood (its bottom edge 10
		// degrees up), its face on the eyes at each (the tilt, not doubled); the step going to done does not start it over; Back
		// and Next each do; the laser stays on a moving button and the trigger presses it; the wall's pull-in at both ends; a
		// shot at each stage, the lines over the card and the glyph under it.  -OLOldTutCardFloat: at the top from the start
		FOutlawsTutorialRun& R = *TutorialRun;
		AOutlawsTutorialCard* C = R.CardActor.Get();
		if (!C || !P->Camera) { Check(false, TEXT("float: the card and the camera are there")); Finish(); return; }
		const FTransform CamX = P->Camera->GetComponentTransform();
		const FVector Local = CamX.InverseTransformPosition(C->GetActorLocation());
		const double MidDeg = FMath::RadiansToDegrees(FMath::Atan2(Local.Z, Local.X));
		const double BottomDeg = FMath::RadiansToDegrees(FMath::Atan2(double(P->TutCardBottomZ), double(P->TutCardDist)));
		const double TopDeg = FMath::RadiansToDegrees(FMath::Atan2(double(P->TutCardTopZ), double(P->TutCardDist)));
		const FVector ToEye = (CamX.GetLocation() - C->GetActorLocation()).GetSafeNormal();
		const double FaceDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(C->GetActorTransform().GetUnitAxis(EAxis::Z), ToEye), -1.0, 1.0)));
		const FString Where = FString::Printf(TEXT("age %.2f, fade %.2f, rise %.2f: its middle %.1f degrees up, its edges %.1f to %.1f, %.0f cm out, its face %.1f degrees off the eyes"),
			C->GetAge(), C->GetFade(), C->GetRise(), MidDeg, BottomDeg, TopDeg, P->TutCardDist, FaceDeg);
		const FString Id = TutorialCardId();
		const float HoldEnd = 3.3f, MidRise = 4.05f, Settled = 4.8f;
		T.At = Now + 0.03;
		if (Now - T.CardSeen > 40.0) { Check(false, FString::Printf(TEXT("float: stuck at step %d (%s)"), T.Job, *Where)); Finish(); return; }
		// (hand H's laser laid on the point from beside the head)
		auto Lay = [P, &CamX](int32 H, const FVector& To)
		{
			const FVector From = CamX.GetLocation() + CamX.TransformVectorNoScale(FVector(25.0, H == 0 ? -15.0 : 15.0, -20.0));
			P->AimOf(H)->SetWorldLocationAndRotation(From, (To - From).Rotation());
		};
		const FString Trig = TEXT("OLVR_RightTrigger");
		switch (T.Job)
		{
		case 0:
			C->Pick(1);
			T.Job = 1;
			return;
		case 1:
			if (Id != TEXT("turn")) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: float: the new card: %s"), *Where);
			if (OLFlags::Param(TEXT("OLOldTutCardFloat")))
			{
				Check(C->GetFade() == 1.f && C->GetRise() == 1.f && FMath::Abs(BottomDeg - 10.0) < 0.3, FString::Printf(TEXT("old: the card is at the top at once (%s)"), *Where));
				Finish();
				return;
			}
			Check(C->GetFade() < 1.f && C->GetRise() == 0.f && FMath::Abs(MidDeg) < 1.0, FString::Printf(TEXT("the new card fades in at the middle of the view (%s)"), *Where));
			T.Job = 2;
			return;
		case 2:
			if (C->GetAge() < 1.5f) return;
			Check(C->GetFade() == 1.f && C->GetRise() == 0.f && FMath::Abs(MidDeg) < 0.5 && FaceDeg < 1.0 && BottomDeg < 0.0 && TopDeg > 0.0, FString::Printf(TEXT("it holds there, faded in (%s)"), *Where));
			P->TurnBy(70.f);
			T.Job = 3;
			return;
		case 3:
			if (!C->IsGood()) return;
			Check(C->GetAge() > 1.4f && C->GetRise() == 0.f, FString::Printf(TEXT("the same step going to done does not start it over (%s)"), *Where));
			T.Job = 4;
			return;
		case 4:
			if (C->GetAge() < MidRise) return;
			Check(C->GetRise() > 0.1f && C->GetRise() < 0.9f && MidDeg > 2.0 && MidDeg < 14.5 && FaceDeg < 1.0, FString::Printf(TEXT("3 s on it is rising, its face still on the eyes (%s)"), *Where));
			T.Job = 5;
			return;
		case 5:
			if (C->GetAge() < Settled) return;
			Check(C->GetRise() == 1.f && FMath::Abs(BottomDeg - 10.0) < 0.2 && FMath::Abs(MidDeg - 15.8) < 0.5 && FaceDeg < 1.0, FString::Printf(TEXT("it ends where it always stood, tilted at the eyes, not doubled (%s)"), *Where));
			Check(C->IsGood(), TEXT("still done"));
			C->Pick(0);
			T.Job = 6;
			return;
		case 6:
			if (Id != TEXT("walk")) return;
			Check(C->GetAge() < 0.7f && C->GetRise() == 0.f && FMath::Abs(MidDeg) < 1.0, FString::Printf(TEXT("Back counts as new (%s)"), *Where));
			C->Pick(1);
			T.Job = 7;
			return;
		case 7:
			if (Id != TEXT("turn")) return;
			Check(C->GetAge() < 0.7f && C->GetRise() == 0.f && FMath::Abs(MidDeg) < 1.0, FString::Printf(TEXT("Next counts as new (%s)"), *Where));
			// (the headset's rules for the rest: the lasers, the glyph plate)
			P->bVR = true;
			C->TestPinAge(0.6f);
			T.JobTick = 0;
			T.Job = 8;
			return;
		case 8:
			Lay(1, C->TestButtonAt(1));
			if (++T.JobTick < 6) return;
			Check(AOutlawsTutorialCard::HandOnCard(1), FString::Printf(TEXT("the laser on Next at the middle of the view (%s)"), *Where));
			C->TestPinAge(HoldEnd + 0.1f);
			T.JobTick = 0;
			T.Job = 9;
			return;
		case 9:
			Lay(1, C->TestButtonAt(1));
			if (++T.JobTick < 3) return;
			C->TestPinAge(-1.f);
			T.Job = 10;
			return;
		case 10:
			// (let run: the laser put on the button again each frame, as a hand following it)
			Lay(1, C->TestButtonAt(1));
			T.At = Now;
			if (C->GetRise() < 0.4f) return;
			Check(AOutlawsTutorialCard::HandOnCard(1), FString::Printf(TEXT("the laser stays on Next as the card moves (%s)"), *Where));
			P->TestAxes.Add(Trig, 1.f);
			T.Job = 11;
			return;
		case 11:
			if (Id == TEXT("turn")) { Lay(1, C->TestButtonAt(1)); return; }
			Check(Id == TEXT("take_revolver"), FString::Printf(TEXT("the trigger on Next on the rising card pressed it (%s)"), *Id));
			P->TestAxes.Remove(Trig);
			T.Job = 12;
			return;
		case 12:
		{
			// the nearest wall from the walk's post: the feet put before it, facing it
			FVector Dir = FVector::ForwardVector, Spot = FVector::ZeroVector;
			double Near = 1e9;
			const FVector From = R.Marker + FVector(0.0, 0.0, 150.0);
			for (int32 k = 0; k < 24; ++k)
			{
				const FVector D = FRotator(0.0, k * 15.0, 0.0).Vector();
				FHitResult H;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutWall), true, P);
				if (!World->LineTraceSingleByChannel(H, From, From + D * 3000.0, ECC_Visibility, Q) || H.Distance >= Near || H.Distance <= 200.0) continue;
				// (a real wall, not a prop: the line at 260 cm up meets it at the same place too)
				FHitResult HighHit;
				if (!World->LineTraceSingleByChannel(HighHit, From + FVector(0.0, 0.0, 110.0), From + FVector(0.0, 0.0, 110.0) + D * 3000.0, ECC_Visibility, Q) || FMath::Abs(HighHit.Distance - H.Distance) > 20.0) continue;
				Near = H.Distance; Dir = D; Spot = H.ImpactPoint;
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: float wall candidate yaw %.0f, %.0f cm, hit %s, high hit %.0f cm"), k * 15.0, H.Distance, H.GetActor() ? *H.GetActor()->GetName() : TEXT("?"), HighHit.Distance);
			}
			PutFeet(FVector(Spot.X, Spot.Y, R.Start.Z) - Dir * 62.0, Spot);
			if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.0, Dir.Rotation().Yaw, 0.0));
			C->TestPinAge(1.5f);
			T.At = Now + 0.6;
			T.Job = 13;
			return;
		}
		case 13:
			Check(P->TutCardDist < 65.f && FMath::Abs(MidDeg) < 1.0, FString::Printf(TEXT("a wall near: the card is pulled in at the middle of the view (%s)"), *Where));
			C->TestPinAge(Settled);
			T.At = Now + 0.4;
			T.Job = 14;
			return;
		case 14:
			Check(P->TutCardDist < 70.f && FMath::Abs(BottomDeg - 10.0) < 0.5, FString::Printf(TEXT("a wall near: pulled in at the top too (%s)"), *Where));
			PutFeet(R.Marker - R.Dir * 150.0, R.Rail);
			Face(R.Rail);
			T.JobTick = 0;
			T.At = Now + 0.5;
			T.Job = 15;
			return;
		default:
		{
			// a shot at each stage, each held still long enough for the frame
			static const float Ages[] = { 0.15f, 1.5f, 4.05f, 4.8f };
			static const TCHAR* const Names[] = { TEXT("float_fade"), TEXT("float_hold"), TEXT("float_rise"), TEXT("float_top") };
			if (T.Job % 2 == 1)
			{
				const int32 i = (T.Job - 15) / 2;
				if (i >= UE_ARRAY_COUNT(Ages)) { C->TestPinAge(-1.f); Finish(); return; }
				C->TestPinAge(Ages[i]);
				Notify(TEXT("A notice line goes over the card"), 3.f);
				T.At = Now + 0.5;
			}
			else
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: float: shot: %s"), *Where);
				Shot(Names[(T.Job - 16) / 2]);
				T.At = Now + 0.4;
			}
			++T.Job;
			return;
		}
		}
	}
	case 4:
	{
		// one card: its action, its "Good.", its Next; the next card's step and line
		const FString Id = TutorialCardId();
		if (!bTutorial)
		{
			// the last card done: the tutorial is over
			T.Phase = 6;
			T.At = Now + 0.5;
			T.CardSeen = Now;
			return;
		}
		FOutlawsTutorialRun& R = *TutorialRun;
		AOutlawsTutorialCard* C = R.CardActor.Get();
		if (!C) { Check(false, TEXT("the card panel is there")); Finish(); return; }
		if (Id != T.CardId)
		{
			const int32 Step = C->GetStep();
			const bool bBack = !T.BackFrom.IsEmpty();
			Check(!Id.IsEmpty() && C->IsUp() && C->GetLine() == R.Line && !R.Line.IsEmpty() && !C->IsGood() && Step == (bBack ? T.LastStep - 1 : T.LastStep + 1),
				FString::Printf(TEXT("%s %s, the card %s up: step %d of %d, \"%s\"%s"), *T.CardId, bBack ? TEXT("Back") : TEXT("Next"), *Id, Step, C->GetSteps(), *C->GetLine(),
					C->GetHint().IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" / \"%s\""), *C->GetHint())));
			if (bBack)
			{
				// (Back: the card before's station set up again)
				if (Id == TEXT("fire"))
				{
					int32 Up = 0;
					for (int32 i = 0; i < NumBottles; ++i)
					{
						const AOutlawsSpriteActor* B = R.Bottles[i].Get();
						const FProp* Pr = B ? Props.FindByPredicate([B](const FProp& X) { return X.Sprite.Get() == B; }) : nullptr;
						Up += B && !B->IsHidden() && Pr && Pr->Current == 0 ? 1 : 0;
					}
					Check(Up == NumBottles, FString::Printf(TEXT("Back to the bottles: all %d stand on the rail again (%d up)"), NumBottles, Up));
					Shot(TEXT("back_fire"));
				}
				else if (Id == TEXT("throw_knife"))
				{
					Check(Counts.FindRef(TEXT("iknife")) == 2 && (P->Equipped[0] == KnifeIdx || P->Equipped[1] == KnifeIdx),
						FString::Printf(TEXT("Back to the knife: both knives back, one in the hand (%d, hands %d %d)"), Counts.FindRef(TEXT("iknife")), P->Equipped[0], P->Equipped[1]));
				}
				else if (Id == TEXT("grab"))
				{
					AOutlawsEnemy* E = R.Men[0].Get();
					Check(E && !E->IsDead() && E != T.ManBefore.Get() && P->GrabbedEnemy[0].Get() != E && P->GrabbedEnemy[1].Get() != E,
						FString::Printf(TEXT("Back to the grab: a bad guy comes back (%s), not held"), E ? *E->GetName() : TEXT("none")));
				}
				T.BackFrom.Reset();
			}
			if (T.CardId == TEXT("jump")) P->bCrouchHeld = false;      // (the jump card's crouch let go)
			T.LastStep = Step;
			T.CardId = Id;
			T.CardSeen = Now;
			T.Tries = 0;
			T.bGoodSeen = false;
			T.PendingShot.Reset();
			T.At = Now + 1.0;
			if (Id != TEXT("punch"))
			{
				// (the hands let go: the grip held on a bad guy is kept only into the punch card)
				P->CoopTestHand(0, FVector::ZeroVector, false);
				P->CoopTestHand(1, FVector::ZeroVector, false);
			}
			if (Mode == TEXT("skip") && Step >= 4)
			{
				T.Phase = 20;
				return;
			}
			// Back once from three stations: the bottles (from the card after them), the knife (from take_dynamite), the grab (from punch)
			const FString AfterFire = Skipped(P, &R, Fan) ? TEXT("reload") : TEXT("fan");
			const TCHAR* BackOn = Id == AfterFire ? TEXT("fire") : Id == TEXT("take_dynamite") ? TEXT("throw_knife") : Id == TEXT("punch") ? TEXT("grab") : nullptr;
			if (Mode == TEXT("walk") && BackOn && !T.Backed.Contains(Id))
			{
				T.Backed.Add(Id);
				T.BackFrom = Id;
				T.ManBefore = R.Men[0].Get();
				if (Id == TEXT("punch")) { P->CoopTestHand(0, FVector::ZeroVector, false); P->CoopTestHand(1, FVector::ZeroVector, false); }
				C->Pick(0);
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: Back pressed on %s"), *Id);
				T.At = Now + 0.8;
				return;
			}
			if (Id == TEXT("fire") && T.Backed.Contains(AfterFire) && !T.Shot.Contains(TEXT("next_undone")))
			{
				// (Next always works: pressed on the bottles again before they are shot)
				T.Shot.Add(TEXT("next_undone"));
				C->Pick(1);
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: Next pressed on fire before its action"));
				T.At = Now + 0.8;
				return;
			}
			return;
		}
		if (Mode == TEXT("skip") && !T.bSkipNext)
		{
			// (Next always works) the first card's Next pressed before its action: the next card comes up all the same
			T.bSkipNext = true;
			Check(!C->IsGood(), FString::Printf(TEXT("skip: %s is not done when Next is pressed"), *Id));
			C->Pick(1);
			T.At = Now + 0.8;
			return;
		}
		if (C->IsGood())
		{
			if (!T.bGoodSeen)
			{
				T.bGoodSeen = true;
				Check(R.bGood && Id == T.CardId, FString::Printf(TEXT("%s: its action seen, the card shows the tick and \"Good.\" and stays up (Next lit)"), *Id));
				if (Id == TEXT("fire")) Shot(TEXT("fire_good"));
				T.At = Now + 0.6;
				return;
			}
			C->Pick(1);
			T.At = Now + 0.8;
			return;
		}
		if (Now - T.CardSeen > 25.0)
		{
			Check(false, FString::Printf(TEXT("card %s done within 25 s"), *T.CardId));
			Finish();
			return;
		}
		if (!T.PendingShot.IsEmpty())
		{
			Shot(T.PendingShot);
			T.PendingShot.Reset();
			T.At = Now + 0.3;
			return;
		}
		const int32 Try = T.Tries++;
		T.At = Now + 1.0;
		if (Try == 0)
		{
			// the card photographed, the desktop's view turned to its target (as the headset's player would look)
			if (Id == TEXT("walk")) Face(R.Marker);
			else if (Id == TEXT("take_revolver") || Id == TEXT("fire") || Id == TEXT("fan")) Face(R.Rail);
			else if (Id == TEXT("pickup")) Face(R.BoxAt);
			else if (Id == TEXT("take_knife") || Id == TEXT("throw_knife")) Face(R.KnifePost);
			else if (Id == TEXT("take_dynamite")) Face(R.Barrel);
			else if (Id == TEXT("throw_dynamite")) Face(P->GetActorLocation() + R.Dir * 1000.0);
			else if (Id == TEXT("climb")) Face(R.Crates);
			else if (Id == TEXT("door")) Face(R.DoorAt);
			else if (Id == TEXT("horse")) Face(R.HorseAt);
			if (Id == TEXT("throw_knife") || Id == TEXT("throw_dynamite"))
			{
				// (user 2026-10-14: the barrel was not to be seen) the card's target in plain sight from where the player
				// stands looking down the street: nothing between the eyes and it, within 50 degrees of the street's way, and
				// the barrel well apart from the post (it stood behind it)
				const bool bBarrel = Id == TEXT("throw_dynamite");
				const FVector Target = bBarrel ? R.Barrel + FVector(0.0, 0.0, 50.0) : R.KnifePost + FVector(0.0, 0.0, 120.0);
				FHitResult H;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutSee), true, P);
				const bool bHit = World->LineTraceSingleByChannel(H, Cam(), Target, ECC_Visibility, Q);
				const bool bClear = !bHit || H.Distance > FVector::Dist(Cam(), Target) - 45.0;
				const double Off = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(R.Dir.GetSafeNormal2D(), (Target - Cam()).GetSafeNormal2D()), -1.0, 1.0)));
				Check(bClear && Off < 50.0, FString::Printf(TEXT("%s: the %s is in plain sight from the player (%.0f degrees off the street's way, %.0f cm off, the line to it %s)"), *Id,
					bBarrel ? TEXT("barrel") : TEXT("post"), Off, FVector::Dist(Cam(), Target), bClear ? TEXT("clear") : *FString::Printf(TEXT("blocked by %s"), H.GetActor() ? *H.GetActor()->GetName() : TEXT("the level"))));
				if (bBarrel)
				{
					const double Apart = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((R.KnifePost - Cam()).GetSafeNormal2D(), (Target - Cam()).GetSafeNormal2D()), -1.0, 1.0)));
					Check(R.BarrelActor.IsValid() && Apart > 10.0, FString::Printf(TEXT("throw_dynamite: the game's barrel is there (%s), %.0f degrees apart from the post"),
						R.BarrelActor.IsValid() ? *R.BarrelActor->GetName() : TEXT("none"), Apart));
				}
			}
			if (Id == TEXT("fire"))
			{
				int32 Made = 0;
				for (const auto& B : R.Bottles) Made += B.IsValid() ? 1 : 0;
				Check(Made == NumBottles, FString::Printf(TEXT("fire: the game's five bottles stand on the rail (%d made)"), Made));
			}
			if (Id == TEXT("pickup")) Check(R.Box.IsValid(), TEXT("pickup: the box of bullets lies on the ground"));
			const bool bArrow = R.Arrow.IsValid() && !R.Arrow->IsHidden();
			const bool bWantsArrow = Id == TEXT("walk") || Id == TEXT("fire") || Id == TEXT("fan") || Id == TEXT("pickup") || Id == TEXT("take_knife") || Id == TEXT("throw_knife") || Id == TEXT("take_dynamite")
				|| Id == TEXT("throw_dynamite") || Id == TEXT("climb") || Id == TEXT("door") || Id == TEXT("horse");
			if (bWantsArrow) Check(bArrow, FString::Printf(TEXT("%s: the arrow floats over its target"), *Id));
			T.PendingShot = Id;
			T.At = Now + 0.6;
			return;
		}
		// (one controller, REQ-ONECTRL: everything on its one hand)
		const int32 One = P->OneHand();
		const int32 DomH = One >= 0 ? One : P->LeftDominant() ? 0 : 1;
		const int32 OffH = One >= 0 ? One : 1 - DomH;
		auto GunHand = [P]() { return P->Equipped[1] == RevolverIdx ? 1 : P->Equipped[0] == RevolverIdx ? 0 : -1; };
		// the reload button pressed for an empty revolver; the desktop runs no hand's gun tick (UpdateGuns is VR only), so
		// there the cylinder is filled from the reserve as the button's load would
		auto LoadGun = [P, this](int32 H)
		{
			P->bButtonReload[H] = true;
			if (P->IsVR()) return;
			int32& Reserve = Counts.FindOrAdd(TEXT("ibullit"));
			const int32 N = FMath::Clamp(6 - P->Mag[H], 0, Reserve);
			P->Mag[H] += N;
			Reserve -= N;
			UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the desktop's reload: %d rounds in (%d left)"), N, Reserve);
		};
		if (Id == TEXT("walk"))
		{
			Face(R.Marker);
			P->HoldMoveStick(FVector2D(0.f, 1.f), 0.5f);
			T.At = Now + 0.25;
			if (Try > 60) P->HoldMoveStick(FVector2D::ZeroVector, 0.f);
		}
		else if (Id == TEXT("turn")) { P->TurnBy(45.f); T.At = Now + 0.5; }
		else if (Id == TEXT("take_revolver")) { Check(P->TutorialTestWheelTake(DomH, RevolverIdx), TEXT("the revolver taken from the main hand's wheel")); }
		else if (Id == TEXT("fire"))
		{
			// one bottle at a time: the hand turned on it and fired; out of rounds, a reload; after a few misses its hit applied
			int32 b = 0;
			while (b < NumBottles && [&]() { const AOutlawsSpriteActor* B = R.Bottles[b].Get(); const FProp* Pr = B ? Props.FindByPredicate([B](const FProp& X) { return X.Sprite.Get() == B; }) : nullptr; return !B || B->IsHidden() || !Pr || Pr->Current != 0; }()) ++b;
			int32 H = GunHand();
			if (H < 0) { P->TutorialTestWheelTake(DomH, RevolverIdx); H = GunHand(); }
			if (b < NumBottles && H >= 0)
			{
				if (P->Mag[H] <= 0) { LoadGun(H); T.At = Now + 1.5; return; }
				const FVector Target = R.BottleAt[b] + FVector(0.0, 0.0, 12.0);
				Face(Target);
				Aim(H, Target);
				P->TutorialTestAimGun(H, Target);
				FireAt(H, Target);
				if (Try >= 4 + b * 2)
				{
					const int32 Pi = Props.IndexOfByPredicate([&R, b](const FProp& X) { return X.Sprite.Get() == R.Bottles[b].Get(); });
					UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the aimed shots missed bottle %d: its hit applied"), b + 1);
					if (Pi != INDEX_NONE) PropReact(Pi, false);
				}
			}
			T.At = Now + 0.5;
		}
		else if (Id == TEXT("fan"))
		{
			// (the desktop runs no hand's weapon tick: the fanned shot itself; -OLTutorialTest=fan swipes a hand past the hammer)
			const int32 H = GunHand();
			Check(H == DomH && P->Equipped[OffH] < 0 && P->Mag[DomH] >= FanRounds, FString::Printf(TEXT("fan: the revolver in the main hand with %d rounds, the other hand empty (%d, %d)"), P->Mag[DomH], P->Equipped[0], P->Equipped[1]));
			P->FireCooldown[DomH] = 0.f;
			P->FireGun(DomH, true);
		}
		else if (Id == TEXT("reload"))
		{
			if (P->bFaceReload && One < 0) { if (P->LeftDominant()) { P->PressY(); P->PressYReleased(); } else { P->PressB(); P->PressBReleased(); } }
			else { const int32 H = GunHand() >= 0 ? GunHand() : DomH; P->OpenWheel(H); P->ReleaseWheel(H); }       // (a tap of the wheel button)
		}
		else if (Id == TEXT("pickup"))
		{
			// walked over (the box under the feet); then taken outright if the walk-over did not
			if (Try == 1) { PutFeet(R.BoxAt - R.Dir * 30.0, R.BoxAt + R.Dir * 100.0); T.At = Now + 0.4; }
			else if (Try >= 2) { PutFeet(R.BoxAt, R.BoxAt + R.Dir * 100.0); T.At = Now + 0.8; }
			if (Try >= 4)
			{
				const int32 Pi = Pickups.IndexOfByPredicate([&R](const FPickup& X) { return X.Actor.Get() == R.Box.Get(); });
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the box not walked up: taken outright (%d)"), Pi);
				if (Pi != INDEX_NONE) CollectPickup(Pi);
			}
		}
		else if (Id == TEXT("take_knife")) { Check(P->TutorialTestWheelTake(OffH, KnifeIdx), One >= 0 ? TEXT("the knife taken from the one wheel") : TEXT("the knife taken from the other hand's wheel")); }
		else if (Id == TEXT("throw_knife") || Id == TEXT("throw_dynamite"))
		{
			const int32 Want = Id == TEXT("throw_knife") ? KnifeIdx : DynamiteIdx;
			const int32 H = P->Equipped[0] == Want ? 0 : 1;
			const FVector Target = Id == TEXT("throw_knife") ? R.KnifePost + FVector(0.0, 0.0, 120.0) : R.Barrel + FVector(0.0, 0.0, 60.0);
			Face(Target);
			Aim(H, Target + FVector(0.0, 0.0, 150.0));
			// (the desktop runs no trigger for a throwable and has no swinging hand: the trigger's light and the release's own
			// throw, then the flight a swing toward the target gives, a 45 degree lob at the throw's own fall)
			if (Want == DynamiteIdx && !P->bFuseLit[H]) { P->bFuseLit[H] = true; P->Fuse[H] = 4.5f; }
			const int32 Before = P->Thrown.Num();
			P->ThrowHeld(H);
			if (P->Thrown.Num() > Before)
			{
				AOutlawsPlayer::FThrown& Th = P->Thrown.Last();
				const FVector To = Target - FVector(0.0, 0.0, 100.0);
				const double D = FVector::Dist2D(Th.Pos, To);
				const double G = Th.Pull > 0.f ? Th.Pull : 980.0;
				const double V = FMath::Sqrt(FMath::Max(1.0, D * G));
				Th.Vel = (To - Th.Pos).GetSafeNormal2D() * V * 0.7071 + FVector(0.0, 0.0, V * 0.7071);
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: thrown at the target %.0f cm off (%.0f cm/s, lit %d)"), D, V, Th.Fuse > 0.f ? 1 : 0);
			}
		}
		else if (Id == TEXT("take_dynamite")) { Check(P->TutorialTestWheelTake(OffH, DynamiteIdx), TEXT("the dynamite taken from the other hand's wheel")); }
		else if (Id == TEXT("climb"))
		{
			const FVector In = (R.Crates - P->GetActorLocation()).GetSafeNormal2D();
			const FVector Face2 = FMath::Abs(FVector::DotProduct(In, R.Dir)) > FMath::Abs(FVector::DotProduct(In, R.Side))
				? R.Dir * FMath::Sign(FVector::DotProduct(In, R.Dir)) : R.Side * FMath::Sign(FVector::DotProduct(In, R.Side));
			if (Try == 1)
			{
				// stood at the crates, then the climb at their lip as the grabbing hands start it (the climb tests' way)
				PutFeet(R.Crates - Face2 * (R.CrateHalf + 45.0), R.Crates);
				T.At = Now + 0.6;
			}
			else if (Try >= 2)
			{
				const FVector Lip = FVector(R.Crates.X, R.Crates.Y, R.CrateTop) - Face2 * R.CrateHalf;
				P->CoopTestHand(0, Lip - R.Side * 20.0, true);
				P->CoopTestHand(1, Lip + R.Side * 20.0, true);
				P->GrabPoint = Lip - Face2 * 3.0 - FVector(0.0, 0.0, 4.0);
				P->LedgeTopZ = float(Lip.Z);
				P->LedgeIn = Face2;
				const bool bOk = P->StartMantle();
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the climb at the crates' lip (z %.0f) %s"), Lip.Z, bOk ? TEXT("started") : TEXT("refused"));
				P->CoopTestHand(0, FVector::ZeroVector, false);
				P->CoopTestHand(1, FVector::ZeroVector, false);
				T.At = Now + 2.0;
			}
		}
		else if (Id == TEXT("jump"))
		{
			// (user 2026-10-14: the card asks for two things and must wait for both) the jump first: no tick yet; then the
			// crouch (the stick held down), and the card is done.  One controller: the jump alone
			if (Try == 1) { P->DoJump(); T.At = Now + 1.5; }
			else if (Try == 2 && One < 0)
			{
				Check(TutorialCardId() == TEXT("jump") && !C->IsGood(), TEXT("jump: the jump alone leaves the crouch and jump card waiting"));
				P->bCrouchHeld = true;
				T.At = Now + 0.6;
			}
			else if (Try >= 3) { P->bCrouchHeld = false; T.At = Now + 0.6; }
		}
		else if (Id == TEXT("door"))
		{
			// walked up to the door (on the start's side of it), the hand's push; REQ-TUTDOORS: a push for each leaf of the pair
			if (Try == 1) { PutFeet(R.DoorAt + R.DoorOut * 140.0, R.DoorAt); T.At = Now + 0.6; }
			else if (Try == 3 && R.DoorSector2 >= 0)
			{
				Check(!C->IsGood(), TEXT("door: one leaf open leaves the card waiting for the other"));
				const bool bOk = PushDoorSector(R.DoorSector2);
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the hand's push on the other leaf (sector %d): %s"), R.DoorSector2, bOk ? TEXT("it moves") : TEXT("nothing"));
				T.At = Now + 1.0;
			}
			else if (Try == 2 || (Try > 3 && R.DoorSector2 < 0))
			{
				const bool bOk = PushDoorSector(R.DoorSector);
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the hand's push on the door (sector %d): %s"), R.DoorSector, bOk ? TEXT("it moves") : TEXT("nothing"));
				T.At = Now + 1.0;
			}
		}
		else if (Id == TEXT("lantern"))
		{
			Check(P->TutorialTestWheelItem(OffH, TEXT("ilantern")), TEXT("the lantern picked on the other hand's wheel"));
			if (Try >= 3 && !bLampOn)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the wheel's pick left the lantern out: lit as the item is used"));
				UseItem(TEXT("ilantern"));
			}
		}
		else if (Id == TEXT("grab") || Id == TEXT("grab_shoot"))
		{
			const int32 m = Id == TEXT("grab") ? 0 : 1;
			AOutlawsEnemy* E = R.Men[m].Get();
			if (!E) { Check(false, FString::Printf(TEXT("%s: the bad guy is there"), *Id)); Finish(); return; }
			// (REQ-TUTGRABFIST: the lantern from the card before is out of that hand; one controller keeps today's path)
			if (Id == TEXT("grab") && One < 0 && !T.Shot.Contains(TEXT("grab_hand")))
			{
				T.Shot.Add(TEXT("grab_hand"));
				Check(OLFlags::Param(TEXT("OLOldTutGrabFist")) || (P->Equipped[OffH] < 0 && P->HeldTool[OffH].IsEmpty() && R.Line.Contains(OffH == 0 ? TEXT("left") : TEXT("right"))),
					FString::Printf(TEXT("grab: the hand the card names is empty (weapon %d, tool \"%s\"): \"%s\""), P->Equipped[OffH], *P->HeldTool[OffH], *R.Line));
			}
			if (P->GrabbedEnemy[0].Get() != E && P->GrabbedEnemy[1].Get() != E)
			{
				if (!R.bFrozen[m]) { T.At = Now + 0.5; return; }            // (he comes, then holds still)
				Check(true, FString::Printf(TEXT("%s: he holds still for the card (%.0f cm off)"), *Id, FVector::Dist2D(E->GetActorLocation(), P->GetActorLocation())));
				// walked up to him (put before him), the free hand on his chest, the grip squeezed
				const FVector Fwd = (E->GetActorLocation() - P->GetActorLocation()).GetSafeNormal2D();
				PutFeet(E->GetActorLocation() - Fwd * 110.0 - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), E->GetActorLocation());
				const int32 Free = One >= 0 ? One : P->Equipped[0] < 0 ? 0 : P->Equipped[1] < 0 ? 1 : 0;
				P->CoopTestHand(Free, E->GrabChest(), true);
				Check(P->CoopTestGrab(Free, E), FString::Printf(TEXT("%s: taken by the %s hand (the grip kept squeezed)"), *Id, Free == 0 ? TEXT("left") : TEXT("right")));
				T.At = Now + 0.8;
				return;
			}
			if (m == 0 && One >= 0)
			{
				// (one controller: the grip let go, he is thrown)
				P->CoopTestHand(One, FVector::ZeroVector, false);
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: one controller: the grip let go of him"));
				T.At = Now + 0.8;
				return;
			}
			if (m == 1)
			{
				// the gun hand turned on him and fired
				const int32 Gun = P->GrabbedEnemy[0].Get() == E ? 1 : 0;
				if (P->Equipped[Gun] != RevolverIdx) P->TutorialTestWheelTake(Gun, RevolverIdx);
				if (P->Mag[Gun] <= 0) { LoadGun(Gun); T.At = Now + 1.5; return; }
				Aim(Gun, E->GetActorLocation() + FVector(0.0, 0.0, 20.0));
				P->TutorialTestAimGun(Gun, E->GetActorLocation() + FVector(0.0, 0.0, 10.0));
				FireAt(Gun, E->GetActorLocation() + FVector(0.0, 0.0, 10.0));
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: fired at the held man (try %d): %s"), Try, E->IsDead() ? TEXT("down") : TEXT("standing"));
				if (Try >= 6 && !E->IsDead())
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the aimed shots missed him: his hit applied"));
					E->TakeHit(5.f, E->GetActorLocation(), R.Dir, P->GetActorLocation());
				}
				T.At = Now + 0.7;
			}
		}
		else if (Id == TEXT("punch"))
		{
			const int32 Held = P->GrabbedEnemy[0].IsValid() ? 0 : 1;
			// (REQ-PUNCHGRIP) the card says to hold the grip for a fist, naming the punching hand; in VR its drawing marks
			// the other hand's grip
			const FString CardLine = R.Line;
			const OutlawsGlyphs::FGlyph& G = P->TutGlyph.Glyph;
			const bool bGlyph = !P->IsVR() || (G.Marks.Num() == 1 && G.Marks[0].Ctl == OutlawsGlyphs::ECtl::Grip && G.Marks[0].Hand == 1 - Held);
			const TCHAR* FistSide = Held == 0 ? TEXT("right") : TEXT("left");
			Check(CardLine.Contains(TEXT("hold the grip to make a fist")) && CardLine.Contains(FistSide) && bGlyph,
				FString::Printf(TEXT("the punch card says to hold the grip for a fist with the %s hand: \"%s\" (drawing %s)"), FistSide, *CardLine, *G.Describe()));
			Check(P->TutorialTestPunchHeld(1 - Held), TEXT("a punch from the other hand landed on the held man"));
			T.At = Now + 1.0;
		}
		else if (Id == TEXT("fight"))
		{
			{
				const int32 G = GunHand();
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: fight try %d: men %s %s, gun hand %d, rounds %d, reloading %d, ibullit %d"), Try,
					R.Men[2].IsValid() ? (R.Men[2]->IsDead() ? TEXT("dead") : TEXT("up")) : TEXT("none"), R.Men[3].IsValid() ? (R.Men[3]->IsDead() ? TEXT("dead") : TEXT("up")) : TEXT("none"),
					G, G >= 0 ? P->Mag[G] : -1, G >= 0 ? int32(P->bReloadingNow[G]) : -1, Counts.FindRef(TEXT("ibullit")));			}
			for (int32 m = 2; m < 4; ++m)
			{
				AOutlawsEnemy* E = R.Men[m].Get();
				if (!E || E->IsDead()) continue;
				int32 H = GunHand();
				if (H < 0) { P->TutorialTestWheelTake(DomH, RevolverIdx); H = GunHand(); }
				if (H < 0) break;
				if (P->Mag[H] <= 0) { LoadGun(H); T.At = Now + 1.5; return; }
				Face(E->GetActorLocation());
				Aim(H, E->GetActorLocation() + FVector(0.0, 0.0, 20.0));
				P->TutorialTestAimGun(H, E->GetActorLocation() + FVector(0.0, 0.0, 10.0));
				FireAt(H, E->GetActorLocation() + FVector(0.0, 0.0, 10.0));
				UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: fired at %s %.0f cm off (try %d): %s"), *E->GetName(), FVector::Dist(E->GetActorLocation(), P->GetActorLocation()), Try, E->IsDead() ? TEXT("down") : TEXT("standing"));
				if (Try >= 10 && !E->IsDead())
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the aimed shots missed %s: his hit applied"), *E->GetName());
					E->TakeHit(5.f, E->GetActorLocation(), R.Dir, P->GetActorLocation());
				}
				break;
			}
			T.At = Now + 0.6;
		}
		else if (Id == TEXT("heal"))
		{
			if (Try == 1) Check(Health < 10.f, FString::Printf(TEXT("heal: the player is hurt for the bag (health %.1f)"), Health));
			Check(P->TutorialTestWheelItem(DomH, TEXT("idocbag")), TEXT("the Medical Bag picked on the main hand's wheel"));
		}
		else if (Id == TEXT("pause"))
		{
			// the menu opened (the card hides under it), then shut
			if (Try == 1) { P->CoopOpenMenu(); T.At = Now + 1.0; }
			else if (Try == 2)
			{
				Check(P->bMenuOpen && !C->IsShown(), FString::Printf(TEXT("pause: the pause menu is up and the card hides under it (%s)"), *Page));
				P->CloseMenu();
				T.At = Now + 0.8;
			}
			else if (Try >= 3 && P->bMenuOpen) P->CloseMenu();
		}
		else if (Id == TEXT("horse"))
		{
			// walked to the horse, a hand on its back, the grip squeezed
			PutFeet(R.HorseAt - R.Dir * 120.0, R.HorseAt);
			P->CoopTestHand(1, R.HorseAt + FVector(0.0, 0.0, 130.0) - R.Dir * 40.0, true);
			T.At = Now + 0.5;
		}
		return;
	}
	case 6:
	{
		// finished: the title menu again, its line, no tutorial
		if (!bFrontMenu)
		{
			if (Now - T.CardSeen > 60.0) { Check(false, TEXT("the title menu came back within 60 s")); Finish(); }
			return;
		}
		P->CoopTestHand(1, FVector::ZeroVector, false);
		Check(!bTutorial && !TutorialRun.IsValid(), TEXT("finished: back at the title menu, no tutorial running"));
		Check(P->MenuToastLeft > 0.f, TEXT("finished: the title menu shows its line (Tutorial complete)"));
		// (one controller has no punch card, the third Back's station: two)
		const int32 BacksWanted = P->OneHand() >= 0 ? 2 : 3;
		if (Mode == TEXT("walk")) Check(T.Backed.Num() == BacksWanted, FString::Printf(TEXT("Back pressed from %d stations (%d)"), BacksWanted, T.Backed.Num()));
		Shot(TEXT("complete"));
		T.Phase = 7;
		T.At = Now + 1.0;
		return;
	}
	case 7:
		Finish();
		return;
	case 10:
	{
		// (menu) each other row opens its page, then Back.  REQ-MPMENU: the Multiplayer walk, New Game > Multiplayer > Co-op >
		// Host / Join and > Competitive > Host / Join, each page's rows and title checked and photographed, and Back from each
		// (the B / Y back, or the page's own Back row) up one level.  Press: a row's label, or empty for the B / Y back
		struct FStep { const TCHAR* Press; const TCHAR* Page; const TCHAR* Rows; const TCHAR* Title; const TCHAR* Shot; };
		static const FStep Steps[] = {
			{ TEXT("Main Game"), TEXT("#difficulty:story"), nullptr, nullptr, nullptr },
			{ TEXT(""), TEXT("#newgame"), nullptr, nullptr, nullptr },
			{ TEXT("Historical Missions"), TEXT("#missions"), nullptr, nullptr, nullptr },
			{ TEXT(""), TEXT("#newgame"), nullptr, nullptr, nullptr },
			{ TEXT("Multiplayer"), TEXT("#coopmulti"), TEXT("Co-op | Competitive | Back"), TEXT("MULTIPLAYER"), TEXT("mpmenu_multiplayer") },
			{ TEXT("Co-op"), TEXT("#coop"), TEXT("New Game | Load Game | Join Game | Back"), TEXT("CO-OP"), TEXT("mpmenu_coop") },       // (REQ-COOPSAVES)
			{ TEXT("Join Game"), TEXT("#coopjoin"), nullptr, TEXT("JOIN A CO-OP GAME"), TEXT("mpmenu_join_coop") },
			{ TEXT(""), TEXT("#coop"), nullptr, nullptr, nullptr },
			{ TEXT("Back"), TEXT("#coopmulti"), nullptr, nullptr, nullptr },
			{ TEXT("Competitive"), TEXT("#coopcomp"), TEXT("Host Game | Join Game | Back"), TEXT("COMPETITIVE"), TEXT("mpmenu_competitive") },
			{ TEXT("Host Game"), TEXT("#mphost"), nullptr, TEXT("HOST GAME"), nullptr },
			{ TEXT(""), TEXT("#coopcomp"), nullptr, nullptr, nullptr },
			{ TEXT("Join Game"), TEXT("#coopjoin"), nullptr, TEXT("JOIN A MATCH"), TEXT("mpmenu_join_match") },
			{ TEXT(""), TEXT("#coopcomp"), nullptr, nullptr, nullptr },
			{ TEXT("Back"), TEXT("#coopmulti"), nullptr, nullptr, nullptr },
			{ TEXT("Back"), TEXT("#newgame"), nullptr, nullptr, nullptr },
			// REQ-EXTRAS: the title's Extras and each of its pages, Back from each
			{ TEXT(""), TEXT("#front"), nullptr, nullptr, nullptr },
			{ TEXT("Extras"), TEXT("#extras"), TEXT("Achievements | Collectibles | Credits"), TEXT("EXTRAS"), TEXT("extras_page") },
			{ TEXT("Achievements"), TEXT("#extras_achiev"), nullptr, TEXT("ACHIEVEMENTS"), TEXT("extras_achievements") },
			{ TEXT(""), TEXT("#extras"), nullptr, nullptr, nullptr },
			{ TEXT("Collectibles"), TEXT("#extras_shelf"), TEXT("? | ? | ? | ? | ? | ? | ? | ? | ?"), TEXT("COLLECTIBLES"), TEXT("extras_collectibles") },
			{ TEXT(""), TEXT("#extras"), nullptr, nullptr, nullptr },
			{ TEXT(""), TEXT("#front"), nullptr, nullptr, nullptr },
			{ TEXT("New Game"), TEXT("#newgame"), nullptr, nullptr, nullptr },
		};
		const int32 NumSteps = UE_ARRAY_COUNT(Steps);
		if (T.MenuStep < NumSteps * 2)
		{
			const FStep& St = Steps[T.MenuStep / 2];
			if (T.MenuStep % 2 == 0)
			{
				if (St.Press[0] == 0) P->CoopMenuBack();
				else Check(P->TutorialTestPressLabel(St.Press), FString::Printf(TEXT("%s pressed on %s (%s)"), St.Press, *Page, *P->TutorialTestRows()));
				T.At = Now + 0.8;
			}
			else
			{
				const FString Rows = P->TutorialTestRows();
				const FString Title = P->MenuPage ? P->MenuPage->Title : FString();
				Check(Page == St.Page, FString::Printf(TEXT("%s opened %s (%s): %s"), St.Press[0] ? St.Press : TEXT("B / Y back"), St.Page, *Page, *Rows));
				if (St.Rows) Check(Rows == St.Rows, FString::Printf(TEXT("%s reads %s (%s)"), St.Page, St.Rows, *Rows));
				if (St.Title) Check(Title == St.Title, FString::Printf(TEXT("%s is titled %s (%s)"), St.Page, St.Title, *Title));
				if (St.Shot)
				{
					// (every row and line inside the parchment, above the help line)
					FString Fit;
					Check(P->MenuPageFits(Fit), FString::Printf(TEXT("%s fits (%s)"), St.Page, *Fit));
					Shot(St.Shot);
				}
				T.At = Now + 0.8;
			}
			++T.MenuStep;
			return;
		}
		Check(Page == TEXT("#newgame"), FString::Printf(TEXT("back on New Game's page (%s)"), *Page));
		Check(P->TutorialTestPressLabel(TEXT("Historical Missions")), TEXT("Historical Missions pressed"));
		T.Phase = 11;
		T.At = Now + 0.8;
		return;
	}
	case 11:
		Check(P->TutorialTestPressLabel(TEXT("Ice Caves")), FString::Printf(TEXT("Ice Caves pressed (%s)"), *P->TutorialTestRows()));
		T.Phase = 12;
		T.At = Now + 0.8;
		return;
	case 12:
		Check(P->TutorialTestPressLabel(TEXT("Good")), TEXT("Good pressed: Ice Caves starts"));
		T.Phase = 13;
		T.At = Now + 1.0;
		T.CardSeen = Now;
		return;
	case 13:
		if (bFrontEnd || bRebuildPending || bNeedPlace || !Inf)
		{
			if (Now - T.CardSeen > 90.0) { Check(false, TEXT("Ice Caves built within 90 s")); Finish(); }
			return;
		}
		Check(LevelName.Equals(TEXT("icecaves"), ESearchCase::IgnoreCase) && !bTutorial, FString::Printf(TEXT("Historical Missions > Ice Caves is up (%s), no tutorial"), *LevelName));
		Shot(TEXT("icecaves"));
		T.Phase = 7;
		T.At = Now + 1.5;
		return;
	case 20:
	{
		// (skip) the pause page: Skip tutorial on top
		P->CoopOpenMenu();
		T.Phase = 21;
		T.At = Now + 1.0;
		return;
	}
	case 21:
	{
		const FString Rows = P->TutorialTestRows();
		UE_LOG(LogOutlaws, Display, TEXT("OLTUT test: the pause page in the tutorial: %s"), *Rows);
		Check(P->bMenuOpen && P->MenuPage && P->MenuPage->Items.Num() > 0 && P->MenuPage->Items[0].Label == TEXT("Skip tutorial"), TEXT("Skip tutorial is the pause page's top row"));
		Check(!Rows.Contains(TEXT("Save")) && !Rows.Contains(TEXT("Load")), TEXT("no Save or Load rows in the tutorial"));
		Check(!Rows.Contains(TEXT("Tools")) && !Rows.Contains(TEXT("Unstuck")), TEXT("no Tools or Unstuck row in the tutorial (REQ-NOUNSTUCK)"));
		Shot(TEXT("pause"));
		T.Phase = 22;
		T.At = Now + 0.8;
		return;
	}
	case 22:
		Check(P->TutorialTestPressLabel(TEXT("Skip tutorial")), TEXT("Skip tutorial pressed"));
		T.Phase = 23;
		T.At = Now + 0.5;
		T.CardSeen = Now;
		return;
	case 23:
		if (!bFrontMenu)
		{
			if (Now - T.CardSeen > 60.0) { Check(false, TEXT("skipped: the title menu came back within 60 s")); Finish(); }
			return;
		}
		Check(!bTutorial && !TutorialRun.IsValid(), TEXT("skipped: back at the title menu, no tutorial running"));
		Shot(TEXT("skipped"));
		T.Phase = 7;
		T.At = Now + 1.0;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
