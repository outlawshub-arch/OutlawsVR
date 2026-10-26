// REQ-TUTORIAL2: the tutorial's card, a small panel with Back and Next in the join bubble's look (OutlawsBeacon.h), up where
// the notice lines stand (10 degrees over the middle of the view, 80 cm out, pulled in front of a nearer wall), so it never
// covers a shot.  Its buttons are pressed as the bubble's are: a hand's laser on one and the trigger.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OutlawsTutorialCard.generated.h"

class UCanvasRenderTarget2D;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UTexture2D;
class UFont;
class UCanvas;

UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsTutorialCard : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsTutorialCard();
	/** The card shown: "Step N of M", its line and a second line under it, done (a tick and "Good.", Next lit) or not;
	 *  Back left out on the first step, Next reads Finish on the last. */
	void SetCard(int32 InStep, int32 InSteps, const FString& InLine, const FString& InHint, bool bInGood);
	void SetGood(bool bInGood);
	void Hide();
	bool IsUp() const { return bUp; }
	bool IsGood() const { return bGood; }
	int32 GetStep() const { return Step; }
	int32 GetSteps() const { return Steps; }
	const FString& GetLine() const { return Line; }
	const FString& GetHint() const { return Hint; }
	/** Pressed (the trigger on a button; tests too): 0 Back, 1 Next.  Kept till the tutorial takes it. */
	void Pick(int32 Button);
	int32 TakePick() { const int32 B = Picked; Picked = -1; return B; }
	/** Seen on screen this frame (up, not hidden behind the menu or the death view). */
	bool IsShown() const { return bUp && !IsHidden(); }
	/** A hand's laser is on one of the card's buttons: the trigger is the card's, not the gun's. */
	static bool HandOnCard(int32 Hand);

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
	UFont* SmallFont = nullptr;
	bool bUp = false, bGood = false;
	int32 Step = 1, Steps = 1;
	FString Line, Hint;
	int32 Hover[2] = { -1, -1 };
	bool bArmed[2] = { false, false };
	int32 Picked = -1;
	float Dist = 80.f;
	FBox2D Buttons[2];
	void Place(float Dt);
	void Redraw();
	UFUNCTION()
	void DrawCanvas(UCanvas* C, int32 Width, int32 Height);
};
