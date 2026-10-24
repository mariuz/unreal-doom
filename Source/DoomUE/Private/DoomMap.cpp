#include "DoomMap.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "zlib.h"

using namespace WadRead;

namespace
{
	enum EMapLump { ML_THINGS = 1, ML_LINEDEFS, ML_SIDEDEFS, ML_VERTEXES, ML_SEGS, ML_SSECTORS, ML_NODES, ML_SECTORS };

	FName MapTexName(const uint8* P)
	{
		const FString S = Name8(P);
		return (S.IsEmpty() || S == TEXT("-")) ? NAME_None : FName(*S);
	}

	float MapCross(const FVector2f& A, const FVector2f& B) { return A.X * B.Y - A.Y * B.X; }

	/** Keeps the part of Poly where MapCross(Dir, P - Origin) * Sign <= 0 (Sign 1: the right side). */
	void ClipPolygon(TArray<FVector2f>& Poly, const FVector2f& Origin, const FVector2f& Dir, float Sign)
	{
		if (Poly.Num() < 3)
		{
			return;
		}
		const float Len = Dir.Size();
		if (Len < 1e-6f)
		{
			return;
		}
		const FVector2f N = Dir / Len;
		constexpr float Eps = 0.01f;
		TArray<FVector2f> Out;
		Out.Reserve(Poly.Num() + 2);
		for (int32 i = 0; i < Poly.Num(); ++i)
		{
			const FVector2f& A = Poly[i];
			const FVector2f& B = Poly[(i + 1) % Poly.Num()];
			const float DA = MapCross(N, A - Origin) * Sign;
			const float DB = MapCross(N, B - Origin) * Sign;
			const bool bInA = DA <= Eps;
			const bool bInB = DB <= Eps;
			if (bInA)
			{
				Out.Add(A);
			}
			if (bInA != bInB)
			{
				const float T = DA / (DA - DB);
				Out.Add(A + (B - A) * T);
			}
		}
		// drop near-duplicate points
		TArray<FVector2f> Clean;
		for (const FVector2f& P : Out)
		{
			if (Clean.Num() == 0 || FVector2f::DistSquared(Clean.Last(), P) > 1e-4f)
			{
				Clean.Add(P);
			}
		}
		if (Clean.Num() > 1 && FVector2f::DistSquared(Clean[0], Clean.Last()) <= 1e-4f)
		{
			Clean.Pop();
		}
		Poly = MoveTemp(Clean);
	}
}

bool FDoomMap::Load(const FDoomWad& Wad, const FString& MapName)
{
	Name = MapName.ToUpper();
	const int32 Marker = Wad.FindLump(Name);
	if (Marker == INDEX_NONE)
	{
		UE_LOG(LogDoomMap, Error, TEXT("Map %s not found"), *Name);
		return false;
	}
	const bool bLoaded = (Marker + 1 < Wad.NumLumps() && Wad.Lump(Marker + 1).NameStr == TEXT("TEXTMAP"))
		? LoadUdmf(Wad, Marker)
		: LoadBinary(Wad, Marker);
	if (!bLoaded || Subsectors.Num() == 0)
	{
		UE_LOG(LogDoomMap, Error, TEXT("%s: no usable BSP (vanilla, ZDoom extended or glBSP GL nodes)"), *Name);
		return false;
	}
	AssignSubsectorSectors();
	Derive();
	BuildSubsectorPolygons();

	double Area = 0;
	for (const FDoomSubsector& Ss : Subsectors)
	{
		for (int32 k = 1; k + 1 < Ss.Polygon.Num(); ++k)
		{
			Area += 0.5 * MapCross(Ss.Polygon[k] - Ss.Polygon[0], Ss.Polygon[k + 1] - Ss.Polygon[0]);
		}
	}
	UE_LOG(LogDoomMap, Log, TEXT("Loaded %s: %d vertices, %d lines, %d sectors, %d subsectors (area %.0f), %d things, %s nodes"),
		*Name, Vertices.Num(), Lines.Num(), Sectors.Num(), Subsectors.Num(), Area, Things.Num(), *NodesFormat);
	return true;
}

namespace
{
	/** UDMF tokens: identifiers / numbers, "strings", and the symbols { } = ; */
	struct FUdmfLexer
	{
		const FString& S;
		int32 P = 0;

		void Skip()
		{
			while (P < S.Len())
			{
				if (FChar::IsWhitespace(S[P])) { ++P; continue; }
				if (S[P] == '/' && P + 1 < S.Len() && S[P + 1] == '/') { while (P < S.Len() && S[P] != '\n') ++P; continue; }
				if (S[P] == '/' && P + 1 < S.Len() && S[P + 1] == '*')
				{
					const int32 E = S.Find(TEXT("*/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, P + 2);
					P = E == INDEX_NONE ? S.Len() : E + 2;
					continue;
				}
				break;
			}
		}

		/** Next token; bString tells a quoted string from a bare word. Empty at the end. */
		FString Next(bool& bString)
		{
			Skip();
			bString = false;
			if (P >= S.Len()) return FString();
			const TCHAR C = S[P];
			if (C == '{' || C == '}' || C == '=' || C == ';') { ++P; return FString(1, &C); }
			FString T;
			if (C == '"')
			{
				bString = true;
				++P;
				while (P < S.Len() && S[P] != '"')
				{
					if (S[P] == '\\' && P + 1 < S.Len()) ++P;
					T.AppendChar(S[P++]);
				}
				++P;
				return T;
			}
			while (P < S.Len() && !FChar::IsWhitespace(S[P]) && S[P] != '{' && S[P] != '}' && S[P] != '=' && S[P] != ';' && S[P] != '"')
			{
				T.AppendChar(S[P++]);
			}
			return T;
		}
	};

	using FUdmfBlock = TMap<FString, FString>;

	float UdmfFloat(const FUdmfBlock& B, const TCHAR* Key, float Default)
	{
		const FString* V = B.Find(Key);
		return V ? FCString::Atof(**V) : Default;
	}
	int32 UdmfInt(const FUdmfBlock& B, const TCHAR* Key, int32 Default)
	{
		const FString* V = B.Find(Key);
		return V ? FCString::Atoi(**V) : Default;
	}
	bool UdmfBool(const FUdmfBlock& B, const TCHAR* Key, bool Default = false)
	{
		const FString* V = B.Find(Key);
		return V ? V->Equals(TEXT("true"), ESearchCase::IgnoreCase) : Default;
	}
	FName UdmfTexture(const FUdmfBlock& B, const TCHAR* Key)
	{
		const FString* V = B.Find(Key);
		return (!V || V->IsEmpty() || *V == TEXT("-")) ? NAME_None : FName(*V->ToUpper().Left(8));
	}
}

bool FDoomMap::LoadUdmf(const FDoomWad& Wad, int32 Marker)
{
	TConstArrayView<uint8> Raw = Wad.LumpData(Marker + 1);
	const FString Text(Raw.Num(), reinterpret_cast<const ANSICHAR*>(Raw.GetData()));
	FUdmfLexer Lex{ Text };
	FString Namespace;
	TArray<TPair<FString, FUdmfBlock>> Blocks;
	bool bString = false;
	for (FString Tok = Lex.Next(bString); !Tok.IsEmpty(); Tok = Lex.Next(bString))
	{
		const FString Key = Tok.ToLower();
		const FString After = Lex.Next(bString);
		if (After == TEXT("="))
		{
			const FString Value = Lex.Next(bString);
			Lex.Next(bString); // ;
			if (Key == TEXT("namespace")) Namespace = Value.ToLower();
			continue;
		}
		if (After != TEXT("{"))
		{
			UE_LOG(LogDoomMap, Error, TEXT("%s: TEXTMAP syntax error near '%s'"), *Name, *Tok);
			return false;
		}
		FUdmfBlock Block;
		for (FString F = Lex.Next(bString); !F.IsEmpty() && F != TEXT("}"); F = Lex.Next(bString))
		{
			Lex.Next(bString); // =
			const FString V = Lex.Next(bString);
			Lex.Next(bString); // ;
			Block.Add(F.ToLower(), V);
		}
		Blocks.Emplace(Key, MoveTemp(Block));
	}

	for (const TPair<FString, FUdmfBlock>& B : Blocks)
	{
		const FUdmfBlock& F = B.Value;
		if (B.Key == TEXT("vertex"))
		{
			Vertices.Add(FVector2f(UdmfFloat(F, TEXT("x"), 0), UdmfFloat(F, TEXT("y"), 0)));
		}
		else if (B.Key == TEXT("sector"))
		{
			FDoomSector& S = Sectors.AddDefaulted_GetRef();
			S.Floor = UdmfFloat(F, TEXT("heightfloor"), 0);
			S.Ceiling = UdmfFloat(F, TEXT("heightceiling"), 0);
			S.FloorPic = UdmfTexture(F, TEXT("texturefloor"));
			S.CeilingPic = UdmfTexture(F, TEXT("textureceiling"));
			S.Light = S.OriginalLight = (int16)UdmfInt(F, TEXT("lightlevel"), 160);
			S.Special = (int16)UdmfInt(F, TEXT("special"), 0);
			S.Tag = (int16)UdmfInt(F, TEXT("id"), 0);
		}
		else if (B.Key == TEXT("sidedef"))
		{
			FDoomSide& S = Sides.AddDefaulted_GetRef();
			S.XOffset = UdmfFloat(F, TEXT("offsetx"), 0);
			S.YOffset = UdmfFloat(F, TEXT("offsety"), 0);
			S.Upper = UdmfTexture(F, TEXT("texturetop"));
			S.Lower = UdmfTexture(F, TEXT("texturebottom"));
			S.Middle = UdmfTexture(F, TEXT("texturemiddle"));
			S.Sector = UdmfInt(F, TEXT("sector"), 0);
		}
		else if (B.Key == TEXT("linedef"))
		{
			FDoomLine& L = Lines.AddDefaulted_GetRef();
			L.V1 = UdmfInt(F, TEXT("v1"), 0);
			L.V2 = UdmfInt(F, TEXT("v2"), 0);
			L.Special = (int16)UdmfInt(F, TEXT("special"), 0);
			// Doom-namespace specials keep their sector tag in arg0 (else the line id)
			L.Tag = (int16)UdmfInt(F, TEXT("arg0"), FMath::Max(0, UdmfInt(F, TEXT("id"), 0)));
			L.Side[0] = UdmfInt(F, TEXT("sidefront"), -1);
			L.Side[1] = UdmfInt(F, TEXT("sideback"), -1);
			static const TPair<const TCHAR*, uint16> FlagNames[] = {
				{ TEXT("blocking"), DoomLineFlags::Blocking }, { TEXT("blockmonsters"), DoomLineFlags::BlockMonsters },
				{ TEXT("twosided"), DoomLineFlags::TwoSided }, { TEXT("dontpegtop"), DoomLineFlags::DontPegTop },
				{ TEXT("dontpegbottom"), DoomLineFlags::DontPegBottom }, { TEXT("secret"), DoomLineFlags::Secret },
				{ TEXT("blocksound"), DoomLineFlags::SoundBlock }, { TEXT("dontdraw"), DoomLineFlags::DontDraw },
				{ TEXT("mapped"), DoomLineFlags::Mapped } };
			for (const auto& Flag : FlagNames)
			{
				if (UdmfBool(F, Flag.Key)) L.Flags |= Flag.Value;
			}
		}
		else if (B.Key == TEXT("thing"))
		{
			FDoomMapThing& T = Things.AddDefaulted_GetRef();
			T.P = FVector2f(UdmfFloat(F, TEXT("x"), 0), UdmfFloat(F, TEXT("y"), 0));
			T.Angle = UdmfFloat(F, TEXT("angle"), 0);
			T.Type = UdmfInt(F, TEXT("type"), 0);
			uint16 Flags = 0;
			if (UdmfBool(F, TEXT("skill1")) || UdmfBool(F, TEXT("skill2"))) Flags |= 1;
			if (UdmfBool(F, TEXT("skill3"))) Flags |= 2;
			if (UdmfBool(F, TEXT("skill4")) || UdmfBool(F, TEXT("skill5"))) Flags |= 4;
			if (UdmfBool(F, TEXT("ambush"))) Flags |= 8;
			if (!UdmfBool(F, TEXT("single"), false)) Flags |= 16;
			T.Flags = Flags;
		}
	}
	// validate references
	for (FDoomSide& S : Sides)
	{
		S.Sector = FMath::Clamp(S.Sector, 0, FMath::Max(0, Sectors.Num() - 1));
	}
	for (FDoomLine& L : Lines)
	{
		for (int32& Side : L.Side)
		{
			if (!Sides.IsValidIndex(Side)) Side = INDEX_NONE;
		}
	}
	if (Sectors.Num() == 0 || Vertices.Num() == 0)
	{
		UE_LOG(LogDoomMap, Error, TEXT("%s: TEXTMAP has no geometry"), *Name);
		return false;
	}
	if (Namespace != TEXT("doom"))
	{
		UE_LOG(LogDoomMap, Warning, TEXT("%s: UDMF namespace \"%s\": only Doom-format specials are supported"), *Name, *Namespace);
	}

	// nodes: the ZNODES lump between TEXTMAP and ENDMAP (as written by ZDBSP and other builders)
	for (int32 i = Marker + 2; i < Wad.NumLumps() && Wad.Lump(i).NameStr != TEXT("ENDMAP"); ++i)
	{
		if (Wad.Lump(i).NameStr == TEXT("ZNODES"))
		{
			const bool bOk = LoadExtendedNodes(Wad.LumpData(i));
			NodesFormat = TEXT("UDMF + ") + NodesFormat;
			return bOk;
		}
	}
	UE_LOG(LogDoomMap, Error, TEXT("%s: UDMF map without ZNODES (build its nodes with ZDBSP or a map editor)"), *Name);
	return false;
}

bool FDoomMap::LoadBinary(const FDoomWad& Wad, int32 Marker)
{
	auto Get = [&](int32 Offset, const TCHAR* Expected) -> TConstArrayView<uint8>
	{
		const int32 Index = Marker + Offset;
		if (Index < Wad.NumLumps() && Wad.Lump(Index).NameStr == Expected)
		{
			return Wad.LumpData(Index);
		}
		return {};
	};

	TConstArrayView<uint8> D = Get(ML_VERTEXES, TEXT("VERTEXES"));
	Vertices.SetNum(D.Num() / 4);
	for (int32 i = 0; i < Vertices.Num(); ++i)
	{
		Vertices[i] = FVector2f(S16(&D[i * 4]), S16(&D[i * 4 + 2]));
	}

	D = Get(ML_SECTORS, TEXT("SECTORS"));
	Sectors.SetNum(D.Num() / 26);
	for (int32 i = 0; i < Sectors.Num(); ++i)
	{
		const uint8* P = &D[i * 26];
		FDoomSector& S = Sectors[i];
		S.Floor = S16(P);
		S.Ceiling = S16(P + 2);
		S.FloorPic = MapTexName(P + 4);
		S.CeilingPic = MapTexName(P + 12);
		S.Light = S.OriginalLight = S16(P + 20);
		S.Special = S16(P + 22);
		S.Tag = S16(P + 24);
	}
	if (Sectors.Num() == 0)
	{
		UE_LOG(LogDoomMap, Error, TEXT("%s has no sectors"), *Name);
		return false;
	}

	D = Get(ML_SIDEDEFS, TEXT("SIDEDEFS"));
	Sides.SetNum(D.Num() / 30);
	for (int32 i = 0; i < Sides.Num(); ++i)
	{
		const uint8* P = &D[i * 30];
		FDoomSide& S = Sides[i];
		S.XOffset = S16(P);
		S.YOffset = S16(P + 2);
		S.Upper = MapTexName(P + 4);
		S.Lower = MapTexName(P + 12);
		S.Middle = MapTexName(P + 20);
		S.Sector = FMath::Clamp<int32>(U16(P + 28), 0, Sectors.Num() - 1);
	}

	D = Get(ML_LINEDEFS, TEXT("LINEDEFS"));
	Lines.SetNum(D.Num() / 14);
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const uint8* P = &D[i * 14];
		FDoomLine& L = Lines[i];
		L.V1 = U16(P);
		L.V2 = U16(P + 2);
		L.Flags = U16(P + 4);
		L.Special = S16(P + 6);
		L.Tag = S16(P + 8);
		for (int32 s = 0; s < 2; ++s)
		{
			const uint16 Sd = U16(P + 10 + s * 2);
			L.Side[s] = (Sd == 0xFFFF || Sd >= Sides.Num()) ? INDEX_NONE : Sd;
		}
	}

	D = Get(ML_THINGS, TEXT("THINGS"));
	Things.SetNum(D.Num() / 10);
	for (int32 i = 0; i < Things.Num(); ++i)
	{
		const uint8* P = &D[i * 10];
		FDoomMapThing& T = Things[i];
		T.P = FVector2f(S16(P), S16(P + 2));
		T.Angle = S16(P + 4);
		T.Type = U16(P + 6);
		T.Flags = U16(P + 8);
	}

	// REJECT, when it is the size the sectors want
	{
		const TConstArrayView<uint8> R = Get(9, TEXT("REJECT"));
		const int64 Want = ((int64)Sectors.Num() * Sectors.Num() + 7) / 8;
		Reject.Reset();
		if (R.Num() >= Want) Reject = TArray<uint8>(R.GetData(), (int32)Want);
	}

	TConstArrayView<uint8> NodesL = Get(ML_NODES, TEXT("NODES"));
	TConstArrayView<uint8> SegsL = Get(ML_SEGS, TEXT("SEGS"));
	TConstArrayView<uint8> SsL = Get(ML_SSECTORS, TEXT("SSECTORS"));
	auto HasMagic = [](TConstArrayView<uint8> L, std::initializer_list<const char*> Magics)
	{
		for (const char* M : Magics)
		{
			if (L.Num() >= 4 && FMemory::Memcmp(L.GetData(), M, 4) == 0) return true;
		}
		return false;
	};
	if (HasMagic(NodesL, { "XNOD", "ZNOD", "XGLN", "ZGLN", "XGL2", "ZGL2", "XGL3", "ZGL3" }))
	{
		return LoadExtendedNodes(NodesL);
	}
	else if (HasMagic(SsL, { "XGLN", "ZGLN", "XGL2", "ZGL2", "XGL3", "ZGL3" }))
	{
		return LoadExtendedNodes(SsL);
	}
	else if (NodesL.Num() == 0 || SsL.Num() == 0 || SegsL.Num() == 0)
	{
		const int32 GlMarker = Wad.FindLump(TEXT("GL_") + Name);
		return GlMarker != INDEX_NONE && LoadGlNodes(Wad, GlMarker);
	}
	return LoadVanillaNodes(NodesL, SegsL, SsL);
}

bool FDoomMap::LoadVanillaNodes(TConstArrayView<uint8> NodesLump, TConstArrayView<uint8> SegsLump, TConstArrayView<uint8> SsLump)
{
	if (NodesLump.Num() >= 4 && (FMemory::Memcmp(NodesLump.GetData(), "XNOD", 4) == 0 || FMemory::Memcmp(NodesLump.GetData(), "ZNOD", 4) == 0))
	{
		return false;
	}
	Segs.SetNum(SegsLump.Num() / 12);
	for (int32 i = 0; i < Segs.Num(); ++i)
	{
		const uint8* P = &SegsLump[i * 12];
		FDoomSeg& S = Segs[i];
		S.V1 = FMath::Min<int32>(U16(P), Vertices.Num() - 1);
		S.V2 = FMath::Min<int32>(U16(P + 2), Vertices.Num() - 1);
		S.Line = U16(P + 6);
		S.Side = S16(P + 8) ? 1 : 0;
		S.Offset = S16(P + 10);
		if (!Lines.IsValidIndex(S.Line))
		{
			S.Line = INDEX_NONE;
		}
	}
	Subsectors.SetNum(SsLump.Num() / 4);
	for (int32 i = 0; i < Subsectors.Num(); ++i)
	{
		FDoomSubsector& Ss = Subsectors[i];
		Ss.NumSegs = U16(&SsLump[i * 4]);
		Ss.FirstSeg = U16(&SsLump[i * 4 + 2]);
		Ss.Sector = 0;
		for (int32 s = Ss.FirstSeg; s < Ss.FirstSeg + Ss.NumSegs && s < Segs.Num(); ++s)
		{
			const FDoomSeg& Seg = Segs[s];
			if (Seg.Line != INDEX_NONE)
			{
				const int32 Side = Lines[Seg.Line].Side[Seg.Side];
				if (Side != INDEX_NONE)
				{
					Ss.Sector = Sides[Side].Sector;
					break;
				}
			}
		}
	}
	Nodes.SetNum(NodesLump.Num() / 28);
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const uint8* P = &NodesLump[i * 28];
		FDoomNode& N = Nodes[i];
		N.Origin = FVector2f(S16(P), S16(P + 2));
		N.Dir = FVector2f(S16(P + 4), S16(P + 6));
		for (int32 c = 0; c < 2; ++c)
		{
			const uint16 Child = U16(P + 24 + c * 2);
			N.Child[c] = (Child & 0x8000) ? ((Child & 0x7FFF) | FDoomNode::SubsectorBit) : Child;
		}
	}
	return Subsectors.Num() > 0;
}

void FDoomMap::AssignSubsectorSectors()
{
	for (FDoomSubsector& Ss : Subsectors)
	{
		Ss.Sector = 0;
		for (int32 s = Ss.FirstSeg; s < Ss.FirstSeg + Ss.NumSegs && s < Segs.Num(); ++s)
		{
			const FDoomSeg& Seg = Segs[s];
			if (Seg.Line != INDEX_NONE && Lines[Seg.Line].Side[Seg.Side] != INDEX_NONE)
			{
				Ss.Sector = Sides[Lines[Seg.Line].Side[Seg.Side]].Sector;
				break;
			}
		}
	}
}

namespace
{
	/** Bounds-checked little-endian cursor; any overrun marks it bad. */
	struct FCursor
	{
		const uint8* P;
		int32 Num;
		int32 Pos = 0;
		bool bBad = false;
		bool Need(int32 N) { if (Pos + N > Num) { bBad = true; return false; } return true; }
		uint8 U8() { return Need(1) ? P[Pos++] : 0; }
		uint16 U16() { if (!Need(2)) return 0; const uint16 V = WadRead::U16(P + Pos); Pos += 2; return V; }
		int16 S16() { return (int16)U16(); }
		uint32 U32() { if (!Need(4)) return 0; const uint32 V = (uint32)WadRead::S32(P + Pos); Pos += 4; return V; }
		int32 S32() { return (int32)U32(); }
	};

	/** zlib stream of unknown inflated size (ZNOD, ZGLN, ZGL2, ZGL3). */
	bool Inflate(const uint8* Src, int32 SrcLen, TArray<uint8>& Out)
	{
		z_stream Z;
		FMemory::Memzero(Z);
		if (inflateInit(&Z) != Z_OK) return false;
		Z.next_in = const_cast<Bytef*>(Src);
		Z.avail_in = SrcLen;
		Out.SetNum(FMath::Max(SrcLen * 4, 4096));
		int32 Ret = Z_OK;
		while (Ret == Z_OK)
		{
			if ((int32)Z.total_out >= Out.Num()) Out.SetNum(Out.Num() * 2);
			Z.next_out = Out.GetData() + Z.total_out;
			Z.avail_out = Out.Num() - Z.total_out;
			Ret = inflate(&Z, Z_NO_FLUSH);
		}
		Out.SetNum(Z.total_out);
		inflateEnd(&Z);
		return Ret == Z_STREAM_END;
	}
}

bool FDoomMap::LoadExtendedNodes(TConstArrayView<uint8> Lump)
{
	const FString Magic = FString::ConstructFromPtrSize(reinterpret_cast<const ANSICHAR*>(Lump.GetData()), 4);
	NodesFormat = Magic;
	TArray<uint8> Inflated;
	const uint8* Data = Lump.GetData() + 4;
	int32 Len = Lump.Num() - 4;
	if (Magic[0] == 'Z')
	{
		if (!Inflate(Data, Len, Inflated))
		{
			UE_LOG(LogDoomMap, Error, TEXT("%s: %s nodes do not decompress"), *Name, *Magic);
			return false;
		}
		Data = Inflated.GetData();
		Len = Inflated.Num();
	}
	const bool bGl = Magic.EndsWith(TEXT("GLN")) || Magic.EndsWith(TEXT("GL2")) || Magic.EndsWith(TEXT("GL3"));
	const bool bLongLines = Magic.EndsWith(TEXT("GL2")) || Magic.EndsWith(TEXT("GL3"));
	const bool bFixedNodes = Magic.EndsWith(TEXT("GL3"));
	FCursor C{ Data, Len };

	// vertices: the map's own first, then the node builder's extra ones (16.16 fixed point)
	const uint32 OrgVerts = C.U32();
	const uint32 NewVerts = C.U32();
	const int32 Base = Vertices.Num();
	if (NewVerts > (uint32)(Len / 8)) return false;
	for (uint32 i = 0; i < NewVerts && !C.bBad; ++i)
	{
		const int32 X = C.S32(), Y = C.S32();
		Vertices.Add(FVector2f(X / 65536.f, Y / 65536.f));
	}
	auto MapVertex = [&](uint32 V) -> int32
	{
		const int32 I = V < OrgVerts ? (int32)V : Base + (int32)(V - OrgVerts);
		return FMath::Clamp(I, 0, Vertices.Num() - 1);
	};

	const uint32 NumSs = C.U32();
	if (NumSs > (uint32)Len) return false;
	Subsectors.SetNum(NumSs);
	int32 First = 0;
	for (uint32 i = 0; i < NumSs && !C.bBad; ++i)
	{
		Subsectors[i].FirstSeg = First;
		Subsectors[i].NumSegs = C.U32();
		First += Subsectors[i].NumSegs;
	}

	const uint32 NumSegs = C.U32();
	if (NumSegs > (uint32)Len || (int32)NumSegs != First) return false;
	Segs.SetNum(NumSegs);
	for (uint32 i = 0; i < NumSegs && !C.bBad; ++i)
	{
		FDoomSeg& S = Segs[i];
		S.V1 = MapVertex(C.U32());
		uint32 LineIndex;
		if (bGl)
		{
			C.U32(); // partner seg
			LineIndex = bLongLines ? C.U32() : C.U16();
			if (!bLongLines && LineIndex == 0xFFFF) LineIndex = 0xFFFFFFFF;
		}
		else
		{
			S.V2 = MapVertex(C.U32());
			LineIndex = C.U16();
		}
		S.Side = C.U8() ? 1 : 0;
		S.Line = (LineIndex == 0xFFFFFFFF || !Lines.IsValidIndex((int32)LineIndex)) ? INDEX_NONE : (int32)LineIndex;
	}
	if (bGl)
	{
		// GL subsectors are closed polygons: a seg ends where the next one starts
		for (const FDoomSubsector& Ss : Subsectors)
		{
			for (int32 k = 0; k < Ss.NumSegs; ++k)
			{
				Segs[Ss.FirstSeg + k].V2 = Segs[Ss.FirstSeg + (k + 1) % Ss.NumSegs].V1;
			}
		}
	}

	const uint32 NumNodes = C.U32();
	if (NumNodes > (uint32)Len) return false;
	Nodes.SetNum(NumNodes);
	for (uint32 i = 0; i < NumNodes && !C.bBad; ++i)
	{
		FDoomNode& N = Nodes[i];
		if (bFixedNodes)
		{
			const int32 X = C.S32(), Y = C.S32(), DX = C.S32(), DY = C.S32();
			N.Origin = FVector2f(X / 65536.f, Y / 65536.f);
			N.Dir = FVector2f(DX / 65536.f, DY / 65536.f);
		}
		else
		{
			const int16 X = C.S16(), Y = C.S16(), DX = C.S16(), DY = C.S16();
			N.Origin = FVector2f(X, Y);
			N.Dir = FVector2f(DX, DY);
		}
		for (int32 b = 0; b < 8; ++b) C.S16(); // bounding boxes
		for (int32 c = 0; c < 2; ++c)
		{
			const uint32 Child = C.U32();
			N.Child[c] = (Child & 0x80000000u) ? (int32)((Child & 0x7FFFFFFF) | FDoomNode::SubsectorBit) : (int32)Child;
		}
	}
	if (C.bBad)
	{
		UE_LOG(LogDoomMap, Error, TEXT("%s: %s nodes are truncated"), *Name, *Magic);
		return false;
	}
	return true;
}

bool FDoomMap::LoadGlNodes(const FDoomWad& Wad, int32 Marker)
{
	const int32 Limit = Marker + 6;
	TConstArrayView<uint8> VertL = Wad.LumpData(Wad.FindLumpAfter(TEXT("GL_VERT"), Marker + 1, Limit));
	TConstArrayView<uint8> SegsL = Wad.LumpData(Wad.FindLumpAfter(TEXT("GL_SEGS"), Marker + 1, Limit));
	TConstArrayView<uint8> SsL = Wad.LumpData(Wad.FindLumpAfter(TEXT("GL_SSECT"), Marker + 1, Limit));
	TConstArrayView<uint8> NodesL = Wad.LumpData(Wad.FindLumpAfter(TEXT("GL_NODES"), Marker + 1, Limit));
	if (VertL.Num() == 0 || SegsL.Num() == 0 || SsL.Num() == 0)
	{
		return false;
	}
	auto Magic = [](TConstArrayView<uint8> L, const char* M) { return L.Num() >= 4 && FMemory::Memcmp(L.GetData(), M, 4) == 0; };
	// V1: 16-bit vertices; V2+: "gNdN" and 16.16 fixed vertices. V3 marks segs and subsectors with
	// "gNd3" and 32-bit fields; V4/V5 use 32-bit fields without the marker and 32-bit node children.
	int32 Version = 1;
	if (Magic(VertL, "gNd2")) Version = 2;
	if (Magic(VertL, "gNd3") || Magic(SegsL, "gNd3")) Version = 3;
	if (Magic(VertL, "gNd4") || Magic(VertL, "gNd5")) Version = 5;
	NodesFormat = FString::Printf(TEXT("glBSP v%d"), Version);

	const int32 Base = Vertices.Num();
	FCursor V{ VertL.GetData(), VertL.Num(), Version == 1 ? 0 : 4 };
	while (V.Pos + (Version == 1 ? 4 : 8) <= V.Num)
	{
		if (Version == 1)
		{
			const int16 X = V.S16(), Y = V.S16();
			Vertices.Add(FVector2f(X, Y));
		}
		else
		{
			const int32 X = V.S32(), Y = V.S32();
			Vertices.Add(FVector2f(X / 65536.f, Y / 65536.f));
		}
	}
	const uint32 GlFlag = Version <= 2 ? 0x8000u : Version == 3 ? 0x40000000u : 0x80000000u;
	auto MapVertex = [&](uint32 R) -> int32
	{
		const int32 I = (R & GlFlag) ? Base + (int32)(R & ~GlFlag) : (int32)R;
		return FMath::Clamp(I, 0, Vertices.Num() - 1);
	};

	FCursor S{ SegsL.GetData(), SegsL.Num(), Version == 3 ? 4 : 0 };
	const int32 SegSize = Version <= 2 ? 10 : 16;
	while (S.Pos + SegSize <= S.Num)
	{
		FDoomSeg& Seg = Segs.AddDefaulted_GetRef();
		const uint32 A = Version <= 2 ? S.U16() : S.U32();
		const uint32 B = Version <= 2 ? S.U16() : S.U32();
		Seg.V1 = MapVertex(A);
		Seg.V2 = MapVertex(B);
		const uint16 LineIndex = S.U16();
		Seg.Side = S.U16() ? 1 : 0;
		if (Version <= 2) S.U16(); else S.U32(); // partner
		Seg.Line = (LineIndex == 0xFFFF || !Lines.IsValidIndex(LineIndex)) ? INDEX_NONE : LineIndex;
	}

	FCursor Ss{ SsL.GetData(), SsL.Num(), Version == 3 ? 4 : 0 };
	const int32 SsSize = Version <= 2 ? 4 : 8;
	while (Ss.Pos + SsSize <= Ss.Num)
	{
		FDoomSubsector& Sub = Subsectors.AddDefaulted_GetRef();
		Sub.NumSegs = Version <= 2 ? Ss.U16() : Ss.S32();
		Sub.FirstSeg = Version <= 2 ? Ss.U16() : Ss.S32();
		if (Sub.FirstSeg < 0 || Sub.FirstSeg + Sub.NumSegs > Segs.Num())
		{
			return false;
		}
	}

	FCursor N{ NodesL.GetData(), NodesL.Num() };
	const int32 NodeSize = Version >= 5 ? 32 : 28;
	while (N.Pos + NodeSize <= N.Num)
	{
		FDoomNode& Node = Nodes.AddDefaulted_GetRef();
		const int16 X = N.S16(), Y = N.S16(), DX = N.S16(), DY = N.S16();
		Node.Origin = FVector2f(X, Y);
		Node.Dir = FVector2f(DX, DY);
		for (int32 b = 0; b < 8; ++b) N.S16();
		for (int32 c = 0; c < 2; ++c)
		{
			if (Version >= 5)
			{
				const uint32 Child = N.U32();
				Node.Child[c] = (Child & 0x80000000u) ? (int32)((Child & 0x7FFFFFFF) | FDoomNode::SubsectorBit) : (int32)Child;
			}
			else
			{
				const uint16 Child = N.U16();
				Node.Child[c] = (Child & 0x8000) ? ((Child & 0x7FFF) | FDoomNode::SubsectorBit) : Child;
			}
		}
	}
	return Subsectors.Num() > 0;
}

void FDoomMap::Derive()
{
	Bounds = FBox2f(ForceInit);
	for (const FVector2f& Vx : Vertices)
	{
		Bounds += Vx;
	}
	for (FDoomSector& S : Sectors)
	{
		S.Bounds = FBox2f(ForceInit);
	}
	SpecialLines.Reset();
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FDoomLine& L = Lines[i];
		L.V1 = FMath::Clamp(L.V1, 0, Vertices.Num() - 1);
		L.V2 = FMath::Clamp(L.V2, 0, Vertices.Num() - 1);
		L.Bounds = FBox2f(ForceInit);
		L.Bounds += V(L.V1);
		L.Bounds += V(L.V2);
		L.FrontSector = L.Side[0] != INDEX_NONE ? Sides[L.Side[0]].Sector : INDEX_NONE;
		L.BackSector = L.Side[1] != INDEX_NONE ? Sides[L.Side[1]].Sector : INDEX_NONE;
		for (int32 s = 0; s < 2; ++s)
		{
			if (L.Side[s] != INDEX_NONE)
			{
				Sides[L.Side[s]].Line = i;
			}
		}
		if (L.FrontSector != INDEX_NONE)
		{
			Sectors[L.FrontSector].Lines.AddUnique(i);
			Sectors[L.FrontSector].Bounds += L.Bounds;
		}
		if (L.BackSector != INDEX_NONE)
		{
			Sectors[L.BackSector].Lines.AddUnique(i);
			Sectors[L.BackSector].Bounds += L.Bounds;
		}
		if (L.Special != 0)
		{
			SpecialLines.Add(i);
		}
	}
	for (FDoomSector& S : Sectors)
	{
		S.Center = S.Bounds.bIsValid ? S.Bounds.GetCenter() : FVector2f::ZeroVector;
	}
	for (int32 i = 0; i < Subsectors.Num(); ++i)
	{
		if (Sectors.IsValidIndex(Subsectors[i].Sector))
		{
			Sectors[Subsectors[i].Sector].Subsectors.Add(i);
		}
	}
}

void FDoomMap::BuildSubsectorPolygons()
{
	const FBox2f B = Bounds.ExpandBy(64.f);
	TArray<FVector2f> Start = { FVector2f(B.Min.X, B.Min.Y), FVector2f(B.Max.X, B.Min.Y), FVector2f(B.Max.X, B.Max.Y), FVector2f(B.Min.X, B.Max.Y) };
	ClipToNode(Nodes.Num() == 0 ? FDoomNode::SubsectorBit : Nodes.Num() - 1, Start, 0);
}

void FDoomMap::ClipToNode(int32 NodeOrLeaf, TArray<FVector2f> Poly, int32 Depth)
{
	if (Depth > 512)
	{
		return;
	}
	if (NodeOrLeaf & FDoomNode::SubsectorBit)
	{
		const int32 Index = NodeOrLeaf & ~FDoomNode::SubsectorBit;
		if (!Subsectors.IsValidIndex(Index))
		{
			return;
		}
		FDoomSubsector& Ss = Subsectors[Index];
		for (int32 s = Ss.FirstSeg; s < Ss.FirstSeg + Ss.NumSegs && s < Segs.Num(); ++s)
		{
			const FVector2f A = V(Segs[s].V1);
			ClipPolygon(Poly, A, V(Segs[s].V2) - A, 1.f);
		}
		Ss.Polygon = MoveTemp(Poly);
		return;
	}
	if (!Nodes.IsValidIndex(NodeOrLeaf))
	{
		return;
	}
	const FDoomNode& N = Nodes[NodeOrLeaf];
	TArray<FVector2f> Front = Poly;
	ClipPolygon(Front, N.Origin, N.Dir, 1.f);
	ClipToNode(N.Child[0], MoveTemp(Front), Depth + 1);
	ClipPolygon(Poly, N.Origin, N.Dir, -1.f);
	ClipToNode(N.Child[1], MoveTemp(Poly), Depth + 1);
}

int32 FDoomMap::SubsectorAt(const FVector2f& P) const
{
	if (Nodes.Num() == 0)
	{
		return Subsectors.Num() > 0 ? 0 : INDEX_NONE;
	}
	int32 N = Nodes.Num() - 1;
	for (int32 Guard = 0; Guard < 4096; ++Guard)
	{
		if (N & FDoomNode::SubsectorBit)
		{
			const int32 Ss = N & ~FDoomNode::SubsectorBit;
			return Subsectors.IsValidIndex(Ss) ? Ss : INDEX_NONE;
		}
		if (!Nodes.IsValidIndex(N))
		{
			return INDEX_NONE;
		}
		const FDoomNode& Node = Nodes[N];
		const int32 Side = MapCross(Node.Dir, P - Node.Origin) < 0 ? 0 : 1;
		N = Node.Child[Side];
	}
	return INDEX_NONE;
}

int32 FDoomMap::PointOnLineSide(const FVector2f& P, int32 Line) const
{
	const FDoomLine& L = Lines[Line];
	const FVector2f A = V(L.V1);
	return MapCross(V(L.V2) - A, P - A) < 0 ? 0 : 1;
}

int32 FDoomMap::OtherSector(int32 Line, int32 Sector) const
{
	const FDoomLine& L = Lines[Line];
	if (L.FrontSector == INDEX_NONE || L.BackSector == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	return L.FrontSector == Sector ? L.BackSector : L.FrontSector;
}

float FDoomMap::LowestFloorSurrounding(int32 Sector) const
{
	float H = Sectors[Sector].Floor;
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE) H = FMath::Min(H, Sectors[O].Floor);
	}
	return H;
}

float FDoomMap::HighestFloorSurrounding(int32 Sector) const
{
	float H = -500.f;
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE) H = FMath::Max(H, Sectors[O].Floor);
	}
	return H;
}

float FDoomMap::NextHighestFloor(int32 Sector, float Current) const
{
	float Best = TNumericLimits<float>::Max();
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE && Sectors[O].Floor > Current) Best = FMath::Min(Best, Sectors[O].Floor);
	}
	return Best == TNumericLimits<float>::Max() ? Current : Best;
}

float FDoomMap::LowestCeilingSurrounding(int32 Sector) const
{
	float H = 32000.f;
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE) H = FMath::Min(H, Sectors[O].Ceiling);
	}
	return H;
}

float FDoomMap::HighestCeilingSurrounding(int32 Sector) const
{
	float H = 0.f;
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE) H = FMath::Max(H, Sectors[O].Ceiling);
	}
	return H;
}

int32 FDoomMap::MinSurroundingLight(int32 Sector, int32 Max) const
{
	int32 Min = Max;
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE) Min = FMath::Min<int32>(Min, Sectors[O].Light);
	}
	return Min;
}

int32 FDoomMap::MaxSurroundingLight(int32 Sector, int32 Min) const
{
	int32 Max = Min;
	for (int32 L : Sectors[Sector].Lines)
	{
		const int32 O = OtherSector(L, Sector);
		if (O != INDEX_NONE) Max = FMath::Max<int32>(Max, Sectors[O].Light);
	}
	return Max;
}

TArray<int32> FDoomMap::SectorsWithTag(int32 Tag) const
{
	TArray<int32> Out;
	for (int32 i = 0; i < Sectors.Num(); ++i)
	{
		if (Sectors[i].Tag == Tag) Out.Add(i);
	}
	return Out;
}

bool FDoomMap::LineOpening(int32 Line, float& OutTop, float& OutBottom) const
{
	const FDoomLine& L = Lines[Line];
	if (L.FrontSector == INDEX_NONE || L.BackSector == INDEX_NONE)
	{
		OutTop = OutBottom = 0;
		return false;
	}
	const FDoomSector& F = Sectors[L.FrontSector];
	const FDoomSector& B = Sectors[L.BackSector];
	OutTop = FMath::Min(F.Ceiling, B.Ceiling);
	OutBottom = FMath::Max(F.Floor, B.Floor);
	return OutTop > OutBottom;
}

bool FDoomMap::SegmentCrossesLine(const FVector2f& A, const FVector2f& B, int32 Line, int32& OutSideA) const
{
	const FDoomLine& L = Lines[Line];
	const FVector2f P1 = V(L.V1), P2 = V(L.V2);
	const FVector2f D = P2 - P1;
	const float SA = MapCross(D, A - P1);
	const float SB = MapCross(D, B - P1);
	if ((SA < 0) == (SB < 0))
	{
		return false;
	}
	const FVector2f T = B - A;
	const float S1 = MapCross(T, P1 - A);
	const float S2 = MapCross(T, P2 - A);
	if ((S1 < 0) == (S2 < 0) && S1 != 0 && S2 != 0)
	{
		return false;
	}
	OutSideA = SA < 0 ? 0 : 1;
	return true;
}

float FDoomMap::DistanceToLine(const FVector2f& P, int32 Line) const
{
	const FDoomLine& L = Lines[Line];
	const FVector2f A = V(L.V1), B = V(L.V2);
	const FVector2f AB = B - A;
	const float Len2 = AB.SizeSquared();
	const float T = Len2 > 0 ? FMath::Clamp(FVector2f::DotProduct(P - A, AB) / Len2, 0.f, 1.f) : 0.f;
	return FVector2f::Distance(P, A + AB * T);
}

void FDoomMap::TraceLines(const FVector2f& A, const FVector2f& B, TArray<FLineHit>& OutHits) const
{
	OutHits.Reset();
	FBox2f TB(ForceInit);
	TB += A;
	TB += B;
	TB = TB.ExpandBy(1.f);
	const FVector2f T = B - A;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FDoomLine& L = Lines[i];
		if (!L.Bounds.ExpandBy(1.f).Intersect(TB))
		{
			continue;
		}
		int32 Side;
		if (!SegmentCrossesLine(A, B, i, Side))
		{
			continue;
		}
		const FVector2f P1 = V(L.V1);
		const FVector2f D = V(L.V2) - P1;
		const float Den = MapCross(T, D);
		if (FMath::Abs(Den) < 1e-6f)
		{
			continue;
		}
		FLineHit H;
		H.Line = i;
		H.Frac = MapCross(P1 - A, D) / Den;
		H.Side = Side;
		OutHits.Add(H);
	}
	OutHits.Sort([](const FLineHit& X, const FLineHit& Y) { return X.Frac < Y.Frac; });
}

double FDoomMap::SubsectorArea() const
{
	double Area = 0;
	for (const FDoomSubsector& Ss : Subsectors)
	{
		for (int32 k = 1; k + 1 < Ss.Polygon.Num(); ++k)
		{
			const FVector2f A = Ss.Polygon[k] - Ss.Polygon[0], B = Ss.Polygon[k + 1] - Ss.Polygon[0];
			Area += 0.5 * (A.X * B.Y - A.Y * B.X);
		}
	}
	return Area;
}
