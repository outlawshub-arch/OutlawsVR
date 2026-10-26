// QSPEED-1 (2026-10-21): every bad guy driven from one tick, and his CharacterMovement run only while he moves.
//
// Before, each bad guy was two entries in the engine's tick queue (his actor tick and his CharacterMovement's), ~550 at
// bad guys x3.25, each with its task, its delta and its interval bookkeeping, and the movement ran for every man whose
// tick came up, standing or not.  Now:
//  - one tick function per world (TG_PrePhysics, as theirs was, and after the level actor's tick: the lifts and doors it
//    moves carry the men standing on them in the same frame, as the movement's own tick prerequisite on its base did)
//    ticks every bad guy, then his movement, in the order they began play;
//  - each keeps his own two schedules as the engine kept them: the actor's tick and the movement's enabled or not
//    (SetActorTickEnabled, SetComponentTickEnabled, Activate / Deactivate: FROZEN, WANDER, dormant, the client's puppets),
//    their intervals (UpdateTickRate's 0.2 / 0.5 s, the culled puppet's 0.25 s), the delta an interval tick is handed and
//    the bad guys' pace (CustomTimeDilation);
//  - a man whose movement comes due while he is at rest skips it: walking on a walkable floor, no velocity, no input, no
//    launch, impulse, force or requested velocity, not just teleported, no floor check forced, and his capsule and the
//    floor he stands on (its place, its turn, its collision) where they were after his last move.  A standing man's move
//    changes nothing then (the floor is the cached one, no sweep), so this is that move left out.  Anything that would
//    move him breaks the rest the same frame: his own walk (MoveAlong's input), a hit's knock (the launch), a push, a
//    teleport (the unstuck, a load, Appear, the wander), the sector carry (it moves the capsule), a lift or door under him
//    moving or losing its collision, a fall (not walking).
// ol.enemy.MoveAtRest 0 runs every movement that comes due (the driver still ticks them), 2 (tests) runs the skipped ones
// anyway and logs anything they changed; ol.enemy.Driver 0 hands them back to the engine's tick queue in play (and 1 takes
// them again: -OLCpuBench / -OLFreezeSoak with -OLEnemyDriverAB=<s> switch it every s seconds and log the game thread of
// each way); ol.enemy.DriverPasses 1 thinks and moves each man in turn (2, the default: every think, then every move);
// -OLOldEnemyDriver: the engine ticks them as before, from the start.
// The first frame matters: the bad guys are spawned in the level actor's tick and the far freeze looks up their sectors on
// their first tick, so the driver is set after the level actor from the moment it is made.

#include "OutlawsEnemy.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "Engine/EngineBaseTypes.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CsvProfiler.h"

CSV_DECLARE_CATEGORY_EXTERN(Outlaws);

namespace
{
	TAutoConsoleVariable<int32> CVarEnemyMoveAtRest(TEXT("ol.enemy.MoveAtRest"), 1,
		TEXT("QSPEED-1: 1 a bad guy at rest skips his CharacterMovement until something would move him; 0 it runs whenever its tick is due; 2 (tests) runs it anyway and logs any change it made."));
	TAutoConsoleVariable<int32> CVarEnemyDriver(TEXT("ol.enemy.Driver"), 1,
		TEXT("QSPEED-1: 1 the enemy driver ticks the bad guys; 0 the engine's tick queue does, as before (switchable in play, for an A/B in one run)."));
	TAutoConsoleVariable<int32> CVarEnemyDriverPasses(TEXT("ol.enemy.DriverPasses"), 2,
		TEXT("QSPEED-1: 2 every bad guy thinks, then every one moves; 1 each thinks and moves in turn."));
	TAutoConsoleVariable<int32> CVarEnemyDriverAfterLevel(TEXT("ol.enemy.DriverAfterLevel"), 1,
		TEXT("QSPEED-1: 1 the bad guys tick after the level actor (its lifts and doors moved first); 0 in no set order with it."));
	// both ways (the engine's ticks or the driver's): the bad guys' ticks and moves, for the A/B (AOutlawsEnemy::DriverStats)
	int64 GTickCalls = 0, GMoveCalls = 0, GRestSkips = 0, GRestChecked = 0, GRestChanged = 0;
	uint64 GStatsFrame = 0;
}

int64 GTickByInterval[4] = { 0, 0, 0, 0 };
void AOutlawsEnemy::CountTick(float Interval) { ++GTickCalls; ++GTickByInterval[Interval <= 0.f ? 0 : Interval <= 0.15f ? 1 : Interval <= 0.3f ? 2 : 3]; }
void UOutlawsEnemyMovement::CountMove() { ++GMoveCalls; }

FString AOutlawsEnemy::DriverStats()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldEnemyDriver"));
	const uint64 Frames = GStatsFrame > 0 && GFrameCounter > GStatsFrame ? GFrameCounter - GStatsFrame : 0;
	FString S = Frames == 0 ? FString(TEXT("enemy ticks: no frames yet"))
		: FString::Printf(TEXT("enemy ticks (%s) a frame: %.1f bad guy ticks, %.1f moves run, %.1f moves at rest skipped over %d frames"),
			bOld ? TEXT("engine, -OLOldEnemyDriver") : TEXT("driver"), double(GTickCalls) / Frames, double(GMoveCalls) / Frames, double(GRestSkips) / Frames, int32(Frames));
	if (Frames > 0) S += FString::Printf(TEXT(" (by his tick interval: every frame %.1f, 0.1 s %.1f, 0.2 s %.1f, 0.5 s %.1f)"),
		double(GTickByInterval[0]) / Frames, double(GTickByInterval[1]) / Frames, double(GTickByInterval[2]) / Frames, double(GTickByInterval[3]) / Frames);
	for (int64& C : GTickByInterval) C = 0;
	if (GRestChecked > 0) S += FString::Printf(TEXT("; rest check: %lld skipped moves run anyway, %lld changed anything (expect 0)"), GRestChecked, GRestChanged);
	GTickCalls = GMoveCalls = GRestSkips = 0;
	GStatsFrame = GFrameCounter;
	return S;
}

struct FOutlawsEnemyDriver
{
	/** One tick function's schedule, as the engine's tick queue keeps it (TickTaskManager.cpp: the enabled list, the
	 *  cooling-down list, CalculateDeltaTime). */
	struct FSched
	{
		double Next = -1.0;          // -1: in the every-frame list; else the world time its interval comes due
		double Last = -1.0;          // the last interval tick's world time (-1: none since it ticked every frame)
		float Interval = 0.f;
		bool bOn = false;

		/** Due this frame; OutDt the delta it is handed (before the actor's dilation). */
		bool Due(bool bEnabled, float InInterval, double Now, float WorldDt, float& OutDt)
		{
			if (!bEnabled)
			{
				bOn = false;
				Last = -1.0;         // (a disabled tick function forgets its last tick)
				return false;
			}
			if (!bOn)
			{
				bOn = true;          // (enabled again: into the every-frame list, as AddTickFunction puts it)
				Next = -1.0;
			}
			if (Next < 0.0)
			{
				if (InInterval > 0.f)
				{
					// queued from the every-frame list and moved to the cooldown list: an interval tick from here
					OutDt = Last >= 0.0 ? float(Now - Last) : WorldDt;
					Last = Now;
					Next = Now + InInterval;
					Interval = InInterval;
				}
				else
				{
					OutDt = WorldDt;
					Last = -1.0;
				}
				return true;
			}
			if (InInterval != Interval)
			{
				// (UpdateTickIntervalAndCoolDown: cooling down, rescheduled for the new interval from now)
				Interval = InInterval;
				Next = InInterval > 0.f ? Now + InInterval : -1.0;
				if (Next >= 0.0) return false;
				OutDt = WorldDt;
				Last = -1.0;
				return true;
			}
			if (Now < Next) return false;
			OutDt = Last >= 0.0 ? float(Now - Last) : WorldDt;
			Last = Now;
			Next += Interval;                                   // (the overrun credited, as the cooldown list does)
			if (Next <= Now) Next = Now;
			return true;
		}
	};

	struct FEntry
	{
		// (plain pointers: a bad guy leaves the list in his EndPlay, before he can go)
		AOutlawsEnemy* E = nullptr;
		UOutlawsEnemyMovement* M = nullptr;
		FSched Actor, Move;
		// where his last move left him (the rest test)
		bool bRestValid = false;
		FVector RestLoc = FVector::ZeroVector;
		FQuat RestQuat = FQuat::Identity;
		TWeakObjectPtr<UPrimitiveComponent> RestBase;
		bool bRestHadBase = false;
		FVector RestBaseLoc = FVector::ZeroVector;
		FQuat RestBaseQuat = FQuat::Identity;
	};

	class FTick : public FTickFunction
	{
	public:
		FOutlawsEnemyDriver* Owner = nullptr;
		virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override
		{
			Owner->Run(DeltaTime, TickType);
		}
		virtual FString DiagnosticMessage() override { return TEXT("OutlawsEnemyDriver[Tick]"); }
		virtual FName DiagnosticContext(bool bDetailed) override { return FName(TEXT("OutlawsEnemyDriver")); }
	};

	TWeakObjectPtr<UWorld> World;
	FTick Tick;
	TArray<FEntry> Entries;
	bool bHoles = false;
	TWeakObjectPtr<AOutlawsLevelActor> PrereqLevel;
	// counts for the log line and the csv
	int32 FrameActorTicks = 0, FrameMoves = 0, FrameRests = 0;
	uint32 FrameNo = 0;
	int64 SumActorTicks = 0, SumMoves = 0, SumRests = 0, SumFrames = 0;
	double NextLogAt = 0.0;

	static TArray<TUniquePtr<FOutlawsEnemyDriver>>& All()
	{
		static TArray<TUniquePtr<FOutlawsEnemyDriver>>* Drivers = new TArray<TUniquePtr<FOutlawsEnemyDriver>>();     // (never freed: a world's cleanup takes its own)
		return *Drivers;
	}

	/** The driver in use for this game world (not -OLOldEnemyDriver). */
	static bool Wanted(const UWorld* W)
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldEnemyDriver"));
		return !bOld && W && W->IsGameWorld();
	}
	/** ... and driving now (ol.enemy.Driver 0 hands them back to the engine's tick queue: the same-run A/B). */
	static bool& Driving() { static bool bOn = true; return bOn; }
	static bool Drives(const UWorld* W) { return Wanted(W) && Driving(); }

	/** A tick function out of the engine's queue: its enabled flag kept as plain on or off (one taken out while cooling down
	 *  between interval ticks keeps that state, which the queue will not take back). */
	static void Leave(FTickFunction& F)
	{
		if (!F.IsTickFunctionRegistered()) return;
		const bool bOn = F.IsTickFunctionEnabled();
		F.UnRegisterTickFunction();
		F.SetTickFunctionEnable(bOn);
	}

	/** ol.enemy.Driver switched: every bad guy's two tick functions back into the engine's queue, or out of it, at a frame's edge. */
	static void SetDriving(bool bOn)
	{
		Driving() = bOn;
		int32 Num = 0;
		for (const TUniquePtr<FOutlawsEnemyDriver>& D : All())
		{
			for (FEntry& En : D->Entries)
			{
				AOutlawsEnemy* E = En.E;
				if (!E || !IsValidChecked(E) || !E->GetLevel()) continue;
				UCharacterMovementComponent* M = En.M;
				++Num;
				if (bOn)
				{
					Leave(E->PrimaryActorTick);
					if (M) Leave(M->PrimaryComponentTick);
					En.Actor = FSched();
					En.Move = FSched();
					En.bRestValid = false;
				}
				else
				{
					if (!E->PrimaryActorTick.IsTickFunctionRegistered()) E->PrimaryActorTick.RegisterTickFunction(E->GetLevel());
					if (M && M->IsRegistered() && !M->PrimaryComponentTick.IsTickFunctionRegistered()) M->PrimaryComponentTick.RegisterTickFunction(E->GetLevel());
				}
			}
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT enemy driver: %s (%d bad guys)"), bOn ? TEXT("driving again") : TEXT("handed back to the engine's tick queue"), Num);
	}

	static FOutlawsEnemyDriver* For(UWorld* W, bool bMake)
	{
		for (const TUniquePtr<FOutlawsEnemyDriver>& D : All()) if (D->World.Get() == W) return D.Get();
		if (!bMake || !W || !W->PersistentLevel) return nullptr;
		static bool bCleanupHooked = false;
		if (!bCleanupHooked)
		{
			bCleanupHooked = true;
			// (the tick function leaves the world's level before the level goes)
			FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* Gone, bool, bool)
			{
				TArray<TUniquePtr<FOutlawsEnemyDriver>>& Ds = All();
				for (int32 i = Ds.Num() - 1; i >= 0; --i)
				{
					if (Ds[i]->World.Get() == Gone || !Ds[i]->World.IsValid())
					{
						Ds[i]->Tick.UnRegisterTickFunction();
						Ds.RemoveAt(i);
					}
				}
			});
		}
		TUniquePtr<FOutlawsEnemyDriver> D = MakeUnique<FOutlawsEnemyDriver>();
		D->World = W;
		D->Tick.Owner = D.Get();
		D->Tick.TickGroup = TG_PrePhysics;
		D->Tick.bCanEverTick = true;
		D->Tick.bStartWithTickEnabled = true;
		D->Tick.bTickEvenWhenPaused = false;
		D->Tick.bAllowTickOnDedicatedServer = true;
		D->Tick.bRunOnAnyThread = false;
		D->Tick.RegisterTickFunction(W->PersistentLevel);
		// after the level actor from the first frame: it spawns the bad guys (so it is there already), and on their first tick
		// its culling must be ready (the far freeze looks their sectors up then), as their own ticks, spawned in its tick, were
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It)
		{
			D->PrereqLevel = *It;
			D->Tick.AddPrerequisite(*It, It->PrimaryActorTick);
			break;
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT enemy driver: on for %s (one tick for every bad guy; movement at rest skipped: ol.enemy.MoveAtRest %d)"),
			*W->GetName(), CVarEnemyMoveAtRest.GetValueOnGameThread());
		FOutlawsEnemyDriver* Raw = D.Get();
		All().Add(MoveTemp(D));
		return Raw;
	}

	static void Add(AOutlawsEnemy* E)
	{
		FOutlawsEnemyDriver* D = For(E->GetWorld(), true);
		if (!D) return;
		FEntry& En = D->Entries.AddDefaulted_GetRef();
		En.E = E;
		En.M = Cast<UOutlawsEnemyMovement>(E->GetCharacterMovement());
	}

	static void Remove(AOutlawsEnemy* E)
	{
		FOutlawsEnemyDriver* D = For(E->GetWorld(), false);
		if (!D) return;
		for (FEntry& En : D->Entries)
		{
			if (En.E == E)
			{
				En.E = nullptr;
				En.M = nullptr;              // (taken out after the frame's pass: one may go mid-pass)
				D->bHoles = true;
			}
		}
	}

	bool AtRest(const FEntry& En, const AOutlawsEnemy* E, const UOutlawsEnemyMovement* M) const
	{
		if (!En.bRestValid || M->MovementMode != MOVE_Walking || !M->Velocity.IsZero() || M->bJustTeleported || M->bForceNextFloorCheck
			|| M->bAlwaysCheckFloor || !E->GetPendingMovementInputVector().IsZero() || M->HasPendingMove() || M->HasRootMotionSources()
			|| !M->CurrentFloor.IsWalkableFloor())
		{
			return false;
		}
		const USceneComponent* C = M->UpdatedComponent;
		if (!C || C->GetComponentLocation() != En.RestLoc || !C->GetComponentQuat().Equals(En.RestQuat, 0.f)) return false;
		const UPrimitiveComponent* Base = E->GetMovementBase();
		if (Base != En.RestBase.Get() || (Base == nullptr) != !En.bRestHadBase) return false;
		if (Base && (!Base->IsQueryCollisionEnabled() || Base->GetComponentLocation() != En.RestBaseLoc || !Base->GetComponentQuat().Equals(En.RestBaseQuat, 0.f))) return false;
		return true;
	}

	static void Snapshot(FEntry& En, const AOutlawsEnemy* E, const UOutlawsEnemyMovement* M)
	{
		const USceneComponent* C = M->UpdatedComponent;
		En.bRestValid = C != nullptr;
		if (!C) return;
		En.RestLoc = C->GetComponentLocation();
		En.RestQuat = C->GetComponentQuat();
		UPrimitiveComponent* Base = E->GetMovementBase();
		En.RestBase = Base;
		En.bRestHadBase = Base != nullptr;
		if (Base)
		{
			En.RestBaseLoc = Base->GetComponentLocation();
			En.RestBaseQuat = Base->GetComponentQuat();
		}
	}

	void Run(float DeltaTime, ELevelTick TickType)
	{
		OL_PERF_SCOPE("Enemies: driver (all)");
		UWorld* W = World.Get();
		if (!W || TickType == LEVELTICK_ViewportsOnly) return;      // (as an actor's tick: none on a viewports-only tick)
		const bool bWant = CVarEnemyDriver.GetValueOnGameThread() != 0;
		if (bWant != Driving())
		{
			SetDriving(bWant);                                      // (this frame's ticks are the engine's; from the next, the new way)
			return;
		}
		if (!Driving()) return;
		const double Now = W->GetTimeSeconds();
		const int32 RestMode = CVarEnemyMoveAtRest.GetValueOnGameThread();
		const bool bRestSkip = RestMode != 0;
		FrameActorTicks = FrameMoves = FrameRests = 0;
		++FrameNo;
		const int32 N = Entries.Num();                              // (one made this frame starts next frame)
		const bool bAfterLevel = CVarEnemyDriverAfterLevel.GetValueOnGameThread() != 0;
		if (!bAfterLevel && PrereqLevel.IsValid())
		{
			Tick.RemovePrerequisite(PrereqLevel.Get(), PrereqLevel->PrimaryActorTick);
			PrereqLevel = nullptr;
		}
		// the lifts and doors the level actor moves come first (the movement's own base prerequisite did that)
		if (bAfterLevel && !PrereqLevel.IsValid())
		{
			for (const FEntry& En : Entries)
			{
				if (AOutlawsLevelActor* L = En.E ? En.E->Level.Get() : nullptr)
				{
					PrereqLevel = L;
					Tick.AddPrerequisite(L, L->PrimaryActorTick);
					break;
				}
			}
		}
		// one bad guy's think (his actor tick), when it is due (a man whose tick is off costs a look at two flags)
		auto Think = [&](int32 i) -> AOutlawsEnemy*
		{
			AOutlawsEnemy* E = Entries[i].E;
			if (!E) return nullptr;
			FActorTickFunction& F = E->PrimaryActorTick;
			// (the registration overrides keep his tick out of the engine's queue; a look every 64 frames in case anything put it
			// back, so he is never ticked twice for long; every frame would cost a cache miss a man)
			if (((i + FrameNo) & 63) == 0) Leave(F);
			float Dt = 0.f;
			if (Entries[i].Actor.Due(F.IsTickFunctionEnabled(), F.TickInterval, Now, DeltaTime, Dt))
			{
				if (!IsValidChecked(E) || E->IsUnreachable() || !E->GetWorld()) return nullptr;
				++FrameActorTicks;
				E->TickActor(Dt * E->CustomTimeDilation, TickType, F);
				// (gone in his own tick: the entry is a hole now)
				if (Entries[i].E != E || !IsValidChecked(E)) return nullptr;
			}
			return E;
		};
		// his movement, when it is due and he is not at rest
		auto Move = [&](int32 i, AOutlawsEnemy* E)
		{
			UOutlawsEnemyMovement* M = Entries[i].M;
			if (!M) return;
			FActorComponentTickFunction& F = M->PrimaryComponentTick;
			if (((i + FrameNo) & 63) == 0) Leave(F);
			float Dt = 0.f;
			const bool bWasOn = Entries[i].Move.bOn;
			if (!Entries[i].Move.Due(F.IsTickFunctionEnabled(), F.TickInterval, Now, DeltaTime, Dt)) return;
			if (!M->IsRegistered() || !IsValidChecked(E) || E->IsUnreachable() || !IsValidChecked(M)) return;
			if (!bWasOn) Entries[i].bRestValid = false;               // (switched back on: one move first, whatever he looks like)
			if (bRestSkip && AtRest(Entries[i], E, M))
			{
				++FrameRests;
				++GRestSkips;
#if WITH_OUTLAWS_TESTS
				if (RestMode == 2)
				{
					// (tests: the skipped move run all the same, and what it changed counted: expect nothing)
					const FVector L0 = M->UpdatedComponent->GetComponentLocation();
					const FVector V0 = M->Velocity;
					const uint8 Mode0 = M->MovementMode;
					const float F0 = M->CurrentFloor.FloorDist;
					M->TickComponent(Dt * E->CustomTimeDilation, TickType, &F);
					++GRestChecked;
					if (Entries[i].E != E || !IsValidChecked(E)) return;
					const FVector L1 = M->UpdatedComponent->GetComponentLocation();
					if (L1 != L0 || M->Velocity != V0 || M->MovementMode != Mode0 || M->CurrentFloor.FloorDist != F0)
					{
						if (GRestChanged++ < 8)
						{
							UE_LOG(LogOutlawsAI, Display, TEXT("OLRT enemy driver rest check: %s's skipped move changed him: moved %s, velocity %s -> %s, mode %d -> %d, floor gap %.3f -> %.3f"),
								*E->GetOutlawsName(), *(L1 - L0).ToCompactString(), *V0.ToCompactString(), *M->Velocity.ToCompactString(), Mode0, int32(M->MovementMode), F0, M->CurrentFloor.FloorDist);
						}
					}
					Snapshot(Entries[i], E, M);
				}
#endif
				return;
			}
			++FrameMoves;
			M->TickComponent(Dt * E->CustomTimeDilation, TickType, &F);
			if (Entries[i].E == E && IsValidChecked(E)) Snapshot(Entries[i], E, M);
		};
		if (CVarEnemyDriverPasses.GetValueOnGameThread() == 1)
		{
			// each man's think and then his move
			for (int32 i = 0; i < N; ++i) if (AOutlawsEnemy* E = Think(i)) Move(i, E);
		}
		else
		{
			// every think, then every move (the input each think gave is moved on in the same frame either way)
			// (the men further on fetched ahead: most are off this frame, and the loop is then a walk of cold memory)
			constexpr int32 Ahead = 8;
			for (int32 i = 0; i < N; ++i)
			{
				if (i + Ahead < N) if (const AOutlawsEnemy* P = Entries[i + Ahead].E) FPlatformMisc::Prefetch(&P->PrimaryActorTick);
				Think(i);
			}
			for (int32 i = 0; i < N; ++i)
			{
				if (i + Ahead < N) if (const UOutlawsEnemyMovement* P = Entries[i + Ahead].M) FPlatformMisc::Prefetch(&P->PrimaryComponentTick);
				if (AOutlawsEnemy* E = Entries[i].E) Move(i, E);
			}
		}
		if (bHoles)
		{
			bHoles = false;
			Entries.RemoveAll([](const FEntry& En) { return En.E == nullptr; });
		}
		SumActorTicks += FrameActorTicks;
		SumMoves += FrameMoves;
		SumRests += FrameRests;
		++SumFrames;
		CSV_CUSTOM_STAT(Outlaws, EnemyDriverTicks, FrameActorTicks, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(Outlaws, EnemyMoves, FrameMoves, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(Outlaws, EnemyMovesAtRest, FrameRests, ECsvCustomStatOp::Set);
		const double Real = FPlatformTime::Seconds();
		if (Real >= NextLogAt)
		{
			if (NextLogAt > 0.0 && SumFrames > 0)
			{
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT enemy driver: %d driven; a frame: %.1f bad guy ticks, %.1f moves run, %.1f moves at rest skipped (%d frames)"),
					Entries.Num(), double(SumActorTicks) / SumFrames, double(SumMoves) / SumFrames, double(SumRests) / SumFrames, int32(SumFrames));
			}
			SumActorTicks = SumMoves = SumRests = SumFrames = 0;
			NextLogAt = Real + 30.0;
		}
	}
};

void AOutlawsEnemy::RegisterActorTickFunctions(bool bRegister)
{
	Super::RegisterActorTickFunctions(bRegister);
	if (bRegister && FOutlawsEnemyDriver::Drives(GetWorld())) FOutlawsEnemyDriver::Leave(PrimaryActorTick);
}

void AOutlawsEnemy::BeginPlay()
{
	Super::BeginPlay();
	if (FOutlawsEnemyDriver::Wanted(GetWorld())) FOutlawsEnemyDriver::Add(this);
}

void AOutlawsEnemy::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (FOutlawsEnemyDriver::Wanted(GetWorld())) FOutlawsEnemyDriver::Remove(this);
	Super::EndPlay(EndPlayReason);
}

void UOutlawsEnemyMovement::RegisterComponentTickFunctions(bool bRegister)
{
	Super::RegisterComponentTickFunctions(bRegister);
	if (bRegister && FOutlawsEnemyDriver::Drives(GetWorld())) FOutlawsEnemyDriver::Leave(PrimaryComponentTick);
}

bool UOutlawsEnemyMovement::HasPendingMove() const
{
	return !PendingImpulseToApply.IsZero() || !PendingForceToApply.IsZero() || !PendingLaunchVelocity.IsZero() || bHasRequestedVelocity;
}
