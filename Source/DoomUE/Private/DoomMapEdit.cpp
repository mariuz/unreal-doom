// Map editing operations (see DoomMapEdit.h).
#include "DoomMapEdit.h"
#include "DoomUE.h"
#include "Algo/Reverse.h"

namespace
{
	constexpr float OnTol = 0.5f; // map units: a point this close to a vertex or line is on it

	double Cross2(const FVector2D& A, const FVector2D& B) { return A.X * B.Y - A.Y * B.X; }

	/** The segments cross at a point inside both (touching ends and overlapping don't count). */
	bool CrossProper(const FVector2f& A, const FVector2f& B, const FVector2f& C, const FVector2f& D)
	{
		const FVector2D a(A), b(B), c(C), d(D);
		const double D1 = Cross2(b - a, c - a), D2 = Cross2(b - a, d - a);
		const double D3 = Cross2(d - c, a - c), D4 = Cross2(d - c, b - c);
		const double Eps = 1e-3 * FMath::Max(1.0, (b - a).Size() * (d - c).Size());
		return ((D1 > Eps && D2 < -Eps) || (D1 < -Eps && D2 > Eps)) && ((D3 > Eps && D4 < -Eps) || (D3 < -Eps && D4 > Eps));
	}

	float DistToSegment(const FVector2f& P, const FVector2f& A, const FVector2f& B, float* OutT = nullptr)
	{
		const FVector2f D = B - A;
		const float Len2 = D.SizeSquared();
		const float T = Len2 > 0 ? FMath::Clamp(FVector2f::DotProduct(P - A, D) / Len2, 0.f, 1.f) : 0.f;
		if (OutT) *OutT = T;
		return FVector2f::Distance(P, A + D * T);
	}

	bool InsidePolygon(const TArray<FVector2f>& Poly, const FVector2f& P)
	{
		bool bIn = false;
		for (int32 i = 0, j = Poly.Num() - 1; i < Poly.Num(); j = i++)
		{
			if ((Poly[i].Y > P.Y) != (Poly[j].Y > P.Y) && P.X < (Poly[j].X - Poly[i].X) * (P.Y - Poly[i].Y) / (Poly[j].Y - Poly[i].Y) + Poly[i].X)
			{
				bIn = !bIn;
			}
		}
		return bIn;
	}

	bool OnBoundary(const TArray<FVector2f>& Poly, const FVector2f& P)
	{
		for (int32 i = 0; i < Poly.Num(); ++i)
		{
			if (DistToSegment(P, Poly[i], Poly[(i + 1) % Poly.Num()]) < OnTol) return true;
		}
		return false;
	}

	/** A one-sided wall again: the middle shows what the upper or lower did. */
	void MakeOneSided(FDoomMap& Map, FDoomLine& L)
	{
		L.Side[1] = INDEX_NONE;
		L.Flags = (L.Flags | DoomLineFlags::Blocking) & ~DoomLineFlags::TwoSided;
		if (L.Side[0] != INDEX_NONE)
		{
			FDoomSide& S = Map.Sides[L.Side[0]];
			if (S.Middle.IsNone()) S.Middle = !S.Upper.IsNone() ? S.Upper : S.Lower;
		}
	}
}

FVector2f DoomMapEdit::Snap(const FVector2f& P, float Grid)
{
	if (Grid <= 0) return P;
	return FVector2f(FMath::RoundToFloat(P.X / Grid) * Grid, FMath::RoundToFloat(P.Y / Grid) * Grid);
}

int32 DoomMapEdit::VertexNear(const FDoomMap& Map, const FVector2f& P, float Radius)
{
	int32 Best = INDEX_NONE;
	float BestD = Radius;
	// only vertices lines use (the node builder's splits are not editable)
	TBitArray<> Used(false, Map.Vertices.Num());
	for (const FDoomLine& L : Map.Lines)
	{
		if (Map.Vertices.IsValidIndex(L.V1)) Used[L.V1] = true;
		if (Map.Vertices.IsValidIndex(L.V2)) Used[L.V2] = true;
	}
	for (int32 v = 0; v < Map.Vertices.Num(); ++v)
	{
		const float D = FVector2f::Distance(P, Map.Vertices[v]);
		if (Used[v] && D <= BestD) { BestD = D; Best = v; }
	}
	return Best;
}

int32 DoomMapEdit::LineNear(const FDoomMap& Map, const FVector2f& P, float Radius)
{
	int32 Best = INDEX_NONE;
	float BestD = Radius;
	for (int32 l = 0; l < Map.Lines.Num(); ++l)
	{
		const float D = DistToSegment(P, Map.V(Map.Lines[l].V1), Map.V(Map.Lines[l].V2));
		if (D <= BestD) { BestD = D; Best = l; }
	}
	return Best;
}

int32 DoomMapEdit::ThingNear(const FDoomMap& Map, const FVector2f& P, float Radius)
{
	int32 Best = INDEX_NONE;
	float BestD = Radius;
	for (int32 t = 0; t < Map.Things.Num(); ++t)
	{
		const float D = FVector2f::Distance(P, Map.Things[t].P);
		if (D <= BestD) { BestD = D; Best = t; }
	}
	return Best;
}

int32 DoomMapEdit::SectorInside(const FDoomMap& Map, const FVector2f& P)
{
	const int32 Ss = Map.SubsectorAt(P);
	if (!Map.Subsectors.IsValidIndex(Ss))
	{
		return INDEX_NONE;
	}
	const TArray<FVector2f>& Poly = Map.Subsectors[Ss].Polygon;
	if (Poly.Num() < 3 || (!InsidePolygon(Poly, P) && !OnBoundary(Poly, P)))
	{
		return INDEX_NONE;
	}
	return Map.Subsectors[Ss].Sector;
}

int32 DoomMapEdit::SplitLine(FDoomMap& Map, int32 Line, const FVector2f& P)
{
	const int32 NV = Map.Vertices.Add(P);
	FDoomLine NewLine = Map.Lines[Line];
	FDoomLine& Old = Map.Lines[Line];
	const float FirstLen = FVector2f::Distance(Map.V(Old.V1), P);
	const float SecondLen = FVector2f::Distance(P, Map.V(Old.V2));
	NewLine.V1 = NV;
	Old.V2 = NV;
	// the front's texture runs V1 -> V2, the back's V2 -> V1: each piece starts where it starts
	if (Old.Side[0] != INDEX_NONE)
	{
		FDoomSide S = Map.Sides[Old.Side[0]];
		S.XOffset += FirstLen;
		NewLine.Side[0] = Map.Sides.Add(S);
	}
	if (Old.Side[1] != INDEX_NONE)
	{
		FDoomSide S = Map.Sides[Old.Side[1]];
		NewLine.Side[1] = Map.Sides.Add(S);
		Map.Sides[Map.Lines[Line].Side[1]].XOffset += SecondLen;
	}
	Map.Lines.Add(NewLine);
	return NV;
}

int32 DoomMapEdit::DrawSector(FDoomMap& Map, TArray<FVector2f> Points, const FDoomSector& Template, FName Wall, FString& OutError)
{
	// tidy the outline: no repeated or closing points
	for (int32 i = Points.Num() - 1; i >= 0 && Points.Num() > 0; --i)
	{
		if (Points.Num() > 1 && Points[i].Equals(Points[(i + 1) % Points.Num()], OnTol)) Points.RemoveAt(i);
	}
	if (Points.Num() < 3)
	{
		OutError = TEXT("a sector needs at least three corners");
		return INDEX_NONE;
	}
	// the node builder's split vertices go (the BSP is rebuilt after an edit anyway); on a failure
	// the map is put back as it was
	{
		int32 MaxV = -1;
		for (const FDoomLine& L : Map.Lines) MaxV = FMath::Max3(MaxV, L.V1, L.V2);
		if (Map.Vertices.Num() > MaxV + 1) Map.Vertices.SetNum(MaxV + 1);
	}
	const FSnapshot Before = Take(Map);
	// clockwise (Doom's y points up): the inside is on each edge's right, its lines' front
	double Area2 = 0;
	for (int32 i = 0; i < Points.Num(); ++i) Area2 += Cross2(FVector2D(Points[i]), FVector2D(Points[(i + 1) % Points.Num()]));
	if (FMath::Abs(Area2) < 1.0)
	{
		OutError = TEXT("the outline has no area");
		return INDEX_NONE;
	}
	if (Area2 > 0)
	{
		Algo::Reverse(Points);
	}
	const int32 N = Points.Num();
	for (int32 i = 0; i < N; ++i)
	{
		for (int32 j = i + 2; j < N; ++j)
		{
			if (i == 0 && j == N - 1) continue;
			if (CrossProper(Points[i], Points[(i + 1) % N], Points[j], Points[(j + 1) % N]))
			{
				OutError = TEXT("the outline crosses itself");
				return INDEX_NONE;
			}
		}
	}
	// nothing of the map may cross it or lie inside it
	for (int32 l = 0; l < Map.Lines.Num(); ++l)
	{
		const FVector2f A = Map.V(Map.Lines[l].V1), B = Map.V(Map.Lines[l].V2);
		for (int32 i = 0; i < N; ++i)
		{
			if (CrossProper(Points[i], Points[(i + 1) % N], A, B))
			{
				OutError = FString::Printf(TEXT("the outline crosses line %d"), l);
				return INDEX_NONE;
			}
		}
		const FVector2f Mid = (A + B) * 0.5f;
		const bool bAIn = InsidePolygon(Points, A) || OnBoundary(Points, A);
		const bool bBIn = InsidePolygon(Points, B) || OnBoundary(Points, B);
		if ((bAIn && !OnBoundary(Points, A)) || (bBIn && !OnBoundary(Points, B)) || (bAIn && bBIn && InsidePolygon(Points, Mid) && !OnBoundary(Points, Mid)))
		{
			OutError = FString::Printf(TEXT("the outline would enclose line %d (draw around it, or inside it)"), l);
			return INDEX_NONE;
		}
	}
	// what it is drawn in: a point just inside one of its edges
	int32 Container = INDEX_NONE;
	for (int32 i = 0; i < N; ++i)
	{
		const FVector2f A = Points[i], B = Points[(i + 1) % N];
		const FVector2f D = (B - A).GetSafeNormal();
		const FVector2f In = (A + B) * 0.5f + FVector2f(D.Y, -D.X) * 1.f; // right of the edge
		if (InsidePolygon(Points, In))
		{
			Container = SectorInside(Map, In);
			break;
		}
	}

	FDoomSector NewS = Template;
	NewS.Lines.Reset();
	NewS.Subsectors.Reset();
	NewS.OriginalLight = NewS.Light;
	const int32 New = Map.Sectors.Add(NewS);

	// the corners: existing vertices, or new ones (splitting a line they land on)
	auto VertexFor = [&](const FVector2f& P)
	{
		for (int32 v = 0; v < Map.Vertices.Num(); ++v)
		{
			if (Map.Vertices[v].Equals(P, OnTol)) return v;
		}
		for (int32 l = 0; l < Map.Lines.Num(); ++l)
		{
			float T;
			if (DistToSegment(P, Map.V(Map.Lines[l].V1), Map.V(Map.Lines[l].V2), &T) < OnTol && T > 0.f && T < 1.f) return SplitLine(Map, l, P);
		}
		return Map.Vertices.Add(P);
	};
	TArray<int32> Corners;
	for (const FVector2f& P : Points) Corners.Add(VertexFor(P));
	// existing vertices along each edge split it
	TArray<int32> Ring;
	for (int32 i = 0; i < N; ++i)
	{
		const int32 U = Corners[i], V = Corners[(i + 1) % N];
		const FVector2f A = Map.Vertices[U], B = Map.Vertices[V];
		TArray<TPair<float, int32>> Between;
		TBitArray<> Used(false, Map.Vertices.Num());
		for (const FDoomLine& L : Map.Lines) { Used[L.V1] = true; Used[L.V2] = true; }
		for (int32 v = 0; v < Map.Vertices.Num(); ++v)
		{
			if (v == U || v == V || !Used[v]) continue;
			float T;
			if (DistToSegment(Map.Vertices[v], A, B, &T) < OnTol && T > 0.f && T < 1.f) Between.Add({ T, v });
		}
		Between.Sort([](const TPair<float, int32>& X, const TPair<float, int32>& Y) { return X.Key < Y.Key; });
		Ring.Add(U);
		for (const TPair<float, int32>& B2 : Between) Ring.Add(B2.Value);
	}
	auto NewSide = [&](int32 Sector, bool bTwoSided)
	{
		FDoomSide S;
		S.Sector = Sector;
		if (bTwoSided) { S.Upper = Wall; S.Lower = Wall; }
		else S.Middle = Wall;
		return Map.Sides.Add(S);
	};
	for (int32 i = 0; i < Ring.Num(); ++i)
	{
		const int32 U = Ring[i], V = Ring[(i + 1) % Ring.Num()];
		if (U == V) continue;
		// an existing line along this piece?
		int32 Existing = INDEX_NONE;
		for (int32 l = 0; l < Map.Lines.Num() && Existing == INDEX_NONE; ++l)
		{
			const FDoomLine& L = Map.Lines[l];
			if ((L.V1 == U && L.V2 == V) || (L.V1 == V && L.V2 == U)) Existing = l;
		}
		if (Existing == INDEX_NONE)
		{
			FDoomLine L;
			L.V1 = U;
			L.V2 = V;
			L.Side[0] = NewSide(New, Container != INDEX_NONE);
			if (Container != INDEX_NONE)
			{
				L.Side[1] = NewSide(Container, true);
				L.Flags = DoomLineFlags::TwoSided;
			}
			else
			{
				L.Flags = DoomLineFlags::Blocking;
			}
			Map.Lines.Add(L);
			continue;
		}
		FDoomLine& L = Map.Lines[Existing];
		const int32 OurSide = L.V1 == U ? 0 : 1; // our inside is on the line's front when it runs our way
		const int32 SideIndex = L.Side[OurSide];
		const int32 SideSector = SideIndex != INDEX_NONE ? Map.Sides[SideIndex].Sector : INDEX_NONE;
		if (SideSector != Container)
		{
			OutError = FString::Printf(TEXT("the outline runs along line %d, which already has sector %d on that side"), Existing, SideSector);
			Restore(Map, Before);
			return INDEX_NONE;
		}
		if (SideIndex != INDEX_NONE)
		{
			// a wall of the sector it is drawn in now belongs to the new one
			FDoomSide S = Map.Sides[SideIndex];
			S.Sector = New;
			L.Side[OurSide] = Map.Sides.Add(S);
		}
		else
		{
			// a one-sided wall facing away: a doorway between the old sector and the new
			const int32 Other = L.Side[1 - OurSide];
			FDoomSide& O = Map.Sides[Other];
			if (O.Upper.IsNone()) O.Upper = O.Middle;
			if (O.Lower.IsNone()) O.Lower = O.Middle;
			O.Middle = NAME_None;
			L.Side[OurSide] = NewSide(New, true);
			L.Flags = (L.Flags & ~DoomLineFlags::Blocking) | DoomLineFlags::TwoSided;
		}
	}
	return New;
}

void DoomMapEdit::DeleteSector(FDoomMap& Map, int32 Sector)
{
	if (!Map.Sectors.IsValidIndex(Sector))
	{
		return;
	}
	// its things go with it
	for (int32 t = Map.Things.Num() - 1; t >= 0; --t)
	{
		if (SectorInside(Map, Map.Things[t].P) == Sector) Map.Things.RemoveAt(t);
	}
	auto SectorOf = [&](int32 Side) { return Side != INDEX_NONE ? Map.Sides[Side].Sector : INDEX_NONE; };
	for (int32 l = Map.Lines.Num() - 1; l >= 0; --l)
	{
		FDoomLine& L = Map.Lines[l];
		const int32 F = SectorOf(L.Side[0]), B = SectorOf(L.Side[1]);
		if ((F == Sector || F == INDEX_NONE) && (B == Sector || B == INDEX_NONE))
		{
			Map.Lines.RemoveAt(l);
		}
		else if (F == Sector)
		{
			// the other sector keeps it as a wall facing it
			Swap(L.V1, L.V2);
			L.Side[0] = L.Side[1];
			MakeOneSided(Map, L);
		}
		else if (B == Sector)
		{
			MakeOneSided(Map, L);
		}
	}
	// the sector itself: no side refers to it any more
	Compact(Map);
}

void DoomMapEdit::Compact(FDoomMap& Map)
{
	TArray<int32> VMap, SMap, SecMap;
	VMap.Init(INDEX_NONE, Map.Vertices.Num());
	SMap.Init(INDEX_NONE, Map.Sides.Num());
	SecMap.Init(INDEX_NONE, Map.Sectors.Num());
	TArray<FVector2f> Vertices;
	TArray<FDoomSide> Sides;
	TArray<FDoomSector> Sectors;
	for (FDoomLine& L : Map.Lines)
	{
		for (int32* V : { &L.V1, &L.V2 })
		{
			if (VMap[*V] == INDEX_NONE) VMap[*V] = Vertices.Add(Map.Vertices[*V]);
			*V = VMap[*V];
		}
		for (int32& S : L.Side)
		{
			if (S == INDEX_NONE) continue;
			if (SMap[S] == INDEX_NONE)
			{
				FDoomSide Side = Map.Sides[S];
				if (SecMap[Side.Sector] == INDEX_NONE)
				{
					FDoomSector Sec = Map.Sectors[Side.Sector];
					Sec.Lines.Reset();
					Sec.Subsectors.Reset();
					SecMap[Side.Sector] = Sectors.Add(Sec);
				}
				Side.Sector = SecMap[Side.Sector];
				SMap[S] = Sides.Add(Side);
			}
			S = SMap[S];
		}
	}
	Map.Vertices = MoveTemp(Vertices);
	Map.Sides = MoveTemp(Sides);
	Map.Sectors = MoveTemp(Sectors);
}

void DoomMapEdit::MakeStarterMap(FDoomMap& Map, const FString& Name, FName Wall, FName Floor, FName Ceiling)
{
	Map = FDoomMap();
	Map.Name = Name;
	FDoomSector S;
	S.Floor = 0;
	S.Ceiling = 128;
	S.FloorPic = Floor;
	S.CeilingPic = Ceiling;
	S.Light = S.OriginalLight = 192;
	FString Error;
	DrawSector(Map, { {0, 0}, {512, 0}, {512, 512}, {0, 512} }, S, Wall, Error);
	FDoomMapThing Start;
	Start.P = FVector2f(256, 128);
	Start.Angle = 90;
	Start.Type = 1;
	Start.Flags = 7;
	Map.Things.Add(Start);
	Map.Rebuild();
}

DoomMapEdit::FSnapshot DoomMapEdit::Take(const FDoomMap& Map)
{
	FSnapshot S;
	S.Vertices = Map.Vertices;
	S.Lines = Map.Lines;
	S.Sides = Map.Sides;
	S.Sectors = Map.Sectors;
	S.Things = Map.Things;
	return S;
}

void DoomMapEdit::Restore(FDoomMap& Map, const FSnapshot& S)
{
	Map.Vertices = S.Vertices;
	Map.Lines = S.Lines;
	Map.Sides = S.Sides;
	Map.Sectors = S.Sectors;
	Map.Things = S.Things;
	Map.Rebuild();
}
