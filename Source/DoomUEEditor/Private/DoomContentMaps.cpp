// The content builder's maps: importing WAD maps as levels, baking static-mesh tiles, and the
// editing test (-edittest).
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

// ---- Map import --------------------------------------------------------------------------------

namespace
{
	/** Splits a static chunk into 1024-unit tiles and saves them as static meshes in TileFolder. */
	bool SaveTileMeshes(const FString& MapName, const FString& TileFolder, const TMap<FName, ADoomLevel::FSectionData>& Solid,
		const TMap<FName, ADoomLevel::FSectionData>& NonSolid, TArray<UStaticMesh*>& Meshes)
	{
		TArray<ADoomLevel::FTile> Tiles;
		ADoomLevel::SplitTiles(Solid, NonSolid, Tiles);
		// slots keep their section key as the name; the game puts its own material instances there
		UMaterialInterface* Placeholder = DoomAssets::WorldMaterial(true);
		auto Coord = [](int32 V) { return V < 0 ? FString::Printf(TEXT("N%d"), -V) : FString::FromInt(V); };
		for (const ADoomLevel::FTile& Tile : Tiles)
		{
			const FString Name = FString::Printf(TEXT("SM_%s_%s_%s"), *MapName, *Coord(Tile.Cell.X), *Coord(Tile.Cell.Y));
			UPackage* Pkg = DoomContent::MakePackage(Name, *TileFolder);
			UStaticMesh* Mesh = NewObject<UStaticMesh>(Pkg, FName(*Name), RF_Public | RF_Standalone);
			ADoomLevel::FillTileMesh(Mesh, Tile, [Placeholder](FName) { return Placeholder; });
			Mesh->Build(true);
			Meshes.Add(Mesh);
		}
		FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
		bool bOk = true;
		for (UStaticMesh* Mesh : Meshes)
		{
			bOk &= DoomContent::Save(Mesh);
		}
		return bOk;
	}
}

bool DoomContent::BakeWadTiles(const FString& InMapName)
{
	const FString MapName = InMapName.ToUpper();
	FDoomWad* Wad = DoomEditorData::Wad();
	FDoomGraphics* Gfx = DoomEditorData::Graphics();
	FDoomMap Map;
	if (!Wad || !Gfx || !Map.Load(*Wad, MapName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("Bake tiles: no map %s in the WAD"), *MapName);
		return false;
	}
	// the static chunk exactly as the game builds it from the WAD: same movable sectors and sky
	TSet<int32> Movable;
	UDoomWorld::ComputeMovableSectors(Map, Movable);
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false);
	ADoomLevel* Level = World->SpawnActor<ADoomLevel>();
	Level->bEditorPreview = true;
	Level->Build(Map, *Gfx, Movable, true, FDoomGraphics::SkyTextureFor(MapName));
	TMap<FName, ADoomLevel::FSectionData> Solid, NonSolid;
	Level->GetStaticSections(Solid, NonSolid);
	World->DestroyWorld(false);
	if (Solid.Num() == 0 && NonSolid.Num() == 0)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Bake tiles: no static geometry in %s"), *MapName);
		return false;
	}
	const FString WadSet = DoomEditorData::WadSet();
	const FString TileFolder = UDoomMapTiles::Folder(WadSet, MapName);
	TArray<UStaticMesh*> Meshes;
	bool bOk = SaveTileMeshes(MapName, TileFolder, Solid, NonSolid, Meshes);
	const FString SetName = UDoomMapTiles::AssetName(MapName);
	UPackage* Pkg = DoomContent::MakePackage(SetName, *TileFolder);
	UDoomMapTiles* Set = NewObject<UDoomMapTiles>(Pkg, FName(*SetName), RF_Public | RF_Standalone);
	Set->MapName = MapName;
	Set->WadSet = WadSet;
	Set->Signature = ADoomLevel::Signature(Solid, NonSolid);
	for (UStaticMesh* Mesh : Meshes)
	{
		Set->Tiles.Add(Mesh);
	}
	bOk &= DoomContent::Save(Set);
	UE_LOG(LogDoomContent, Display, TEXT("Bake tiles: %s: %d tiles in %s (signature %08x)"), *MapName, Meshes.Num(), *TileFolder, (uint32)Set->Signature);
	return bOk;
}

bool DoomContent::BakeMapTiles(ADoomMapActor* MapActor)
{
	if (!MapActor)
	{
		return false;
	}
	// the preview builds the same chunks as the game, sector edits included
	MapActor->RebuildPreview();
	TMap<FName, ADoomLevel::FSectionData> Solid, NonSolid;
	MapActor->GetStaticSections(Solid, NonSolid);
	if (Solid.Num() == 0 && NonSolid.Num() == 0)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Bake: no geometry for %s (is the IWAD in Content/WADs?)"), *MapActor->MapName);
		return false;
	}
	const uint32 Sig = ADoomLevel::Signature(Solid, NonSolid);
	int32 Verts = 0;
	for (const auto& P : Solid) Verts += P.Value.V.Num();
	for (const auto& P : NonSolid) Verts += P.Value.V.Num();
	UE_LOG(LogDoomContent, Display, TEXT("Bake: static chunk %d + %d sections, %d vertices"), Solid.Num(), NonSolid.Num(), Verts);
	const FString MapName = MapActor->MapName.ToUpper();
	const FString TileFolder = FString::Printf(TEXT("/Game/Doom/Maps/%s_Tiles/"), *MapName);
	TArray<UStaticMesh*> Meshes;
	bool bOk = SaveTileMeshes(MapName, TileFolder, Solid, NonSolid, Meshes);
	MapActor->Modify();
	MapActor->BakedTiles.Reset();
	for (UStaticMesh* Mesh : Meshes)
	{
		MapActor->BakedTiles.Add(Mesh);
	}
	MapActor->BakedTileSignature = Sig;
	MapActor->MarkPackageDirty();
	UE_LOG(LogDoomContent, Display, TEXT("Bake: %s: %d static mesh tiles in %s (signature %08x)"), *MapName, Meshes.Num(), *TileFolder, Sig);
	return bOk;
}

FString DoomContent::MapPackage(const FString& MapName)
{
	return FString(TEXT("/Game/Doom/Maps/")) + MapName.ToUpper();
}

bool DoomContent::ImportMap(const FString& InMapName, bool bOverwrite)
{
	const FString MapName = InMapName.ToUpper();
	FDoomWad* Wad = DoomEditorData::Wad();
	if (!Wad || !Wad->MapNames().Contains(MapName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("Import: no map %s in the WAD"), *MapName);
		return false;
	}
	const FString PkgName = MapPackage(MapName);
	if (FPackageName::DoesPackageExist(PkgName) && !bOverwrite)
	{
		UE_LOG(LogDoomContent, Display, TEXT("Import: %s already exists (use -force to replace it)"), *PkgName);
		return true;
	}
	if (FindPackage(nullptr, *PkgName))
	{
		UE_LOG(LogDoomContent, Error, TEXT("Import: %s is loaded; close it before replacing it"), *PkgName);
		return false;
	}
	UPackage* Pkg = CreatePackage(*PkgName);
	Pkg->MarkAsFullyLoaded(); // replacing a level on disk: this new package is the whole of it
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*MapName), Pkg);
	World->SetFlags(RF_Public | RF_Standalone);

	FActorSpawnParameters SP;
	SP.Name = TEXT("DoomMap");
	ADoomMapActor* MapActor = World->SpawnActor<ADoomMapActor>(ADoomMapActor::StaticClass(), FTransform::Identity, SP);
	MapActor->MapName = MapName;
	MapActor->SetActorLabel(FString::Printf(TEXT("DoomMap %s"), *MapName));
	MapActor->SetFolderPath(TEXT("Doom"));
	MapActor->ImportThings();
	BakeMapTiles(MapActor); // static geometry as tile assets: distance fields in packaged builds

	FAssetRegistryModule::AssetCreated(World);
	Pkg->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(PkgName, FPackageName::GetMapPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Standalone;
	Args.Error = GWarn;
	const bool bOk = UPackage::SavePackage(Pkg, World, *File, Args);
	int32 Markers = 0;
	for (TActorIterator<ADoomThingMarker> It(World); It; ++It) ++Markers;
	UE_LOG(LogDoomContent, Display, TEXT("Import: %s %s (%d thing markers)"), bOk ? TEXT("saved") : TEXT("FAILED to save"), *File, Markers);
	World->DestroyWorld(false);
	World->RemoveFromRoot();
	if (bOk)
	{
		// packaged builds cook the maps listed in the packaging settings: add one line to
		// DefaultGame.ini (written as text so the rest of the file stays as it is)
		const FString Ini = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("DefaultGame.ini"));
		const FString Line = FString::Printf(TEXT("+MapsToCook=(FilePath=\"%s\")"), *PkgName);
		const FString Section = TEXT("[/Script/UnrealEd.ProjectPackagingSettings]");
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Ini) && !Text.Contains(Line))
		{
			const int32 At = Text.Find(Section);
			if (At == INDEX_NONE)
			{
				Text += FString::Printf(TEXT("\r\n%s\r\n%s\r\n"), *Section, *Line);
			}
			else
			{
				Text.InsertAt(At + Section.Len(), TEXT("\r\n") + Line);
			}
			FFileHelper::SaveStringToFile(Text, *Ini);
			UE_LOG(LogDoomContent, Display, TEXT("Import: %s added to the maps to cook"), *PkgName);
		}
	}
	return bOk;
}

// Editor round trip: open an imported level as the editor would, build the preview, edit a
// sector and a marker, and save the result as /Game/Doom/Tests/<MAP>_Edit for -game checks.
bool DoomContent::EditTest(const FString& InMapName)
{
	const FString MapName = InMapName.ToUpper();
	if (!FPackageName::DoesPackageExist(MapPackage(MapName)) && !ImportMap(MapName, false))
	{
		return false;
	}
	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(MapPackage(MapName), FPackageName::GetMapPackageExtension()));
	ADoomMapActor* MapActor = ADoomMapActor::Find(World);
	if (!MapActor)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Edittest: no map actor in %s"), *MapPackage(MapName));
		return false;
	}
	MapActor->RebuildPreview();
	const int32 PreviewComponents = MapActor->NumMeshComponents();
	const FDoomMap* Map = MapActor->GetPreviewMap();
	if (!Map || PreviewComponents == 0)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Edittest: the preview did not build"));
		return false;
	}
	// raise the player start's sector by 48 and brighten it; turn the first imp into a baron
	ADoomThingMarker* Start = nullptr;
	ADoomThingMarker* Imp = nullptr;
	int32 Markers = 0;
	for (TActorIterator<ADoomThingMarker> It(World); It; ++It)
	{
		++Markers;
		if (It->DoomEdNum == 1 && !Start) Start = *It;
		if (It->DoomEdNum == 3001 && !Imp) Imp = *It;
	}
	if (!Start)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Edittest: no player start marker"));
		return false;
	}
	const int32 Sec = MapActor->SectorAtLocation(Start->GetActorLocation());
	const float OldFloor = Map->Sectors[Sec].Floor;
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADoomSectorEdit* Edit = World->SpawnActor<ADoomSectorEdit>(ADoomSectorEdit::StaticClass(), FTransform(Start->GetActorLocation() + FVector(0, 0, 64)), SP);
	Edit->ReadFromSector();
	Edit->bFloor = true;
	Edit->FloorHeight = OldFloor + 48.f;
	Edit->bLight = true;
	Edit->Light = 255;
	Edit->SetActorLabel(TEXT("Raised start"));
	Edit->SetFolderPath(TEXT("Doom/Sector Edits"));
	if (Imp)
	{
		Imp->DoomEdNum = 3003;
		Imp->ThingType = TEXT("Baron");
	}
	// a line edit: the first DR door line loses its special, becomes impassable and gets a new
	// front upper texture (its front side may be shared, so the edit must not touch other lines)
	const int32 Connections = MapActor->NumConnectionLines();
	const int32 Ghosts = MapActor->NumGhostLines();
	int32 DoorLine = INDEX_NONE;
	for (int32 i = 0; i < Map->Lines.Num() && DoorLine < 0; ++i)
	{
		if (Map->Lines[i].Special == 1 && Map->Lines[i].Side[0] != INDEX_NONE) DoorLine = i;
	}
	ADoomLineEdit* LineEdit = nullptr;
	const FName NewTexture = DoomEditorData::Graphics() && DoomEditorData::Graphics()->TextureNames().Num() > 1 ? DoomEditorData::Graphics()->TextureNames()[1] : NAME_None;
	if (DoorLine >= 0)
	{
		const FDoomLine& L = Map->Lines[DoorLine];
		const FVector2f Mid = (Map->V(L.V1) + Map->V(L.V2)) * 0.5f;
		LineEdit = World->SpawnActor<ADoomLineEdit>(ADoomLineEdit::StaticClass(), FTransform(Doom::ToUE(Mid, 32.f)), SP);
		LineEdit->Line = DoorLine;
		LineEdit->ReadFromLine();
		LineEdit->bSpecial = true;
		LineEdit->Special = 0;
		LineEdit->bFlags = true;
		LineEdit->bImpassable = true;
		LineEdit->bFrontUpper = true;
		LineEdit->FrontUpper = NewTexture;
		LineEdit->SetActorLabel(TEXT("Door line made a wall"));
		LineEdit->SetFolderPath(TEXT("Doom/Line Edits"));
	}
	// ... and a tagged special pointed at a tag no sector has: the map checker must name that line
	int32 TaggedLine = INDEX_NONE;
	for (int32 i = 0; i < Map->Lines.Num() && TaggedLine < 0; ++i)
	{
		const int32 Sp = Map->Lines[i].Special;
		if (i != DoorLine && Sp != 0 && Map->Lines[i].Tag != 0 && Sp != 11 && Sp != 51 && Sp != 52 && Sp != 124) TaggedLine = i;
	}
	ADoomLineEdit* BrokenTag = nullptr;
	if (TaggedLine >= 0)
	{
		const FDoomLine& L = Map->Lines[TaggedLine];
		BrokenTag = World->SpawnActor<ADoomLineEdit>(ADoomLineEdit::StaticClass(), FTransform(Doom::ToUE((Map->V(L.V1) + Map->V(L.V2)) * 0.5f, 32.f)), SP);
		BrokenTag->Line = TaggedLine;
		BrokenTag->bTag = true;
		BrokenTag->Tag = 999;
		BrokenTag->SetActorLabel(TEXT("Tag pointing at nothing"));
	}
	MapActor->RebuildPreview();
	const float NewFloor = MapActor->GetPreviewMap()->Sectors[Sec].Floor;
	TArray<TWeakObjectPtr<AActor>> IssueActors;
	const TArray<FDoomMapIssue> Issues = MapActor->RunCheck(IssueActors);
	bool bCheckerOk = TaggedLine < 0;
	for (int32 i = 0; i < Issues.Num(); ++i)
	{
		bCheckerOk |= Issues[i].Line == TaggedLine && Issues[i].Text.Contains(TEXT("tag 999")) && IssueActors[i].Get() == BrokenTag;
	}
	DoomMapCheck::Log(MapName + TEXT("_Edit"), Issues);
	if (BrokenTag)
	{
		World->EditorDestroyActor(BrokenTag, true); // not saved with the test level
		MapActor->RebuildPreview();
	}
	bool bLineOk = DoorLine >= 0;
	FString LineReport = TEXT("no DR door line");
	if (DoorLine >= 0)
	{
		const FDoomMap* After = MapActor->GetPreviewMap();
		const FDoomLine& L = After->Lines[DoorLine];
		const int32 FrontSide = L.Side[0];
		int32 SharedBy = 0;
		for (int32 i = 0; i < After->Lines.Num(); ++i)
		{
			SharedBy += i != DoorLine && (After->Lines[i].Side[0] == FrontSide || After->Lines[i].Side[1] == FrontSide);
		}
		bLineOk = L.Special == 0 && (L.Flags & DoomLineFlags::Blocking) && After->Sides[FrontSide].Upper == NewTexture
			&& !After->SpecialLines.Contains(DoorLine) && SharedBy == 0;
		LineReport = FString::Printf(TEXT("line %d: special %d, impassable %d, front upper %s, side shared by %d other lines"),
			DoorLine, L.Special, (L.Flags & DoomLineFlags::Blocking) != 0, *After->Sides[FrontSide].Upper.ToString(), SharedBy);
	}
	// Export WAD: the edits travel in the PWAD (the raised floor, the walled-up door, the markers' things)
	const FString WadPath = FPaths::ProjectSavedDir() / TEXT("Test") / MapName + TEXT("_Edit.wad");
	MapActor->ExportPath = WadPath;
	MapActor->ExportWad();
	MapActor->ExportPath.Reset();
	FDoomWad Exported;
	FDoomMap Back;
	const bool bExportRead = Exported.AddFile(WadPath) && (Exported.Finalize(), Back.Load(Exported, MapName));
	const FDoomMap* Edited = MapActor->GetPreviewMap();
	const bool bExportOk = bExportRead && Edited && Back.Sectors.IsValidIndex(Sec) && FMath::IsNearlyEqual(Back.Sectors[Sec].Floor, NewFloor)
		&& Back.Things.Num() == Edited->Things.Num() && (DoorLine < 0 || (Back.Lines[DoorLine].Special == 0 && Back.Sides[Back.Lines[DoorLine].Side[0]].Upper == NewTexture));
	// the round trip with a Doom editor: the PWAD joins the WADs, a save "there" (another sector's
	// floor and light changed, written over the file) reloads, and the preview shows it with the
	// level's markers still in charge of the things
	const int32 EditedThings = Edited ? Edited->Things.Num() : -1; // the preview map is replaced below
	bool bRoundTrip = false;
	FString RoundTrip = TEXT("not run");
	if (bExportRead)
	{
		UDoomSettings* Settings = GetMutableDefault<UDoomSettings>();
		const TArray<FString> OldPwads = Settings->PWADs;
		DoomEditorData::UsePwad(WadPath);
		const bool bSource = FPaths::IsSamePath(DoomEditorData::MapSource(MapName), FPaths::ConvertRelativePathToFull(WadPath));
		int32 Other = INDEX_NONE;
		for (int32 s = 0; s < Back.Sectors.Num() && Other == INDEX_NONE; ++s) if (s != Sec) Other = s;
		FDoomMap Changed = Back;
		Changed.Sectors[Other].Floor += 32.f;
		Changed.Sectors[Other].Light = 99;
		DoomWadWriter::SaveMap(Changed, MapName, WadPath);
		IFileManager::Get().SetTimeStamp(*WadPath, FDateTime::UtcNow() - FTimespan::FromSeconds(2));
		const bool bReloaded = DoomEditorData::ReloadIfChanged();
		MapActor->RebuildPreview();
		const FDoomMap* After = MapActor->GetPreviewMap();
		bRoundTrip = bSource && bReloaded && After && After->Sectors[Other].Floor == Back.Sectors[Other].Floor + 32.f && After->Sectors[Other].Light == 99
			&& After->Things.Num() == EditedThings && FMath::IsNearlyEqual(After->Sectors[Sec].Floor, NewFloor);
		RoundTrip = FString::Printf(TEXT("source %s, reloaded %d, sector %d floor %.0f -> %.0f light %d, things %d, edited sector %d floor %.0f"),
			bSource ? TEXT("the PWAD") : TEXT("WRONG"), bReloaded, Other, Back.Sectors[Other].Floor, After ? After->Sectors[Other].Floor : -1.f,
			After ? After->Sectors[Other].Light : -1, After ? After->Things.Num() : -1, Sec, After ? After->Sectors[Sec].Floor : -1.f);
		Settings->PWADs = OldPwads;
		Settings->SaveConfig();
		DoomEditorData::Reload();
		MapActor->RebuildPreview();
	}
	const FString TestPackage = FString(TEXT("/Game/Doom/Tests/")) + MapName + TEXT("_Edit");
	const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, TestPackage);
	const bool bPass = PreviewComponents > 0 && FMath::IsNearlyEqual(NewFloor, OldFloor + 48.f) && Edit->Sector == Sec && bSaved && bLineOk && Connections > 0 && Ghosts > 0 && bCheckerOk && bExportOk && bRoundTrip;
	UE_LOG(LogDoomContent, Display, TEXT("Edittest %s: preview %d components, %d markers, start sector %d floor %.0f -> %.0f; %s; %d connection lines (%d for door and lift travel); the checker %s line %d's broken tag; exported %s (%d things); round trip: %s; saved %s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), PreviewComponents, Markers, Sec, OldFloor, NewFloor, *LineReport, Connections, Ghosts,
		bCheckerOk ? TEXT("found") : TEXT("missed"), TaggedLine, bExportOk ? TEXT("and read back") : TEXT("WRONG"), bExportRead ? Back.Things.Num() : -1, *RoundTrip, *TestPackage);
	return bPass;
}
