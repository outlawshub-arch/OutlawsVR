#pragma once

// M2 (tools/olue/PLANS.md "M2. Modes and scoring"): the original's six game classes over M1's match.  This is the rules of the
// five beyond Death Match, with no network in them, beside FOutlawsDeathmatch (frags, deaths, spawns): the teams (red 1, blue 2),
// the mode items (the two flags, the chicken, the document) and who holds them, the timed scores (the chicken carrier's, IT's),
// the captures and the team scores, and who has won.  The session (OutlawsCoopModes.cpp) feeds it the players, the kills and
// where everyone stands, and does what it says (the messages, the end).  Where the original's rules are not known the choice is
// marked "guess" here and in COOP_DESIGN.md.

#include "CoreMinimal.h"
#include "OutlawsMpHost.h"

class FOutlawsLevel;

enum class EOutlawsModeItemKind : uint8 { RedFlag, BlueFlag, Chicken, Document };
enum class EOutlawsModeItemState : uint8 { Home, Carried, Dropped };

/** A mode item: where it belongs, where it is, who carries it. */
struct FOutlawsModeItem
{
	EOutlawsModeItemKind Kind = EOutlawsModeItemKind::Chicken;
	int32 Team = 0;                   // the flag's team (1 red, 2 blue); 0 the chicken and the document
	FVector Home = FVector::ZeroVector;
	FVector At = FVector::ZeroVector;
	EOutlawsModeItemState State = EOutlawsModeItemState::Home;
	int32 Carrier = -1;
	double DroppedAt = 0.0;
	/** The item's ground sprite (the game's own: grflag, gbflag, ichick, gdoc). */
	const TCHAR* Nwx() const;
	FString Describe() const;
};

struct FOutlawsModePlayer
{
	int32 Id = -1;
	int32 Team = 0;                   // 0 in the free-for-all classes
	float Held = 0.f;                 // seconds with the chicken, or as IT
	int32 Captures = 0;
	bool bAlive = false;
};

class OUTLAWSRUNTIME_API FOutlawsMatchModes
{
public:
	EOutlawsGameClass Class = EOutlawsGameClass::DeathMatch;
	/** Secret Document only: red against blue (each team's base its capture point), else free-for-all (guess). */
	bool bTeamDocument = false;
	bool bFriendlyFire = false;
	/** Captures (CTF, Secret Document) or team kills (Team Play) to win; seconds held to win (Chicken, Tag); 0: none. */
	int32 ScoreLimit = 0;
	float SecondsLimit = 0.f;
	static constexpr double TouchCm = 110.0;          // a player this near an item (across; 150 up or down) touches it
	static constexpr double ReturnSeconds = 30.0;     // a dropped flag or document goes home after this (guess)
	static constexpr double DocFarFromStartsCm = 1500.0;   // the document never starts within 15 m of a player start (user)

	// what the rules ask of the session
	TFunction<void(const FString& Text, float Seconds)> OnSay;
	TFunction<void(int32 Who, const FString& Text, float Seconds)> OnSayTo;
	TFunction<FString(int32 Who)> NameOf;
	/** The match is won (Winner: a player's name, or "The red team"). */
	TFunction<void(const FString& Winner, const FString& Why)> OnWin;

	bool IsTeamClass() const;

	/** The level's bases, chicken and document places, from its objects (FLAGSTAR, CHCKGENE, DOCSTART; STARTPOS for
	 *  the distances); the items set at home.  The words for the log. */
	FString ReadLevel(const FOutlawsLevel& L);

	void AddPlayer(int32 Id, double Now);
	void RemovePlayer(int32 Id, double Now);
	FOutlawsModePlayer* Find(int32 Id);
	const FOutlawsModePlayer* Find(int32 Id) const;
	const TArray<FOutlawsModePlayer>& GetPlayers() const { return Players; }
	int32 TeamOf(int32 Id) const;
	/** A team change (the pause menu): what it carries is dropped first.  False: the same team, or not a team class. */
	bool SetTeam(int32 Id, int32 Team, const FVector& At, double Now);
	/** False: the rules refuse the hit (a teammate's, friendly fire off). */
	bool HitAllowed(int32 Shooter, int32 Victim) const;

	/** The countdown ran out: the items home, the scores and the holds afresh; Tag's first IT. */
	void GoLive(double Now);
	void SetLive(bool bInLive) { bLive = bInLive; }
	/** Victim died where it lay (Killer: -1 or Victim itself for a suicide or the world). */
	void NoteKill(int32 Killer, int32 Victim, const FVector& VictimAt, double Now);
	/** A respawn: alive again. */
	void NoteAlive(int32 Id, bool bAlive);
	/** Each frame on the host: where each living player stands (false: not drawn).  Touches, carries, captures, returns, the
	 *  held time, the limits. */
	void Tick(double Now, float Dt, TFunctionRef<bool(int32 Id, FVector& OutFeet)> Where);

	TArray<FOutlawsModeItem> Items;
	TArray<FVector> CapturePoints;            // Secret Document free-for-all: every one counts (guess)
	int32 TeamScore[3] = {};                  // by team (1 red, 2 blue)
	int32 It = -1;                            // Tag: the player who is IT
	TArray<FVector> Bases;                    // [0] red, [1] blue (CTF; Secret Document's team capture points)
	static const TCHAR* TeamName(int32 Team) { return Team == 1 ? TEXT("red") : Team == 2 ? TEXT("blue") : TEXT("no"); }

private:
	TArray<FOutlawsModePlayer> Players;
	bool bLive = false;
	bool bWon = false;
	void Say(const FString& Text, float Seconds = 3.f);
	void SayTo(int32 Who, const FString& Text, float Seconds = 3.f);
	FString Name(int32 Who) const;
	void Carry(FOutlawsModeItem& I, int32 Who);
	void Drop(FOutlawsModeItem& I, const FVector& At, double Now);
	void SendHome(FOutlawsModeItem& I, const TCHAR* Why);
	void Win(const FString& Winner, const FString& Why);
	void ChooseIt(int32 NotThis, const TCHAR* Why);
	FOutlawsModeItem* Carried(int32 Who);
};
