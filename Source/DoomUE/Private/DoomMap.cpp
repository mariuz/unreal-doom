#include "DoomMap.h"
#include "DoomWad.h"
#include "DoomUE.h"

using namespace WadRead;

namespace
{
	enum EMapLump { ML_THINGS = 1, ML_LINEDEFS, ML_SIDEDEFS, ML_VERTEXES, ML_SEGS, ML_SSECTORS, ML_NODES, ML_SECTORS };

	FName TexName(const uint8* P)
	{
		const FString S = Name8(P);
		return (S.IsEmpty() || S == TEXT("-")) ? NAME_None : FName(*S);
	}

	float Cross(const FVector2f& A, const FVector2f& B) { return A.X * B.Y - A.Y * B.X; }

	/** Keeps the part of Poly where Cross(Dir, P - Origin) * Sign <= 0 (Sign 1: the right side). */
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
			const float DA = Cross(N, A - Origin) * Sign;
			const float DB = Cross(N, B - Origin) * Sign;
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
		UE_LOG(LogDoom, Error, TEXT("Map %s not found"), *Name);
		return false;
	}
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
		S.FloorPic = TexName(P + 4);
		S.CeilingPic = TexName(P + 12);
		S.Light = S.OriginalLight = S16(P + 20);
		S.Special = S16(P + 22);
		S.Tag = S16(P + 24);
	}
	if (Sectors.Num() == 0)
	{
		UE_LOG(LogDoom, Error, TEXT("%s has no sectors"), *Name);
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
		S.Upper = TexName(P + 4);
		S.Lower = TexName(P + 12);
		S.Middle = TexName(P + 20);
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

	if (!LoadVanillaNodes(Get(ML_NODES, TEXT("NODES")), Get(ML_SEGS, TEXT("SEGS")), Get(ML_SSECTORS, TEXT("SSECTORS"))))
	{
		UE_LOG(LogDoom, Error, TEXT("%s: unsupported or missing BSP (extended/GL nodes are on the roadmap)"), *Name);
		return false;
	}
	Derive();
	BuildSubsectorPolygons();

	double Area = 0;
	for (const FDoomSubsector& Ss : Subsectors)
	{
		for (int32 k = 1; k + 1 < Ss.Polygon.Num(); ++k)
		{
			Area += 0.5 * Cross(Ss.Polygon[k] - Ss.Polygon[0], Ss.Polygon[k + 1] - Ss.Polygon[0]);
		}
	}
	UE_LOG(LogDoom, Log, TEXT("Loaded %s: %d vertices, %d lines, %d sectors, %d subsectors (area %.0f), %d things"),
		*Name, Vertices.Num(), Lines.Num(), Sectors.Num(), Subsectors.Num(), Area, Things.Num());
	return true;
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
		const int32 Side = Cross(Node.Dir, P - Node.Origin) < 0 ? 0 : 1;
		N = Node.Child[Side];
	}
	return INDEX_NONE;
}

int32 FDoomMap::PointOnLineSide(const FVector2f& P, int32 Line) const
{
	const FDoomLine& L = Lines[Line];
	const FVector2f A = V(L.V1);
	return Cross(V(L.V2) - A, P - A) < 0 ? 0 : 1;
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
	const float SA = Cross(D, A - P1);
	const float SB = Cross(D, B - P1);
	if ((SA < 0) == (SB < 0))
	{
		return false;
	}
	const FVector2f T = B - A;
	const float S1 = Cross(T, P1 - A);
	const float S2 = Cross(T, P2 - A);
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
		const float Den = Cross(T, D);
		if (FMath::Abs(Den) < 1e-6f)
		{
			continue;
		}
		FLineHit H;
		H.Line = i;
		H.Frac = Cross(P1 - A, D) / Den;
		H.Side = Side;
		OutHits.Add(H);
	}
	OutHits.Sort([](const FLineHit& X, const FLineHit& Y) { return X.Frac < Y.Frac; });
}
