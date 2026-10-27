// BUG-SHOVELWHACK: -OLToolWhackProbe swings the shovel and the crowbar (and the fist, which the fix leaves as it was on bad
// guys) at bad guys and at other players on the desktop (any build, Shipping too, as -OLShipSmoke and -OLFirstUseProbe:
// through OLFlags, not under WITH_OUTLAWS_TESTS; nothing runs without the flag).  Each case stands one bad guy (or, in a
// match, a bot) before the player, puts the tool in the right hand with its grip held and moves the hand frame through a
// swing a frame at a time, calling UpdateTools (the fist: UpdatePunches) as the game's tick does; the bad guy's health (or
// the match's strikes) before and after says whether the blow landed.  Lines "OLWHACK <level> <case>: PASS|FAIL|SKIP ..."
// and "OLWHACK RESULT".  With -OLShipSmoke each level is played until its cases are done (OutlawsShipSmoke::HoldLevel);
// without it the game quits after the first level's cases.  See OutlawsPlayer.h (TickToolWhackProbe).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsShipSmoke.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"

namespace
{
	// how the tool travels: across (laid level, turned past him), chopped (from up to down), resting on him, carried into him;
	// the fist: a punch (a jab straight at him), or resting (pushed slowly into him)
	enum class EWhackSwing : uint8 { Across, Chop, Rest, WalkInto, Punch };
	enum class EWhackWho : uint8 { Plain, Extra, OtherKind, Player };
	enum class EWhackAim : uint8 { Head, Body, Low };
	struct FWhackCase
	{
		const TCHAR* Tool = TEXT("ishovel");   // "ishovel", "icrowbar" or "fist"
		EWhackWho Who = EWhackWho::Plain;
		EWhackSwing Swing = EWhackSwing::Across;
		EWhackAim Aim = EWhackAim::Head;
		double Range = 80.0;        // cm from the eye to his middle line
		bool bDuck = false;         // ducking as the blow comes
		bool bAfterDig = false;     // the spade driven into townex's DIG_ME first (through UpdateTools), then this blow
		bool bExpectHit = true;     // false: a hit is the FAIL (resting, walking into him, a fist out of reach)
		bool IsFist() const { return Tool[0] == TEXT('f'); }
		FString Name() const
		{
			const TCHAR* Tl = IsFist() ? TEXT("fist") : Tool[1] == TEXT('s') ? TEXT("shovel") : TEXT("crowbar");
			const TCHAR* Sw = Swing == EWhackSwing::Across ? TEXT("across") : Swing == EWhackSwing::Chop ? TEXT("chopped") : Swing == EWhackSwing::Rest ? TEXT("resting")
				: Swing == EWhackSwing::Punch ? TEXT("punch") : TEXT("walked into");
			const TCHAR* Am = Aim == EWhackAim::Head ? TEXT("head") : Aim == EWhackAim::Body ? TEXT("body") : TEXT("low");
			const TCHAR* Wo = Who == EWhackWho::Extra ? TEXT(", a multiplied copy") : Who == EWhackWho::OtherKind ? TEXT(", another kind") : Who == EWhackWho::Player ? TEXT(", another player") : TEXT("");
			const bool bFar = !bExpectHit && (Swing == EWhackSwing::Across || Swing == EWhackSwing::Chop || Swing == EWhackSwing::Punch);
			return FString::Printf(TEXT("%s %s %s at %.0f cm%s%s%s%s"), Tl, Sw, Am, Range, bDuck ? TEXT(", ducking") : TEXT(""), Wo,
				bAfterDig ? TEXT(", straight after a dig") : TEXT(""), bFar ? TEXT(", out of reach") : TEXT(""));
		}
	};
	FWhackCase& AddCase(TArray<FWhackCase>& Out, const TCHAR* Tool, EWhackWho Who, EWhackSwing S, EWhackAim A, double R, bool bDuck, bool bHit)
	{
		FWhackCase& K = Out.AddDefaulted_GetRef();
		K.Tool = Tool;
		K.Who = Who;
		K.Swing = S;
		K.Aim = A;
		K.Range = R;
		K.bDuck = bDuck;
		K.bExpectHit = bHit;
		return K;
	}
	// each tool across at the head, the body and low, 40, 80 and 120 cm off, standing and ducking; chopped at the head; resting
	// on him; carried into him.  The fist the same way (a punch; 120 cm is out of an arm's reach), and resting on him
	void AddMatrix(TArray<FWhackCase>& Out, EWhackWho Who, bool bAllChops)
	{
		for (const TCHAR* Tool : { TEXT("ishovel"), TEXT("icrowbar") })
		{
			for (const bool bDuck : { false, true })
				for (const double R : { 40.0, 80.0, 120.0 })
					for (const EWhackAim A : { EWhackAim::Head, EWhackAim::Body, EWhackAim::Low })
						AddCase(Out, Tool, Who, EWhackSwing::Across, A, R, bDuck, true);
			if (bAllChops) for (const double R : { 40.0, 80.0, 120.0 }) AddCase(Out, Tool, Who, EWhackSwing::Chop, EWhackAim::Head, R, false, true);
			else AddCase(Out, Tool, Who, EWhackSwing::Chop, EWhackAim::Head, 80.0, false, true);
			AddCase(Out, Tool, Who, EWhackSwing::Rest, EWhackAim::Body, 80.0, false, false);
			AddCase(Out, Tool, Who, EWhackSwing::WalkInto, EWhackAim::Body, 120.0, false, false);
		}
		for (const bool bDuck : { false, true })
			for (const double R : { 40.0, 80.0, 120.0 })
				for (const EWhackAim A : { EWhackAim::Head, EWhackAim::Body, EWhackAim::Low })
					AddCase(Out, TEXT("fist"), Who, EWhackSwing::Punch, A, R, bDuck, R < 100.0);
		AddCase(Out, TEXT("fist"), Who, EWhackSwing::Rest, EWhackAim::Body, 40.0, false, false);
	}
	TArray<FWhackCase> StoryCases()
	{
		TArray<FWhackCase> Out;
		AddMatrix(Out, EWhackWho::Plain, true);
		AddCase(Out, TEXT("ishovel"), EWhackWho::Extra, EWhackSwing::Across, EWhackAim::Head, 80.0, false, true);
		AddCase(Out, TEXT("ishovel"), EWhackWho::OtherKind, EWhackSwing::Across, EWhackAim::Head, 80.0, false, true);
		AddCase(Out, TEXT("ishovel"), EWhackWho::Plain, EWhackSwing::Across, EWhackAim::Head, 80.0, false, true).bAfterDig = true;
		return Out;
	}
	TArray<FWhackCase> MatchCases()
	{
		TArray<FWhackCase> Out;
		AddMatrix(Out, EWhackWho::Player, false);
		return Out;
	}
	constexpr float WhackLead = 3.f;           // s of play after the build before the first case (a match: 8 s, past the spawn guard)
	constexpr float WhackSettle = 0.5f;        // s between standing him there and the swing (his picture placed; a bot's puppet there)
	constexpr float WhackFrame = 1.f / 72.f;   // the swing's frame (a Quest 2 frame)
	constexpr int32 WhackFrames = 12;          // frames a swing takes (about 1/6 s)
	constexpr double WalkSpeed = 450.0;        // cm/s, walking into him with the tool held out
	constexpr double PunchSpeed = 400.0;       // cm/s, a jab (over HitSpeedHard's 250)
	constexpr double RestSpeed = 60.0;         // cm/s, a fist pushed slowly into him
	constexpr double PunchArm = 62.0;          // cm, the knuckles' furthest out of the eye
	constexpr float CrouchHalf = 55.f;         // a crouching player's capsule half height (a puppet is drawn crouched under 60)

	struct FWhackProbe
	{
		TWeakObjectPtr<AOutlawsLevelActor> Level;
		FString LevelName;
		TArray<FWhackCase> Cases;
		bool bMatch = false;
		float T = 0.f;
		int32 Case = 0;
		int32 Stage = 0;                       // 0 pick and stand him there, 1 swing
		float NextAt = 0.f;
		bool bDone = false;
		bool bDug = false;
		bool bFaced = false;
		TWeakObjectPtr<AOutlawsEnemy> Man;     // a bad guy, or a bot's body in a match
		int32 Who = -1;                        // the bot's player number in a match
		FVector ManFrom = FVector::ZeroVector; // where he stood before the case (he goes back there after it)
		TSet<TWeakObjectPtr<AOutlawsEnemy>> Used;
		int32 Pass = 0, Fail = 0, Skip = 0;
	};
	FWhackProbe GWhack;
	int32 GWhackLevels = 0, GWhackPassAll = 0, GWhackFailAll = 0;

	// (as OutlawsPlayerWeapons.cpp's BodyGap: the stretch A..B from his capsule's upright middle line, less its radius)
	double GapTo(const AOutlawsEnemy* E, const FVector& A, const FVector& B)
	{
		const UCapsuleComponent* Cap = E->GetCapsuleComponent();
		const FVector C = E->GetActorLocation();
		const FVector Z(0.0, 0.0, FMath::Max(0.f, Cap->GetScaledCapsuleHalfHeight() - Cap->GetScaledCapsuleRadius()));
		FVector P, Q;
		FMath::SegmentDistToSegmentSafe(A, B, C - Z, C + Z, P, Q);
		return FVector::Dist(P, Q) - Cap->GetScaledCapsuleRadius();
	}

	bool IsPlainKind(const AOutlawsEnemy* E) { return E->GetOutlawsName().StartsWith(TEXT("BGY")); }

	// the bot's body that draws as player Who in a match (hidden; its puppet is what shows)
	AOutlawsEnemy* BotBody(UWorld* W, int32 Who)
	{
		for (TActorIterator<AOutlawsEnemy> It(W); It; ++It) if (It->CoopSelf == Who && !It->IsDead()) return *It;
		return nullptr;
	}
}

bool AOutlawsPlayer::ToolWhackProbeOn()
{
	static const bool bOn = OLFlags::Param(TEXT("OLToolWhackProbe"));
	return bOn;
}

void AOutlawsPlayer::TickToolWhackProbe(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen || !Camera) return;
	FWhackProbe& P = GWhack;
	UOutlawsCoop* Co = UOutlawsCoop::IsActive(L) ? UOutlawsCoop::Get(L) : nullptr;
	if (P.Level.Get() != L || P.LevelName != L->LevelName)
	{
		P = FWhackProbe();
		P.Level = L;
		P.LevelName = L->LevelName;
		P.bMatch = Co && Co->IsMatch();
		P.Cases = P.bMatch ? MatchCases() : StoryCases();
		P.NextAt = P.bMatch ? 8.f : WhackLead;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHACK %s: probe armed%s, %d bad guys, %d cases"), *P.LevelName, P.bMatch ? TEXT(" (a match)") : TEXT(""), L->GetEnemies().Num(), P.Cases.Num());
	}
	if (P.bDone) return;
	OutlawsShipSmoke::HoldLevel(3.0);      // (-OLShipSmoke: this level is played on until the cases are done)
	P.T += float(FApp::GetDeltaTime());
	L->bInvincible = true;
	// (a bot ducking for its case kept down until the swing: its own play would stand it up)
	if (P.Stage == 1 && P.Cases.IsValidIndex(P.Case) && P.Cases[P.Case].bDuck && P.Cases[P.Case].Who == EWhackWho::Player)
	{
		if (AOutlawsEnemy* M = P.Man.Get(); M && !M->IsDead()) M->DuckForProbe(CrouchHalf);
	}
	if (P.T < P.NextAt) return;
	const FString& Lv = P.LevelName;
	const FVector Up(0.0, 0.0, 1.0);
	if (!P.bFaced)
	{
		// facing the clearest way: the longest free line at the eye's height and at the knees over eight headings
		const FVector Eye = Camera->GetComponentLocation();
		const double KneeZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 45.0;
		double BestYaw = GetActorRotation().Yaw, Best = -1.0;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsWhackProbeFace), false, this);
		for (int32 i = 0; i < 8; ++i)
		{
			const double Yaw = i * 45.0;
			const FVector D = FRotator(0.0, Yaw, 0.0).Vector();
			double Free = 1e9;
			for (const double Z : { Eye.Z, KneeZ })
			{
				const FVector S(Eye.X, Eye.Y, Z);
				FHitResult H;
				Free = FMath::Min(Free, GetWorld()->LineTraceSingleByChannel(H, S, S + D * 250.0, ECC_Visibility, Q) ? double(H.Distance) : 250.0);
			}
			if (Free > Best) { Best = Free; BestYaw = Yaw; }
		}
		if (Controller) Controller->SetControlRotation(FRotator(0.0, BestYaw, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLWHACK %s: facing yaw %.0f (%.0f cm clear)"), *Lv, BestYaw, Best);
		P.bFaced = true;
		P.NextAt = P.T + 0.5f;
		return;
	}
	auto Finish = [&]()
	{
		P.bDone = true;
		++GWhackLevels;
		GWhackPassAll += P.Pass;
		GWhackFailAll += P.Fail;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHACK %s done: %d passed, %d failed, %d skipped"), *Lv, P.Pass, P.Fail, P.Skip);
		UE_LOG(LogOutlaws, Display, TEXT("OLWHACK RESULT %s: %d levels, %d passed, %d failed"), GWhackFailAll == 0 ? TEXT("PASS") : TEXT("FAIL"), GWhackLevels, GWhackPassAll, GWhackFailAll);
		TestAxes.Remove(TEXT("OLVR_RightGrip"));
		if (!OutlawsShipSmoke::Active()) FPlatformMisc::RequestExit(false);
	};
	if (P.Case >= P.Cases.Num())
	{
		Finish();
		return;
	}
	const FWhackCase& K = P.Cases[P.Case];
	const FString Name = K.Name();
	auto PutBack = [&]()
	{
		AOutlawsEnemy* M = P.Man.Get();
		if (!M || M->IsDead()) return;
		if (K.bDuck) M->StandForProbe();
		M->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		M->SetActorLocation(P.ManFrom, false, nullptr, ETeleportType::TeleportPhysics);
		M->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	};
	auto Next = [&](const TCHAR* Verdict, const FString& Why)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLWHACK %s %s: %s: %s"), *Lv, *Name, Verdict, *Why);
		PutBack();
		++P.Case;
		P.Stage = 0;
		P.Man.Reset();
		P.Who = -1;
		P.NextAt = P.T + 0.2f;
	};
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	const FVector Side = FVector::CrossProduct(Up, F);
	const double PlayerFeetZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const bool bPlayer = K.Who == EWhackWho::Player;

	if (P.Stage == 0)
	{
		if (K.bAfterDig && !P.bDug)
		{
			// the spade into DIG_ME first, as the user did (the bank's floor in townex): stood beside it, the shovel held
			// upright over the spot and driven down through the floor in one frame
			const FOutlawsLevel* Live = L->LiveLevel();
			const FOutlawsSector* Dig = nullptr;
			if (Live) for (const FOutlawsSector& S : Live->Sectors) if (S.Name.Equals(TEXT("DIG_ME"), ESearchCase::IgnoreCase)) { Dig = &S; break; }
			if (!Dig)
			{
				++P.Skip;
				Next(TEXT("SKIP"), TEXT("no DIG_ME on this level"));
				return;
			}
			FVector2D C = FVector2D::ZeroVector;
			for (const FVector2D& V : Dig->Verts) C += V;
			C /= FMath::Max(1, Dig->Verts.Num());
			const FVector Spot = FOutlawsGeometry::ToUE(C.X, Dig->Floor.Y, C.Y);
			L->AddCount(TEXT("ishovel"), 1);
			EquipTool(1, TEXT("ishovel"));
			TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);
			TeleportTo(Spot - F * 45.0 + Up * (GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.0), GetActorRotation(), false, true);
			GetCharacterMovement()->StopMovementImmediately();
			const double Tip = ToolTipX[1];
			auto Place = [&](double Lift)
			{
				const FTransform Want(FRotationMatrix::MakeFromX(-Up).ToQuat(), Spot + Up * (Tip + Lift));
				FrameOf(1)->SetWorldTransform(ToolRest(1).Inverse() * Want);
			};
			ToolCooldown[1] = 0.f;
			bToolTipValid[1] = false;
			Place(20.0);
			UpdateTools(WhackFrame);
			Place(-6.0);
			UpdateTools(WhackFrame);
			bToolTipValid[1] = false;
			TestAxes.Remove(TEXT("OLVR_RightGrip"));
			UE_LOG(LogOutlaws, Display, TEXT("OLWHACK %s: the spade driven into DIG_ME (sector %d) at %s; cooldown after it %.2f s"), *Lv, Dig->Index, *Spot.ToCompactString(), ToolCooldown[1]);
			P.bDug = true;
			P.bFaced = false;                     // (facing the clearest way again where it stands now)
			P.NextAt = P.T + 1.0f;                // (the user's whack came after the dig, not in its cooldown)
			return;
		}
		AOutlawsEnemy* Pick = nullptr;
		if (bPlayer)
		{
			// a bot's body, its puppet drawn where the body is
			for (int32 Who = 0; Who < UOutlawsCoop::MaxPlayers && !Pick; ++Who)
			{
				if (!Co || Who == Co->MyIndex() || !Co->PuppetOf(Who)) continue;
				if (AOutlawsEnemy* B = BotBody(GetWorld(), Who)) { Pick = B; P.Who = Who; }
			}
		}
		else
		{
			// a bad guy: alive, seen (not a bot's body), not a named outlaw, a voice or a critter; one not used before first,
			// else one a blow left standing
			for (int32 Round = 0; Round < 2 && !Pick; ++Round)
				for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
				{
					AOutlawsEnemy* E = Wk.Get();
					if (!E || E->IsDead() || E->IsHidden() || E->CoopSelf >= 0 || E->bBoss || E->IsVoiceOnly() || E->IsCritter() || E->bHeldByPlayer || (Round == 0 && P.Used.Contains(Wk))) continue;
					if (K.Who == EWhackWho::Extra && E->CoopCopyOf < 0) continue;
					if (K.Who == EWhackWho::OtherKind && (IsPlainKind(E) || E->CoopCopyOf >= 0)) continue;
					if (K.Who == EWhackWho::Plain && E->CoopCopyOf >= 0) continue;
					if (K.bDuck && (E->Def.DuckChor < 0 || !E->GetSprite() || !E->GetSprite()->Chors.IsValidIndex(E->Def.DuckChor))) continue;
					Pick = E;
					break;
				}
		}
		if (!Pick)
		{
			++P.Skip;
			Next(TEXT("SKIP"), K.Who == EWhackWho::Extra ? TEXT("no multiplied copies here (-OLSoloBadGuys)") : K.Who == EWhackWho::OtherKind ? TEXT("no bad guy of another kind here (-OLRando)")
				: bPlayer ? TEXT("no other player here (a match with bots)") : TEXT("no bad guy left"));
			return;
		}
		if (!bPlayer) P.Used.Add(Pick);
		P.Man = Pick;
		P.ManFrom = Pick->GetActorLocation();
		if (K.IsFist())
		{
			DropTool(1);
		}
		else
		{
			L->AddCount(K.Tool, 1);
			EquipTool(1, K.Tool);
		}
		Pick->SetDormant(false);
		Pick->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);     // (not shoved off by the player's body)
		Pick->SetActorLocation(FVector(Eye.X, Eye.Y, PlayerFeetZ) + F * K.Range + Up * Pick->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), false, nullptr, ETeleportType::TeleportPhysics);
		Pick->SetActorRotation((-F).Rotation());
		Pick->GetCharacterMovement()->DisableMovement();
		// another player ducking: the bot's body as short as a crouching player's, so its puppet comes in crouched
		if (bPlayer && K.bDuck) Pick->DuckForProbe(CrouchHalf);
		P.Stage = 1;
		P.NextAt = P.T + (bPlayer ? 1.2f : WhackSettle);
		return;
	}

	// the swing
	AOutlawsEnemy* E = P.Man.Get();
	if (!E || E->IsDead())
	{
		++P.Skip;
		Next(TEXT("SKIP"), TEXT("the bad guy was gone before the swing"));
		return;
	}
	const AOutlawsRemotePlayer* R = bPlayer && Co ? Co->PuppetOf(P.Who) : nullptr;
	if (bPlayer && !R)
	{
		++P.Skip;
		Next(TEXT("SKIP"), TEXT("the bot's puppet was gone before the swing"));
		return;
	}
	const bool bDucking = K.bDuck && (bPlayer ? R->ShownHalf() < 63.f : E->DuckForProbe());
	if (K.bDuck && !bDucking)
	{
		++P.Skip;
		Next(TEXT("SKIP"), bPlayer ? FString::Printf(TEXT("the puppet of player %d was not drawn crouched (half %.0f)"), P.Who, R->ShownHalf())
			: FString::Printf(TEXT("%s has no duck"), *E->GetOutlawsName()));
		return;
	}
	// what is drawn: the bad guy's picture, or the puppet's; his middle line, his feet and the drawing's height over them
	const AOutlawsSpriteActor* Spr = bPlayer ? R->ShownSprite() : E->GetSprite();
	const UCapsuleComponent* Cap = E->GetCapsuleComponent();
	const FVector Mid = bPlayer ? R->ShownBody() : E->GetActorLocation();
	const double FeetZ = Spr ? Spr->GetActorLocation().Z : (bPlayer ? Mid.Z - R->ShownHalf() : Mid.Z - Cap->GetScaledCapsuleHalfHeight());
	const double DrawnH = Spr ? Spr->DrawnHeightCm() : 2.0 * (bPlayer ? R->ShownHalf() : Cap->GetScaledCapsuleHalfHeight());
	const double BodyTop = bPlayer ? Mid.Z + FMath::Max(40.0, double(R->ShownHalf())) : Mid.Z + Cap->GetScaledCapsuleHalfHeight();
	// the picture's top as it stands drawn (its quad's place and scale: a little under DrawnH when it is drawn nearer the eye
	// and scaled down), looked for over his middle and up to 60 cm toward the eye; the aim goes by it
	double PicH = 0.0;
	if (Spr)
	{
		const FVector ToEye = FVector(Eye.X - Mid.X, Eye.Y - Mid.Y, 0.0).GetSafeNormal();
		for (int32 d = 0; d <= 12; ++d)
			for (int32 z = 130; z > 0 && 2.0 * z > PicH; --z)
				if (Spr->DrawnTouches(FVector(Mid.X, Mid.Y, FeetZ) + ToEye * (5.0 * d) + FVector(0.0, 0.0, 2.0 * z), 3.0)) { PicH = 2.0 * z; break; }
	}
	if (PicH <= 0.0) PicH = DrawnH;
	// the head's middle (the top fifteenth of the figure), the chest, the knees
	const double AimZ = FeetZ + PicH * (K.Aim == EWhackAim::Head ? 0.94 : K.Aim == EWhackAim::Body ? 0.55 : 0.22);
	const FVector Aim(Mid.X, Mid.Y, AimZ);
	const double Range = FVector::Dist2D(Eye, Mid);
	const float Before = E->GetHealth();
	const int32 StrikesBefore = UOutlawsCoop::MatchStrikesSent();
	const int32 DealtBefore = Co ? Co->MatchHitsDealtSoFar() : 0;
	const int32 GripWas = TestAxes.Contains(TEXT("OLVR_RightGrip")) ? 1 : 0;
	TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);
	auto Landed = [&]() { return bPlayer ? UOutlawsCoop::MatchStrikesSent() > StrikesBefore : E->GetHealth() < Before; };
	// the others within 3 m (a crowd with the multiplier): a blow one of them takes first is not his
	TArray<TPair<TWeakObjectPtr<AOutlawsEnemy>, float>> Near;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
	{
		const AOutlawsEnemy* O = Wk.Get();
		if (O && O != E && !O->IsDead() && FVector::Dist(O->GetActorLocation(), Eye) < 300.0) Near.Emplace(Wk, O->GetHealth());
	}
	bool bInPicture = false;
	int32 LandedAt = -1, Frames = WhackFrames;
	FString HandNote;
	if (K.IsFist())
	{
		// the fist (UpdatePunches): the knuckles driven straight at his aim from 15 cm out of the eye to arm's length (or 5
		// cm short of his middle line) at a jab's speed; resting, pushed slowly the last 7 cm into him.  The right hand bare
		// for it (no knife or mounted gun in it), the desktop taken as VR for it
		const bool bRest = K.Swing == EWhackSwing::Rest;
		const double OutTo = FMath::Min(PunchArm, Range - 5.0);
		const double OutFrom = bRest ? OutTo - 7.0 : 15.0;
		const FVector Base(Eye.X, Eye.Y, AimZ);
		const FVector From = Base + F * OutFrom, To = Base + F * OutTo;
		const double Speed = bRest ? RestSpeed : PunchSpeed;
		Frames = FMath::Max(2, FMath::CeilToInt(FVector::Dist(From, To) / (Speed * WhackFrame)));
		const FVector Dir = (To - From).GetSafeNormal();
		const int32 EquippedWas = Equipped[1];
		const bool bVRWas = bVR;
		Equipped[1] = -1;
		bVR = true;
		PunchCool[1] = 0.f;
		for (int32 i = 0; i <= Frames; ++i)
		{
			const FVector Knuckles = FMath::Lerp(From, To, double(i) / Frames);
			GripOf(1)->SetWorldLocation(Knuckles - Dir * 5.0);
			HandVelocity[1] = Dir * Speed;
			UpdatePunches(WhackFrame);
			if (Spr && !bInPicture) bInPicture = Spr->DrawnTouches(Knuckles, 22.0);
			if (LandedAt < 0 && Landed()) LandedAt = i;
		}
		HandVelocity[1] = FVector::ZeroVector;
		bVR = bVRWas;
		Equipped[1] = EquippedWas;
		HandNote = FString::Printf(TEXT("; the knuckles %.0f to %.0f cm out at %.0f cm/s"), OutFrom, OutTo, Speed);
	}
	else
	{
		const double Tip = ToolTipX[1];
		// the fist: 30 cm out from the eye, 18 cm when he stands face to face, the arm out (45 cm) for one 120 cm off; over
		// the aim for a chop
		const double Reach = Range >= 100.0 ? 45.0 : FMath::Min(30.0, FMath::Max(15.0, Range - 22.0));
		const FVector Fist0 = FVector(Eye.X, Eye.Y, 0.0) + F * Reach + FVector(0.0, 0.0, AimZ + (K.Swing == EWhackSwing::Chop ? 12.0 : 0.0));
		const FVector ToHim = FVector(Aim.X - Fist0.X, Aim.Y - Fist0.Y, 0.0).GetSafeNormal();
		// the arc about the fist: across, the tool laid level and turned from 50 degrees to one side to 50 to the other; a
		// chop, from pointing 60 degrees up to 60 down, through the line to his middle; resting and walking in, pointing at him
		auto DirAt = [&](double A)
		{
			const double Rd = FMath::DegreesToRadians(A);
			return K.Swing == EWhackSwing::Across ? (ToHim * FMath::Cos(Rd) + Side * FMath::Sin(Rd)).GetSafeNormal()
				: K.Swing == EWhackSwing::Chop ? (ToHim * FMath::Cos(Rd) + Up * FMath::Sin(Rd)).GetSafeNormal() : ToHim;
		};
		const double A0 = K.Swing == EWhackSwing::Across ? -50.0 : K.Swing == EWhackSwing::Chop ? 60.0 : 0.0, A1 = -A0;
		Frames = K.Swing == EWhackSwing::WalkInto ? 16 : WhackFrames;
		// walking in: the fist starts far enough back that the tool's end only meets him part way, then rides on with the body
		const FVector WalkV = F * WalkSpeed;
		const FVector FistStart = K.Swing == EWhackSwing::WalkInto ? Fist0 - F * FMath::Max(0.0, Tip - (Range - Reach) + 60.0) : Fist0;
		double BestWhole = 1e9;
		ToolCooldown[1] = 0.f;
		bToolTipValid[1] = false;
		if (K.Swing == EWhackSwing::WalkInto) GetCharacterMovement()->Velocity = WalkV;
		for (int32 i = 0; i <= Frames; ++i)
		{
			const double A = FMath::Lerp(A0, A1, double(i) / Frames);
			const FVector D = DirAt(A);
			const FVector Fist = K.Swing == EWhackSwing::WalkInto ? FistStart + WalkV * (WhackFrame * i) : Fist0;
			const FTransform Want(FRotationMatrix::MakeFromXZ(D, K.Swing == EWhackSwing::Chop ? Side.Cross(D) : Up).ToQuat(), Fist);
			FrameOf(1)->SetWorldTransform(ToolRest(1).Inverse() * Want);
			UpdateTools(WhackFrame);
			const FVector T = ToolTip(1);
			const FVector Butt = Fist + (T - Fist).GetSafeNormal() * FMath::Min(25.0, Tip * 0.45);
			if (!bPlayer) BestWhole = FMath::Min(BestWhole, GapTo(E, Butt, T));
			for (int32 s = 0; s <= 20 && Spr && !bInPicture; ++s) bInPicture = Spr->DrawnTouches(FMath::Lerp(Butt, T, s / 20.0), 22.0);
			if (LandedAt < 0 && Landed()) LandedAt = i;
		}
		if (K.Swing == EWhackSwing::WalkInto) GetCharacterMovement()->Velocity = FVector::ZeroVector;
		bToolTipValid[1] = false;              // (the tick's own UpdateTools after this starts over from the hand's place)
		HandNote = FString::Printf(TEXT("; the fist %.0f cm out, tool %.0f cm, nearest striking length %.1f cm off his body"), Reach, Tip, bPlayer ? -1.0 : BestWhole);
	}
	if (!GripWas) TestAxes.Remove(TEXT("OLVR_RightGrip"));
	const bool bHit = LandedAt >= 0;
	if (!bHit && K.bExpectHit && !bPlayer)
	{
		for (const TPair<TWeakObjectPtr<AOutlawsEnemy>, float>& O : Near)
		{
			const AOutlawsEnemy* B = O.Key.Get();
			if (B && B->GetHealth() < O.Value)
			{
				++P.Skip;
				Next(TEXT("SKIP"), FString::Printf(TEXT("%s (%s) %.0f cm off took the blow first; he was %.0f cm from the eye"), *B->GetOutlawsName(), *B->GetName(),
					FVector::Dist2D(B->GetActorLocation(), Eye), Range));
				return;
			}
		}
	}
	FString Got;
	if (bPlayer)
	{
		const float Dmg = UOutlawsCoop::LastMatchStrikeDamage();
		const float Base = (K.IsFist() ? 0.8f : K.Tool[1] == TEXT('s') ? 1.4f : 1.2f) * L->PlayerDamageScale();
		const bool bDmgOk = !bHit || (Dmg >= Base * 0.899f && Dmg <= Base * 1.101f);
		Got = FString::Printf(TEXT("player %d (a bot, %s%s): strikes %d -> %d, dealt by the host %d -> %d%s"), P.Who, *E->GetOutlawsName(), bDucking ? TEXT(", drawn crouched") : TEXT(""),
			StrikesBefore, UOutlawsCoop::MatchStrikesSent(), DealtBefore, Co ? Co->MatchHitsDealtSoFar() : 0,
			bHit ? *FString::Printf(TEXT(", %.2f damage (%s for %.1f x0.9..1.1)"), Dmg, bDmgOk ? TEXT("right") : TEXT("WRONG"), Base) : TEXT(""));
		if (!bDmgOk) LandedAt = -2;
	}
	else
	{
		Got = FString::Printf(TEXT("%s (%s%s): health %.2f -> %.2f"), *E->GetOutlawsName(), *E->GetName(), E->CoopCopyOf >= 0 ? TEXT(", a multiplied copy") : TEXT(""), Before, E->GetHealth());
	}
	const bool bOk = LandedAt != -2 && bHit == K.bExpectHit;
	const FString Why = FString::Printf(TEXT("%s %.0f cm from the eye%s%s; aimed at %.0f cm over his feet, drawn %.0f tall (%.0f as placed), body top %.0f%s%s, in his picture %s"),
		*Got, Range, bHit ? *FString::Printf(TEXT(", landed at frame %d of %d"), LandedAt, Frames) : TEXT(""), bDucking ? TEXT(", ducking") : TEXT(""),
		AimZ - FeetZ, DrawnH, PicH, BodyTop - FeetZ, K.Swing == EWhackSwing::WalkInto ? TEXT(", walking at 450 cm/s") : TEXT(""), *HandNote, bInPicture ? TEXT("yes") : TEXT("no"));
	if (bOk) ++P.Pass;
	else ++P.Fail;
	Next(bOk ? TEXT("PASS") : TEXT("FAIL"), Why);
}
