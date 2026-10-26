#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "OutlawsTypes.h"
#include "OutlawsRuntimeLibrary.generated.h"

/** Validation helpers callable from Python / Blueprint (no actors spawned). */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsRuntimeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Editor only: the expression feeding a material's World Position Offset (the input is hidden from Python in 5.1). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	static class UMaterialExpression* GetWorldPositionOffsetExpression(class UMaterial* Material);

	/** Editor only: connect an expression's first output to a material's World Position Offset. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	static bool ConnectWorldPositionOffset(class UMaterial* Material, class UMaterialExpression* Expression);

	/** Editor only: a Custom expression's input by name (added if it has none) fed by another expression's first output
	 *  (Python cannot link a custom input in 5.1). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	static bool LinkCustomInput(class UMaterialExpression* Custom, const FString& Name, class UMaterialExpression* From);

	/** Install folder the loader would use (empty when not found). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	static FString FindInstallRoot(const FString& InstallPath);

	/** Every level with both an LVT and an OBT in the LABs. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	static TArray<FString> ListLevels(const FString& InstallPath);

	/**
	 * Parse, triangulate and build the geometry of one level, resolve its objects and decode
	 * its sprites, without creating any GPU resources.  Returns a one-line summary with the
	 * numbers the Python exporter also reports, for side-by-side comparison.
	 */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	static FString ValidateLevel(const FString& LevelName, const FString& InstallPath, EOutlawsDifficulty Difficulty = EOutlawsDifficulty::Bad);

	/**
	 * -OLAdjoinScan (BUG-ADJOINDEFAULT): every level in the player's LABs built twice through the loader, with the sign
	 * rule before the fix and after it.  Logs each surface on an adjoined wall the original does not draw (OLSCAN old /
	 * new lines), the counts per level and in all, and every geometry count that differs between the two builds.
	 */
	static void RunAdjoinScan(const FString& InstallPath);

	/**
	 * -OLWallFacingScan (BUG-WALLSFLIPPED): every level's walls, counted where the wall's face (the inward normal the
	 * geometry builds it with) points out of its own sector, with the winding rule before the fix (the vertex list's
	 * order) and after it (the walls' order); and each sloped surface whose reference sector's winding changed.
	 */
	static void RunWallFacingScan(const FString& InstallPath);
};
