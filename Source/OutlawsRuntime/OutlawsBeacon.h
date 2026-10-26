#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OutlawsBeacon.generated.h"

class FSocket;
class UCanvas;
class UFont;
class UTexture2D;
class UCanvasRenderTarget2D;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class AOutlawsPlayer;
class AOutlawsLevelActor;

/**
 * R11 (user 2026-10-03: "if it detects someone on the same network is playing a game, it should ask if you want to join
 * them ... pop up in a bubble in front of the menu, not too far from the player's face ... an X button"): a joinable game
 * says so on the home network (a UDP broadcast every 2 s); a game on the main menu or played alone hears it and offers a
 * bubble to join.  Joining a game played alone knocks first: its player accepts, and their game reopens for co-op from its
 * autosave (the lobby's own Load path), the joiner coming in as R7's late join.  LAN only; nothing goes over the internet.
 */

/** A game heard on the network. */
struct FOutlawsHeardGame
{
	FString Key;             // "ip:beaconport"
	FString Ip;
	int32 BeaconPort = 0;
	FString Id;              // the instance's own random id (one machine may run two)
	FString Name;
	FString State;           // coop_lobby | coop_level | single
	FString Level;
	int32 Players = 1;
	int32 Max = 1;
	bool bPassword = false;
	FString Build;
	int32 GamePort = 7777;
	double HeardAt = 0.0;
	double FirstAt = 0.0;
};

/** What a bubble offers. */
enum class EOutlawsBubbleKind : uint8
{
	None,
	Offer,           // "<name> is playing ...": Join / X
	Leave,           // "Leave your game and join <name>? Your game autosaves.": Join / X
	Asking,          // "Asking <name>...": X
	Refused,         // "<name> said not now": X
	Knocked,         // "<name> wants to join your game": Accept / Not now
	Opening          // "Opening your game to <name>...": nothing
};

/** The bubble: a small panel in front of the eyes (60 cm, 15 degrees under the view line) in the DialogBox look. */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsJoinBubble : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsJoinBubble();
	void Show(EOutlawsBubbleKind InKind, const FString& InText, const FString& InYes, const FString& InNo);
	EOutlawsBubbleKind GetKind() const { return Kind; }
	const FString& GetText() const { return Text; }
	/** Which button a hand's laser is on (0 yes, 1 no, -1 none): the gun keeps quiet while it is on one. */
	int32 HandOn(int32 Hand) const { return Hover[Hand]; }
	/** Picked (the trigger on a button; tests and desktop keys too): 0 yes, 1 no. */
	TFunction<void(int32 Button)> OnPick;
	void Pick(int32 Button);

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Plane;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Laser[2];
	UPROPERTY(Transient)
	TObjectPtr<UCanvasRenderTarget2D> Canvas;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> Kept;
	UTexture2D* Box = nullptr;
	UTexture2D* Wood = nullptr;
	UFont* Font = nullptr;
	UFont* ButtonFont = nullptr;
	EOutlawsBubbleKind Kind = EOutlawsBubbleKind::None;
	FString Text, Yes, No;
	int32 Hover[2] = { -1, -1 };
	bool bArmed[2] = { false, false };
	float Yaw = 0.f, YawVel = 0.f;
	bool bFollow = false, bPlaced = false;
	FBox2D Buttons[2];
	void Place(float Dt);
	void Redraw();
	UFUNCTION()
	void DrawCanvas(UCanvas* C, int32 Width, int32 Height);
};

UCLASS()
class OUTLAWSRUNTIME_API UOutlawsBeacon : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	static UOutlawsBeacon* Get(const UObject* Context);
	/** The Options > Multiplayer rows: "mp_announce" (let players on my network find my game), "mp_listen" (tell me when
	 *  someone nearby is playing); both On unless the settings file says Off. */
	static bool Setting(const FString& Row);
	static void Toggle(const FString& Row);
	/** A hand's laser is on the bubble's button: the trigger is the bubble's, not the gun's. */
	static bool HandOnBubble(const UObject* Context, int32 Hand);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickable() const override { return !IsTemplate() && bEnabled; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	/** One beacon's text in (the socket's, or a test's own). */
	void HandleMessage(const FString& Msg, const FString& FromIp, int32 FromPort, double Now);

private:
	bool bEnabled = false;
	FSocket* Socket = nullptr;
	int32 BeaconPort = 7778;
	TArray<TPair<FString, int32>> SendTo;     // where beacons go (broadcast, or -OLBeaconUnicast for tests)
	FString MyId;
	double NextBeacon = 0.0;
	TMap<FString, FOutlawsHeardGame> Heard;
	TMap<FString, FString> Dismissed;        // key -> the state it was dismissed in
	TMap<FString, double> DismissedAt;
	FString Offered;                         // the host the bubble is about
	double QuietSince = 0.0;                 // play: the triggers let go since
	UPROPERTY(Transient)
	TObjectPtr<AOutlawsJoinBubble> Bubble;
	// a join under way: the host's key, what we wait for
	FString JoinKey;
	double JoinAskedAt = 0.0;
	double JoinYesAt = 0.0;
	// the knocked side: who knocked, and the re-host waiting for its lobby
	FString KnockFrom;                       // "ip:port"
	FString KnockName;
	bool bOpening = false;
	double OpeningAt = 0.0;
	// -OLBeaconTest
	FString TestRole;
	int32 TestStep = 0;
	double TestAt = 0.0;
	double StartedAt = 0.0;

	AOutlawsLevelActor* FindLevel() const;
	AOutlawsPlayer* FindPlayer() const;
	bool OpenSocket();
	void Send(const FString& Msg, const FString& Ip, int32 Port);
	void Receive(double Now);
	/** This game's state for a beacon (empty: not joinable now). */
	FString MyState(FString& OutLevel, int32& OutPlayers, int32& OutMax) const;
	/** Listening now: the main menu, or alone in a level (not in a session, not dead, not in a cutscene). */
	bool MayListen(bool& bInPlay) const;
	void UpdateBubble(double Now);
	void ShowBubble(EOutlawsBubbleKind Kind, const FString& Text, const FString& Yes, const FString& No);
	void HideBubble();
	void OnPick(int32 Button);
	void StartJoin(const FOutlawsHeardGame& G, double Now);
	void ConnectTo(const FOutlawsHeardGame& G);
	FString OfferText(const FOutlawsHeardGame& G, bool& bSameBuild) const;
	void TickTest(double Now);
};
