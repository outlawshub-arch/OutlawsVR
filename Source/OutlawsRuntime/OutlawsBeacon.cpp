// R11, join bubbles (PLANS.md "R11"; user 2026-10-03: "if it detects someone on the same network is playing a game, it should
// ask if you want to join them, but only if they're in game or in their single player game ... pop up in a bubble in front of
// the menu, not too far from the player's face ... an X button if they want to get rid of the bubble").
// - The beacon: a joinable game (a co-op host's lobby or level, or a level played alone) says so every 2 s, one UDP datagram
//   to the network's broadcast addresses on port 7778: "OLB1", its id, the player's name, the state, the level, players and
//   the most, a password flag, the build, the game port (tab separated).  Off in automated runs unless -OLBeacon is given.
// - The listener: on the main menu or alone in a level (no session, not dead, no cutscene) it remembers what it hears for 6 s
//   and offers the newest game not dismissed in a bubble.  X hides that game until its state (or level) changes, and for ten
//   minutes at most otherwise.  In play the bubble waits for a quiet second (both triggers let go).
// - Joining: a co-op host is joined straight away (the co-op join with the address filled in).  A game played alone is
//   knocked on ("OLK1" to its beacon's address); its player sees "<name> wants to join your game" (Accept / Not now) and the
//   answer comes back ("OLR1"); on Accept that game autosaves and reopens as a co-op host from the autosave (the lobby's Load
//   path, started at once, alone), and the joiner connects once its beacon says co-op: R7's late join puts it beside them.
//   Joining from a game of one's own asks first ("Leave your game ..."), then autosaves it.
// - The bubble: 60 cm out, 15 degrees under the view line, on the menu's lazy yaw spring; the DialogBox frame over the wood
//   (as the Frag Table), Montagu Slab; its buttons picked by a hand's laser and the trigger, as the menu's rows are.

#include "OutlawsBeacon.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsCoop.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "Camera/CameraComponent.h"
#include "CanvasItem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MotionControllerComponent.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "UnrealClient.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr double BeaconEvery = 2.0;
	constexpr double HeardFor = 6.0;
	constexpr double DismissFor = 600.0;
	constexpr double QuietNeeded = 1.0;
	constexpr int32 BubblePxW = 1024, BubblePxH = 360;
	constexpr float BubbleW = 36.f;                           // cm across (its height by the canvas's shape)
	constexpr float BubbleDist = 60.f, BubbleDrop = 15.f;     // cm out, degrees under the view line

	bool BeaconTest(FString& Role)
	{
		return OLFlags::Value(TEXT("OLBeaconTest"), Role);
	}

	FString StateKey(const FOutlawsHeardGame& G)
	{
		return G.State + TEXT("|") + G.Level;
	}
}

// ---- the bubble ------------------------------------------------------------------------------------------------------------

AOutlawsJoinBubble::AOutlawsJoinBubble()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	Plane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Plane"));
	RootComponent = Plane;
	Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Plane->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (PlaneMesh.Succeeded()) Plane->SetStaticMesh(PlaneMesh.Object);
	for (int32 H = 0; H < 2; ++H)
	{
		Laser[H] = CreateDefaultSubobject<UStaticMeshComponent>(H == 0 ? TEXT("LaserL") : TEXT("LaserR"));
		Laser[H]->SetupAttachment(RootComponent);
		Laser[H]->SetUsingAbsoluteLocation(true);
		Laser[H]->SetUsingAbsoluteRotation(true);
		Laser[H]->SetUsingAbsoluteScale(true);
		Laser[H]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Laser[H]->SetCastShadow(false);
		if (CubeMesh.Succeeded()) Laser[H]->SetStaticMesh(CubeMesh.Object);
		Laser[H]->SetVisibility(false);
	}
}

void AOutlawsJoinBubble::Show(EOutlawsBubbleKind InKind, const FString& InText, const FString& InYes, const FString& InNo)
{
	Kind = InKind;
	Text = InText;
	Yes = InYes;
	No = InNo;
	Hover[0] = Hover[1] = -1;
	bArmed[0] = bArmed[1] = false;           // (a trigger already held does not pick at once)
	SetActorHiddenInGame(Kind == EOutlawsBubbleKind::None);
	Redraw();
}

void AOutlawsJoinBubble::Pick(int32 Button)
{
	if (Button == 0 && Yes.IsEmpty()) return;
	if (Button == 1 && No.IsEmpty()) return;
	if (OnPick) OnPick(Button);
}

void AOutlawsJoinBubble::Redraw()
{
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!Font && P)
	{
		Box = P->KpfArt(TEXT("DialogBox@2x"));
		Wood = P->KpfArt(TEXT("DialogBackground@2x"));
		Font = P->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 30);
		ButtonFont = P->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 26);
		for (UObject* O : { (UObject*)Box, (UObject*)Wood, (UObject*)Font, (UObject*)ButtonFont }) if (O) Kept.Add(O);
		if (P->LaserMaterial()) for (int32 H = 0; H < 2; ++H) Laser[H]->SetMaterial(0, P->LaserMaterial());
	}
	if (!Font && GEngine) Font = ButtonFont = GEngine->GetMediumFont();
	if (!Canvas)
	{
		Canvas = NewObject<UCanvasRenderTarget2D>(this);
		Canvas->RenderTargetFormat = RTF_RGBA8_SRGB;
		Canvas->TargetGamma = 1.f;
		Canvas->ClearColor = FLinearColor::Black;
		Canvas->InitAutoFormat(BubblePxW, BubblePxH);
		Canvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsJoinBubble::DrawCanvas);
		// the wheel's overlay material (no depth test): over the world and in front of the menu, as the wheel is drawn
		UMaterialInterface* Base = P ? P->LoadOverlayMaterial(TEXT("M_OLR_UIOverlay")) : nullptr;
		if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
		if (Base)
		{
			Plane->SetTranslucentSortPriority(20);
			Mid = UMaterialInstanceDynamic::Create(Base, this);
			Mid->SetTextureParameterValue(TEXT("Tex"), Canvas);
			Plane->SetMaterial(0, Mid);
		}
		Plane->SetRelativeScale3D(FVector(BubbleW / 100.f, BubbleW * BubblePxH / BubblePxW / 100.f, 1.f));
	}
	Canvas->UpdateResource();
}

void AOutlawsJoinBubble::DrawCanvas(UCanvas* C, int32 Width, int32 Height)
{
	const float W = float(Width), H = float(Height);
	auto Col = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	auto Fill = [C](float X, float Y, float Wd, float Ht, const FLinearColor& Colour)
	{
		FCanvasTileItem T(FVector2D(X, Y), FVector2D(Wd, Ht), Colour);
		T.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(T);
	};
	auto Txt = [C](UFont* F, const FString& Str, float X, float Y, const FLinearColor& Colour, float MaxW)
	{
		if (!F || Str.IsEmpty()) return;
		float XL = 0.f, YL = 0.f;
		C->TextSize(F, Str, XL, YL);
		const float Sc = XL > MaxW ? MaxW / XL : 1.f;
		FCanvasTextItem T(FVector2D(X - XL * Sc * 0.5f, Y), FText::FromString(Str), F, Colour);
		T.Scale = FVector2D(Sc, Sc);
		T.bCentreY = true;
		T.BlendMode = SE_BLEND_Translucent;
		T.EnableShadow(FLinearColor::Black, FVector2D(3.f, 2.f));
		C->DrawItem(T);
	};
	// the wood inside, at its native size; the frame's eight pieces over it (corners 116 px of the 1200 at 2x, drawn 64)
	constexpr float Pad = 22.f, Corner = 64.f;
	if (Wood && Wood->GetResource())
	{
		const float TW = Wood->GetSizeX() * 0.6f, TH = Wood->GetSizeY() * 0.6f;
		for (float ty = Pad; ty < H - Pad; ty += TH)
		{
			for (float tx = Pad; tx < W - Pad; tx += TW)
			{
				const float CW = FMath::Min(TW, W - Pad - tx), CH = FMath::Min(TH, H - Pad - ty);
				FCanvasTileItem I(FVector2D(tx, ty), Wood->GetResource(), FVector2D(CW, CH), FVector2D(0.f, 0.f), FVector2D(CW / TW, CH / TH), FLinearColor::White);
				I.BlendMode = SE_BLEND_Opaque;
				C->DrawItem(I);
			}
		}
	}
	else
	{
		Fill(Pad, Pad, W - 2.f * Pad, H - 2.f * Pad, Col(0x2a, 0x1a, 0x0e));
	}
	if (Box && Box->GetResource())
	{
		const float S = float(Box->GetSizeX()), U0 = 116.f / S, U1 = (S - 116.f) / S;
		const float Xs[4] = { 0.f, Corner, W - Corner, W }, Ys[4] = { 0.f, Corner, H - Corner, H }, Us[4] = { 0.f, U0, U1, 1.f };
		for (int32 j = 0; j < 3; ++j)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				if (i == 1 && j == 1) continue;
				FCanvasTileItem I(FVector2D(Xs[i], Ys[j]), Box->GetResource(), FVector2D(Xs[i + 1] - Xs[i], Ys[j + 1] - Ys[j]), FVector2D(Us[i], Us[j]), FVector2D(Us[i + 1], Us[j + 1]), FLinearColor::White);
				I.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(I);
			}
		}
	}
	const FLinearColor Gold = Col(0xd1, 0xaf, 0x27), Hot = Col(0xff, 0xef, 0x65), Pale = Col(0xf0, 0xe6, 0xd0);
	// the words: one or two lines (split at the first ". " or "? ")
	FString A = Text, B;
	int32 Cut = Text.Find(TEXT("? "));
	if (Cut == INDEX_NONE) Cut = Text.Find(TEXT(". "));
	if (Cut != INDEX_NONE) { A = Text.Left(Cut + 1); B = Text.Mid(Cut + 2); }
	const bool bButtons = !Yes.IsEmpty() || !No.IsEmpty();
	const float TextMid = bButtons ? 125.f : H * 0.5f;
	if (B.IsEmpty()) Txt(Font, A, W * 0.5f, TextMid, Pale, W - 140.f);
	else { Txt(Font, A, W * 0.5f, TextMid - 32.f, Pale, W - 140.f); Txt(ButtonFont, B, W * 0.5f, TextMid + 30.f, Pale, W - 140.f); }
	// the buttons: the yes one wide, the no one (X) square or wide as its word needs
	Buttons[0] = Buttons[1] = FBox2D(ForceInit);
	const float By = 220.f, Bh = 84.f;
	const float YesW = Yes.IsEmpty() ? 0.f : 330.f, NoW = No.IsEmpty() ? 0.f : (No.Len() <= 1 ? 110.f : 250.f);
	const float Gap = YesW > 0.f && NoW > 0.f ? 40.f : 0.f;
	float X = (W - YesW - Gap - NoW) * 0.5f;
	for (int32 b = 0; b < 2; ++b)
	{
		const float Bw = b == 0 ? YesW : NoW;
		if (Bw <= 0.f) continue;
		const bool bHot = Hover[0] == b || Hover[1] == b;
		const bool bDead = b == 0 && Yes == TEXT("different version");
		Fill(X - 3.f, By - 3.f, Bw + 6.f, Bh + 6.f, bHot ? Hot : Gold);
		Fill(X, By, Bw, Bh, bDead ? Col(0x30, 0x2a, 0x24) : Col(0x14, 0x0c, 0x06));
		Txt(ButtonFont, b == 0 ? Yes : No, X + Bw * 0.5f, By + Bh * 0.5f, bDead ? Col(0x90, 0x88, 0x80) : bHot ? Hot : Gold, Bw - 24.f);
		Buttons[b] = FBox2D(FVector2D(X, By), FVector2D(X + Bw, By + Bh));
		X += Bw + Gap;
	}
}

void AOutlawsJoinBubble::Place(float Dt)
{
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	const UCameraComponent* Cam = P ? P->FindComponentByClass<UCameraComponent>() : nullptr;
	if (!Cam) return;
	const FVector Eye = Cam->GetComponentLocation();
	const float HeadYaw = Cam->GetComponentRotation().Yaw;
	if (!bPlaced) { Yaw = HeadYaw; bPlaced = true; }
	// the menu's lazy follow: off once the head has turned 25 degrees away, a damped spring back to the middle
	const float Err = FRotator::NormalizeAxis(HeadYaw - Yaw);
	if (FMath::Abs(Err) > 25.f) bFollow = true;
	if (bFollow && FMath::Abs(Err) < 1.5f && FMath::Abs(YawVel) < 6.f) bFollow = false;
	const float Step = FMath::Min(Dt, 0.05f);
	constexpr float Wn = 4.5f;
	YawVel += ((bFollow ? Wn * Wn * Err : 0.f) - 2.f * Wn * YawVel) * Step;
	Yaw = FRotator::NormalizeAxis(Yaw + YawVel * Step);
	const FVector Ahead = FRotator(-BubbleDrop, Yaw, 0.f).Vector();
	const FVector Centre = Eye + Ahead * BubbleDist;
	// (the plane's face is its +Z, turned to the eye: X to the bubble's right, Y down)
	const FVector ToEye = (Eye - Centre).GetSafeNormal();
	const FVector F = FVector(Ahead.X, Ahead.Y, 0.0).GetSafeNormal();
	const FVector Right(-F.Y, F.X, 0.0);
	const FVector Down = FVector::CrossProduct(ToEye, Right).GetSafeNormal();
	SetActorLocationAndRotation(Centre, FRotationMatrix::MakeFromXY(Right, Down).Rotator());
}

void AOutlawsJoinBubble::Tick(float DeltaSeconds)
{
	OL_PERF_SCOPE("Other ours");
	Super::Tick(DeltaSeconds);
	if (Kind == EOutlawsBubbleKind::None) return;
	Place(DeltaSeconds);
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!P || !P->bVR) return;
	// each hand's laser onto the bubble: the button it is on, the trigger picks it (as the menu's rows)
	const FTransform T = GetActorTransform();
	const FVector N = T.GetUnitAxis(EAxis::Z);
	bool bChanged = false;
	for (int32 H = 0; H < 2; ++H)
	{
		int32 On = -1;
		float Len = 0.f;
		const UMotionControllerComponent* Aim = P->AimOf(H);
		if (Aim)
		{
			const FVector O = Aim->GetComponentLocation(), D = Aim->GetForwardVector();
			const double Den = FVector::DotProduct(D, N);
			if (FMath::Abs(Den) > 1e-3)
			{
				const double Dist = FVector::DotProduct(T.GetLocation() - O, N) / Den;
				if (Dist > 0.0 && Dist < 200.0)
				{
					// the plane is 100 x 100 before its scale: local X, Y in -50..50
					const FVector L = T.InverseTransformPosition(O + D * Dist);
					const FVector2D Px((L.X / 100.0 + 0.5) * BubblePxW, (L.Y / 100.0 + 0.5) * BubblePxH);
					for (int32 b = 0; b < 2; ++b) if (Buttons[b].bIsValid && Buttons[b].IsInside(Px)) On = b;
					if (Px.X >= 0.0 && Px.X <= BubblePxW && Px.Y >= 0.0 && Px.Y <= BubblePxH) Len = float(Dist);
				}
			}
			Laser[H]->SetVisibility(Len > 0.f);
			if (Len > 0.f)
			{
				Laser[H]->SetWorldLocationAndRotation(O + D * (Len * 0.5f), D.Rotation());
				Laser[H]->SetWorldScale3D(FVector(Len / 100.0, 0.003, 0.003));
			}
		}
		if (On != Hover[H]) { Hover[H] = On; bChanged = true; }
		const float Trig = P->Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		if (Trig < 0.3f) bArmed[H] = true;
		else if (Trig > 0.7f && bArmed[H])
		{
			bArmed[H] = false;
			if (On >= 0) { Pick(On); return; }
		}
	}
	if (bChanged) Redraw();
}

// ---- the beacon ------------------------------------------------------------------------------------------------------------

UOutlawsBeacon* UOutlawsBeacon::Get(const UObject* Context)
{
	const UWorld* W = GEngine && Context ? GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UOutlawsBeacon>() : nullptr;
}

bool UOutlawsBeacon::Setting(const FString& Row)
{
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const FString* X = S.Find(Row == TEXT("mp_announce") ? TEXT("MpAnnounce") : TEXT("MpListen"));
	return !X || X->ToBool();
}

void UOutlawsBeacon::Toggle(const FString& Row)
{
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const bool bOn = !Setting(Row);
	S.Add(Row == TEXT("mp_announce") ? TEXT("MpAnnounce") : TEXT("MpListen"), bOn ? TEXT("true") : TEXT("false"));
	AOutlawsLevelActor::WriteSettings(S);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON %s %s"), Row == TEXT("mp_announce") ? TEXT("announce") : TEXT("listen"), bOn ? TEXT("on") : TEXT("off"));
}

bool UOutlawsBeacon::HandOnBubble(const UObject* Context, int32 Hand)
{
	const UOutlawsBeacon* B = Get(Context);
	return B && IsValid(B->Bubble) && B->Bubble->GetKind() != EOutlawsBubbleKind::None && !B->Bubble->IsHidden() && B->Bubble->HandOn(Hand) >= 0;
}

void UOutlawsBeacon::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// off in automated runs (final_pass and the rest) unless asked for
	bEnabled = !FApp::IsUnattended() || OLFlags::Param(TEXT("OLBeacon"));
	if (!bEnabled) return;
	OLFlags::Value(TEXT("OLBeaconPort"), BeaconPort);
	MyId = FGuid::NewGuid().ToString(EGuidFormats::Short).Left(8);
	FString Uni;
	if (OLFlags::Value(TEXT("OLBeaconUnicast"), Uni))
	{
		FString Ip, PortText;
		if (Uni.Split(TEXT(":"), &Ip, &PortText)) SendTo.Emplace(Ip, FCString::Atoi(*PortText));
		else SendTo.Emplace(Uni, BeaconPort);
	}
	else
	{
		SendTo.Emplace(TEXT("255.255.255.255"), BeaconPort);
		// the subnet's own broadcast address (a /24 home network)
		if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			bool bBind = false;
			const TSharedRef<FInternetAddr> Me = SS->GetLocalHostAddr(*GLog, bBind);
			FString Ip = Me->ToString(false);
			int32 Dot = INDEX_NONE;
			if (Ip.FindLastChar(TEXT('.'), Dot) && !Ip.StartsWith(TEXT("127."))) SendTo.Emplace(Ip.Left(Dot) + TEXT(".255"), BeaconPort);
		}
	}
	BeaconTest(TestRole);
	StartedAt = FPlatformTime::Seconds();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON up: id %s, port %d, sending to %d address%s, announce %s, listen %s%s"), *MyId, BeaconPort, SendTo.Num(), SendTo.Num() == 1 ? TEXT("") : TEXT("es"),
		Setting(TEXT("mp_announce")) ? TEXT("on") : TEXT("off"), Setting(TEXT("mp_listen")) ? TEXT("on") : TEXT("off"), TestRole.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", test %s"), *TestRole));
}

void UOutlawsBeacon::Deinitialize()
{
	if (Socket)
	{
		Socket->Close();
		if (ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)) SS->DestroySocket(Socket);
		Socket = nullptr;
	}
	Super::Deinitialize();
}

UWorld* UOutlawsBeacon::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

TStatId UOutlawsBeacon::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsBeacon, STATGROUP_Tickables);
}

AOutlawsLevelActor* UOutlawsBeacon::FindLevel() const
{
	UWorld* W = GetTickableGameObjectWorld();
	if (!W) return nullptr;
	TActorIterator<AOutlawsLevelActor> It(W);
	return It ? *It : nullptr;
}

AOutlawsPlayer* UOutlawsBeacon::FindPlayer() const
{
	UWorld* W = GetTickableGameObjectWorld();
	return W ? Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(W, 0)) : nullptr;
}

bool UOutlawsBeacon::OpenSocket()
{
	if (Socket) return true;
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SS) return false;
	Socket = SS->CreateSocket(NAME_DGram, TEXT("OutlawsBeacon"), FNetworkProtocolTypes::IPv4);
	if (!Socket) return false;
	Socket->SetReuseAddr(true);
	Socket->SetBroadcast(true);
	Socket->SetNonBlocking(true);
	const TSharedRef<FInternetAddr> Any = SS->CreateInternetAddr();
	Any->SetAnyAddress();
	Any->SetPort(BeaconPort);
	if (!Socket->Bind(*Any))
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLBEACON could not listen on port %d"), BeaconPort);
		SS->DestroySocket(Socket);
		Socket = nullptr;
		bEnabled = false;
		return false;
	}
	return true;
}

void UOutlawsBeacon::Send(const FString& Msg, const FString& Ip, int32 Port)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Socket || !SS) return;
	const TSharedRef<FInternetAddr> To = SS->CreateInternetAddr();
	bool bValid = false;
	To->SetIp(*Ip, bValid);
	To->SetPort(Port);
	if (!bValid) return;
	const FTCHARToUTF8 Utf(*Msg);
	int32 Sent = 0;
	Socket->SendTo(reinterpret_cast<const uint8*>(Utf.Get()), Utf.Length(), Sent, *To);
}

void UOutlawsBeacon::Receive(double Now)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Socket || !SS) return;
	uint32 Pending = 0;
	for (int32 Guard = 0; Guard < 32 && Socket->HasPendingData(Pending); ++Guard)
	{
		TArray<uint8> Buf;
		Buf.SetNumZeroed(FMath::Min<uint32>(FMath::Max<uint32>(Pending, 1u), 2048u) + 1);
		int32 Read = 0;
		const TSharedRef<FInternetAddr> From = SS->CreateInternetAddr();
		if (!Socket->RecvFrom(Buf.GetData(), Buf.Num() - 1, Read, *From) || Read <= 0) break;
		Buf[Read] = 0;
		const FString Msg = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buf.GetData()));
		HandleMessage(Msg, From->ToString(false), From->GetPort(), Now);
	}
}

FString UOutlawsBeacon::MyState(FString& OutLevel, int32& OutPlayers, int32& OutMax) const
{
	AOutlawsLevelActor* L = FindLevel();
	OutLevel.Reset();
	OutPlayers = 1;
	OutMax = 1;
	if (!L) return FString();
	if (UOutlawsCoop::IsActive(L))
	{
		UOutlawsCoop* C = UOutlawsCoop::Get(L);
		const AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
		if (!C || !C->IsHost() || !S) return FString();          // (a client's game is not its to offer)
		OutPlayers = S->Connected;
		OutMax = S->MaxPlayers;
		OutLevel = S->StartSerial > 0 ? S->Level : FString();
		return S->StartSerial > 0 ? TEXT("coop_level") : TEXT("coop_lobby");
	}
	if (L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || L->IsPlayerDead()) return FString();
	OutLevel = L->LevelName;
	OutMax = UOutlawsCoop::MaxPlayers;
	return TEXT("single");
}

bool UOutlawsBeacon::MayListen(bool& bInPlay) const
{
	bInPlay = false;
	AOutlawsLevelActor* L = FindLevel();
	if (!L || UOutlawsCoop::IsActive(L) || L->IsRebuildPending() || L->IsMoviePlaying()) return false;
	if (L->IsFrontEnd()) return true;
	bInPlay = true;
	return !L->IsPlayerDead();
}

FString UOutlawsBeacon::OfferText(const FOutlawsHeardGame& G, bool& bSameBuild) const
{
	bSameBuild = G.Build == OutlawsBuildInfo::Commit();
	const FString Who = G.Name.IsEmpty() ? FString(TEXT("Someone nearby")) : G.Name;
	FString T;
	if (G.State.StartsWith(TEXT("coop"))) T = FString::Printf(TEXT("%s is hosting a co-op game (%d/%d)"), *Who, G.Players, G.Max);
	else if (!G.Level.IsEmpty()) T = FString::Printf(TEXT("%s is playing %s"), *Who, *AOutlawsLevelActor::LevelTitle(G.Level));
	else T = FString::Printf(TEXT("%s is playing alone"), *Who);
	if (G.bPassword) T += TEXT("  (password)");
	return T;
}

void UOutlawsBeacon::HandleMessage(const FString& Msg, const FString& FromIp, int32 FromPort, double Now)
{
	TArray<FString> F;
	Msg.ParseIntoArray(F, TEXT("\t"), false);
	if (F.Num() < 2 || F[1] == MyId) return;                 // (our own broadcast back)
	const FString Key = FString::Printf(TEXT("%s:%d"), *FromIp, FromPort);
	if (F[0] == TEXT("OLB1") && F.Num() >= 10)
	{
		bool bInPlay = false;
		if (!Setting(TEXT("mp_listen"))) return;
		FOutlawsHeardGame& G = Heard.FindOrAdd(Key);
		const bool bNew = G.Key.IsEmpty() || StateKey(G) != F[3] + TEXT("|") + F[4];
		G.Key = Key;
		G.Ip = FromIp;
		G.BeaconPort = FromPort;
		G.Id = F[1];
		G.Name = F[2];
		G.State = F[3];
		G.Level = F[4];
		G.Players = FCString::Atoi(*F[5]);
		G.Max = FCString::Atoi(*F[6]);
		G.bPassword = F[7] == TEXT("1");
		G.Build = F[8];
		G.GamePort = FCString::Atoi(*F[9]);
		G.HeardAt = Now;
		if (bNew)
		{
			G.FirstAt = Now;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON heard %s at %s: %s %s (%d/%d), build %s, game port %d%s"), *G.Name, *Key, *G.State, *G.Level, G.Players, G.Max, *G.Build, G.GamePort,
				MayListen(bInPlay) ? TEXT("") : TEXT(" (not listening now)"));
		}
		return;
	}
	if (F[0] == TEXT("OLK1") && F.Num() >= 3)
	{
		// someone wants to join this game: a co-op host lets them straight in; a game played alone asks its player
		FString Lv;
		int32 Pl = 0, Mx = 0;
		const FString State = MyState(Lv, Pl, Mx);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON knock from %s (%s), this game %s"), *F[2], *Key, State.IsEmpty() ? TEXT("not joinable") : *State);
		if (State.StartsWith(TEXT("coop")))
		{
			Send(FString::Printf(TEXT("OLR1\t%s\t%s\tyes"), *MyId, *FindLevel()->MpPlayerName), FromIp, FromPort);
			return;
		}
		if (State != TEXT("single") || bOpening || !KnockFrom.IsEmpty())
		{
			Send(FString::Printf(TEXT("OLR1\t%s\t%s\tno"), *MyId, FindLevel() ? *FindLevel()->MpPlayerName : TEXT("")), FromIp, FromPort);
			return;
		}
		KnockFrom = Key;
		KnockName = F[2].IsEmpty() ? FString(TEXT("Someone nearby")) : F[2];
		ShowBubble(EOutlawsBubbleKind::Knocked, FString::Printf(TEXT("%s wants to join your game"), *KnockName), TEXT("Accept"), TEXT("Not now"));
		return;
	}
	if (F[0] == TEXT("OLR1") && F.Num() >= 4 && Key == JoinKey)
	{
		const FString Who = F[2].IsEmpty() ? FString(TEXT("They")) : F[2];
		if (F[3] == TEXT("yes"))
		{
			JoinYesAt = Now;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON %s said yes: joining once their game is open"), *Who);
			ShowBubble(EOutlawsBubbleKind::Opening, FString::Printf(TEXT("%s said yes. Joining..."), *Who), FString(), FString());
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON %s said not now"), *Who);
			JoinKey.Reset();
			ShowBubble(EOutlawsBubbleKind::Refused, FString::Printf(TEXT("%s said not now"), *Who), FString(), TEXT("X"));
		}
	}
}

void UOutlawsBeacon::ShowBubble(EOutlawsBubbleKind Kind, const FString& Text, const FString& Yes, const FString& No)
{
	UWorld* W = GetTickableGameObjectWorld();
	if (!W) return;
	if (!IsValid(Bubble) || Bubble->GetWorld() != W)
	{
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Bubble = W->SpawnActor<AOutlawsJoinBubble>(AOutlawsJoinBubble::StaticClass(), FTransform::Identity, SP);
		if (!Bubble) return;
		Bubble->OnPick = [this](int32 Button) { OnPick(Button); };
	}
	if (Bubble->GetKind() == Kind && Bubble->GetText() == Text) return;
	Bubble->Show(Kind, Text, Yes, No);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON bubble: \"%s\" [%s] [%s]"), *Text, *Yes, *No);
}

void UOutlawsBeacon::HideBubble()
{
	if (IsValid(Bubble) && Bubble->GetKind() != EOutlawsBubbleKind::None)
	{
		Bubble->Show(EOutlawsBubbleKind::None, FString(), FString(), FString());
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON bubble hidden"));
	}
	Offered.Reset();
}

void UOutlawsBeacon::OnPick(int32 Button)
{
	const double Now = FPlatformTime::Seconds();
	const EOutlawsBubbleKind Kind = IsValid(Bubble) ? Bubble->GetKind() : EOutlawsBubbleKind::None;
	FOutlawsHeardGame* G = Heard.Find(Offered);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON picked %s on \"%s\""), Button == 0 ? TEXT("yes") : TEXT("no"), IsValid(Bubble) ? *Bubble->GetText() : TEXT(""));
	switch (Kind)
	{
	case EOutlawsBubbleKind::Offer:
	case EOutlawsBubbleKind::Leave:
		if (Button == 1 || !G)
		{
			if (G) { Dismissed.Add(G->Key, StateKey(*G)); DismissedAt.Add(G->Key, Now); }
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON dismissed %s (%s) until its game changes"), G ? *G->Name : TEXT("?"), G ? *StateKey(*G) : TEXT(""));
			HideBubble();
			return;
		}
		if (G->Build != OutlawsBuildInfo::Commit())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON %s plays a different version (%s, this %s): no join"), *G->Name, *G->Build, OutlawsBuildInfo::Commit());
			return;
		}
		if (Kind == EOutlawsBubbleKind::Offer && FindLevel() && !FindLevel()->IsFrontEnd())
		{
			const FString Key = G->Key;
			ShowBubble(EOutlawsBubbleKind::Leave, FString::Printf(TEXT("Leave your game and join %s? Your game autosaves."), *G->Name), TEXT("Join"), TEXT("X"));
			Offered = Key;
			return;
		}
		if (Kind == EOutlawsBubbleKind::Leave)
		{
			AOutlawsLevelActor* L = FindLevel();
			const bool bSaved = L && L->SaveGame(-1);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON leaving this game for %s: autosave %s"), *G->Name, bSaved ? TEXT("written") : TEXT("FAILED"));
		}
		StartJoin(*G, Now);
		return;
	case EOutlawsBubbleKind::Knocked:
	{
		FString Ip, PortText;
		KnockFrom.Split(TEXT(":"), &Ip, &PortText);
		AOutlawsLevelActor* L = FindLevel();
		const FString Me = L ? L->MpPlayerName : FString();
		if (Button == 1)
		{
			Send(FString::Printf(TEXT("OLR1\t%s\t%s\tno"), *MyId, *Me), Ip, FCString::Atoi(*PortText));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON not now to %s"), *KnockName);
			KnockFrom.Reset();
			HideBubble();
			return;
		}
		// Accept: the game autosaves and opens for co-op from that save, alone; the knocker joins it (R7's late join)
		Send(FString::Printf(TEXT("OLR1\t%s\t%s\tyes"), *MyId, *Me), Ip, FCString::Atoi(*PortText));
		const bool bSaved = L && L->SaveGame(-1);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON accepted %s: autosave %s, reopening as a co-op host"), *KnockName, bSaved ? TEXT("written") : TEXT("FAILED"));
		ShowBubble(EOutlawsBubbleKind::Opening, FString::Printf(TEXT("Opening your game to %s..."), *KnockName), FString(), FString());
		bOpening = true;
		OpeningAt = Now;
		KnockFrom.Reset();
		if (UOutlawsCoop* C = L ? UOutlawsCoop::Get(L) : nullptr) C->HostFromSave(-1);
		return;
	}
	case EOutlawsBubbleKind::Asking:
	case EOutlawsBubbleKind::Refused:
		if (G && Kind == EOutlawsBubbleKind::Refused) { Dismissed.Add(G->Key, StateKey(*G)); DismissedAt.Add(G->Key, Now); }
		JoinKey.Reset();
		HideBubble();
		return;
	default:
		return;
	}
}

void UOutlawsBeacon::StartJoin(const FOutlawsHeardGame& G, double Now)
{
	JoinKey = G.Key;
	Offered = G.Key;
	if (G.State.StartsWith(TEXT("coop")))
	{
		ConnectTo(G);
		return;
	}
	AOutlawsLevelActor* L = FindLevel();
	Send(FString::Printf(TEXT("OLK1\t%s\t%s"), *MyId, L ? *L->MpPlayerName : TEXT("")), G.Ip, G.BeaconPort);
	JoinAskedAt = Now;
	JoinYesAt = 0.0;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON knocked on %s's game (%s)"), *G.Name, *G.Key);
	ShowBubble(EOutlawsBubbleKind::Asking, FString::Printf(TEXT("Asking %s..."), *G.Name), FString(), TEXT("X"));
}

void UOutlawsBeacon::ConnectTo(const FOutlawsHeardGame& G)
{
	AOutlawsLevelActor* L = FindLevel();
	UOutlawsCoop* C = L ? UOutlawsCoop::Get(L) : nullptr;
	const FString Address = FString::Printf(TEXT("%s:%d"), *G.Ip, G.GamePort);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON joining %s at %s (the co-op join)"), *G.Name, *Address);
	JoinKey.Reset();
	JoinYesAt = 0.0;
	HideBubble();
	// (the join page shown: the co-op join with the address filled in, as typed there)
	if (AOutlawsPlayer* P = FindPlayer()) P->CoopShowPage(TEXT("#coopjoin"), true);
	if (C) C->Join(Address, true);
}

void UOutlawsBeacon::UpdateBubble(double Now)
{
	// a re-host under way: its lobby up, straight into the level from the autosave, alone (the joiner comes in beside)
	if (bOpening)
	{
		AOutlawsLevelActor* L = FindLevel();
		UOutlawsCoop* C = L ? UOutlawsCoop::Get(L) : nullptr;
		if (C && C->StartAloneFromLobby())
		{
			bOpening = false;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON reopened for co-op: the lobby starts from the autosave"));
			HideBubble();
		}
		else if (Now - OpeningAt > 40.0)
		{
			bOpening = false;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON reopening for co-op did not reach the lobby in 40 s"));
		}
		return;
	}
	// a join under way: connect once the host's beacon says co-op
	if (!JoinKey.IsEmpty())
	{
		const FOutlawsHeardGame* G = Heard.Find(JoinKey);
		if (JoinYesAt > 0.0 && G && G->State.StartsWith(TEXT("coop"))) { ConnectTo(*G); return; }
		if (JoinYesAt > 0.0 && Now - JoinYesAt > 30.0)
		{
			JoinKey.Reset();
			ShowBubble(EOutlawsBubbleKind::Refused, TEXT("Their game did not open in time"), FString(), TEXT("X"));
		}
		else if (JoinYesAt == 0.0 && Now - JoinAskedAt > 30.0)
		{
			JoinKey.Reset();
			ShowBubble(EOutlawsBubbleKind::Refused, FString::Printf(TEXT("No answer from %s"), G ? *G->Name : TEXT("them")), FString(), TEXT("X"));
		}
		return;
	}
	const EOutlawsBubbleKind Kind = IsValid(Bubble) ? Bubble->GetKind() : EOutlawsBubbleKind::None;
	if (Kind == EOutlawsBubbleKind::Knocked || Kind == EOutlawsBubbleKind::Refused || Kind == EOutlawsBubbleKind::Leave || Kind == EOutlawsBubbleKind::Opening) return;
	bool bInPlay = false;
	if (!MayListen(bInPlay) || !Setting(TEXT("mp_listen")))
	{
		if (Kind == EOutlawsBubbleKind::Offer) HideBubble();
		return;
	}
	// the newest game heard and not dismissed
	const FOutlawsHeardGame* Best = nullptr;
	for (const TPair<FString, FOutlawsHeardGame>& Kv : Heard)
	{
		const FOutlawsHeardGame& G = Kv.Value;
		const FString* D = Dismissed.Find(G.Key);
		if (D && *D == StateKey(G) && Now - DismissedAt.FindRef(G.Key) < DismissFor) continue;
		if (!Best || G.FirstAt > Best->FirstAt) Best = &G;
	}
	if (!Best)
	{
		if (Kind == EOutlawsBubbleKind::Offer) HideBubble();
		return;
	}
	// in play: only after a quiet second (the triggers let go), so it never comes up under a shot
	AOutlawsPlayer* P = FindPlayer();
	if (bInPlay && P && P->bVR && (P->Axis(TEXT("OLVR_LeftTrigger")) > 0.3f || P->Axis(TEXT("OLVR_RightTrigger")) > 0.3f)) QuietSince = Now;
	if (bInPlay && Kind != EOutlawsBubbleKind::Offer && Now - QuietSince < QuietNeeded) return;
	bool bSame = true;
	const FString Text = OfferText(*Best, bSame);
	if (Offered != Best->Key || Kind != EOutlawsBubbleKind::Offer || Bubble->GetText() != Text)
	{
		Offered = Best->Key;
		ShowBubble(EOutlawsBubbleKind::Offer, Text, bSame ? FString(TEXT("Join")) : FString(TEXT("different version")), TEXT("X"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON offered %s %.1f s after first hearing it%s"), *Best->Name, Now - Best->FirstAt, bInPlay ? TEXT(" (in play, after a quiet second)") : TEXT(" (main menu)"));
	}
}

void UOutlawsBeacon::Tick(float DeltaTime)
{
	OL_PERF_SCOPE("Other ours");
	if (!bEnabled) return;
	if (!OpenSocket()) return;
	const double Now = FPlatformTime::Seconds();
	Receive(Now);
	for (auto It = Heard.CreateIterator(); It; ++It)
	{
		if (Now - It.Value().HeardAt > HeardFor)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON %s gone quiet"), *It.Value().Name);
			It.RemoveCurrent();
		}
	}
	if (Now >= NextBeacon)
	{
		NextBeacon = Now + BeaconEvery;
		FString Lv;
		int32 Pl = 1, Mx = 1;
		const FString State = Setting(TEXT("mp_announce")) ? MyState(Lv, Pl, Mx) : FString();
		if (!State.IsEmpty())
		{
			int32 MyGamePort = UOutlawsCoop::Port;
			FParse::Value(FCommandLine::Get(), TEXT("port="), MyGamePort);
			FString Build = OutlawsBuildInfo::Commit();
			OLFlags::Value(TEXT("OLBeaconFakeBuild"), Build);          // (tests: a different version)
			AOutlawsLevelActor* L = FindLevel();
			const FString Msg = FString::Printf(TEXT("OLB1\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%s\t%d"), *MyId, L ? *L->MpPlayerName : TEXT(""), *State, *Lv, Pl, Mx, 0, *Build, MyGamePort);
			for (const TPair<FString, int32>& To : SendTo) Send(Msg, To.Key, To.Value);
			static FString LastLogged;
			if (LastLogged != State + Lv) { LastLogged = State + Lv; UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON announcing: %s %s (%d/%d)"), *State, *Lv, Pl, Mx); }
		}
	}
	UpdateBubble(Now);
#if WITH_OUTLAWS_TESTS
	if (!TestRole.IsEmpty()) TickTest(Now);
#endif
}

// ---- co-op: the knocked game reopens for co-op (the lobby's Load path) -----------------------------------------------------

void UOutlawsCoop::HostFromSave(int32 Slot)
{
	Host();
	LobbyLoadSlot = Slot;           // (after Host, whose session reset clears it)
	bStartAloneFromSave = true;
}

bool UOutlawsCoop::StartAloneFromLobby()
{
	// (its lobby up a second: the session there and the map built under it)
	static double LobbySeenAt = 0.0;
	AOutlawsCoopSession* S = GetSession();
	AOutlawsLevelActor* L = FindLevel();
	if (!bStartAloneFromSave || Mode != EOutlawsCoopMode::Host || !S || S->StartSerial > 0 || !L || !L->HasActorBegunPlay()) { LobbySeenAt = 0.0; return false; }
	const double Now = FPlatformTime::Seconds();
	if (LobbySeenAt == 0.0) LobbySeenAt = Now;
	if (Now - LobbySeenAt < 1.0) return false;
	LobbySeenAt = 0.0;
	bStartAloneFromSave = false;
	StartGame(true);
	return true;
}

// ---- -OLBeaconTest=target | joiner | play ----------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsBeacon::TickTest(double Now)
{
	auto Shot = [](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: screenshot %s.png"), Name);
	};
	auto Done = [this](const TCHAR* Why)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: done (%s): %s"), *TestRole, Why);
		TestStep = 99;
		FGenericPlatformMisc::RequestExit(false);
	};
	const EOutlawsBubbleKind Kind = IsValid(Bubble) ? Bubble->GetKind() : EOutlawsBubbleKind::None;
	AOutlawsLevelActor* L = FindLevel();
	if (TestStep >= 99) return;
	if (Now - StartedAt > 200.0) { Done(TEXT("TIMED OUT")); return; }
	if (TestRole == TEXT("target"))
	{
		// plays alone; after 50 s goes to another level (the dismissed bubble comes back); accepts a knock
		if (TestStep == 0 && L && !L->IsFrontEnd() && Now - StartedAt > 50.0 && L->LevelName == TEXT("hideout"))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: the target changes level"));
			L->JumpToLevel(TEXT("town"));
			TestStep = 1;
		}
		if (Kind == EOutlawsBubbleKind::Knocked && TestAt == 0.0) TestAt = Now;
		if (Kind == EOutlawsBubbleKind::Knocked && TestAt > 0.0 && Now - TestAt > 1.5 && TestStep < 10) { Shot(TEXT("ol_bubble_accept")); TestStep = 10; }
		if (TestStep == 10 && Now - TestAt > 2.5) { Bubble->Pick(0); TestStep = 11; }
		if (TestStep == 11 && L && UOutlawsCoop::IsActive(L))
		{
			UOutlawsCoop* C = UOutlawsCoop::Get(L);
			const AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
			if (S && S->Connected >= 2 && S->StartSerial > 0) { TestAt = Now; TestStep = 12; UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: the joiner is in this game (%d players, level %s)"), S->Connected, *S->Level); }
		}
		if (TestStep == 12 && Now - TestAt > 15.0) Done(TEXT("hosted the joiner"));
		return;
	}
	if (TestRole == TEXT("play"))
	{
		if (Kind == EOutlawsBubbleKind::Offer && TestStep == 0) { TestAt = Now; TestStep = 1; }
		if (TestStep == 1 && Now - TestAt > 1.5) { Shot(TEXT("ol_bubble_play")); TestStep = 2; }
		if (TestStep == 2 && Now - TestAt > 3.0) Done(TEXT("offered in play"));
		return;
	}
	if (TestRole != TEXT("joiner")) return;
	switch (TestStep)
	{
	case 0:      // the offer on the main menu
		if (Kind != EOutlawsBubbleKind::Offer) return;
		TestAt = Now;
		TestStep = 1;
		return;
	case 1:
		if (Now - TestAt < 1.5) return;
		Shot(TEXT("ol_bubble_menu"));
		TestStep = 2;
		return;
	case 2:      // X: hidden, and not back while their game is the same
		if (Now - TestAt < 2.5) return;
		Bubble->Pick(1);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: X picked: bubble %s"), Kind == EOutlawsBubbleKind::None || (IsValid(Bubble) && Bubble->GetKind() == EOutlawsBubbleKind::None) ? TEXT("hidden") : TEXT("STILL SHOWN"));
		TestAt = Now;
		TestStep = 3;
		return;
	case 3:
		if (Kind == EOutlawsBubbleKind::Offer)
		{
			const FOutlawsHeardGame* G = Heard.Find(Offered);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: the bubble came back %.1f s after X (%s)"), Now - TestAt, G ? *StateKey(*G) : TEXT("?"));
			TestAt = Now;
			TestStep = 4;
		}
		return;
	case 4:      // a game of a different version heard: its bubble says so
		if (Now - TestAt < 1.5) return;
		HandleMessage(FString::Printf(TEXT("OLB1\tfake0001\tOld Timer\tsingle\tcanyon\t1\t8\t0\toldbuild\t7777")), TEXT("127.0.0.9"), 7778, Now);
		TestAt = Now;
		TestStep = 5;
		return;
	case 5:
		if (Now - TestAt < 1.0) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: the other version's bubble: \"%s\" %s"), IsValid(Bubble) ? *Bubble->GetText() : TEXT(""),
			IsValid(Bubble) && Bubble->GetText().Contains(TEXT("Old Timer")) ? TEXT("(Join reads \"different version\")") : TEXT("NOT SHOWN"));
		Shot(TEXT("ol_bubble_version"));
		TestStep = 6;
		TestAt = Now;
		return;
	case 6:
		if (Now - TestAt < 1.0) return;
		Bubble->Pick(0);                 // (a different version: nothing happens)
		Bubble->Pick(1);
		Heard.Remove(TEXT("127.0.0.9:7778"));
		TestStep = 7;
		TestAt = Now;
		return;
	case 7:      // the real one again, Join: the knock, the target accepts, the co-op join
		if (Kind != EOutlawsBubbleKind::Offer || Now - TestAt < 1.0) return;
		Bubble->Pick(0);
		TestStep = 8;
		return;
	case 8:
		if (L && UOutlawsCoop::IsActive(L) && UOutlawsCoop::Get(L)->IsClient())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: joined as a co-op client"));
			TestStep = 9;
			TestAt = Now;
		}
		return;
	case 9:
		if (L && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && Now - TestAt > 8.0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLBEACON test: in the host's level %s"), *L->LevelName);
			TestStep = 10;
			TestAt = Now;
		}
		return;
	case 10:
		if (Now - TestAt > 10.0) Done(TEXT("joined"));
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
