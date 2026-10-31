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
			const double AreaOrig = Orig.SubsectorArea(), AreaBuilt = Built.SubsectorArea();
			FRandomStream Rng(1234);
			int32 Samples = 0, Agree = 0;
			for (int32 k = 0; k < 20000; ++k)
			{
				const FVector2f Pt(Rng.FRandRange(Orig.Bounds.Min.X, Orig.Bounds.Max.X), Rng.FRandRange(Orig.Bounds.Min.Y, Orig.Bounds.Max.Y));
				// only points well inside a sector (not on a line) are comparable
				bool bNearLine = false;
				for (int32 L = 0; L < Orig.Lines.Num() && !bNearLine; ++L) bNearLine = Orig.DistanceToLine(Pt, L) < 1.f;
				if (bNearLine) continue;
				// and only points inside the map: in the void either BSP may name any sector
				const int32 Ss = Orig.SubsectorAt(Pt);
				if (!Orig.Subsectors.IsValidIndex(Ss)) continue;
				const TArray<FVector2f>& Poly = Orig.Subsectors[Ss].Polygon;
				bool bInside = Poly.Num() >= 3;
				int32 Sign = 0;
				for (int32 v = 0; v < Poly.Num() && bInside; ++v)
				{
					const FVector2f E = Poly[(v + 1) % Poly.Num()] - Poly[v], Q = Pt - Poly[v];
					const float C = E.X * Q.Y - E.Y * Q.X;
					const int32 Sv = C > 0.f ? 1 : (C < 0.f ? -1 : 0);
					if (Sv != 0 && Sign != 0 && Sv != Sign) bInside = false;
					if (Sv != 0) Sign = Sv;
				}
				if (!bInside) continue;
				++Samples;
				Agree += Orig.SectorAt(Pt) == Built.SectorAt(Pt);
			}
			const double AreaErr = FMath::Abs(AreaBuilt - AreaOrig) / FMath::Max(AreaOrig, 1.0);
			const double Disagree = Samples ? 1.0 - (double)Agree / Samples : 1.0;
			// sectors are what matter; the outside of the map has no defined sector (either BSP may say anything)
			const bool bPass = bBuilt && AreaErr < 0.001 && Disagree < 0.02;
			Failed += bPass ? 0 : 1;
			UE_LOG(LogDoomContent, Display, TEXT("Nodetest %s %s: %s in %.0f ms, %d subsectors (WAD %d), area %.0f (WAD %.0f), same sector at %d of %d points (%.2f%% differ)"),
				*Name, bPass ? TEXT("PASS") : TEXT("FAIL"), *Built.NodesFormat, Ms, Built.Subsectors.Num(), Orig.Subsectors.Num(), AreaBuilt, AreaOrig,
				Agree, Samples, Disagree * 100.0);
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
			int32 Samples = 0, Agree = 0;
			if (bRead)
			{
				FRandomStream Rng(99);
				for (int32 k = 0; k < 5000; ++k)
				{
					const FVector2f Pt(Rng.FRandRange(Orig.Bounds.Min.X, Orig.Bounds.Max.X), Rng.FRandRange(Orig.Bounds.Min.Y, Orig.Bounds.Max.Y));
					const int32 Ss = Orig.SubsectorAt(Pt);
					if (!Orig.Subsectors.IsValidIndex(Ss)) continue;
					bool bNear = false;
					for (int32 L = 0; L < Orig.Lines.Num() && !bNear; ++L) bNear = Orig.DistanceToLine(Pt, L) < 2.f;
					if (bNear) continue;
					// inside the original subsector (either winding)
					const TArray<FVector2f>& Poly = Orig.Subsectors[Ss].Polygon;
					int32 Sign = 0;
					bool bInside = Poly.Num() >= 3;
					for (int32 v = 0; v < Poly.Num() && bInside; ++v)
					{
						const FVector2f E = Poly[(v + 1) % Poly.Num()] - Poly[v], Q = Pt - Poly[v];
						const float C = E.X * Q.Y - E.Y * Q.X;
						const int32 Sv = C > 0.f ? 1 : (C < 0.f ? -1 : 0);
						if (Sv && Sign && Sv != Sign) bInside = false;
						if (Sv) Sign = Sv;
					}
					if (!bInside) continue;
					++Samples;
					Agree += Orig.SectorAt(Pt) == Read.SectorAt(Pt);
				}
			}
			const int32 ErrOrig = DoomMapCheck::Count(DoomMapCheck::Run(Orig, DoomEditorData::Graphics(), Name), FDoomMapIssue::ESeverity::Error);
			const int32 ErrRead = bRead ? DoomMapCheck::Count(DoomMapCheck::Run(Read, DoomEditorData::Graphics(), Name), FDoomMapIssue::ESeverity::Error) : -1;
			const double AreaErr = bRead ? FMath::Abs(Read.SubsectorArea() - Orig.SubsectorArea()) / FMath::Max(Orig.SubsectorArea(), 1.0) : 1.0;
			const bool bPass = bSame && Samples > 0 && Agree >= Samples * 0.998 && ErrRead <= ErrOrig && AreaErr < 0.002;
			Failed += bPass ? 0 : 1;
			UE_LOG(LogDoomContent, Display, TEXT("Wadtest %s %s: %s; read back %s, same records %d, same sector at %d of %d points, area off %.3f%%, checker errors %d (was %d)"),
				*Name, bPass ? TEXT("PASS") : TEXT("FAIL"), *Note, bRead ? TEXT("ok") : TEXT("FAILED"), bSame ? 1 : 0, Agree, Samples, AreaErr * 100.0, ErrRead, ErrOrig);
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
