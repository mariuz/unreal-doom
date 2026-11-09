#include "DoomContentBuilder.h"
#include "DoomContentPrivate.h"
#include "UObject/Linker.h"
#include "Misc/FeedbackContext.h"
#include "DoomGameData.h"
#include "DoomMapTiles.h"
#include "DoomAssets.h"
#include "DoomUE.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Engine/Texture2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "NiagaraSystem.h"
#include "DoomMapActors.h"
#include "DoomMapCheck.h"
#include "DoomMapGen.h"
#include "DoomWadWriter.h"
#include "DoomAI.h"
#include "DoomWad.h"
#include "DoomSettings.h"
#include "DoomInput.h"
#include "HAL/FileManager.h"
#include "DoomMap.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomWorld.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "DoomLevel.h"
#include "StaticMeshCompiler.h"
#include "Engine/StaticMesh.h"
#include "DoomWad.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "NiagaraEmitterHandle.h"

DEFINE_LOG_CATEGORY(LogDoomContent);

namespace
{
	// The map's own geometry as the reference for a BSP (the node and round-trip tests): a BSP may put
	// anything in the void, and a WAD's nodes are not exact either (rounded split vertices can leave
	// holes, a stray one-line sector can own a patch of void), so neither is compared with the other.
	namespace MapTruth
	{
		/** Each sector's area from its own lines (shoelace over its sides, the sector on their right),
		 *  or -1 if its sides don't form closed loops or one of its lines ends where no other line does
		 *  (an open sector, or a stray line poking into the void: E3M4's line 1087, whose stub past a
		 *  wall makes a subsector out in the void in any BSP). */
		TArray<double> SectorAreas(const FDoomMap& M)
		{
			TArray<double> Area;
			Area.Init(0.0, M.Sectors.Num());
			TArray<TMap<int32, int32>> Degree; // per sector: vertex -> edges out minus edges in
			Degree.SetNum(M.Sectors.Num());
			for (const FDoomLine& L : M.Lines)
			{
				for (int32 s = 0; s < 2; ++s)
				{
					const int32 Side = L.Side[s];
					if (Side == INDEX_NONE || !M.Sides.IsValidIndex(Side) || !Area.IsValidIndex(M.Sides[Side].Sector)) continue;
					const int32 Sec = M.Sides[Side].Sector;
					const int32 A = s == 0 ? L.V1 : L.V2, B = s == 0 ? L.V2 : L.V1; // the sector on the right
					const FVector2d Pa(M.Vertices[A]), Pb(M.Vertices[B]);
					Area[Sec] -= 0.5 * (Pa.X * Pb.Y - Pb.X * Pa.Y);
					Degree[Sec].FindOrAdd(A) += 1;
					Degree[Sec].FindOrAdd(B) -= 1;
				}
			}
			TArray<int32> Uses; // lines ending at each vertex
			Uses.Init(0, M.Vertices.Num());
			for (const FDoomLine& L : M.Lines)
			{
				if (Uses.IsValidIndex(L.V1)) ++Uses[L.V1];
				if (Uses.IsValidIndex(L.V2)) ++Uses[L.V2];
			}
			for (const FDoomLine& L : M.Lines)
			{
				if ((Uses.IsValidIndex(L.V1) && Uses[L.V1] == 1) || (Uses.IsValidIndex(L.V2) && Uses[L.V2] == 1))
				{
					for (int32 s = 0; s < 2; ++s)
					{
						if (L.Side[s] != INDEX_NONE && M.Sides.IsValidIndex(L.Side[s]) && Area.IsValidIndex(M.Sides[L.Side[s]].Sector))
						{
							Area[M.Sides[L.Side[s]].Sector] = -1.0;
						}
					}
				}
			}
			for (int32 s = 0; s < Area.Num(); ++s)
			{
				bool bClosed = Area[s] > 0;
				for (const TPair<int32, int32>& It : Degree[s]) bClosed &= It.Value == 0;
				if (!bClosed) Area[s] = -1.0;
			}
			return Area;
		}

		/** The sector the walls around P enclose: the first line each of four rays (+-X, +-Y) meets,
		 *  on P's side. INDEX_NONE in the void (a ray escapes or meets a side with no sidedef), -2 if
		 *  the rays disagree (an open sector, overlapping lines). */
		int32 SectorAt(const FDoomMap& M, const FVector2f& P)
		{
			const float Reach = (M.Bounds.Max - M.Bounds.Min).Size() * 2.f + 64.f;
			static const FVector2f Dirs[4] = { FVector2f(1, 0), FVector2f(-1, 0), FVector2f(0, 1), FVector2f(0, -1) };
			int32 Result = -3;
			TArray<FLineHit> Hits;
			for (const FVector2f& D : Dirs)
			{
				Hits.Reset();
				M.TraceLines(P, P + D * Reach, Hits);
				int32 Sec = INDEX_NONE;
				if (Hits.Num())
				{
					const int32 Side = M.Lines[Hits[0].Line].Side[Hits[0].Side];
					Sec = Side != INDEX_NONE ? M.Sides[Side].Sector : INDEX_NONE;
				}
				if (Result == -3) Result = Sec;
				else if (Result != Sec) return -2;
			}
			return Result;
		}

		/** The BSP's floor area per sector (its subsector polygons). */
		TArray<double> PolygonAreas(const FDoomMap& M)
		{
			TArray<double> Area;
			Area.Init(0.0, M.Sectors.Num());
			for (const FDoomSubsector& Ss : M.Subsectors)
			{
				double A = 0;
				for (int32 k = 1; k + 1 < Ss.Polygon.Num(); ++k)
				{
					const FVector2f U = Ss.Polygon[k] - Ss.Polygon[0], W = Ss.Polygon[k + 1] - Ss.Polygon[0];
					A += 0.5 * (U.X * W.Y - U.Y * W.X);
				}
				if (Area.IsValidIndex(Ss.Sector)) Area[Ss.Sector] += FMath::Abs(A);
			}
			return Area;
		}

		/** How far a BSP's floors are from the closed sectors' own areas: the sum of the per-sector
		 *  differences over the sum of the areas (holes and spill into the void both count). */
		double AreaError(const FDoomMap& M, const TArray<double>& Truth, TArray<double>* OutPerSector = nullptr)
		{
			const TArray<double> Have = PolygonAreas(M);
			double Off = 0, Total = 0;
			if (OutPerSector) OutPerSector->Init(0.0, Truth.Num());
			for (int32 s = 0; s < Truth.Num(); ++s)
			{
				if (Truth[s] <= 0) continue;
				Off += FMath::Abs(Have[s] - Truth[s]);
				Total += Truth[s];
				if (OutPerSector) (*OutPerSector)[s] = Have[s] - Truth[s];
			}
			return Total > 0 ? Off / Total : 0.0;
		}

		/** Points spread over the map that the walls place in one sector, at least 1 unit from any line. */
		void SamplePoints(const FDoomMap& M, int32 Count, int32 Seed, TArray<TPair<FVector2f, int32>>& Out)
		{
			FRandomStream Rng(Seed);
			for (int32 k = 0; k < Count; ++k)
			{
				const FVector2f Pt(Rng.FRandRange(M.Bounds.Min.X, M.Bounds.Max.X), Rng.FRandRange(M.Bounds.Min.Y, M.Bounds.Max.Y));
				bool bNearLine = false;
				for (int32 L = 0; L < M.Lines.Num() && !bNearLine; ++L) bNearLine = M.DistanceToLine(Pt, L) < 1.f;
				if (bNearLine) continue;
				const int32 Sec = SectorAt(M, Pt);
				if (Sec >= 0) Out.Add({ Pt, Sec });
			}
		}
	}
}

bool DoomContent::BuildGameData(bool bOverwrite)
{
	const TCHAR* DataFolder = TEXT("/Game/Doom/Data/");
	const FString Name = TEXT("DA_DoomGameData");
	if (!bOverwrite && FPackageName::DoesPackageExist(FString(DataFolder) + Name))
	{
		return true;
	}
	UPackage* Pkg = DoomContent::MakePackage(Name, DataFolder);
	UDoomGameData* Data = NewObject<UDoomGameData>(Pkg, *Name, RF_Public | RF_Standalone);
	DoomInfo::BuiltInTables(Data->Things, Data->Projectiles, Data->Weapons);
	Data->Rules = FDoomRules();
	UE_LOG(LogDoomContent, Display, TEXT("Game data: %d things, %d projectiles, %d weapons -> %s%s"),
		Data->Things.Num(), Data->Projectiles.Num(), Data->Weapons.Num(), DataFolder, *Name);
	return DoomContent::Save(Data);
}

bool DoomContent::BuildInput(bool bOverwrite)
{
	FDoomInputObjects Existing;
	if (!bOverwrite && DoomInput::LoadAssets(Existing))
	{
		UE_LOG(LogDoomContent, Log, TEXT("Doom input assets already exist"));
		return true;
	}
	UE_LOG(LogDoomContent, Display, TEXT("Generating Doom input assets in %s"), DoomInput::AssetFolder);
	FDoomInputObjects In;
	DoomInput::Build([](UClass* Class, FName Name)
	{
		UPackage* Pkg = DoomContent::MakePackage(Name.ToString(), DoomInput::AssetFolder);
		return NewObject<UObject>(Pkg, Class, Name, RF_Public | RF_Standalone | RF_Transactional);
	}, In);
	bool bOk = true;
	for (UObject* Asset : In.All())
	{
		bOk &= Asset && DoomContent::Save(Asset);
	}
	return bOk;
}

bool DoomContent::BuildAI(bool bOverwrite)
{
	const FString TreePath = FString(DoomAI::AssetFolder) + TEXT("BT_DoomMonster.BT_DoomMonster");
	if (!bOverwrite && LoadObject<UObject>(nullptr, *TreePath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		UE_LOG(LogDoomContent, Log, TEXT("Doom AI assets already exist"));
		return true;
	}
	UE_LOG(LogDoomContent, Display, TEXT("Generating the Doom monster AI (blackboard, behavior tree, EQS queries) in %s"), DoomAI::AssetFolder);
	FDoomAIAssets AI;
	DoomAI::Build([](UClass* Class, FName Name)
	{
		UPackage* Pkg = DoomContent::MakePackage(Name.ToString(), DoomAI::AssetFolder);
		return NewObject<UObject>(Pkg, Class, Name, RF_Public | RF_Standalone | RF_Transactional);
	}, AI);
	bool bOk = true;
	for (UObject* Asset : AI.All())
	{
		bOk &= Asset && DoomContent::Save(Asset);
	}
	return bOk;
}

int32 UDoomContentCommandlet::Main(const FString& Params)
{
	// -exportlyra=E1M1 -out=<dir>: the map as a Lyra arena (OBJ tiles, ramps, gameplay JSON)
	FString LyraMap;
	if (FParse::Value(*Params, TEXT("-exportlyra="), LyraMap))
	{
		FString OutDir = FPaths::ProjectSavedDir() / TEXT("LyraExport");
		FParse::Value(*Params, TEXT("-out="), OutDir);
		return DoomContent::ExportLyraLevel(LyraMap, OutDir) ? 0 : 1;
	}
	// -nodetest=E1M1,E1M2 (or all): the node builder against the WAD's own nodes: the same total area,
	// and the same sector at points sampled over the map
	FString NodeMaps;
	if (FParse::Value(*Params, TEXT("-nodetest="), NodeMaps, false))
	{
		TArray<FString> Names;
		if (NodeMaps.Equals(TEXT("all"), ESearchCase::IgnoreCase)) Names = DoomEditorData::MapNames();
		else NodeMaps.ParseIntoArray(Names, TEXT(","));
		int32 Failed = 0;
		for (const FString& Raw : Names)
		{
			const FString Name = Raw.ToUpper();
			FDoomMap Orig;
			if (!DoomEditorData::Wad() || !Orig.Load(*DoomEditorData::Wad(), Name))
			{
				UE_LOG(LogDoomContent, Error, TEXT("Nodetest: no map %s"), *Name);
				++Failed;
				continue;
			}
			FDoomMap Built = Orig;
			const double T0 = FPlatformTime::Seconds();
			const bool bBuilt = Built.Rebuild();
			const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
			// both BSPs against the map's own walls (MapTruth): the sector at points the walls enclose, and
			// each closed sector's floor area; the WAD's nodes are reported for reference
			const TArray<double> Truth = MapTruth::SectorAreas(Orig);
			TArray<double> OffBuilt;
			const double AreaErrBuilt = MapTruth::AreaError(Built, Truth, &OffBuilt), AreaErrOrig = MapTruth::AreaError(Orig, Truth);
			TArray<TPair<FVector2f, int32>> Points;
			MapTruth::SamplePoints(Orig, 20000, 1234, Points);
			int32 AgreeBuilt = 0, AgreeOrig = 0;
			const bool bDebug = FParse::Param(*Params, TEXT("nodedebug"));
			TMap<int32, TArray<FVector2f>> WrongAt; // built subsector -> points where it names another sector
			for (const TPair<FVector2f, int32>& It : Points)
			{
				AgreeOrig += Orig.SectorAt(It.Key) == It.Value;
				const bool bSame = Built.SectorAt(It.Key) == It.Value;
				AgreeBuilt += bSame;
				if (!bSame && bDebug)
				{
					WrongAt.FindOrAdd(Built.SubsectorAt(It.Key)).Add(It.Key);
				}
			}
			if (bDebug)
			{
				// the closed sectors whose built floors are furthest from their walls' area, and the built
				// subsectors that name the wrong sector
				TArray<int32> Order;
				for (int32 s = 0; s < OffBuilt.Num(); ++s) if (FMath::Abs(OffBuilt[s]) >= 1000) Order.Add(s);
				Order.Sort([&OffBuilt](int32 X, int32 Y) { return FMath::Abs(OffBuilt[X]) > FMath::Abs(OffBuilt[Y]); });
				for (int32 i = 0; i < FMath::Min(10, Order.Num()); ++i)
				{
					UE_LOG(LogDoomContent, Display, TEXT("  sector %d: built floor %+.0f against its walls' %.0f"), Order[i], OffBuilt[Order[i]], Truth[Order[i]]);
				}
				// the worst sector's built subsectors that reach outside it: a grid of points in each polygon
				for (int32 i = 0; i < FMath::Min(1, Order.Num()); ++i)
				{
					for (int32 SsIndex = 0; SsIndex < Built.Subsectors.Num(); ++SsIndex)
					{
						const FDoomSubsector& Ss = Built.Subsectors[SsIndex];
						if (Ss.Sector != Order[i] || Ss.Polygon.Num() < 3) continue;
						FBox2f Box(Ss.Polygon);
						int32 In = 0, Out = 0;
						for (int32 gx = 0; gx < 24; ++gx)
						{
							for (int32 gy = 0; gy < 24; ++gy)
							{
								const FVector2f Pt(FMath::Lerp(Box.Min.X, Box.Max.X, (gx + 0.5f) / 24.f), FMath::Lerp(Box.Min.Y, Box.Max.Y, (gy + 0.5f) / 24.f));
								bool bInside = true;
								int32 Sign = 0;
								for (int32 v = 0; v < Ss.Polygon.Num() && bInside; ++v)
								{
									const FVector2f E = Ss.Polygon[(v + 1) % Ss.Polygon.Num()] - Ss.Polygon[v], Q = Pt - Ss.Polygon[v];
									const float C = E.X * Q.Y - E.Y * Q.X;
									const int32 Sv = C > 0.f ? 1 : (C < 0.f ? -1 : 0);
									if (Sv && Sign && Sv != Sign) bInside = false;
									if (Sv) Sign = Sv;
								}
								if (!bInside) continue;
								const int32 TrueSec = MapTruth::SectorAt(Orig, Pt);
								(TrueSec == Ss.Sector || TrueSec == -2 ? In : Out) += 1;
							}
						}
						if (Out * 10 < In + Out) continue;
						FString SegText, PolyText;
						for (int32 s = Ss.FirstSeg; s < Ss.FirstSeg + Ss.NumSegs; ++s)
						{
							const FDoomSeg& Sg = Built.Segs[s];
							SegText += FString::Printf(TEXT(" L%d/%d(%.0f,%.0f>%.0f,%.0f)"), Sg.Line, Sg.Side,
								Built.Vertices[Sg.V1].X, Built.Vertices[Sg.V1].Y, Built.Vertices[Sg.V2].X, Built.Vertices[Sg.V2].Y);
						}
						for (const FVector2f& P : Ss.Polygon) PolyText += FString::Printf(TEXT(" (%.0f,%.0f)"), P.X, P.Y);
						UE_LOG(LogDoomContent, Display, TEXT("  spill: subsector %d, %d of %d grid points outside; segs%s; polygon%s"),
							SsIndex, Out, In + Out, *SegText, *PolyText);
					}
				}
				WrongAt.ValueSort([](const TArray<FVector2f>& X, const TArray<FVector2f>& Y) { return X.Num() > Y.Num(); });
				int32 Shown = 0;
				for (const TPair<int32, TArray<FVector2f>>& It : WrongAt)
				{
					if (Shown++ >= 8 || !Built.Subsectors.IsValidIndex(It.Key)) break;
					const FDoomSubsector& Ss = Built.Subsectors[It.Key];
					const FVector2f Pt = It.Value[0];
					FString SegText;
					for (int32 s = Ss.FirstSeg; s < Ss.FirstSeg + Ss.NumSegs; ++s)
					{
						const FDoomSeg& Sg = Built.Segs[s];
						const int32 Side = Built.Lines[Sg.Line].Side[Sg.Side];
						SegText += FString::Printf(TEXT(" L%d/%d(s%d %.0f,%.0f>%.0f,%.0f)"), Sg.Line, Sg.Side, Side != INDEX_NONE ? Built.Sides[Side].Sector : -1,
							Built.Vertices[Sg.V1].X, Built.Vertices[Sg.V1].Y, Built.Vertices[Sg.V2].X, Built.Vertices[Sg.V2].Y);
					}
					UE_LOG(LogDoomContent, Display, TEXT("  subsector %d: %d wrong points, e.g. (%.0f,%.0f) walls say sector %d, built %d; segs%s"),
						It.Key, It.Value.Num(), Pt.X, Pt.Y, MapTruth::SectorAt(Orig, Pt), Ss.Sector, *SegText);
				}
			}
			const int32 Samples = Points.Num();
			// no worse than the map's own nodes against the same walls: where both BSPs disagree with the walls,
			// the map's sector references are inconsistent (E3M8, TNT MAP02 and MAP31), not the builder wrong
			const bool bPass = bBuilt && AgreeBuilt >= AgreeOrig - Samples / 200 && AreaErrBuilt <= AreaErrOrig + 0.005;
			Failed += bPass ? 0 : 1;
			UE_LOG(LogDoomContent, Display, TEXT("Nodetest %s %s: %s in %.0f ms, %d subsectors (WAD %d); against the walls: right sector at %d of %d points (WAD nodes %d), floors off by %.2f%% (WAD nodes %.2f%%)"),
				*Name, bPass ? TEXT("PASS") : TEXT("FAIL"), *Built.NodesFormat, Ms, Built.Subsectors.Num(), Orig.Subsectors.Num(),
				AgreeBuilt, Samples, AgreeOrig, AreaErrBuilt * 100.0, AreaErrOrig * 100.0);
		}
		return Failed ? 1 : 0;
	}
	// a WAD map or a generated one (RND<seed>), for the export switches
	auto GetMap = [](const FString& Name, FDoomMap& Out)
	{
		uint32 Seed = 0;
		if (DoomMapGen::ParseName(Name, Seed))
		{
			const DoomMapGen::FOptions O = DoomMapGen::DefaultOptions(Seed, DoomEditorData::Wad() && DoomEditorData::Wad()->IsDoom2());
			return DoomMapGen::Generate(O, FDoomGraphics::PictureCheck(DoomEditorData::Graphics()), Out);
		}
		return DoomEditorData::Wad() && Out.Load(*DoomEditorData::Wad(), Name);
	};
	// -exportmap=RND0002A[,MAP01] [-out=path.wad]: a map as a PWAD (Saved/Maps/<name>.wad by default);
	// a generated map goes in as MAP01 (Doom 2) or E1M1 unless a slot is given
	FString ExportArg;
	if (FParse::Value(*Params, TEXT("-exportmap="), ExportArg, false))
	{
		TArray<FString> Parts;
		ExportArg.ParseIntoArray(Parts, TEXT(","));
		const FString Name = Parts.Num() ? Parts[0].ToUpper() : FString();
		uint32 Seed = 0;
		const bool bGen = DoomMapGen::ParseName(Name, Seed);
		const FString Slot = Parts.Num() > 1 ? Parts[1].ToUpper()
			: bGen ? FString(DoomEditorData::Wad() && DoomEditorData::Wad()->IsDoom2() ? TEXT("MAP01") : TEXT("E1M1")) : Name;
		FString OutPath;
		if (!FParse::Value(*Params, TEXT("-out="), OutPath))
		{
			OutPath = FPaths::ProjectSavedDir() / TEXT("Maps") / Name + TEXT(".wad");
		}
		FDoomMap M;
		FString Note;
		if (!GetMap(Name, M) || !DoomWadWriter::SaveMap(M, Slot, OutPath, &Note))
		{
			UE_LOG(LogDoomContent, Error, TEXT("Exportmap: could not export %s"), *Name);
			return 1;
		}
		UE_LOG(LogDoomContent, Display, TEXT("Exportmap %s -> %s as %s: %s"), *Name, *FPaths::ConvertRelativePathToFull(OutPath), *Slot, *Note);
		return 0;
	}
	// -wadtest=E1M1,RND00003 (or all): each map written as a PWAD and read back: the same lines,
	// sides, sectors and things, the same sectors at sampled points, the map checker no worse
	FString WadTest;
	if (FParse::Value(*Params, TEXT("-wadtest="), WadTest, false))
	{
		TArray<FString> Names;
		if (WadTest.Equals(TEXT("all"), ESearchCase::IgnoreCase)) Names = DoomEditorData::MapNames();
		else WadTest.ParseIntoArray(Names, TEXT(","));
		int32 Failed = 0;
		for (const FString& Raw : Names)
		{
			const FString Name = Raw.ToUpper();
			FDoomMap Orig;
			if (!GetMap(Name, Orig))
			{
				UE_LOG(LogDoomContent, Error, TEXT("Wadtest: no map %s"), *Name);
				++Failed;
				continue;
			}
			const FString Path = FPaths::ProjectSavedDir() / TEXT("Test") / FString::Printf(TEXT("wadtest_%s.wad"), *Name);
			FString Note;
			FDoomWad Back;
			FDoomMap Read;
			const bool bWritten = DoomWadWriter::SaveMap(Orig, Name, Path, &Note);
			const bool bRead = bWritten && Back.AddFile(Path) && (Back.Finalize(), Read.Load(Back, Name));
			bool bSame = bRead && Read.Lines.Num() == Orig.Lines.Num() && Read.Sides.Num() == Orig.Sides.Num()
				&& Read.Sectors.Num() == Orig.Sectors.Num() && Read.Things.Num() == Orig.Things.Num();
			for (int32 i = 0; bSame && i < Orig.Lines.Num(); ++i)
			{
				const FDoomLine& A = Orig.Lines[i];
				const FDoomLine& B = Read.Lines[i];
				bSame = A.Special == B.Special && A.Tag == B.Tag && A.Side[0] == B.Side[0] && A.Side[1] == B.Side[1]
					&& (A.Flags & ~DoomLineFlags::Mapped) == B.Flags;
			}
			for (int32 i = 0; bSame && i < Orig.Sides.Num(); ++i)
			{
				const FDoomSide& A = Orig.Sides[i];
				const FDoomSide& B = Read.Sides[i];
				bSame = A.Sector == B.Sector && A.Upper == B.Upper && A.Lower == B.Lower && A.Middle == B.Middle;
			}
			for (int32 i = 0; bSame && i < Orig.Sectors.Num(); ++i)
			{
				const FDoomSector& A = Orig.Sectors[i];
				const FDoomSector& B = Read.Sectors[i];
				bSame = A.Floor == B.Floor && A.Ceiling == B.Ceiling && A.FloorPic == B.FloorPic && A.CeilingPic == B.CeilingPic
					&& A.Light == B.Light && A.Special == B.Special && A.Tag == B.Tag;
			}
			// the read-back map (nodes built on export) against the map's own walls (MapTruth)
			int32 Samples = 0, Agree = 0, AgreeOrig = 0;
			double AreaErr = 1.0, AreaErrOrig = 0.0;
			if (bRead)
			{
				TArray<TPair<FVector2f, int32>> Points;
				MapTruth::SamplePoints(Orig, 5000, 99, Points);
				Samples = Points.Num();
				for (const TPair<FVector2f, int32>& It : Points)
				{
					Agree += Read.SectorAt(It.Key) == It.Value;
					AgreeOrig += Orig.SectorAt(It.Key) == It.Value;
				}
				const TArray<double> Truth = MapTruth::SectorAreas(Orig);
				AreaErr = MapTruth::AreaError(Read, Truth);
				AreaErrOrig = MapTruth::AreaError(Orig, Truth);
			}
			const int32 ErrOrig = DoomMapCheck::Count(DoomMapCheck::Run(Orig, DoomEditorData::Graphics(), Name), FDoomMapIssue::ESeverity::Error);
			const int32 ErrRead = bRead ? DoomMapCheck::Count(DoomMapCheck::Run(Read, DoomEditorData::Graphics(), Name), FDoomMapIssue::ESeverity::Error) : -1;
			const bool bPass = bSame && Samples > 0 && Agree >= AgreeOrig - Samples / 200 && ErrRead <= ErrOrig && AreaErr <= AreaErrOrig + 0.005;
			Failed += bPass ? 0 : 1;
			UE_LOG(LogDoomContent, Display, TEXT("Wadtest %s %s: %s; read back %s, same records %d, right sector at %d of %d points (before %d), floors off by %.2f%% (before %.2f%%), checker errors %d (was %d)"),
				*Name, bPass ? TEXT("PASS") : TEXT("FAIL"), *Note, bRead ? TEXT("ok") : TEXT("FAILED"), bSame ? 1 : 0, Agree, Samples, AgreeOrig, AreaErr * 100.0, AreaErrOrig * 100.0, ErrRead, ErrOrig);
		}
		return Failed ? 1 : 0;
	}
	// -navtest=E1M1,RND00003 (or all): monster paths (FDoomNav) from every monster to player 1's start
	FString NavTest;
	if (FParse::Value(*Params, TEXT("-navtest="), NavTest, false))
	{
		TArray<FString> Names;
		if (NavTest.Equals(TEXT("all"), ESearchCase::IgnoreCase)) Names = DoomEditorData::MapNames();
		else NavTest.ParseIntoArray(Names, TEXT(","));
		for (const FString& Raw : Names)
		{
			const FString Name = Raw.ToUpper();
			FDoomMap M;
			if (!GetMap(Name, M)) continue;
			FDoomNav Nav;
			const double T0 = FPlatformTime::Seconds();
			Nav.Build(M);
			const double BuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
			FVector2f Start = FVector2f::ZeroVector;
			for (const FDoomMapThing& T : M.Things) if (T.Type == 1) Start = T.P;
			int32 Monsters = 0, Reached = 0, Isolated = 0;
			double PathMs = 0;
			TArray<FVector2f> Path;
			FString Fails;
			for (const FDoomMapThing& T : M.Things)
			{
				const FDoomThingDef* Def = DoomInfo::FindThing(T.Type);
				if (!Def || !(Def->Flags & EThingFlag::CountKill)) continue;
				++Monsters;
				const double T1 = FPlatformTime::Seconds();
				const bool bOk = Nav.FindPath(M, T.P, Start, Def->Radius, Def->Height, (Def->Flags & EThingFlag::Float) != 0, Path);
				PathMs += (FPlatformTime::Seconds() - T1) * 1000.0;
				Reached += bOk ? 1 : 0;
				if (!bOk && Fails.Len() < 600) Fails += FString::Printf(TEXT(" %s(%.0f,%.0f ss %d)"), *Def->Name, T.P.X, T.P.Y, M.SubsectorAt(T.P));
			}
			UE_LOG(LogDoomContent, Display, TEXT("Navtest %s: %d portals built in %.1f ms; from the start %d of %d subsectors joined, %d walkable (imp); %d of %d monsters can walk to the start (%.2f ms a path); no way:%s"),
				*Name, Nav.NumPortals(), BuildMs, Nav.CountReachable(M, Start, false, 20, 56), M.Subsectors.Num(), Nav.CountReachable(M, Start, true, 20, 56),
				Reached, Monsters, Monsters ? PathMs / Monsters : 0.0, *Fails.Left(300));
		}
		return 0;
	}
	// -gentest=N[,first seed]: N generated maps through the map checker; each must have no errors
	FString GenTest;
	if (FParse::Value(*Params, TEXT("-gentest="), GenTest, false))
	{
		TArray<FString> Parts;
		GenTest.ParseIntoArray(Parts, TEXT(","));
		const int32 Count = Parts.Num() > 0 ? FCString::Atoi(*Parts[0]) : 20;
		const int32 First = Parts.Num() > 1 ? FCString::Atoi(*Parts[1]) : 1;
		int32 Failed = 0, Warnings = 0;
		double TotalMs = 0;
		for (int32 k = 0; k < Count; ++k)
		{
			const DoomMapGen::FOptions O = DoomMapGen::DefaultOptions(First + k, DoomEditorData::Wad() && DoomEditorData::Wad()->IsDoom2());
			FDoomMap M;
			DoomMapGen::FReport R;
			const double T0 = FPlatformTime::Seconds();
			const bool bOk = DoomMapGen::Generate(O, FDoomGraphics::PictureCheck(DoomEditorData::Graphics()), M, &R);
			const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
			TotalMs += Ms;
			const FString Name = DoomMapGen::NameForSeed(O.Seed);
			if (!bOk)
			{
				++Failed;
				UE_LOG(LogDoomContent, Display, TEXT("Gentest %s FAIL: no map"), *Name);
				continue;
			}
			const TArray<FDoomMapIssue> Issues = DoomMapCheck::Run(M, DoomEditorData::Graphics(), Name);
			const int32 Errors = DoomMapCheck::Count(Issues, FDoomMapIssue::ESeverity::Error);
			Warnings += DoomMapCheck::Count(Issues, FDoomMapIssue::ESeverity::Warning);
			Failed += Errors ? 1 : 0;
			UE_LOG(LogDoomContent, Display, TEXT("Gentest %s %s: %s in %.0f ms; %d sectors, %d lines, %d subsectors"),
				*Name, Errors ? TEXT("FAIL") : TEXT("PASS"), *R.ToString(), Ms, M.Sectors.Num(), M.Lines.Num(), M.Subsectors.Num());
			if (Errors || Params.Contains(TEXT("-genlog")))
			{
				DoomMapCheck::Log(Name, Issues);
			}
			// -gendump: the map's lines, sectors and things as text (Saved/Gen/<name>.txt) for plots
			if (Params.Contains(TEXT("-gendump")))
			{
				FString Txt;
				for (const FDoomLine& L : M.Lines)
				{
					Txt += FString::Printf(TEXT("L %.0f %.0f %.0f %.0f %d %d %d\n"), M.V(L.V1).X, M.V(L.V1).Y, M.V(L.V2).X, M.V(L.V2).Y,
						L.BackSector != INDEX_NONE ? 1 : 0, L.Special, L.FrontSector);
				}
				for (const FDoomSector& S : M.Sectors)
				{
					Txt += FString::Printf(TEXT("S %.0f %.0f %s\n"), S.Floor, S.Ceiling, *S.FloorPic.ToString());
				}
				for (const FDoomMapThing& T : M.Things)
				{
					Txt += FString::Printf(TEXT("T %.0f %.0f %d\n"), T.P.X, T.P.Y, T.Type);
				}
				FFileHelper::SaveStringToFile(Txt, *(FPaths::ProjectSavedDir() / TEXT("Gen") / Name + TEXT(".txt")));
			}
		}
		UE_LOG(LogDoomContent, Display, TEXT("Gentest: %d of %d maps without errors, %d warnings, %.0f ms a map"), Count - Failed, Count, Warnings, Count ? TotalMs / Count : 0.0);
		return Failed ? 1 : 0;
	}
	// -checkmap=E1M1,E1M2 (or all): the map checker on the WAD's maps, to the log
	FString CheckMaps;
	if (FParse::Value(*Params, TEXT("-checkmap="), CheckMaps, false))
	{
		TArray<FString> Names;
		if (CheckMaps.Equals(TEXT("all"), ESearchCase::IgnoreCase)) Names = DoomEditorData::MapNames();
		else CheckMaps.ParseIntoArray(Names, TEXT(","));
		for (const FString& Name : Names)
		{
			FDoomMap M;
			if (DoomEditorData::Wad() && M.Load(*DoomEditorData::Wad(), Name.ToUpper()))
			{
				DoomMapCheck::Log(Name.ToUpper(), DoomMapCheck::Run(M, DoomEditorData::Graphics(), Name.ToUpper()));
			}
			else
			{
				UE_LOG(LogDoomContent, Error, TEXT("Checkmap: no map %s"), *Name);
			}
		}
		return 0;
	}
	// -renamelevels: levels imported as /Game/Doom/Maps/<MAP> become L_<MAP>
	if (FParse::Param(*Params, TEXT("renamelevels")))
	{
		int32 Renamed = 0;
		for (const FString& Name : DoomEditorData::MapNames())
		{
			Renamed += DoomContent::RenameOldLevel(Name);
		}
		UE_LOG(LogDoomContent, Display, TEXT("Rename levels: %d renamed"), Renamed);
		return 0;
	}
	// -validate: Tools > Validate Data on the Doom assets; -validatetest: the checks on broken copies too
	if (FParse::Param(*Params, TEXT("validatetest")))
	{
		return DoomContent::ValidateTest() ? 0 : 1;
	}
	if (FParse::Param(*Params, TEXT("validate")))
	{
		return DoomContent::Validate() ? 0 : 1;
	}
	FString EditMap;
	FString DrawMap;
	if (FParse::Value(*Params, TEXT("-drawtest="), DrawMap))
	{
		return DoomContent::DrawTest(DrawMap) ? 0 : 1;
	}
	if (FParse::Value(*Params, TEXT("-edittest="), EditMap))
	{
		return DoomContent::EditTest(EditMap) ? 0 : 1;
	}
	// -importmap=E1M1[,E1M2...] or -importmap=all: save WAD maps as editable levels
	FString Import;
	if (FParse::Value(*Params, TEXT("-importmap="), Import, false))
	{
		TArray<FString> Names;
		if (Import.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			Names = DoomEditorData::MapNames();
		}
		else
		{
			Import.ParseIntoArray(Names, TEXT(","));
		}
		bool bOk = true;
		for (const FString& N : Names)
		{
			bOk &= DoomContent::ImportMap(N, Params.Contains(TEXT("-force")));
		}
		return bOk ? 0 : 1;
	}
	// -rebaketiles=E1M1[,E1M2...]|all: an imported level's tiles again with the current build settings
	FString Rebake;
	if (FParse::Value(*Params, TEXT("-rebaketiles="), Rebake, false))
	{
		TArray<FString> Names;
		if (Rebake.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			for (const FString& N : DoomEditorData::MapNames())
			{
				if (FPackageName::DoesPackageExist(DoomContent::MapPackage(N))) Names.Add(N);
			}
		}
		else
		{
			Rebake.ParseIntoArray(Names, TEXT(","));
		}
		int32 Failed = 0;
		for (const FString& N : Names)
		{
			Failed += DoomContent::RebakeLevelTiles(N) ? 0 : 1;
		}
		UE_LOG(LogDoomContent, Display, TEXT("Rebake: %d levels, %d failed"), Names.Num(), Failed);
		return Failed ? 1 : 0;
	}
	// -baketiles=E1M1,E1M2|all [-iwad= -file=]: tile sets for maps played straight from the WAD
	FString BakeTiles;
	if (FParse::Value(*Params, TEXT("-baketiles="), BakeTiles, false))
	{
		TArray<FString> Names;
		if (BakeTiles.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			Names = DoomEditorData::MapNames();
		}
		else
		{
			BakeTiles.ParseIntoArray(Names, TEXT(","));
		}
		int32 Failed = 0;
		for (const FString& N : Names)
		{
			Failed += DoomContent::BakeWadTiles(N) ? 0 : 1;
		}
		UE_LOG(LogDoomContent, Display, TEXT("Bake tiles: %d maps of %s, %d failed"), Names.Num(), *DoomEditorData::WadSet(), Failed);
		return Failed ? 1 : 0;
	}
	// -ai [-force]: the SMART monsters' blackboard, behavior tree and EQS queries as assets
	if (FParse::Param(*Params, TEXT("ai")))
	{
		return DoomContent::BuildAI(Params.Contains(TEXT("-force"))) ? 0 : 1;
	}
	// -input [-force]: the input actions and mapping contexts as assets
	if (Params.Contains(TEXT("-input")))
	{
		return DoomContent::BuildInput(Params.Contains(TEXT("-force"))) ? 0 : 1;
	}
	// -gamedata [-force]: the editable game tables, from the built-in ones
	if (Params.Contains(TEXT("-gamedata")))
	{
		return DoomContent::BuildGameData(Params.Contains(TEXT("-force"))) ? 0 : 1;
	}
	FString Dump;
	if (FParse::Value(*Params, TEXT("-dumpniagara="), Dump))
	{
		DoomContent::DumpNiagara(Dump);
		return 0;
	}
	const bool bForce = Params.Contains(TEXT("-force"));
	// -effects / -materials: regenerate only one kind (new materials cost a long shader compile)
	const bool bOnlyEffects = Params.Contains(TEXT("-effects")), bOnlyMaterials = Params.Contains(TEXT("-materials"));
	return DoomContent::Build(bForce, !bOnlyEffects, !bOnlyMaterials) ? 0 : 1;
}
