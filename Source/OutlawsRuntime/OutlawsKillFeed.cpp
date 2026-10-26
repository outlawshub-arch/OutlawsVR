#include "OutlawsKillFeed.h"

namespace
{
	/** Template's "%s" filled in order (A, then B). */
	FString Fill(const FString& Template, const FString& A, const FString& B = FString())
	{
		FString Out = Template;
		int32 At = Out.Find(TEXT("%s"), ESearchCase::CaseSensitive);
		if (At == INDEX_NONE) return Out;
		Out = Out.Left(At) + A + Out.Mid(At + 2);
		const int32 Next = Out.Find(TEXT("%s"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + A.Len());
		if (Next != INDEX_NONE) Out = Out.Left(Next) + B + Out.Mid(Next + 2);
		return Out;
	}
}

EOutlawsObit OutlawsKillFeed::FromCause(uint8 Cause)
{
	switch (static_cast<EOutlawsHurtCause>(Cause))
	{
	case EOutlawsHurtCause::Fall: return EOutlawsObit::Fell;
	case EOutlawsHurtCause::Drown: return EOutlawsObit::Drowned;
	case EOutlawsHurtCause::OwnBlast: return EOutlawsObit::SelfDestructed;
	case EOutlawsHurtCause::OwnShot: return EOutlawsObit::Suicide;
	default: return EOutlawsObit::Died;
	}
}

const TCHAR* OutlawsKillFeed::Name(EOutlawsObit Kind)
{
	switch (Kind)
	{
	case EOutlawsObit::Killed: return TEXT("killed");
	case EOutlawsObit::BlewUp: return TEXT("blew up");
	case EOutlawsObit::SelfDestructed: return TEXT("self-destructed");
	case EOutlawsObit::Suicide: return TEXT("suicide");
	case EOutlawsObit::Fell: return TEXT("fell");
	case EOutlawsObit::Drowned: return TEXT("drowned");
	default: return TEXT("died");
	}
}

FString OutlawsKillFeed::Line(EOutlawsObit Kind, const FString& Killer, const FString& Victim, bool bKillerIsMe, bool bVictimIsMe,
	TFunctionRef<FString(int32, const TCHAR*)> Msg)
{
	switch (Kind)
	{
	case EOutlawsObit::Killed:
		if (bKillerIsMe) return Fill(Msg(823, TEXT("You killed %s!")), Victim);
		if (bVictimIsMe) return Fill(Msg(20043, TEXT("%s killed you!")), Killer);
		return Fill(Msg(20042, TEXT("%s killed %s!")), Killer, Victim);
	case EOutlawsObit::BlewUp:
		// (the original has no line for the one who threw it: 823's form with 20061's verb)
		if (bKillerIsMe) return Fill(TEXT("You blew up %s!"), Victim);
		if (bVictimIsMe) return Fill(Msg(20062, TEXT("%s blew you up!")), Killer);
		return Fill(Msg(20061, TEXT("%s blew up %s!")), Killer, Victim);
	case EOutlawsObit::SelfDestructed:
		return bVictimIsMe ? Msg(60, TEXT("You blew yourself up!")) : Fill(Msg(20059, TEXT("%s self-destructed!")), Victim);
	case EOutlawsObit::Suicide:
		return bVictimIsMe ? Msg(41, TEXT("You killed yourself!")) : Fill(Msg(20040, TEXT("%s committed suicide!")), Victim);
	case EOutlawsObit::Fell:
		return bVictimIsMe ? Msg(45, TEXT("You fell to your death!")) : Fill(Msg(20044, TEXT("%s died from a fall!")), Victim);
	case EOutlawsObit::Drowned:
		return bVictimIsMe ? Msg(49, TEXT("You drowned!")) : Fill(Msg(20048, TEXT("%s drowned!")), Victim);
	default:
		return bVictimIsMe ? Msg(47, TEXT("You died!")) : Fill(Msg(20046, TEXT("%s died!")), Victim);
	}
}
