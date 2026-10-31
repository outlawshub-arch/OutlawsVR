// Common.kpf menus: see OutlawsMenuDoc.h.
#include "OutlawsMenuDoc.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"

namespace
{
	uint16 U16(const uint8* P) { return uint16(P[0] | (P[1] << 8)); }
	uint32 U32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }

	bool ReadAt(IFileHandle* H, int64 Offset, int64 Size, TArray<uint8>& Out)
	{
		Out.SetNumUninitialized(Size);
		return H->Seek(Offset) && (Size == 0 || H->Read(Out.GetData(), Size));
	}

	// ---- a small XML-ish DOM, enough for RmlUi documents --------------------------------------
	struct FNode
	{
		FString Tag;
		TMap<FString, FString> Attr;
		TArray<TSharedPtr<FNode>> Children;
		FString Text;          // text directly inside this element
		bool Has(const TCHAR* A) const { return Attr.Contains(A); }
		FString Get(const TCHAR* A) const { return Attr.FindRef(A); }
		bool HasClass(const TCHAR* C) const
		{
			TArray<FString> Parts;
			Get(TEXT("class")).ParseIntoArrayWS(Parts);
			return Parts.Contains(C);
		}
	};

	TSharedPtr<FNode> ParseRml(FString S)
	{
		// comments out
		for (int32 A = S.Find(TEXT("<!--")); A != INDEX_NONE; A = S.Find(TEXT("<!--")))
		{
			const int32 B = S.Find(TEXT("-->"), ESearchCase::CaseSensitive, ESearchDir::FromStart, A + 4);
			S.RemoveAt(A, (B == INDEX_NONE ? S.Len() : B + 3) - A);
		}
		TSharedPtr<FNode> Root = MakeShared<FNode>();
		Root->Tag = TEXT("#root");
		TArray<FNode*> Stack = { Root.Get() };
		const int32 N = S.Len();
		int32 i = 0;
		while (i < N)
		{
			if (S[i] != TEXT('<'))
			{
				const int32 Lt = S.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i);
				const int32 End = Lt == INDEX_NONE ? N : Lt;
				Stack.Last()->Text += S.Mid(i, End - i);
				i = End;
				continue;
			}
			const int32 Gt = S.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i);
			if (Gt == INDEX_NONE) break;
			FString Tag = S.Mid(i + 1, Gt - i - 1);
			i = Gt + 1;
			if (Tag.StartsWith(TEXT("!")) || Tag.StartsWith(TEXT("?"))) continue;
			if (Tag.StartsWith(TEXT("/")))
			{
				const FString Name = Tag.Mid(1).TrimStartAndEnd().ToLower();
				for (int32 k = Stack.Num() - 1; k > 0; --k)
				{
					if (Stack[k]->Tag == Name)
					{
						Stack.SetNum(k);
						break;
					}
				}
				continue;
			}
			const bool bSelfClose = Tag.EndsWith(TEXT("/"));
			if (bSelfClose) Tag.LeftChopInline(1);
			TSharedPtr<FNode> Node = MakeShared<FNode>();
			int32 p = 0;
			while (p < Tag.Len() && !FChar::IsWhitespace(Tag[p])) ++p;
			Node->Tag = Tag.Left(p).ToLower();
			// attributes: name, name=value, name="value", name='value'
			while (p < Tag.Len())
			{
				while (p < Tag.Len() && FChar::IsWhitespace(Tag[p])) ++p;
				const int32 A0 = p;
				while (p < Tag.Len() && !FChar::IsWhitespace(Tag[p]) && Tag[p] != TEXT('=')) ++p;
				const FString Name = Tag.Mid(A0, p - A0).ToLower();
				FString Value;
				if (p < Tag.Len() && Tag[p] == TEXT('='))
				{
					++p;
					if (p < Tag.Len() && (Tag[p] == TEXT('"') || Tag[p] == TEXT('\'')))
					{
						const TCHAR Q = Tag[p++];
						const int32 V0 = p;
						while (p < Tag.Len() && Tag[p] != Q) ++p;
						Value = Tag.Mid(V0, p - V0);
						++p;
					}
					else
					{
						const int32 V0 = p;
						while (p < Tag.Len() && !FChar::IsWhitespace(Tag[p])) ++p;
						Value = Tag.Mid(V0, p - V0);
					}
				}
				if (!Name.IsEmpty()) Node->Attr.Add(Name, Value);
			}
			Stack.Last()->Children.Add(Node);
			static const TSet<FString> Void = { TEXT("hr"), TEXT("br"), TEXT("img"), TEXT("link"), TEXT("meta") };
			if (!bSelfClose && !Void.Contains(Node->Tag))
			{
				Stack.Add(Node.Get());
			}
		}
		return Root;
	}

	void AllText(const FNode& N, FString& Out)
	{
		Out += N.Text;
		for (const TSharedPtr<FNode>& C : N.Children) AllText(*C, Out);
	}

	FString AllText(const FNode& N)
	{
		FString Out;
		AllText(N, Out);
		return Out;
	}

	const FNode* FindTag(const FNode& N, const TCHAR* Tag)
	{
		for (const TSharedPtr<FNode>& C : N.Children)
		{
			if (C->Tag == Tag) return C.Get();
			if (const FNode* F = FindTag(*C, Tag)) return F;
		}
		return nullptr;
	}

	void FindAll(const FNode& N, const TCHAR* Tag, TArray<const FNode*>& Out)
	{
		for (const TSharedPtr<FNode>& C : N.Children)
		{
			if (C->Tag == Tag) Out.Add(C.Get());
			FindAll(*C, Tag, Out);
		}
	}
}

namespace OutlawsMenu
{
	bool FKpf::Open(const FString& InPath)
	{
		Path = InPath;
		Entries.Reset();
		TUniquePtr<IFileHandle> H(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		if (!H)
		{
			return false;
		}
		// the end-of-central-directory record, within the last 64 KB
		const int64 Size = H->Size();
		const int64 Tail = FMath::Min<int64>(Size, 65557);
		TArray<uint8> Buf;
		if (!ReadAt(H.Get(), Size - Tail, Tail, Buf))
		{
			return false;
		}
		int32 Eocd = INDEX_NONE;
		for (int32 k = Buf.Num() - 22; k >= 0; --k)
		{
			if (U32(Buf.GetData() + k) == 0x06054b50) { Eocd = k; break; }
		}
		if (Eocd == INDEX_NONE)
		{
			return false;
		}
		const int32 Count = U16(Buf.GetData() + Eocd + 10);
		const int64 CdSize = U32(Buf.GetData() + Eocd + 12);
		const int64 CdOff = U32(Buf.GetData() + Eocd + 16);
		TArray<uint8> Cd;
		if (!ReadAt(H.Get(), CdOff, CdSize, Cd))
		{
			return false;
		}
		int64 Pos = 0;
		for (int32 e = 0; e < Count && Pos + 46 <= Cd.Num(); ++e)
		{
			const uint8* P = Cd.GetData() + Pos;
			if (U32(P) != 0x02014b50) break;
			FEntry En;
			En.Method = U16(P + 10);
			En.Size = U32(P + 20);
			const int32 NameLen = U16(P + 28), ExtraLen = U16(P + 30), CommentLen = U16(P + 32);
			En.LocalHeader = U32(P + 42);
			if (Pos + 46 + NameLen > Cd.Num()) break;
			const FString Name = FString(NameLen, reinterpret_cast<const ANSICHAR*>(P + 46)).ToLower();
			Entries.Add(Name, En);
			Pos += 46 + NameLen + ExtraLen + CommentLen;
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT menu: %s holds %d entries"), *FPaths::GetCleanFilename(Path), Entries.Num());
		return Entries.Num() > 0;
	}

	bool FKpf::Read(const FString& Name, TArray<uint8>& Out) const
	{
		const FEntry* E = Entries.Find(Name.ToLower());
		if (!E || E->Method != 0)
		{
			return false;     // only stored entries (every menu and string file in the remaster's kpf)
		}
		TUniquePtr<IFileHandle> H(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		TArray<uint8> Local;
		if (!H || !ReadAt(H.Get(), E->LocalHeader, 30, Local) || U32(Local.GetData()) != 0x04034b50)
		{
			return false;
		}
		const int64 Data = E->LocalHeader + 30 + U16(Local.GetData() + 26) + U16(Local.GetData() + 28);
		return ReadAt(H.Get(), Data, E->Size, Out);
	}

	bool FKpf::ReadText(const FString& Name, FString& Out) const
	{
		TArray<uint8> Bytes;
		if (!Read(Name, Bytes))
		{
			return false;
		}
		// UTF-8 (a BOM is fine)
		int32 Skip = Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF ? 3 : 0;
		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Skip), Bytes.Num() - Skip);
		Out = FString(Conv.Length(), Conv.Get());
		return true;
	}

	void FKpf::List(const FString& Prefix, TArray<FString>& Out) const
	{
		const FString P = Prefix.ToLower();
		for (const TPair<FString, FEntry>& Kv : Entries)
		{
			if (Kv.Key.StartsWith(P)) Out.Add(Kv.Key);
		}
	}

	bool FSource::Load(const FString& InstallRoot)
	{
		if (!Kpf.Open(FPaths::Combine(InstallRoot, TEXT("Common.kpf"))))
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT menu: no Common.kpf under %s"), *InstallRoot);
			return false;
		}
		UiKpf.Open(FPaths::Combine(InstallRoot, TEXT("UI.kpf")));
		// (REQ-LEGACYKPF) the menu overlay: -OLMenuOverlay=<file> or MenuOverlayPath in OutlawsSettings.ini (editor runs),
		// else Tools\MenuOverlay.kpf in the PC package, else MenuOverlay.kpf beside Common.kpf (the headset's files/Outlaws).
		// -OLNoMenuOverlay reads Common.kpf alone (the stock look); loc_english stays on Common.kpf either way
		if (!OLFlags::Param(TEXT("OLNoMenuOverlay")))
		{
			TArray<FString> Tries;
			FString Arg;
			if (OLFlags::Value(TEXT("OLMenuOverlay"), Arg, /*bShouldStopOnSeparator*/ false)) Tries.Add(FPaths::ConvertRelativePathToFull(Arg.TrimQuotes()));
			const TMap<FString, FString> Settings = AOutlawsLevelActor::ReadSettings();       // (held: Find points into it)
			if (const FString* Set = Settings.Find(TEXT("MenuOverlayPath"))) Tries.Add(FPaths::ConvertRelativePathToFull(Set->TrimQuotes()));
			Tries.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::RootDir(), TEXT("Tools"), TEXT("MenuOverlay.kpf"))));
			Tries.Add(FPaths::Combine(InstallRoot, TEXT("MenuOverlay.kpf")));
			for (const FString& T : Tries)
			{
				if (!T.IsEmpty() && FPaths::FileExists(T) && Overlay.Open(T))
				{
					UE_LOG(LogOutlaws, Log, TEXT("OLRT menu: the menu overlay is open, %d entries over Common.kpf (%s)"), Overlay.Num(), *T);
					break;
				}
			}
		}
		// key = "value" lines; /// and // lines are comments
		FString Loc;
		if (Kpf.ReadText(TEXT("localization/loc_english.txt"), Loc))
		{
			TArray<FString> Lines;
			Loc.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				int32 Eq, Q0, Q1;
				if (!Line.FindChar(TEXT('='), Eq) || !Line.FindChar(TEXT('"'), Q0) || !Line.FindLastChar(TEXT('"'), Q1) || Q1 <= Q0 || Q0 < Eq) continue;
				const FString Key = Line.Left(Eq).TrimStartAndEnd();
				if (Key.IsEmpty() || Key.StartsWith(TEXT("/"))) continue;
				Strings.Add(Key.ToLower(), Line.Mid(Q0 + 1, Q1 - Q0 - 1).Replace(TEXT("\\n"), TEXT(" ")));
			}
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT menu: %d English strings"), Strings.Num());
		return true;
	}

	FString FSource::Loc(const FString& Text) const
	{
		// data bindings ({{ ... }}) are dropped; $keys become their English text
		FString T = Text;
		for (int32 A = T.Find(TEXT("{{")); A != INDEX_NONE; A = T.Find(TEXT("{{")))
		{
			const int32 B = T.Find(TEXT("}}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, A);
			T.RemoveAt(A, (B == INDEX_NONE ? T.Len() : B + 2) - A);
		}
		TArray<FString> Words;
		T.ParseIntoArrayWS(Words);
		for (FString& W : Words)
		{
			if (W.StartsWith(TEXT("$")))
			{
				if (const FString* S = Strings.Find(W.Mid(1).ToLower()))
				{
					W = *S;
				}
			}
		}
		return FString::Join(Words, TEXT(" "));
	}

	bool FSource::GetPage(const FString& File, FPage& Out) const
	{
		FString Text;
		if (!ReadText(TEXT("menu/") + File, Text))
		{
			return false;
		}
		const TSharedPtr<FNode> Root = ParseRml(Text);
		const FNode* Body = FindTag(*Root, TEXT("body"));
		if (!Body)
		{
			return false;
		}
		Out = FPage();
		Out.File = File;
		// platform-only entries that a PC build hides
		auto Hidden = [](const FNode& N)
		{
			return N.HasClass(TEXT("ps5_only")) || N.HasClass(TEXT("ps_only")) || N.HasClass(TEXT("xbox_only")) ||
				N.HasClass(TEXT("switch_only")) || N.HasClass(TEXT("console_only"));
		};
		TFunction<void(const FNode&)> Walk = [&](const FNode& N)
		{
			for (const TSharedPtr<FNode>& Cp : N.Children)
			{
				const FNode& C = *Cp;
				if (Hidden(C) || C.Tag == TEXT("template") || C.Tag == TEXT("menu_helps") || C.Tag == TEXT("dialog_helps"))
				{
					continue;
				}
				if ((C.Tag == TEXT("menu_title") || C.Tag == TEXT("dialog_title")) && Out.Title.IsEmpty())
				{
					Out.Title = Loc(AllText(C));
				}
				else if (C.Tag == TEXT("dialog_message"))
				{
					Out.Message = Loc(AllText(C));
				}
				else if (C.Tag == TEXT("section_heading"))
				{
					FItem I;
					I.Kind = EItemKind::Heading;
					I.Label = Loc(AllText(C));
					Out.Items.Add(I);
				}
				else if (C.Tag == TEXT("menu_link") || C.Tag == TEXT("dialog_link") || C.Tag == TEXT("dialog_button"))
				{
					FItem I;
					const FNode* Fill = FindTag(C, TEXT("m_fill"));
					I.Label = Loc(Fill ? AllText(*Fill) : AllText(C));
					I.Action = C.Get(TEXT("onclick"));
					I.bGoBack = C.Has(TEXT("kex-goback"));
					I.Classes = C.Get(TEXT("class"));
					if (C.Has(TEXT("kex-menu-next")))
					{
						I.Kind = EItemKind::Link;
						I.Target = C.Get(TEXT("kex-menu-next"));
					}
					else if (C.Has(TEXT("kex-dialog-next")))
					{
						I.Kind = EItemKind::Dialog;
						I.Target = C.Get(TEXT("kex-dialog-next"));
					}
					else
					{
						I.Kind = C.Tag == TEXT("dialog_button") ? EItemKind::Button : EItemKind::Action;
					}
					if (!I.Label.IsEmpty()) Out.Items.Add(I);
				}
				else if (C.Tag == TEXT("label"))
				{
					// a caption with its control: <label class="select|range"> text <select>/<input> </label>
					FItem I;
					I.Label = Loc(C.Text);
					if (const FNode* Sel = FindTag(C, TEXT("select")))
					{
						I.Kind = EItemKind::Select;
						I.Name = Sel->Get(TEXT("name"));
						TArray<const FNode*> Opts;
						FindAll(*Sel, TEXT("option"), Opts);
						for (const FNode* O : Opts)
						{
							const FString V = O->Has(TEXT("data-for")) ? FString(TEXT("(from the system)")) : Loc(AllText(*O));
							if (!V.IsEmpty() && !I.Options.Contains(V)) I.Options.Add(V);
						}
					}
					else if (const FNode* In = FindTag(C, TEXT("input")))
					{
						const FString Type = In->Get(TEXT("type"));
						I.Name = In->Get(TEXT("name"));
						I.Kind = Type == TEXT("range") ? EItemKind::Range : Type == TEXT("radio") ? EItemKind::Radio : EItemKind::Check;
						if (I.Kind == EItemKind::Range)
						{
							I.Min = FCString::Atof(*In->Get(TEXT("min")));
							I.Max = In->Has(TEXT("max")) ? FCString::Atof(*In->Get(TEXT("max"))) : 1.f;
							I.Step = FMath::Max(1e-3f, (I.Max - I.Min) / 10.f);
						}
						if (I.Label.IsEmpty()) I.Label = Loc(AllText(*In));
					}
					else
					{
						continue;
					}
					if (!I.Label.IsEmpty()) Out.Items.Add(I);
				}
				else if (C.Tag == TEXT("input"))
				{
					FItem I;
					const FString Type = C.Get(TEXT("type"));
					I.Name = C.Get(TEXT("name"));
					I.Label = Loc(AllText(C));
					I.Kind = Type == TEXT("range") ? EItemKind::Range : Type == TEXT("radio") ? EItemKind::Radio : EItemKind::Check;
					if (I.Kind == EItemKind::Range)
					{
						I.Min = FCString::Atof(*C.Get(TEXT("min")));
						I.Max = C.Has(TEXT("max")) ? FCString::Atof(*C.Get(TEXT("max"))) : 1.f;
						I.Step = FMath::Max(1e-3f, (I.Max - I.Min) / 10.f);
					}
					if (I.Label.IsEmpty()) I.Label = I.Name;
					Out.Items.Add(I);
				}
				else if (C.Tag == TEXT("menu_save") || (C.Has(TEXT("data-for")) && C.Has(TEXT("onclick"))))
				{
					// save / load slots are filled from the system: one row stands for them
					FItem I;
					I.Kind = EItemKind::Action;
					I.Action = C.Get(TEXT("onclick"));
					I.Label = I.Action.Contains(TEXT("save")) ? TEXT("Save slots (from the system)") : TEXT("Saved games (from the system)");
					if (!Out.Items.ContainsByPredicate([&I](const FItem& X) { return X.Label == I.Label; })) Out.Items.Add(I);
				}
				else if (C.Tag == TEXT("kex_bind"))
				{
					FItem I;
					I.Kind = EItemKind::Bind;
					I.Label = Loc(C.Get(TEXT("label")));
					I.Name = C.Get(TEXT("cmd"));
					if (!I.Label.IsEmpty()) Out.Items.Add(I);
				}
				else
				{
					Walk(C);
				}
			}
		};
		Walk(*Body);
		return true;
	}
}

// ---- REQ-TRIGGERSTEP: the one rule for stepping a value row ------------------------------------------------------------------

OutlawsMenu::FStepPress& OutlawsMenu::StepPress()
{
	static FStepPress Press;
	return Press;
}

int32 OutlawsMenu::StepIndex(int32 Cur, int32 Count, bool bNumber)
{
	if (Count <= 0) return Cur;
	if (Cur < 0 || Cur >= Count) return 0;
	FStepPress& P = StepPress();
	const int32 Dir = P.Dir < 0 ? -1 : 1;
	const int32 Next = Cur + Dir;
	if (Next >= 0 && Next < Count) return Next;
	if (bNumber && P.bStopAtEnds)
	{
		P.bHitEnd = true;          // (the end: the row stays; the press's hand gets a short tick)
		return Cur;
	}
	return (Next + Count) % Count;
}

float OutlawsMenu::StepNumber(float Now, float Lo, float Hi, float Step)
{
	FStepPress& P = StepPress();
	const bool bUp = P.Dir >= 0;
	const bool bAtEnd = bUp ? Now >= Hi - Step * 0.02f : Now <= Lo + Step * 0.02f;
	if (bAtEnd)
	{
		if (P.bStopAtEnds)
		{
			P.bHitEnd = true;
			return bUp ? Hi : Lo;
		}
		return bUp ? Lo : Hi;
	}
	return bUp ? FMath::Min(Hi, FMath::RoundToFloat((Now + Step) / Step) * Step) : FMath::Max(Lo, FMath::RoundToFloat((Now - Step) / Step) * Step);
}
