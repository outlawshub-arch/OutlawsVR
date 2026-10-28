// Historical Missions (HM1, user 2026-10-03; PLANS.md HM1): the original's Marshal Training as sheriff.rca runs it.
//   the crawl: intro.txt rising over hintro.pcx with intro.msc (the Historical Missions button, "Marshal Training");
//   the office: its wanted posters (PORCH1..5, SPAWN_LEVEL <mission> PORCHxD) start the missions with the office's kit;
//   a mission's horse brings the player back, with the outlaw alive, dead or not at all.

#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsAchievements.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsTextures.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Misc/CommandLine.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsInf.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "GameFramework/PlayerController.h"

void AOutlawsLevelActor::StartHom(EOutlawsDifficulty InDifficulty)
{
	Difficulty = InDifficulty;
	bFrontEnd = false;
	bMovieToMenu = false;
	PlayTime = 0.0;
	Counts.Reset();
	Inventory.Reset();
	InventoryLabels.Reset();
	Gold = 0;
	Armour = 0;
	ArmourMax = 0;                           // (BUG-PLATECARRY: no plate at all, not the last game's size)
	Health = 10.f;
	EntryLevel.Reset();
	StopMusic();
	bMenuMusic = false;
	bHom = true;
	bTutorial = false;                       // (REQ-TUTORIAL)
	TutorialRun.Reset();
	BeginRandoRun(TEXT("marshal training"));  // (R10: the missions' bad guys and items; never the office)
	NextLevelName = TEXT("office");          // (sheriff.rca: TEXT intro.txt, then LEVEL office)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT historical missions: Marshal Training (difficulty %d)"), int32(InDifficulty));
	// the crawl's words, its picture and its music
	TArray<FString> Lines;
	FString Text;
	if (AssetsRef && AssetsRef->GetText(TEXT("intro.txt"), Text)) Text.ParseIntoArrayLines(Lines, false);
	UTexture2D* Back = nullptr;
	TArray<uint8> Bytes;
	OutlawsFormats::FPcxImage Img;
	if (AssetsRef && AssetsRef->Get(TEXT("hintro.pcx"), Bytes) && OutlawsFormats::DecodePcx(Bytes, Img) && Img.Palette.Num() == 256 && Img.Indices.Num() == Img.W * Img.H)
	{
		TArray<FColor> Px;
		Px.SetNumUninitialized(Img.W * Img.H);
		for (int32 i = 0; i < Px.Num(); ++i) { Px[i] = Img.Palette[Img.Indices[i]]; Px[i].A = 255; }
		Back = OutlawsTextures::MakeTextureBGRA(Img.W, Img.H, Px, false, TF_Bilinear, TA_Clamp);
	}
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!World || !P || Lines.Num() == 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT historical missions: no intro.txt here: straight to the office"));
		StartNextLevel();
		return;
	}
	Clear();
	P->GetCharacterMovement()->DisableMovement();
	P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!Movie)
	{
		StartNextLevel();
		return;
	}
	Movie->OnFinished.AddUObject(this, &AOutlawsLevelActor::StartNextLevel);
	FadeCamera(1.f, 0.f, 1.5f, false);
	StartMusic(TEXT("intro.msc"));
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	// (REQ-HOMCRAWL) the words in the game's western font, as the remaster draws them: mf3s.laf, the solid face
	OutlawsFormats::FLafFont Font;
	TArray<uint8> FontBytes;
	const bool bFont = AssetsRef && AssetsRef->Get(TEXT("mf3s.laf"), FontBytes) && OutlawsFormats::DecodeLaf(FontBytes, Font);
	Movie->PlayCrawl(Lines, Back, Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector(), 55.f,
		bFont ? &Font : nullptr, bHDTextures);
	SetActorTickEnabled(true);
}

FString AOutlawsLevelActor::HomMissionLabel() const
{
	TArray<FString> Names, Labels;
	GetMissionChoices(Names, Labels);
	const int32 i = Names.IndexOfByPredicate([this](const FString& N) { return N.Equals(HomMission, ESearchCase::IgnoreCase); });
	if (Labels.IsValidIndex(i)) return Labels[i];
	const FString Title = LevelTitle(HomMission);           // (a bonus level: its own title where the game has one)
	return Title.IsEmpty() ? HomMission.ToLower() : Title;
}

void AOutlawsLevelActor::HomSpawnLevel(const FString& Args)
{
	TArray<FString> Parts;
	Args.ParseIntoArrayWS(Parts);
	if (Parts.Num() == 0) return;
	HomMission = Parts[0].ToLower();
	HomReturnSector = Parts.Num() > 1 ? Parts[1].ToUpper() : FString();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: poster for %s (back to %s)%s"), *HomMission, *HomReturnSector, bHom ? TEXT("") : TEXT(" outside a Historical Missions session"));
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	// (HM1-DOORS) a ranked door's end wall (no return sector: GALLERY, SHOOT, INDY) is walked through, not a poster chosen:
	// no "ride out?" page, the bonus level at once, as the original
	if (OLFlags::Param(TEXT("OLHomTest")) || HomReturnSector.IsEmpty() || !P)
	{
		HomRide();                           // (the test rides at once)
		return;
	}
	P->HomAskRide();
}

void AOutlawsLevelActor::HomRide()
{
	if (HomMission.IsEmpty()) return;
	const FString Mission = HomMission, Back = HomReturnSector;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: riding out to %s"), *Mission);
	if (UOutlawsCoop::IsActive(this) && UOutlawsCoop::HomRide(this, Mission, Back)) return;     // (REQ-COOPHM: every player rides)
	StartMission(Mission, Difficulty);       // (the mission's own start kit: office.itm FLAGS 0x40 empties the office's)
	bHom = true;
	HomMission = Mission;
	HomReturnSector = Back;
}

namespace
{
	// (HM1_RESEARCH.md, from the exe) the outlaw each mission hunts (messages 831..835), the bounty (0x1400713b0: 1000 taken
	// alive, 500 dead, nothing escaped), the ranks by the session's game score (Deputy 9300, Sheriff 12400, Marshal 15500:
	// the badge items idep / isher / imarsh, USE_ID 2 / 3 / 4, the office's doors keyed on them)
	struct FHomOutlaw { const TCHAR* Mission; const TCHAR* Name; };
	const FHomOutlaw HomOutlaws[] = { { TEXT("shfort"), TEXT("Spittin' Jack Sanchez") }, { TEXT("shsimms"), TEXT("Buckshot Bill Morgan") },
		{ TEXT("shgranry"), TEXT("Mary") }, { TEXT("shgulch"), TEXT("Dynamite Dick") }, { TEXT("shwildns"), TEXT("Bloodeye Tim") } };
	const TCHAR* HomOutlawName(const FString& Mission)
	{
		for (const FHomOutlaw& O : HomOutlaws) if (Mission.Equals(O.Mission, ESearchCase::IgnoreCase)) return O.Name;
		return TEXT("the outlaw");
	}
	struct FHomRank { int32 Score; const TCHAR* Badge; const TCHAR* Title; };
	const FHomRank HomRanks[] = { { 9300, TEXT("idep"), TEXT("Deputy") }, { 12400, TEXT("isher"), TEXT("Sheriff") }, { 15500, TEXT("imarsh"), TEXT("Marshal") } };
}

void AOutlawsLevelActor::HomMissionEnd()
{
	// the outcome (REQ-HMBOSSEND, user 15 Oct: the original's logic [EXE 0x14008b2e5], replacing the carry to the horse): fixed
	// as the outlaw goes down, brought down by a fist or a knife captured, by anything else killed; the horse is gripped
	// empty-handed, wherever he lies; with him standing, he escaped
	const AOutlawsEnemy* B = HomBoss();
	const int32 Outcome = !B || !B->IsDead() ? 0 : (B->bTakenAlive ? 1 : 2);
	const int32 Bounty = Outcome == 1 ? 1000 : Outcome == 2 ? 500 : 0;
	const int32 MGold = FMath::Max(0, Gold - HomGoldAtStart), MKills = FMath::Max(0, Kills - HomKillsAtStart);
	// the level's score: 100 a gold, 25 a kill, -5 a wound, and the bounty; an escape scores nothing and adds nothing
	const int32 LevelScore = Outcome == 0 ? 0 : FMath::Max(0, 100 * MGold + 25 * MKills - 5 * HomWounds + Bounty);
	const int32 RankBefore = HomGameScore;
	if (Outcome != 0)
	{
		HomTotalGold += MGold;
		HomTotalKills += MKills;
		HomTotalWounds += HomWounds;
		HomTotalBounty += Bounty;
		HomGameScore = FMath::Max(0, 100 * HomTotalGold + 25 * HomTotalKills - 5 * HomTotalWounds + HomTotalBounty);
		if (!HomReturnSector.IsEmpty()) HomPorchEvents.Add(HomReturnSector, Outcome == 1 ? 131072 : 262144);
	}
	// a ranked door's bonus level (GALLERY, SHOOT, INDY: SPAWN_LEVEL with no return sector) hunts no outlaw: back to the
	// office with a word, no result, no card, nothing scored
	if (HomReturnSector.IsEmpty())
	{
		HomResultText = FString::Printf(TEXT("Back from %s."), *HomMissionLabel());
		HomCardRows.Reset();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: bonus level %s ended: back to the office"), *HomMission);
		NextLevelName = TEXT("office");
		NextMovie.Reset();
		return;
	}
	OutlawsAchievements::NoteHomEnd(this, HomMission, Outcome, HomGameScore);       // (REQ-EXTRAS: the ranks, all five killed or captured)
	const TCHAR* Who = HomOutlawName(HomMission);
	HomResultText = Outcome == 1 ? FString::Printf(TEXT("You captured %s!"), Who) : Outcome == 2 ? FString::Printf(TEXT("You killed %s!"), Who)
		: FString::Printf(TEXT("%s escaped."), Who);
	HomResultText += FString::Printf(TEXT("  Gold %d, kills %d, wounds %d, bounty %d: level score %d, game score %d."), MGold, MKills, HomWounds, Bounty, LevelScore, HomGameScore);
	for (const FHomRank& Rk : HomRanks)
	{
		if (RankBefore < Rk.Score && HomGameScore >= Rk.Score) HomResultText += FString::Printf(TEXT("  You're a %s now!"), Rk.Title);
	}
	// the results card (captured.rml: the stamp, Gold, Kills, Wounds, Bounty, Level score, Game score, the award)
	HomCardTitle = Who;
	HomCardRows.Reset();
	HomCardRows.Emplace(TEXT("Result"), Outcome == 1 ? TEXT("CAPTURED") : Outcome == 2 ? TEXT("KILLED") : TEXT("ESCAPED"));
	HomCardRows.Emplace(TEXT("Gold"), FString::FromInt(MGold));
	HomCardRows.Emplace(TEXT("Kills"), FString::FromInt(MKills));
	HomCardRows.Emplace(TEXT("Wounds"), FString::FromInt(HomWounds));
	HomCardRows.Emplace(TEXT("Bounty"), FString::FromInt(Bounty));
	HomCardRows.Emplace(TEXT("Level score"), FString::FromInt(LevelScore));
	HomCardRows.Emplace(TEXT("Game score"), FString::FromInt(HomGameScore));
	{
		FString Award = TEXT("None yet");
		for (const FHomRank& Rk : HomRanks) if (HomGameScore >= Rk.Score) Award = Rk.Title;
		HomCardRows.Emplace(TEXT("Award"), Award);
	}
	{
		UWorld* World = GetWorld();
		const APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: %s ended: %s (boss %s, %.0f m from the player at the horse, held %d): %s"), *HomMission,
			Outcome == 1 ? TEXT("CAPTURED") : Outcome == 2 ? TEXT("KILLED") : TEXT("ESCAPED"), B ? (B->IsDead() ? TEXT("down") : TEXT("standing")) : TEXT("none"),
			B && Pawn ? FVector::Dist(B->GetActorLocation(), Pawn->GetActorLocation()) / 100.0 : -1.0, B && B->bHeldByPlayer ? 1 : 0, *HomResultText);
	}
	NextLevelName = TEXT("office");
	NextMovie.Reset();
}

AOutlawsEnemy* AOutlawsLevelActor::HomBoss() const
{
	if (AOutlawsEnemy* B = Boss.Get()) return B;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies) if (Wk.IsValid() && Wk->bBoss) return Wk.Get();
	return nullptr;
}

int32 AOutlawsLevelActor::HomBossNameId(const AOutlawsEnemy* B)
{
	// [EXE 0x140069e90] the outlaw's index by his FUNC (ShMary_New 1 .. ShBlood_New 5), one ITM each: SH_MARY, SH_BILL,
	// SH_CHEZ, SH_DICK, SH_BLOOD; his name local.msg 830 + the index, 836 for anyone else
	static const TCHAR* Itms[] = { TEXT("sh_mary"), TEXT("sh_bill"), TEXT("sh_chez"), TEXT("sh_dick"), TEXT("sh_blood") };
	const FString N = B ? B->GetOutlawsName().ToLower() : FString();
	for (int32 i = 0; i < UE_ARRAY_COUNT(Itms); ++i) if (N == Itms[i]) return 831 + i;
	return 836;
}

void AOutlawsLevelActor::HomBossDown(int32 Outcome, int32 NameId)
{
	// [EXE 0x14008b2e5..0x14008b5fa] the original's lines as the outlaw goes down: 822 "You captured %s!" or 823 "You killed
	// %s!" with his name, then 830 (the fallbacks are the same English lines, for an install whose local.msg lacks them)
	if (bHomBossDown || (Outcome != 1 && Outcome != 2)) return;
	bHomBossDown = true;
	if (AOutlawsEnemy* B = HomBoss()) B->bTakenAlive = Outcome == 1;       // (a co-op client's copy: the host's call)
	const FString Name = UserMessageOr(NameId, TEXT("the outlaw"));
	const FString Fate = UserMessageOr(Outcome == 1 ? 822 : 823, Outcome == 1 ? TEXT("You captured %s!") : TEXT("You killed %s!")).Replace(TEXT("%s"), *Name);
	const FString Go = UserMessageOr(830, TEXT("Now use your horse, and get while the gettin's good!"));
	Notify(Fate, 6.f);
	Notify(Go, 6.f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: the outlaw is down (%s): \"%s\" then \"%s\""), Outcome == 1 ? TEXT("captured") : TEXT("killed"), *Fate, *Go);
}

void AOutlawsLevelActor::HomOnOffice()
{
	if (!Inf) return;
	// each poster's stamp and its cell's outlaw, from every mission so far (the original's own INF does the rest: the porch
	// sector's stop fires the stamp and the body generator)
	for (const TPair<FString, int32>& Kv : HomPorchEvents)
	{
		TArray<int32> Secs;
		Inf->FindSectors(Kv.Key, FString(), Secs);
		for (int32 s : Secs) Inf->SectorEvent(s, uint32(Kv.Value));
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: office %s gets event %d (%d sectors)"), *Kv.Key, Kv.Value, Secs.Num());
	}
	// the rank: every badge the game score has earned (the doors open on them)
	for (const FHomRank& Rk : HomRanks)
	{
		if (HomGameScore >= Rk.Score && GetCount(Rk.Badge) <= 0) AddCount(Rk.Badge, 1);
	}
	if (!HomResultText.IsEmpty())
	{
		Notify(HomResultText, 10.f);
		HomResultText.Reset();
	}
	HomCardRows.Reset();
	if (!UOutlawsCoop::IsActive(this)) SaveHom();      // (REQ-HMCAREER: a co-op session never writes over the player's own career)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: in the office, game score %d"), HomGameScore);
}

namespace
{
	FString HomSavePath() { return FPaths::GetPath(AOutlawsLevelActor::SavePath(-1)) / TEXT("hom_autosave.json"); }
}

bool AOutlawsLevelActor::HasHomSave()
{
	return FPaths::FileExists(HomSavePath());
}

bool AOutlawsLevelActor::SaveHom() const
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetStringField(TEXT("kind"), TEXT("olue-hom"));
	O->SetNumberField(TEXT("difficulty"), int32(Difficulty));
	O->SetNumberField(TEXT("score"), HomGameScore);
	O->SetNumberField(TEXT("gold"), HomTotalGold);
	O->SetNumberField(TEXT("kills"), HomTotalKills);
	O->SetNumberField(TEXT("wounds"), HomTotalWounds);
	O->SetNumberField(TEXT("bounty"), HomTotalBounty);
	TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Kv : HomPorchEvents) P->SetNumberField(Kv.Key, Kv.Value);
	O->SetObjectField(TEXT("porches"), P);
	FString Text;
	const bool bOk = FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&Text)) && FFileHelper::SaveStringToFile(Text, *HomSavePath());
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: session %s %s (score %d, %d outlaws done)"), bOk ? TEXT("saved to") : TEXT("NOT saved to"), *HomSavePath(), HomGameScore, HomPorchEvents.Num());
	return bOk;
}

void AOutlawsLevelActor::HomResume()
{
	FString Text;
	TSharedPtr<FJsonObject> O;
	if (!FFileHelper::LoadFileToString(Text, *HomSavePath()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) || !O.IsValid())
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: no session to resume: a new one"));
		StartHom(Difficulty);
		return;
	}
	const EOutlawsDifficulty D = static_cast<EOutlawsDifficulty>(FMath::Clamp(int32(O->GetNumberField(TEXT("difficulty"))), 0, 2));
	StartMission(TEXT("office"), D);          // (the office's own start; StartMission clears the session, so it is set after)
	bHom = true;
	HomGameScore = int32(O->GetNumberField(TEXT("score")));
	HomTotalGold = int32(O->GetNumberField(TEXT("gold")));
	HomTotalKills = int32(O->GetNumberField(TEXT("kills")));
	HomTotalWounds = int32(O->GetNumberField(TEXT("wounds")));
	HomTotalBounty = int32(O->GetNumberField(TEXT("bounty")));
	HomPorchEvents.Reset();
	const TSharedPtr<FJsonObject>* P = nullptr;
	if (O->TryGetObjectField(TEXT("porches"), P) && P) for (const auto& Kv : (*P)->Values) HomPorchEvents.Add(Kv.Key, int32(Kv.Value->AsNumber()));
	HomResultText = FString::Printf(TEXT("Welcome back, game score %d."), HomGameScore);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: resuming (score %d, %d outlaws done)"), HomGameScore, HomPorchEvents.Num());
}

// (BUG-RESPAWNCRASH, user 2026-10-11: the Quest's crash was a death respawn in shwildns) the session lives on the level actor
// alone, which a map load of L_Runtime replaces: all of it goes along in the rebuild's JSON, the hom_autosave's fields and the
// mission's own (its name and way back, its wounds and what it began with, the result and card still to show)
void AOutlawsLevelActor::WriteHomCarry(FJsonObject& O) const
{
	const TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
	H->SetStringField(TEXT("mission"), HomMission);
	H->SetStringField(TEXT("return"), HomReturnSector);
	H->SetNumberField(TEXT("score"), HomGameScore);
	H->SetNumberField(TEXT("gold"), HomTotalGold);
	H->SetNumberField(TEXT("kills"), HomTotalKills);
	H->SetNumberField(TEXT("wounds"), HomTotalWounds);
	H->SetNumberField(TEXT("bounty"), HomTotalBounty);
	H->SetNumberField(TEXT("mission_wounds"), HomWounds);
	H->SetNumberField(TEXT("gold_at_start"), HomGoldAtStart);
	H->SetNumberField(TEXT("kills_at_start"), HomKillsAtStart);
	H->SetStringField(TEXT("result"), HomResultText);
	H->SetStringField(TEXT("card_title"), HomCardTitle);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const TPair<FString, FString>& R : HomCardRows)
	{
		Rows.Add(MakeShared<FJsonValueString>(R.Key));
		Rows.Add(MakeShared<FJsonValueString>(R.Value));
	}
	H->SetArrayField(TEXT("card_rows"), Rows);
	const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Kv : HomPorchEvents) P->SetNumberField(Kv.Key, Kv.Value);
	H->SetObjectField(TEXT("porches"), P);
	O.SetObjectField(TEXT("hom"), H);
}

void AOutlawsLevelActor::ReadHomCarry(const FJsonObject& O)
{
	bHom = true;
	HomMission = O.GetStringField(TEXT("mission"));
	HomReturnSector = O.GetStringField(TEXT("return"));
	HomGameScore = int32(O.GetNumberField(TEXT("score")));
	HomTotalGold = int32(O.GetNumberField(TEXT("gold")));
	HomTotalKills = int32(O.GetNumberField(TEXT("kills")));
	HomTotalWounds = int32(O.GetNumberField(TEXT("wounds")));
	HomTotalBounty = int32(O.GetNumberField(TEXT("bounty")));
	HomWounds = int32(O.GetNumberField(TEXT("mission_wounds")));
	HomGoldAtStart = int32(O.GetNumberField(TEXT("gold_at_start")));
	HomKillsAtStart = int32(O.GetNumberField(TEXT("kills_at_start")));
	HomResultText = O.GetStringField(TEXT("result"));
	HomCardTitle = O.GetStringField(TEXT("card_title"));
	HomCardRows.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (O.TryGetArrayField(TEXT("card_rows"), Rows) && Rows)
	{
		for (int32 i = 0; i + 1 < Rows->Num(); i += 2) HomCardRows.Emplace((*Rows)[i]->AsString(), (*Rows)[i + 1]->AsString());
	}
	HomPorchEvents.Reset();
	const TSharedPtr<FJsonObject>* P = nullptr;
	if (O.TryGetObjectField(TEXT("porches"), P) && P) for (const auto& Kv : (*P)->Values) HomPorchEvents.Add(Kv.Key, int32(Kv.Value->AsNumber()));
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hom: the session carried through the map load (mission %s, back to %s, score %d, %d outlaws done)"),
		*HomMission, HomReturnSector.IsEmpty() ? TEXT("none") : *HomReturnSector, HomGameScore, HomPorchEvents.Num());
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickHomTest(float T)
{
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (!World || !Pawn || T < 0.f) return;
	const double Now = World->GetTimeSeconds();
	auto Step = [&](int32 Next, double Wait) { HomTestStep = Next; HomTestAt = Now + Wait; };
	if (Now < HomTestAt) return;
	switch (HomTestStep)
	{
	case 0:
		if (T < 3.f) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: Marshal Training"));
		StartHom(EOutlawsDifficulty::Good);
		Step(1, 4.0);
		return;
	case 1:
		if (Movie && OLFlags::Param(TEXT("OLCrawlShot")))
		{
			// (REQ-HOMCRAWL) the crawl where the remaster's screenshots caught it: THE HEAT OF THIS CONFLICT (line 18) 62% down
			// the picture, then IT WAS A TIME FOR JUSTICE (line 28) 88% down; the canvas and the view saved for each
			Movie->SeekCrawl(18, 0.622f);
			Step(20, 1.0);
			return;
		}
		if (Movie) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: crawl %s, skipped"), Movie->IsCrawl() ? TEXT("showing") : TEXT("not a crawl")); Movie->Skip(); }
		Step(2, 1.0);
		return;
	case 20:
	case 21:
	{
		const bool bEnd = HomTestStep == 21;
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("OLShots");
		const FString Name = bEnd ? TEXT("ol_crawl_end") : TEXT("ol_crawl_mid");
		if (Movie) { if (bEnd) Movie->SeekCrawl(28, 0.876f); else Movie->SeekCrawl(18, 0.622f); }   // (it went on rising while it waited)
		const bool bSaved = Movie && Movie->SaveCrawlPng(Dir / Name + TEXT("_canvas.png"));
		FScreenshotRequest::RequestScreenshot(Dir / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crawl shot %s (canvas %d)"), *Name, bSaved ? 1 : 0);
		if (!bEnd) Step(21, 1.0);
		else { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test done")); FGenericPlatformMisc::RequestExit(false); Step(99, 100.0); }
		return;
	}
	case 2:
		if (!LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) || IsRebuildPending() || !Inf) return;
		{
			TArray<int32> Secs;
			Inf->FindSectors(TEXT("PORCH3D"), FString(), Secs);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: in the office (hom %d), PORCH3D sectors %d"), bHom ? 1 : 0, Secs.Num());
		}
		Step(OLFlags::Param(TEXT("OLHomDoorTest")) ? 100 : 3, 3.0);
		return;
	// (-OLHomDoorTest, HM1-DOORS) each ranked anteroom's end wall nudged as the player's hand does (EVENT_MASK 48): its
	// SPAWN_LEVEL takes the bonus level, which ends back in the office with no card
	case 100:
	case 103:
	case 106:
	{
		static const TCHAR* Doors[3][2] = { { TEXT("DEPUTY"), TEXT("B20E") }, { TEXT("SHERIFF"), TEXT("B1FE") }, { TEXT("MARSHAL"), TEXT("B1E2") } };
		const int32 Di = (HomTestStep - 100) / 3;
		const bool bTaken = DebugNudgeWall(Doors[Di][0], Doors[Di][1]) || DebugNudgeWall(Doors[Di][0], FString(TEXT("#")) + Doors[Di][1]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom door test: %s's end wall nudged (%s)"), Doors[Di][0], bTaken ? TEXT("taken") : TEXT("NOT TAKEN"));
		Step(bTaken ? HomTestStep + 1 : 9, bTaken ? 2.0 : 0.5);
		return;
	}
	case 101:
	case 104:
	case 107:
		if (LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) || IsRebuildPending()) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom door test: in %s (hom %d, title %s)"), *LevelName, bHom ? 1 : 0, *HomMissionLabel());
		CompleteLevel(TEXT("hom door test: the bonus level's end"));
		Step(HomTestStep + 1, 2.5);
		return;
	case 102:
	case 105:
	case 108:
		if (Movie) Movie->Skip();
		if (!LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) || IsRebuildPending()) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom door test: back in the office (%s), card rows %d"), *HomResultText, HomCardRows.Num());
		Step(HomTestStep == 108 ? 9 : HomTestStep + 1, 3.0);
		return;
	// (REQ-HMBOSSEND) -OLHomBoss=<mission> (shfort, shsimms, shgranry, shgulch, shwildns; the Granary by default): its poster,
	// its outlaw punched out (-OLHomKill: shot), the lines logged; -OLHomSaveLoad: a save and its load with him down; then the
	// player at the horse, empty-handed and far from him, grips it
	case 3:
	{
		FString Mission = TEXT("shgranry");
		OLFlags::Value(TEXT("OLHomBoss"), Mission);
		static const TCHAR* Posters[][2] = { { TEXT("shfort"), TEXT("PORCH1D") }, { TEXT("shsimms"), TEXT("PORCH2D") }, { TEXT("shgranry"), TEXT("PORCH3D") },
			{ TEXT("shgulch"), TEXT("PORCH4D") }, { TEXT("shwildns"), TEXT("PORCH5D") } };
		FString Porch = TEXT("PORCH3D");
		for (const auto& Pr : Posters) if (Mission.Equals(Pr[0], ESearchCase::IgnoreCase)) Porch = Pr[1];
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: the poster for %s (%s)"), *Mission, *Porch);
		HomSpawnLevel(Mission.ToUpper() + TEXT(" ") + Porch);
		Step(4, 2.0);
		return;
	}
	case 4:
		if (LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) || !LevelName.Equals(HomMission, ESearchCase::IgnoreCase) || IsRebuildPending()) return;
		Step(5, 4.0);
		return;
	case 5:
	{
		AOutlawsEnemy* B = HomBoss();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: in %s (hom %d), boss %s (name %d)"), *LevelName, bHom ? 1 : 0, B ? *B->GetOutlawsName() : TEXT("none"), HomBossNameId(B));
		if (B)
		{
			bInvincible = true;
			B->SetDormant(false);
			const bool bKill = OLFlags::Param(TEXT("OLHomKill"));
			for (int32 k = 0; k < 40 && !B->IsDead(); ++k)
			{
				AOutlawsEnemy::GNextHitMelee = !bKill;          // (a fist; -OLHomKill: a gun)
				B->TakeHit(1.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(30.0, 0.0, 0.0));
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: boss %s by %s: dead %d, taken alive %d"), *B->GetOutlawsName(), bKill ? TEXT("a gun") : TEXT("a fist"), B->IsDead() ? 1 : 0, B->bTakenAlive ? 1 : 0);
		}
		Step(50, 1.0);
		return;
	}
	case 50:
	{
		// the lines as they show, a second after he went down (ol_hom_down_<mission>_<captured|killed>.png)
		const AOutlawsEnemy* B = HomBoss();
		const FString Name = FString::Printf(TEXT("ol_hom_down_%s_%s"), *HomMission.ToLower(), B && B->bTakenAlive ? TEXT("captured") : TEXT("killed"));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (Name + TEXT(".png")), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: shot %s"), *Name);
		Step(OLFlags::Param(TEXT("OLHomSaveLoad")) ? 60 : 6, 2.0);
		return;
	}
	case 60:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: the lines shown %d; a save with the outlaw down (slot 7): %s"), bHomBossDown ? 1 : 0, SaveGame(7) ? TEXT("saved") : TEXT("NOT SAVED"));
		Step(61, 1.0);
		return;
	case 61:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: loading slot 7: %s"), LoadGame(7) ? TEXT("loading") : TEXT("NOT LOADING"));
		Step(62, 2.0);
		return;
	case 62:
	{
		if (IsRebuildPending() || PendingLoad.IsValid() || bNeedPlace) return;
		const AOutlawsEnemy* B = HomBoss();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: loaded in %s (hom %d, mission %s): boss %s dead %d, taken alive %d, lines shown again %d"), *LevelName, bHom ? 1 : 0, *HomMission,
			B ? *B->GetOutlawsName() : TEXT("none"), B && B->IsDead() ? 1 : 0, B && B->bTakenAlive ? 1 : 0, bHomBossDown ? 1 : 0);
		Step(6, 2.0);
		return;
	}
	case 6:
	{
		// the horse (HORSEND, the mission's exit): the player beside it, nothing in the hands, the outlaw left where he fell
		int32 Horse = -1;
		for (int32 i = 0; i < Props.Num(); ++i) if (Props[i].bNudge && Props[i].Name.Equals(TEXT("HORSEND"), ESearchCase::IgnoreCase)) Horse = i;
		const AOutlawsEnemy* B = HomBoss();
		bool bWorked = false;
		if (Horse >= 0)
		{
			const FVector Base = Props[Horse].Base;
			Pawn->TeleportTo(Base + FVector(Props[Horse].Radius + 60.0, 0.0, 100.0), Pawn->GetActorRotation(), false, true);
			bWorked = TurnPropByHand(Base + FVector(0.0, 0.0, 60.0));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: the horse gripped empty-handed (outlaw held %d, %.0f m away): %s"), B && B->bHeldByPlayer ? 1 : 0,
			B ? FVector::Dist(B->GetActorLocation(), Pawn->GetActorLocation()) / 100.0 : -1.0, Horse < 0 ? TEXT("NO HORSEND") : bWorked ? TEXT("worked") : TEXT("NOT WORKED"));
		if (!bWorked) CompleteLevel(TEXT("hom test: no horse to grip"));
		else HomTestHorseAt = Now;              // (BUG-SIMMSINF: the horse alone must end it, within 30 s)
		Step(70, 2.5);
		return;
	}
	case 70:
	{
		// (the results card beside the counts on the black, shot before the hold is skipped; BUG-SCORESHEETSIZE: once the
		// sheets' shaders are in, -OLShotNoShaderWait shoots without waiting for them)
		const bool bWait = !OLFlags::Param(TEXT("OLShotNoShaderWait"));
		const int32 Pending = Movie ? Movie->SheetShadersPending(bWait) : 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: the results card (%d rows, screen %d), sheet shaders compiling %d%s"), HomCardRows.Num(), Movie ? 1 : 0,
			Pending, Pending > 0 ? (bWait ? TEXT(" (waited)") : TEXT(" (not waited)")) : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_hom_results.png"), false, false);
		Step(7, 1.0);
		return;
	}
	case 7:
		if (Movie) Movie->Skip();                  // (the counts' hold between levels)
		if (HomTestHorseAt > 0.0 && Now - HomTestHorseAt > 30.0 && LevelName.Equals(HomMission, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: FAIL the horse ended nothing in %s (30 s)"), *LevelName);
			Step(9, 0.5);
			return;
		}
		if (!LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) || IsRebuildPending()) return;
		Step(8, 3.0);
		return;
	case 8:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: back in the office: score %d, porches %d (PORCH3D %d), save %d"), HomGameScore, HomPorchEvents.Num(),
			HomPorchEvents.FindRef(TEXT("PORCH3D")), HasHomSave() ? 1 : 0);
		// (BUG-HOMCELL) the cells' figures: which are shown
		{
			TArray<FString> Shown;
			for (const TPair<FString, TWeakObjectPtr<AOutlawsSpriteActor>>& Kv : CellGens)
			{
				if (Kv.Value.IsValid() && !Kv.Value->IsHidden()) Shown.Add(FString::Printf(TEXT("%s (#%s) at %s"), *Kv.Value->OutlawsName, *Kv.Key, *Kv.Value->GetActorLocation().ToCompactString()));
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test: the cells: %d figure generators, shown: %s"), CellGens.Num(), Shown.Num() ? *FString::Join(Shown, TEXT("; ")) : TEXT("none"));
		}
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_hom_office.png"), false, false);
		Step(9, 1.5);
		return;
	case 9:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hom test done"));
		FGenericPlatformMisc::RequestExit(false);
		Step(10, 100.0);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
