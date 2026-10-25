#include "DoomMapCheck.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomUE.h"

#define LOCTEXT_NAMESPACE "DoomMapCheck"

namespace
{
	using ESev = FDoomMapIssue::ESeverity;

	bool IsDoorUseSpecial(int32 S) { return S == 1 || S == 26 || S == 27 || S == 28 || S == 31 || S == 32 || S == 33 || S == 34 || S == 117 || S == 118; }
	bool IsExit(int32 S) { return S == 11 || S == 51 || S == 52 || S == 124; }
	bool IsTeleport(int32 S) { return S == 39 || S == 97 || S == 125 || S == 126; }
	/** Specials that don't use a tag: manual doors, exits, the scrolling wall. */
	bool UsesTag(int32 S) { return S != 0 && !IsDoorUseSpecial(S) && !IsExit(S) && S != 48; }

	/** 0 blue, 1 yellow, 2 red for locked doors and switches, else INDEX_NONE. */
	int32 LockColor(int32 S)
	{
		switch (S)
		{
		case 26: case 32: case 99: case 133: return 0;
		case 27: case 34: case 136: case 137: return 1;
		case 28: case 33: case 134: case 135: return 2;
		default: return INDEX_NONE;
		}
	}
	FText CheckColorName(int32 C) { return C == 0 ? LOCTEXT("Blue", "blue") : C == 1 ? LOCTEXT("Yellow", "yellow") : LOCTEXT("Red", "red"); }
	/** The key things of a colour: card, skull. */
	bool IsKey(int32 Type, int32 Color)
	{
		static const int32 Keys[3][2] = { { 5, 40 }, { 6, 39 }, { 13, 38 } };
		return Type == Keys[Color][0] || Type == Keys[Color][1];
	}
	FText CheckSkillName(int32 Bit) { return Bit == 1 ? LOCTEXT("Easy", "easy") : Bit == 2 ? LOCTEXT("Medium", "medium") : LOCTEXT("Hard", "hard"); }

	/** Map things that aren't in the info tables but are fine: player starts, teleport destinations,
	 *  MAP30's boss spawner and its target spots. */
	bool IsSpecialThing(int32 Type) { return (Type >= 1 && Type <= 4) || Type == 11 || Type == 14 || Type == 87 || Type == 89; }

	/** Maps a boss death can end (no exit line needed). */
	bool IsBossMap(const FString& M)
	{
		static const TSet<FString> Bosses = { TEXT("E1M8"), TEXT("E2M8"), TEXT("E3M8"), TEXT("E4M6"), TEXT("E4M8"), TEXT("MAP07"), TEXT("MAP30"), TEXT("MAP32") };
		return Bosses.Contains(M.ToUpper());
	}
}

TArray<FDoomMapIssue> DoomMapCheck::Run(const FDoomMap& Map, const FDoomGraphics* Gfx, const FString& MapName)
{
	TArray<FDoomMapIssue> Out;
	auto LineMid = [&Map](int32 L) { return (Map.V(Map.Lines[L].V1) + Map.V(Map.Lines[L].V2)) * 0.5f; };
	auto LineZ = [&Map](int32 L)
	{
		const int32 S = Map.Lines[L].FrontSector;
		return Map.Sectors.IsValidIndex(S) ? Map.Sectors[S].Floor + 32.f : 32.f;
	};
	auto AddLine = [&](ESev Sev, int32 L, const FText& Text)
	{
		FDoomMapIssue I; I.Severity = Sev; I.Line = L; I.Where = LineMid(L); I.Z = LineZ(L);
		I.Text = FText::Format(LOCTEXT("LineIssue", "Line {0}: {1}"), DoomText::Number(L), Text);
		Out.Add(I);
	};
	auto AddSector = [&](ESev Sev, int32 S, const FText& Text)
	{
		FDoomMapIssue I; I.Severity = Sev; I.Sector = S; I.Where = Map.Sectors[S].Center; I.Z = Map.Sectors[S].Floor + 32.f;
		I.Text = FText::Format(LOCTEXT("SectorIssue", "Sector {0}: {1}"), DoomText::Number(S), Text);
		Out.Add(I);
	};
	auto AddThing = [&](ESev Sev, int32 T, const FText& Text)
	{
		const FDoomMapThing& Th = Map.Things[T];
		const int32 S = Map.SectorAt(Th.P);
		FDoomMapIssue I; I.Severity = Sev; I.Thing = T; I.Where = Th.P; I.Z = Map.Sectors.IsValidIndex(S) ? Map.Sectors[S].Floor + 16.f : 0.f;
		const FDoomThingDef* Def = DoomInfo::FindThing(Th.Type);
		I.Text = FText::Format(LOCTEXT("ThingIssue", "Thing {0} ({1}): {2}"), DoomText::Number(T), Def ? FText::FromString(Def->Name) : FText::Format(LOCTEXT("ThingTypeN", "type {0}"), DoomText::Number(Th.Type)), Text);
		Out.Add(I);
	};

	TMultiMap<int32, int32> SectorsByTag;
	for (int32 s = 0; s < Map.Sectors.Num(); ++s)
	{
		SectorsByTag.Add(Map.Sectors[s].Tag, s);
	}

	// --- lines: specials and tags --------------------------------------------------------------
	bool bHasExit = false;
	for (int32 i = 0; i < Map.Lines.Num(); ++i)
	{
		const FDoomLine& L = Map.Lines[i];
		bHasExit |= IsExit(L.Special);
		if (IsDoorUseSpecial(L.Special) && L.BackSector == INDEX_NONE)
		{
			AddLine(ESev::Error, i, FText::Format(LOCTEXT("DoorOneSided", "door special {0} on a one-sided line (a door needs a sector behind it)"), DoomText::Number(L.Special)));
		}
		if (UsesTag(L.Special))
		{
			TArray<int32> Targets;
			SectorsByTag.MultiFind(L.Tag, Targets);
			if (L.Tag == 0)
			{
				AddLine(ESev::Warning, i, FText::Format(LOCTEXT("SpecialTagZero", "special {0} with tag 0: in Doom it works on every untagged sector"), DoomText::Number(L.Special)));
			}
			else if (Targets.Num() == 0)
			{
				AddLine(ESev::Error, i, FText::Format(LOCTEXT("TagNoSector", "special {0} has tag {1}, but no sector has that tag"), DoomText::Number(L.Special), DoomText::Number(L.Tag)));
			}
			else if (IsTeleport(L.Special))
			{
				bool bDestination = false;
				for (const FDoomMapThing& T : Map.Things)
				{
					bDestination |= T.Type == 14 && Targets.Contains(Map.SectorAt(T.P));
				}
				if (!bDestination)
				{
					AddLine(ESev::Error, i, FText::Format(LOCTEXT("TeleportNoDest", "teleporter to tag {0}, but no teleport destination stands in those sectors"), DoomText::Number(L.Tag)));
				}
			}
		}
		else if (L.Tag != 0 && L.Special == 0)
		{
			AddLine(ESev::Info, i, FText::Format(LOCTEXT("TagNoSpecial", "tag {0} without a special"), DoomText::Number(L.Tag)));
		}
		// textures
		if (Gfx)
		{
			for (int32 s = 0; s < 2; ++s)
			{
				if (!Map.Sides.IsValidIndex(L.Side[s]))
				{
					continue;
				}
				const FDoomSide& Sd = Map.Sides[L.Side[s]];
				for (FName N : { Sd.Upper, Sd.Middle, Sd.Lower })
				{
					if (!N.IsNone() && !Gfx->HasTexture(N))
					{
						AddLine(ESev::Error, i, FText::Format(s ? LOCTEXT("UnknownTextureBack", "unknown texture {0} on the back side") : LOCTEXT("UnknownTextureFront", "unknown texture {0} on the front side"), FText::FromName(N)));
					}
					if (!N.IsNone() && !Gfx->SwitchPartner(N).IsNone() && L.Special == 0)
					{
						// often on purpose (decoration), so only a note
						AddLine(ESev::Info, i, FText::Format(LOCTEXT("SwitchNoSpecial", "switch texture {0} on a line with no special"), FText::FromName(N)));
					}
				}
			}
		}
		if (L.Side[0] == INDEX_NONE)
		{
			AddLine(ESev::Error, i, LOCTEXT("NoFrontSide", "no front side"));
			continue;
		}
		const FDoomSide& Front = Map.Sides[L.Side[0]];
		if (L.BackSector == INDEX_NONE)
		{
			if (Front.Middle.IsNone())
			{
				AddLine(ESev::Error, i, LOCTEXT("MissingMiddleTextureOnA", "missing middle texture on a one-sided wall"));
			}
			continue;
		}
		if (L.FrontSector == L.BackSector || !Map.Sides.IsValidIndex(L.Side[1]))
		{
			continue; // self-referencing sectors (deep water and similar tricks) leave these out on purpose
		}
		const FDoomSide& Back = Map.Sides[L.Side[1]];
		const FDoomSector& FS = Map.Sectors[L.FrontSector];
		const FDoomSector& BS = Map.Sectors[L.BackSector];
		const bool bBothSky = FDoomGraphics::IsSkyFlat(FS.CeilingPic) && FDoomGraphics::IsSkyFlat(BS.CeilingPic);
		// warnings, not errors: maps leave some out on purpose (the step is never seen, a special effect)
		if (FS.Floor < BS.Floor && Front.Lower.IsNone()) AddLine(ESev::Warning, i, LOCTEXT("MissingLowerTextureOnThe", "missing lower texture on the front side (the step up may show a hole)"));
		if (BS.Floor < FS.Floor && Back.Lower.IsNone()) AddLine(ESev::Warning, i, LOCTEXT("MissingLowerTextureOnThe", "missing lower texture on the back side (the step up may show a hole)"));
		if (!bBothSky && FS.Ceiling > BS.Ceiling && Front.Upper.IsNone()) AddLine(ESev::Warning, i, LOCTEXT("MissingUpperTextureOnThe", "missing upper texture on the front side (the lower ceiling may show a hole)"));
		if (!bBothSky && BS.Ceiling > FS.Ceiling && Back.Upper.IsNone()) AddLine(ESev::Warning, i, LOCTEXT("MissingUpperTextureOnThe", "missing upper texture on the back side (the lower ceiling may show a hole)"));
	}
	if (!bHasExit && !IsBossMap(MapName))
	{
		FDoomMapIssue I; I.Severity = ESev::Warning; I.Text = LOCTEXT("NoExit", "No exit line (11, 51, 52 or 124): the level can't be finished");
		Out.Add(I);
	}

	// --- sectors: closed outlines, heights -----------------------------------------------------
	for (int32 s = 0; s < Map.Sectors.Num(); ++s)
	{
		const FDoomSector& S = Map.Sectors[s];
		if (S.Ceiling < S.Floor)
		{
			AddSector(ESev::Error, s, FText::Format(LOCTEXT("CeilingBelowFloor", "ceiling {0} below floor {1}"), DoomText::Number(FMath::RoundToInt(S.Ceiling)), DoomText::Number(FMath::RoundToInt(S.Floor))));
		}
		// every vertex of a closed outline is used an even number of times (lines with this sector on
		// both sides don't count)
		TMap<int32, int32> Uses;
		for (int32 Li : S.Lines)
		{
			const FDoomLine& L = Map.Lines[Li];
			if (L.FrontSector == s && L.BackSector == s) continue;
			Uses.FindOrAdd(L.V1)++;
			Uses.FindOrAdd(L.V2)++;
		}
		int32 Open = 0;
		for (const TPair<int32, int32>& U : Uses)
		{
			Open += (U.Value & 1);
		}
		if (Open > 0)
		{
			AddSector(ESev::Error, s, FText::Format(LOCTEXT("NotClosed", "not closed: {0} line ends meet no other line of the sector"), DoomText::Number(Open)));
		}
	}

	// --- things: starts, keys, fit -------------------------------------------------------------
	bool bStart = false;
	int32 CoopStarts = 0;
	for (int32 t = 0; t < Map.Things.Num(); ++t)
	{
		const FDoomMapThing& T = Map.Things[t];
		bStart |= T.Type == 1;
		CoopStarts += (T.Type >= 2 && T.Type <= 4);
		const int32 S = Map.SectorAt(T.P);
		if (!Map.Sectors.IsValidIndex(S))
		{
			AddThing(ESev::Error, t, LOCTEXT("OutsideTheMap", "outside the map"));
			continue;
		}
		const FDoomThingDef* Def = DoomInfo::FindThing(T.Type);
		if (!Def && !IsSpecialThing(T.Type))
		{
			AddThing(ESev::Warning, t, LOCTEXT("UnknownThingTypeAPwad", "unknown thing type (a PWAD's own, or a typo)"));
			continue;
		}
		// (a closed door, 0 high, is a monster closet: it opens later)
		const float Room = Map.Sectors[S].Ceiling - Map.Sectors[S].Floor;
		if (Def && (Def->Flags & EThingFlag::CountKill) && !(Def->Flags & EThingFlag::Hanging) && Room > 0.f && Room < Def->Height)
		{
			AddThing(ESev::Warning, t, FText::Format(LOCTEXT("TooTall", "too tall for its sector ({0} units high, needs {1}): it can't move"), DoomText::Number(FMath::RoundToInt(Map.Sectors[S].Ceiling - Map.Sectors[S].Floor)), DoomText::Number(FMath::RoundToInt(Def->Height))));
		}
		if ((T.Flags & 7) == 0 && !IsSpecialThing(T.Type))
		{
			AddThing(ESev::Warning, t, LOCTEXT("OnNoSkillLevelIt", "on no skill level: it never appears"));
		}
	}
	if (!bStart)
	{
		FDoomMapIssue I; I.Severity = ESev::Error; I.Text = LOCTEXT("NoPlayerStart", "No player 1 start (thing 1)");
		Out.Add(I);
	}
	else if (CoopStarts < 3)
	{
		FDoomMapIssue I; I.Severity = ESev::Info; I.Text = FText::Format(LOCTEXT("CoopStarts", "{0} of the 3 co-op starts (things 2-4): co-op needs them"), CoopStarts);
		Out.Add(I);
	}
	// locked doors and switches need their key on every skill they're played on
	for (int32 i = 0; i < Map.Lines.Num(); ++i)
	{
		const int32 Color = LockColor(Map.Lines[i].Special);
		if (Color == INDEX_NONE)
		{
			continue;
		}
		for (int32 Bit : { 1, 2, 4 })
		{
			bool bKey = false;
			for (const FDoomMapThing& T : Map.Things)
			{
				bKey |= IsKey(T.Type, Color) && (T.Flags & Bit) && !(T.Flags & 16);
			}
			if (!bKey)
			{
				AddLine(ESev::Error, i, FText::Format(LOCTEXT("LockNoKey", "{0} lock, but no {0} key on {1} skill"), CheckColorName(Color), CheckSkillName(Bit)));
			}
		}
	}
	// errors first, then warnings, then notes; in map order within each
	Out.StableSort([](const FDoomMapIssue& A, const FDoomMapIssue& B) { return (uint8)A.Severity < (uint8)B.Severity; });
	return Out;
}

int32 DoomMapCheck::Count(const TArray<FDoomMapIssue>& Issues, FDoomMapIssue::ESeverity Severity)
{
	int32 N = 0;
	for (const FDoomMapIssue& I : Issues) N += I.Severity == Severity;
	return N;
}

void DoomMapCheck::Log(const FString& MapName, const TArray<FDoomMapIssue>& Issues)
{
	UE_LOG(LogDoomMap, Display, TEXT("Checkmap %s: %d errors, %d warnings, %d notes"), *MapName,
		Count(Issues, ESev::Error), Count(Issues, ESev::Warning), Count(Issues, ESev::Info));
	for (const FDoomMapIssue& I : Issues)
	{
		const TCHAR* Kind = I.Severity == ESev::Error ? TEXT("error") : I.Severity == ESev::Warning ? TEXT("warning") : TEXT("note");
		UE_LOG(LogDoomMap, Display, TEXT("Checkmap %s %s: %s"), *MapName, Kind, *I.Text.ToString());
	}
}

#undef LOCTEXT_NAMESPACE
