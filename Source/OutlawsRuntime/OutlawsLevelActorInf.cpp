// AOutlawsLevelActor, INF half: doors, lifts, moving floors and ceilings, switches.
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsPerfTest.h"
#include "OutlawsPcmWave.h"
#include "UObject/UObjectGlobals.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "OutlawsPerf.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "VorbisAudioInfo.h"
#include "AudioDecompress.h"
// FSoundQualityInfo lives in a Developer header (TargetPlatform/Interfaces/IAudioFormat.h) a packaged game has
// no access to; this is its exact UE 5.1 layout, which the engine's Vorbis decoder fills
#if !defined(OUTLAWS_SOUND_QUALITY_INFO)
#define OUTLAWS_SOUND_QUALITY_INFO
struct FSoundQualityInfo
{
	int32 Quality;
	uint32 NumChannels;
	uint32 SampleRate;
	uint32 SampleDataSize;
	float Duration;
	bool bStreaming;
	FString DebugName;
};
#endif
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsStats.h"
#include "OutlawsFormats.h"
#include "Misc/FileHelper.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsDeathmatch.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "OutlawsInk.h"
#include "OutlawsTextures.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsFormats.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundAttenuation.h"
#include "Components/AudioComponent.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"

// a 3D one-shot from farther than this (cm) from the eye is not started (user 2026-10-03; the attenuation ends at 38 m); 0 = play all
static TAutoConsoleVariable<float> CVarOLFarSoundDistance(TEXT("ol.FarSoundDistance"), 4000.f,
	TEXT("A 3D sound whose source is farther (cm) than this from the listener is not started; 0 plays them all."));

namespace
{
	/** A sector that holds MorphSector as a hole (all walls of one of its rings adjoin it). */
	const FOutlawsSector* FindParentSector(const FOutlawsLevel& L, int32 MorphSector)
	{
		TArray<TArray<int32>> Rings, RingWalls;
		for (const FOutlawsSector& P : L.Sectors)
		{
			if (P.Index == MorphSector) continue;
			bool bAny = false;
			for (const FOutlawsWall& W : P.Walls)
			{
				if (W.Adjoin == MorphSector) { bAny = true; break; }
			}
			if (!bAny) continue;
			FOutlawsLevel::LoopsFromWallsEx(P, Rings, RingWalls);
			for (const TArray<int32>& RW : RingWalls)
			{
				bool bAll = RW.Num() > 0;
				for (int32 w : RW) bAll &= P.Walls[w].Adjoin == MorphSector;
				if (bAll) return &P;
			}
		}
		return nullptr;
	}

	bool SegmentsCross(const FVector2D& P1, const FVector2D& P2, const FVector2D& Q1, const FVector2D& Q2)
	{
		auto Cross = [](const FVector2D& A, const FVector2D& B, const FVector2D& C) { return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X); };
		const double D1 = Cross(Q1, Q2, P1), D2 = Cross(Q1, Q2, P2), D3 = Cross(P1, P2, Q1), D4 = Cross(P1, P2, Q2);
		return ((D1 > 0 && D2 < 0) || (D1 < 0 && D2 > 0)) && ((D3 > 0 && D4 < 0) || (D3 < 0 && D4 > 0));
	}

	double PointSegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const double Len2 = AB.SizeSquared();
		const double T = Len2 > 1e-12 ? FMath::Clamp(FVector2D::DotProduct(P - A, AB) / Len2, 0.0, 1.0) : 0.0;
		return FVector2D::Distance(P, A + AB * T);
	}
}

namespace
{
	// hands, drops and ledges look at the world only: a bad guy is never a ledge or a shelf
	FCollisionResponseParams NoPawns()
	{
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return RP;
	}
}

void AOutlawsLevelActor::LoadGameTables()
{
	// read once per actor: message text and the inventory item table do not change between levels
	// local.msg lines: 801  5:  "You need the brass key."   (id, priority, text)
	FString Msg;
	if (UserMessages.Num() == 0 && AssetsRef && AssetsRef->GetText(TEXT("local.msg"), Msg))
	{
		TArray<FString> Lines;
		Msg.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			const FString T = Line.TrimStart();
			int32 Q0, Q1;
			if (T.IsEmpty() || !FChar::IsDigit(T[0]) || !T.FindChar(TEXT('"'), Q0) || !T.FindLastChar(TEXT('"'), Q1) || Q1 <= Q0) continue;
			UserMessages.Add(FCString::Atoi(*T), T.Mid(Q0 + 1, Q1 - Q0 - 1));
		}
	}
	// inventory items (FUNC Inv_GenericObject / Inv_HealthObject / the weapons): "INT USE_ID 101 ...
	// STR SND_USE_S unlock STR SND_USE_F locked" (ibrsskey.itm), "INT MAX_COUNT 100 STR ITEM_CLASS ammo"
	// (ibullit.itm).  Ground items (Inv_*GroundObject) are the pickups, read per level.
	if (ItemDefs.Num() == 0 && AssetsRef)
	{
		TArray<FString> Itms;
		AssetsRef->ListByExtension(TEXT(".itm"), Itms);
		for (const FString& Name : Itms)
		{
			FString Text;
			if (!AssetsRef->GetText(Name, Text)) continue;
			FString Func;
			TMap<FString, FString> Data;
			OutlawsFormats::ParseItm(Text, Func, Data);
			if (Func.Contains(TEXT("GroundObject")) || !(Func.StartsWith(TEXT("Inv_"), ESearchCase::IgnoreCase) || Func.StartsWith(TEXT("Weapon_"), ESearchCase::IgnoreCase)))
			{
				continue;
			}
			FItemDef D;
			D.Name = FPaths::GetBaseFilename(Name);
			D.Class = Data.FindRef(TEXT("ITEM_CLASS")).ToLower();
			D.MaxCount = Data.Contains(TEXT("MAX_COUNT")) ? FMath::Max(1, FCString::Atoi(*Data[TEXT("MAX_COUNT")])) : 1;
			D.UseId = Data.Contains(TEXT("USE_ID")) ? FCString::Atoi(*Data[TEXT("USE_ID")]) : -1;
			D.UseOk = Data.FindRef(TEXT("SND_USE_S"));
			D.bWeapon = Func.StartsWith(TEXT("Weapon_"), ESearchCase::IgnoreCase) || D.Class == TEXT("weap");
			ItemDefs.Add(D.Name.ToLower(), D);
			if (D.UseId >= 0)
			{
				FItemInfo Info;
				Info.Name = D.Name;
				Info.UseOk = D.UseOk;
				Info.UseFail = Data.FindRef(TEXT("SND_USE_F"));
				ItemsByUseId.Add(D.UseId, Info);
				UseIdByItem.Add(D.Name.ToLower(), D.UseId);
			}
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT item table: %d inventory items, %d with a use id"), ItemDefs.Num(), ItemsByUseId.Num());
	}
}

TMap<FString, int32> AOutlawsLevelActor::StoryKit(const FOutlawsAssets* A, const FString& Character)
{
	// BUG-CHAROIL: the story's kit (campaign and Historical Missions, alone or in co-op) is James's single-player kit
	// (cidjames.itm: 24 bullets, 16 cartridges, pistol, rifle, 30 oil; the lantern is always carried) with the worn
	// character's own on top, each item the larger count (Dr. Death's ITM gives no oil; his bag and cartridges stay his)
	TMap<FString, int32> Kit;
	FString Text;
	if (A && A->GetText(TEXT("cidjames.itm"), Text))
	{
		FString Func;
		TMap<FString, FString> Data;
		OutlawsFormats::ParseItm(Text, Func, Data);
		for (int32 i = 1; i < 16; ++i)
		{
			const FString* Item = Data.Find(FString::Printf(TEXT("START_ITEM_%d"), i));
			if (!Item) break;
			Kit.FindOrAdd(Item->TrimStartAndEnd().ToLower()) += FCString::Atoi(*Data.FindRef(FString::Printf(TEXT("START_COUNT_%d"), i)));
		}
	}
	if (Kit.Num() == 0)
	{
		Kit.Add(TEXT("ibullit"), 24);
		Kit.Add(TEXT("icart"), 16);
	}
	const FOutlawsCharKit& Own = FOutlawsCharKit::Of(A, Character);
	if (!Character.IsEmpty() && Own.bRead)
	{
		for (const TPair<FString, int32>& S : Own.Start)
		{
			int32& C = Kit.FindOrAdd(S.Key);
			C = FMath::Max(C, S.Value);
		}
	}
	return Kit;
}

void AOutlawsLevelActor::ApplyStartKit(const FString& Character, int32 KitMode)
{
	// James' single-player kit (cidjames.itm START_ITEM_n / START_COUNT_n): 24 bullets, 16 cartridges,
	// pistol, rifle, 30 oil; full health, no armour.  BUG-CHARLOADOUT: a match's or a co-op joiner's character has its own
	// ITM's (Mary: shells and the shotgun; Bob: the double barrel; Two-Feathers: knives; Dr. Death: the medical bag)
	Health = 10.f;
	Armour = 0;
	Gold = 0;
	BadgeLeft = 0.f;
	Counts.Reset();
	bDead = false;
	DeathTimer = 0.f;
	const FString Want = !Character.IsEmpty() ? Character : UOutlawsCoop::KitCharacterFor(this);
	const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(AssetsRef.Get(), Want);
	WornCharacter = Kit.bRead ? Want : FString();
	StartWeaponSlot = -1;
	FString Text;
	if (Kit.bRead)
	{
		GotShotSound = TEXT("hit");
		// a match: the character's ITM kit as the original gives it (all but Dr. Death's carry 30 oil); the story: James's
		// basics with the character's own on top (BUG-CHAROIL)
		const bool bMatch = KitMode >= 0 ? KitMode == 1 : UOutlawsCoop::IsMatchActive(this);
		if (bMatch) for (const TPair<FString, int32>& S : Kit.Start) Counts.FindOrAdd(S.Key) += S.Value;
		else Counts = StoryKit(AssetsRef.Get(), Want);
		StartWeaponSlot = Kit.StartSlot;
		DieSound = Kit.DieSound.IsEmpty() ? FString(TEXT("jadie")) : Kit.DieSound;      // (BUG-MPDEATHSOUND: szdie, bmdie...)
		FString Line;
		for (const TPair<FString, int32>& S : Counts) Line += FString::Printf(TEXT("%s%s %d"), Line.IsEmpty() ? TEXT("") : TEXT(", "), *S.Key, S.Value);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT start kit of %s (%s, %s): %s; first weapon slot %d"), *Want, *Kit.Itm, bMatch ? TEXT("the match's ITM kit") : TEXT("James's basics and its own"), *Line, StartWeaponSlot);
	}
	else if (AssetsRef && AssetsRef->GetText(TEXT("cidjames.itm"), Text))
	{
		FString Func;
		TMap<FString, FString> Data;
		OutlawsFormats::ParseItm(Text, Func, Data);
		// the marshal's grunt when shot: hit.wav (the character's SND_GOTSHOT / SND_HITHEAD are not it)
		GotShotSound = TEXT("hit");
		// (BUG-MPDEATHSOUND: James's SND_DIE, jadie.wav)
		const FString Die = FPaths::GetBaseFilename(Data.FindRef(TEXT("SND_DIE")).TrimStartAndEnd()).ToLower();
		DieSound = !Die.IsEmpty() && Die != TEXT("null") ? Die : FString(TEXT("jadie"));
		for (int32 i = 1; i < 16; ++i)
		{
			const FString* Item = Data.Find(FString::Printf(TEXT("START_ITEM_%d"), i));
			if (!Item) break;
			Counts.Add(Item->ToLower(), FCString::Atoi(*Data.FindRef(FString::Printf(TEXT("START_COUNT_%d"), i))));
		}
	}
	// the character's own weapons (sounds, damage, spread) on this machine's player, James's when none
	AOutlawsPlayer::SetWeaponCharacter(AssetsRef.Get(), WornCharacter);
	if (UWorld* W = GetWorld())
	{
		if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(W, 0))) P->WearNails(WornCharacter);
	}
	if (Counts.Num() == 0)
	{
		Counts.Add(TEXT("ibullit"), 24);
		Counts.Add(TEXT("icart"), 16);
	}
	// Bob's Ranch: the marshal arrives with nothing but his fists (user 2026-09-27); the first gpistol found
	// is one revolver, a second one the pair again
	if (LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase))
	{
		Counts.Reset();
		Inventory.Reset();
		InventoryLabels.Reset();
		Armour = 0;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT the ranch: stripped to fists"));
	}
}

int32 AOutlawsLevelActor::GetCount(const FString& Item) const
{
	return Counts.FindRef(Item.ToLower());
}

bool AOutlawsLevelActor::AddCount(const FString& Item, int32 Count)
{
	const FString Key = Item.ToLower();
	const FItemDef* D = ItemDefs.Find(Key);
	const FString Class = D ? D->Class : FString();
	if (Class == TEXT("armr"))
	{
		// a plate is only taken when it beats the one worn, and it replaces it
		if (Count <= Armour) return false;
		Armour = FMath::Min(Count, D->MaxCount);
		ArmourMax = Armour;          // (its size: the held plate's, user 2026-10-01)
		++PlateSerial;
		return true;
	}
	int32 Max = D ? D->MaxCount : 1;
	if (Key == TEXT("ipistol") || Key == TEXT("isawgun")) Max = FMath::Max(Max, 2);   // a second revolver or sawed-off makes the pair (user 2026-09-27/28)
	// and in co-op every revolver past the pair is kept as a spare, to drop for a friend (user 2026-10-02); alone the pair is
	// all (user 2026-10-04, BUG-PISTOLCAP: the all-weapons cheat gave 99), and more carried in from co-op is cut to it
	const bool bCoop = UOutlawsCoop::IsActive(this);
	if (Key == TEXT("ipistol") && bCoop) Max = FMath::Max(Max, 99);
	int32& C = Counts.FindOrAdd(Key);
	if (Key == TEXT("ipistol") && !bCoop && C > Max) C = Max;
	if (C >= Max || Count <= 0) return false;
	C = FMath::Min(Max, C + Count);
	if (Class == TEXT("gbar")) Gold += Count;
	if (Class == TEXT("gsak")) Gold += 5 * Count;
	if (D && D->UseId >= 0 && Inf && !IsToolUseId(D->UseId)) Inf->Items.Add(D->UseId);   // a tool counts only in the hand
	return true;
}

bool AOutlawsLevelActor::TakeCount(const FString& Item, int32 Count)
{
	const FString Key = Item.ToLower();
	int32* C = Counts.Find(Key);
	if (!C || *C < Count) return false;
	*C -= Count;
	return true;
}

void AOutlawsLevelActor::UseUpItem(int32 UseId)
{
	// one item with that USE_ID goes (igear1..3 all answer 112: whichever is carried); with none left the INF's lock
	// closes again (user 2026-09-29: one gear worked all three slots and stayed in the pockets)
	FString Gone;
	int32 Left = 0;
	for (const TPair<FString, FItemDef>& Kv : ItemDefs)
	{
		if (Kv.Value.UseId != UseId) continue;
		if (Gone.IsEmpty() && GetCount(Kv.Key) > 0 && TakeCount(Kv.Key, 1)) Gone = Kv.Key;
		Left += GetCount(Kv.Key);
	}
	if (Gone.IsEmpty()) return;
	if (GetCount(Gone) <= 0)
	{
		const int32 At = Inventory.IndexOfByPredicate([&Gone](const FString& S) { return S.Equals(Gone, ESearchCase::IgnoreCase); });
		if (At != INDEX_NONE)
		{
			Inventory.RemoveAt(At);
			if (InventoryLabels.IsValidIndex(At)) InventoryLabels.RemoveAt(At);
		}
	}
	if (Left <= 0 && Inf) Inf->Items.Remove(UseId);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT INF used up %s (use id %d), %d such left"), *Gone, UseId, Left);
}

void AOutlawsLevelActor::HealPlayer(float Points)
{
	if (!bDead) Health = FMath::Min(10.f, Health + Points);
}

bool AOutlawsLevelActor::UseMedicalBag()
{
	// heals what is missing (whole points), spending only that many bag points
	const int32 Bag = GetCount(TEXT("idocbag"));
	if (bDead || Bag <= 0 || Health >= 10.f) return false;
	const int32 Spend = FMath::Min(Bag, FMath::CeilToInt(10.f - Health));
	Health = FMath::Min(10.f, Health + Spend);
	TakeCount(TEXT("idocbag"), Spend);
	const FItemDef* D = ItemDefs.Find(TEXT("idocbag"));
	PlayWav(D && !D->UseOk.IsEmpty() ? D->UseOk : TEXT("cool"), FVector::ZeroVector, false, true);   // heard in the head (it played at this actor, far off)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT medical bag: %d points used, health %.1f, %d left"), Spend, Health, Bag - Spend);
	return true;
}

bool AOutlawsLevelActor::IsListedItem(const FString& Item) const
{
	// (BUG-WHEELINV) a knife or a stick of dynamite picked up gave its weapon's item, which has no inventory ITM of its own
	// and no wheel card: it listed blank.  Only the level's inventory ITMs list, never a weapon (the scope is one) or a stock
	static const TSet<FString> Counted = { TEXT("ammo"), TEXT("armr"), TEXT("heal"), TEXT("gbar"), TEXT("gsak"), TEXT("fuel") };
	const FItemDef* D = ItemDefs.Find(Item.ToLower());
	return D && !D->bWeapon && !Counted.Contains(D->Class);
}

void AOutlawsLevelActor::GetWheelItems(TArray<FString>& OutItems, TArray<FString>& OutLabels) const
{
	OutItems.Reset();
	OutLabels.Reset();
	if (const int32 Bag = GetCount(TEXT("idocbag")))
	{
		OutItems.Add(TEXT("idocbag"));
		OutLabels.Add(FString::Printf(TEXT("Medical Bag %d"), Bag));
	}
	if (const int32 Elixir = GetCount(TEXT("ielixer")))
	{
		OutItems.Add(TEXT("ielixer"));
		OutLabels.Add(FString::Printf(TEXT("Elixir %d"), Elixir));
	}
	// the lantern: always carried (the original's L key), lit and put out from the wheel (user 2026-09-27)
	OutItems.Add(TEXT("ilantern"));
	OutLabels.Add(FString::Printf(TEXT("Lantern %s, oil %d"), bLampOn ? TEXT("(lit)") : TEXT("(out)"), GetCount(TEXT("ioil"))));
	for (int32 i = 0; i < Inventory.Num(); ++i)
	{
		if (OutItems.Contains(Inventory[i]) || !IsListedItem(Inventory[i])) continue;
		OutItems.Add(Inventory[i]);
		FString Label = InventoryLabels.IsValidIndex(i) ? InventoryLabels[i] : Inventory[i];
		if (Inventory[i] == TEXT("ilantern")) Label += FString::Printf(TEXT(" %s, oil %d"), bLampOn ? TEXT("(lit)") : TEXT("(out)"), GetCount(TEXT("ioil")));
		OutLabels.Add(Label);
	}
}

bool AOutlawsLevelActor::UseItem(const FString& Item)
{
	const FString Key = Item.ToLower();
	if (Key == TEXT("idocbag"))
	{
		return UseMedicalBag();
	}
	if (Key == TEXT("ilantern"))
	{
		return ToggleLamp();
	}
	if (Key == TEXT("ielixer"))
	{
		// HEALTH_B 100: full health, one used
		if (bDead || Health >= 10.f || !TakeCount(Key, 1)) return false;
		Health = 10.f;
		ShowMessage(311);
		return true;
	}
	const int32 i = Inventory.IndexOfByKey(Key);
	if (i != INDEX_NONE)
	{
		const FString Line = InventoryLabels.IsValidIndex(i) ? InventoryLabels[i] : Key;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel item: %s"), *Line);
		Notify(Line, 2.f);
		return true;
	}
	return false;
}

FString AOutlawsLevelActor::FloorSoundAt(const FVector& Feet) const
{
	if (!LevelData)
	{
		return FString();
	}
	const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Feet, 0.5));
	return S ? S->FloorSound : FString();
}

float AOutlawsLevelActor::GravityAt(const FVector& Feet) const
{
	if (!LevelData)
	{
		return -60.f;
	}
	const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Feet, 0.5));
	return S ? S->Gravity : -60.f;
}

bool AOutlawsLevelActor::PaletteGradeAt(const FVector& Eye, FLinearColor OutRows[3]) const
{
	if (!LevelData || !bPaletteGrades)
	{
		return false;
	}
	const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Eye));
	if (!S || S->Palette == MainPalette || !PaletteGrades.IsValidIndex(S->Palette) || !PaletteGrades[S->Palette].bValid)
	{
		return false;
	}
	// under water the water's own colour does this job (the tinted surfaces and the view's filter): the underwater
	// palette's grade on top turned every blue violet (it all but drops green; user 2026-09-28: purple)
	if (WaterTints.Contains(S->Index))
	{
		return false;
	}
	for (int32 i = 0; i < 3; ++i) OutRows[i] = PaletteGrades[S->Palette].Rows[i];
	return true;
}

void AOutlawsLevelActor::BuildPaletteGrades(const FOutlawsAssets& A, const FOutlawsLevel& L)
{
	// Outlaws draws each sector through its own palette (underwater, dusk...).  The HD textures were
	// never made for those, so rather than remap colours, each palette is fitted against the level's
	// main one as a smooth grade: a 3x3 colour matrix and offset in linear light, least squares over
	// the 255 palette entries (index 0 is the transparent colour)
	PaletteGrades.Reset();
	PaletteGrades.SetNum(L.Palettes.Num());
	TArray<int32> Uses;
	Uses.SetNumZeroed(FMath::Max(1, L.Palettes.Num()));
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (Uses.IsValidIndex(S.Palette)) ++Uses[S.Palette];
	}
	MainPalette = 0;
	for (int32 i = 1; i < Uses.Num(); ++i)
	{
		if (Uses[i] > Uses[MainPalette]) MainPalette = i;
	}
	auto Load = [&A](const FString& Name, TArray<FLinearColor>& Out)
	{
		TArray<uint8> Bytes;
		OutlawsFormats::FPcxImage Img;
		const FString File = Name.Contains(TEXT(".")) ? Name : Name + TEXT(".pcx");
		if (!A.Get(File, Bytes) || !OutlawsFormats::DecodePcx(Bytes, Img) || Img.Palette.Num() < 256) return false;
		Out.SetNum(256);
		for (int32 i = 0; i < 256; ++i) Out[i] = FLinearColor::FromSRGBColor(Img.Palette[i]);
		return true;
	};
	TArray<FLinearColor> Main;
	if (!L.Palettes.IsValidIndex(MainPalette) || !Load(L.Palettes[MainPalette], Main))
	{
		return;
	}
	for (int32 p = 0; p < L.Palettes.Num(); ++p)
	{
		TArray<FLinearColor> Other;
		if (p == MainPalette || Uses[p] == 0 || !Load(L.Palettes[p], Other)) continue;
		// normal equations: (X^T X) M = X^T Y, X rows (r, g, b, 1), Y rows (r', g', b')
		double N[4][4] = {}, B[4][3] = {};
		for (int32 i = 1; i < 256; ++i)
		{
			const double X[4] = { Main[i].R, Main[i].G, Main[i].B, 1.0 };
			const double Y[3] = { Other[i].R, Other[i].G, Other[i].B };
			for (int32 r = 0; r < 4; ++r)
			{
				for (int32 c = 0; c < 4; ++c) N[r][c] += X[r] * X[c];
				for (int32 c = 0; c < 3; ++c) B[r][c] += X[r] * Y[c];
			}
		}
		for (int32 r = 0; r < 3; ++r) N[r][r] += 1e-4;     // steady when a palette lacks a hue
		// Gauss-Jordan with partial pivoting
		bool bOk = true;
		for (int32 c = 0; c < 4 && bOk; ++c)
		{
			int32 Piv = c;
			for (int32 r = c + 1; r < 4; ++r) if (FMath::Abs(N[r][c]) > FMath::Abs(N[Piv][c])) Piv = r;
			if (FMath::Abs(N[Piv][c]) < 1e-9) { bOk = false; break; }
			for (int32 k = 0; k < 4; ++k) Swap(N[c][k], N[Piv][k]);
			for (int32 k = 0; k < 3; ++k) Swap(B[c][k], B[Piv][k]);
			const double D = N[c][c];
			for (int32 k = 0; k < 4; ++k) N[c][k] /= D;
			for (int32 k = 0; k < 3; ++k) B[c][k] /= D;
			for (int32 r = 0; r < 4; ++r)
			{
				if (r == c) continue;
				const double F = N[r][c];
				for (int32 k = 0; k < 4; ++k) N[r][k] -= F * N[c][k];
				for (int32 k = 0; k < 3; ++k) B[r][k] -= F * B[c][k];
			}
		}
		if (!bOk) continue;
		FPaletteGrade& G = PaletteGrades[p];
		G.bValid = true;
		for (int32 ch = 0; ch < 3; ++ch)
		{
			// out channel ch = B[0][ch] r + B[1][ch] g + B[2][ch] b + B[3][ch]
			G.Rows[ch] = FLinearColor(B[0][ch], B[1][ch], B[2][ch], B[3][ch]);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT palette %s (%d sectors) graded against %s: R %s G %s B %s"), *L.Palettes[p], Uses[p], *L.Palettes[MainPalette],
			*G.Rows[0].ToString(), *G.Rows[1].ToString(), *G.Rows[2].ToString());
	}
}

bool AOutlawsLevelActor::OldFireBright()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldFireBright"));     // (REQ-FIREBRIGHT's before: every sprite by its sector's light)
	return bOld;
}

void AOutlawsLevelActor::BuildFullBright(const FOutlawsAssets& A, const FOutlawsLevel& L)
{
	// REQ-FIREBRIGHT (user 2026-10-28): the Jedi engine lights a pixel through the level's colour map, a PCX whose rows are
	// the light levels (and the shade tables under them): an entry no row ever changes is drawn as it is in the dark.  In the
	// story's maps those are the shade tints 1..15, the fire ramp 16..19 and 250..254, and white; the remaster's GPU path
	// keeps them so too (its HD art has a full-bright layer for the same pixels)
	FullBrightIx.Init(false, 256);
	const int32 Ci = L.Cmaps.IsValidIndex(MainPalette) ? MainPalette : 0;
	const FString Name = L.Cmaps.IsValidIndex(Ci) ? L.Cmaps[Ci] : L.Palettes.IsValidIndex(MainPalette) ? L.Palettes[MainPalette] : FString();
	TArray<uint8> Bytes;
	OutlawsFormats::FPcxImage Img;
	if (Name.IsEmpty() || !A.Get(Name.Contains(TEXT(".")) ? Name : Name + TEXT(".pcx"), Bytes) || !OutlawsFormats::DecodePcx(Bytes, Img) || Img.W != 256 || Img.H < 32
		|| Img.Indices.Num() < Img.W * Img.H)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT full bright: no colour map for %s (%s): no art full bright"), *LevelName, Name.IsEmpty() ? TEXT("none named") : *Name);
		return;
	}
	FString Kept;
	int32 N = 0;
	for (int32 c = 16; c < 256; ++c)
	{
		bool bSame = true;
		for (int32 y = 0; y < Img.H && bSame; ++y) bSame = Img.Indices[y * 256 + c] == c;
		if (!bSame) continue;
		FullBrightIx[c] = true;
		Kept += FString::Printf(TEXT("%s%d"), N++ ? TEXT(" ") : TEXT(""), c);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT full bright: %d entries of %s (%d rows) kept from the light: %s"), N, *Name, Img.H, N ? *Kept : TEXT("none"));
}

bool AOutlawsLevelActor::IsFullBrightArt(const OutlawsTextures::FSpriteAtlas& Atlas, float* OutShare) const
{
	// the share of the art's drawn pixels, the shade tints (1..15) aside, in colours the colour map never dims
	uint64 Lit = 0, Full = 0;
	for (int32 i = 16; i < Atlas.IndexUse.Num(); ++i)
	{
		Lit += Atlas.IndexUse[i];
		if (FullBrightIx.IsValidIndex(i) && FullBrightIx[i]) Full += Atlas.IndexUse[i];
	}
	const float Share = Lit > 0 ? float(double(Full) / double(Lit)) : 0.f;
	if (OutShare) *OutShare = Share;
	return !OldFireBright() && Share >= 0.4f;
}

bool AOutlawsLevelActor::OldFireBright2()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldFireBright2"));    // (REQ-FIREBRIGHT2's before: the fire's art full bright as a whole)
	return bOld;
}

bool AOutlawsLevelActor::FireKeepsBright(uint8 V) const
{
	// the colour map's kept entries (the fire ramp 16..19, 250..254, white), and the flame's bright yellows and oranges: the
	// remaster's full-bright layer of camfire1 (its SPX's second layer) lights 99 % of the pixels drawn in 133 (231 187 51)
	// and 80 (255 131 0), which the colour map dims from row 3; the logs' browns (at most 183 bright, or paler) 0..28 %
	if (V < 16) return false;
	if (FullBrightIx.IsValidIndex(V) && FullBrightIx[V]) return true;
	if (!SpritePalette.IsValidIndex(V)) return false;
	const FColor C = SpritePalette[V];
	const int32 Hi = FMath::Max3(C.R, C.G, C.B), Lo = FMath::Min3(C.R, C.G, C.B);
	return C.R == Hi && Hi >= 200 && (Hi - Lo) * 100 >= Hi * 70;          // red the largest, bright, saturation 0.7 or more
}

UMaterialInterface* AOutlawsLevelActor::SpriteMaterialFor(AOutlawsSpriteActor* S, const OutlawsTextures::FSpriteAtlas& Atlas)
{
	if (!S || !S->bFullBright || OldFireBright2() || !SpriteFireMaterial) return SpriteMaterial;
	OutlawsTextures::FSpriteAtlas& At = const_cast<OutlawsTextures::FSpriteAtlas&>(Atlas);
	if (!At.FireMask && At.Indices.Num() == At.AtlasW * At.AtlasH && At.AtlasW > 0)
	{
		// the mask: 255 where the 8-bit frame draws a colour that dims (the logs, the stones), 0 where it draws one that does
		// not (FireKeepsBright: the flame) and where it draws nothing (the HD look's soft halo lies there: bright, its own colour)
		TArray<FColor> Px;
		Px.SetNumZeroed(At.Indices.Num());
		int32 Dim = 0, Drawn = 0;
		for (int32 i = 0; i < At.Indices.Num(); ++i)
		{
			const uint8 V = At.Indices[i];
			if (!At.Alpha.IsValidIndex(i) || !At.Alpha[i]) continue;
			++Drawn;
			if (FireKeepsBright(V)) continue;
			Px[i] = FColor(255, 255, 255, 255);
			++Dim;
		}
		// a dim speck inside the flame (a dark red of its edge, 76 or 154, between kept pixels) is lit with it: scaled up for
		// the HD look such specks showed as black dots along the flame (the remaster's layer lights about a third of them)
		const int32 W = At.AtlasW, H = At.AtlasH;
		auto Kept = [&](int32 x, int32 y) { const int32 j = y * W + x; return x >= 0 && y >= 0 && x < W && y < H && At.Alpha[j] && Px[j].R == 0; };
		TArray<int32> Specks;
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				if (Px[y * W + x].R == 0) continue;
				int32 N = 0;
				for (int32 dy = -1; dy <= 1; ++dy) for (int32 dx = -1; dx <= 1; ++dx) N += (dx || dy) && Kept(x + dx, y + dy);
				if (N >= 5) Specks.Add(y * W + x);
			}
		}
		for (const int32 j : Specks) Px[j] = FColor(0, 0, 0, 0);
		Dim -= Specks.Num();
		UTexture2D* Lo = OutlawsTextures::MakeTextureBGRA(At.AtlasW, At.AtlasH, Px, false, TF_Nearest, TA_Clamp);
		UTexture2D* Hd = OutlawsTextures::MakeTextureBGRA(At.AtlasW, At.AtlasH, Px, false, TF_Bilinear, TA_Clamp);
		if (!Lo || !Hd) return SpriteMaterial;
		Textures.Add(FString::Printf(TEXT("FIREMASK:%s:%p"), *At.Nwx, &At), Lo);      // (keeps them referenced)
		Textures.Add(FString::Printf(TEXT("FIREMASKHD:%s:%p"), *At.Nwx, &At), Hd);
		At.FireMask = Lo;
		At.FireMaskHd = Hd;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT full bright per pixel: %s, %d of its %d drawn px dim with the light, %d specks in the flame lit (%dx%d mask)"), *At.Nwx, Dim, Drawn,
			Specks.Num(), At.AtlasW, At.AtlasH);
	}
	if (!At.FireMask) return SpriteMaterial;
	if (bHDTextures) FireMaskLayerFor(At);       // (REQ-FIREBRIGHT3: the HD look's mask from the remaster's own layer)
	S->FireMask = At.FireMask;
	S->FireMaskHd = At.FireMaskHd;
	return SpriteFireMaterial;
}

void AOutlawsLevelActor::FireMaskLayerFor(OutlawsTextures::FSpriteAtlas& At)
{
	// REQ-FIREBRIGHT3 (planner 2026-10-29: pale specks on the logs in HD): the HD look's mask from the remaster's own
	// full-bright layer where the sprite has one (camfire1's lights 99 % of the flame's heart, 0..36 % of the log browns),
	// made once, the first time the HD look is wanted; else the 8-bit colours' mask, filtered, stands
	if (At.bFireMaskLayerTried || !At.FireMask || !AssetsRef) return;
	At.bFireMaskLayerTried = true;
	TArray<FColor> Px;
	int32 W = 0, H = 0, Lit = 0;
	if (!OutlawsTextures::DecodeSpriteAtlasFullBrightMask(*AssetsRef, At, Px, W, H, &Lit))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT full bright HD mask: %s has no full-bright layer, the 8-bit colours' mask filtered"), *At.Nwx);
		return;
	}
	UTexture2D* T = OutlawsTextures::MakeTextureBGRA(W, H, Px, false, TF_Bilinear, TA_Clamp);
	if (!T) return;
	Textures.Add(FString::Printf(TEXT("FIREMASKLAYER:%s:%p"), *At.Nwx, &At), T);      // (keeps it referenced)
	At.FireMaskHd = T;
	At.bFireMaskLayer = true;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT full bright HD mask: %s from the remaster's full-bright layer, %d of %d texels kept bright (%dx%d)"), *At.Nwx, Lit, W * H, W, H);
}

void AOutlawsLevelActor::GiveAllWeapons()
{
	// every weapon item and each ammunition to its limit (ITM MAX_COUNT)
	for (const TCHAR* Item : { TEXT("ipistol"), TEXT("irifle"), TEXT("isgun"), TEXT("idbsgun"), TEXT("isawgun"), TEXT("idynam"), TEXT("iknife"), TEXT("igatgun"),
		TEXT("ibullit"), TEXT("icart"), TEXT("ishells"), TEXT("iclip") })
	{
		AddCount(Item, 10000);
	}
	// and the rifle's scope, as if it had been picked up (REQ-CHEATSCOPE): the same GiveEntry a pickup of it goes through
	FPickup::FEntry Scope;
	Scope.Item = TEXT("iscope");
	GiveEntry(Scope);
	Notify(TEXT("All weapons"), 2.f);
}

void AOutlawsLevelActor::DamagePlayer(float Damage, float LocationMult, bool bSkipArmour)
{
	const uint8 Cause = HurtCause;           // (REQ-KILLFEED: this hit's cause, for its death's kill line)
	HurtCause = 0;
	if (bDead || Damage <= 0.f || bInvincible || bFrontEnd) { bHurtByBadGuy = false; return; }      // (nothing hurts at the main menu)
	++HomWounds;                             // (HM1: the results' Wounds)
	if (UOutlawsCoop::IsActive(this) && UOutlawsCoop::MenuShields(this, Damage)) return;      // (co-op, 2026-10-02: out of harm in the pause menu)
	if (HasBadgePower()) Damage *= 0.5f;     // the Sheriff's Badge
	if (const UWorld* WD = GetWorld()) LastDangerTime = WD->GetTimeSeconds();     // (hurt: the autosave waits)
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	const FVector At = Pawn ? Pawn->GetActorLocation() : GetActorLocation();
	// the plate takes it first: count / 10 points, used up when the hit is bigger
	const float Plate = bSkipArmour || !bArmourPassive ? 0.f : Armour / 10.f;      // (a fall goes past it)
	if (Plate > 0.f)
	{
		++PlateHits;                         // (BUG-PLATECARRY: a blow to the worn plate, for its dents and its breaking)
		if (Plate >= Damage)
		{
			Armour -= FMath::RoundToInt(Damage * 10.f);
			Damage = 0.f;
		}
		else
		{
			Damage -= Plate;
			Armour = 0;
		}
		PlayWav(Armour >= 50 ? TEXT("armor3") : TEXT("armor1"), At);
	}
	// where it hit scales only what got past the plate (research/outlaws_weapons_items.md 3)
	Damage *= LocationMult;
	UOutlawsStats::NoteHurt(this, FMath::Min(Damage, Health));      // (REQ-STATS)
	Health = FMath::Max(bTutorial ? 1.f : 0.f, Health - Damage);     // (REQ-TUTORIAL: the player cannot die there)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT player hurt: health %.1f, armour %d"), Health, Armour);
	const bool bByBadGuy = bHurtByBadGuy;
	bHurtByBadGuy = false;
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn);
	if (P)
	{
		P->OnHurt(Damage, bHurtFrom ? &HurtFrom : nullptr);
	}
	bHurtFrom = false;
	if (Health <= 0.f)
	{
		// the death cry; the view falls and turns sepia, and waits for the player to respawn
		bDead = true;
		DeathCause = Cause;
		DeathTimer = 0.f;
		UOutlawsStats::NoteDeath(this, bByBadGuy);       // (the level's end panels)
		PlayWav(DieSound, At);                     // (BUG-MPDEATHSOUND: the worn character's SND_DIE; the others hear it with its death)
		if (P)
		{
			P->OnDied();
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT player died"));
		OutlawsPerfTest::NoteDeath();              // (REQ-PERFTESTMODE: a performance test run's marker; its respawn stays in the run)
	}
	else if (Damage > 0.f && !bQuietHurt && World && World->GetTimeSeconds() >= NextGrunt && !GotShotSound.IsEmpty())
	{
		// a grunt when a hit gets through the plate (not more than one every 0.3 s)
		NextGrunt = World->GetTimeSeconds() + 0.3;
		PlayWav(GotShotSound, At + FVector(0.0, 0.0, 60.0));
	}
}

void AOutlawsLevelActor::DrownPlayer()
{
	if (bDead || bFrontEnd) return;
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	PlayWav(TEXT("choke"), Pawn ? Pawn->GetActorLocation() + FVector(0.0, 0.0, 60.0) : GetActorLocation());
	bQuietHurt = true;
	bHurtFrom = false;               // (all round: the water is everywhere)
	HurtCause = uint8(EOutlawsHurtCause::Drown);
	DamagePlayer(1.f, 1.f, true);
	bQuietHurt = false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT drowning: health %.1f"), Health);
}

void AOutlawsLevelActor::ShotOwnHead()
{
	if (bDead || bFrontEnd) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT the player shot their own head"));
	const bool bWasInvincible = bInvincible;
	const float BadgeWas = BadgeLeft;
	bInvincible = false;                   // (no cheat nor badge stands in the way of this one)
	BadgeLeft = 0.f;
	bHurtFrom = false;
	HurtCause = uint8(EOutlawsHurtCause::OwnShot);
	DamagePlayer(1e6f, 1.f, true);
	bInvincible = bWasInvincible;
	BadgeLeft = BadgeWas;
	ShowMessage(1098, 0, TEXT("Now why would you want to do a fool thing like that?"));
}

void AOutlawsLevelActor::FadeCamera(float From, float To, float Time, bool bHold)
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		PC->PlayerCameraManager->StartCameraFade(From, To, Time, FLinearColor::Black, false, bHold);
	}
}

namespace
{
	uint32 GOlGcCount = 0;
}

uint32 GetOlGcCount()
{
	return GOlGcCount;
}

#if WITH_OUTLAWS_TESTS
bool OlExitAt(const TCHAR* Point)
{
	// (BUG-EXITCRASH) -OLExitAt=<point>[:<n>]: the game asks to quit, as the menu's Exit does, the n-th time (1 if not given)
	// it reaches that point: init (the game instance's start, before the first frame), title (the main menu up), restart (a
	// level change asked for), build (a level about to be built), mapload (a level change that loads the map again)
	static FString Want;
	static int32 WantN = 1;
	static int32 Seen = 0;
	static bool bRead = false;
	if (!bRead)
	{
		bRead = true;
		FString V, N;
		if (OLFlags::Value(TEXT("OLExitAt"), V) && !V.Split(TEXT(":"), &Want, &N)) Want = V;
		if (!N.IsEmpty()) WantN = FCString::Atoi(*N);
	}
	if (Want.IsEmpty() || !Want.Equals(Point, ESearchCase::IgnoreCase) || ++Seen != WantN) return false;
	if (FCString::Stricmp(Point, TEXT("mapload")) == 0)
	{
		// (mapload: the quit lands at the start of the next frame, the one whose travel then loads the map: a world made
		// after the quit was asked for)
		static FDelegateHandle H;
		H = FCoreDelegates::OnBeginFrame.AddLambda([]()
		{
			FCoreDelegates::OnBeginFrame.Remove(H);
			UE_LOG(LogOutlaws, Display, TEXT("OLEXIT quit asked at mapload (the frame's start)"));
			FGenericPlatformMisc::RequestExit(false);
		});
		return true;
	}
	// (-OLExitHow=close: the game window closed instead, as Alt+F4 or the window's X does: Slate destroys the window and the
	// viewport's close request ends the game; a hidden -RenderOffscreen run has no native window to send WM_CLOSE to)
	if (OLFlags::ValueIs(TEXT("OLExitHow"), TEXT("close")) && GEngine && GEngine->GameViewport && GEngine->GameViewport->GetWindow().IsValid())
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLEXIT window closed at %s %d"), Point, WantN);
		GEngine->GameViewport->GetWindow()->RequestDestroyWindow();
		return true;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLEXIT quit asked at %s %d"), Point, WantN);
	FGenericPlatformMisc::RequestExit(false);
	return true;
}
#endif

void AOutlawsLevelActor::RestartLevel()
{
	RestartLevelThen(nullptr);
}

void AOutlawsLevelActor::RestartLevelThen(TFunction<void()> Then)
{
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn))
	{
		P->ResetForLevel();
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT level restart"));
#if WITH_OUTLAWS_TESTS
	OlExitAt(TEXT("restart"));
#endif
	UnlockStoryPending = -1;                   // (REQ-UNLOCKS: a story finish not earned by now was left before its cutscene ended)
	RebuildKind = NextRebuildKind != ERebuildKind::Other ? NextRebuildKind : Then ? ERebuildKind::Other : ERebuildKind::Restart;
	NextRebuildKind = ERebuildKind::Other;
	Clear();
	AfterRestart = MoveTemp(Then);
	// the level just cleared is let go of before the next is made (user 2026-10-01: the Quest closed at its memory limit,
	// about 6 GB, loading the canyon again: the old level's textures and meshes waited for the periodic collection while
	// the new one's were made, two levels at once).  The engine collects at the end of this frame (collecting here, in
	// the middle of the world's tick, freed things it was about to tick); the tick builds once it has
	if (World && World->IsGameWorld() && GEngine)
	{
		static bool bHooked = false;
		if (!bHooked)
		{
			bHooked = true;
			FCoreUObjectDelegates::GetPostGarbageCollect().AddLambda([]() { ++GOlGcCount; });
		}
		bRebuildPending = true;
		RebuildGcMark = GOlGcCount;
		RebuildFrames = 0;
		RebuildStage = 0;
		GEngine->ForceGarbageCollection(true);
		// (dark till it is built: set at once; a fade of no length left the view as it was, and a load from the main menu
		// showed the emptied world)
		if (APlayerController* FPC = World->GetFirstPlayerController(); FPC && FPC->PlayerCameraManager)
		{
			FPC->PlayerCameraManager->SetManualCameraFade(1.f, FLinearColor::Black, false);
		}
		SetActorTickEnabled(true);
		return;
	}
	FinishRestart();
}

void OlEmptyItemTemplates();

void AOutlawsLevelActor::ReleaseBeforeBuild()
{
	// (BUG-RESPAWNCRASH, user 2026-10-10: the Quest ran out of memory over a long session, every in-place rebuild keeping
	// about 75 MB) what only another level is built over: the sounds played (every bad guy's, the HD taunts decoded whole),
	// the item templates (a copy per light level) and the 3D ground items of the level left
	if (!LastBuiltLevel.IsEmpty() && !LastBuiltLevel.Equals(LevelName, ESearchCase::IgnoreCase))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT another level (%s after %s): %d sounds, %d ground models and the item templates let go"), *LevelName, *LastBuiltLevel,
			WavCache.Num(), GroundModels.Num());
		WavCache.Reset();
		GroundModels.Reset();
		OlEmptyItemTemplates();
	}
	// then what a map load does (UEngine::TrimMemory): the GPU's deferred deletes run, the allocators' cached pages back to
	// the system
	ENQUEUE_RENDER_COMMAND(OlTrimBeforeBuild)([](FRHICommandListImmediate& RHICmdList)
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
	});
	FlushRenderingCommands();                  // (the old level's GPU copies released too)
	FMemory::Trim(true);
}

void AOutlawsLevelActor::FinishRestart()
{
	bRebuildPending = false;
	ReleaseBeforeBuild();
	if (LoadMapInsteadOfRebuild()) return;     // (BUG-RESPAWNCRASH: memory too high for another level in this world)
	LastBuildBeforeMB = FMath::Min(SoakEmptyMB, ResidentMB());
	SoakEmptyMB = 1e12;
	static const bool bSoak = OLFlags::Has(TEXT("OLMemSoak"));
	if (bSoak) LogMemSnapshot(FString::Printf(TEXT("before the build of %s (the old level collected twice)"), *LevelName), true);
#if WITH_OUTLAWS_TESTS
	OlExitAt(TEXT("build"));
#endif
	Build();
	LastBuiltLevel = LevelName;
	LastBuildAfterMB = ResidentMB();
	// (BUG-RESPAWNCRASH) the Quest's free swap with it: the headset thrashed once it ran low
	UE_LOG(LogOutlaws, Log, TEXT("OLRT rebuilt after two collections: %.0f MB used before the build, %.0f after; swap free %.0f MB"),
		LastBuildBeforeMB, LastBuildAfterMB, SwapFreeMB());
	if (!EntryLevel.IsEmpty() && EntryLevel.Equals(LevelName, ESearchCase::IgnoreCase))
	{
		Counts = EntryCounts;                  // (Build gave the start kit; the level's own entry kit replaces it)
		Gold = EntryGold;
		Armour = EntryArmour;
		// (BUG-PLATERESTART, the planner's rule: a restart gives back what the level was entered with) the plate at its size
		// then too, not a bigger one's picked up since (85 of 150 for the 85 of 100 carried in)
		if (EntryArmourMax >= 0) ArmourMax = EntryArmourMax;
		// (BUG-HOLSTERRELOAD) and the guns hold the rounds they held then: none comes back full for nothing (the next level
		// loads its own from the carried ammunition, TopUpForLevelStart)
		UWorld* Wd = GetWorld();
		AOutlawsPlayer* P = Wd && Wd->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(Wd->GetFirstPlayerController()->GetPawn()) : nullptr;
		if (P && RebuildKind != ERebuildKind::NextLevel) P->RestoreEntryMags();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT restart with what %s was entered with: %d items, gold %d, armour %d"), *LevelName, Counts.Num(), Gold, Armour);
	}
	StartSaveDelay = 1.5f;                 // every level start autosaves (a load cancels it)
	AutosaveWait = 0.f;
	// back to the start, as BeginPlay does the first time
	PlacePlayerTries = 0;
	bNeedPlace = bPlacePlayer && bHavePlayerStart && !TryPlacePlayer();
	SetActorTickEnabled(true);
	FadeCamera(1.f, 0.f, 1.0f, false);
	if (AfterRestart)
	{
		TFunction<void()> F = MoveTemp(AfterRestart);
		AfterRestart = nullptr;
		F();
	}
	if (bHom && LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase)) HomOnOffice();
	else if (bHom) { HomWounds = 0; HomGoldAtStart = Gold; HomKillsAtStart = Kills; }      // (a mission begins)
}

void AOutlawsLevelActor::ShowMessage(int32 Id, int32 Count, const FString& Fallback)
{
	const FString* Text = UserMessages.Find(Id);
	const FString Line = (Text ? *Text : Fallback).Replace(TEXT("%d"), *FString::FromInt(Count));
	if (Line.IsEmpty()) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT msg %d: %s"), Id, *Line);
	Notify(Line, 3.f);
}

void AOutlawsLevelActor::Notify(const FString& Line, float Seconds)
{
	// the game's messages go to the line in front of the player's eyes
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn))
	{
		P->ShowMessage(Line, Seconds);
	}
	else if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, Seconds, FColor::White, Line);
	}
}

FVector AOutlawsLevelActor::PickupLocationByClass(const FString& ItemClass) const
{
	for (const FPickup& P : Pickups)
	{
		if (!P.Actor.IsValid()) continue;
		for (const FPickup::FEntry& E : P.Entries)
		{
			if (E.Class.Equals(ItemClass, ESearchCase::IgnoreCase)) return P.Actor->GetActorLocation();
		}
	}
	return FVector::ZeroVector;
}

bool AOutlawsLevelActor::RegisterPickup(AActor* Sprite, const FString& ObjectName, const FString& ItmText)
{
	FString Func;
	TMap<FString, FString> Data;
	OutlawsFormats::ParseItm(ItmText, Func, Data);
	// the Sheriff's Badge (Inv_GSheriffStar) is a ground item like any other: PICKUP ibadge, its message and twirl.wav
	const bool bGround = Func.Equals(TEXT("Inv_GroundObject"), ESearchCase::IgnoreCase) || Func.Equals(TEXT("Inv_GSheriffStar"), ESearchCase::IgnoreCase);
	const bool bInstant = Func.Equals(TEXT("Inv_InstantGroundObject"), ESearchCase::IgnoreCase);
	if (!Sprite || (!bGround && !bInstant))
	{
		return false;
	}
	FPickup P;
	P.Actor = Sprite;
	P.Ground = ObjectName.ToLower();
	P.Sound = Data.FindRef(TEXT("PICKUP_SOUND"));
	P.bInstant = bInstant;
	if (bInstant)
	{
		// canteen / elixir: HEALTH_B tenths of full health, on touch
		P.HealthB = FCString::Atoi(*Data.FindRef(TEXT("HEALTH_B")));
		FPickup::FEntry E;
		E.Count = 0;
		E.Class = Data.FindRef(TEXT("ITEM_CLASS")).ToLower();
		E.Message = Data.Contains(TEXT("PICKUP_MSG")) ? FCString::Atoi(*Data[TEXT("PICKUP_MSG")]) : -1;
		P.Entries.Add(E);
		P.Message = E.Message;
	}
	else
	{
		// PICKUP_COUNT entries: PICKUP / COUNT / PICKUP_MSG / ITEM_CLASS, then PICKUP1 / COUNT1 ...
		const int32 N = Data.Contains(TEXT("PICKUP_COUNT")) ? FMath::Max(1, FCString::Atoi(*Data[TEXT("PICKUP_COUNT")])) : 1;
		for (int32 i = 0; i < N; ++i)
		{
			const FString Sfx = i == 0 ? FString() : FString::FromInt(i);
			const FString* Item = Data.Find(TEXT("PICKUP") + Sfx);
			if (!Item) continue;
			FPickup::FEntry E;
			E.Item = FPaths::GetBaseFilename(*Item).ToLower();
			E.Count = Data.Contains(TEXT("COUNT") + Sfx) ? FCString::Atoi(*Data[TEXT("COUNT") + Sfx]) : 1;
			E.Message = Data.Contains(TEXT("PICKUP_MSG") + Sfx) ? FCString::Atoi(*Data[TEXT("PICKUP_MSG") + Sfx]) : -1;
			E.Class = Data.FindRef(TEXT("ITEM_CLASS") + Sfx).ToLower();
			if (E.Class.IsEmpty())
			{
				if (const FItemDef* D = ItemDefs.Find(E.Item)) E.Class = D->Class;
			}
			P.Entries.Add(E);
		}
		if (P.Entries.Num() == 0)
		{
			return false;
		}
		P.Item = P.Entries[0].Item;
		P.Message = P.Entries[0].Message;
		const int32* Id = UseIdByItem.Find(P.Item);
		P.UseId = Id ? *Id : -1;
	}
	// (CPU1 object LOD) only the plain supplies may fade out far away (the classes the randomizer moves: ammunition,
	// weapons, health, armour, gold, oil); keys, puzzle pieces, story and mission items, anything without a class and
	// every pickup in Historical Missions stay drawn (as their sprite at most)
	if (AOutlawsSpriteActor* Sa = Cast<AOutlawsSpriteActor>(P.Actor.Get()))
	{
		static const TCHAR* const Plain[] = { TEXT("ammo"), TEXT("weap"), TEXT("heal"), TEXT("armr"), TEXT("gbar"), TEXT("gsak"), TEXT("fuel") };
		bool bPlain = !bHom && P.Entries.Num() > 0 && P.Ground != TEXT("gidol");
		for (const FPickup::FEntry& En : P.Entries)
		{
			bool bOk = false;
			for (const TCHAR* C : Plain) bOk |= En.Class.Equals(C, ESearchCase::IgnoreCase);
			bPlain &= bOk;
		}
		Sa->bLodKeep = !bPlain;
	}
	Pickups.Add(P);
	return true;
}

int32 AOutlawsLevelActor::CollectPickupsAt(FVector PawnLocation, float CapsuleRadius, float CapsuleHalfHeight, bool bFeetOnly)
{
	int32 Got = 0;
	// VR (bFeetOnly): only what the player steps on, so hands can reach items first
	// VR: the player's RADIUS (1.5 u = 45 cm, cidjames.itm) meets the item's half width (ground item
	// ITMs carry no RADIUS of their own)
	const double BaseReach = bFeetOnly ? FMath::Max<double>(CapsuleRadius, 1.5 * FOutlawsGeometry::UnitsToCm) : CapsuleRadius + PickupReach;
	const double Feet = PawnLocation.Z - CapsuleHalfHeight;
	for (int32 i = Pickups.Num() - 1; i >= 0; --i)
	{
		FPickup& P = Pickups[i];
		AActor* A = P.Actor.Get();
		if (!A)
		{
			Pickups.RemoveAt(i);
			continue;
		}
		if (P.bHeld)
		{
			continue;
		}
		// sprites stand on their anchor: touch = within reach across, between the feet and the head
		const FVector L = A->GetActorLocation();
		const double Reach = BaseReach + (bFeetOnly ? 0.5 * A->GetActorScale3D().Y : 0.0);
		const double Dist2 = FVector2D::DistSquared(FVector2D(L.X, L.Y), FVector2D(PawnLocation.X, PawnLocation.Y));
		const bool bNear = Dist2 <= Reach * Reach;
		bool bTouch = bNear && (bFeetOnly ? (L.Z >= Feet - 30.0 && L.Z <= Feet + 40.0)
			: (L.Z >= PawnLocation.Z - CapsuleHalfHeight - PickupReach && L.Z <= PawnLocation.Z + CapsuleHalfHeight + PickupReach));
		// VR: on a desk, table or shelf (up to 1.7 m over the feet) walking up to it takes it too, a little
		// further across (the furniture's edge keeps the body back), when nothing stands between the chest
		// and it (not through a wall or a shelf's back) (user 2026-09-27)
		if (bFeetOnly && !bTouch && L.Z > Feet + 40.0 && L.Z <= Feet + 170.0 && Dist2 <= FMath::Square(Reach + 25.0))
		{
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsRaisedPickup), false, A);
			if (UWorld* W = GetWorld())
			{
				if (APlayerController* PC = W->GetFirstPlayerController()) Q.AddIgnoredActor(PC->GetPawn());
				FHitResult Hit;
				const FVector Chest = PawnLocation + FVector(0.0, 0.0, CapsuleHalfHeight * 0.4);
				bTouch = !W->LineTraceSingleByChannel(Hit, Chest, L + FVector(0.0, 0.0, 12.0), ECC_Visibility, Q);
			}
		}
		if (P.bDropLock)
		{
			// (user 2026-10-02) dropped from the wheel by this player: theirs again only once they have been 1.5 m from it
			// (across), or 30 s on; the other player in co-op takes it at once (their machine's copy has no lock)
			const double Away = FPlatformTime::Seconds() - P.DropLockAt;
			if (Dist2 <= FMath::Square(150.0) && Away < 30.0) continue;
			P.bDropLock = false;
			P.bWaitClear = bTouch;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop %s armed for its dropper (%s)"), *P.Ground, Away >= 30.0 ? TEXT("30 s on") : TEXT("stepped 1.5 m away"));
		}
		if (P.bWaitClear)
		{
			if (!bTouch) P.bWaitClear = false;       // the player stepped away: walking over it works again
			continue;
		}
		if (!bTouch)
		{
			continue;
		}
		if (!CollectPickup(i))
		{
			P.bWaitClear = true;      // nothing fitted (full ammo, worse plate, full health): it stays until the player steps away
			continue;
		}
		++Got;
	}
	return Got;
}

bool AOutlawsLevelActor::GiveEntry(const FPickup::FEntry& E)
{
	if (E.Item.Equals(TEXT("ibadge"), ESearchCase::IgnoreCase))
	{
		// 30 s at full speed (the slider's 100 %); slower, longer, but not by as much as the speed drops (user 2026-10-01:
		// the run to the next room on the badge, a known tactic, still harder slow): the speed's share to the 0.6
		BadgeLeft = BadgeSeconds * FMath::Pow(0.5f / FMath::Max(0.05f, Pace()), 0.6f);
		SetActorTickEnabled(true);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT badge power for %.0f s (pace %.2f)"), BadgeLeft, Pace());
		return true;
	}
	if (E.Item.IsEmpty() || !AddCount(E.Item, E.Count))
	{
		return false;
	}
	// what the wrist lists: keys, puzzle pieces, documents, weapons and the like; ammo, plates,
	// heals and gold are counted instead
	static const TSet<FString> Counted = { TEXT("ammo"), TEXT("armr"), TEXT("heal"), TEXT("gbar"), TEXT("gsak"), TEXT("fuel") };
	if (!Counted.Contains(E.Class) && IsListedItem(E.Item))
	{
		const FString* Text = UserMessages.Find(E.Message);
		Inventory.Add(E.Item);
		InventoryLabels.Add(Text ? *Text : E.Item);
	}
	return true;
}

bool AOutlawsLevelActor::CollectPickup(int32 Index, bool bSilent)
{
	if (!Pickups.IsValidIndex(Index))
	{
		return false;
	}
	FPickup& P = Pickups[Index];
	AActor* A = P.Actor.Get();
	const FVector L = A ? A->GetActorLocation() : GetActorLocation();
	const bool bCoop = UOutlawsCoop::IsActive(this);
	if (bCoop) UOutlawsCoop::BeforeTake(this);          // (co-op: the host may yet give it to the other player)
	if (P.bInstant)
	{
		// canteen / elixir: only below full health
		if (Health >= 10.f || bDead)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT pickup %s refused: %s"), *P.Ground, bDead ? TEXT("the player is dead") : TEXT("full health"));
			return false;
		}
		Health = FMath::Min(10.f, Health + P.HealthB / 10.f);
		ShowMessage(P.Message, 0, P.Ground);
	}
	else
	{
		// take what fits; a pickup that gives nothing stays on the ground
		bool bAny = false;
		for (const FPickup::FEntry& E : P.Entries)
		{
			if (GiveEntry(E))
			{
				bAny = true;
				ShowMessage(E.Message, E.Count, E.Item);
			}
		}
		if (!bAny)
		{
			// (no "You already have the maximum of that item." line: the pickup just stays; user 2026-09-28)
			// (BUG-KNIFEPICKUP) why, in the log (Shipping's too): each entry's item, how many carried and the most
			TArray<FString> Why;
			for (const FPickup::FEntry& E : P.Entries)
			{
				const FItemDef* D = ItemDefs.Find(E.Item.ToLower());
				if (E.Item.IsEmpty()) Why.Add(TEXT("an entry with no item"));
				else if (D && D->Class == TEXT("armr")) Why.Add(FString::Printf(TEXT("%s: plate %d no better than the %d worn"), *E.Item, E.Count, Armour));
				else Why.Add(FString::Printf(TEXT("%s: %d carried, the most %d"), *E.Item, GetCount(E.Item), D ? D->MaxCount : 1));
			}
			UE_LOG(LogOutlaws, Log, TEXT("OLRT pickup %s refused: nothing fitted (%s)"), *P.Ground, Why.Num() ? *FString::Join(Why, TEXT("; ")) : TEXT("no entries"));
			return false;
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT picked up %s (%s): health %.1f armour %d"), *P.Ground, *P.Item, Health, Armour);
	UOutlawsStats::NoteItem(this);           // (REQ-STATS)
	if (bCoop) UOutlawsCoop::NotePickupTaken(this, Index);     // (co-op: the other machine takes it away)
	if (!bSilent) PlayWav(P.Sound.IsEmpty() ? TEXT("powerup") : P.Sound, L);
	Pickups.RemoveAt(Index);
	if (A)
	{
		for (FDynamite& D : Dynamite)
		{
			if (D.Mesh.IsValid() && D.Mesh->GetOwner() == A) { D.bGone = true; D.Mesh = nullptr; }     // a stick taken: no blast from it
		}
		// counted now; its look flies to this player's chest and shrinks away, however it was taken (user 2026-10-02)
		UWorld* W = GetWorld();
		AOutlawsPlayer* Me = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
		if (!Me || !Me->FlyCollected(A)) A->Destroy();
	}
	return true;
}

bool AOutlawsLevelActor::CanSee(const FVector& From, const FVector& To, const AActor* Target) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return true;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSight), true);
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn()) Q.AddIgnoredActor(Pawn);
	}
	if (Target) Q.AddIgnoredActor(Target);
	FHitResult Hit;
	return !World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q, NoPawns());
}

AActor* AOutlawsLevelActor::FindPickupAlong(FVector Eye, FVector Hand, float MaxRange, float MaxAngleDeg) const
{
	const int32 Best = PickupAlongIndex(Eye, Hand, MaxRange, MaxAngleDeg);
	return Best >= 0 ? Pickups[Best].Actor.Get() : nullptr;
}

int32 AOutlawsLevelActor::PickupAlongIndex(FVector Eye, FVector Hand, float MaxRange, float MaxAngleDeg) const
{
	const FVector ToHand = Hand - Eye;
	const double HandDist = ToHand.Size();
	if (HandDist < 5.0)
	{
		return -1;
	}
	const FVector Aim = ToHand / HandDist;
	int32 Best = -1;
	double BestScore = 1e30;
	for (int32 i = 0; i < Pickups.Num(); ++i)
	{
		const FPickup& P = Pickups[i];
		AActor* A = P.Actor.Get();
		if (!A || P.bHeld || P.bDropLock) continue;          // (a wheel drop not armed for its dropper yet, user 2026-10-02)
		const double H = FMath::Max(1.0, A->GetActorScale3D().Z);
		const FVector C = A->GetActorLocation() + FVector(0.0, 0.0, H * 0.5);
		const FVector ToObj = C - Eye;
		const double Dist = ToObj.Size();
		if (Dist > MaxRange || Dist <= HandDist) continue;      // the hand must be between the face and it
		// the angle off the eye-to-hand line, less the item's own angular size
		const double Off = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Aim, ToObj / Dist), -1.0, 1.0)));
		const double Size = FMath::RadiansToDegrees(FMath::Atan2(0.5 * FMath::Max(A->GetActorScale3D().Y, H), Dist));
		const double Score = Off - Size;
		if (Score < MaxAngleDeg && Score < BestScore && CanSee(Eye, C, A))
		{
			BestScore = Score;
			Best = i;
		}
	}
	return Best;
}

AActor* AOutlawsLevelActor::PullPickupAlong(FVector Eye, FVector Hand, float MaxRange, float MaxAngleDeg)
{
	const int32 Best = PickupAlongIndex(Eye, Hand, MaxRange, MaxAngleDeg);
	if (Best < 0)
	{
		return nullptr;
	}
	FPickup& P = Pickups[Best];
	P.bHeld = true;
	P.bWaitClear = false;
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NotePickupHeld(this, Best);     // (co-op: the other machine hides it)
	AActor* A = P.Actor.Get();
	PlayWav(P.Sound.IsEmpty() ? TEXT("powerup") : P.Sound, A->GetActorLocation());
	return A;
}

AActor* AOutlawsLevelActor::FindPickupNear(FVector Eye, FVector Hand, float Radius) const
{
	const int32 Best = PickupNearIndex(Eye, Hand, Radius);
	return Best >= 0 ? Pickups[Best].Actor.Get() : nullptr;
}

int32 AOutlawsLevelActor::PickupNearIndex(FVector Eye, FVector Hand, float Radius) const
{
	int32 Best = -1;
	double BestD = Radius;
	for (int32 i = 0; i < Pickups.Num(); ++i)
	{
		const FPickup& P = Pickups[i];
		AActor* A = P.Actor.Get();
		if (!A || P.bHeld || P.bDropLock) continue;          // (a wheel drop not armed for its dropper yet, user 2026-10-02)
		// distance from the hand to the sprite's upright centre line (anchor at its bottom); a stick of dynamite (a
		// model lying along the floor): to its box
		const FVector Bottom = A->GetActorLocation();
		const double H = FMath::Max(1.0, A->GetActorScale3D().Z);
		const FVector Near = FMath::ClosestPointOnSegment(Hand, Bottom, Bottom + FVector(0.0, 0.0, H));
		const bool bModel = P.Ground.StartsWith(TEXT("gdynam"));
		const double D = bModel ? FMath::Sqrt(A->GetComponentsBoundingBox(true).ComputeSquaredDistanceToPoint(Hand))
			: FVector::Dist(Hand, Near) - 0.25 * A->GetActorScale3D().Y;   // a little of its width counts
		if (D < BestD && CanSee(Eye, Bottom + FVector(0.0, 0.0, H * 0.5), A))
		{
			BestD = D;
			Best = i;
		}
	}
	return Best;
}

AActor* AOutlawsLevelActor::TakePickupNear(FVector Eye, FVector Hand, float Radius)
{
	const int32 Best = PickupNearIndex(Eye, Hand, Radius);
	if (Best < 0)
	{
		return nullptr;
	}
	Pickups[Best].bHeld = true;
	Pickups[Best].bWaitClear = false;
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NotePickupHeld(this, Best);     // (co-op: the other machine hides it)
	AActor* A = Pickups[Best].Actor.Get();
	A->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);   // it may have been riding a lift
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hand took %s"), *Pickups[Best].Ground);
	return A;
}

bool AOutlawsLevelActor::CollectPickupActor(AActor* Item, bool bSilent)
{
	for (int32 i = 0; i < Pickups.Num(); ++i)
	{
		if (Pickups[i].Actor.Get() == Item)
		{
			return CollectPickup(i, bSilent);      // false: nothing fitted, the hand still holds it
		}
	}
	return false;
}

void AOutlawsLevelActor::DropPickupActor(AActor* Item, FVector Eye)
{
	UWorld* World = GetWorld();
	for (FPickup& P : Pickups)
	{
		if (P.Actor.Get() != Item || !Item) continue;
		P.bHeld = false;
		P.bWaitClear = true;
		Item->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		// sprites have no physics: set it down on whatever is below the hand, on the player's side of
		// any wall the hand reached through
		if (World)
		{
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDrop), true);
			APawn* Pawn = nullptr;
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				Pawn = PC->GetPawn();
				if (Pawn) Q.AddIgnoredActor(Pawn);
			}
			Q.AddIgnoredActor(Item);
			FVector From = Item->GetActorLocation();
			FHitResult Hit;
			if (Pawn && World->LineTraceSingleByChannel(Hit, Eye, From, ECC_Visibility, Q, NoPawns()))
			{
				From = Hit.ImpactPoint - (From - Eye).GetSafeNormal() * 10.0;
			}
			if (World->LineTraceSingleByChannel(Hit, From, From - FVector(0.0, 0.0, 5000.0), ECC_Visibility, Q, NoPawns()))
			{
				Item->SetActorLocation(Hit.ImpactPoint + FVector(0.0, 0.0, 0.5));
				// landed on a lift floor or a carrying door: ride it like an authored rider
				if (const int32* Gi = ComponentGroup.Find(Hit.GetComponent()))
				{
					const FMoverGroup& G = Groups[*Gi];
					if (!G.bMorph || (Inf && Inf->Inf.Items[Inf->Elevators[G.Elevator].Item].Classes[Inf->Elevators[G.Elevator].ClassIndex].bMoveObjects))
					{
						Item->AttachToComponent(Hit.GetComponent(), FAttachmentTransformRules::KeepWorldTransform);
					}
				}
			}
			else if (Pawn)
			{
				Item->SetActorLocation(Pawn->GetActorLocation() - FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight() - 0.5));
			}
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hand dropped %s"), *P.Ground);
		if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NotePickupDropped(this, Item);   // (co-op: shown there again)
		return;
	}
}

void AOutlawsLevelActor::ShootAt(const FHitResult& Hit)
{
	if (!Inf)
	{
		return;
	}
	bool bTaken = false;
	if (const int32* Gi = ComponentGroup.Find(Hit.GetComponent()))
	{
		bTaken |= Inf->SectorEvent(Inf->Elevators[Groups[*Gi].Elevator].Sectors[0], OutlawsInf::EvShoot);
	}
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Hit.ImpactPoint, PlayerSector, Walls);
	for (const TPair<int32, int32>& SW : Walls)
	{
		bTaken |= Inf->WallEvent(SW.Key, SW.Value, OutlawsInf::EvShoot);
	}
	if (bTaken)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT shot a scripted surface at %s"), *Hit.ImpactPoint.ToString());
	}
}

namespace
{
	// the surface under a palm: along the eye-to-hand line, from a little behind the hand (a palm
	// already pushed into the wall) to a little past it
	bool HandTrace(UWorld* World, const FVector& Eye, const FVector& Hand, FHitResult& Hit)
	{
		const FVector Dir = (Hand - Eye).GetSafeNormal();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsHandUse), true);
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn()) Q.AddIgnoredActor(Pawn);
		}
		return World->LineTraceSingleByChannel(Hit, Hand - Dir * 25.0, Hand + Dir * 15.0, ECC_Visibility, Q, NoPawns());
	}
}

int32 AOutlawsLevelActor::DoorAtHand(FVector Eye, FVector Hand, bool& bOutClosed, int32 FromSector) const
{
	const int32 Near = FromSector >= 0 ? FromSector : PlayerSector;   // whose side of the door the hand is on
	UWorld* World = GetWorld();
	bOutClosed = false;
	FHitResult Hit;
	if (!Inf || !World || !HandTrace(World, Eye, Hand, Hit))
	{
		return -1;
	}
	// a push needs the surface at the fist (or the fist already through it): within 14 cm of it (8 missed pushes that
	// stopped short of the leaf, the body keeping the player off the wall: user 2026-10-01)
	if (FVector::DotProduct(Hit.ImpactPoint - Hand, (Hand - Eye).GetSafeNormal()) > 14.0)
	{
		return -1;
	}
	// a door sector: closed means every door elevator in it sits at its first stop with nothing
	// queued.  A push only ever opens: one on a moving, held-open or parked-open door would be
	// the nudge that shuts it (Trigger cuts a wait short and steps to the next stop)
	auto DoorSector = [this](int32 Sector, bool& bClosed) -> bool
	{
		// (BUG-TRAINDOOR2) a sector with no mover at all still goes on to the nudge-trigger rule below: LAST_DOOR has none,
		// and the early return here kept 1b6b1d9's rule from ever being reached
		static const TArray<int32> NoElevators;
		const TArray<int32>* Found = Inf->ElevatorsBySector.Find(Sector);
		const TArray<int32>* Es = Found ? Found : &NoElevators;
		bool bDoor = false;
		bClosed = true;
		for (int32 e : *Es)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			if (!E.IsMorph() && E.Class != EOutlawsInfClass::MoveCeiling) continue;
			bDoor = true;
			// (BUG-BOXPUSHONCE) a pushable box (SHOVE_ME: MORPH_MOVE2 with HOLD stops 0, 4, 8, 12 and a TERMINATE at 16)
			// takes a push at every stop it rests at, so a stop whose next one goes further on, not back to the first,
			// counts as shut too; a two-stop door at its open stop still steps back to stop 0 and is left alone
			const int32 N = Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num();
			const bool bOnward = E.Stop > 0 && E.Stop + 1 < N && FMath::Abs(Inf->StopValue(E, E.Stop + 1) - Inf->StopValue(E, 0)) > 1e-6;
			bClosed = bClosed && (E.Stop == 0 || bOnward) && E.Target < 0 && !E.bWaiting && !E.bTerminated && E.bMasterOn;
		}
		if (bDoor) return true;
		// (user 2026-10-03, BUG-TRAINDOOR: "I should just be able to push it like any other door") a sector named as a door
		// with a nudge trigger of its own and no mover (the train's LAST_DOOR, END_LEVEL on a nudge): pushed as a door is
		const TArray<int32>* Ts = Inf->TriggersBySector.Find(Sector);
		if (!Ts || !Inf->Live.Sectors.IsValidIndex(Sector) || !Inf->Live.Sectors[Sector].Name.Contains(TEXT("DOOR"))) return false;
		for (int32 ti : *Ts)
		{
			const FOutlawsTrigger& Tr = Inf->Triggers[ti];
			if (!Tr.bLine && Tr.bMaster && (Tr.Mask & (OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack)) != 0) { bClosed = true; return true; }
		}
		return false;
	};
	if (const int32* Gi = ComponentGroup.Find(Hit.GetComponent()))
	{
		const int32 Master = Inf->Elevators[Groups[*Gi].Elevator].Sectors[0];
		return DoorSector(Master, bOutClosed) ? Master : -1;
	}
	// a wall opening onto a door sector (the frame beside the leaf); a door frame is always next
	// to the player's sector, so the level-wide wall search is skipped for this per-frame probe
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Hit.ImpactPoint, Near, Walls, true);
	for (const TPair<int32, int32>& SW : Walls)
	{
		const FOutlawsWall& W = Inf->Live.Sectors[SW.Key].Walls[SW.Value];
		for (int32 Adj : { W.Adjoin, W.DAdjoin })
		{
			if (Adj >= 0 && Adj != Near && DoorSector(Adj, bOutClosed))
			{
				return Adj;
			}
		}
	}
	return -1;
}

#if WITH_OUTLAWS_TESTS
FString AOutlawsLevelActor::DebugDoorProbe(const FVector& Eye, const FVector& Hand) const
{
	// the surface under the hand, the walls there and their adjoins with names, and what DoorAtHand makes of it
	UWorld* World = GetWorld();
	FHitResult Hit;
	if (!Inf || !World || !HandTrace(World, Eye, Hand, Hit)) return TEXT("no surface under the hand");
	const UPrimitiveComponent* C = Hit.GetComponent();
	const int32* Gi = ComponentGroup.Find(C);
	FString S = FString::Printf(TEXT("hit %s (owner %s) %.1f cm past the hand, group %d, player sector %d;"),
		C ? *C->GetName() : TEXT("none"), C && C->GetOwner() ? *C->GetOwner()->GetName() : TEXT("none"),
		FVector::DotProduct(Hit.ImpactPoint - Hand, (Hand - Eye).GetSafeNormal()), Gi ? *Gi : -1, PlayerSector);
	if (Gi) S += FString::Printf(TEXT(" group master sector %d;"), Inf->Elevators[Groups[*Gi].Elevator].Sectors[0]);
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Hit.ImpactPoint, PlayerSector, Walls, true);
	for (const TPair<int32, int32>& SW : Walls)
	{
		const FOutlawsWall& W = Inf->Live.Sectors[SW.Key].Walls[SW.Value];
		S += FString::Printf(TEXT(" wall %d.%d adjoin %d '%s' dadjoin %d"), SW.Key, SW.Value, W.Adjoin,
			Inf->Live.Sectors.IsValidIndex(W.Adjoin) ? *Inf->Live.Sectors[W.Adjoin].Name : TEXT(""), W.DAdjoin);
		if (const TArray<int32>* Es = Inf->ElevatorsBySector.Find(W.Adjoin))
		{
			for (int32 e : *Es) S += FString::Printf(TEXT(" elev %d class %d morph %d"), e, (int32)Inf->Elevators[e].Class, Inf->Elevators[e].IsMorph() ? 1 : 0);
		}
		if (const TArray<int32>* Ts = Inf->TriggersBySector.Find(W.Adjoin))
		{
			for (int32 ti : *Ts) S += FString::Printf(TEXT(" trig %d line %d master %d mask 0x%x"), ti, Inf->Triggers[ti].bLine ? 1 : 0, Inf->Triggers[ti].bMaster ? 1 : 0, Inf->Triggers[ti].Mask);
		}
		S += TEXT(";");
	}
	bool bClosed = false;
	const int32 Door = DoorAtHand(Eye, Hand, bClosed);
	S += FString::Printf(TEXT(" DoorAtHand %d closed %d"), Door, bClosed ? 1 : 0);
	return S;
}
#endif // WITH_OUTLAWS_TESTS

FString AOutlawsLevelActor::DebugSectorBrief(int32 Si) const
{
	if (!Inf || !Inf->Live.Sectors.IsValidIndex(Si)) return TEXT("no sector");
	const FOutlawsSector& S = Inf->Live.Sectors[Si];
	FBox2D Box(ForceInit);
	for (const FVector2D& V : S.Verts) Box += V;
	FString R = FString::Printf(TEXT("sector %d '%s' flags 0x%x floor %.2f ceil %.2f verts %d box (%.1f %.1f)-(%.1f %.1f) UE X %.0f..%.0f Y %.0f..%.0f"), Si, *S.Name, S.Flags, S.Floor.Y, S.Ceil.Y, S.Verts.Num(),
		Box.Min.X, Box.Min.Y, Box.Max.X, Box.Max.Y, Box.Min.Y * 30.0, Box.Max.Y * 30.0, Box.Min.X * 30.0, Box.Max.X * 30.0);
	if (const TArray<int32>* Es = Inf->ElevatorsBySector.Find(Si))
	{
		for (int32 e : *Es) R += FString::Printf(TEXT("; elev %d '%s' class %d stop %d"), e, *Inf->Elevators[e].Name, (int32)Inf->Elevators[e].Class, Inf->Elevators[e].Stop);
	}
	return R;
}

#if WITH_OUTLAWS_TESTS
FString AOutlawsLevelActor::DebugWallProbe(const FVector& Feet, double Radius) const
{
	// (BUG-WALLSTUCK) the sector at the feet, every wall of it and its neighbours within Radius of the feet (distance, adjoin,
	// textures, flags, the neighbour's floor and ceiling), and a ring of 16 traces at knee, waist and head height
	if (!Inf) return TEXT("no level");
	const double U = FOutlawsGeometry::UnitsToCm;
	const FOutlawsSector* At = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(Feet, 0.1));
	FString S = FString::Printf(TEXT("feet in sector %d"), At ? At->Index : -1);
	if (!At) return S;
	S += FString::Printf(TEXT(" '%s' floor %.2f ceil %.2f flags 0x%x;"), *At->Name, At->Floor.Y, At->Ceil.Y, At->Flags);
	TSet<int32> Seen;
	TArray<int32> Check = { At->Index };
	const FVector2D P = FOutlawsGeometry::ToOutlawsPlan(Feet);
	for (const FOutlawsSector& Any : Inf->Live.Sectors)
	{
		// every sector over or under the spot too (any layer, any height), with its floor and ceiling
		if (Any.Verts.Num() >= 3 && Any.Index != At->Index && Inf->Live.ContainsXZ(Any, P.X, P.Y))
		{
			Check.AddUnique(Any.Index);
			S += FString::Printf(TEXT(" also over/under: %d floor %.2f ceil %.2f;"), Any.Index, Any.Floor.Y, Any.Ceil.Y);
		}
	}
	for (int32 n = Check.Num() - 1; n >= 0; --n)
	{
		for (const FOutlawsWall& W : Inf->Live.Sectors[Check[n]].Walls) { if (W.Adjoin >= 0) Check.AddUnique(W.Adjoin); }
	}
	for (int32 Si : Check)
	{
		if (!Inf->Live.Sectors.IsValidIndex(Si)) continue;
		const FOutlawsSector& Sec = Inf->Live.Sectors[Si];
		for (int32 w = 0; w < Sec.Walls.Num(); ++w)
		{
			const FOutlawsWall& W = Sec.Walls[w];
			const FVector2D A = Sec.Verts[W.V1], B = Sec.Verts[W.V2];
			const double D = FMath::PointDistToSegment(FVector(P, 0.0), FVector(A, 0.0), FVector(B, 0.0)) * U;
			if (D > Radius) continue;
			const FOutlawsSector* Adj = Inf->Live.Sectors.IsValidIndex(W.Adjoin) ? &Inf->Live.Sectors[W.Adjoin] : nullptr;
			S += FString::Printf(TEXT(" [%d.%d %.0f cm adj %d mid %d top %d bot %d f1 0x%x f2 0x%x; sec floor %.2f ceil %.2f"), Si, w, D, W.Adjoin, W.Mid, W.Top, W.Bot, W.Flags1, W.Flags2, Sec.Floor.Y, Sec.Ceil.Y);
			if (Adj) S += FString::Printf(TEXT("; adj floor %.2f ceil %.2f flags 0x%x"), Adj->Floor.Y, Adj->Ceil.Y, Adj->Flags);
			S += TEXT("]");
		}
	}
	if (UWorld* World = GetWorld())
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsWallProbe), true);
		if (APlayerController* PC = World->GetFirstPlayerController()) { if (APawn* Pawn = PC->GetPawn()) Q.AddIgnoredActor(Pawn); }
		for (double H : { 40.0, 100.0, 160.0 })
		{
			S += FString::Printf(TEXT(" | at %.0f cm:"), H);
			for (int32 k = 0; k < 16; ++k)
			{
				const FVector Dir = FRotator(0.0, k * 22.5, 0.0).Vector();
				const FVector From = Feet + FVector(0.0, 0.0, H);
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, From, From + Dir * Radius, ECC_Visibility, Q))
					S += FString::Printf(TEXT(" %d:%.0f%s"), k, Hit.Distance, Hit.bStartPenetrating ? TEXT("in") : TEXT(""));
			}
		}
	}
	return S;
}
#endif // WITH_OUTLAWS_TESTS

bool AOutlawsLevelActor::PushDoorSector(int32 Sector)
{
	// the nudge a hand push sends: straight to the door's sector, so the walls beside the frame
	// (a switch or trigger line sharing the corner) are not tripped as a hand use would
	if (!Inf || !Inf->Live.Sectors.IsValidIndex(Sector))
	{
		return false;
	}
	const bool bTaken = Inf->SectorEvent(Sector, Sector == PlayerSector ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack);
	if (bTaken)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hand push: sector %d"), Sector);
	}
	return bTaken;
}

bool AOutlawsLevelActor::HandEvent(int32 Sector, int32 Wall, uint32 Event)
{
	// one hand gesture is one press: the hand driven into a switch and the grip closing on it, or both hands, within
	// 0.7 s count once (user 2026-09-29: the mill's GATE4 gates, which open and close on each press, rose and dropped
	// again as if timed)
	if (!Inf) return false;
	const uint64 K = FOutlawsBuildFilter::Key(Sector, Wall);
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (const double* T = HandUseAt.Find(K); T && Now - *T < 0.7) return true;
	const bool bTaken = Wall >= 0 ? Inf->WallEvent(Sector, Wall, Event) : Inf->SectorEvent(Sector, Event);
	if (bTaken) HandUseAt.Add(K, Now);
	return bTaken;
}

bool AOutlawsLevelActor::HandUseSpot(FVector Eye, FVector Hand, FVector& OutAt, FVector& OutNormal) const
{
	UWorld* World = GetWorld();
	if (!Inf || !World) return false;
	FHitResult Hit;
	if (!HandTrace(World, Eye, Hand, Hit)) return false;
	// within the push's reach of the fist (DoorAtHand's 14 cm)
	if (FVector::DotProduct(Hit.ImpactPoint - Hand, (Hand - Eye).GetSafeNormal()) > 14.0) return false;
	OutAt = Hit.ImpactPoint;
	OutNormal = Hit.ImpactNormal;
	if (const int32* Gi = ComponentGroup.Find(Hit.GetComponent()))
	{
		const FMoverGroup& G = Groups[*Gi];
		const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
		return G.bMorph || E.Class == EOutlawsInfClass::MoveCeiling;
	}
	const uint32 Nudge = OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack;
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Hit.ImpactPoint, PlayerSector, Walls);
	for (const TPair<int32, int32>& SW : Walls)
	{
		if (const TArray<int32>* Ts = Inf->TriggersByWall.Find(FOutlawsInfRuntime::WallKey(SW.Key, SW.Value)))
		{
			for (const int32 t : *Ts) if (Inf->Triggers.IsValidIndex(t) && (Inf->Triggers[t].Mask & Nudge)) return true;
		}
		const FOutlawsWall& Wl = Inf->Live.Sectors[SW.Key].Walls[SW.Value];
		for (const int32 Adj : { Wl.Adjoin, Wl.DAdjoin })
		{
			if (Adj < 0 || Adj == PlayerSector) continue;
			if (const TArray<int32>* Es = Inf->ElevatorsBySector.Find(Adj))
			{
				for (const int32 e : *Es) if (Inf->Elevators[e].IsMorph() || Inf->Elevators[e].Class == EOutlawsInfClass::MoveCeiling) return true;
			}
		}
	}
	return false;
}

bool AOutlawsLevelActor::UseByHand(FVector Eye, FVector Hand, bool bIncludePlatforms)
{
	UWorld* World = GetWorld();
	if (!Inf || !World)
	{
		return false;
	}
	if (TurnPropByHand(Hand))
	{
		return true;
	}
	FHitResult Hit;
	if (!HandTrace(World, Eye, Hand, Hit))
	{
		return false;
	}
	// a door leaf (MORPH_*, MOVE_CEILING) is used by touch; a lift or platform (MOVE_FLOOR, MOVE_FC)
	// is left to ledge climbing unless the caller asks for it (no ledge there)
	if (const int32* Gi = ComponentGroup.Find(Hit.GetComponent()))
	{
		const FMoverGroup& G = Groups[*Gi];
		const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
		const bool bDoor = G.bMorph || E.Class == EOutlawsInfClass::MoveCeiling;
		if (!bDoor && !bIncludePlatforms)
		{
			return false;
		}
		const int32 Master = E.Sectors[0];
		const uint32 Ev = Master == PlayerSector ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack;
		bool bTaken = HandEvent(Master, -1, Ev);
		if (!bTaken && bDoor)
		{
			// the door's own walls may carry the line triggers instead (ranch's BOBLOCKED and RHDOOR: opened from one
			// face, "locked from the other side" from the other; user 2026-09-29): the pushed face, at its authored
			// place, whatever height on the leaf the hand met
			FVector P = Hit.ImpactPoint;
			if (E.Class == EOutlawsInfClass::MorphSpin) P = G.Pivot + FRotator(0.0, -E.Current, 0.0).UnrotateVector(P - G.Pivot);
			else if (E.Class == EOutlawsInfClass::MorphMove) P -= G.Dir * E.Current;
			TArray<TPair<int32, int32>> Faces;
			WallsAt(P, Master, Faces, true, true);
			for (const TPair<int32, int32>& SW : Faces)
			{
				if (SW.Key == Master) bTaken |= HandEvent(SW.Key, SW.Value, Ev);
			}
		}
		if (bTaken)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT hand use: %s"), *E.Name);
		}
		return bTaken;
	}
	// a scripted wall (switch, trigger line) or a wall opening onto a door sector: only counts when
	// something took the nudge
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Hit.ImpactPoint, PlayerSector, Walls);
	bool bUsed = false;
	for (const TPair<int32, int32>& SW : Walls)
	{
		bUsed |= HandEvent(SW.Key, SW.Value, SW.Key == PlayerSector ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack);
		const FOutlawsWall& W = Inf->Live.Sectors[SW.Key].Walls[SW.Value];
		for (int32 Adj : { W.Adjoin, W.DAdjoin })
		{
			if (Adj < 0 || Adj == PlayerSector) continue;
			// a sector with only triggers (the townex guestbook) takes the nudge as the Jedi engine's
			// use does (it nudges the next sector's whole INF list, triggers too)
			const TArray<int32>* Es = Inf->ElevatorsBySector.Find(Adj);
			const bool bTriggers = Inf->TriggersBySector.Contains(Adj);
			if (!Es && !bTriggers) continue;
			bool bDoorSector = bIncludePlatforms || !Es;
			if (Es)
			{
				for (int32 e : *Es)
				{
					const FOutlawsElevator& E = Inf->Elevators[e];
					bDoorSector |= E.IsMorph() || E.Class == EOutlawsInfClass::MoveCeiling;
				}
			}
			if (bDoorSector)
			{
				bUsed |= HandEvent(Adj, -1, OutlawsInf::EvNudgeBack);
			}
		}
	}
	// a palm flat on a surface (the guestbook's desk top, away from its edges): the sector it rests on
	if (!bUsed)
	{
		const FOutlawsSector* On = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(Hit.ImpactPoint, 0.1));
		if (On && On->Index != PlayerSector && Inf->TriggersBySector.Contains(On->Index))
		{
			bUsed = HandEvent(On->Index, -1, OutlawsInf::EvNudgeBack);
		}
	}
	if (bUsed)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hand use: wall at %s"), *Hit.ImpactPoint.ToString());
	}
	return bUsed;
}

void AOutlawsLevelActor::UpdatePickups()
{
	OL_PERF_SCOPE("Level: pickups");
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn || Pickups.Num() == 0)
	{
		return;
	}
	float Radius = 40.f, Half = 90.f;
	if (const ACharacter* Ch = Cast<ACharacter>(Pawn))
	{
		Radius = Ch->GetCapsuleComponent()->GetScaledCapsuleRadius();
		Half = Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	}
	const AOutlawsPlayer* Player = Cast<AOutlawsPlayer>(Pawn);
	CollectPickupsAt(Pawn->GetActorLocation(), Radius, Half, Player && Player->IsVR());
}

FVector AOutlawsLevelActor::PickupLocation(const FString& GroundItem) const
{
	for (const FPickup& P : Pickups)
	{
		if (P.Ground.Equals(GroundItem, ESearchCase::IgnoreCase) && P.Actor.IsValid())
		{
			return P.Actor->GetActorLocation();
		}
	}
	return FVector::ZeroVector;
}

void AOutlawsLevelActor::BuildInf(const FOutlawsLevel& L, const FString& InfText, FOutlawsBuildFilter& SF)
{
	Inf = MakeShared<FOutlawsInfRuntime>();
	Inf->Init(L, InfText);     // elevators are placed at stop 0 in Inf->Live
	const FOutlawsLevel& Live = Inf->Live;
	// (REQ-SECTORLIGHT) the sectors a CHANGE_LIGHT lights (its master and slaves; the piano's roll too): their faces are
	// built into sections of their own, drawn at the light's level as it moves (TickSectorLights)
	for (int32 e = 0; FOutlawsInfRuntime::SectorLightsOn() && e < Inf->Elevators.Num(); ++e)
	{
		const FOutlawsElevator& E = Inf->Elevators[e];
		if (!E.IsLight() || E.BaseLight.Num() != E.Sectors.Num()) continue;
		for (int32 i = 0; i < E.Sectors.Num(); ++i)
		{
			if (!FOutlawsGeometry::SectorLight.Contains(E.Sectors[i])) FOutlawsGeometry::SectorLight.Add(E.Sectors[i], FIntVector(e, E.BaseLight[i] - E.BaseLight[0], 0));
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT sector light %s (elevator %d): sectors %s, AMBIENT %s, %d stops, speed %.1f, now %d"), *E.Name, e,
			*FString::JoinBy(E.Sectors, TEXT(","), [](int32 s) { return FString::FromInt(s); }), *FString::JoinBy(E.BaseLight, TEXT(","), [](int32 a) { return FString::FromInt(a); }),
			Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num(), E.Speed, FMath::FloorToInt(E.Current));
	}
	// "ELEVATOR EXPLODE" sectors (no stops: the runtime keeps no elevator for them): their open faces and the mirrors
	// become a wall of their own that a blast removes (BuildExplodeWalls); the static world keeps their bands but not
	// their signs (user 2026-09-29: the cliff's crack floated over an open hole)
	for (const FOutlawsInfItem& It : Inf->Inf.Items)
	{
		if (It.bLine || !It.Classes.ContainsByPredicate([](const FOutlawsInfClass& C) { return C.Class == EOutlawsInfClass::Explode; })) continue;
		TArray<int32> Secs;
		Inf->FindSectors(It.Name, It.NameId, Secs);
		FExplodeWall X;
		X.Name = It.Name;
		X.Sector = Secs.Num() > 0 ? Secs[0] : -1;
		for (int32 s : Secs)
		{
			const FOutlawsSector& S = Live.Sectors[s];
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				if (W.Adjoin < 0 || !Live.Sectors.IsValidIndex(W.Adjoin) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				X.Walls.AddUnique(FOutlawsBuildFilter::Key(s, w));
				if (Live.Sectors[W.Adjoin].Walls.IsValidIndex(W.Mirror)) X.Walls.AddUnique(FOutlawsBuildFilter::Key(W.Adjoin, W.Mirror));
				const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
				const FVector Q[4] = { FOutlawsGeometry::ToUE(A.X, S.Floor.Y, A.Y), FOutlawsGeometry::ToUE(B.X, S.Floor.Y, B.Y),
					FOutlawsGeometry::ToUE(B.X, S.Ceil.Y, B.Y), FOutlawsGeometry::ToUE(A.X, S.Ceil.Y, A.Y) };
				const FVector2D UV[4];
				X.Blocker.AddFace(Q, UV, 4, 1.f, false);
				X.Blocker.AddFace(Q, UV, 4, 1.f, true);
				for (const FVector& P : Q) X.Box += P;
			}
		}
		for (const uint64 K : X.Walls)
		{
			SF.SkipWalls.Add(K);
			SF.WallBands.FindOrAdd(K) |= 8;          // (a partial skip: the sign goes with the wall)
		}
		if (X.Walls.Num() > 0)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT INF %s: an explodable wall of %d faces (sector %d)"), *It.Name, X.Walls.Num(), X.Sector);
			ExplodeWalls.Add(MoveTemp(X));
		}
	}
	TSet<int32> ClaimedFloor, ClaimedCeil;
	TSet<uint64> ClaimedWalls;
	for (int32 e = 0; e < Inf->Elevators.Num(); ++e)
	{
		FOutlawsElevator& E = Inf->Elevators[e];
		if (E.IsScroll())
		{
			continue;          // its surfaces are a scroll mesh (BuildScrolls), it only drives the offset
		}
		const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
		FMoverGroup G;
		G.Elevator = e;
		G.bMorph = E.IsMorph();
		TSet<int32> Secs;
		for (int32 s : E.Sectors) Secs.Add(s);
		// an automatic door, or a morph whose surrounding faces carry no "moves with the elevator" flag at all
		// (hideout's RHSECRET: nothing would swing but its floor): every face on it moves with it
		bool bAutoDoor = E.Sectors.Num() > 0 && Inf->AutoDoors.Contains(E.Sectors[0]);
		if (!bAutoDoor && E.IsMorph())
		{
			bool bAnyFlagged = false, bAnyFacing = false;
			for (const FOutlawsSector& S : Live.Sectors)
			{
				if (Secs.Contains(S.Index)) continue;
				for (const FOutlawsWall& W : S.Walls)
				{
					if (Secs.Contains(W.Adjoin) || Secs.Contains(W.DAdjoin))
					{
						bAnyFacing = true;
						bAnyFlagged |= (W.Flags1 & 0x40) != 0;
					}
				}
			}
			if (bAnyFacing && !bAnyFlagged)
			{
				bAutoDoor = true;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT INF %s: no face flagged to move with it, so all of them do"), *E.Name);
			}
		}

		// walls: the group's own, plus neighbours' walls on it (for doors only those flagged to
		// move with the elevator, Dark Forces wall flag 0x40)
		TSet<uint64> Walls;
		for (const FOutlawsSector& S : Live.Sectors)
		{
			const bool bOwn = Secs.Contains(S.Index);
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				const bool bAdj = Secs.Contains(W.Adjoin) || Secs.Contains(W.DAdjoin);
				if (bOwn || (bAdj && (!G.bMorph || bAutoDoor || (W.Flags1 & 0x40))))
				{
					const uint64 K = FOutlawsBuildFilter::Key(S.Index, w);
					if (!ClaimedWalls.Contains(K))
					{
						Walls.Add(K);
						ClaimedWalls.Add(K);
					}
				}
			}
		}
		G.GroupFilter.bOnly = true;
		G.GroupFilter.OnlyWalls = Walls;
		G.FloorFilter.bOnly = true;
		G.CeilFilter.bOnly = true;
		for (int32 s : E.Sectors)
		{
			const FOutlawsSector& S = Live.Sectors[s];
			if (G.bMorph)
			{
				SF.SkipFloor.Add(s);
				SF.SkipCeil.Add(s);
				// the room around a door fills its door-shaped hole; the door keeps only the
				// surfaces that differ from the room's (the top of a raised-floor door leaf)
				SF.FillHolesOf.Add(s);
				const FOutlawsSector* Parent = FindParentSector(Live, s);
				if (!ClaimedFloor.Contains(s) && !(Parent && FMath::Abs(Parent->Floor.Y - S.Floor.Y) < 1e-3)) G.GroupFilter.OnlyFloor.Add(s);
				if (!ClaimedCeil.Contains(s) && !(Parent && FMath::Abs(Parent->Ceil.Y - S.Ceil.Y) < 1e-3)) G.GroupFilter.OnlyCeil.Add(s);
				ClaimedFloor.Add(s);
				ClaimedCeil.Add(s);
			}
			else
			{
				// only the surface it moves (the other stays with the world, or goes to the item's second elevator:
				// ranch's HAY1 floor and ceiling; user 2026-09-29)
				const bool bF = E.Class == EOutlawsInfClass::MoveFloor || E.Class == EOutlawsInfClass::MoveFC;
				const bool bC = E.Class == EOutlawsInfClass::MoveCeiling || E.Class == EOutlawsInfClass::MoveFC;
				if (bF && !ClaimedFloor.Contains(s)) { G.FloorFilter.OnlyFloor.Add(s); SF.SkipFloor.Add(s); ClaimedFloor.Add(s); }
				if (bC && !ClaimedCeil.Contains(s)) { G.CeilFilter.OnlyCeil.Add(s); SF.SkipCeil.Add(s); ClaimedCeil.Add(s); }
			}
		}
		SF.SkipWalls.Append(Walls);

		if (E.Class == EOutlawsInfClass::MorphSpin)
		{
			FVector2D Center = C.Center;
			if (!C.bHasCenter && E.Sectors.Num() > 0)
			{
				const FOutlawsSector& S = Live.Sectors[E.Sectors[0]];
				Center = FVector2D::ZeroVector;
				for (const FVector2D& V : S.Verts) Center += V;
				Center /= FMath::Max(1, S.Verts.Num());
			}
			G.Pivot = FOutlawsGeometry::ToUE(Center.X, 0.0, Center.Y);
			G.Pivot.Z = 0.0;
		}
		else if (E.Class == EOutlawsInfClass::MorphMove)
		{
			// ANGLE uses the yaw convention: 0 = north, clockwise; north is UE +X
			const double A = FMath::DegreesToRadians(C.Angle);
			G.Dir = FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * FOutlawsGeometry::UnitsToCm;
		}
		E.Group = Groups.Add(G);
	}
	// scrolls: out of the static world onto their own meshes (BuildScrolls), one per kind of motion;
	// surfaces a mover already owns stay with the mover
	ScrollMeshes.Reset();
	TMap<FString, int32> ScrollKinds;
	for (int32 i = 0; i < Inf->Scrolls.Num(); ++i)
	{
		const FOutlawsScroll& Sc = Inf->Scrolls[i];
		const FString Kind = FString::Printf(TEXT("%d %.3f %.3f %.3f %d %d"), int32(Sc.Class), Sc.Dir.X, Sc.Dir.Y, Sc.Speed, Sc.Elevator, Sc.Light);
		int32* Found = ScrollKinds.Find(Kind);
		if (!Found)
		{
			FScrollMesh M;
			M.Scroll = i;
			M.bWall = Sc.Class == EOutlawsInfClass::ScrollWall;
			M.bCeil = Sc.Class == EOutlawsInfClass::ScrollCeiling;
			if (!M.bWall && Sc.Sectors.Num() > 0 && Live.Sectors.IsValidIndex(Sc.Sectors[0]))
			{
				FVector2D O;
				const FOutlawsSector& S0 = Live.Sectors[Sc.Sectors[0]];
				Live.SurfaceTexFrame(S0, !M.bCeil, O, M.FrameA, M.FrameN, M.Stretch);
				M.Rad = FMath::DegreesToRadians(-(M.bCeil ? S0.Ceil.Angle : S0.Floor.Angle));
			}
			M.Filter.bOnly = true;
			Found = &ScrollKinds.Add(Kind, ScrollMeshes.Add(M));
		}
		FOutlawsBuildFilter& F = ScrollMeshes[*Found].Filter;
		for (int32 s : Sc.Sectors)
		{
			if (Sc.Class == EOutlawsInfClass::ScrollFloor && !SF.SkipFloor.Contains(s)) { SF.SkipFloor.Add(s); F.OnlyFloor.Add(s); }
			else if (Sc.Class == EOutlawsInfClass::ScrollCeiling && !SF.SkipCeil.Contains(s)) { SF.SkipCeil.Add(s); F.OnlyCeil.Add(s); }
			else if (Sc.Class == EOutlawsInfClass::ScrollWall)
			{
				const FOutlawsSector& S = Live.Sectors[s];
				for (int32 w = 0; w < S.Walls.Num(); ++w)
				{
					if (Sc.OnlyWalls.Num() > 0 && !Sc.OnlyWalls.Contains(w)) continue;
					const uint64 K = FOutlawsBuildFilter::Key(s, w);
					if ((S.Walls[w].Flags1 & OutlawsFlags::WallScrollAny) && !SF.SkipWalls.Contains(K))
					{
						// only the bands the flags name scroll (SCROLLTOP 0x80, SCROLLMID 0x100, SCROLLBOTTOM 0x200, SCROLLSIGN
						// 0x400); the others stay with the static world (user 2026-09-29)
						const uint32 Fl = S.Walls[w].Flags1;
						const uint8 Mask = (Fl & 0x80 ? 1 : 0) | (Fl & 0x100 ? 2 : 0) | (Fl & 0x200 ? 4 : 0) | (Fl & 0x400 ? 8 : 0);
						SF.SkipWalls.Add(K);
						SF.WallBands.Add(K, Mask);
						F.OnlyWalls.Add(K);
						F.WallBands.Add(K, Mask);
					}
				}
			}
		}
	}
	// (BUG-MILLVIS) a scrolling floor or ceiling round a door leaf fills the leaf's hole as the static world does (the
	// mill's GRATE swung open in GWATER7.2's running water and left a black slit where it had stood)
	const bool bOldMillvis = OLFlags::Param(TEXT("OLOldMillvis"));     // (-OLOldMillvis: as before, for shots)
	if (!bOldMillvis) for (FScrollMesh& M : ScrollMeshes) M.Filter.FillHolesOf = SF.FillHolesOf;
	for (const FOutlawsTrigger& T : Inf->Triggers)
	{
		if (T.bLine && (T.Mask & (OutlawsInf::EvCrossFront | OutlawsInf::EvCrossBack)))
		{
			CrossWalls.AddUnique(FOutlawsInfRuntime::WallKey(T.Sector, T.Wall));
		}
	}
	Inf->OnWallMessage = [this](int32 S, int32 W, const FOutlawsInfMessage& M) { AtxWallMessage(S, W, M); };
	// (BUG-HOMCELL) MASTER_ON to a figure generator: its figure shows, once (the original generates its one figure)
	Inf->OnObjectMessage = [this](const FString& Id, const FOutlawsInfMessage& M)
	{
		const TWeakObjectPtr<AOutlawsSpriteActor>* G = CellGens.Find(Id.ToUpper());
		if (!G || !G->IsValid() || M.Type != EOutlawsInfMsg::MasterOn) return;
		(*G)->SetActorHiddenInGame(false);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT figure generator %s (#%s) turned on: shown at %s"), *(*G)->OutlawsName, *Id, *(*G)->GetActorLocation().ToCompactString());
	};
	Inf->OnElevatorMoved = [this](const FOutlawsElevator& E)
	{
		if (Groups.IsValidIndex(E.Group)) Groups[E.Group].bDirty = true;
		// (user 2026-10-03, BUG-HAYBALE) a second elevator on the same sector (ranch's HAY1: its floor and its ceiling
		// each their own) moves walls the first one's group holds: that group is rebuilt too, or the bale's sides hang
		// where the floor stopped while its ceiling falls on
		for (int32 g = 0; g < Groups.Num(); ++g)
		{
			if (g == E.Group || Groups[g].bDirty || !Inf->Elevators.IsValidIndex(Groups[g].Elevator)) continue;
			const FOutlawsElevator& O = Inf->Elevators[Groups[g].Elevator];
			if (O.IsMorph()) continue;
			for (int32 s : O.Sectors) if (E.Sectors.Contains(s)) { Groups[g].bDirty = true; break; }
		}
	};
	Inf->OnSound = [this](const FString& Wav, const FOutlawsElevator& E) { PlayWav(Wav, ElevatorLocation(E)); };
	Inf->OnLoopSound = [this](const FString& Wav, const FOutlawsElevator& E, bool bStart)
	{
		const int32 Key = int32(&E - Inf->Elevators.GetData());
		if (TuneSound(Key, Wav, ElevatorLocation(E), bStart)) return;     // (REQ-PIANO: a tune plays once, to its end)
		if (TWeakObjectPtr<UAudioComponent>* Old = LoopSounds.Find(Key))
		{
			if (Old->IsValid()) (*Old)->Stop();
			LoopSounds.Remove(Key);
		}
		if (bStart)
		{
			if (UAudioComponent* AC = PlayWav(Wav, ElevatorLocation(E), true)) LoopSounds.Add(Key, AC);
		}
	};
	Inf->OnKeyCheck = [this](int32 UseId, bool bHasIt, const FOutlawsElevator& E)
	{
		// the item's own sounds: "unlock" when it opens the lock, "locked" (the rattle) when missing
		const FItemInfo* Info = ItemsByUseId.Find(UseId);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT INF lock %s needs item %d (%s): %s"), *E.Name, UseId, Info ? *Info->Name : TEXT("?"), bHasIt ? TEXT("opened") : TEXT("locked"));
		if (Info)
		{
			PlayWav(bHasIt ? Info->UseOk : Info->UseFail, ElevatorLocation(E));
		}
	};
	Inf->OnItemUsedUp = [this](int32 UseId) { UseUpItem(UseId); };
	Inf->OnUserMessage = [this](int32 N)
	{
		const FString* Text = UserMessages.Find(N);
		const FString Line = Text ? *Text : FString::Printf(TEXT("[message %d]"), N);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT INF user message %d: %s"), N, *Line);
		Notify(Line, 4.f);
	};
	Inf->OnLevelEvent = [this](const FString& What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT INF level event %s"), *What);
		if (What == TEXT("END_LEVEL") || What == TEXT("COMPLETE"))
		{
			CompleteLevel(TEXT("INF ") + What);      // the train ends this way (train.inf END_LEVEL)
		}
		else if (What.StartsWith(TEXT("SPAWN_LEVEL ")))
		{
			HomSpawnLevel(What.Mid(12));             // (HM1: an office poster: ride out to that mission)
		}
		else if (What.StartsWith(TEXT("TRIGGER_MOVIE ")))
		{
			TriggerMidLevelMovie(What.Mid(14));      // the ranch's boss door: ram.san, then Bob fights (user 2026-10-03)
		}
		else
		{
			Notify(What, 6.f);
		}
	};
	SetupTunes();
}

double AOutlawsLevelActor::WavSeconds(const FString& Name) const
{
	if (!AssetsRef || Name.IsEmpty() || FChar::IsDigit(Name[0])) return 0.0;
	TArray<uint8> D;
	FString File = Name;
	if (!File.Contains(TEXT("."))) File += TEXT(".wav");
	if (!AssetsRef->GetSound(File, false, D) || D.Num() <= 12 || FMemory::Memcmp(D.GetData(), "RIFF", 4) != 0) return 0.0;
	FWav W;
	ParseRiffWav(D, W);
	return double(W.Pcm.Num()) / double(2 * FMath::Max(1, W.Channels) * FMath::Max(1, W.Rate));
}

void AOutlawsLevelActor::SetupTunes()
{
	// REQ-PIANO (townex, town): the piano.  PIANO_KEYS (TRIGGER STANDARD, EVENT_MASK 48, client LEFT_PIANO_DOOR) opens the
	// doors; RIGHT_PIANO_DOOR lights SCROLL (the roll behind them, its SCROLL_WALL runs while lit) and starts MUSIC_BOX,
	// a hidden MOVE_FLOOR whose moving sound (SOUND 2) is PIANO.WAV, a 10 s tune; its arrival shuts the doors, darkens the
	// roll and puts PIANO_KEYS' master back on.  Here a moving sound of 5 s or more is a tune (no other level's is over
	// 3.7 s): it plays once to its end rather than looping until the arrival, and the move lasts the tune (INFERRED: at
	// SPEED .25 the box arrives in 4 s, the doors shutting mid-tune), so the roll scrolls while it plays
	TuneSeconds.Reset();
	TuneSpeeds.Reset();
	KeySectors.Reset();
	TuneBusyUntil = -1.0;
	if (!Inf) return;
	for (int32 e = 0; e < Inf->Elevators.Num(); ++e)
	{
		FOutlawsElevator& E = Inf->Elevators[e];
		const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
		const FString* S2 = C.Sounds.Find(2);
		if (!S2 || E.IsScroll()) continue;
		const double Len = WavSeconds(*S2);
		if (Len < 5.0) continue;
		double Span = 0.0;
		for (int32 s = 0; s < C.Stops.Num(); ++s) Span = FMath::Max(Span, FMath::Abs(Inf->StopValue(E, s) - E.Current));
		if (Span > 0.0) E.Speed = Span / Len;
		TuneSeconds.Add(e, Len);
		TuneSpeeds.Add(e, E.Speed);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT piano: %s's moving sound %s is a tune of %.2f s: it plays once, the move takes as long (speed %.4f)"), *E.Name, **S2, Len, E.Speed);
	}
	// the keys: a sector with a nudge trigger whose top carries the piano keys texture (PIANOKEY.PCX)
	const uint32 Nudge = OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack;
	for (const TPair<int32, TArray<int32>>& Kv : Inf->TriggersBySector)
	{
		if (!Inf->Live.Sectors.IsValidIndex(Kv.Key)) continue;
		const FOutlawsSector& S = Inf->Live.Sectors[Kv.Key];
		if (!Inf->Live.Textures.IsValidIndex(S.Floor.Tex) || !Inf->Live.Textures[S.Floor.Tex].StartsWith(TEXT("PIANOKEY"), ESearchCase::IgnoreCase)) continue;
		bool bNudge = false;
		for (const int32 t : Kv.Value) bNudge |= Inf->Triggers.IsValidIndex(t) && (Inf->Triggers[t].Mask & Nudge) != 0;
		if (!bNudge) continue;
		KeySectors.Add(Kv.Key);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT piano: the keys are sector %d (%s)"), Kv.Key, *S.Name);
	}
	if (KeySectors.Num() == 0) return;
	// the lockout: the keys take no press while the tune plays and for 2 s after it ends (the host's word in co-op: a
	// client's press comes here as the host's own)
	Inf->OnEventGate = [this](int32 Sector, uint32 Event)
	{
		if (!KeySectors.Contains(Sector) || !GetWorld()) return true;
		const double Now = GetWorld()->GetTimeSeconds();
		if (Now >= TuneBusyUntil) return true;
		++TuneRefusals;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT piano: the keys (sector %d) refused, %.1f s of the lockout left"), Sector, TuneBusyUntil - Now);
		return false;
	};
}

bool AOutlawsLevelActor::TuneSound(int32 Elevator, const FString& Wav, const FVector& At, bool bStart)
{
	const double* Len = TuneSeconds.Find(Elevator);
	if (!Len) return false;
	if (!bStart) return true;            // (its arrival does not cut it)
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	TuneBusyUntil = Now + *Len + 2.0;
	++TuneStarts;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT piano: the tune %s starts (%.2f s; the keys locked for %.2f s)"), *Wav, *Len, *Len + 2.0);
	PlayWav(Wav, At);
	return true;
}

FVector AOutlawsLevelActor::ElevatorLocation(const FOutlawsElevator& E) const
{
	if (!Inf || E.Sectors.Num() == 0) return GetActorLocation();
	const FOutlawsSector& S = Inf->Live.Sectors[E.Sectors[0]];
	FVector2D C = FVector2D::ZeroVector;
	for (const FVector2D& V : S.Verts) C += V;
	C /= FMath::Max(1, S.Verts.Num());
	return FOutlawsGeometry::ToUE(C.X, S.Floor.Y + 3.0, C.Y);
}

void AOutlawsLevelActor::CreateMoverMeshes()
{
	for (int32 gi = 0; gi < Groups.Num(); ++gi)
	{
		FMoverGroup& G = Groups[gi];
		const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
		int32 Tris = 0;
		if (G.bMorph)
		{
			FOutlawsGeometry GG;
			GG.Build(Inf->Live, TexInfoCache, bReverseWinding, &G.GroupFilter);
			if (GG.World.Num() == 0) continue;
			G.Main = NewMeshComponent(TEXT("OutlawsDoor"), true);
			AddSections(G.Main, GG.World, true, WorldMaterial, WorldMaskedMaterial, Tris);
			MoverMeshes.Add(G.Main);
			ComponentGroup.Add(G.Main, gi);
			G.BuiltValue = 0.0;        // morph geometry is built as authored
			G.Shape = MoveTemp(GG.World);
		}
		else
		{
			FOutlawsGeometry GF;
			GF.Build(Inf->Live, TexInfoCache, bReverseWinding, &G.FloorFilter);
			if (GF.World.Num() > 0 || GF.Water.Num() > 0)
			{
				G.Floor = NewMeshComponent(TEXT("OutlawsLiftFloor"), true);
				const int32 Next = AddSections(G.Floor, GF.World, true, WorldMaterial, WorldMaskedMaterial, Tris);
				AddSections(G.Floor, GF.Water, false, WaterMaterial, WaterMaterial, Tris, Next);
				MoverMeshes.Add(G.Floor);
				ComponentGroup.Add(G.Floor, gi);
			}
			FOutlawsGeometry GC;
			GC.Build(Inf->Live, TexInfoCache, bReverseWinding, &G.CeilFilter);
			if (GC.World.Num() > 0)
			{
				G.Ceil = NewMeshComponent(TEXT("OutlawsLiftCeiling"), true);
				AddSections(G.Ceil, GC.World, true, WorldMaterial, WorldMaskedMaterial, Tris);
				MoverMeshes.Add(G.Ceil);
				ComponentGroup.Add(G.Ceil, gi);
			}
			G.Main = NewMeshComponent(TEXT("OutlawsLiftWalls"), false);          // drawn (rebuilt as it moves)
			MoverMeshes.Add(G.Main);
			ComponentGroup.Add(G.Main, gi);                                      // (its collision: CookLiftWalls)
			G.BuiltValue = E.Current;  // Live already holds the stop-0 heights
		}
		G.bDirty = true;
		UpdateGroup(G);
		CookLiftWalls(G, true);
	}
	// (QSPEED-3) a door or lift moving every frame asked the physics for its overlaps at every move; nothing here reads
	// overlap events (blocking is unchanged)
	if (!OLFlags::Param(TEXT("OLOldMovers"))) for (UProceduralMeshComponent* M : MoverMeshes) if (M) M->SetGenerateOverlapEvents(false);
	BuildMoverBatches();
}

void AOutlawsLevelActor::UpdateGroup(FMoverGroup& G)
{
	G.bDirty = false;
	if (!Inf || !Inf->Elevators.IsValidIndex(G.Elevator)) return;
	const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
	++G.Updates;
	if (G.bMorph)
	{
		OL_PERF_SCOPE("Movers: morph moves");
		if (!G.Main) return;
		if (E.Class == EOutlawsInfClass::MorphSpin)
		{
			// INF spin angles are counter-clockwise seen from above (Jedi sector_rotateWalls: standard
			// x/z rotation, object yaw -= delta); UE yaw turns clockwise, hence the minus
			const FRotator R(0.0, -E.Current, 0.0);
			G.Main->SetWorldLocationAndRotation(G.Pivot - R.RotateVector(G.Pivot), R);
		}
		else
		{
			G.Main->SetWorldLocation(G.Dir * E.Current);
		}
		return;
	}
	const double Dz = (E.Current - G.BuiltValue) * FOutlawsGeometry::UnitsToCm;
	const bool bFloor = E.Class == EOutlawsInfClass::MoveFloor || E.Class == EOutlawsInfClass::MoveFC;
	const bool bCeil = E.Class == EOutlawsInfClass::MoveCeiling || E.Class == EOutlawsInfClass::MoveFC;
	{
		OL_PERF_SCOPE("Movers: lift floors");
		if (G.Floor && bFloor) G.Floor->SetWorldLocation(FVector(0.0, 0.0, Dz));
		if (G.Ceil && bCeil) G.Ceil->SetWorldLocation(FVector(0.0, 0.0, Dz));
	}
	if (G.Main)
	{
		// (QSPEED-3) the drawn walls of a lift moving out of sight with no player near wait: built (and cooked) once a
		// second, when its collision was cooked before too; at once when it comes into view, a player comes near or it stops
		G.bWallsLate = WallsCanWait(G);
		if (G.bWallsLate) return;
		OL_PERF_SCOPE("Movers: wall rebuilds");
		FillFloorShift(G.GroupFilter);
		FOutlawsGeometry GW;
		GW.Build(Inf->Live, TexInfoCache, bReverseWinding, &G.GroupFilter);
		G.Main->ClearAllMeshSections();
		int32 Tris = 0;
		AddSections(G.Main, GW.World, false, WorldMaterial, WorldMaskedMaterial, Tris);
		G.WallShape = MoveTemp(GW.World);
		// its fingerprint (CookLiftWalls: a shape seen before is not cooked again): the faces in the builder's order
		TArray<FString> Keys;
		G.WallShape.GetKeys(Keys);
		Keys.Sort();
		uint32 Hash = 0;
		for (const FString& K : Keys)
		{
			const FOutlawsMeshSection& S = G.WallShape[K];
			Hash = FCrc::StrCrc32(*K, Hash);
			Hash = FCrc::MemCrc32(S.Positions.GetData(), S.Positions.Num() * sizeof(FVector), Hash);
			Hash = FCrc::MemCrc32(S.Triangles.GetData(), S.Triangles.Num() * sizeof(int32), Hash);
		}
		G.WallHash = Hash;
		G.bSolidStale = true;
		CookLiftWalls(G, false);
	}
}

bool AOutlawsLevelActor::PlayerNearMover(const FMoverGroup& G) const
{
	// any player's pawn within 15 m of the walls
	UWorld* World = GetWorld();
	const FBox B = G.Main ? G.Main->Bounds.GetBox() : FBox(ForceInit);
	if (!World || !B.IsValid) return false;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APawn* Pw = It->IsValid() ? (*It)->GetPawn() : nullptr;
		if (Pw && B.ComputeSquaredDistanceToPoint(Pw->GetActorLocation()) < FMath::Square(1500.0)) return true;
	}
	return false;
}

bool AOutlawsLevelActor::WallsCanWait(const FMoverGroup& G) const
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldMovers"));      // (-OLOldMovers: every move rebuilt, as before)
	if (bOld || G.bMorph || !G.Main || !Inf || !Inf->Elevators.IsValidIndex(G.Elevator) || Inf->Elevators[G.Elevator].Target < 0) return false;
	const int32 g = int32(&G - Groups.GetData());
	if (!GroupCulled.IsValidIndex(g) || !GroupCulled[g]) return false;            // drawn: built every move
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - G.SolidCookedAt >= 1.0) return false;                                  // its collision's once a second
	return !PlayerNearMover(G);
}

void AOutlawsLevelActor::CookLiftWalls(FMoverGroup& G, bool bForce)
{
	if (G.bMorph || !G.Main || !G.bSolidStale || !Inf || !Inf->Elevators.IsValidIndex(G.Elevator)) return;
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	int32 Use = G.Solids.IndexOfByPredicate([&G](const FMoverGroup::FWallSolid& W) { return W.Mesh && W.Hash == G.WallHash; });
	if (Use == INDEX_NONE)
	{
		const bool bMoving = Inf->Elevators[G.Elevator].Target >= 0;
		if (!bForce && bMoving && Now - G.SolidCookedAt < 1.0)
		{
			// a new shape while it moves: cooked at once near a player (any player's pawn within 15 m of the walls),
			// else once a second; at once when it stops
			bool bNear = false;
			const FBox B = G.Main->Bounds.GetBox();
			if (World && B.IsValid)
			{
				for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It && !bNear; ++It)
				{
					const APawn* Pw = It->IsValid() ? (*It)->GetPawn() : nullptr;
					bNear = Pw && B.ComputeSquaredDistanceToPoint(Pw->GetActorLocation()) < FMath::Square(1500.0);
				}
			}
			if (!bNear) return;
		}
		// a copy for it: a new one (up to 4), else the one used longest ago cooked again
		if (G.Solids.Num() < 4)
		{
			FMoverGroup::FWallSolid W;
			W.Mesh = NewMeshComponent(TEXT("OutlawsLiftWallsSolid"), true);
			W.Mesh->SetVisibility(false);
			W.Mesh->SetHiddenInGame(true);
			if (!OLFlags::Param(TEXT("OLOldMovers"))) W.Mesh->SetGenerateOverlapEvents(false);
			MoverMeshes.Add(W.Mesh);
			ComponentGroup.Add(W.Mesh, Groups.IndexOfByPredicate([&G](const FMoverGroup& O) { return &O == &G; }));
			Use = G.Solids.Add(W);
		}
		else
		{
			Use = G.ActiveSolid == 0 ? 1 : 0;
			for (int32 i = 0; i < G.Solids.Num(); ++i) if (i != G.ActiveSolid && G.Solids[i].UsedAt < G.Solids[Use].UsedAt) Use = i;
		}
		OL_PERF_SCOPE("Movers: wall cooks");
		FMoverGroup::FWallSolid& W = G.Solids[Use];
		W.Hash = G.WallHash;
		W.Mesh->ClearAllMeshSections();
		int32 Tris = 0;
		AddSections(W.Mesh, G.WallShape, true, WorldMaterial, WorldMaskedMaterial, Tris);
		G.SolidCookedAt = Now;
	}
	G.bSolidStale = false;
	G.Solids[Use].UsedAt = Now;
	if (Use != G.ActiveSolid)
	{
		// the copy for this shape collides, the one before stops (its cooked body kept for when that shape comes back)
		if (G.Solids.IsValidIndex(G.ActiveSolid) && G.Solids[G.ActiveSolid].Mesh) G.Solids[G.ActiveSolid].Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		G.Solids[Use].Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		G.ActiveSolid = Use;
	}
}

void AOutlawsLevelActor::FillFloorShift(FOutlawsBuildFilter& F) const
{
	// how far each lift floor now sits from where the level put it (flagged walls follow it or stay)
	F.FloorShift.Reset();
	for (const FOutlawsElevator& E : Inf->Elevators)
	{
		if (E.Class != EOutlawsInfClass::MoveFloor && E.Class != EOutlawsInfClass::MoveFC) continue;
		for (int32 i = 0; i < E.Sectors.Num(); ++i)
		{
			const double Shift = Inf->Live.Sectors[E.Sectors[i]].Floor.Y - E.BaseFloor[i];
			if (FMath::Abs(Shift) > 1e-6) F.FloorShift.Add(E.Sectors[i], Shift);
		}
	}
}

void AOutlawsLevelActor::TickInf(float Dt)
{
	OL_PERF_SCOPE("Level: INF doors/lifts");
	if (!Inf) return;
	Inf->WaitScale = 1.0 / FMath::Max(0.1f, CoopWaitPace >= 0.f ? CoopWaitPace : Pace());      // (the stops' waits at the player's pace; co-op: the slower player's)
	{
		OL_PERF_SCOPE("Level: INF run");               // (CPU1 step 0a: the parts of the total)
		Inf->Tick(Dt);
	}
	TickSectorLights();
	static const bool bStill = OLFlags::Param(TEXT("OLStillScrolls"));     // (tests: the train's ground held still)
	if (!bStill) TickScrolls();
	{
		OL_PERF_SCOPE("Level: track player + V1");
		TrackPlayer();
	}
	OL_PERF_SCOPE("Level: mover rebuilds");
	for (FMoverGroup& G : Groups)
	{
		if (G.bDirty) UpdateGroup(G);
		else if (G.bWallsLate && !WallsCanWait(G)) UpdateGroup(G);     // (QSPEED-3: come into view, a player near, a second gone)
		else if (G.bSolidStale) CookLiftWalls(G, false);      // (a lift come to rest, or a player come near)
	}
	TickMoverBatches();
#if WITH_OUTLAWS_TESTS
	static const bool bMoverStats = OLFlags::Param(TEXT("OLMoverStats"));
	if (bMoverStats && GetWorld() && GetWorld()->GetTimeSeconds() >= MoverStatsNext)
	{
		// (-OLMoverStats) which doors and lifts were rebuilt or moved in the last 10 s, and how often
		MoverStatsNext = GetWorld()->GetTimeSeconds() + 10.0;
		FString Line;
		for (FMoverGroup& G : Groups)
		{
			if (G.Updates > 0 && Inf->Elevators.IsValidIndex(G.Elevator))
			{
				const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
				Line += FString::Printf(TEXT(" %s(%s%s, at rest %d)=%d"), *E.Name, G.bMorph ? TEXT("morph") : TEXT("lift"), G.Main && !G.bMorph ? TEXT(" walls") : TEXT(""), E.Target < 0 ? 1 : 0, G.Updates);
			}
			G.Updates = 0;
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLMOVERS %s:%s"), *LevelName, Line.IsEmpty() ? TEXT(" none") : *Line);
	}
#endif
}

// (CPU1) the train's carriages are 60 sliding doors of 3 textures each, every one its own mesh: 180 draws in view.
// Doors at rest near each other are drawn as one mesh, a section per texture; each door keeps its own mesh, hidden
// while batched, for collision, hits and its motion. A door that moves leaves its batch at once (the batch is rebuilt
// without it, its own mesh shown) and goes back after resting 0.5 s, baked where it stopped. V1 culling: a door's
// mesh as before; a batch is shown while any door in it is shown. Morph doors only: lifts (rebuilt as they move) and
// doors with see-through textures (drawn in their own order) keep the old path. ol.MoverMerge 0 at level build: old.
static TAutoConsoleVariable<int32> CVarOLMoverMerge(TEXT("ol.MoverMerge"), 1,
	TEXT("Draw resting doors near each other as one mesh, a section per texture (1), or each door as its own mesh (0). Read when a level builds."));
static TAutoConsoleVariable<float> CVarOLMoverMergeCell(TEXT("ol.MoverMergeCell"), 3000.f,
	TEXT("Size (cm) of the plan cells whose resting doors share one batch mesh. Read when a level builds."));

void AOutlawsLevelActor::BuildMoverBatches()
{
	MoverBatches.Reset();
	if (!Inf || CVarOLMoverMerge.GetValueOnGameThread() == 0 || OLFlags::Param(TEXT("OLNoMoverMerge"))) return;
	const double Cell = FMath::Max(100.0, (double)CVarOLMoverMergeCell.GetValueOnGameThread());
	TMap<FIntPoint, TArray<int32>> Cells;
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		const FMoverGroup& G = Groups[g];
		if (!G.bMorph || !G.Main || G.Shape.Num() == 0) continue;
		bool bGlass = false;
		FBox B(ForceInit);
		for (const TPair<FString, FOutlawsMeshSection>& P : G.Shape)
		{
			bGlass |= P.Key.StartsWith(TEXT("~"));
			for (const FVector& V : P.Value.Positions) B += V;
		}
		if (bGlass || !B.IsValid) continue;
		const FVector C = G.Main->GetComponentTransform().TransformPosition(B.GetCenter());
		Cells.FindOrAdd(FIntPoint(FMath::FloorToInt(C.X / Cell), FMath::FloorToInt(C.Y / Cell))).Add(g);
	}
	int32 Batched = 0, Kept = 0;
	for (TPair<FIntPoint, TArray<int32>>& P : Cells)
	{
		// only where the merge at least halves the cell's sections (the train's carriages: many doors of the same few
		// textures); a few doors of their own textures (the mill's) keep their own meshes and culling
		TSet<FString> Keys;
		int32 Own = 0;
		for (const int32 g : P.Value)
		{
			for (const TPair<FString, FOutlawsMeshSection>& Sp : Groups[g].Shape)
			{
				Keys.Add(FString::Printf(TEXT("%s#%d#%lld"), *Sp.Key, Sp.Value.WaterGroup, Sp.Value.AtxWall));
				++Own;
			}
		}
		if (P.Value.Num() < 4 || Keys.Num() * 2 > Own) { Kept += P.Value.Num(); continue; }
		FMoverBatch& Bt = MoverBatches.AddDefaulted_GetRef();
		Bt.Mesh = NewMeshComponent(TEXT("OutlawsDoorBatch"), false);
		MoverMeshes.Add(Bt.Mesh);
		Bt.Members = P.Value;
		Bt.bDirty = true;
		for (const int32 g : P.Value)
		{
			FMoverGroup& G = Groups[g];
			G.Batch = MoverBatches.Num() - 1;
			G.bInBatch = Inf->Elevators[G.Elevator].Target < 0;
			G.BatchedValue = Inf->Elevators[G.Elevator].Current;
			G.RestSince = -1.0;
			Batched += G.bInBatch ? 1 : 0;
		}
	}
	TickMoverBatches();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT mover merge: %d doors in %d batches, %d doors kept on their own (cell %.0f cm)"), Batched, MoverBatches.Num(), Kept, Cell);
}

void AOutlawsLevelActor::TickMoverBatches()
{
	if (MoverBatches.Num() == 0 || !Inf) return;
	OL_PERF_SCOPE("Movers: batches");
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		FMoverGroup& G = Groups[g];
		if (!MoverBatches.IsValidIndex(G.Batch) || !Inf->Elevators.IsValidIndex(G.Elevator)) continue;
		const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
		const bool bStill = E.Target < 0;
		if (G.bInBatch)
		{
			if (bStill && E.Current == G.BatchedValue) continue;
			G.bInBatch = false;                            // moving: out of the batch, its own mesh drawn
			G.RestSince = -1.0;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT mover merge: door %d (elevator %d) moves, out of batch %d"), g, G.Elevator, G.Batch);
			MoverBatches[G.Batch].bDirty = true;
			ShowMoverGroup(g);
		}
		else if (!bStill || G.bDirty)
		{
			G.RestSince = -1.0;
		}
		else if (G.RestSince < 0.0)
		{
			G.RestSince = Now;
		}
		else if (Now - G.RestSince >= 0.5)
		{
			G.bInBatch = true;                             // still again: baked where it stopped
			G.BatchedValue = E.Current;
			MoverBatches[G.Batch].bDirty = true;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT mover merge: door %d (elevator %d) at rest at %.2f, back in batch %d"), g, G.Elevator, E.Current, G.Batch);
		}
	}
	bool bAny = false;
	for (FMoverBatch& Bt : MoverBatches)
	{
		if (!Bt.bDirty || !Bt.Mesh) continue;
		Bt.bDirty = false;
		bAny = true;
		TMap<FString, FOutlawsMeshSection> All;
		for (const int32 g : Bt.Members)
		{
			const FMoverGroup& G = Groups[g];
			if (!G.bInBatch) continue;
			const FTransform T = G.Main->GetComponentTransform();
			for (const TPair<FString, FOutlawsMeshSection>& P : G.Shape)
			{
				const FOutlawsMeshSection& S = P.Value;
				// (the material is chosen by texture, water and ATX wall: sections merge only when all three match)
				FOutlawsMeshSection& D = All.FindOrAdd(FString::Printf(TEXT("%s#%d#%lld"), *P.Key, S.WaterGroup, S.AtxWall));
				if (D.Positions.Num() == 0)
				{
					D.Texture = S.Texture;
					D.bMasked = S.bMasked;
					D.WaterGroup = S.WaterGroup;
					D.AtxWall = S.AtxWall;
				}
				const int32 Base = D.Positions.Num();
				for (const FVector& V : S.Positions) D.Positions.Add(T.TransformPosition(V));
				for (const FVector& N : S.Normals) D.Normals.Add(T.TransformVectorNoScale(N));
				D.UVs.Append(S.UVs);
				D.Colors.Append(S.Colors);
				for (const int32 I : S.Triangles) D.Triangles.Add(Base + I);
			}
		}
		Bt.Mesh->ClearAllMeshSections();
		int32 Tris = 0;
		AddSections(Bt.Mesh, All, false, WorldMaterial, WorldMaskedMaterial, Tris);
	}
	if (bAny) ShowMoverBatches(true);
}

void AOutlawsLevelActor::ShowMoverGroup(int32 g)
{
	if (!Groups.IsValidIndex(g)) return;
	const FMoverGroup& G = Groups[g];
	const bool bCulled = GroupCulled.IsValidIndex(g) && GroupCulled[g];
	const bool bByBatch = G.bInBatch && MoverBatches.IsValidIndex(G.Batch) && MoverBatches[G.Batch].bShown;
	if (G.Main) G.Main->SetHiddenInGame(bCulled || bByBatch);
	for (UProceduralMeshComponent* M : { G.Floor, G.Ceil }) if (M) M->SetHiddenInGame(bCulled);
}

void AOutlawsLevelActor::ShowMoverBatches(bool bForce)
{
	// a batch is drawn while half or more of its resting doors may be seen (V1); with fewer, each of those is drawn by its
	// own mesh as before (a mill door seen alone does not bring its neighbours' sections with it)
	for (int32 b = 0; b < MoverBatches.Num(); ++b)
	{
		FMoverBatch& Bt = MoverBatches[b];
		if (!Bt.Mesh) continue;
		int32 In = 0, Seen = 0;
		for (const int32 g : Bt.Members)
		{
			if (!Groups[g].bInBatch) continue;
			++In;
			Seen += GroupCulled.IsValidIndex(g) && GroupCulled[g] ? 0 : 1;
		}
		const bool bShow = Seen > 0 && Seen * 2 >= In;
		if (bShow == Bt.bShown && !bForce) continue;
		Bt.bShown = bShow;
		Bt.Mesh->SetHiddenInGame(!bShow);
		for (const int32 g : Bt.Members) ShowMoverGroup(g);
	}
}

void AOutlawsLevelActor::NotePlayerTeleported()
{
	bZoneHavePrev = false;
	// (BUG-WHARFPLANK) a teleport (a load, a respawn, the unstuck's rescue) onto an overlapping sector's floor: the movement
	// looks for a floor there at once (OnTeleported), before any tick has made the sector the body's, so the floor was still
	// passed through, no floor was found and the body fell; on a long frame past the floor's slack.  The overlap choice is
	// made from the place moved to first; the next tick decides the sector as ever (-OLOldOverlapTick: as before)
	static const bool bOldOverlapTick = OLFlags::Param(TEXT("OLOldOverlapTick"));
	if (bOldOverlapTick || !Inf || OverlapParts.Num() == 0) return;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch) return;
	const FVector Loc = Ch->GetActorLocation();
	const FVector2D Plan = FOutlawsGeometry::ToOutlawsPlan(Loc);
	const double Y = (Loc.Z - Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) / FOutlawsGeometry::UnitsToCm;
	const int32 At = Inf->Live.Floor3D(Plan.X, Y + 0.2, Plan.Y, PlayerSkip());
	if (At >= 0) ApplyOverlapChoice(At, false);
}

void AOutlawsLevelActor::BuildScrolls(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo)
{
	int32 Surfaces = 0;
	int32 WaterScrollSections = 0, TintedSections = 0;
	static const bool bOldScrollTint = OLFlags::Param(TEXT("OLOldScrollTint"));     // (-OLOldScrollTint: untinted as before, for shots)
	for (FScrollMesh& SM : ScrollMeshes)
	{
		if (SM.Filter.OnlyFloor.Num() + SM.Filter.OnlyCeil.Num() + SM.Filter.OnlyWalls.Num() == 0)
		{
			continue;
		}
		FOutlawsGeometry G;
		G.Build(L, TexInfo, bReverseWinding, &SM.Filter);
		UProceduralMeshComponent* Solid = NewMeshComponent(TEXT("OutlawsScrollSolid"), true);
		Solid->SetHiddenInGame(true);          // collides as the world does; the drawn copy below slides
		SM.Mesh = NewMeshComponent(TEXT("OutlawsScroll"), false);
		ObjectMeshes.Add(Solid);               // both go with the level (Clear)
		ObjectMeshes.Add(SM.Mesh);
		TArray<FString> Keys;
		G.World.GetKeys(Keys);
		Keys.Sort();
		TArray<int32> Collide;
		for (const FString& K : Keys)
		{
			const FOutlawsMeshSection& S = G.World[K];
			if (S.Triangles.Num() == 0) continue;
			const int32 j = SM.Sections.Num();
			Solid->CreateMeshSection_LinearColor(j, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			Collide.Add(j);
			SM.Mesh->CreateMeshSection_LinearColor(j, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			if (S.LightElev >= 0)
			{
				// (REQ-SECTORLIGHT) a lit sector's scrolling face (the piano's roll): drawn at its light, its UVs as they slide
				FLightFace& F = LightFaces.AddDefaulted_GetRef();
				F.Mesh = SM.Mesh;
				F.Section = j;
				F.Scroll = int32(&SM - ScrollMeshes.GetData());
				F.ScrollSection = j;
				F.Elev = S.LightElev;
				F.Off = S.LightOff;
				F.Wall = S.LightWall;
				F.Pos = S.Positions;
				F.Col = S.Colors;
				F.Shown = S.Colors.Num() > 0 ? S.Colors[0].R : -1.f;
				F.Box = FBox(S.Positions);
			}
			// under water: that water's tinted instance, as the static walls round it (BUG-SCROLLTINT: scrolling walls
			// and floors under water kept their own colours; the builder groups their faces as it does the world's)
			SM.Mesh->SetMaterial(j, MaterialFor(S.bMasked ? WorldMaskedMaterial.Get() : WorldMaterial.Get(), S.Texture, bOldScrollTint ? -1 : S.WaterGroup, S.AtxWall));
			TintedSections += !bOldScrollTint && S.WaterGroup >= 0;
#if WITH_OUTLAWS_TESTS
			if (S.WaterGroup >= 0 && S.Positions.Num() > 0)      // (where they are, for the pose shots)
			{
				const FBox Box(S.Positions);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT scroll under water: %s water %d, %s mesh, centre %s, size %s"), *K, S.WaterGroup,
					SM.bWall ? TEXT("wall") : TEXT("flat"), *Box.GetCenter().ToCompactString(), *Box.GetSize().ToCompactString());
			}
#endif
			const FOutlawsTexInfo* TI = TexInfo.Find(S.Texture);
			const double W = TI ? FMath::Max(1, TI->W) : 64.0, H = TI ? FMath::Max(1, TI->H) : 64.0;
			SM.UVPerUnit.Add(FVector2D(FOutlawsGeometry::TexelsPerUnit / W, FOutlawsGeometry::TexelsPerUnit / H));
			SM.Sections.Add(S);
		}
		CookCollisionOnce(Solid, Collide);
		// a scrolled floor that is a water surface (the mill's rivers: the dry sector's floor over the water below, its
		// current running): drawn with the water's material, no collision (user 2026-09-29: the outdoor water had no
		// surface; only the world sections were kept)
		TArray<FString> WaterKeys;
		G.Water.GetKeys(WaterKeys);
		WaterKeys.Sort();
		for (const FString& K : WaterKeys)
		{
			const FOutlawsMeshSection& S = G.Water[K];
			if (S.Triangles.Num() == 0) continue;
			const int32 j = SM.Sections.Num();
			SM.Mesh->CreateMeshSection_LinearColor(j, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			SM.Mesh->SetMaterial(j, MaterialFor(WaterMaterial.Get(), S.Texture, -1, S.AtxWall));
			const FOutlawsTexInfo* TI = TexInfo.Find(S.Texture);
			const double W = TI ? FMath::Max(1, TI->W) : 64.0, H = TI ? FMath::Max(1, TI->H) : 64.0;
			SM.UVPerUnit.Add(FVector2D(FOutlawsGeometry::TexelsPerUnit / W, FOutlawsGeometry::TexelsPerUnit / H));
			SM.Sections.Add(S);
			++WaterScrollSections;
		}
		Surfaces += SM.Filter.OnlyFloor.Num() + SM.Filter.OnlyCeil.Num() + SM.Filter.OnlyWalls.Num();
	}
	if (ScrollMeshes.Num() > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT scrolls: %d surfaces on %d meshes, %d water surface sections, %d under water (tinted)"), Surfaces, ScrollMeshes.Num(), WaterScrollSections, TintedSections);
	}
}

void AOutlawsLevelActor::TickScrolls()
{
	OL_PERF_SCOPE("Level: scrolls");
	// the offset slides the texture as the Jedi engine's texture offsets do: floors and ceilings take
	// it on both axes (u with x, v with z), walls along their length (u) and up (v, flipped for UE's
	// v-down), wrapped to one texture repeat
	// BUG-SCROLLSRGB (found 28 Oct): the colours (the sector's light) written as the build writes them, with no sRGB conversion:
	// UpdateMeshSection_LinearColor converts by default where CreateMeshSection_LinearColor does not, so every scrolling surface
	// (water, the mill's belts, the train's ground, the piano's roll) drew brighter than its sector after its first slide, the
	// still faces beside it not.  -OLOldScrollSRGB: converted as before, for shots
	static const bool bOldScrollSrgb = OLFlags::Param(TEXT("OLOldScrollSRGB"));
	for (FScrollMesh& SM : ScrollMeshes)
	{
		if (!SM.Mesh || !Inf->Scrolls.IsValidIndex(SM.Scroll))
		{
			continue;
		}
		const FVector2D O = Inf->Scrolls[SM.Scroll].Offset;
		for (int32 j = 0; j < SM.Sections.Num(); ++j)
		{
			const FVector2D K = SM.UVPerUnit[j];
			FVector2D D;
			if (SM.bWall)
			{
				D = FVector2D(FMath::Frac(O.X * K.X), FMath::Frac(-O.Y * K.Y));
			}
			else
			{
				// the world offset in the surface's texture frame, as the builder lays the texture (u runs along -x)
				const double Lx = FVector2D::DotProduct(O, SM.FrameA), Lz = FVector2D::DotProduct(O, SM.FrameN) * SM.Stretch;
				const double Rx = Lx * FMath::Cos(SM.Rad) - Lz * FMath::Sin(SM.Rad), Rz = Lx * FMath::Sin(SM.Rad) + Lz * FMath::Cos(SM.Rad);
				D = FVector2D(FMath::Frac(-Rx * K.X), FMath::Frac((SM.bCeil ? Rz : -Rz) * K.Y));
			}
			if (j == 0)
			{
				if (D.Equals(SM.Shown, 1e-5)) break;
				SM.Shown = D;
			}
			const FOutlawsMeshSection& S = SM.Sections[j];
			TArray<FVector2D> UVs = S.UVs;
			for (FVector2D& U : UVs) U += D;
			if (!OutlawsPerf::FreezeMeshUpdates()) SM.Mesh->UpdateMeshSection_LinearColor(j, S.Positions, S.Normals, UVs, S.Colors, TArray<FProcMeshTangent>(), bOldScrollSrgb);
		}
	}
}

void AOutlawsLevelActor::DebugScrollColours(int32& OutSections, int32& OutSlid, int32& OutVerts, int32& OutAsStill, int32& OutAsOld, int32& OutLit, FVector& OutAt, FVector& OutFacing,
	FString& OutWhere) const
{
	// (-OLScrollSrgbProbe) each scroll section's vertex colours as last given the GPU, against the build's colours unconverted (as
	// the still faces have them) and sRGB-converted (as TickScrolls wrote them before BUG-SCROLLSRGB); the largest section's middle
	OutSections = OutSlid = OutVerts = OutAsStill = OutAsOld = OutLit = 0;
	double Biggest = -1.0;
	for (const FScrollMesh& SM : ScrollMeshes)
	{
		UProceduralMeshComponent* M = SM.Mesh.Get();
		if (!M) continue;
		const bool bSlid = SM.Shown.X >= 0.0;
		for (int32 j = 0; j < SM.Sections.Num(); ++j)
		{
			const FOutlawsMeshSection& S = SM.Sections[j];
			const FProcMeshSection* P = M->GetProcMeshSection(j);
			if (!P || S.Positions.Num() == 0) continue;
			++OutSections;
			if (!bSlid) continue;
			++OutSlid;
			for (int32 k = 0; k < P->ProcVertexBuffer.Num() && k < S.Colors.Num(); ++k)
			{
				const FColor Got = P->ProcVertexBuffer[k].Color, Raw = S.Colors[k].ToFColor(false);
				++OutVerts;
				OutAsStill += Got == Raw ? 1 : 0;
				OutAsOld += Got == S.Colors[k].ToFColor(true) ? 1 : 0;
				OutLit += Raw.R > 0 && Raw.R < 255 ? 1 : 0;
			}
			const FBox B(S.Positions);
			const double Size = B.GetSize().Size();
			if (Size > Biggest)
			{
				Biggest = Size;
				OutAt = M->GetComponentTransform().TransformPosition(B.GetCenter());
				FVector N = FVector::ZeroVector;
				for (const FVector& Nv : S.Normals) N += Nv;
				N = N.GetSafeNormal();
				OutFacing = FMath::Abs(N.Z) > 0.7 || N.IsNearlyZero() ? FVector(1.0, 0.0, 0.0) : FVector(N.X, N.Y, 0.0).GetSafeNormal();
				OutWhere = FString::Printf(TEXT("%s at %s, %.0f cm across, %s"), *S.Texture, *OutAt.ToCompactString(), Size, FMath::Abs(N.Z) > 0.7 ? TEXT("a floor or ceiling") : TEXT("a wall"));
			}
		}
	}
}

// (REQ-SECTORLIGHT, cheaper) the game thread a frame the lit 3D models' redraws may take, nearest the eye first (the first
// always goes): one 3D rock of cliff's is about 5,500 vertices sent again, 0.4 ms on the PC editor build
static TAutoConsoleVariable<float> CVarOLSectorLightModelMs(TEXT("ol.SectorLightModelMs"), 0.2f,
	TEXT("Milliseconds a frame the sector lights' 3D model redraws may take, nearest first (the first always goes)"));

void AOutlawsLevelActor::FindLightThings()
{
	// (REQ-SECTORLIGHT) the sprites and 3D models the level put in a lit sector, once, after the level's things are made (not
	// the bad guys: they take the light where they stand at each step, LightAt); a model's colours as it was made
	bLightThingsFound = true;
	LightThings.Reset();
	for (const TObjectPtr<AActor>& A : SpawnedActors)
	{
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(A.Get());
		const FIntVector* G = S && !Cast<AOutlawsEnemy>(S->GetOwner()) ? FOutlawsGeometry::SectorLight.Find(S->HomeSector) : nullptr;
		if (!G) continue;
		FLightThing& T = LightThings.AddDefaulted_GetRef();
		T.Sprite = S;
		T.Elev = G->X;
		T.Off = G->Y;
		T.Built = S->LightLevel;
		T.Shown = S->LightLevel;
		T.ModelLv = S->LightLevel;
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(S->Model.Get());
		const int32 Own = M ? OutlawsInk::OwnSections(M) : 0;
		for (int32 i = 0; i < Own; ++i)
		{
			const FProcMeshSection* Ps = M->GetProcMeshSection(i);
			if (!Ps || Ps->ProcVertexBuffer.Num() == 0) continue;
			T.Sections.Add(i);
			TArray<FVector>& P = T.Pos.AddDefaulted_GetRef();
			TArray<FLinearColor>& C = T.Col.AddDefaulted_GetRef();
			P.Reserve(Ps->ProcVertexBuffer.Num());
			C.Reserve(Ps->ProcVertexBuffer.Num());
			for (const FProcMeshVertex& V : Ps->ProcVertexBuffer)
			{
				P.Add(V.Position);
				C.Add(FLinearColor(V.Color.R / 255.f, V.Color.G / 255.f, V.Color.B / 255.f, V.Color.A / 255.f));     // (as made: no sRGB)
			}
		}
	}
	int32 Models = 0, ModelVerts = 0;
	for (const FLightThing& T : LightThings)
	{
		Models += T.Sections.Num() > 0 ? 1 : 0;
		for (const TArray<FVector>& P : T.Pos) ModelVerts += P.Num();
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT sector lights: %d faces, %d sprites (%d of them 3D models, %d vertices) in lit sectors"), LightFaces.Num(), LightThings.Num(), Models, ModelVerts);
	// (cheaper) each light's faces and things together: a light whose drawn level holds is not looked at
	LightGroups.Reset();
	LightGroups.SetNum(Inf ? Inf->Elevators.Num() : 0);
	for (int32 i = 0; i < LightFaces.Num(); ++i) if (LightGroups.IsValidIndex(LightFaces[i].Elev)) LightGroups[LightFaces[i].Elev].Faces.Add(i);
	for (int32 i = 0; i < LightThings.Num(); ++i) if (LightGroups.IsValidIndex(LightThings[i].Elev)) LightGroups[LightThings[i].Elev].Things.Add(i);
}

void AOutlawsLevelActor::TickSectorLights()
{
	// (REQ-SECTORLIGHT) each CHANGE_LIGHT's sectors drawn at its present level when that changes: their faces' vertex
	// brightness as the builder makes it (LightLevel(ambient, the wall's light) / 31), the sprites' light, the models'
	// shading scaled from the light they were made at, and the parsed level's AMBIENT, which LightAt reads (the bad guys,
	// the held gear, the decals and drops made there)
	if (!Inf || !FOutlawsInfRuntime::SectorLightsOn() || FOutlawsGeometry::SectorLight.Num() == 0) return;
	OL_PERF_SCOPE("Level: sector lights");
	const double T0 = FPlatformTime::Seconds();
	if (!bLightThingsFound) FindLightThings();
	if (LevelData && LevelData->Sectors.Num() == Inf->Live.Sectors.Num())
	{
		for (const TPair<int32, FIntVector>& Kv : FOutlawsGeometry::SectorLight)
		{
			if (LevelData->Sectors.IsValidIndex(Kv.Key)) LevelData->Sectors[Kv.Key].Ambient = Inf->Live.Sectors[Kv.Key].Ambient;     // (same count: both valid)
		}
	}
	const bool bFreeze = OutlawsPerf::FreezeMeshUpdates();
	auto LevelOf = [this](int32 Elev, int32 Off, int32 Wall)
	{
		return FOutlawsLevel::LightLevel(FMath::FloorToInt(Inf->Elevators[Elev].Current) + Off, Wall);
	};
	// faces and models redrawn within 40 m of the eye only (a flicker of a level or two further off is not seen; the first
	// 1.2 ms a frame was shsimms's 25 lights redrawn everywhere): one come near is drawn at its present level at once.  A
	// sprite's light is a parameter, set wherever it stands
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const bool bEye = PC && PC->PlayerCameraManager;
	const FVector Eye = bEye ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	auto Near = [bEye, &Eye](const FBox& At) { return !bEye || At.ComputeSquaredDistanceToPoint(Eye) < FMath::Square(4000.0); };
	// (cheaper) a light is looked at only when its drawn level (the whole part) moved or one of its faces or models waits
	// to be drawn (it was out of range, or the meshes were frozen); -OLOldLightRedraw: every one every frame, as at first
	static const bool bOldRedraw = OLFlags::Param(TEXT("OLOldLightRedraw"));
	int32 Updated = 0;
	for (int32 e = 0; e < LightGroups.Num() && Inf->Elevators.IsValidIndex(e); ++e)
	{
		FLightGroup& G = LightGroups[e];
		if (G.Faces.Num() == 0 && G.Things.Num() == 0) continue;
		const int32 Whole = FMath::FloorToInt(Inf->Elevators[e].Current);
		if (!bOldRedraw && Whole == G.Lv && !G.bWaiting) continue;
		G.Lv = Whole;
		G.bWaiting = false;
		for (const int32 i : G.Faces)
		{
			FLightFace& F = LightFaces[i];
			UProceduralMeshComponent* M = F.Mesh.Get();
			if (!M) continue;
			const int32 Drawn = LevelOf(F.Elev, F.Off, F.Wall);
			const float B = float(Drawn) / 31.f;
			if (B == F.Shown) continue;
			if (bFreeze || !Near(F.Box))
			{
				G.bWaiting = true;
				continue;
			}
			F.Shown = B;
			++Updated;
			++DebugLightFaceUpdates;
			if (F.CopyLv.Num() > 0)
			{
				// a world part's copies (SplitWorldDraws): the one at this level shown, the one shown hidden
				int32 k = F.CopyLv.Find(Drawn);
				if (k == INDEX_NONE)
				{
					// a level none holds (a message sent the light off its stops): the copy after the shown one redrawn at it
					k = (F.ShownCopy + 1) % F.CopyLv.Num();
					F.CopyLv[k] = Drawn;
					for (FLinearColor& C : F.Col) C.R = C.G = C.B = B;
					M->UpdateMeshSection_LinearColor(F.Section0 + k, F.Pos, TArray<FVector>(), TArray<FVector2D>(), F.Col, TArray<FProcMeshTangent>(), false);
				}
				if (k != F.ShownCopy)
				{
					M->SetMeshSectionVisible(F.Section0 + k, true);
					if (F.ShownCopy >= 0) M->SetMeshSectionVisible(F.Section0 + F.ShownCopy, false);
				}
				F.ShownCopy = k;
				F.Section = F.Section0 + k;
				continue;
			}
			for (FLinearColor& C : F.Col) C.R = C.G = C.B = B;           // (alpha: the swirl's mark, kept)
			// (the vertex colours as made: no sRGB, as CreateMeshSection_LinearColor's default; UpdateMeshSection_LinearColor's is the other way)
			M->UpdateMeshSection_LinearColor(F.Section, F.Pos, TArray<FVector>(), TArray<FVector2D>(), F.Col, TArray<FProcMeshTangent>(), false);
			if (ScrollMeshes.IsValidIndex(F.Scroll) && ScrollMeshes[F.Scroll].Sections.IsValidIndex(F.ScrollSection)) ScrollMeshes[F.Scroll].Sections[F.ScrollSection].Colors = F.Col;
		}
		for (const int32 i : G.Things)
		{
			FLightThing& T = LightThings[i];
			AOutlawsSpriteActor* S = T.Sprite.Get();
			if (!S) continue;
			const int32 Lv = LevelOf(T.Elev, T.Off, 0);
			if (Lv != T.Shown)
			{
				T.Shown = Lv;
				S->SetLightLevel(Lv);
				++Updated;
			}
			UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(S->Model.Get());
			if (!M || T.Built <= 0 || Lv == T.ModelLv || T.Sections.Num() == 0) continue;
			// (cheaper: cliff's 51 lit models are 279,000 vertices, nearly all of the cost) a model's colours are sent again
			// only while it is drawn (seen in the last tenth of a second: not behind the eye or a wall, nor its sprite shown
			// instead); one that comes into view takes its present level a frame or two later
			if (bFreeze || !Near(FBox(S->GetActorLocation(), S->GetActorLocation())) || (!bOldRedraw && !M->WasRecentlyRendered(0.1f)))
			{
				G.bWaiting = true;
				continue;
			}
			if (!bOldRedraw)
			{
				LightModelsDue.Add(FIntVector(i, e, 0));
				continue;
			}
			const double M0 = FPlatformTime::Seconds();
			T.ModelLv = Lv;
			const float K = float(Lv) / float(T.Built);
			for (int32 k = 0; k < T.Sections.Num(); ++k)
			{
				TArray<FLinearColor> C = T.Col[k];
				for (FLinearColor& V : C)
				{
					V.R = FMath::Min(1.f, V.R * K);
					V.G = FMath::Min(1.f, V.G * K);
					V.B = FMath::Min(1.f, V.B * K);
				}
				M->UpdateMeshSection_LinearColor(T.Sections[k], T.Pos[k], TArray<FVector>(), TArray<FVector2D>(), C, TArray<FProcMeshTangent>(), false);
			}
			++Updated;
			++DebugLightModelUpdates;
			DebugLightModelSeconds += FPlatformTime::Seconds() - M0;
		}
	}
	// (cheaper) the drawn models due a new light, nearest the eye first, within ol.SectorLightModelMs of the game thread a
	// frame (the first always): one left over waits for the next frame.  Each is sent its colours at that light, made once
	// (no sRGB, as made) and kept for its next return there
	LightModelsDue.Sort([this, &Eye](const FIntVector& X, const FIntVector& Y)
	{
		const AOutlawsSpriteActor* Sx = LightThings[X.X].Sprite.Get();
		const AOutlawsSpriteActor* Sy = LightThings[Y.X].Sprite.Get();
		return FVector::DistSquared(Sx->GetActorLocation(), Eye) < FVector::DistSquared(Sy->GetActorLocation(), Eye);
	});
	const double Budget = double(CVarOLSectorLightModelMs.GetValueOnGameThread()) / 1000.0;
	double Spent = 0.0;
	for (int32 n = 0; n < LightModelsDue.Num(); ++n)
	{
		if (n > 0 && Spent >= Budget)
		{
			LightGroups[LightModelsDue[n].Y].bWaiting = true;
			continue;
		}
		const double M0 = FPlatformTime::Seconds();
		FLightThing& T = LightThings[LightModelsDue[n].X];
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(T.Sprite->Model.Get());
		const int32 Lv = LevelOf(T.Elev, T.Off, 0);
		T.ModelLv = Lv;
		TArray<TArray<FColor>>* At = T.ColAt.Find(Lv);
		if (!At)
		{
			const float K = float(Lv) / float(T.Built);
			At = &T.ColAt.Add(Lv);
			At->SetNum(T.Sections.Num());
			for (int32 k = 0; k < T.Sections.Num(); ++k)
			{
				const TArray<FLinearColor>& Src = T.Col[k];
				TArray<FColor>& Dst = (*At)[k];
				Dst.SetNumUninitialized(Src.Num());
				for (int32 v = 0; v < Src.Num(); ++v)
				{
					Dst[v] = FLinearColor(FMath::Min(1.f, Src[v].R * K), FMath::Min(1.f, Src[v].G * K), FMath::Min(1.f, Src[v].B * K), Src[v].A).ToFColor(false);
				}
			}
		}
		for (int32 k = 0; k < T.Sections.Num(); ++k)
		{
			M->UpdateMeshSection(T.Sections[k], T.Pos[k], TArray<FVector>(), TArray<FVector2D>(), TArray<FVector2D>(), TArray<FVector2D>(), TArray<FVector2D>(), (*At)[k], TArray<FProcMeshTangent>());
		}
		++Updated;
		++DebugLightModelUpdates;
		const double Took = FPlatformTime::Seconds() - M0;
		Spent += Took;
		DebugLightModelSeconds += Took;
	}
	LightModelsDue.Reset();
	const double Frame = FPlatformTime::Seconds() - T0;
	DebugLightSeconds += Frame;
	DebugLightMaxFrame = FMath::Max(DebugLightMaxFrame, Frame);
	if (Updated > 0) { DebugLightUpdateFrameSeconds += Frame; ++DebugLightUpdateFrames; }
	DebugLightUpdates += Updated;
	++DebugLightTicks;
}

AOutlawsLevelActor::FLightStats AOutlawsLevelActor::DebugLight(int32 Elevator) const
{
	FLightStats St;
	if (!Inf || !Inf->Elevators.IsValidIndex(Elevator)) return St;
	St.Shown = FMath::FloorToInt(Inf->Elevators[Elevator].Current);
	// the face, sprite and model nearest the eye (those further than 40 m wait till the eye comes near)
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	double BestFace = 1e30, BestSprite = 1e30, BestModel = 1e30;
	for (const FLightFace& F : LightFaces)
	{
		if (F.Elev != Elevator) continue;
		++St.Faces;
		const double D = F.Box.ComputeSquaredDistanceToPoint(Eye);
		if (D >= BestFace || !F.Mesh.IsValid()) continue;
		BestFace = D;
		// its drawn brightness, and what its sector's light and its wall's make it
		const FProcMeshSection* Ps = F.Mesh->GetProcMeshSection(F.Section);
		St.FaceBright = Ps && Ps->ProcVertexBuffer.Num() > 0 && Ps->bSectionVisible ? Ps->ProcVertexBuffer[0].Color.R / 255.f : -1.f;     // (a copy hidden: not drawn)
		// (cheaper) one copy of the face shown, no other
		for (int32 k = 0; k < F.CopyLv.Num(); ++k)
		{
			const FProcMeshSection* Cs = F.Mesh->GetProcMeshSection(F.Section0 + k);
			if (Cs && Cs->bSectionVisible != (F.Section0 + k == F.Section)) St.FaceBright = -2.f;
		}
		St.FaceWant = float(FOutlawsLevel::LightLevel(St.Shown + F.Off, F.Wall)) / 31.f;
	}
	for (const FLightThing& T : LightThings)
	{
		if (T.Elev != Elevator || !T.Sprite.IsValid()) continue;
		++St.Sprites;
		const int32 Want = FOutlawsLevel::LightLevel(St.Shown + T.Off);
		const double D = FVector::DistSquared(T.Sprite->GetActorLocation(), Eye);
		if (D < BestSprite && !T.Sprite->bFullBright) { BestSprite = D; St.SpriteLight = T.Sprite->LightLevel; St.SpriteWant = Want; }
		if (T.Sections.Num() == 0) continue;
		++St.Models;
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(T.Sprite->Model.Get());
		const FProcMeshSection* Ps = M ? M->GetProcMeshSection(T.Sections[0]) : nullptr;
		// (a model drawn: one out of view takes its light when it comes back into view)
		if (D < BestModel && Ps && Ps->ProcVertexBuffer.Num() > 0 && T.Col.Num() > 0 && T.Col[0].Num() > 0 && T.Built > 0 && M->WasRecentlyRendered(0.1f))
		{
			BestModel = D;
			St.ModelShade = Ps->ProcVertexBuffer[0].Color.R / 255.f;                                    // (drawn now)
			St.ModelWant = FMath::Min(1.f, T.Col[0][0].R * float(Want) / float(T.Built));              // (as made, scaled)
		}
	}
	St.bParsedSame = LevelData.IsValid();
	for (const int32 s : Inf->Elevators[Elevator].Sectors)
	{
		St.bParsedSame &= LevelData.IsValid() && LevelData->Sectors.IsValidIndex(s) && LevelData->Sectors[s].Ambient == Inf->Live.Sectors[s].Ambient;
	}
	return St;
}

FVector AOutlawsLevelActor::SteerCurrent(ACharacter* Body, const FVector& Push)
{
	// the mill's inlets: gates shot shut across a channel whose current still runs into them pinned the swimmer there,
	// as in the original (user 2026-09-29).  The water looks 1.5 m ahead along its flow; a closed gate (anything solid)
	// there and it turns, 15 degrees at a time either way up to 90, toward the nearest open way, as water runs round an
	// obstacle, carrying the body down the open branch; no way at all and it stops pushing.  And a swimmer going
	// against it always gains: the current is held to 60% of the swimmer's own speed along their way
	UWorld* World = GetWorld();
	UCharacterMovementComponent* BM = Body->GetCharacterMovement();
	const float Dt = FMath::Max(1e-4f, World->GetDeltaSeconds());
	FVector Flow = Push / Dt;                                        // cm/s
	const float Speed = float(Flow.Size());
	FVector Dir = Flow / Speed;
	const FVector Flat = FVector(Dir.X, Dir.Y, 0.0);
	if (Flat.Size() > 0.5)                                           // (a mostly level flow; updrafts go as they are)
	{
		UCapsuleComponent* Cap = Body->GetCapsuleComponent();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCurrent), false, Body);
		IgnoreHiddenOverlaps(Q);
		const FCollisionResponseParams RP(Cap->GetCollisionResponseToChannels());
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius() * 0.9f, Cap->GetScaledCapsuleHalfHeight() * 0.8f);
		const FVector From = Body->GetActorLocation();
		auto Open = [&](const FVector& D) { return !World->SweepTestByChannel(From, From + D * 150.0, FQuat::Identity, Cap->GetCollisionObjectType(), Shape, Q, RP); };
		FVector Want = FVector::ZeroVector;
		if (Open(Dir))
		{
			Want = Dir;
		}
		else
		{
			for (int32 k = 1; k <= 6 && Want.IsZero(); ++k)
			{
				for (const float Sgn : { 1.f, -1.f })
				{
					const FVector D = Dir.RotateAngleAxis(Sgn * 15.f * k, FVector::UpVector);
					if (Open(D)) { Want = D; break; }
				}
			}
		}
		// eased, so the turn does not snap from frame to frame
		if (FlowSteer.IsZero()) FlowSteer = Want;
		FlowSteer = FMath::Lerp(FlowSteer, Want, FMath::Min(1.f, 6.f * Dt));
		Dir = FlowSteer.IsNearlyZero() ? FVector::ZeroVector : FlowSteer.GetSafeNormal();
		if (Want.IsZero()) Dir = FVector::ZeroVector;
	}
	float Along = Speed;
	const FVector Own = BM->Velocity;                                 // the body's own swim (the current moves it apart)
	const float OwnSpeed = float(Own.Size());
	if (OwnSpeed > 30.f && !Dir.IsZero() && FVector::DotProduct(Own / OwnSpeed, Dir) < -0.3f)
	{
		Along = FMath::Min(Along, 0.6f * OwnSpeed);
	}
	return Dir * Along * Dt;
}

void AOutlawsLevelActor::TrackPlayer()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* P = PC ? PC->GetPawn() : nullptr;
	if (!P || !Inf) return;
	const FVector Loc = P->GetActorLocation();
	float Half = 90.f;
	if (const ACharacter* Ch = Cast<ACharacter>(P))
	{
		Half = Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	}
	const FVector2D LocPlan = FOutlawsGeometry::ToOutlawsPlan(Loc);
	const double X = LocPlan.X, Z = LocPlan.Y;
	const double Y = (Loc.Z - Half) / FOutlawsGeometry::UnitsToCm;
	// (V1) the body's sector is tracked state, as the Jedi engine keeps it: the move since the last tick crosses walls
	// from the sector it was in (sector_addObject on the wall-cross list); only a move that does not explain where the
	// body is (a lift, a mantle, a teleport, the level's start) picks again, by the feet (sector_which3D)
	// (BUG-MILLCLIMB) on a level with a zone gate, a zone gate's far side is left out (PlayerSkip), and a jump (the level's
	// start, a teleport, a load: over 2.4 m in one tick) decides the side again from where the body stands
	// (BUG-JUMPPICK) and so on every level: a load, a respawn, the level's start or a teleport (the unstuck's too) onto a
	// catwalk, pier or tunnel floor running through a bigger room's air took the smaller sector (miner's -67 and -80
	// catwalks over the -101 cavern, the wharf's pier over its pilings), so the floor stood on was passed through and the
	// room below drawn.  A jump, and any move the tracking cannot follow (a mantle, a lift), now picks the walled sector
	// whose floor is nearest under the feet (Floor3D).  -OLOldJumppick: the smallest sector, gated levels' jumps only
	static const bool bOldJumppick = OLFlags::Param(TEXT("OLOldJumppick"));
	const FVector BodyNow(Loc.X, Loc.Y, Loc.Z - Half);
	const bool bJump = (!bOldJumppick || ZoneGates.Num() > 0) && (PlayerSector < 0 || !bZoneHavePrev || FVector::Dist(BodyNow, ZonePrevBody) > 240.0);
	ZonePrevBody = BodyNow;
	bZoneHavePrev = true;
	int32 Now = PlayerSector >= 0 && bHavePlayerPrev && !bJump ? Inf->Live.TrackMove(PlayerSector, PlayerPrev, FVector2D(X, Z), Y + 0.2, PlayerSkip()) : -2;
	const bool bByCrossing = Now != -2;
	if (Now == -2) Now = bJump ? ZoneRedecide(X, Y + 0.2, Z, PlayerSector < 0 ? TEXT("the level's start") : TEXT("a jump"))
		: bOldJumppick ? Inf->Live.Which3D(X, Y + 0.2, Z, PlayerSkip()) : Inf->Live.Floor3D(X, Y + 0.2, Z, PlayerSkip());
	static const bool bOldMillvis = OLFlags::Param(TEXT("OLOldMillvis"));
	if (!bOldMillvis && Inf->Live.Sectors.IsValidIndex(Now) && Inf->Live.Sectors[Now].NoWalls())
	{
		// (BUG-MILLVIS) never the sky backdrop wrapped round the level while a room with walls holds the feet: the mill's 607
		// (20 to 47.5 over the whole level) took the body on a mantle and on a crossing out of a hall over the flumes, and
		// kept it, so the eye's flood ran from the open sky and the halls round the player went undrawn.  The smallest
		// walled sector holding the feet, as sector_which3D picks
		const FVector2D Pt(X, Z);
		double BestArea = TNumericLimits<double>::Max();
		int32 Walled = -1;
		const TBitArray<>* Skip = PlayerSkip();
		for (const FOutlawsSector& Ws : Inf->Live.Sectors)
		{
			if (Ws.NoWalls() || Ws.Verts.Num() < 3 || FOutlawsLevel::Skipped(Skip, Ws.Index) || !Inf->Live.ContainsXZ(Ws, X, Z)) continue;
			const double F = Inf->Live.SurfaceY(Ws, Pt, true), C = Inf->Live.SurfaceY(Ws, Pt, false);
			if (Y + 0.2 < F - 0.5 || Y + 0.2 > C + 0.5) continue;
			double A2 = 0.0;
			for (int32 i = 0; i < Ws.Verts.Num(); ++i) { const FVector2D& P0 = Ws.Verts[i]; const FVector2D& Q0 = Ws.Verts[(i + 1) % Ws.Verts.Num()]; A2 += P0.X * Q0.Y - Q0.X * P0.Y; }
			if (FMath::Abs(A2) < BestArea) { BestArea = FMath::Abs(A2); Walled = Ws.Index; }
		}
		if (Walled >= 0) Now = Walled;
	}
	if (!bJump && ZoneGates.Num() > 0) ZoneAfterMove(PlayerSector, Now, bByCrossing);
	const FOutlawsSector* S = Inf->Live.Sectors.IsValidIndex(Now) ? &Inf->Live.Sectors[Now] : nullptr;
	if (Now != PlayerSector)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT membership: player %d -> %d (%s)%s"), PlayerSector, Now, bByCrossing ? TEXT("by crossing") : bJump ? TEXT("by which3D, a jump") : TEXT("by which3D"),
			S && !S->Name.IsEmpty() ? *(TEXT(" ") + S->Name) : TEXT(""));
	}
	UpdateOverlaps(Now);
	UpdateVisibility();                 // the eye's portal floods: culling, and the overlap choice below reads them
	// (BUG-WHARFPLANK) from the sector just picked: the old one left the floor of a sector moved into (a teleport, a load,
	// a walk across) passed through for that tick and the next, a long frame's fall going past it (-OLOldOverlapTick: as before)
	static const bool bOldOverlapTick = OLFlags::Param(TEXT("OLOldOverlapTick"));
	ApplyOverlapChoice(bOldOverlapTick ? PlayerSector : Now, false);
	// (behind the main menu: what is seen from here, no more; no push, no hurt, no sector or line events.  The sector
	// itself is kept: the choice between rooms sharing a place reads it)
	if (bFrontEnd)
	{
		PlayerSector = Now;
		PlayerPrev = FVector2D(X, Z);
		bHavePlayerPrev = true;
		return;
	}
	// a sector's VELOCITY moves the player the Jedi way, added to the position each tick: a VELFLOORONLY sector
	// (the train's ground) only while standing on its floor and only across; any other anywhere inside it,
	// up and down too (canyon's underwater updraft, its wind, its river currents) (user 2026-09-27)
	const bool bTrainLevel = LevelName.Equals(TEXT("train"), ESearchCase::IgnoreCase);
	FVector TrainTarget = FVector::ZeroVector;           // (the train's moving floors: their own carry, below)
	if (S && !bDead && !S->Velocity.IsNearlyZero())
	{
		const bool bFloorOnly = (S->Flags & OutlawsFlags::VelFloorOnly) != 0;
		if (bTrainLevel && bFloorOnly && Y - S->Floor.Y < 0.6)
		{
			TrainTarget = FOutlawsGeometry::ToUE(S->Velocity.X, 0.0, S->Velocity.Z);
		}
		else if (!bFloorOnly || Y - S->Floor.Y < 0.6)
		{
			// at the player's pace, as the step is (user 2026-10-02: at the lower movement speeds the currents and moving
			// floors could not be walked against): the push and the walk keep the original's balance at every speed
			FVector Push = FOutlawsGeometry::ToUE(S->Velocity.X, bFloorOnly ? 0.0 : S->Velocity.Y, S->Velocity.Z) * (World->GetDeltaSeconds() * Pace());
			// (BUG-SURFACEJOLT) a body held at the surface is carried along it: the sector its feet are in (deep under the
			// head, a pool's lower sector, a falls' downdraft) no longer pulls it under the surface; a diver still feels it
			const AOutlawsPlayer* Swimmer = Cast<AOutlawsPlayer>(P);
			if (Swimmer && Swimmer->IsSurfaceHeld() && Push.Z < 0.0)
			{
				if (!bCurrentHeldLogged) UE_LOG(LogOutlaws, Log, TEXT("OLRT swim: sector %d's current pulls down %.0f cm/s: held at the surface, carried along it only"), S->Index, -Push.Z / FMath::Max(1e-4f, World->GetDeltaSeconds()));
				bCurrentHeldLogged = true;
				Push.Z = 0.0;
			}
			else
			{
				bCurrentHeldLogged = false;
			}
			// pressed into a wall it slides along it, the way the push and the wall leave open (user 2026-09-29: a
			// current against a wall snagged the body there)
			ACharacter* Body = Cast<ACharacter>(P);
			UCharacterMovementComponent* BM = Body ? Body->GetCharacterMovement() : nullptr;
			if (BM && (S->Flags & OutlawsFlags::Water) && !Push.IsNearlyZero())
			{
				Push = SteerCurrent(Body, Push);
			}
			if (BM)
			{
				FHitResult Hit;
				BM->SafeMoveUpdatedComponent(Push, P->GetActorQuat(), true, Hit);
				if (Hit.IsValidBlockingHit())
				{
					static_cast<UMovementComponent*>(BM)->SlideAlongSurface(Push, 1.f - Hit.Time, Hit.Normal, Hit, true);   // (public on the base; the character's own runs)
				}
			}
			else
			{
				P->AddActorWorldOffset(Push, true);
			}
			// an updraft (the canyon's wind tunnel: 8 up as it blows you 93 along) holds the player up: no fall builds
			// against it, so its drift lifts them slowly and they can never drop into the tunnel (user 2026-09-28; gravity
			// out-grew the small lift and they sank)
			// (air only: a water sector's upwelling (the hideout's) is the swim's to fight, else it floats the diver out)
			if (!bFloorOnly && S->Velocity.Y > 0.0 && !(S->Flags & OutlawsFlags::Water))
			{
				if (ACharacter* Ch = Cast<ACharacter>(P))
				{
					if (UCharacterMovementComponent* M = Ch->GetCharacterMovement(); M && M->Velocity.Z < 0.0)
					{
						M->Velocity.Z = 0.0;
					}
				}
			}
		}
	}
	// (user 2026-10-05, BUG-TRAINPLAYER) the train's moving floors carry the player at their full speed, as they do the bad guys
	// and bodies (not at the player's pace: that is for the currents, to be walked against). Eased in and out at a full
	// carry's worth per 0.3 s, so stepping on or off is never a jolt of 19 m/s in the headset; never carried onto a deadly
	// floor (walking onto one still kills, below); a wall in the way takes its share of the carry off
	if (bTrainLevel && !bDead)
	{
		const float Dt = World->GetDeltaSeconds();
		const float EaseRate = float(FMath::Max(TrainTarget.Size(), PlayerCarryVel.Size())) / 0.3f;
		PlayerCarryVel = FMath::VInterpConstantTo(PlayerCarryVel, TrainTarget, Dt, EaseRate);
		ACharacter* Body = Cast<ACharacter>(P);
		UCharacterMovementComponent* BM = Body ? Body->GetCharacterMovement() : nullptr;
		if (BM && !PlayerCarryVel.IsNearlyZero() && Dt > 0.f)
		{
			const FVector Push = PlayerCarryVel * Dt;
			const FVector To = Loc + Push;
			using namespace OutlawsFlags;
			auto Deadly = [Y](const FOutlawsSector* Sec)
			{
				return Sec && ((Sec->Flags & SecDamageDie) || ((Sec->Flags & FloorDamageDie) && Y - Sec->Floor.Y < 0.6));
			};
			const FVector2D ToPlan = FOutlawsGeometry::ToOutlawsPlan(To);
			const FOutlawsSector* There = Inf->Live.SectorAt(ToPlan.X, Y + 0.2, ToPlan.Y);
			if (!Deadly(S) && Deadly(There))
			{
				PlayerCarryVel = FVector::ZeroVector;
				static int32 Held = 0;
				if (Held++ < 3) UE_LOG(LogOutlaws, Log, TEXT("OLRT the moving floor would carry the player onto a deadly one (sector %d): held back"), There->Index);
			}
			else
			{
				FHitResult Hit;
				BM->SafeMoveUpdatedComponent(Push, P->GetActorQuat(), true, Hit);
				if (Hit.IsValidBlockingHit())
				{
					static_cast<UMovementComponent*>(BM)->SlideAlongSurface(Push, 1.f - Hit.Time, Hit.Normal, Hit, true);
					const FVector N = FVector(Hit.Normal.X, Hit.Normal.Y, 0.0).GetSafeNormal();
					if (!N.IsNearlyZero() && FVector::DotProduct(PlayerCarryVel, N) < 0.0) PlayerCarryVel -= N * FVector::DotProduct(PlayerCarryVel, N);
				}
			}
		}
	}
	else
	{
		PlayerCarryVel = FVector::ZeroVector;
	}
	// damaging sectors: SEC_ anywhere in it, FLR_ only standing on its floor; dealt a quarter point at a time
	if (S && !bDead)
	{
		using namespace OutlawsFlags;
		const uint32 F = S->Flags;
		const bool bOnFloor = Y - S->Floor.Y < 0.6;
		const bool bDie = (F & SecDamageDie) || (bOnFloor && (F & FloorDamageDie));
		const float Rate = ((F & SecDamageLarge) || (bOnFloor && (F & FloorDamageLarge))) ? 1.f
			: ((F & SecDamageSmall) || (bOnFloor && (F & FloorDamageSmall))) ? 0.5f : 0.f;
		if ((bDie || Rate > 0.f) && HurtSector != Now)
		{
			HurtSector = Now;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT damaging sector %d flags 0x%x: %s"), Now, F, bDie ? TEXT("deadly") : Rate >= 1.f ? TEXT("1 a second") : TEXT("0.5 a second"));
		}
		// the player cries out while a damaging floor or sector hurts: shriek1.wav alone, at most every 1.2 s, and
		// no pain grunt over it (user 2026-09-28)
		auto Shriek = [&]()
		{
			const double Now = FPlatformTime::Seconds();
			if (Now - LastShriek < 1.2) return;
			LastShriek = Now;
			PlayWav(TEXT("shriek1"), Loc, false, true);
		};
		if (bDie)
		{
			// a deadly floor or sector: death at once, and no cry over it (the death cry follows; user 2026-09-27)
			DamagePlayer(1e6f, 1.f, true);
		}
		else if (Rate > 0.f)
		{
			FloorHurt += Rate * World->GetDeltaSeconds();
			if (FloorHurt >= 0.25f)
			{
				Shriek();
				bQuietHurt = true;
				DamagePlayer(FloorHurt, 1.f, true);
				bQuietHurt = false;
				FloorHurt = 0.f;
			}
		}
	}
	if (Now != PlayerSector)
	{
		if (PlayerSector >= 0) Inf->SectorEvent(PlayerSector, OutlawsInf::EvLeave);
		if (Now >= 0) Inf->SectorEvent(Now, OutlawsInf::EvEnter);
		if (S && (S->Flags & OutlawsFlags::SecSecretTag)) UOutlawsStats::NoteSecret(this, Now);      // (REQ-STATS)
		PlayerSector = Now;
	}
	const FVector2D Cur(X, Z);
	if (bHavePlayerPrev && CrossWalls.Num() > 0 && FVector2D::DistSquared(Cur, PlayerPrev) > 1e-10)
	{
		for (uint64 K : CrossWalls)
		{
			const int32 s = int32(K >> 32), w = int32(K & 0xffffffffull);
			const FOutlawsSector& Sec = Inf->Live.Sectors[s];
			const FOutlawsWall& W = Sec.Walls[w];
			const FVector2D A = Sec.Verts[W.V1], B = Sec.Verts[W.V2];
			if (SegmentsCross(PlayerPrev, Cur, A, B))
			{
				const FVector2D Ed = B - A;
				const FVector2D In = Sec.IsClockwise() ? FVector2D(-Ed.Y, Ed.X) : FVector2D(Ed.Y, -Ed.X);
				const bool bFromFront = FVector2D::DotProduct(PlayerPrev - A, In) > 0.0;
				Inf->WallEvent(s, w, bFromFront ? OutlawsInf::EvCrossFront : OutlawsInf::EvCrossBack);
			}
		}
	}
	PlayerPrev = Cur;
	bHavePlayerPrev = true;
}

USceneComponent* AOutlawsLevelActor::RideFor(int32 Sector, double ObjY, bool bHang, double& OutDzCm) const
{
	OutDzCm = 0.0;
	if (!Inf)
	{
		return nullptr;
	}
	for (const FMoverGroup& G : Groups)
	{
		const FOutlawsElevator& E = Inf->Elevators[G.Elevator];
		const int32 k = E.Sectors.IndexOfByKey(Sector);
		if (k == INDEX_NONE)
		{
			continue;
		}
		if (G.bMorph)
		{
			// MORPH_*2 carries what stands in it (Jedi INF_EFLAG_MOVE_FLOOR)
			if (G.Main && Inf->Inf.Items[E.Item].Classes[E.ClassIndex].bMoveObjects) return G.Main;
			continue;
		}
		const bool bFloorMoves = E.Class == EOutlawsInfClass::MoveFloor || E.Class == EOutlawsInfClass::MoveFC;
		const bool bCeilMoves = E.Class == EOutlawsInfClass::MoveCeiling || E.Class == EOutlawsInfClass::MoveFC;
		if (!bHang && bFloorMoves && G.Floor && G.FloorFilter.OnlyFloor.Contains(Sector) && FMath::Abs(ObjY - E.BaseFloor[k]) < 0.5)
		{
			OutDzCm = (G.BuiltValue - E.BaseFloor[0]) * FOutlawsGeometry::UnitsToCm;
			return G.Floor;
		}
		if (bHang && bCeilMoves && G.Ceil && G.CeilFilter.OnlyCeil.Contains(Sector))
		{
			const double Base = E.Class == EOutlawsInfClass::MoveCeiling ? E.BaseCeil[0] : E.BaseFloor[0];
			OutDzCm = (G.BuiltValue - Base) * FOutlawsGeometry::UnitsToCm;
			return G.Ceil;
		}
	}
	return nullptr;
}

namespace
{
	/** P (Outlaws x, z) inside the sector's floor plan: crossings of a ray along +x over its walls (holes and all). */
	bool InsideFloorPlan(const FOutlawsSector& S, double X, double Z)
	{
		return FOutlawsLevel::InsideWalls(S, X, Z);
	}
}

bool AOutlawsLevelActor::DebugNudgeWall(const FString& Name, const FString& WallId)
{
	if (!Inf) return false;
	TArray<int32> Secs;
	Inf->FindSectors(Name, FString(), Secs);
	bool bTaken = false;
	for (int32 s : Secs)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[s];
		bool bFound = false;
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			if (S.Walls[w].Id.Equals(WallId, ESearchCase::IgnoreCase)) { bTaken |= Inf->WallEvent(s, w, OutlawsInf::EvNudgeFront); bFound = true; }
		}
		if (!bFound && WallId.IsNumeric() && S.Walls.IsValidIndex(FCString::Atoi(*WallId))) bTaken |= Inf->WallEvent(s, FCString::Atoi(*WallId), OutlawsInf::EvNudgeFront);   // a bare index
	}
	return bTaken;
}

bool AOutlawsLevelActor::DebugWallSpot(const FString& Name, const FString& WallId, FVector& Out) const
{
	if (!Inf) return false;
	TArray<int32> Secs;
	Inf->FindSectors(Name, FString(), Secs);
	for (int32 s : Secs)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[s];
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			if (S.Walls[w].Id.Equals(WallId, ESearchCase::IgnoreCase)) { Out = WallMidpoint(s, w); return true; }
		}
	}
	return false;
}

FString AOutlawsLevelActor::DebugElevatorReport(const FString& Prefix) const
{
	TArray<FString> Parts;
	if (!Inf) return TEXT("(no INF)");
	for (const FOutlawsElevator& E : Inf->Elevators)
	{
		if (!E.Name.StartsWith(Prefix, ESearchCase::IgnoreCase)) continue;
		FString Line = FString::Printf(TEXT("%s[class %d] stop %d target %d cur %.2f master %d term %d wait %d;"), *E.Name, int32(E.Class), E.Stop, E.Target, E.Current, E.bMasterOn ? 1 : 0, E.bTerminated ? 1 : 0, E.bWaiting ? 1 : 0);
		for (int32 s : E.Sectors)
		{
			if (Inf->Live.Sectors.IsValidIndex(s)) Line += FString::Printf(TEXT(" s%d floor %.2f ceil %.2f"), s, Inf->Live.Sectors[s].Floor.Y, Inf->Live.Sectors[s].Ceil.Y);
		}
		Parts.Add(Line);
	}
	return Parts.Num() ? FString::Join(Parts, TEXT(" || ")) : TEXT("(none)");
}

bool AOutlawsLevelActor::DebugWallSlides(int32 Sector, int32 Wall) const
{
	return Inf.IsValid() && Inf->WallSlides(Sector, Wall);
}

FString AOutlawsLevelActor::DebugSectorVerts(const FString& Name, int32 Count) const
{
	if (!Inf) return TEXT("(no INF)");
	TArray<int32> Secs;
	Inf->FindSectors(Name, FString(), Secs);
	FString Out;
	for (int32 s : Secs)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[s];
		for (int32 v = 0; v < FMath::Min(Count, S.Verts.Num()); ++v) Out += FString::Printf(TEXT("s%d v%d (%.2f, %.2f) "), s, v, S.Verts[v].X, S.Verts[v].Y);
	}
	return Out.IsEmpty() ? TEXT("(none)") : Out;
}

bool AOutlawsLevelActor::DebugSectorSpot(const FString& NameOrIndex, FVector& OutFloorPoint) const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return false;
	int32 Index = -1;
	if (NameOrIndex.IsNumeric())
	{
		Index = FCString::Atoi(*NameOrIndex);
	}
	else if (Inf)
	{
		TArray<int32> Secs;
		Inf->FindSectors(NameOrIndex, FString(), Secs);
		if (Secs.Num()) Index = Secs[0];
	}
	if (!Lv->Sectors.IsValidIndex(Index) || Lv->Sectors[Index].Verts.Num() == 0) return false;
	const FOutlawsSector& S = Lv->Sectors[Index];
	FVector2D C = FVector2D::ZeroVector;
	for (const FVector2D& V : S.Verts) C += V;
	C /= double(S.Verts.Num());
	OutFloorPoint = FOutlawsGeometry::ToUE(C.X, Lv->SurfaceY(S, C, true), C.Y);
	return true;
}

bool AOutlawsLevelActor::DebugWallView(int32 Sector, int32 Wall, double BackUnits, FVector& OutEye, FVector& OutAt) const
{
	const FOutlawsLevel* Lv = LevelData.Get();
	if (!Lv || !Lv->Sectors.IsValidIndex(Sector) || !Lv->Sectors[Sector].Walls.IsValidIndex(Wall)) return false;
	const FOutlawsSector& S = Lv->Sectors[Sector];
	const FOutlawsWall& W = S.Walls[Wall];
	if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) return false;
	const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
	const double Len = FVector2D::Distance(A, B);
	if (Len < 1e-4) return false;
	const FVector2D T = (B - A) / Len;
	// the inward normal (as the geometry's walls face), and the point looked at: a sign's middle (8 units square, the
	// size of the level's DEFAULT) when the wall has one, else the wall's middle
	const bool bCW = S.IsClockwise();
	const FVector2D N = bCW ? FVector2D(T.Y, -T.X) : FVector2D(-T.Y, T.X);
	double Along = Len * 0.5, Up = (S.Floor.Y + S.Ceil.Y) * 0.5;
	if (W.Overlay >= 0)
	{
		Along = FMath::Clamp(-W.MidX + W.OvX + 4.0, 0.0, Len);
		Up = FMath::Min(S.Floor.Y - W.OvY + 4.0, S.Ceil.Y - 0.5);
	}
	const FVector2D P = A + T * Along;
	const FVector2D E = P + N * BackUnits;
	const FOutlawsSector* Under = Lv->SectorAt(E.X, Up, E.Y);
	const double Floor = Under ? Lv->SurfaceY(*Under, E, true) : S.Floor.Y;
	OutAt = FOutlawsGeometry::ToUE(P.X, Up, P.Y);
	OutEye = FOutlawsGeometry::ToUE(E.X, Floor, E.Y);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wall view: sector %d wall %d, looking at (%.2f, %.2f, %.2f) from (%.2f, %.2f) in sector %d, floor %.2f"),
		Sector, Wall, P.X, Up, P.Y, E.X, E.Y, Under ? Under->Index : -1, Floor);
	return true;
}

FString AOutlawsLevelActor::DebugAtxReport() const
{
	TArray<FString> Parts;
	for (const TPair<FString, FAtxState>& Kv : AtxLoops) Parts.Add(FString::Printf(TEXT("loop %s pc %d frame %s x%d"), *Kv.Key, Kv.Value.Pc, *Kv.Value.Frame, Kv.Value.Mids.Num()));
	for (const TPair<int64, FAtxState>& Kv : AtxWalls) Parts.Add(FString::Printf(TEXT("wall %d/%d %s pc %d halted %d frame %s"), Kv.Value.Sector, Kv.Value.Wall, *Kv.Value.Stem, Kv.Value.Pc, Kv.Value.bHalted ? 1 : 0, *Kv.Value.Frame));
	return Parts.Num() ? FString::Join(Parts, TEXT(" | ")) : TEXT("(no ATX)");
}

bool AOutlawsLevelActor::WaterSurfaceHit(const FVector& A, const FVector& B, FVector& Out) const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return false;
	const double U = FOutlawsGeometry::UnitsToCm;
	double Best = 2.0;
	const TBitArray<>& Surfaces = WaterSurfaceSectors(*Lv);
	for (const FOutlawsSector& S : Lv->Sectors)
	{
		if (!Surfaces.IsValidIndex(S.Index) || !Surfaces[S.Index]) continue;
		const double Zs = S.Ceil.Y * U;
		const double Da = A.Z - Zs, Db = B.Z - Zs;
		if (Da * Db > 0.0 || Da == Db) continue;
		const double T = Da / (Da - Db);
		if (T >= Best) continue;
		const FVector P = A + (B - A) * T;
		const FVector2D Plan = FOutlawsGeometry::ToOutlawsPlan(P);
		if (!InsideFloorPlan(S, Plan.X, Plan.Y)) continue;
		Best = T;
		Out = FVector(P.X, P.Y, Zs);
	}
	return Best <= 1.0;
}

const TBitArray<>& AOutlawsLevelActor::WaterSurfaceSectors(const FOutlawsLevel& Lv) const
{
	// (BUG-WATERHANDS) the water sectors whose ceiling is a water surface: a dry sector on it through a vertical adjoin,
	// written on either side (the water sector's VADJOIN up, or the dry sector's VADJOIN down onto it, as the geometry's
	// surfaces are built: VerticalPortal). Only the first was read: where the level wrote the adjoin on the dry side
	// (the user's town and cliff swims) no surface was ever found between the eyes and a hand, so the hands and the held
	// gear stayed pulled to the eyes under the water and drew over its surface. The ceiling of a water sector with more
	// water stacked on it is not a surface (a stroke across it is no splash). Worked out once per level (the adjoins and
	// the flags do not change)
	if (SurfaceSectorBits.Num() == Lv.Sectors.Num() && SurfaceSectorLevel == &Lv && SurfaceSectorLevelName == LevelName) return SurfaceSectorBits;
	SurfaceSectorLevel = &Lv;
	SurfaceSectorLevelName = LevelName;
	SurfaceSectorBits.Init(false, Lv.Sectors.Num());
	auto Meets = [](const FOutlawsSector& Lo, const FOutlawsSector& Hi) { return FMath::Abs(Lo.Ceil.Y - Hi.Floor.Y) < 1e-3; };
	auto Mark = [&](const FOutlawsSector& Water, const FOutlawsSector& Above)
	{
		if ((Water.Flags & OutlawsFlags::Water) && !(Above.Flags & OutlawsFlags::Water) && Meets(Water, Above)) SurfaceSectorBits[Water.Index] = true;
	};
	for (const FOutlawsSector& S : Lv.Sectors)
	{
		const FOutlawsSector* T = Lv.Sector(S.VAdjoin);
		if (!T) continue;
		Mark(S, *T);         // the water sector's adjoin up to the air
		Mark(*T, S);         // the air sector's adjoin down onto the water
	}
	int32 Count = 0;
	for (TConstSetBitIterator<> It(SurfaceSectorBits); It; ++It) ++Count;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT water surfaces: %d water sectors with air on their ceiling"), Count);
	return SurfaceSectorBits;
}

void AOutlawsLevelActor::SpawnSplash(const FVector& At, bool bBig, bool bSound, float Scale, float Volume)
{
	const int32 Light = FMath::Min(31, LightAt(At + FVector(0.0, 0.0, 20.0)) + 4);
	// the atlas with its chors (splash3/4: 7 and 5 frames at 10 fps, then TERMINATE): without them the one-shot never
	// ended and the sprite's default loop ran till the spark's 3 s were up (user 2026-09-29: it played a few times over)
	if (AOutlawsSpriteActor* S = MakeSprite(GetAtlas(bBig ? TEXT("splash3.nwx") : TEXT("splash4.nwx"), true), TEXT("SPLASH"), At, 0.f, Light))
	{
		SpawnedActors.Add(S);
		if (Scale != 1.f) S->SetActorScale3D(S->GetActorScale3D() * Scale);
		S->PlayChor(0, false);
		FSpark K;
		K.Sprite = S;
		K.bOneShot = true;
		K.Life = 3.f;
		Sparks.Add(K);
		SetActorTickEnabled(true);
	}
	if (bSound)
	{
		if (UAudioComponent* AC = PlayWav(bBig ? TEXT("splash") : TEXT("smsplash"), At))
		{
			if (Volume != 1.f) AC->SetVolumeMultiplier(AC->VolumeMultiplier * Volume);
		}
	}
}

bool AOutlawsLevelActor::WaterAt(FVector P, float& OutSurfaceZ) const
{
	OutSurfaceZ = 0.f;
	if (!LevelData)
	{
		return false;
	}
	// a water sector is water from floor to ceiling; the dry sector above adjoins it vertically
	int32 Top = -1;
	return WaterSurfaceAt(P, OutSurfaceZ, Top) && P.Z < OutSurfaceZ;
}

bool AOutlawsLevelActor::WaterSurfaceAt(const FVector& P, float& OutSurfaceZ, int32& OutTopSector, int32* OutSector, bool* OutAir) const
{
	// (BUG-SURFACEJOLT) the surface over the point: up through every water sector stacked on the one it is in (a deep pool
	// built of two or more, one on the next) to the top one's ceiling, at the point (a sloped ceiling's own height there).
	// The ceiling of the sector the point was in was taken before: a body whose middle sat in a lower water sector of a
	// stack was "at the surface" at that sector's top, a step under the real one, and the surface hold pulled it down
	OutSurfaceZ = 0.f;
	OutTopSector = -1;
	if (OutSector) *OutSector = -1;
	if (OutAir) *OutAir = false;
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return false;
	const double U = FOutlawsGeometry::UnitsToCm;
	const FVector2D Plan = FOutlawsGeometry::ToOutlawsPlan(P);
	const FOutlawsSector* S = Lv->SectorAt(Plan.X, P.Z / U, Plan.Y);
	if (!S || !(S->Flags & OutlawsFlags::Water)) return false;
	if (OutSector) *OutSector = S->Index;
	double Ceil = Lv->SurfaceY(*S, Plan, false);
	for (int32 Up = 0; Up < 8; ++Up)
	{
		// the sector just over this one's ceiling here (its vertical adjoin, or whatever is stacked there): water too, go on up
		const FOutlawsSector* Above = Lv->Sectors.IsValidIndex(S->VAdjoin) ? &Lv->Sectors[S->VAdjoin] : nullptr;
		if (!Above || !(Above->Flags & OutlawsFlags::Water) || FMath::Abs(Lv->SurfaceY(*Above, Plan, true) - Ceil) > 0.5 || !Lv->ContainsXZ(*Above, Plan.X, Plan.Y))
		{
			Above = Lv->SectorAt(Plan.X, Ceil + 0.05, Plan.Y);
			if (Above && (Above == S || FMath::Abs(Lv->SurfaceY(*Above, Plan, true) - Ceil) > 0.5)) Above = nullptr;
		}
		if (!Above || !(Above->Flags & OutlawsFlags::Water)) break;
		S = Above;
		Ceil = Lv->SurfaceY(*S, Plan, false);
	}
	OutTopSector = S->Index;
	OutSurfaceZ = float(Ceil * U);
	if (OutAir) { const TBitArray<>& Air = WaterSurfaceSectors(*Lv); *OutAir = Air.IsValidIndex(S->Index) && Air[S->Index]; }
	return true;
}

bool AOutlawsLevelActor::DebugPointInSector(int32 Index, FVector& OutAtCeiling) const
{
	// (BUG-SURFACEJOLT test) a point well inside a sector's floor plan (the inside grid point nearest its box's middle), at
	// its ceiling's height there
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	const FOutlawsSector* S = Lv ? Lv->Sector(Index) : nullptr;
	if (!S || S->Verts.Num() < 3) return false;
	FBox2D Box(ForceInit);
	for (const FVector2D& V : S->Verts) Box += V;
	double Best = 1e30;
	bool bFound = false;
	for (int32 i = 1; i < 16; ++i) for (int32 j = 1; j < 16; ++j)
	{
		const FVector2D Plan(FMath::Lerp(Box.Min.X, Box.Max.X, i / 16.0), FMath::Lerp(Box.Min.Y, Box.Max.Y, j / 16.0));
		if (!InsideFloorPlan(*S, Plan.X, Plan.Y)) continue;
		// away from the walls: the nearest edge's distance counts most
		double Edge = 1e30;
		for (int32 k = 0; k < S->Verts.Num(); ++k)
		{
			const FVector2D A = S->Verts[k], B = S->Verts[(k + 1) % S->Verts.Num()];
			Edge = FMath::Min(Edge, FMath::PointDistToSegment(FVector(Plan, 0.0), FVector(A, 0.0), FVector(B, 0.0)));
		}
		if (-Edge < Best) { Best = -Edge; OutAtCeiling = FOutlawsGeometry::ToUE(Plan, Lv->SurfaceY(*S, Plan, false)); bFound = true; }
	}
	return bFound;
}

FString AOutlawsLevelActor::DebugWaterSurfaceCheck(int32& OutChecked, int32& OutFound, int32& OutOldFound, int32 OnlySector) const
{
	// (BUG-WATERHANDS test) every water surface of the level (or one sector's): the eyes 30 cm over it and a hand 40 cm
	// under it at a point inside the sector; WaterSurfaceHit must find the surface between them (what UpdateHandWater
	// asks each frame to let the held gear sit behind the water). The old rule (the water sector's own VADJOIN only) is
	// counted beside it
	OutChecked = OutFound = OutOldFound = 0;
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return TEXT("no level");
	const double U = FOutlawsGeometry::UnitsToCm;
	const TBitArray<>& Surfaces = WaterSurfaceSectors(*Lv);
	TArray<FString> Missed;
	for (const FOutlawsSector& S : Lv->Sectors)
	{
		if (!Surfaces[S.Index] || (OnlySector >= 0 && S.Index != OnlySector)) continue;
		FBox2D Box(ForceInit);
		for (const FVector2D& V : S.Verts) Box += V;
		bool bPlaced = false;
		for (int32 k = 0; k < 49 && !bPlaced; ++k)
		{
			const FVector2D Plan(FMath::Lerp(Box.Min.X, Box.Max.X, (1 + k % 7) / 8.0), FMath::Lerp(Box.Min.Y, Box.Max.Y, (1 + k / 7) / 8.0));
			if (!InsideFloorPlan(S, Plan.X, Plan.Y)) continue;
			bPlaced = true;
			const double Zs = S.Ceil.Y * U;
			const FVector Eye(Plan.Y * U, Plan.X * U, Zs + 30.0), Hand(Plan.Y * U, Plan.X * U, Zs - 40.0);
			FVector Hit;
			++OutChecked;
			if (WaterSurfaceHit(Eye, Hand, Hit) && FMath::Abs(Hit.Z - Zs) < 1.0) ++OutFound;
			else if (Missed.Num() < 12) Missed.Add(FString::FromInt(S.Index));
			if (S.VAdjoin >= 0) ++OutOldFound;
		}
	}
	return Missed.Num() ? FString::Printf(TEXT("missed:%s"), *FString::Join(Missed, TEXT(" "))) : FString(TEXT("none missed"));
}

FString AOutlawsLevelActor::DebugWaterSectorInfo(int32 Index) const
{
	// (BUG-SURFACEJOLT test) a water sector as the swim reads it: its heights, flags, current and vertical adjoin, and over
	// a grid of its plan at 55 and 140 cm under its surface (a treading body's middle and feet), where the sector there
	// is another (a stacked one) and where the old reading (that sector's own ceiling) was not the surface
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	const FOutlawsSector* S = Lv ? Lv->Sector(Index) : nullptr;
	if (!S) return FString::Printf(TEXT("sector %d: none"), Index);
	const double U = FOutlawsGeometry::UnitsToCm;
	FBox2D Box(ForceInit);
	for (const FVector2D& V : S->Verts) Box += V;
	int32 Points = 0, Stacked = 0, OldWrong = 0;
	float Worst = 0.f;
	TSet<int32> Others, Currents;
	for (int32 i = 1; i < 8; ++i) for (int32 j = 1; j < 8; ++j)
	{
		const FVector2D Plan(FMath::Lerp(Box.Min.X, Box.Max.X, i / 8.0), FMath::Lerp(Box.Min.Y, Box.Max.Y, j / 8.0));
		if (!Lv->ContainsXZ(*S, Plan.X, Plan.Y)) continue;
		const double Top = Lv->SurfaceY(*S, Plan, false) * U;
		for (const double Under : { 55.0, 140.0 })
		{
			const FVector P(Plan.Y * U, Plan.X * U, Top - Under);
			const FOutlawsSector* At = Lv->SectorAt(Plan.X, P.Z / U, Plan.Y);
			float Surface = 0.f;
			int32 TopSec = -1;
			if (!At) continue;
			++Points;
			if (At != S) { ++Stacked; Others.Add(At->Index); }
			if (!At->Velocity.IsNearlyZero()) Currents.Add(At->Index);
			if (WaterSurfaceAt(P, Surface, TopSec) && At->Flags & OutlawsFlags::Water)
			{
				const float Old = float(At->Ceil.Y * U);
				if (FMath::Abs(Old - Surface) > 5.f) { ++OldWrong; Worst = FMath::Max(Worst, FMath::Abs(Old - Surface)); }
			}
		}
	}
	FString Oth, Cur;
	for (int32 o : Others) Oth += FString::Printf(TEXT(" %d"), o);
	for (int32 c : Currents)
	{
		const FOutlawsSector* C = Lv->Sector(c);
		Cur += FString::Printf(TEXT(" %d(%.0f,%.0f,%.0f%s)"), c, C->Velocity.X, C->Velocity.Y, C->Velocity.Z, (C->Flags & OutlawsFlags::VelFloorOnly) ? TEXT(" floor only") : TEXT(""));
	}
	return FString::Printf(TEXT("sector %d '%s': flags 0x%x%s, floor %.0f ceil %.0f cm (sloped floor %d ceil %d), vadjoin %d, air on its ceiling %d, gravity %.0f, current (%.0f,%.0f,%.0f); %d grid points at the middle's and the feet's depth: %d in another sector (%s ), %d where the old reading (that sector's own ceiling) missed the surface by up to %.0f cm; currents there:%s"),
		Index, *S->Name, S->Flags, (S->Flags & OutlawsFlags::Water) ? TEXT(" water") : TEXT(""), S->Floor.Y * U, S->Ceil.Y * U, S->SlopeFloor.bSet ? 1 : 0, S->SlopeCeil.bSet ? 1 : 0,
		S->VAdjoin, WaterSurfaceSectors(*Lv)[Index] ? 1 : 0, S->Gravity, S->Velocity.X, S->Velocity.Y, S->Velocity.Z, Points, Stacked, *Oth, OldWrong, Worst, Cur.IsEmpty() ? TEXT(" none") : *Cur);
}

bool AOutlawsLevelActor::DebugMidView(int32 Nth, bool bShatter, FVector& OutFloor, float& OutYaw, FString& OutWhat) const
{
	if (!LevelData)
	{
		return false;
	}
	int32 Seen = 0;
	for (const FOutlawsSector& S : LevelData->Sectors)
	{
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			const FOutlawsWall& W = S.Walls[w];
			if (!(W.Flags1 & OutlawsFlags::WallMidTex) || W.Mid < 0 || W.Adjoin < 0) continue;
			if (((W.Flags1 & OutlawsFlags::WallShatter) != 0) != bShatter) continue;
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2], C = (A + B) * 0.5;
			// try both sides; keep the one inside this sector
			const FVector2D N = FVector2D(-(B - A).Y, (B - A).X).GetSafeNormal();
			for (const double Sign : { 1.0, -1.0 })
			{
				const FVector2D P = C + N * Sign * (200.0 / FOutlawsGeometry::UnitsToCm);
				if (LevelData->SectorAt(P.X, S.Floor.Y + 1.0, P.Y) != &S) continue;
				if (Seen++ < Nth) break;
				OutFloor = FOutlawsGeometry::ToUE(P.X, S.Floor.Y, P.Y);
				const FVector To = FOutlawsGeometry::ToUE(C.X, S.Floor.Y, C.Y) - OutFloor;
				OutYaw = To.Rotation().Yaw;
				OutWhat = FString::Printf(TEXT("sector %d wall %d mid %s flags 0x%x"), S.Index, w, *LevelData->TexStem(W.Mid), W.Flags1);
				return true;
			}
		}
	}
	return false;
}

bool AOutlawsLevelActor::FindWaterPoint(FVector& OutPoint) const
{
	if (!LevelData)
	{
		return false;
	}
	const FOutlawsSector* Best = nullptr;
	FVector2D BestAt = FVector2D::ZeroVector;
	for (const FOutlawsSector& S : LevelData->Sectors)
	{
		const double Depth = S.Ceil.Y - S.Floor.Y;
		if (!(S.Flags & OutlawsFlags::Water) || S.VAdjoin < 0 || Depth < 5.0 || S.Verts.Num() < 3) continue;
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		C /= S.Verts.Num();
		const double Y = S.Ceil.Y - 2.0;
		if (LevelData->SectorAt(C.X, Y, C.Y) != &S) continue;   // centroid outside a concave sector
		if (!Best || Depth > Best->Ceil.Y - Best->Floor.Y)
		{
			Best = &S;
			BestAt = C;
		}
	}
	if (!Best)
	{
		return false;
	}
	OutPoint = FOutlawsGeometry::ToUE(BestAt.X, Best->Ceil.Y - 2.0, BestAt.Y);
	return true;
}

void AOutlawsLevelActor::OnUsePressed()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC) return;
	FVector Loc;
	FRotator Rot;
	PC->GetPlayerViewPoint(Loc, Rot);
	Use(Loc, Rot.Vector());
}

void AOutlawsLevelActor::WallsAt(const FVector& P, int32 Prefer, TArray<TPair<int32, int32>>& Out, bool bNearOnly, bool bAnyHeight) const
{
	Out.Reset();
	if (!Inf) return;
	const FOutlawsLevel& L = Inf->Live;
	const FVector2D Pt = FOutlawsGeometry::ToOutlawsPlan(P);
	const double Y = P.Z / FOutlawsGeometry::UnitsToCm;
	TArray<int32> Candidates;
	if (L.Sectors.IsValidIndex(Prefer))
	{
		Candidates.Add(Prefer);
		for (const FOutlawsWall& W : L.Sectors[Prefer].Walls)
		{
			if (W.Adjoin >= 0) Candidates.AddUnique(W.Adjoin);
			if (W.DAdjoin >= 0) Candidates.AddUnique(W.DAdjoin);
		}
	}
	auto Scan = [&](const TArray<int32>& Secs)
	{
		double Best = 0.5;   // units (15 cm)
		TArray<TPair<int32, int32>> Hits;
		TArray<double> Dist;
		for (int32 s : Secs)
		{
			const FOutlawsSector& S = L.Sectors[s];
			if (!bAnyHeight && (Y < S.Floor.Y - 1.0 || Y > S.Ceil.Y + 1.0)) continue;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				const double D = PointSegmentDistance(Pt, S.Verts[W.V1], S.Verts[W.V2]);
				if (D < Best + 0.05)
				{
					Hits.Add(TPair<int32, int32>(s, w));
					Dist.Add(D);
					Best = FMath::Min(Best, D);
				}
			}
		}
		for (int32 i = 0; i < Hits.Num(); ++i)
		{
			if (Dist[i] <= Best + 0.05) Out.Add(Hits[i]);   // a wall and its mirror in the next sector
		}
	};
	Scan(Candidates);
	if (Out.Num() == 0 && !bNearOnly)
	{
		TArray<int32> All;
		for (const FOutlawsSector& S : L.Sectors) All.Add(S.Index);
		Scan(All);
	}
}

void AOutlawsLevelActor::Use(FVector ViewOrigin, FVector ViewDirection)
{
	if (!Inf) return;
	UWorld* World = GetWorld();
	if (!World) return;
	// standing in a sector nudges it from inside (lifts you ride)
	if (PlayerSector >= 0)
	{
		Inf->SectorEvent(PlayerSector, OutlawsInf::EvNudgeFront);
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUse), true);
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn()) Q.AddIgnoredActor(Pawn);
	}
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, ViewOrigin, ViewOrigin + ViewDirection.GetSafeNormal() * UseDistance, ECC_Visibility, Q))
	{
		return;
	}
	if (const int32* Gi = ComponentGroup.Find(Hit.GetComponent()))
	{
		const FOutlawsElevator& E = Inf->Elevators[Groups[*Gi].Elevator];
		const int32 Master = E.Sectors[0];
		Inf->SectorEvent(Master, Master == PlayerSector ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT use: %s (%s)"), *E.Name, *Inf->Inf.Items[E.Item].Classes[E.ClassIndex].RawClass);
		return;
	}
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Hit.ImpactPoint, PlayerSector, Walls);
	for (const TPair<int32, int32>& SW : Walls)
	{
		const bool bFront = SW.Key == PlayerSector;
		Inf->WallEvent(SW.Key, SW.Value, bFront ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack);
		// nudging a wall that opens onto an elevator sector nudges that sector from outside
		const FOutlawsWall& W = Inf->Live.Sectors[SW.Key].Walls[SW.Value];
		for (int32 Adj : { W.Adjoin, W.DAdjoin })
		{
			if (Adj >= 0 && Adj != PlayerSector && (Inf->ElevatorsBySector.Contains(Adj) || Inf->TriggersBySector.Contains(Adj)))
			{
				Inf->SectorEvent(Adj, OutlawsInf::EvNudgeBack);
			}
		}
	}
}

void AOutlawsLevelActor::ParseRiffWav(const TArray<uint8>& D, FWav& Dest)
{
	FWav* Wav = &Dest;
	int32 Pos = 12, Bits = 16;
	while (Pos + 8 <= D.Num())
	{
		int32 Size;
		FMemory::Memcpy(&Size, D.GetData() + Pos + 4, 4);
		const uint8* Body = D.GetData() + Pos + 8;
		if (FMemory::Memcmp(D.GetData() + Pos, "fmt ", 4) == 0 && Size >= 16)
		{
			uint16 Ch, B;
			uint32 Rate;
			FMemory::Memcpy(&Ch, Body + 2, 2);
			FMemory::Memcpy(&Rate, Body + 4, 4);
			FMemory::Memcpy(&B, Body + 14, 2);
			Wav->Channels = Ch;
			Wav->Rate = int32(Rate);
			Bits = B;
		}
		else if (FMemory::Memcmp(D.GetData() + Pos, "data", 4) == 0)
		{
			const int32 N = FMath::Min(Size, D.Num() - (Pos + 8));
			if (Bits == 16)
			{
				Wav->Pcm.Append(Body, N);
			}
			else if (Bits == 8)
			{
				Wav->Pcm.SetNumUninitialized(N * 2);
				int16* Out = reinterpret_cast<int16*>(Wav->Pcm.GetData());
				for (int32 i = 0; i < N; ++i) Out[i] = int16((int32(Body[i]) - 128) << 8);
			}
			break;
		}
		if (Size < 0) break;
		Pos += 8 + Size + (Size & 1);
	}
}

TArray<FString>* AOutlawsLevelActor::WavSpy = nullptr;

TSharedPtr<AOutlawsLevelActor::FWav> AOutlawsLevelActor::DecodeWav(const FString& Name, bool bDiskFile)
{
	// a sound read and decoded whole (PlayWav's, and WarmWav's behind the load)
	TSharedPtr<FWav> Wav = MakeShared<FWav>();
	TArray<uint8> D;
	// (REQ-RELOADSLICES) "name@from-to": that part of the wave, cut once and kept under its own name
	int32 SliceAt = INDEX_NONE;
	const bool bSlice = !bDiskFile && Name.FindChar(TEXT('@'), SliceAt);
	FString File = bSlice ? Name.Left(SliceAt) : Name;
	if (!bDiskFile && !File.Contains(TEXT("."))) File += TEXT(".wav");
	const bool bGot = bDiskFile ? FFileHelper::LoadFileToArray(D, *Name.Mid(5)) : AssetsRef->GetSound(File, bHDSounds, D);
	if (bGot && D.Num() > 12 && FMemory::Memcmp(D.GetData(), "RIFF", 4) == 0)
	{
		ParseRiffWav(D, *Wav);
	}
	else if (bGot && D.Num() > 4 && FMemory::Memcmp(D.GetData(), "OggS", 4) == 0)
	{
		// the Remastered taunts (oltaunthd.lab) are Ogg Vorbis: decoded whole to 16-bit PCM
		LoadVorbisLibraries();
		FVorbisAudioInfo Vorbis;
		FSoundQualityInfo Q;
		if (Vorbis.ReadCompressedInfo(D.GetData(), D.Num(), &Q) && Q.SampleDataSize > 0)
		{
			Wav->Channels = Q.NumChannels;
			Wav->Rate = int32(Q.SampleRate);
			Wav->Pcm.SetNumZeroed(Q.SampleDataSize);
			Vorbis.ExpandFile(Wav->Pcm.GetData(), &Q);
			static int32 Logged = 0;
			if (Logged++ < 2) UE_LOG(LogOutlaws, Log, TEXT("OLRT HD sound %s: Ogg Vorbis, %d ch, %d Hz, %.2f s"), *Name, Wav->Channels, Wav->Rate, Q.SampleDataSize / float(2 * FMath::Max(1, Wav->Channels) * FMath::Max(1, Wav->Rate)));
		}
	}
	if (bSlice && Wav->Pcm.Num() > 0)
	{
		FString A, B;
		Name.Mid(SliceAt + 1).Split(TEXT("-"), &A, &B);
		const int32 Block = 2 * FMath::Max(1, Wav->Channels);
		const int32 Frames = Wav->Pcm.Num() / Block;
		const int32 From = FMath::Clamp(FMath::RoundToInt(FCString::Atof(*A) * Wav->Rate), 0, Frames);
		const int32 To = B.IsEmpty() ? Frames : FMath::Clamp(FMath::RoundToInt(FCString::Atof(*B) * Wav->Rate), From, Frames);
		TArray<uint8> Cut(Wav->Pcm.GetData() + From * Block, (To - From) * Block);
		Wav->Pcm = MoveTemp(Cut);
	}
	return Wav;
}

void AOutlawsLevelActor::WarmWav(const FString& Name)
{
	// (FIRSTHITCH) read into the level's cache now, behind the load, as the first PlayWav of it would
	if (!AssetsRef || Name.IsEmpty() || FChar::IsDigit(Name[0])) return;
	const FString Key = Name.ToLower();
	if (Key.StartsWith(TEXT("file:")) || WavCache.Contains(Key)) return;
	WavCache.Add(Key, DecodeWav(Name, false));
}

UAudioComponent* AOutlawsLevelActor::PlayWav(const FString& Name, const FVector& Where, bool bLoop, bool b2D)
{
	if (WavSpy) WavSpy->Add(Name.ToLower());
	if (bRestoring) return nullptr;       // a save being put back makes no noise
	if (bRoomTestLog && !b2D && !bLoop) RoomSend(nullptr, Where);        // (R13's test, muted: the send it would get)
	if ((bBackdrop || bFrontEnd) && !b2D) return nullptr;   // (the level behind the main menu is silent: the menu's music alone)
	UWorld* World = GetWorld();
	// a number in place of the file name means "no sound"
	if (!bPlaySounds || !AssetsRef || !World || !World->IsGameWorld() || Name.IsEmpty() || FChar::IsDigit(Name[0])) return nullptr;
	// a 3D one-shot whose source is beyond hearing (the attenuation ends at 38 m) is not started at all: no wave, no component,
	// no voice in the mixer (user 2026-10-03).  The loops (a moving door, a generator) start as before: they go on till
	// stopped, and attenuate in and out of range.  Every caller takes a null (a 2D sound, or none, is the listener's own)
	if (!b2D && !bLoop)
	{
		const float Far = CVarOLFarSoundDistance.GetValueOnGameThread();
		const APlayerController* PC = Far > 0.f ? World->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager && FVector::DistSquared(PC->PlayerCameraManager->GetCameraLocation(), Where) > FMath::Square(double(Far)))
		{
			++FarSoundsSkipped;
			return nullptr;
		}
	}
	const FString Key = Name.ToLower();
	// "file:<path>": a wav on disk (the converted music), kept for the last two tracks only (MusicWavCache)
	const bool bDiskFile = Key.StartsWith(TEXT("file:"));
	TSharedPtr<FWav> Wav;
	const int32 MusicAt = bDiskFile ? MusicWavCache.IndexOfByPredicate([&Key](const TPair<FString, TSharedPtr<FWav>>& P) { return P.Key == Key; }) : INDEX_NONE;
	if (MusicAt != INDEX_NONE)
	{
		Wav = MusicWavCache[MusicAt].Value;
		MusicWavCache.RemoveAt(MusicAt);
		MusicWavCache.Emplace(Key, Wav);
	}
	else if (TSharedPtr<FWav>* Found = bDiskFile ? nullptr : WavCache.Find(Key))
	{
		Wav = *Found;
	}
	else
	{
		Wav = DecodeWav(Name, bDiskFile);
		if (!bDiskFile) WavCache.Add(Key, Wav);
		else if (Wav->Pcm.Num() > 0)
		{
			MusicWavCache.Emplace(Key, Wav);
			if (MusicWavCache.Num() > 2)
			{
				TSharedPtr<FWav> Old = MoveTemp(MusicWavCache[0].Value);
				MusicWavCache.RemoveAt(0);
				Async(EAsyncExecution::ThreadPool, [Old = MoveTemp(Old)]() mutable { Old.Reset(); });
			}
		}
	}
	if (!Wav || Wav->Pcm.Num() == 0) return nullptr;
	{
		// whole samples only (a decode can leave a stray byte: the procedural wave ensures on it)
		const int32 Block = 2 * FMath::Max(1, Wav->Channels);
		if (Wav->Pcm.Num() % Block) Wav->Pcm.SetNum(Wav->Pcm.Num() - Wav->Pcm.Num() % Block);
		if (Wav->Pcm.Num() == 0) return nullptr;
	}
	const double Tp0 = FPlatformTime::Seconds();
	// played from the shared samples, no copy (UOutlawsPcmWave; user 2026-10-01: a copy a play was 9 MB a second)
	UOutlawsPcmWave* S = NewObject<UOutlawsPcmWave>(this);
	S->SetSampleRate(Wav->Rate);
	S->NumChannels = Wav->Channels;
	S->Duration = float(Wav->Pcm.Num()) / float(2 * FMath::Max(1, Wav->Channels) * FMath::Max(1, Wav->Rate));
	S->SoundGroup = SOUNDGROUP_Default;
	S->bLooping = false;
	S->Pcm = TSharedPtr<const TArray<uint8>>(Wav, &Wav->Pcm);
	S->bLoopPcm = bLoop;
	if (bLoop)
	{
		S->Duration = INDEFINITELY_LOOPING_DURATION;      // the moving sound: round and round till it is stopped
	}
	if (!SoundAttenuation)
	{
		SoundAttenuation = NewObject<USoundAttenuation>(this);
		SoundAttenuation->Attenuation.bAttenuate = true;
		SoundAttenuation->Attenuation.bSpatialize = true;
		SoundAttenuation->Attenuation.AttenuationShapeExtents = FVector(300.f, 0.f, 0.f);
		SoundAttenuation->Attenuation.FalloffDistance = 3500.f;
	}
	const double Tp1 = FPlatformTime::Seconds();
	// a procedural wave never reports that it finished, so stop it when its samples run out
	UAudioComponent* AC = b2D ? UGameplayStatics::SpawnSound2D(this, S, SoundVolume * MasterVolume * LevelSoundDuck)
		: UGameplayStatics::SpawnSoundAtLocation(this, S, Where, FRotator::ZeroRotator, SoundVolume * MasterVolume * LevelSoundDuck, 1.f, 0.f, SoundAttenuation);
	const double Tp2 = FPlatformTime::Seconds();
	if (AC && LevelSoundDuck < 1.f) DuckedSounds.Add(AC);        // (started under a mid-level movie: back up with the rest)
	if (AC && !bLoop)
	{
		TimedSounds.Emplace(AC, FPlatformTime::Seconds() + S->Duration + 0.1);
	}
	if (AC)
	{
		// under water all of it is muffled but the bubbles (user 2026-09-30)
		const bool bSpared = Key.StartsWith(TEXT("bubbles"));
		LiveSounds.Emplace(AC, bSpared);
		if (bMuffled && !bSpared)
		{
			AC->SetLowPassFilterFrequency(MuffleHz);
			AC->SetLowPassFilterEnabled(true);
		}
		if (!b2D) RoomSend(AC, Where);       // (R13: the room's reverb, by where it comes from)
	}
	if (bDiskFile)
	{
		const double Tp3 = FPlatformTime::Seconds();
		MusicPlayMs[0] = (Tp1 - Tp0) * 1000.0;
		MusicPlayMs[1] = (Tp2 - Tp1) * 1000.0;
		MusicPlayMs[2] = (Tp3 - Tp2) * 1000.0;
	}
	return AC;
}

void AOutlawsLevelActor::SetMuffled(bool bOn)
{
	if (bOn == bMuffled) return;
	bMuffled = bOn;
	LiveSounds.RemoveAll([](const TPair<TWeakObjectPtr<UAudioComponent>, bool>& P) { return !P.Key.IsValid(); });
	for (const TPair<TWeakObjectPtr<UAudioComponent>, bool>& P : LiveSounds)
	{
		if (P.Value) continue;
		P.Key->SetLowPassFilterFrequency(MuffleHz);
		P.Key->SetLowPassFilterEnabled(bOn);
	}
}

void AOutlawsLevelActor::StopSpentSounds()
{
	OL_PERF_SCOPE("Level: spent sounds");
	LiveSounds.RemoveAll([](const TPair<TWeakObjectPtr<UAudioComponent>, bool>& P) { return !P.Key.IsValid() || !P.Key->IsPlaying(); });
	const double Now = FPlatformTime::Seconds();
	TimedSounds.RemoveAll([Now](const TPair<TWeakObjectPtr<UAudioComponent>, double>& T)
	{
		if (!T.Key.IsValid()) return true;
		if (Now < T.Value) return false;
		T.Key->Stop();
		return true;
	});
}

TArray<FString> AOutlawsLevelActor::ReloadOpenRun(const FString& Wav, int32 Shells)
{
	// the cuts of SRELOAD (user 2026-10-15): the open 0 to 0.25 s, a shell its clicks, 0.30 to 0.55 s (BUG-RELOADPACE: it was
	// 0.25 to 0.65, two shells 1.37 s in all), the shut from 0.65 s
	TArray<FString> Run;
	Run.Add(Wav + FString::Printf(TEXT("@0-%g"), ReloadOpenLen));
	for (int32 i = 0; i < Shells; ++i) Run.Add(Wav + FString::Printf(TEXT("@%g-%g"), ReloadShellFrom, ReloadShellTo));
	return Run;
}

FString AOutlawsLevelActor::ReloadShutSlice(const FString& Wav)
{
	return Wav + FString::Printf(TEXT("@%g-"), ReloadShutFrom);
}

FString AOutlawsLevelActor::ReloadCodes(const TArray<FString>& Wavs)
{
	FString Out;
	const TArray<FString> Run = ReloadOpenRun(TEXT("sreload"), 1);
	const FString Shut = ReloadShutSlice(TEXT("sreload"));
	for (const FString& W : Wavs)
	{
		if (W == TEXT("shotout")) Out += TEXT("X");
		else if (W == TEXT("sreload")) Out += TEXT("R");
		else if (W == Run[0]) Out += TEXT("O");
		else if (W == Run[1]) Out += TEXT("S");
		else if (W == Shut) Out += TEXT("C");
	}
	return Out;
}

void AOutlawsLevelActor::PlaySoundRun(int32 Key, const TArray<FString>& Wavs, const FVector& Where)
{
	StopSoundRun(Key);
	if (Wavs.Num() == 0) return;
	FSoundRun& R = SoundRuns.Add(Key);
	R.Wavs = Wavs;
	R.Where = Where;
	UE_LOG(LogOutlaws, Log, TEXT("OLSND run %d: %s"), Key, *FString::Join(Wavs, TEXT(", ")));
	TickSoundRuns(0.f);
}

void AOutlawsLevelActor::StopSoundRun(int32 Key)
{
	FSoundRun* R = SoundRuns.Find(Key);
	if (!R) return;
	if (R->Next < R->Wavs.Num() || R->Wait > 0.f)
	{
		++SoundRunCuts;
		UE_LOG(LogOutlaws, Log, TEXT("OLSND run %d cut off: %d of %d started, %.2f s of the last left"), Key, R->Next, R->Wavs.Num(), FMath::Max(0.f, R->Wait));
		if (UAudioComponent* A = R->Playing.Get()) A->Stop();
	}
	SoundRuns.Remove(Key);
}

TArray<AOutlawsLevelActor::FRunHeard>* AOutlawsLevelActor::RunSpy = nullptr;

void AOutlawsLevelActor::TickSoundRuns(float Dt)
{
	SoundRunClock += Dt;
	for (auto It = SoundRuns.CreateIterator(); It; ++It)
	{
		FSoundRun& R = It.Value();
		R.Wait -= Dt;
		while (R.Wait <= 0.f && R.Next < R.Wavs.Num())
		{
			const FString& Part = R.Wavs[R.Next++];
			UAudioComponent* A = PlaySoundAt(Part, R.Where);
			R.Playing = A;
			// its length: from the cut's own times, else the wave's
			FString Spec, From, To;
			float Len = A && A->Sound ? A->Sound->Duration : 0.f;
			if (Part.Split(TEXT("@"), nullptr, &Spec) && Spec.Split(TEXT("-"), &From, &To) && !To.IsEmpty()) Len = FCString::Atof(*To) - FCString::Atof(*From);
			else if (Len <= 0.f)
			{
				// (no component, -nosound: the cut kept in the cache)
				if (const TSharedPtr<FWav>* W = WavCache.Find(Part.ToLower()))
					if (W->IsValid()) Len = float((*W)->Pcm.Num()) / float(2 * FMath::Max(1, (*W)->Channels) * FMath::Max(1, (*W)->Rate));
			}
			if (RunSpy) RunSpy->Add({ Part.ToLower(), SoundRunClock, Len });
			UE_LOG(LogOutlaws, Log, TEXT("OLSND run %d plays %s at %.3f s (%.3f s long)"), It.Key(), *Part, SoundRunClock, Len);
			R.Wait += FMath::Max(0.f, Len);
		}
		if (R.Next >= R.Wavs.Num() && R.Wait <= 0.f) It.RemoveCurrent();
	}
}

const FOutlawsLevel* AOutlawsLevelActor::LiveLevel() const
{
	return Inf ? &Inf->Live : LevelData.Get();
}

int32 AOutlawsLevelActor::UseIdOf(const FString& Item) const
{
	const FItemDef* D = ItemDefs.Find(Item.ToLower());
	return D ? D->UseId : -1;
}

void AOutlawsLevelActor::SetToolHeld(int32 UseId, bool bHeld)
{
	if (!Inf || UseId < 0) return;
	const FItemInfo* Info = ItemsByUseId.Find(UseId);
	if (bHeld && (!Info || GetCount(Info->Name) > 0)) Inf->Items.Add(UseId);
	else if (!bHeld) Inf->Items.Remove(UseId);
}

bool AOutlawsLevelActor::ToolStabFloor(const FVector& Tip)
{
	if (!Inf) return false;
	const FOutlawsSector* S = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(Tip, 0.3));
	if (!S) return false;
	// driven in from above: the sector's own nudge (the dig spot answers EVENT_MASK 16)
	const bool bTaken = Inf->SectorEvent(S->Index, OutlawsInf::EvNudgeFront);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT spade into sector %d %s: %s"), S->Index, *S->Name, bTaken ? TEXT("dug") : TEXT("nothing"));
	return bTaken;
}

AOutlawsSpriteActor* AOutlawsLevelActor::SpawnSceneSprite(const FString& Name, const FString& Anim, const FVector& Feet, float Yaw, int32 Light)
{
	UWorld* World = GetWorld();
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(Anim, false);
	if (!World || !Atlas || !Atlas->Texture || !SpriteQuad || !SpriteMaterial) return nullptr;
	const double Wcm = Atlas->CellW / 16.0 * FOutlawsGeometry::UnitsToCm;
	const double Hcm = Atlas->CellH / 16.0 * FOutlawsGeometry::UnitsToCm;
	const FTransform Xf(FRotator::ZeroRotator, Feet + FVector(0.0, 0.0, 0.5), FVector(1.0, Wcm, Hcm));
	FActorSpawnParameters P;
	P.Owner = this;
	P.ObjectFlags |= RF_Transient;
	P.bDeferConstruction = true;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AOutlawsSpriteActor* S = World->SpawnActor<AOutlawsSpriteActor>(AOutlawsSpriteActor::StaticClass(), Xf, P);
	if (!S) return nullptr;
	S->OutlawsName = Name;
	S->OutlawsYaw = Yaw;
	S->LightLevel = Light;
	S->AtlasCols = Atlas->Cols;
	S->AtlasRows = Atlas->Rows;
	S->Directions = Atlas->Dirs;
	S->FrameCount = Atlas->Frames;
	S->FPS = Atlas->FPS;
	S->Grid = Atlas->Grid;
	S->SpriteTexture = Atlas->Texture;
	S->bFullBright = IsFullBrightArt(*Atlas);       // (REQ-FIREBRIGHT)
	S->Quad->SetStaticMesh(SpriteQuad);
	S->Quad->SetMaterial(0, SpriteMaterialFor(S, *Atlas));     // (REQ-FIREBRIGHT2)
	S->FinishSpawning(Xf);
	SpawnedActors.Add(S);
	UseItemModel(S, Anim, Yaw);
	return S;
}

void AOutlawsLevelActor::UpdateItemGens(float Dt)
{
	OL_PERF_SCOPE("Level: item generators");
	for (FItemGen& G : ItemGens)
	{
		G.Alive.RemoveAll([](const TWeakObjectPtr<AActor>& A) { return !A.IsValid(); });
		G.Wait -= Dt;
		if (G.Wait > 0.f || G.Alive.Num() >= G.Max) continue;
		G.Wait = G.Freq + FMath::FRandRange(0.f, G.Rand);
		if (AOutlawsSpriteActor* S = SpawnSceneSprite(G.Name, G.Anim, G.Pos, G.Yaw, G.Light))
		{
			G.Alive.Add(S);
			if (const TSharedPtr<OutlawsTextures::FSpriteAtlas> GenAtlas = GetAtlas(G.Anim, false))
			{
				S->SetFeet(GenAtlas->BelowPx, GenAtlas->FootPx, GenAtlas->CellH); S->SetFeetAcross(GenAtlas->SidePx2, GenAtlas->CellW);     // (on the frame's insertion point, user 2026-10-02)
			}
			static int32 Logged = 0;
			if (Logged++ < 3) UE_LOG(LogOutlaws, Log, TEXT("OLRT generated %s at %s"), *G.Name, *G.Pos.ToString());
		}
	}
}

bool AOutlawsLevelActor::DebugMovingGround(const FVector& Near, FVector& Out, int32 Kind) const
{
	if (!Inf) return false;
	double Best = 1e30;
	for (const FOutlawsSector& S : Inf->Live.Sectors)
	{
		// (open ground only: the train's UNDER strips run beneath the cars, 6 units high, and a drop over one lands on a car)
		if (!(S.Flags & OutlawsFlags::VelFloorOnly) || S.Velocity.IsNearlyZero() || S.Verts.Num() < 3 || S.Ceil.Y - S.Floor.Y < 10.0) continue;
		const bool bKills = (S.Flags & (OutlawsFlags::SecDamageDie | OutlawsFlags::FloorDamageDie)) != 0;
		if ((Kind == 1 && bKills) || (Kind == 2 && !bKills)) continue;
		FVector2D C(0.0, 0.0);
		for (const FVector2D& V : S.Verts) C += V;
		C /= double(S.Verts.Num());
		if (!Inf->Live.ContainsXZ(S, C.X, C.Y))
		{
			// (the ground: only a sector whose middle is in it, the wide open ground; the strip ahead of the engine is not)
			if (Kind != 1) continue;
			// (a hollow shape, as the train's gaps between cars are: the point of a 16 x 16 grid over it inside it nearest the middle)
			FBox2D Box(ForceInit);
			for (const FVector2D& V : S.Verts) Box += V;
			double Near2 = 1e30;
			FVector2D Pick(0.0, 0.0);
			for (int32 i = 0; i <= 16; ++i)
			{
				for (int32 j = 0; j <= 16; ++j)
				{
					const FVector2D G(FMath::Lerp(Box.Min.X, Box.Max.X, (i + 0.5) / 17.0), FMath::Lerp(Box.Min.Y, Box.Max.Y, (j + 0.5) / 17.0));
					if (Inf->Live.ContainsXZ(S, G.X, G.Y) && FVector2D::DistSquared(G, C) < Near2) { Near2 = FVector2D::DistSquared(G, C); Pick = G; }
				}
			}
			if (Near2 > 1e29) continue;
			C = Pick;
		}
		const FVector P = FOutlawsGeometry::ToUE(C.X, S.Floor.Y, C.Y);
		const double D = FVector::DistSquared(P, Near);
		if (D < Best) { Best = D; Out = P; }
	}
	return Best < 1e29;
}

FVector AOutlawsLevelActor::FloorVelocityAt(const FVector& P) const
{
	if (!Inf) return FVector::ZeroVector;
	const FOutlawsSector* S = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(P, 0.2));
	if (!S || S->Velocity.IsNearlyZero()) return FVector::ZeroVector;
	return FOutlawsGeometry::ToUE(S->Velocity.X, (S->Flags & OutlawsFlags::VelFloorOnly) ? 0.0 : S->Velocity.Y, S->Velocity.Z);
}

void AOutlawsLevelActor::CarryBySectors(float Dt)
{
	OL_PERF_SCOPE("Level: carry by sectors");
	if (!Inf) return;
	// the sectors that carry things or take them away right now (the INF can set a floor moving mid-level), as boxes
	// on the map: a thing outside all of them stands on one that does neither, so only one inside a box needs the
	// sector lookup; none at all and there is nothing to do
	TArray<FBox2D, TInlineAllocator<16>> CarrierBoxes;
	for (const FOutlawsSector& Sec : Inf->Live.Sectors)
	{
		// (and the deadly ones: the bad guys die on them as the player does, BUG-TRAINFALL)
		if (Sec.Velocity.IsNearlyZero() && (Sec.Flags & (OutlawsFlags::SecTerminate | OutlawsFlags::SecDamageDie | OutlawsFlags::FloorDamageDie)) == 0) continue;
		FBox2D B(ForceInit);
		for (const FVector2D& V : Sec.Verts) B += V;       // (x, z), as SectorAt takes them
		if (B.bIsValid) CarrierBoxes.Add(B.ExpandBy(0.5));
	}
	if (CarrierBoxes.Num() == 0)
	{
		Carries.Reset();
		return;
	}
	// the things carried: generated scenery and pickups (items: a terminate sector takes them), the bad guys
	// (BUG-STUTTER2 item 6: 2.9 ms a frame on the train on the Quest, its ground's rocks and shrubs. Each look-up now passes
	// over a sector whose plan box misses the thing before walking its rings, and a rock carried along out of sight is
	// moved when it is next drawn or looked up, not every frame: the same places whenever anything sees or asks)
	struct FThing { AActor* A; bool bItem; AOutlawsSpriteActor* Scenery; };
	TArray<FThing, TInlineAllocator<64>> Things;
	for (FItemGen& G : ItemGens) for (const TWeakObjectPtr<AActor>& W : G.Alive) if (AActor* A = W.Get()) Things.Add({ A, true, Cast<AOutlawsSpriteActor>(A) });
	for (FPickup& P : Pickups) if (P.Actor.IsValid() && !P.bHeld) Things.Add({ P.Actor.Get(), true, nullptr });
	for (const TWeakObjectPtr<AOutlawsEnemy>& E : Enemies) if (E.IsValid()) Things.Add({ static_cast<AActor*>(E.Get()), false, nullptr });
	const double U = FOutlawsGeometry::UnitsToCm;
	// every sector's plan box as its vertices are this frame (the INF moves some), made once the first look-up wants them
	SectorBoxes.Reset();
	for (const FThing& T : Things)
	{
		AActor* A = T.A;
		FCarry& C = Carries.FindOrAdd(A);
		C.Check -= Dt;
		if (C.Check <= 0.f)
		{
			C.Check = 0.1f + FMath::FRandRange(0.f, 0.05f);
			if (T.Scenery) T.Scenery->FlushHeldMove();
			FVector Feet = A->GetActorLocation();
			if (const ACharacter* Ch = Cast<ACharacter>(A)) Feet.Z -= Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const FVector2D At = FOutlawsGeometry::ToOutlawsPlan(Feet);
			bool bNearCarrier = false;
			for (const FBox2D& B : CarrierBoxes)
			{
				if (B.IsInside(At)) { bNearCarrier = true; break; }
			}
			if (bNearCarrier && SectorBoxes.Num() != Inf->Live.Sectors.Num())
			{
				SectorBoxes.SetNumUninitialized(Inf->Live.Sectors.Num());
				for (int32 i = 0; i < Inf->Live.Sectors.Num(); ++i)
				{
					FBox2D B(ForceInit);
					for (const FVector2D& V : Inf->Live.Sectors[i].Verts) B += V;
					SectorBoxes[i] = B;
				}
			}
			const FOutlawsSector* S = bNearCarrier ? Inf->Live.SectorAtBoxed(FOutlawsGeometry::ToOutlaws(Feet, 0.2), SectorBoxes) : nullptr;
			C.Vel = FVector::ZeroVector;
			C.bTerminate = false;
			if (S)
			{
				C.bTerminate = (S->Flags & OutlawsFlags::SecTerminate) != 0;
				const bool bFloorOnly = (S->Flags & OutlawsFlags::VelFloorOnly) != 0;
				if (!S->Velocity.IsNearlyZero() && (!bFloorOnly || Feet.Z / U - S->Floor.Y < 0.6))
				{
					C.Vel = FOutlawsGeometry::ToUE(S->Velocity.X, bFloorOnly ? 0.0 : S->Velocity.Y, S->Velocity.Z);
				}
				// (user 2026-10-05, BUG-TRAINFALL) the player's deadly-sector rule for the bad guys too: SEC_ DIE anywhere in it,
				// FLR_ DIE standing on its floor (the train's ground: off the train is death); the bodies already are
				const uint32 F = S->Flags;
				if (!T.bItem && !bFloorKillsOff && ((F & OutlawsFlags::SecDamageDie) || ((F & OutlawsFlags::FloorDamageDie) && Feet.Z / U - S->Floor.Y < 0.6)))
				{
					if (AOutlawsEnemy* E = Cast<AOutlawsEnemy>(A)) E->FloorKill(Feet);
				}
			}
		}
		if (T.bItem && C.bTerminate)
		{
			static int32 Gone = 0;
			if (Gone++ < 3) UE_LOG(LogOutlaws, Log, TEXT("OLRT %s carried off the map (terminate sector) at %s"), *A->GetName(), *A->GetActorLocation().ToString());
			A->Destroy();
			continue;
		}
		// everything at the floor's own speed, the bad guys and their bodies too (user 2026-10-04, BUG-TRAINBODY: the bodies on the
		// train's ground fell behind its rocks and shrubs, carried at the bad guys' pace; a sector's carry is the world's own
		// motion, never paced, on every level)
		if (C.Vel.IsNearlyZero()) continue;
		if (T.Scenery && T.Scenery->DebugCulled()) T.Scenery->HoldMove(C.Vel * Dt);
		else A->AddActorWorldOffset(C.Vel * Dt, !T.bItem);
	}
	for (auto It = Carries.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
}

bool AOutlawsLevelActor::WaterHueAt(const FVector& P, FLinearColor& OutHue) const
{
	// the sector the point is in, as the faces are grouped: by the sector, never by a test of position
	if (!Inf) return false;
	const FOutlawsSector* S = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(P));
	const int32* C = S ? FOutlawsGeometry::SectorWaterGroup.Find(S->Index) : nullptr;
	if (!C || !WaterChainHue.IsValidIndex(*C)) return false;
	OutHue = WaterChainHue[*C];
	return true;
}

void AOutlawsLevelActor::BuildWaterTints(const FOutlawsLevel& L, const FOutlawsAssets& A, const TArray<FColor>& Palette)
{
	WaterTints.Reset();
	FOutlawsGeometry::SectorWaterGroup.Reset();
	WaterChainHue.Reset();
	WaterChainSurface.Reset();
	TMap<FString, FLinearColor> TexAverage;           // stem -> average colour (linear), per texture
	auto Average = [&](int32 TexIdx, FLinearColor& Out) -> bool
	{
		const FString Stem = L.TexStem(TexIdx);
		if (Stem.IsEmpty()) return false;
		if (const FLinearColor* Known = TexAverage.Find(Stem)) { Out = *Known; return true; }
		TArray<uint8> Bytes;
		OutlawsFormats::FPcxImage Img;
		const FString File = OutlawsFormats::ResolveTextureName(A, L.Textures[TexIdx]);
		if (File.IsEmpty() || !A.Get(File, Bytes) || !OutlawsFormats::DecodePcx(Bytes, Img) || Img.Indices.Num() == 0) return false;
		const TArray<FColor>& Pal = Palette.Num() == 256 ? Palette : Img.Palette;
		if (Pal.Num() < 256) return false;
		// the see-through pixels (palette index 0) and the key colour (magenta) are not the water's colour: cliff's
		// surfaces averaged pure magenta and turned everything under them purple (user 2026-09-28)
		FLinearColor Sum = FLinearColor::Black;
		int32 Used = 0;
		for (uint8 I : Img.Indices)
		{
			const FColor& P = Pal[I];
			if (I == 0 || (P.R > 200 && P.G < 60 && P.B > 200) || (P.R == 0x00 && P.G == 0x6F && P.B == 0x7F)) continue;
			Sum += FLinearColor::FromSRGBColor(P);
			++Used;
		}
		if (Used * 10 < Img.Indices.Num()) return false;          // (next to nothing left: no colour of its own)
		Out = Sum / float(Used);
		Out.A = 1.f;
		TexAverage.Add(Stem, Out);
		return true;
	};
	TSet<int32> Seen;
	int32 Chains = 0;
	for (const FOutlawsSector& Start : L.Sectors)
	{
		if (!(Start.Flags & OutlawsFlags::Water) || Seen.Contains(Start.Index)) continue;
		// the chain: underwater sectors reached through wall adjoins
		TArray<int32> Chain, Open;
		Open.Add(Start.Index);
		Seen.Add(Start.Index);
		while (Open.Num() > 0)
		{
			const int32 s = Open.Pop();
			Chain.Add(s);
			for (const FOutlawsWall& W : L.Sectors[s].Walls)
			{
				for (int32 N : { W.Adjoin, W.DAdjoin })
				{
					if (L.Sectors.IsValidIndex(N) && (L.Sectors[N].Flags & OutlawsFlags::Water) && !Seen.Contains(N))
					{
						Seen.Add(N);
						Open.Add(N);
					}
				}
			}
		}
		// its surfaces' colours, each distinct texture once
		TSet<int32> Surfaces;
		for (int32 s : Chain)
		{
			const FOutlawsSector& S = L.Sectors[s];
			if (S.VAdjoin >= 0 && S.Ceil.Tex >= 0) Surfaces.Add(S.Ceil.Tex);
		}
		FLinearColor Sum = FLinearColor::Black;
		int32 N = 0;
		for (int32 T : Surfaces)
		{
			FLinearColor C;
			if (Average(T, C)) { Sum += C; ++N; }
		}
		const FLinearColor Tint = N > 0 ? Sum / float(N) : FLinearColor(0.10f, 0.22f, 0.28f);   // (no surface over it: a murky green-blue)
		FLinearColor Opaque = Tint;
		Opaque.A = 1.f;
		for (int32 s : Chain) WaterTints.Add(s, Opaque);
		// the chain for the geometry under its surface: its colour's hue and its (highest) surface; each sector a box
		// its hue a good deal more saturated than the texture's average (a pale blue-grey surface still reads as blue
		// water under it), brightest channel 1
		const float Mean = (Opaque.R + Opaque.G + Opaque.B) / 3.f;
		FLinearColor Sat(FMath::Max(0.02f, Mean + (Opaque.R - Mean) * 1.8f), FMath::Max(0.02f, Mean + (Opaque.G - Mean) * 1.8f),
			FMath::Max(0.02f, Mean + (Opaque.B - Mean) * 1.8f), 1.f);
		const float Top = FMath::Max3(Sat.R, Sat.G, Sat.B);
		const int32 ChainIndex = WaterChainHue.Num();
		// and a third of the way to a water blue, so no pool reads violet (cliff's surface averages red as high as green)
		const FLinearColor Own = Top > 1e-3f ? FLinearColor(Sat.R / Top, Sat.G / Top, Sat.B / Top, 1.f) : FLinearColor::White;
		FLinearColor Hue = FMath::Lerp(Own, FLinearColor(0.2f, 0.55f, 1.f, 1.f), 0.35f);
		const float HueTop = FMath::Max3(Hue.R, Hue.G, Hue.B);
		Hue = FLinearColor(Hue.R / HueTop, Hue.G / HueTop, Hue.B / HueTop, 1.f);
		WaterChainHue.Add(Hue);
		double Surface = -TNumericLimits<double>::Max();
		for (int32 s : Chain)
		{
			Surface = FMath::Max(Surface, L.Sectors[s].Ceil.Y * FOutlawsGeometry::UnitsToCm);
			FOutlawsGeometry::SectorWaterGroup.Add(L.Sectors[s].Index, ChainIndex);
		}
		WaterChainSurface.Add(Surface);
		++Chains;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT underwater chain of %d sectors, %d surface textures: tint %s"), Chain.Num(), N, *Tint.ToString());
	}
}

void AOutlawsLevelActor::BlowBubbles(const FVector& At)
{
	const FOutlawsSector* S = Inf ? Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(At)) : nullptr;
	const int32 Light = S ? FOutlawsLevel::LightLevel(S->Ambient) : 31;
	const int32 Count = FMath::RandRange(1, 3);
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector P = At + FVector(FMath::FRandRange(-4.f, 4.f), FMath::FRandRange(-4.f, 4.f), -i * 6.f);
		// a sphere, clear with a white rim and a highlight (user 2026-09-28: bubble2's clear middle is a key colour the
		// sprite drew red)
		if (!BubbleMaterial) BubbleMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Bubble.M_OLR_Bubble"));
		UStaticMesh* Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		if (BubbleMaterial && Ball && GetWorld())
		{
			FActorSpawnParameters SP;
			SP.Owner = this;
			SP.ObjectFlags |= RF_Transient;
			if (AActor* B = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform(P), SP))
			{
				UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(B);
				M->SetStaticMesh(Ball);
				M->SetMaterial(0, BubbleMaterial);
				M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				M->SetCastShadow(false);
				B->SetRootComponent(M);
				M->RegisterComponent();
				M->SetWorldLocation(P);
				// 8 to 24 cm across (the sphere is a metre): today's size the least, up to twice it (user 2026-09-28)
				M->SetWorldScale3D(FVector(FMath::FRandRange(0.08f, 0.24f)));
				SpawnedActors.Add(B);
				FBubble& Bb = Bubbles.AddDefaulted_GetRef();
				Bb.Actor = B;
				Bb.Phase = FMath::FRandRange(0.f, 6.28f);
			}
			continue;
		}
		if (AOutlawsSpriteActor* B = SpawnSceneSprite(TEXT("bubble"), TEXT("bubble1.nwx"), P, 0.f, Light))
		{
			B->SetActorEnableCollision(false);
			FBubble& Bb = Bubbles.AddDefaulted_GetRef();
			Bb.Actor = B;
			Bb.Phase = FMath::FRandRange(0.f, 6.28f);
		}
	}
	static const TCHAR* Takes[] = { TEXT("bubbles1"), TEXT("bubbles2"), TEXT("bubbles3"), TEXT("bubbles4"), TEXT("bubbles5") };
	PlaySoundAt(Takes[FMath::RandRange(0, 4)], At);
}

void AOutlawsLevelActor::UpdateBubbles(float Dt)
{
	OL_PERF_SCOPE("Level: bubbles");
	const double U = FOutlawsGeometry::UnitsToCm;
	for (int32 i = Bubbles.Num() - 1; i >= 0; --i)
	{
		FBubble& B = Bubbles[i];
		AActor* A = B.Actor.Get();
		if (!A || !Inf)
		{
			Bubbles.RemoveAtSwap(i);
			continue;
		}
		B.Age += Dt;
		FVector P = A->GetActorLocation();
		const FOutlawsSector* S = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(P));
		// out of the water (at the surface): gone; a lifetime cap as a second failsafe
		if (!S || !(S->Flags & OutlawsFlags::Water) || B.Age > 30.f)
		{
			A->Destroy();
			Bubbles.RemoveAtSwap(i);
			continue;
		}
		// rise with a little wobble; under a solid ceiling (no vertical adjoin above) it stays, and after 10 s
		// it is gone
		const double Ceil = Inf->Live.SurfaceY(*S, FOutlawsGeometry::ToOutlawsPlan(P), false) * U - 3.0;
		// the bigger ones rise a little faster, more air in them (user 2026-09-28): 77 cm/s at 8 cm across to 113 at 24,
		// twice the first speeds (user 2026-09-29) (a sphere's scale is its size in metres; a sprite bubble rises at the
		// middle speed)
		const double Size = A->GetActorScale3D().X < 0.5 ? A->GetActorScale3D().X : 0.12;
		const double Rise = 90.0 * FMath::Lerp(0.85, 1.25, FMath::Clamp((Size - 0.08) / 0.16, 0.0, 1.0));
		FVector Next = P + FVector(FMath::Sin(B.Age * 3.f + B.Phase) * 6.f * Dt, FMath::Cos(B.Age * 2.3f + B.Phase) * 6.f * Dt, Rise * Dt);
		if (Next.Z >= Ceil && S->VAdjoin < 0)
		{
			Next.Z = Ceil;
			B.Stuck += Dt;
			if (B.Stuck > 10.f)
			{
				A->Destroy();
				Bubbles.RemoveAtSwap(i);
				continue;
			}
		}
		A->SetActorLocation(Next);
	}
}

bool AOutlawsLevelActor::UnderwaterTintAt(const FVector& Eye, FLinearColor& OutTint) const
{
	if (!Inf || WaterTints.Num() == 0) return false;
	const FOutlawsSector* S = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(Eye));
	const FLinearColor* T = S ? WaterTints.Find(S->Index) : nullptr;
	if (!T) return false;
	OutTint = *T;
	return true;
}

FString AOutlawsLevelActor::DebugSectorPaletteAt(const FVector& At) const
{
	if (!LevelData) return TEXT("no level");
	const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(At));
	if (!S) return FString::Printf(TEXT("no sector (main palette %d)"), MainPalette);
	return FString::Printf(TEXT("sector %d palette %d (main %d)%s"), S->Index, S->Palette, MainPalette, WaterTints.Contains(S->Index) ? TEXT(" water") : TEXT(""));
}

int32 AOutlawsLevelActor::DebugWaterSector() const
{
	if (!Inf) return -1;
	int32 Best = -1;
	double Deep = 0.0;
	for (const TPair<int32, FLinearColor>& Kv : WaterTints)
	{
		const FOutlawsSector* S = Inf->Live.Sector(Kv.Key);
		if (S && S->Ceil.Y - S->Floor.Y > Deep) { Deep = S->Ceil.Y - S->Floor.Y; Best = Kv.Key; }
	}
	return Best;
}

bool AOutlawsLevelActor::FloorUnder(const FVector& Feet, float& OutFriction, bool& bOutSlides, FVector& OutDownhill) const
{
	if (!Inf) return false;
	const FOutlawsLevel& Lv = Inf->Live;
	const FVector Ol = FOutlawsGeometry::ToOutlaws(Feet, 0.2);
	const double X = Ol.X, Z = Ol.Z;
	const FOutlawsSector* S = Lv.SectorAt(Ol);
	if (!S) return false;
	OutFriction = S->Friction;
	bOutSlides = (S->Flags & OutlawsFlags::SlopedFloor) && !(S->Flags & OutlawsFlags::NoSlip);
	// the floor's gradient about the feet (units of height per unit across), downhill the other way
	const double E = 0.5;
	const double Gx = (Lv.SurfaceY(*S, FVector2D(X + E, Z), true) - Lv.SurfaceY(*S, FVector2D(X - E, Z), true)) / (2.0 * E);
	const double Gz = (Lv.SurfaceY(*S, FVector2D(X, Z + E), true) - Lv.SurfaceY(*S, FVector2D(X, Z - E), true)) / (2.0 * E);
	OutDownhill = FVector(-Gz, -Gx, 0.0);          // UE X = Outlaws z, UE Y = Outlaws x
	return true;
}

bool AOutlawsLevelActor::SlopeFoot(int32 Sector, FVector& OutFeet, float& OutYaw) const
{
	if (!Inf || !Inf->Live.Sectors.IsValidIndex(Sector)) return false;
	const FOutlawsLevel& Lv = Inf->Live;
	const FOutlawsSector& S = Lv.Sectors[Sector];
	if (S.Verts.Num() < 3) return false;
	FVector2D C = FVector2D::ZeroVector;
	int32 Lo = 0, Hi = 0;
	for (int32 i = 0; i < S.Verts.Num(); ++i)
	{
		C += S.Verts[i];
		if (Lv.SurfaceY(S, S.Verts[i], true) < Lv.SurfaceY(S, S.Verts[Lo], true)) Lo = i;
		if (Lv.SurfaceY(S, S.Verts[i], true) > Lv.SurfaceY(S, S.Verts[Hi], true)) Hi = i;
	}
	C /= S.Verts.Num();
	const FVector2D Start = FMath::Lerp(S.Verts[Lo], C, 0.3);
	OutFeet = FOutlawsGeometry::ToUE(Start.X, Lv.SurfaceY(S, Start, true) + 0.2, Start.Y);
	const FVector Up = FOutlawsGeometry::ToUE(S.Verts[Hi].X, 0.0, S.Verts[Hi].Y) - FOutlawsGeometry::ToUE(Start.X, 0.0, Start.Y);
	OutYaw = Up.Rotation().Yaw;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT walk test: sector %d floor %.2f..%.2f, from %s yaw %.0f"), Sector, Lv.SurfaceY(S, S.Verts[Lo], true), Lv.SurfaceY(S, S.Verts[Hi], true), *OutFeet.ToString(), OutYaw);
	return true;
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::DebugMovingFloors(TArray<int32>& OutSectors, TArray<FString>& OutNames, TArray<FVector>& OutTops) const
{
	// (QSPEED-3, -OLLiftRideTest) each lift floor's first sector (MOVE_FLOOR, MOVE_FC) that moves by itself (a stop that
	// waits, so it goes on round) or is moving now, with a spot near its middle at the floor's height now
	if (!Inf) return;
	for (const FOutlawsElevator& E : Inf->Elevators)
	{
		if ((E.Class != EOutlawsInfClass::MoveFloor && E.Class != EOutlawsInfClass::MoveFC) || E.Sectors.Num() == 0 || E.bTerminated || !E.bMasterOn) continue;
		if (E.Target < 0 && !E.bWaiting) continue;
		const FOutlawsSector& S = Inf->Live.Sectors[E.Sectors[0]];
		if (S.Verts.Num() < 3) continue;
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		C /= S.Verts.Num();
		if (!Inf->Live.ContainsXZ(S, C.X, C.Y)) continue;
		OutSectors.Add(S.Index);
		OutNames.Add(E.Name);
		OutTops.Add(FOutlawsGeometry::ToUE(C.X, Inf->Live.SurfaceY(S, C, true), C.Y));
	}
}

double AOutlawsLevelActor::DebugFloorZ(int32 Sector, const FVector& At) const
{
	if (!Inf || !Inf->Live.Sectors.IsValidIndex(Sector)) return 0.0;
	return Inf->Live.SurfaceY(Inf->Live.Sectors[Sector], FOutlawsGeometry::ToOutlawsPlan(At), true) * FOutlawsGeometry::UnitsToCm;
}
#endif

FString AOutlawsLevelActor::DescribeSectorsAt(const FVector& UE) const
{
	if (!Inf) return TEXT("no INF");
	const FOutlawsLevel& Lv = Inf->Live;
	const FVector Ol = FOutlawsGeometry::ToOutlaws(UE);
	FString Out = FString::Printf(TEXT("at Outlaws (%.2f, %.2f, %.2f):"), Ol.X, Ol.Y, Ol.Z);
	for (double dy : { -0.3, 0.3 })
	{
		const FOutlawsSector* S = Lv.SectorAt(FOutlawsGeometry::ToOutlaws(UE, dy));
		if (!S) { Out += FString::Printf(TEXT(" [%+.1f: none]"), dy); continue; }
		Out += FString::Printf(TEXT(" [%+.1f: sector %d %s layer %d floor %.2f %s ceil %.2f %s flags 0x%x]"), dy, S->Index, *S->Name, S->Layer,
			S->Floor.Y, *Lv.TexStem(S->Floor.Tex), S->Ceil.Y, *Lv.TexStem(S->Ceil.Tex), S->Flags);
	}
	return Out;
}

void AOutlawsLevelActor::DebugDig(int32 Step)
{
	if (!Inf) return;
	TArray<int32> Secs;
	Inf->FindSectors(TEXT("DIG_ME"), FString(), Secs);
	if (Secs.Num() == 0) { UE_LOG(LogOutlaws, Warning, TEXT("OLRT dig test: no DIG_ME here")); return; }
	const FOutlawsSector& S = Inf->Live.Sectors[Secs[0]];
	FVector2D C = FVector2D::ZeroVector;
	for (const FVector2D& V : S.Verts) C += V;
	C /= FMath::Max(1, S.Verts.Num());
	if (Step == 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT dig test: not holding the shovel: %s"), ToolStabFloor(FOutlawsGeometry::ToUE(C.X, S.Floor.Y, C.Y)) ? TEXT("dug (wrong)") : TEXT("nothing (right)"));
		AddCount(TEXT("ishovel"), 1);
		SetToolHeld(UseIdOf(TEXT("ishovel")), true);
	}
	const TArray<int32>* Es = Inf->ElevatorsBySector.Find(S.Index);
	const double Cur = Es && Es->Num() > 0 ? Inf->Elevators[(*Es)[0]].Current : 0.0;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT dig test step %d: floor %.2f (elevator value %.2f)"), Step, S.Floor.Y, Cur);
	ToolStabFloor(FOutlawsGeometry::ToUE(C.X, S.Floor.Y, C.Y));
}

int32 AOutlawsLevelActor::CheatJackpot()
{
	if (!Inf) return 0;
	TSet<int32> Ids;
	Inf->RequiredObjects(Ids);
	int32 Given = 0;
	for (const int32 Id : Ids)
	{
		if (Inf->Items.Contains(Id)) continue;
		for (const TPair<FString, FItemDef>& Kv : ItemDefs)
		{
			if (Kv.Value.UseId != Id) continue;
			if (!AddCount(Kv.Key, 1)) break;
			++Given;
			// (BUG-JACKPOT) onto the wrist list the way a pickup puts it there: the label its pickup in the level carries
			// (PICKUP_MSG), else the item's own name
			FString Label;
			for (const FPickup& P : Pickups)
			{
				for (const FPickup::FEntry& E : P.Entries)
				{
					if (!E.Item.Equals(Kv.Key, ESearchCase::IgnoreCase)) continue;
					if (const FString* Text = UserMessages.Find(E.Message)) Label = *Text;
					break;
				}
				if (!Label.IsEmpty()) break;
			}
			if (Label.IsEmpty())
			{
				// (none placed: a bad guy carries it) the ITM name made readable: iironkey -> Iron Key
				FString Base = Kv.Key.StartsWith(TEXT("i")) ? Kv.Key.Mid(1) : Kv.Key;
				const bool bKey = Base.EndsWith(TEXT("key")) && Base.Len() > 3;
				if (bKey) Base.LeftChopInline(3);
				Label = Base.Left(1).ToUpper() + Base.Mid(1) + (bKey ? TEXT(" Key") : TEXT(""));
			}
			Inventory.Add(Kv.Key.ToLower());
			InventoryLabels.Add(Label);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT jackpot: %s (item %d) listed as \"%s\""), *Kv.Key, Id, *Label);
			break;
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT jackpot: %d of the %d items the level's locks ask for given"), Given, Ids.Num());
	return Given;
}

void AOutlawsLevelActor::GiveItem(int32 UseId)
{
	if (Inf) Inf->Items.Add(UseId);
}

#if WITH_EDITOR
void AOutlawsLevelActor::TakeItem(int32 UseId)
{
	if (Inf) Inf->Items.Remove(UseId);
}
#endif

bool AOutlawsLevelActor::HasItem(int32 UseId) const
{
	return Inf && Inf->HasItem(UseId);
}

void AOutlawsLevelActor::GiveAllKeys()
{
	if (!Inf) return;
	TSet<int32> Ids;
	Inf->RequiredObjects(Ids);
	Inf->Items.Append(Ids);
	TArray<FString> Names;
	for (int32 Id : Ids)
	{
		const FItemInfo* Info = ItemsByUseId.Find(Id);
		Names.Add(FString::Printf(TEXT("%d %s"), Id, Info ? *Info->Name : TEXT("?")));
	}
	Names.Sort();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT gave %d items: %s"), Ids.Num(), *FString::Join(Names, TEXT(", ")));
	Notify(FString::Printf(TEXT("%d lock items given"), Ids.Num()), 4.f);
}

void AOutlawsLevelActor::KeyItemSpots(TArray<FVector>& Out) const
{
	Out.Reset();
	if (!Inf) return;
	TSet<int32> Ids;
	Inf->RequiredObjects(Ids);
	for (const FPickup& P : Pickups)
	{
		const AActor* A = P.Actor.Get();
		if (!A || P.bHeld || P.UseId < 0 || !Ids.Contains(P.UseId) || Inf->Items.Contains(P.UseId)) continue;
		if (P.bMapSeen) continue;          // (REQ-MAPITEMS: seen, its own picture is on the map where it lies)
		Out.Add(A->GetActorLocation());
	}
}

#if WITH_EDITOR
void AOutlawsLevelActor::NudgeSector(const FString& Name, bool bFromOutside)
{
	if (!Inf) return;
	TArray<int32> Secs;
	Inf->FindSectors(Name, FString(), Secs);
	for (int32 s : Secs)
	{
		Inf->SectorEvent(s, bFromOutside ? OutlawsInf::EvNudgeBack : OutlawsInf::EvNudgeFront);
	}
}
#endif

namespace
{
	void OutlawsGiveKeys(UWorld* World)
	{
		if (!World) return;
		for (TActorIterator<AOutlawsLevelActor> It(World); It; ++It)
		{
			It->GiveAllKeys();
		}
	}
	FAutoConsoleCommandWithWorld GOutlawsGiveKeys(TEXT("Outlaws.GiveKeys"),
		TEXT("Give the player every item this level's locks need (keys, crowbar, badges)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&OutlawsGiveKeys));

	void OutlawsHurt(const TArray<FString>& Args, UWorld* World)
	{
		if (!World) return;
		const float D = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 1.f;
		for (TActorIterator<AOutlawsLevelActor> It(World); It; ++It)
		{
			It->DamagePlayer(D);
		}
	}
	FAutoConsoleCommandWithWorldAndArgs GOutlawsHurt(TEXT("Outlaws.Hurt"),
		TEXT("Hurt the player by n points (health is 0..10; armour absorbs first)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OutlawsHurt));

	void OutlawsGive(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 1) return;
		const int32 N = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 1;
		for (TActorIterator<AOutlawsLevelActor> It(World); It; ++It)
		{
			const bool bOk = It->AddCount(Args[0], N);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT give %s x%d: %s, now %d"), *Args[0], N, bOk ? TEXT("ok") : TEXT("refused"), It->GetCount(Args[0]));
		}
	}
	FAutoConsoleCommandWithWorldAndArgs GOutlawsGive(TEXT("Outlaws.Give"),
		TEXT("Give the player an inventory item: Outlaws.Give ibullit 12 (ibplate 150 = heavy plate, idocbag 3)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&OutlawsGive));
}

// (TIDY-8) the headless INF test's calls, editor builds only (OutlawsLevelActor.h says why WITH_EDITOR)
#if WITH_EDITOR
void AOutlawsLevelActor::SendInfMessage(const FString& Target, const FString& Message, int32 Arg)
{
	if (!Inf) return;
	FOutlawsInfMessage M;
	const FString U = Message.ToUpper();
	M.Type = U == TEXT("PREV_STOP") ? EOutlawsInfMsg::PrevStop : U == TEXT("GOTO_STOP") ? EOutlawsInfMsg::GotoStop :
		U == TEXT("MASTER_ON") ? EOutlawsInfMsg::MasterOn : U == TEXT("MASTER_OFF") ? EOutlawsInfMsg::MasterOff :
		(U == TEXT("TRIGGER") || U == TEXT("M_TRIGGER")) ? EOutlawsInfMsg::Trigger : EOutlawsInfMsg::NextStop;
	M.Arg = Arg;
	FString N, Id;
	FOutlawsInfFile::SplitNameId(Target, N, Id);
	Inf->Send(N, Id, M);
}

void AOutlawsLevelActor::AdvanceInf(float Seconds, float Step)
{
	if (!Inf) return;
	Step = FMath::Max(0.001f, Step);
	for (float T = 0.f; T < Seconds; T += Step)
	{
		TickInf(Step);
	}
}

FString AOutlawsLevelActor::ElevatorStates(const FString& NameFilter) const
{
	if (!Inf) return TEXT("no INF");
	TArray<FString> Lines;
	for (const FOutlawsElevator& E : Inf->Elevators)
	{
		if (!NameFilter.IsEmpty() && !E.Name.Contains(NameFilter)) continue;
		const TCHAR* State = E.bTerminated ? TEXT("terminated") : E.Target >= 0 ? TEXT("moving") : E.bWaiting ? TEXT("waiting") : TEXT("idle");
		Lines.Add(FString::Printf(TEXT("%s [%s]: stop %d, value %.2f, %s%s"), *E.Name, *Inf->Inf.Items[E.Item].Classes[E.ClassIndex].RawClass,
			E.Stop, E.Current, State, E.bMasterOn ? TEXT("") : TEXT(", master off")));
	}
	return FString::Join(Lines, TEXT("\n"));
}

FTransform AOutlawsLevelActor::ElevatorMeshTransform(const FString& Name) const
{
	if (!Inf) return FTransform::Identity;
	for (const FOutlawsElevator& E : Inf->Elevators)
	{
		if (E.Name.Equals(Name, ESearchCase::IgnoreCase) && Groups.IsValidIndex(E.Group))
		{
			const FMoverGroup& G = Groups[E.Group];
			const UProceduralMeshComponent* M = G.bMorph ? G.Main :
				(E.Class == EOutlawsInfClass::MoveCeiling ? (G.Ceil ? G.Ceil : G.Floor) : (G.Floor ? G.Floor : G.Ceil));
			if (M) return M->GetComponentTransform();
		}
	}
	return FTransform::Identity;
}
#endif // WITH_EDITOR

int32 AOutlawsLevelActor::SecretSectorCount() const
{
	int32 N = 0;
	if (Inf) for (const FOutlawsSector& S : Inf->Live.Sectors) N += (S.Flags & OutlawsFlags::SecSecretTag) ? 1 : 0;
	return N;
}

#if WITH_OUTLAWS_TESTS
int32 AOutlawsLevelActor::TestSectorCount() const { return Inf ? Inf->Live.Sectors.Num() : 0; }
uint32 AOutlawsLevelActor::TestSectorFlags(int32 Sector) const { return Inf && Inf->Live.Sectors.IsValidIndex(Sector) ? Inf->Live.Sectors[Sector].Flags : 0u; }
#endif // WITH_OUTLAWS_TESTS
