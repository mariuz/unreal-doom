// The node builder: Doom's BSP made from the lines and sectors alone, for maps the generator makes
// and the editors reshape. Vanilla style (like idbsp and BSP 5.x): segs are the lines' sides, cut
// where a partition crosses them; a set of segs that is convex (every seg's line has the others on its
// front, right-hand side) becomes a subsector; otherwise the seg line that splits fewest segs and
// balances best becomes a node's partition. No minisegs: FDoomMap::BuildSubsectorPolygons clips the
// subsector polygons from the node partitions and the segs, as it does for vanilla nodes.
#include "DoomMap.h"
#include "DoomUE.h"

namespace
{
	struct FBSeg
	{
		int32 V1 = 0, V2 = 0;
		int32 Line = INDEX_NONE;
		int32 Side = 0;
		float Offset = 0;
	};

	constexpr double OnEps = 1.0 / 64.0; // map units: closer than this to a partition counts as on it

	struct FBuilder
	{
		TArray<FVector2f>& Vertices;
		TArray<FDoomSeg>& Segs;
		TArray<FDoomSubsector>& Subsectors;
		TArray<FDoomNode>& Nodes;
		int32 Splits = 0;

		FVector2d P(int32 V) const { return FVector2d(Vertices[V]); }

		/** Signed distance of point Q from the line through A in direction D: < 0 front (right), > 0 back. */
		static double Side(const FVector2d& A, const FVector2d& D, const FVector2d& Q)
		{
			const double Len = D.Size();
			return Len > 0 ? (D.X * (Q.Y - A.Y) - D.Y * (Q.X - A.X)) / Len : 0;
		}

		/** -1 entirely front (or on it, facing the same way), 1 entirely back, 0 crosses. */
		int32 Classify(const FBSeg& Part, const FBSeg& S, double& OutDA, double& OutDB) const
		{
			const FVector2d A = P(Part.V1), D = P(Part.V2) - A;
			OutDA = Side(A, D, P(S.V1));
			OutDB = Side(A, D, P(S.V2));
			const bool bOnA = FMath::Abs(OutDA) <= OnEps, bOnB = FMath::Abs(OutDB) <= OnEps;
			if (bOnA && bOnB)
			{
				// along the partition: the side it faces decides
				return FVector2d::DotProduct(P(S.V2) - P(S.V1), D) > 0 ? -1 : 1;
			}
			if ((bOnA || OutDA < 0) && (bOnB || OutDB < 0)) return -1;
			if ((bOnA || OutDA > 0) && (bOnB || OutDB > 0)) return 1;
			return 0;
		}

		/** Every seg has all the others on its front side: a subsector. */
		bool IsConvex(const TArray<FBSeg>& Set) const
		{
			for (const FBSeg& A : Set)
			{
				for (const FBSeg& B : Set)
				{
					if (&A == &B) continue;
					double DA, DB;
					if (Classify(A, B, DA, DB) != -1) return false;
				}
			}
			return true;
		}

		/** Cost of a partition: splits weigh most, then imbalance; INT32_MAX if it divides nothing. */
		int64 Cost(const FBSeg& Part, const TArray<FBSeg>& Set) const
		{
			int32 Front = 0, Back = 0, Cuts = 0;
			for (const FBSeg& S : Set)
			{
				double DA, DB;
				switch (Classify(Part, S, DA, DB))
				{
				case -1: ++Front; break;
				case 1: ++Back; break;
				default: ++Cuts; ++Front; ++Back; break;
				}
			}
			if (Front == 0 || Back == 0)
			{
				return MAX_int64;
			}
			return (int64)Cuts * 16 + FMath::Abs(Front - Back);
		}

		/** Returns the child index (a node, or a subsector with SubsectorBit). */
		int32 Build(TArray<FBSeg> Set, int32 Depth)
		{
			if (Depth > 2000 || IsConvex(Set))
			{
				FDoomSubsector Ss;
				Ss.FirstSeg = Segs.Num();
				Ss.NumSegs = Set.Num();
				for (const FBSeg& S : Set)
				{
					FDoomSeg& Out = Segs.AddDefaulted_GetRef();
					Out.V1 = S.V1; Out.V2 = S.V2; Out.Line = S.Line; Out.Side = S.Side; Out.Offset = S.Offset;
				}
				return Subsectors.Add(MoveTemp(Ss)) | FDoomNode::SubsectorBit;
			}
			// the best of (up to) 96 candidate partitions spread through the set, all of them if none works
			const int32 Step = FMath::Max(1, Set.Num() / 96);
			int32 Best = INDEX_NONE;
			int64 BestCost = MAX_int64;
			for (int32 Pass = 0; Pass < 2 && Best == INDEX_NONE; ++Pass)
			{
				for (int32 i = 0; i < Set.Num(); i += (Pass == 0 ? Step : 1))
				{
					const int64 C = Cost(Set[i], Set);
					if (C < BestCost) { BestCost = C; Best = i; }
				}
			}
			if (Best == INDEX_NONE)
			{
				// nothing divides the set (overlapping segs): accept it as it is
				return Build(MoveTemp(Set), 100000);
			}
			const FBSeg Part = Set[Best];
			const FVector2d A = P(Part.V1), D = P(Part.V2) - A;
			TArray<FBSeg> Front, Back;
			for (const FBSeg& S : Set)
			{
				double DA, DB;
				const int32 C = Classify(Part, S, DA, DB);
				if (C == -1) { Front.Add(S); continue; }
				if (C == 1) { Back.Add(S); continue; }
				// cut at the crossing: a new vertex, the second piece's texture offset moves along
				const double T = DA / (DA - DB);
				const FVector2d Q = P(S.V1) + (P(S.V2) - P(S.V1)) * T;
				const int32 NewV = Vertices.Add(FVector2f(Q));
				++Splits;
				FBSeg First = S, Second = S;
				First.V2 = NewV;
				Second.V1 = NewV;
				Second.Offset = S.Offset + (float)(Q - P(S.V1)).Size();
				(DA < 0 ? Front : Back).Add(First);
				(DB < 0 ? Front : Back).Add(Second);
			}
			const int32 FrontChild = Build(MoveTemp(Front), Depth + 1);
			const int32 BackChild = Build(MoveTemp(Back), Depth + 1);
			FDoomNode& N = Nodes.AddDefaulted_GetRef();
			N.Origin = FVector2f(A);
			N.Dir = FVector2f(D);
			N.Child[0] = FrontChild;
			N.Child[1] = BackChild;
			return Nodes.Num() - 1;
		}
	};
}

bool FDoomMap::BuildNodes()
{
	Segs.Reset();
	Subsectors.Reset();
	Nodes.Reset();
	TArray<FBSeg> All;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FDoomLine& L = Lines[i];
		if (L.V1 == L.V2 || !Vertices.IsValidIndex(L.V1) || !Vertices.IsValidIndex(L.V2) || Vertices[L.V1].Equals(Vertices[L.V2], 0.01f))
		{
			continue; // zero length
		}
		if (L.Side[0] != INDEX_NONE) All.Add({ L.V1, L.V2, i, 0, 0.f });
		if (L.Side[1] != INDEX_NONE) All.Add({ L.V2, L.V1, i, 1, 0.f });
	}
	if (All.Num() == 0)
	{
		return false;
	}
	FBuilder B{ Vertices, Segs, Subsectors, Nodes };
	const int32 Root = B.Build(MoveTemp(All), 0);
	if (Nodes.Num() == 0)
	{
		// the whole map is one convex subsector: SubsectorAt handles no nodes
		check(Root & FDoomNode::SubsectorBit);
	}
	NodesFormat = FString::Printf(TEXT("built (%d splits)"), B.Splits);
	return Subsectors.Num() > 0;
}

bool FDoomMap::Rebuild()
{
	// the node builder's split vertices from last time go; the map's own (used by lines) stay
	int32 MaxV = -1;
	for (const FDoomLine& L : Lines)
	{
		MaxV = FMath::Max3(MaxV, L.V1, L.V2);
	}
	if (Vertices.Num() > MaxV + 1)
	{
		Vertices.SetNum(MaxV + 1);
	}
	for (FDoomSector& S : Sectors)
	{
		S.Lines.Reset();
		S.Subsectors.Reset();
	}
	if (!BuildNodes())
	{
		return false;
	}
	AssignSubsectorSectors();
	Derive();
	BuildSubsectorPolygons();
	return true;
}
