#include "OutlawsInf.h"
#include "OutlawsRuntime.h"

namespace
{
	bool IsNumber(const FString& S)
	{
		if (S.IsEmpty()) return false;
		int32 i = (S[0] == TEXT('-') || S[0] == TEXT('+')) ? 1 : 0;
		bool bDigit = false;
		for (; i < S.Len(); ++i)
		{
			if (FChar::IsDigit(S[i])) bDigit = true;
			else if (S[i] != TEXT('.')) return false;
		}
		return bDigit;
	}

	EOutlawsInfMsg MsgType(const FString& S)
	{
		const FString U = S.ToUpper();
		if (U == TEXT("NEXT_STOP")) return EOutlawsInfMsg::NextStop;
		if (U == TEXT("PREV_STOP")) return EOutlawsInfMsg::PrevStop;
		if (U == TEXT("GOTO_STOP")) return EOutlawsInfMsg::GotoStop;
		if (U == TEXT("MASTER_ON")) return EOutlawsInfMsg::MasterOn;
		if (U == TEXT("MASTER_OFF")) return EOutlawsInfMsg::MasterOff;
		if (U == TEXT("USER_MSG")) return EOutlawsInfMsg::UserMsg;
		if (U == TEXT("END_LEVEL") || U == TEXT("COMPLETE")) return EOutlawsInfMsg::EndLevel;
		if (U == TEXT("SPAWN_LEVEL")) return EOutlawsInfMsg::SpawnLevel;
		if (U == TEXT("M_TRIGGER") || U == TEXT("TRIGGER")) return EOutlawsInfMsg::Trigger;
		if (U == TEXT("DONE")) return EOutlawsInfMsg::Done;
		if (U == TEXT("TRIGGER_MOVIE")) return EOutlawsInfMsg::Movie;
		return EOutlawsInfMsg::Other;
	}

	bool IsMsgKeyword(const FString& S)
	{
		return MsgType(S) != EOutlawsInfMsg::Other;
	}
}

void FOutlawsInfFile::SplitNameId(const FString& In, FString& OutName, FString& OutId)
{
	OutName = In;
	OutId.Reset();
	int32 Open;
	if (In.FindChar(TEXT('('), Open))
	{
		OutName = In.Left(Open);
		FString Rest = In.Mid(Open + 1);
		Rest.RemoveFromEnd(TEXT(")"));
		Rest.RemoveFromStart(TEXT("#"));
		OutId = Rest;
	}
	else if (In.StartsWith(TEXT("!#")))
	{
		OutName.Reset();
		OutId = In.Mid(2);
	}
}

EOutlawsInfClass FOutlawsInfFile::ClassFromString(const FString& Kind, const FString& Sub, bool& bMoveObjects)
{
	bMoveObjects = false;
	const FString K = Kind.ToUpper(), S = Sub.ToUpper();
	if (K == TEXT("TRIGGER"))
	{
		if (S == TEXT("SINGLE")) return EOutlawsInfClass::TriggerSingle;
		return EOutlawsInfClass::TriggerStandard;     // STANDARD, SWITCH1, TOGGLE...
	}
	if (K != TEXT("ELEVATOR") && K != TEXT("ELVATOR"))   // canyon.inf has the typo
	{
		return EOutlawsInfClass::Other;
	}
	bMoveObjects = S.EndsWith(TEXT("2"));
	if (S == TEXT("MOVE_FLOOR") || S == TEXT("BASIC") || S == TEXT("INV") || S == TEXT("DOOR")) return EOutlawsInfClass::MoveFloor;
	if (S == TEXT("MOVE_CEILING")) return EOutlawsInfClass::MoveCeiling;
	if (S == TEXT("MOVE_FC")) return EOutlawsInfClass::MoveFC;
	if (S.StartsWith(TEXT("MORPH_MOVE"))) return EOutlawsInfClass::MorphMove;
	if (S.StartsWith(TEXT("MORPH_SPIN"))) return EOutlawsInfClass::MorphSpin;
	if (S == TEXT("SCROLL_FLOOR")) return EOutlawsInfClass::ScrollFloor;
	if (S == TEXT("SCROLL_CEILING")) return EOutlawsInfClass::ScrollCeiling;
	if (S == TEXT("SCROLL_WALL")) return EOutlawsInfClass::ScrollWall;
	if (S == TEXT("CHANGE_LIGHT")) return EOutlawsInfClass::ChangeLight;
	if (S == TEXT("VELOCITY_X")) return EOutlawsInfClass::VelocityX;
	if (S == TEXT("VELOCITY_Y")) return EOutlawsInfClass::VelocityY;
	if (S == TEXT("VELOCITY_Z")) return EOutlawsInfClass::VelocityZ;
	if (S == TEXT("EXPLODE")) return EOutlawsInfClass::Explode;
	if (S == TEXT("SLOPE_FLOOR")) return EOutlawsInfClass::SlopeFloor;
	if (S == TEXT("SLOPE_CEILING")) return EOutlawsInfClass::SlopeCeiling;
	return EOutlawsInfClass::Other;
}

bool FOutlawsInfFile::Parse(const FString& Text)
{
	Items.Reset();
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	FOutlawsInfItem* Item = nullptr;
	FOutlawsInfClass* Cls = nullptr;
	TArray<FString> T;
	EOutlawsStopAction PrevAction = EOutlawsStopAction::Wait;
	double PrevWait = 4.0;
	for (FString Line : Lines)
	{
		int32 Hash;
		// '#' starts a comment unless it is part of an id ("#F491", "(#A8B2)", "!#7A22")
		for (int32 i = 0; i < Line.Len(); ++i)
		{
			if (Line[i] == TEXT('#') && (i == 0 || FChar::IsWhitespace(Line[i - 1])) && !(i + 1 < Line.Len() && FChar::IsHexDigit(Line[i + 1])))
			{
				Line.LeftInline(i);
				break;
			}
		}
		(void)Hash;
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty()) continue;
		T.Reset();
		Line.ParseIntoArrayWS(T);
		if (T.Num() == 0) continue;
		const FString K = T[0].ToUpper();

		if (K == TEXT("ITEM:"))
		{
			FOutlawsInfItem& It = Items.AddDefaulted_GetRef();
			It.bLine = T.IsValidIndex(1) && T[1].Equals(TEXT("LINE"), ESearchCase::IgnoreCase);
			for (int32 i = 1; i < T.Num(); ++i)
			{
				// (BUG-SIMMSINF) "NAME:START" with no space (shsimms.inf's horse sector) reads as the original's "NAME: %s" does
				if (T[i].Len() > 5 && T[i].StartsWith(TEXT("NAME:"), ESearchCase::IgnoreCase)) SplitNameId(T[i].Mid(5), It.Name, It.NameId);
				if (i + 1 >= T.Num()) continue;
				if (T[i].Equals(TEXT("NAME:"), ESearchCase::IgnoreCase)) SplitNameId(T[i + 1], It.Name, It.NameId);
				if (T[i].Equals(TEXT("NUM:"), ESearchCase::IgnoreCase)) { It.WallId = T[i + 1]; It.WallId.RemoveFromStart(TEXT("#")); }
			}
			Item = &It;
			Cls = nullptr;
			continue;
		}
		if (!Item) continue;
		if (K == TEXT("SEQ") || K == TEXT("SEQEND")) { if (K == TEXT("SEQEND")) Cls = nullptr; continue; }
		if (K == TEXT("CLASS:"))
		{
			FOutlawsInfClass& C = Item->Classes.AddDefaulted_GetRef();
			C.RawClass = FString::Join(TArray<FString>(T.GetData() + 1, T.Num() - 1), TEXT(" "));
			C.Class = ClassFromString(T.IsValidIndex(1) ? T[1] : TEXT(""), T.IsValidIndex(2) ? T[2] : TEXT(""), C.bMoveObjects);
			Cls = &C;
			continue;
		}
		if (!Cls) continue;
		auto Num = [&T](int32 i, double D = 0.0) { return T.IsValidIndex(i) ? FCString::Atod(*T[i]) : D; };
		if (K == TEXT("EVENT_MASK:")) { Cls->EventMask = uint32(FCString::Atoi64(*T[1])); Cls->bHasEventMask = true; }
		else if ((K == TEXT("ENTITY_MASK:") || K == TEXT("OBJECT_MASK:")) && T.IsValidIndex(1)) Cls->bAnyEntity = T[1].StartsWith(TEXT("*")) || (uint32(FCString::Atoi64(*T[1])) & (1u | 0x800u)) != 0;
		else if (K == TEXT("SPEED:") || K == TEXT("SPEED")) { Cls->Speed = Num(1); Cls->bHasSpeed = true; }
		else if (K == TEXT("START:")) Cls->StartStop = FMath::Max(0, int32(Num(1)));
		else if (K == TEXT("ANGLE:")) Cls->Angle = Num(1);
		else if (K == TEXT("CENTER:")) { Cls->Center = FVector2D(Num(1), Num(2)); Cls->bHasCenter = true; }
		else if (K == TEXT("MASTER:")) Cls->bMasterOn = !(T.IsValidIndex(1) && T[1].Equals(TEXT("OFF"), ESearchCase::IgnoreCase));
		else if (K == TEXT("OBJECT:")) { Cls->KeyObject = int32(Num(1, -1)); Cls->KeyObjectFlags = uint32(Num(2, 0.0)); }
		else if (K == TEXT("OBJECT_EXCLUDE:")) Cls->ExcludeObject = int32(Num(1, -1));
		else if (K == TEXT("SLAVE:") && T.IsValidIndex(1)) Cls->Slaves.Add(T[1]);
		else if (K == TEXT("CLIENT:") && T.IsValidIndex(1)) { Cls->Clients.Add(T[1]); Cls->ClientMsg.AddDefaulted(); Cls->ClientHasMsg.Add(false); }
		else if (K == TEXT("SOUND:") && T.IsValidIndex(2)) Cls->Sounds.Add(int32(Num(1)), T[2]);
		else if (K == TEXT("STOP:") && T.IsValidIndex(1))
		{
			FOutlawsInfStop& S = Cls->Stops.AddDefaulted_GetRef();
			FString V = T[1];
			if (V.StartsWith(TEXT("@"))) { S.bRelative = true; V.RemoveFromStart(TEXT("@")); }
			// numeric if it starts like a number (strtof reads the prefix: indy.inf has "26.5TERMINATE")
			if (!V.IsEmpty() && (FChar::IsDigit(V[0]) || V[0] == TEXT('-') || V[0] == TEXT('.'))) S.Value = FCString::Atod(*V);
			else if (S.bRelative) S.Value = 0.0;
			else S.SectorRef = V;
			// delay: none = 4 s (the stop default); a word we do not know = the last delay given
			if (T.IsValidIndex(2))
			{
				const FString A = T[2].ToUpper();
				if (A == TEXT("HOLD")) S.Action = EOutlawsStopAction::Hold;
				else if (A == TEXT("TERMINATE")) S.Action = EOutlawsStopAction::Terminate;
				else if (A == TEXT("COMPLETE")) S.Action = EOutlawsStopAction::Complete;
				else if (IsNumber(A)) { S.Action = EOutlawsStopAction::Wait; S.Wait = FCString::Atod(*A); }
				else { S.Action = PrevAction; S.Wait = PrevWait; }
				PrevAction = S.Action;
				PrevWait = S.Wait;
			}
		}
		else if (K == TEXT("MESSAGE:") && T.IsValidIndex(1))
		{
			FOutlawsInfMessage M;
			int32 i = 1;
			int32 StopIdx = -1;
			// stop message: "MESSAGE: <stop> <target> [TYPE [ARG]]"
			if (IsNumber(T[1]) && T.IsValidIndex(2) && !IsMsgKeyword(T[2]))
			{
				StopIdx = FCString::Atoi(*T[1]);
				SplitNameId(T[2], M.Target, M.TargetId);
				i = 3;
			}
			if (T.IsValidIndex(i))
			{
				M.Type = MsgType(T[i]);
				if (T.IsValidIndex(i + 1) && IsNumber(T[i + 1])) M.Arg = FCString::Atoi(*T[i + 1]);
				else if ((M.Type == EOutlawsInfMsg::SpawnLevel || M.Type == EOutlawsInfMsg::Movie) && T.IsValidIndex(i + 1))
				{
					M.Target = T[i + 1];
					// (HM1: SPAWN_LEVEL <level> <return sector>, the office's posters: the sector the player comes back to)
					if (M.Type == EOutlawsInfMsg::SpawnLevel && T.IsValidIndex(i + 2)) M.Target += TEXT(" ") + T[i + 2];
				}
			}
			if (StopIdx >= 0)
			{
				if (Cls->Stops.IsValidIndex(StopIdx)) Cls->Stops[StopIdx].Messages.Add(M);
				else if (Cls->Stops.Num() > 0) Cls->Stops.Last().Messages.Add(M);
			}
			else if (Cls->Clients.Num() > 0 && !Cls->ClientHasMsg.Last())
			{
				// the level files pair them: "CLIENT: x" then its "MESSAGE: m" (427 trigger classes so, none otherwise)
				Cls->ClientMsg.Last() = M;
				Cls->ClientHasMsg.Last() = true;
			}
			else
			{
				Cls->TriggerMessages.Add(M);
			}
		}
	}
	return Items.Num() > 0;
}

FString FOutlawsInfFile::Summary() const
{
	TMap<FString, int32> Count;
	for (const FOutlawsInfItem& It : Items)
	{
		for (const FOutlawsInfClass& C : It.Classes)
		{
			Count.FindOrAdd(C.RawClass.ToUpper())++;
		}
	}
	TArray<FString> Parts;
	for (const TPair<FString, int32>& Kv : Count) Parts.Add(FString::Printf(TEXT("%s=%d"), *Kv.Key, Kv.Value));
	Parts.Sort();
	return FString::Printf(TEXT("%d items: %s"), Items.Num(), *FString::Join(Parts, TEXT(", ")));
}
