#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Components/StaticMeshComponent.h"
#include "OutlawsFragTable.generated.h"

class UCanvas;
class UFont;
class UTexture2D;
class UCanvasRenderTarget2D;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class AOutlawsPlayer;

/**
 * FT, the Frag Table (user 2026-10-03: "reuse the scoreboard from competitive multiplayer from the original game and rather
 * than just have the kills column we could have more columns for the other stats ... without the background image of the
 * town just have the panel with the names"): the remaster's fragtable_full.rml drawn on a 1920 wide canvas, one routine for
 * the level's end, the pause menu's Scoreboard page and a match's end.  The header box (the title, the header rows) over the
 * players box (a row a player), each the DialogBox frame over the DialogBackground wood, as fragtable.rcss has them.
 */

/** A column: its short header and its width on the canvas (0: the name column, which takes what the others leave). */
struct FFragTableColumn
{
	FString Header;
	float Width = 0.f;
	bool operator==(const FFragTableColumn& O) const { return Header == O.Header && Width == O.Width; }
};

/** A line of the header box: a label and its value (a team's score: Team 1 red, 2 blue). */
struct FFragTableHeaderRow
{
	FString Label;
	FString Value;
	int32 Team = 0;
	bool operator==(const FFragTableHeaderRow& O) const { return Label == O.Label && Value == O.Value && Team == O.Team; }
};

/** A player's row: the name, the team (0 none, 1 red, 2 blue), the values after the name column (as shown), the local player
 *  (drawn in the hot row's #ffef65), and the sort keys (SortA high first, then SortB low first). */
struct FFragTableRow
{
	FString Name;
	int32 Team = 0;
	TArray<FString> Values;
	bool bLocal = false;
	int32 SortA = 0;
	int32 SortB = 0;
	bool operator==(const FFragTableRow& O) const
	{
		return Name == O.Name && Team == O.Team && Values == O.Values && bLocal == O.bLocal && SortA == O.SortA && SortB == O.SortB;
	}
};

/** The table's words: what a source (the level's counts, M1's match) hands the drawing. */
struct OUTLAWSRUNTIME_API FFragTableData
{
	/** The mode's name in a match ("Death Match"), the level's name at its end. */
	FString Title;
	TArray<FFragTableHeaderRow> HeaderRows;
	/** The columns, the name's first. */
	TArray<FFragTableColumn> Columns;
	TArray<FFragTableRow> Rows;

	bool operator==(const FFragTableData& O) const { return Title == O.Title && HeaderRows == O.HeaderRows && Columns == O.Columns && Rows == O.Rows; }
	bool operator!=(const FFragTableData& O) const { return !(*this == O); }

	/** The rows in order: SortA high first, SortB low first, then by name. */
	void SortRows();
	/** The level's end: NAME, KILLS, DEATHS, SHOTS, ACC, LONGEST, TNT, THROWN, (REVIVED in co-op), TIME. */
	void SetLevelEndColumns(bool bCoop);
	/** A match (M1): NAME, KILLS, DEATHS, SHOTS, ACC, LONGEST (and PING once R8's ping exists). */
	void SetMatchColumns(bool bPing = false);
	/** One line a row for the logs. */
	FString Describe() const;
};

/** Where the table's rows come from (M1 feeds its match through one): filled on each look, at most twice a second. */
class IOutlawsFragTableSource
{
public:
	virtual ~IOutlawsFragTableSource() = default;
	virtual bool FillFragTable(FFragTableData& Out) const = 0;
};

/** The art and fonts the table is drawn in: the menus' own, out of the player's UI.kpf and Common.kpf. */
struct OUTLAWSRUNTIME_API FFragTableArt
{
	UTexture2D* Box = nullptr;          // DialogBox@2x (the ninepatch frame)
	UTexture2D* Wood = nullptr;         // DialogBackground@2x
	UTexture2D* Rule = nullptr;         // Embellish2@2x (hr.small under the title)
	UFont* Title = nullptr;
	UFont* Header = nullptr;
	UFont* Cell = nullptr;
	bool Load(AOutlawsPlayer* Player);
	bool IsLoaded() const { return Cell != nullptr; }
};

/** The drawing: Draw on a canvas 1920 wide (Measure tall); returns where the last box ends (the measurer's check). */
struct OUTLAWSRUNTIME_API FOutlawsFragTable
{
	static constexpr int32 CanvasW = 1920;
	static int32 Measure(const FFragTableData& Data);
	static float Draw(UCanvas* C, const FFragTableData& Data, const FFragTableArt& Art, int32 Width, int32 Height);
};

/** (BUG-SCOREOCCLUDE) the panel's plane: its bounds a cube as wide as its sphere, not the flat plane's paper-thin box, so the
 *  eyes inside the widened bounds (AOutlawsFragTablePanel::KeepOverWorld) are inside the box too and occlusion culling (which
 *  tests the box) never drops it behind a near wall. */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsOnTopPlaneComponent : public UStaticMeshComponent
{
	GENERATED_BODY()
public:
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};

/**
 * The table on a panel in the world (the level's end over the cinema, a match's end in front of each player): a plane in
 * the picture's unlit material, as wide as asked and as tall as its rows need, its canvas redrawn when the data changes.
 */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsFragTablePanel : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsFragTablePanel();

	/** Stand WidthCm wide, Distance cm from Eye along Forward (level), its middle at the eye's height plus RaiseCm, tipped to
	 *  face the eye. */
	void PlaceBefore(const FVector& Eye, const FVector& Forward, float Distance = 300.f, float WidthCm = 320.f, float RaiseCm = 0.f);
	/** (REQ-SCOREABOVE) Stand WidthCm wide with its middle at Centre, upright to RightHint's side and tipped to face Eye. */
	void PlaceFacing(const FVector& Centre, const FVector& Eye, const FVector& RightHint, float InWidthCm);
	void SetData(const FFragTableData& Data);
	/** Rows from a source, looked at twice a second (nullptr: SetData's alone). */
	void SetSource(const IOutlawsFragTableSource* InSource) { Source = InSource; LookIn = 0.f; }
	const FFragTableData& GetData() const { return Shown; }
	UCanvasRenderTarget2D* GetCanvas() const { return Canvas; }
	/** The canvas height of the rows shown. */
	int32 GetCanvasH() const { return CanvasH; }
	/** Where the last drawing's boxes ended (the measurer's check). */
	float LastDrawnBottom = 0.f;
	/** (BUG-SCOREBOARD) drawn over the world in the overlay material (no depth test) as the wheel and the menu are; set
	 *  before the first SetData.  Off: the picture's material, cut by walls (the level's end over the cinema). */
	bool bOnTop = false;
	/** The material the plane is drawn in and its sort priority, for the logs. */
	FString DescribeMaterial() const;
	/** (BUG-SCOREOCCLUDE) the plane's bounds scale now (an on-top panel's reach past the eyes, so no wall culls it). */
	float GetBoundsScale() const;
	/** (the tests) the canvas's wood as stored: the median colour inside the first box's frame, read back from the GPU
	 *  (the bytes as written, sRGB).  False with no canvas yet. */
	bool ReadWood(FColor& Out) const;

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UOutlawsOnTopPlaneComponent> Plane;
	UPROPERTY(Transient)
	TObjectPtr<UCanvasRenderTarget2D> Canvas;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;
	FFragTableArt Art;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> ArtKept;       // (the art's fonts and pictures held here: the struct is not seen by the collector)
	FFragTableData Shown;
	const IOutlawsFragTableSource* Source = nullptr;
	float LookIn = 0.f;
	float WidthCm = 320.f;
	int32 CanvasH = 0;
	void Redraw();
	void Fit();
	/** (BUG-SCOREOCCLUDE) an on-top panel's bounds reach past Eye, so occlusion culling never drops it behind a near wall. */
	void KeepOverWorld(const FVector& Eye);
	UFUNCTION()
	void DrawCanvas(UCanvas* C, int32 Width, int32 Height);
};

/** -OLFragTableTest: fake tables of 2, 4 and 8 players on a panel in front of the player, photographed (Saved\OLShots). */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsFragTableTest : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickable() const override { return !IsTemplate(); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	/** A made-up table of N players: 2 the level's end in co-op, 4 a team game, 8 (or more) a death match. */
	static FFragTableData FakeTable(int32 Players);

private:
	int32 Step = 0;
	double At = 0.0;
	int32 Bad = 0;
	UPROPERTY(Transient)
	TObjectPtr<AOutlawsFragTablePanel> Panel;
};
