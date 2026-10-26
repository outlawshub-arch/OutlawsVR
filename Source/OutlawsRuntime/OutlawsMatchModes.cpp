// M2: the rules of the five game classes beyond Death Match (OutlawsMatchModes.h).  From the player's own files:
// - FLAGSTAR objects: the flags' places.  town_2p has two (the two bases, facing each other); the other CTF maps have nine to
//   seventeen: the pair farthest apart are the bases (guess: the original's own choice among them is not known).
// - CHCKGENE: the chicken's place (drygulch); DOCSTART: the document's (five to thirteen a map); the one farthest from every
//   player start is used, never within 15 m of one (user 2026-10-03).  No document object: the chicken's place; neither: the
//   farthest flag place.
// - The items' own sprites: grflag / gbflag (the flags on the ground), ichick (the chicken), gdoc (the document).
// - The remaster's words: "You got the red team's flag!", "The blue team captured the flag!", "You got the chicken!",
//   "<name> is now toting the chicken!", "Bawk! The chicken is loose!", "TAG -- you're IT!", "<name> is IT!",
//   "You captured Secret Document!".
#include "OutlawsMatchModes.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"

const TCHAR* FOutlawsModeItem::Nwx() const
{
	switch (Kind)
	{
	case EOutlawsModeItemKind::RedFlag: return TEXT("grflag.nwx");
	case EOutlawsModeItemKind::BlueFlag: return TEXT("gbflag.nwx");
	case EOutlawsModeItemKind::Document: return TEXT("gdoc.nwx");
	default: return TEXT("ichick.nwx");
	}
}

FString FOutlawsModeItem::Describe() const
{
	static const TCHAR* Kinds[] = { TEXT("red flag"), TEXT("blue flag"), TEXT("chicken"), TEXT("document") };
	static const TCHAR* States[] = { TEXT("home"), TEXT("carried"), TEXT("dropped") };
	return FString::Printf(TEXT("%s %s%s at %s"), Kinds[int32(Kind)], States[int32(State)], Carrier >= 0 ? *FString::Printf(TEXT(" by %d"), Carrier) : TEXT(""), *At.ToCompactString());
}

bool FOutlawsMatchModes::IsTeamClass() const
{
	return Class == EOutlawsGameClass::TeamPlay || Class == EOutlawsGameClass::CaptureTheFlag || (Class == EOutlawsGameClass::SecretDocument && bTeamDocument);
}

void FOutlawsMatchModes::Say(const FString& Text, float Seconds)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLMODE say: %s"), *Text);
	if (OnSay) OnSay(Text, Seconds);
}

void FOutlawsMatchModes::SayTo(int32 Who, const FString& Text, float Seconds)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLMODE say to %d: %s"), Who, *Text);
	if (OnSayTo) OnSayTo(Who, Text, Seconds);
}

FString FOutlawsMatchModes::Name(int32 Who) const
{
	return NameOf ? NameOf(Who) : FString::Printf(TEXT("Player %d"), Who + 1);
}

// ---- the level ------------------------------------------------------------------------------------------------------------

FString FOutlawsMatchModes::ReadLevel(const FOutlawsLevel& L)
{
	Items.Reset();
	Bases.Reset();
	CapturePoints.Reset();
	TArray<FVector> Flags, Docs, Starts, Chickens;
	for (const FOutlawsObject& O : L.Objects)
	{
		const FVector At = FOutlawsGeometry::ToUE(O.Pos.X, O.Pos.Y, O.Pos.Z);
		if (O.Name == TEXT("FLAGSTAR")) Flags.Add(At);
		else if (O.Name == TEXT("DOCSTART")) Docs.Add(At);
		else if (O.Name == TEXT("CHCKGENE")) Chickens.Add(At);
		else if (O.Name == TEXT("STARTPOS") || O.Name == TEXT("PLAYER")) Starts.Add(At);
	}
	// the bases: the two flag places farthest apart (the player starts if a map has fewer than two)
	const TArray<FVector>& BasePool = Flags.Num() >= 2 ? Flags : Starts;
	int32 A = -1, B = -1;
	double Far = -1.0;
	for (int32 i = 0; i < BasePool.Num(); ++i)
	{
		for (int32 j = i + 1; j < BasePool.Num(); ++j)
		{
			const double D = FVector::Dist(BasePool[i], BasePool[j]);
			if (D > Far) { Far = D; A = i; B = j; }
		}
	}
	if (A >= 0)
	{
		Bases.Add(BasePool[A]);
		Bases.Add(BasePool[B]);
	}
	// a place far from every player start: the farthest of Pool, if 15 m or more from each
	auto FarthestFromStarts = [&Starts](const TArray<FVector>& Pool, double& OutNear) -> int32
	{
		int32 Best = -1;
		OutNear = -1.0;
		for (int32 i = 0; i < Pool.Num(); ++i)
		{
			double Near = TNumericLimits<double>::Max();
			for (const FVector& S : Starts) Near = FMath::Min(Near, FVector::Dist(S, Pool[i]));
			if (Near > OutNear) { OutNear = Near; Best = i; }
		}
		return Best;
	};
	FString How;
	switch (Class)
	{
	case EOutlawsGameClass::CaptureTheFlag:
		for (int32 T = 1; T <= 2 && Bases.Num() == 2; ++T)
		{
			FOutlawsModeItem& I = Items.AddDefaulted_GetRef();
			I.Kind = T == 1 ? EOutlawsModeItemKind::RedFlag : EOutlawsModeItemKind::BlueFlag;
			I.Team = T;
			I.Home = I.At = Bases[T - 1];
		}
		How = FString::Printf(TEXT("%d flag places: the bases %.0f m apart (%s)"), Flags.Num(), Far / 100.0, Flags.Num() == 2 ? TEXT("the map's two") : TEXT("the pair farthest apart, guess"));
		break;
	case EOutlawsGameClass::Chicken:
	{
		double Near = 0.0;
		const TArray<FVector>& Pool = Chickens.Num() > 0 ? Chickens : Docs.Num() > 0 ? Docs : Flags;
		const int32 i = FarthestFromStarts(Pool, Near);
		FOutlawsModeItem& I = Items.AddDefaulted_GetRef();
		I.Kind = EOutlawsModeItemKind::Chicken;
		I.Home = I.At = i >= 0 ? Pool[i] : (Starts.Num() > 0 ? Starts[0] : FVector::ZeroVector);
		How = FString::Printf(TEXT("the chicken at %s (%s, %.0f m from the nearest player start)"), *I.Home.ToCompactString(),
			Chickens.Num() > 0 ? TEXT("the map's CHCKGENE") : Docs.Num() > 0 ? TEXT("no CHCKGENE: a document place") : TEXT("no CHCKGENE: a flag place"), Near / 100.0);
		break;
	}
	case EOutlawsGameClass::SecretDocument:
	{
		// the document's place: its own objects, else the chicken's, else a flag place; never within 15 m of a player start
		const TArray<FVector>& Pool = Docs.Num() > 0 ? Docs : Chickens.Num() > 0 ? Chickens : Flags;
		TArray<FVector> Clear;
		for (const FVector& P : Pool)
		{
			bool bOk = true;
			for (const FVector& S : Starts) bOk &= FVector::Dist(S, P) >= DocFarFromStartsCm;
			if (bOk) Clear.Add(P);
		}
		double Near = 0.0;
		const TArray<FVector>& Use = Clear.Num() > 0 ? Clear : Pool;
		const int32 i = FarthestFromStarts(Use, Near);
		FOutlawsModeItem& I = Items.AddDefaulted_GetRef();
		I.Kind = EOutlawsModeItemKind::Document;
		I.Home = I.At = i >= 0 ? Use[i] : (Starts.Num() > 0 ? Starts[0] : FVector::ZeroVector);
		// the capture points: the team bases (team play), else every flag place but the document's own (guess)
		if (!bTeamDocument)
		{
			for (const FVector& F : Flags) if (FVector::Dist(F, I.Home) > 500.0) CapturePoints.Add(F);
			if (CapturePoints.Num() == 0) CapturePoints = Bases;
		}
		How = FString::Printf(TEXT("the document at %s (%s; %d of %d places 15 m or more from every player start; %.0f m from the nearest), %s"), *I.Home.ToCompactString(),
			Docs.Num() > 0 ? TEXT("DOCSTART") : Chickens.Num() > 0 ? TEXT("no DOCSTART: the chicken's place") : TEXT("no DOCSTART or CHCKGENE: a flag place"),
			Clear.Num(), Pool.Num(), Near / 100.0, bTeamDocument ? TEXT("capture at each team's base") : *FString::Printf(TEXT("%d capture points (every flag place: guess)"), CapturePoints.Num()));
		break;
	}
	default:
		How = TEXT("no items");
		break;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLMODE %s on %s: %s; %d flag places, %d document places, %d chicken places, %d player starts"), *OutlawsMpHost::ClassLabel(Class), *L.LevelName, *How,
		Flags.Num(), Docs.Num(), Chickens.Num(), Starts.Num());
	return How;
}

// ---- the players ----------------------------------------------------------------------------------------------------------------

FOutlawsModePlayer* FOutlawsMatchModes::Find(int32 Id)
{
	return Players.FindByPredicate([Id](const FOutlawsModePlayer& P) { return P.Id == Id; });
}

const FOutlawsModePlayer* FOutlawsMatchModes::Find(int32 Id) const
{
	return Players.FindByPredicate([Id](const FOutlawsModePlayer& P) { return P.Id == Id; });
}

int32 FOutlawsMatchModes::TeamOf(int32 Id) const
{
	const FOutlawsModePlayer* P = Find(Id);
	return P ? P->Team : 0;
}

void FOutlawsMatchModes::AddPlayer(int32 Id, double Now)
{
	if (Find(Id)) return;
	FOutlawsModePlayer& P = Players.AddDefaulted_GetRef();
	P.Id = Id;
	P.bAlive = true;
	if (IsTeamClass())
	{
		// (auto-balance) the smaller team; red when even
		int32 N[3] = {};
		for (const FOutlawsModePlayer& O : Players) if (O.Id != Id) ++N[FMath::Clamp(O.Team, 0, 2)];
		P.Team = N[1] <= N[2] ? 1 : 2;
		UE_LOG(LogOutlaws, Log, TEXT("OLMODE %s joins the %s team (red %d, blue %d before)"), *Name(Id), TeamName(P.Team), N[1], N[2]);
		SayTo(Id, FString::Printf(TEXT("You are on the %s team"), TeamName(P.Team)));
	}
	if (Class == EOutlawsGameClass::Tag && bLive && It < 0) ChooseIt(-1, TEXT("nobody was IT"));
}

void FOutlawsMatchModes::RemovePlayer(int32 Id, double Now)
{
	FOutlawsModePlayer* P = Find(Id);
	if (!P) return;
	if (FOutlawsModeItem* I = Carried(Id)) Drop(*I, I->At, Now);
	Players.RemoveAll([Id](const FOutlawsModePlayer& X) { return X.Id == Id; });
	if (It == Id)
	{
		It = -1;
		ChooseIt(Id, TEXT("IT left"));
	}
}

bool FOutlawsMatchModes::SetTeam(int32 Id, int32 Team, const FVector& At, double Now)
{
	FOutlawsModePlayer* P = Find(Id);
	if (!P || !IsTeamClass() || Team < 1 || Team > 2 || P->Team == Team) return false;
	if (FOutlawsModeItem* I = Carried(Id)) Drop(*I, At, Now);
	P->Team = Team;
	Say(FString::Printf(TEXT("%s joins the %s team"), *Name(Id), TeamName(Team)));
	return true;
}

bool FOutlawsMatchModes::HitAllowed(int32 Shooter, int32 Victim) const
{
	if (!IsTeamClass() || bFriendlyFire || Shooter == Victim) return true;
	const int32 A = TeamOf(Shooter), B = TeamOf(Victim);
	return A == 0 || A != B;
}

// ---- the items ------------------------------------------------------------------------------------------------------------------

FOutlawsModeItem* FOutlawsMatchModes::Carried(int32 Who)
{
	return Items.FindByPredicate([Who](const FOutlawsModeItem& I) { return I.State == EOutlawsModeItemState::Carried && I.Carrier == Who; });
}

void FOutlawsMatchModes::Carry(FOutlawsModeItem& I, int32 Who)
{
	I.State = EOutlawsModeItemState::Carried;
	I.Carrier = Who;
	switch (I.Kind)
	{
	case EOutlawsModeItemKind::RedFlag:
	case EOutlawsModeItemKind::BlueFlag:
		SayTo(Who, FString::Printf(TEXT("You got the %s team's flag!"), TeamName(I.Team)));
		Say(FString::Printf(TEXT("%s has the %s team's flag!"), *Name(Who), TeamName(I.Team)));
		break;
	case EOutlawsModeItemKind::Chicken:
		SayTo(Who, TEXT("You got the chicken!"));
		Say(FString::Printf(TEXT("%s is now toting the chicken!"), *Name(Who)));
		break;
	case EOutlawsModeItemKind::Document:
		SayTo(Who, TEXT("You have the Secret Document!"));
		Say(FString::Printf(TEXT("%s has the Secret Document!"), *Name(Who)));
		break;
	}
}

void FOutlawsMatchModes::Drop(FOutlawsModeItem& I, const FVector& At, double Now)
{
	I.State = EOutlawsModeItemState::Dropped;
	I.Carrier = -1;
	I.At = At;
	I.DroppedAt = Now;
	if (I.Kind == EOutlawsModeItemKind::Chicken) Say(TEXT("Bawk! The chicken is loose!"));
	else if (I.Kind == EOutlawsModeItemKind::Document) Say(TEXT("The Secret Document was dropped!"));
	else Say(FString::Printf(TEXT("The %s team's flag was dropped!"), TeamName(I.Team)));
}

void FOutlawsMatchModes::SendHome(FOutlawsModeItem& I, const TCHAR* Why)
{
	I.State = EOutlawsModeItemState::Home;
	I.Carrier = -1;
	I.At = I.Home;
	UE_LOG(LogOutlaws, Log, TEXT("OLMODE %s home (%s)"), *I.Describe(), Why);
}

// ---- the match ------------------------------------------------------------------------------------------------------------------

void FOutlawsMatchModes::GoLive(double Now)
{
	bLive = true;
	bWon = false;
	TeamScore[1] = TeamScore[2] = 0;
	for (FOutlawsModePlayer& P : Players)
	{
		P.Held = 0.f;
		P.Captures = 0;
		P.bAlive = true;
	}
	for (FOutlawsModeItem& I : Items) SendHome(I, TEXT("the match begins"));
	It = -1;
	if (Class == EOutlawsGameClass::Tag) ChooseIt(-1, TEXT("the match begins"));
}

void FOutlawsMatchModes::ChooseIt(int32 NotThis, const TCHAR* Why)
{
	// (guess) a random player, not the one leaving or the one who just was
	TArray<int32> Pool;
	for (const FOutlawsModePlayer& P : Players) if (P.Id != NotThis) Pool.Add(P.Id);
	if (Pool.Num() == 0) { It = -1; return; }
	It = Pool[FMath::RandRange(0, Pool.Num() - 1)];
	UE_LOG(LogOutlaws, Log, TEXT("OLMODE %s is IT (%s)"), *Name(It), Why);
	SayTo(It, TEXT("TAG -- you're IT!"));
	Say(FString::Printf(TEXT("%s is IT!"), *Name(It)));
}

void FOutlawsMatchModes::NoteAlive(int32 Id, bool bAlive)
{
	if (FOutlawsModePlayer* P = Find(Id)) P->bAlive = bAlive;
}

void FOutlawsMatchModes::NoteKill(int32 Killer, int32 Victim, const FVector& VictimAt, double Now)
{
	NoteAlive(Victim, false);
	if (FOutlawsModeItem* I = Carried(Victim)) Drop(*I, VictimAt, Now);
	if (!bLive) return;
	const bool bByOther = Killer >= 0 && Killer != Victim && Find(Killer);
	if (Class == EOutlawsGameClass::TeamPlay && bByOther)
	{
		const int32 TK = TeamOf(Killer), TV = TeamOf(Victim);
		if (TK != TV && TK > 0)
		{
			++TeamScore[TK];
			UE_LOG(LogOutlaws, Log, TEXT("OLMODE the %s team scores (%d to %d)"), TeamName(TK), TeamScore[1], TeamScore[2]);
			if (ScoreLimit > 0 && TeamScore[TK] >= ScoreLimit) Win(FString::Printf(TEXT("The %s team"), TeamName(TK)), FString::Printf(TEXT("%d kills"), TeamScore[TK]));
		}
	}
	if (Class == EOutlawsGameClass::Tag && Victim == It)
	{
		// whoever kills IT is IT; a suicide or the world passes it on at random (guess)
		if (bByOther)
		{
			It = Killer;
			UE_LOG(LogOutlaws, Log, TEXT("OLMODE %s killed IT and is IT"), *Name(Killer));
			SayTo(It, TEXT("TAG -- you're IT!"));
			Say(FString::Printf(TEXT("%s is IT!"), *Name(It)));
		}
		else
		{
			ChooseIt(Victim, TEXT("IT died by its own hand"));
		}
	}
}

void FOutlawsMatchModes::Win(const FString& Winner, const FString& Why)
{
	if (bWon) return;
	bWon = true;
	bLive = false;
	UE_LOG(LogOutlaws, Log, TEXT("OLMODE won: %s (%s)"), *Winner, *Why);
	if (OnWin) OnWin(Winner, Why);
}

void FOutlawsMatchModes::Tick(double Now, float Dt, TFunctionRef<bool(int32 Id, FVector& OutFeet)> Where)
{
	if (!bLive) return;
	// the held time: the chicken's carrier, IT (alive)
	if (Class == EOutlawsGameClass::Chicken || Class == EOutlawsGameClass::Tag)
	{
		int32 Holder = It;
		if (Class == EOutlawsGameClass::Chicken)
		{
			Holder = -1;
			for (const FOutlawsModeItem& I : Items) if (I.State == EOutlawsModeItemState::Carried) Holder = I.Carrier;
		}
		if (FOutlawsModePlayer* P = Holder >= 0 ? Find(Holder) : nullptr; P && P->bAlive)
		{
			const int32 Before = FMath::FloorToInt(P->Held);
			P->Held += Dt;
			if (FMath::FloorToInt(P->Held) / 10 != Before / 10) UE_LOG(LogOutlaws, Log, TEXT("OLMODE %s has held for %.0f s"), *Name(Holder), P->Held);
			if (SecondsLimit > 0.f && P->Held >= SecondsLimit) Win(Name(Holder), FString::Printf(TEXT("%.0f s %s"), P->Held, Class == EOutlawsGameClass::Tag ? TEXT("as IT") : TEXT("with the chicken")));
		}
	}
	// the items: carried ones ride with their carrier; touches, captures and returns
	for (FOutlawsModeItem& I : Items)
	{
		if (I.State == EOutlawsModeItemState::Carried)
		{
			FVector At;
			if (Where(I.Carrier, At)) I.At = At;
		}
		if (I.State == EOutlawsModeItemState::Dropped && I.Kind != EOutlawsModeItemKind::Chicken && Now - I.DroppedAt >= ReturnSeconds)
		{
			SendHome(I, TEXT("dropped too long"));
			Say(I.Kind == EOutlawsModeItemKind::Document ? FString(TEXT("The Secret Document is back in its place")) : FString::Printf(TEXT("The %s team's flag is back at its base"), TeamName(I.Team)));
		}
	}
	for (FOutlawsModePlayer& P : Players)
	{
		FVector Feet;
		if (!P.bAlive || !Where(P.Id, Feet)) continue;
		auto Touches = [&Feet](const FVector& At) { return FVector::Dist2D(Feet, At) <= TouchCm && FMath::Abs(Feet.Z - At.Z) <= 150.0; };
		for (FOutlawsModeItem& I : Items)
		{
			if (I.State == EOutlawsModeItemState::Carried || !Touches(I.At)) continue;
			const bool bFlag = I.Kind == EOutlawsModeItemKind::RedFlag || I.Kind == EOutlawsModeItemKind::BlueFlag;
			if (bFlag && I.Team == P.Team)
			{
				// one's own flag: a dropped one goes home at a touch; at home it is where the enemy's flag is brought
				if (I.State == EOutlawsModeItemState::Dropped)
				{
					SendHome(I, TEXT("its own team touched it"));
					Say(FString::Printf(TEXT("%s returned the %s team's flag"), *Name(P.Id), TeamName(I.Team)));
				}
				continue;
			}
			if (Carried(P.Id)) continue;                 // (one thing at a time)
			Carry(I, P.Id);
		}
		// captures: an enemy flag at one's own flag (at home); the document at a capture point
		FOutlawsModeItem* Mine = Carried(P.Id);
		if (!Mine) continue;
		if ((Mine->Kind == EOutlawsModeItemKind::RedFlag || Mine->Kind == EOutlawsModeItemKind::BlueFlag) && P.Team > 0)
		{
			const FOutlawsModeItem* Own = Items.FindByPredicate([&P](const FOutlawsModeItem& X) { return X.Team == P.Team; });
			if (Own && Own->State == EOutlawsModeItemState::Home && Touches(Own->Home))
			{
				++TeamScore[P.Team];
				++P.Captures;
				SendHome(*Mine, TEXT("captured"));
				Say(FString::Printf(TEXT("The %s team captured the flag!"), TeamName(P.Team)));
				UE_LOG(LogOutlaws, Log, TEXT("OLMODE capture by %s: red %d, blue %d"), *Name(P.Id), TeamScore[1], TeamScore[2]);
				if (ScoreLimit > 0 && TeamScore[P.Team] >= ScoreLimit) Win(FString::Printf(TEXT("The %s team"), TeamName(P.Team)), FString::Printf(TEXT("%d captures"), TeamScore[P.Team]));
			}
		}
		else if (Mine->Kind == EOutlawsModeItemKind::Document)
		{
			bool bAt = false;
			if (bTeamDocument && P.Team > 0 && Bases.IsValidIndex(P.Team - 1)) bAt = Touches(Bases[P.Team - 1]);
			else for (const FVector& C : CapturePoints) bAt = bAt || Touches(C);
			if (bAt)
			{
				++P.Captures;
				if (P.Team > 0) ++TeamScore[P.Team];
				SendHome(*Mine, TEXT("captured"));
				SayTo(P.Id, TEXT("You captured Secret Document!"));
				Say(FString::Printf(TEXT("%s captured the Secret Document!"), *Name(P.Id)));
				const int32 Score = bTeamDocument ? TeamScore[P.Team] : P.Captures;
				if (ScoreLimit > 0 && Score >= ScoreLimit)
				{
					Win(bTeamDocument ? FString::Printf(TEXT("The %s team"), TeamName(P.Team)) : Name(P.Id), FString::Printf(TEXT("%d captures"), Score));
				}
			}
		}
	}
}
