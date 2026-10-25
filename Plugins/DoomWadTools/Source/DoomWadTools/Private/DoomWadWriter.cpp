// Writing maps as PWADs (see DoomWadWriter.h).
#include "DoomWadWriter.h"
#include "DoomMap.h"
#include "DoomWadTools.h"
#include "Misc/FileHelper.h"

namespace
{
	struct FWriter
	{
		TArray<uint8>& Out;
		void U8(uint8 V) { Out.Add(V); }
		void S16(int32 V) { const int16 X = (int16)FMath::Clamp(V, -32768, 32767); Out.Add(X & 0xFF); Out.Add((X >> 8) & 0xFF); }
		void U16(int32 V) { const uint16 X = (uint16)V; Out.Add(X & 0xFF); Out.Add(X >> 8); }
		void S32(int32 V) { for (int32 i = 0; i < 4; ++i) Out.Add((V >> (8 * i)) & 0xFF); }
		void Name8(const FString& N)
		{
			const FString U = N.ToUpper().Left(8);
			for (int32 i = 0; i < 8; ++i) Out.Add(i < U.Len() ? (uint8)U[i] : 0);
		}
		void Tex(FName N) { Name8(N.IsNone() ? FString(TEXT("-")) : N.ToString()); }
	};

	int32 WadRound(float V) { return FMath::RoundToInt(V); }

	/** Doom's BLOCKMAP: 128-unit blocks, each listing the lines that touch it. */
	TArray<uint8> Blockmap(const FDoomMap& M)
	{
		FBox2f B(ForceInit);
		for (const FVector2f& V : M.Vertices) B += V;
		const int32 X0 = FMath::FloorToInt(B.Min.X) - 8, Y0 = FMath::FloorToInt(B.Min.Y) - 8;
		const int32 Cols = FMath::Max(1, FMath::CeilToInt((B.Max.X - X0 + 1) / 128.f));
		const int32 Rows = FMath::Max(1, FMath::CeilToInt((B.Max.Y - Y0 + 1) / 128.f));
		TArray<TArray<int32>> Blocks;
		Blocks.SetNum(Cols * Rows);
		for (int32 i = 0; i < M.Lines.Num(); ++i)
		{
			const FVector2f A = M.V(M.Lines[i].V1), C = M.V(M.Lines[i].V2);
			const int32 BX0 = FMath::Clamp(FMath::FloorToInt((FMath::Min(A.X, C.X) - X0) / 128.f), 0, Cols - 1);
			const int32 BX1 = FMath::Clamp(FMath::FloorToInt((FMath::Max(A.X, C.X) - X0) / 128.f), 0, Cols - 1);
			const int32 BY0 = FMath::Clamp(FMath::FloorToInt((FMath::Min(A.Y, C.Y) - Y0) / 128.f), 0, Rows - 1);
			const int32 BY1 = FMath::Clamp(FMath::FloorToInt((FMath::Max(A.Y, C.Y) - Y0) / 128.f), 0, Rows - 1);
			for (int32 by = BY0; by <= BY1; ++by)
			{
				for (int32 bx = BX0; bx <= BX1; ++bx)
				{
					// does the line cross this block? (the box test is enough for straight runs)
					const FVector2f Lo(X0 + bx * 128.f, Y0 + by * 128.f), Hi = Lo + FVector2f(128.f, 128.f);
					const FVector2f D = C - A;
					auto SideOf = [&](const FVector2f& P) { return D.X * (P.Y - A.Y) - D.Y * (P.X - A.X); };
					const float S0 = SideOf(Lo), S1 = SideOf(FVector2f(Hi.X, Lo.Y)), S2 = SideOf(Hi), S3 = SideOf(FVector2f(Lo.X, Hi.Y));
					const bool bAll = (S0 > 0 && S1 > 0 && S2 > 0 && S3 > 0) || (S0 < 0 && S1 < 0 && S2 < 0 && S3 < 0);
					if (!bAll) Blocks[by * Cols + bx].Add(i);
				}
			}
		}
		TArray<uint8> Data;
		FWriter W{ Data };
		W.S16(X0); W.S16(Y0); W.S16(Cols); W.S16(Rows);
		// offsets in 16-bit words from the lump's start; the lists follow the offset table
		int32 Word = 4 + Cols * Rows;
		TArray<int32> Offsets;
		for (const TArray<int32>& L : Blocks)
		{
			Offsets.Add(Word);
			Word += L.Num() + 2;
		}
		for (int32 O : Offsets) W.U16(FMath::Min(O, 65535));
		for (const TArray<int32>& L : Blocks)
		{
			W.U16(0);
			for (int32 Li : L) W.U16(Li);
			W.U16(0xFFFF);
		}
		return Data;
	}
}

bool DoomWadWriter::MapLumps(const FDoomMap& Source, const FString& MapName, TArray<FLump>& Out, FString* OutNote)
{
	// whole-unit vertices, then fresh nodes on them (the builder's own splits are rounded as written)
	FDoomMap M = Source;
	int32 Rounded = 0;
	int32 MaxV = -1;
	for (const FDoomLine& L : M.Lines) MaxV = FMath::Max3(MaxV, L.V1, L.V2);
	for (int32 i = 0; i <= MaxV && i < M.Vertices.Num(); ++i)
	{
		const FVector2f R(WadRound(M.Vertices[i].X), WadRound(M.Vertices[i].Y));
		Rounded += !R.Equals(M.Vertices[i], 0.001f);
		M.Vertices[i] = R;
	}
	if (!M.Rebuild())
	{
		return false;
	}
	const FString Name = MapName.ToUpper().Left(8);
	Out.Reset();
	Out.Add({ Name, {} });
	auto Add = [&](const TCHAR* LumpName) -> FWriter { FLump& L = Out.Add_GetRef({ LumpName, {} }); return FWriter{ L.Data }; };

	{
		FWriter W = Add(TEXT("THINGS"));
		for (const FDoomMapThing& T : M.Things)
		{
			W.S16(WadRound(T.P.X)); W.S16(WadRound(T.P.Y)); W.S16(WadRound(T.Angle)); W.U16(T.Type); W.U16(T.Flags);
		}
	}
	{
		FWriter W = Add(TEXT("LINEDEFS"));
		for (const FDoomLine& L : M.Lines)
		{
			W.U16(L.V1); W.U16(L.V2);
			W.U16(L.Flags & ~DoomLineFlags::Mapped);
			W.S16(L.Special); W.S16(L.Tag);
			W.U16(L.Side[0] == INDEX_NONE ? 0xFFFF : L.Side[0]);
			W.U16(L.Side[1] == INDEX_NONE ? 0xFFFF : L.Side[1]);
		}
	}
	{
		FWriter W = Add(TEXT("SIDEDEFS"));
		for (const FDoomSide& S : M.Sides)
		{
			W.S16(WadRound(S.XOffset)); W.S16(WadRound(S.YOffset));
			W.Tex(S.Upper); W.Tex(S.Lower); W.Tex(S.Middle);
			W.U16(S.Sector);
		}
	}
	int32 SplitRounded = 0;
	{
		FWriter W = Add(TEXT("VERTEXES"));
		for (int32 i = 0; i < M.Vertices.Num(); ++i)
		{
			const FVector2f& V = M.Vertices[i];
			SplitRounded += (i > MaxV && !FVector2f(WadRound(V.X), WadRound(V.Y)).Equals(V, 0.001f)) ? 1 : 0;
			W.S16(WadRound(V.X)); W.S16(WadRound(V.Y));
		}
	}
	{
		FWriter W = Add(TEXT("SEGS"));
		for (const FDoomSeg& S : M.Segs)
		{
			const FVector2f A = M.V(S.V1), B = M.V(S.V2);
			const double Angle = FMath::Atan2((double)WadRound(B.Y) - WadRound(A.Y), (double)WadRound(B.X) - WadRound(A.X));
			const int32 Bam = (int32)FMath::RoundToDouble((Angle < 0 ? Angle + 2 * UE_DOUBLE_PI : Angle) / (2 * UE_DOUBLE_PI) * 65536.0) & 0xFFFF;
			W.U16(S.V1); W.U16(S.V2); W.U16(Bam);
			W.U16(S.Line == INDEX_NONE ? 0xFFFF : S.Line); W.S16(S.Side); W.S16(WadRound(S.Offset));
		}
	}
	{
		FWriter W = Add(TEXT("SSECTORS"));
		for (const FDoomSubsector& Ss : M.Subsectors)
		{
			W.U16(Ss.NumSegs); W.U16(Ss.FirstSeg);
		}
	}
	{
		// each child's bounding box: its segs' vertices
		TArray<FBox2f> NodeBox;
		NodeBox.Init(FBox2f(ForceInit), M.Nodes.Num());
		auto ChildBox = [&](int32 Child) -> FBox2f
		{
			if (Child & FDoomNode::SubsectorBit)
			{
				FBox2f B(ForceInit);
				const FDoomSubsector& Ss = M.Subsectors[Child & ~FDoomNode::SubsectorBit];
				for (int32 s = Ss.FirstSeg; s < Ss.FirstSeg + Ss.NumSegs; ++s)
				{
					B += M.V(M.Segs[s].V1);
					B += M.V(M.Segs[s].V2);
				}
				return B;
			}
			return NodeBox[Child];
		};
		// children come before their parents (the root is last)
		for (int32 n = 0; n < M.Nodes.Num(); ++n)
		{
			NodeBox[n] = ChildBox(M.Nodes[n].Child[0]) + ChildBox(M.Nodes[n].Child[1]);
		}
		FWriter W = Add(TEXT("NODES"));
		for (const FDoomNode& N : M.Nodes)
		{
			W.S16(WadRound(N.Origin.X)); W.S16(WadRound(N.Origin.Y)); W.S16(WadRound(N.Dir.X)); W.S16(WadRound(N.Dir.Y));
			for (int32 c = 0; c < 2; ++c)
			{
				const FBox2f B = ChildBox(N.Child[c]);
				W.S16(FMath::CeilToInt(B.Max.Y)); W.S16(FMath::FloorToInt(B.Min.Y)); W.S16(FMath::FloorToInt(B.Min.X)); W.S16(FMath::CeilToInt(B.Max.X));
			}
			for (int32 c = 0; c < 2; ++c)
			{
				W.U16((N.Child[c] & FDoomNode::SubsectorBit) ? ((N.Child[c] & ~FDoomNode::SubsectorBit) | 0x8000) : N.Child[c]);
			}
		}
	}
	{
		FWriter W = Add(TEXT("SECTORS"));
		for (const FDoomSector& S : M.Sectors)
		{
			W.S16(WadRound(S.Floor)); W.S16(WadRound(S.Ceiling));
			W.Tex(S.FloorPic); W.Tex(S.CeilingPic);
			W.S16(S.Light); W.S16(S.Special); W.S16(S.Tag);
		}
	}
	{
		// no sector pair is ruled out: an all-zero REJECT
		FLump& L = Out.Add_GetRef({ TEXT("REJECT"), {} });
		L.Data.SetNumZeroed((M.Sectors.Num() * M.Sectors.Num() + 7) / 8);
	}
	Out.Add({ TEXT("BLOCKMAP"), Blockmap(M) });

	const bool bVanillaLimits = M.Vertices.Num() < 65536 && M.Segs.Num() < 65536 && M.Subsectors.Num() < 32768 && M.Nodes.Num() < 32768 && M.Sides.Num() < 65535;
	if (OutNote)
	{
		*OutNote = FString::Printf(TEXT("%d vertices, %d lines, %d sectors, %d things; nodes %s, %d split vertices rounded%s%s"),
			MaxV + 1, M.Lines.Num(), M.Sectors.Num(), M.Things.Num(), *M.NodesFormat, SplitRounded,
			Rounded ? *FString::Printf(TEXT(", %d map vertices rounded"), Rounded) : TEXT(""),
			bVanillaLimits ? TEXT("") : TEXT("; over vanilla's limits (ports with extended limits only)"));
	}
	return true;
}

bool DoomWadWriter::SaveWad(const TArray<FLump>& Lumps, const FString& Path)
{
	TArray<uint8> Data;
	FWriter W{ Data };
	Data.Append((const uint8*)"PWAD", 4);
	W.S32(Lumps.Num());
	W.S32(0); // the directory's offset, filled in below
	TArray<int32> Offsets;
	for (const FLump& L : Lumps)
	{
		Offsets.Add(Data.Num());
		Data.Append(L.Data);
	}
	const int32 Dir = Data.Num();
	for (int32 i = 0; i < Lumps.Num(); ++i)
	{
		W.S32(Lumps[i].Data.Num() ? Offsets[i] : 0);
		W.S32(Lumps[i].Data.Num());
		W.Name8(Lumps[i].Name);
	}
	for (int32 i = 0; i < 4; ++i) Data[8 + i] = (Dir >> (8 * i)) & 0xFF;
	return FFileHelper::SaveArrayToFile(Data, *Path);
}

bool DoomWadWriter::SaveMap(const FDoomMap& Map, const FString& MapName, const FString& Path, FString* OutNote)
{
	TArray<FLump> Lumps;
	if (!MapLumps(Map, MapName, Lumps, OutNote))
	{
		return false;
	}
	const bool bOk = SaveWad(Lumps, Path);
	UE_LOG(LogDoomMap, Log, TEXT("%s %s as %s: %s"), bOk ? TEXT("Saved") : TEXT("Could not save"), *Path, *MapName.ToUpper(), OutNote ? **OutNote : TEXT(""));
	return bOk;
}
