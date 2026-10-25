#include "DoomMapActors.h"
#include "DoomMapActorsPrivate.h"
#include "HAL/FileManager.h"
#include "DoomMapGen.h"
#include "DoomWadWriter.h"
#include "DoomWad.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomAssets.h"
#include "DoomSettings.h"
#include "DoomUE.h"
#include "DoomWorld.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Components/BillboardComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/LineBatchComponent.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/Texture2D.h"
#include "Containers/Ticker.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"
#include "DoomMapTiles.h"
#include "UObject/ConstructorHelpers.h"

// ------------------------------------------------------------------------------------------------
// The editor's WAD
// ------------------------------------------------------------------------------------------------

namespace
{
	struct FEditorWad
	{
		TUniquePtr<FDoomWad> Wad;
		TUniquePtr<FDoomGraphics> Gfx;
		FString WadSet;
		bool bTried = false;
		TArray<FDateTime> Stamps; // of Wad's files when they were read
	};

	FEditorWad& EditorWad()
	{
		static FEditorWad E;
		if (!E.bTried)
		{
			E = FEditorWad();
			E.bTried = true;
			const UDoomSettings* Settings = GetDefault<UDoomSettings>();
			auto Resolve = [](const FString& Name)
			{
				return FPaths::FileExists(Name) ? Name : FPaths::Combine(FPaths::ProjectContentDir(), TEXT("WADs"), Name);
			};
			// -iwad= / -file= choose other WADs (baking another IWAD's tile sets)
			FString IWad = Settings->IWAD;
			FParse::Value(FCommandLine::Get(), TEXT("-iwad="), IWad);
			TArray<FString> PWads = Settings->PWADs;
			FString FileArg;
			if (FParse::Value(FCommandLine::Get(), TEXT("-file="), FileArg, false))
			{
				FileArg.ParseIntoArray(PWads, TEXT(","));
			}
			E.WadSet = UDoomMapTiles::WadSetName(IWad, PWads);
			E.Wad = MakeUnique<FDoomWad>();
			if (E.Wad->AddFile(Resolve(IWad)))
			{
				for (const FString& P : PWads)
				{
					E.Wad->AddFile(Resolve(P));
				}
				E.Wad->Finalize();
			}
			if (E.Wad->IsValid())
			{
				E.Gfx = MakeUnique<FDoomGraphics>(*E.Wad);
				for (const FString& F : E.Wad->FilePaths()) E.Stamps.Add(IFileManager::Get().GetTimeStamp(*F));
			}
			else
			{
				UE_LOG(LogDoomEditor, Warning, TEXT("Doom maps in the editor need the IWAD (%s) in Content/WADs"), *Settings->IWAD);
				E.Wad.Reset();
			}
		}
		return E;
	}
}

FDoomWad* DoomEditorData::Wad() { return EditorWad().Wad.Get(); }

void DoomEditorData::Reload()
{
	FEditorWad& E = EditorWad();
	E.bTried = false;
	EditorWad();
}

bool DoomEditorData::ReloadIfChanged()
{
	FEditorWad& E = EditorWad();
	if (!E.Wad)
	{
		return false;
	}
	const TArray<FString>& Files = E.Wad->FilePaths();
	bool bChanged = false;
	for (int32 i = 0; i < Files.Num() && i < E.Stamps.Num(); ++i)
	{
		const FDateTime Now = IFileManager::Get().GetTimeStamp(*Files[i]);
		// a file still being written waits for the next look
		bChanged |= Now != E.Stamps[i] && (FDateTime::UtcNow() - Now).GetTotalSeconds() > 0.5;
	}
	if (!bChanged)
	{
		return false;
	}
	FEditorWad Old = MoveTemp(E);
	E.bTried = false;
	EditorWad();
	if (!E.Wad)
	{
		E = MoveTemp(Old); // unreadable for now (half written?): keep the old and try again later
		return false;
	}
	UE_LOG(LogDoomEditor, Log, TEXT("Reloaded the editor's WADs (%s changed on disk)"), *FString::Join(Files, TEXT(", ")));
	return true;
}

FString DoomEditorData::MapSource(const FString& MapName)
{
	const FDoomWad* W = Wad();
	if (!W)
	{
		return FString();
	}
	const int32 Lump = W->FindLump(MapName.ToUpper());
	return Lump != INDEX_NONE ? W->FilePaths()[W->Lump(Lump).FileIndex] : FString();
}

bool DoomEditorData::IsFromIWAD(const FString& MapName)
{
	const FDoomWad* W = Wad();
	const int32 Lump = W ? W->FindLump(MapName.ToUpper()) : INDEX_NONE;
	return Lump != INDEX_NONE && W->Lump(Lump).FileIndex == 0;
}

void DoomEditorData::UsePwad(const FString& Path)
{
	UDoomSettings* S = GetMutableDefault<UDoomSettings>();
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
	if (!S->PWADs.ContainsByPredicate([&](const FString& P) { return FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(P), Full); }))
	{
		S->PWADs.Add(Full);
		S->SaveConfig();
	}
	Reload();
}
FString DoomEditorData::WadSet() { return EditorWad().WadSet; }
FDoomGraphics* DoomEditorData::Graphics() { return EditorWad().Gfx.Get(); }

TArray<FString> DoomEditorData::MapNames()
{
	return Wad() ? Wad()->MapNames() : TArray<FString>();
}

// ------------------------------------------------------------------------------------------------
// ADoomMapActor
// ------------------------------------------------------------------------------------------------

TFunction<bool(ADoomMapActor*)> ADoomMapActor::BakeHook;
TFunction<void(ADoomMapActor*, const TArray<FDoomMapIssue>&, const TArray<TWeakObjectPtr<AActor>>&)> ADoomMapActor::CheckReportHook;

TArray<FDoomMapIssue> ADoomMapActor::RunCheck(TArray<TWeakObjectPtr<AActor>>& OutActors) const
{
	OutActors.Reset();
	const UWorld* World = GetWorld();
	if (!PreviewMap || !World)
	{
		return {};
	}
	const TArray<FDoomMapIssue> Issues = DoomMapCheck::Run(*PreviewMap, DoomEditorData::Graphics(), MapName);
	// the actors behind the map data: markers in the order ApplyEdits made the things from them, and
	// the line and sector edits by what they edit
	TArray<AActor*> Markers;
	if (bUseThingMarkers)
	{
		for (TActorIterator<ADoomThingMarker> It(const_cast<UWorld*>(World)); It; ++It) Markers.Add(*It);
	}
	TMap<int32, AActor*> LineEdits, SectorEdits;
	for (TActorIterator<ADoomLineEdit> It(const_cast<UWorld*>(World)); It; ++It) LineEdits.Add(It->Line, *It);
	for (TActorIterator<ADoomSectorEdit> It(const_cast<UWorld*>(World)); It; ++It) SectorEdits.Add(PreviewMap->SectorAt(Doom::ToDoom2D(It->GetActorLocation())), *It);
	for (const FDoomMapIssue& I : Issues)
	{
		AActor* A = nullptr;
		if (I.Thing != INDEX_NONE && Markers.IsValidIndex(I.Thing)) A = Markers[I.Thing];
		else if (I.Line != INDEX_NONE) A = LineEdits.FindRef(I.Line);
		else if (I.Sector != INDEX_NONE) A = SectorEdits.FindRef(I.Sector);
		OutActors.Add(A);
	}
	return Issues;
}

bool ADoomMapActor::LoadEditedMap(FDoomMap& Out) const
{
	FDoomWad* W = DoomEditorData::Wad();
	if (!W)
	{
		return false;
	}
	uint32 GenSeed = 0;
	const bool bLoaded = DoomMapGen::ParseName(MapName, GenSeed)
		? DoomMapGen::Generate(DoomMapGen::DefaultOptions(GenSeed, W->IsDoom2()), DoomEditorData::Graphics(), Out)
		: Out.Load(*W, MapName);
	if (bLoaded)
	{
		ApplyEdits(Out);
	}
	return bLoaded;
}

void ADoomMapActor::ExportWad()
{
	FDoomMap M;
	if (!LoadEditedMap(M))
	{
		UE_LOG(LogDoomEditor, Error, TEXT("Export WAD: no map %s"), *MapName);
		return;
	}
	uint32 GenSeed = 0;
	const bool bGenerated = DoomMapGen::ParseName(MapName, GenSeed);
	const FString Slot = !ExportSlot.IsEmpty() ? ExportSlot.ToUpper()
		: bGenerated ? FString(DoomEditorData::Wad()->IsDoom2() ? TEXT("MAP01") : TEXT("E1M1")) : MapName.ToUpper();
	const FString Path = !ExportPath.IsEmpty() ? ExportPath : FPaths::ProjectSavedDir() / TEXT("Maps") / MapName.ToUpper() + TEXT(".wad");
	FString Note;
	if (DoomWadWriter::SaveMap(M, Slot, Path, &Note))
	{
		UE_LOG(LogDoomEditor, Display, TEXT("Export WAD: %s as %s in %s (%s). Play it with -file=\"%s\" -warp=%s"),
			*MapName, *Slot, *FPaths::ConvertRelativePathToFull(Path), *Note, *FPaths::ConvertRelativePathToFull(Path), *Slot);
	}
}

void ADoomMapActor::EditInDoomBuilder()
{
	uint32 Seed = 0;
	if (DoomMapGen::ParseName(MapName, Seed) || DoomEditorData::IsFromIWAD(MapName))
	{
		// edit a PWAD, never the IWAD: this level's map, edits and all, becomes one
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Maps") / MapName.ToUpper() + TEXT(".wad"));
		const FString OldPath = ExportPath, OldSlot = ExportSlot;
		ExportPath = Path;
		ExportSlot = MapName.ToUpper();
		ExportWad();
		ExportPath = OldPath;
		ExportSlot = OldSlot;
		if (!FPaths::FileExists(Path))
		{
			return;
		}
		DoomEditorData::UsePwad(Path);
	}
	const FString Source = DoomEditorData::MapSource(MapName);
	const FString Builder = GetDefault<UDoomSettings>()->DoomBuilderPath.FilePath;
	if (Builder.IsEmpty() || !FPaths::FileExists(Builder))
	{
		UE_LOG(LogDoomEditor, Display, TEXT("Edit in Doom Builder: open %s (map %s) in your Doom editor; set Project Settings > Doom > Doom Builder Path to launch it from here. Saving there updates this level."), *Source, *MapName.ToUpper());
		return;
	}
	const FString Args = FString::Printf(TEXT("\"%s\" -map %s"), *Source, *MapName.ToUpper());
	FPlatformProcess::CreateProc(*Builder, *Args, true, false, false, nullptr, 0, nullptr, nullptr);
	UE_LOG(LogDoomEditor, Display, TEXT("Edit in Doom Builder: %s %s"), *Builder, *Args);
}

void ADoomMapActor::CheckMap()
{
	if (!PreviewMap)
	{
		RebuildPreview();
	}
	TArray<TWeakObjectPtr<AActor>> Actors;
	const TArray<FDoomMapIssue> Issues = RunCheck(Actors);
	DoomMapCheck::Log(MapName, Issues);
	if (CheckReportHook)
	{
		CheckReportHook(this, Issues, Actors);
	}
}

void ADoomMapActor::BakeStaticMeshes()
{
	if (BakeHook)
	{
		BakeHook(this);
	}
}

ADoomMapActor::ADoomMapActor()
{
	PrimaryActorTick.bCanEverTick = false;
	bEditorPreview = true;
}

ADoomMapActor* ADoomMapActor::Find(const UWorld* World)
{
	if (World)
	{
		for (TActorIterator<ADoomMapActor> It(const_cast<UWorld*>(World)); It; ++It)
		{
			return *It;
		}
	}
	return nullptr;
}

void ADoomMapActor::BeginPlay()
{
	Super::BeginPlay();
	// the game builds its own geometry (with collision, movers and baking) through UDoomWorld
	ClearGeometry();
	PreviewMap.Reset();
	if (Overlay)
	{
		Overlay->Flush();
	}
}

void ADoomMapActor::ApplyEdits(FDoomMap& InMap) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	int32 Edits = 0;
	for (TActorIterator<ADoomSectorEdit> It(const_cast<UWorld*>(World)); It; ++It)
	{
		It->Apply(InMap);
		++Edits;
	}
	int32 LineEdits = 0;
	for (TActorIterator<ADoomLineEdit> It(const_cast<UWorld*>(World)); It; ++It)
	{
		It->Apply(InMap);
		++LineEdits;
	}
	if (LineEdits > 0)
	{
		// specials may have been added or removed: the lines that crossings check
		InMap.SpecialLines.Reset();
		for (int32 i = 0; i < InMap.Lines.Num(); ++i)
		{
			if (InMap.Lines[i].Special != 0) InMap.SpecialLines.Add(i);
		}
	}
	int32 Markers = 0;
	if (bUseThingMarkers)
	{
		TArray<FDoomMapThing> Things;
		for (TActorIterator<ADoomThingMarker> It(const_cast<UWorld*>(World)); It; ++It)
		{
			Things.Add(It->ToThing());
		}
		Markers = Things.Num();
		if (Markers > 0)
		{
			InMap.Things = MoveTemp(Things);
		}
	}
	UE_LOG(LogDoomEditor, Log, TEXT("%s: %d sector edits, %d line edits, %d thing markers from the level"), *MapName, Edits, LineEdits, Markers);
}

int32 ADoomMapActor::SectorAtLocation(const FVector& Location) const
{
	return PreviewMap ? PreviewMap->SectorAt(Doom::ToDoom2D(Location)) : INDEX_NONE;
}

int32 ADoomMapActor::NumConnectionLines() const
{
	return Overlay ? Overlay->BatchedLines.Num() : 0;
}

int32 ADoomMapActor::LineNearLocation(const FVector& Location) const
{
	if (!PreviewMap)
	{
		return INDEX_NONE;
	}
	const FVector2f P = Doom::ToDoom2D(Location);
	int32 Best = INDEX_NONE;
	float BestDist = TNumericLimits<float>::Max();
	for (int32 i = 0; i < PreviewMap->Lines.Num(); ++i)
	{
		const float D = PreviewMap->DistanceToLine(P, i);
		if (D < BestDist)
		{
			BestDist = D;
			Best = i;
		}
	}
	return Best;
}

void ADoomMapActor::RequestPreview()
{
#if WITH_EDITOR
	if (bPreviewQueued || !GetWorld() || GetWorld()->IsGameWorld())
	{
		return;
	}
	bPreviewQueued = true;
	TWeakObjectPtr<ADoomMapActor> Weak(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
	{
		if (ADoomMapActor* A = Weak.Get())
		{
			A->bPreviewQueued = false;
			A->RebuildPreview();
		}
		return false; // once
	}));
#endif
}

void ADoomMapActor::RebuildPreview()
{
	ClearGeometry();
	PreviewMap.Reset();
	UWorld* World = GetWorld();
	FDoomWad* W = DoomEditorData::Wad();
	FDoomGraphics* G = DoomEditorData::Graphics();
	if (!World || World->IsGameWorld() || !W || !G)
	{
		return;
	}
	PreviewMap = MakeUnique<FDoomMap>();
	uint32 GenSeed = 0;
	const bool bLoaded = DoomMapGen::ParseName(MapName, GenSeed)
		? DoomMapGen::Generate(DoomMapGen::DefaultOptions(GenSeed, W->IsDoom2()), G, *PreviewMap)
		: PreviewMap->Load(*W, MapName);
	if (!bLoaded)
	{
		UE_LOG(LogDoomEditor, Warning, TEXT("No map %s in the WAD"), *MapName);
		PreviewMap.Reset();
		return;
	}
	if (Overlay)
	{
		Overlay->Flush();
	}
	if (!bShowPreview)
	{
		return; // keep the map data for sector lookups only
	}
	ApplyEdits(*PreviewMap);
	// the editor world has no UDoomWorld ticking the shader globals: set the ones the preview needs
	if (UMaterialParameterCollection* MPC = DoomAssets::Collection())
	{
		if (UMaterialParameterCollectionInstance* I = World->GetParameterCollectionInstance(MPC))
		{
			I->SetScalarParameterValue(TEXT("PaletteRows"), (float)G->ColormapRows());
			I->SetScalarParameterValue(TEXT("Palette"), 0.f);
			I->SetScalarParameterValue(TEXT("Modern"), bPreviewModern ? 1.f : 0.f);
		}
	}
	// the same chunks as the game, so the static chunk (and its baked tiles) match exactly
	TSet<int32> Movable;
	UDoomWorld::ComputeMovableSectors(*PreviewMap, Movable);
	Build(*PreviewMap, *G, Movable, bPreviewModern, FDoomGraphics::SkyTextureFor(MapName));
	if (bShowConnections)
	{
		DrawConnections(*PreviewMap);
	}
}

namespace
{
	enum class ESpecialKind : uint8 { None, Door, Lift, Teleport, Crusher, Floor, Light, Exit };

	/** What a vanilla line special does, roughly, for the viewport overlay. */
	ESpecialKind KindOf(int32 Special)
	{
		static const TSet<int32> Doors = { 1, 2, 3, 4, 16, 26, 27, 28, 29, 31, 32, 33, 34, 42, 46, 50, 61, 63, 75, 76, 86, 90, 99,
			103, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 133, 134, 135, 136, 137 };
		static const TSet<int32> Lifts = { 10, 14, 15, 20, 21, 22, 47, 53, 54, 62, 66, 67, 68, 87, 88, 89, 95, 120, 121, 122, 123 };
		static const TSet<int32> Teleports = { 39, 97, 125, 126 };
		static const TSet<int32> Crushers = { 6, 25, 40, 41, 43, 44, 49, 57, 72, 73, 74, 77, 141 };
		static const TSet<int32> Lights = { 12, 13, 17, 35, 79, 80, 81, 104, 138, 139 };
		static const TSet<int32> Exits = { 11, 51, 52, 124 };
		if (Special == 0) return ESpecialKind::None;
		if (Doors.Contains(Special)) return ESpecialKind::Door;
		if (Lifts.Contains(Special)) return ESpecialKind::Lift;
		if (Teleports.Contains(Special)) return ESpecialKind::Teleport;
		if (Crushers.Contains(Special)) return ESpecialKind::Crusher;
		if (Lights.Contains(Special)) return ESpecialKind::Light;
		if (Exits.Contains(Special)) return ESpecialKind::Exit;
		return ESpecialKind::Floor; // floors, ceilings, stairs, donuts
	}

	FLinearColor ColorOf(ESpecialKind K)
	{
		switch (K)
		{
		case ESpecialKind::Door: return FLinearColor(0.15f, 0.45f, 1.f);
		case ESpecialKind::Lift: return FLinearColor(0.f, 0.9f, 0.9f);
		case ESpecialKind::Teleport: return FLinearColor(1.f, 0.35f, 0.75f);
		case ESpecialKind::Crusher: return FLinearColor(1.f, 0.15f, 0.1f);
		case ESpecialKind::Light: return FLinearColor(1.f, 0.95f, 0.3f);
		case ESpecialKind::Exit: return FLinearColor(1.f, 1.f, 1.f);
		default: return FLinearColor(1.f, 0.55f, 0.1f);
		}
	}

	bool IsManualDoor(int32 Special)
	{
		return Special == 1 || Special == 26 || Special == 27 || Special == 28 || Special == 31 || Special == 32
			|| Special == 33 || Special == 34 || Special == 117 || Special == 118;
	}
}

void ADoomMapActor::DrawConnections(const FDoomMap& InMap)
{
#if WITH_EDITOR
	if (!Overlay)
	{
		Overlay = NewObject<ULineBatchComponent>(this, TEXT("Connections"), RF_Transient);
		Overlay->SetupAttachment(RootComponent);
		Overlay->bIsEditorOnly = true;
		Overlay->SetHiddenInGame(true);
		Overlay->RegisterComponent();
	}
	Overlay->Flush();
	TArray<FBatchedLine> Lines;
	const float S = Doom::Scale;
	auto Outline = [&](int32 Sector, const FLinearColor& C)
	{
		const FDoomSector& Sec = InMap.Sectors[Sector];
		for (int32 L : Sec.Lines)
		{
			const FDoomLine& Li = InMap.Lines[L];
			Lines.Emplace(Doom::ToUE(InMap.V(Li.V1), Sec.Floor + 2.f), Doom::ToUE(InMap.V(Li.V2), Sec.Floor + 2.f), C, 0.f, 3.f * S, SDPG_World);
		}
	};
	// what happens in each sector: the strongest kind of special that works on it
	TArray<ESpecialKind> SectorKind;
	SectorKind.Init(ESpecialKind::None, InMap.Sectors.Num());
	TMultiMap<int32, int32> ByTag;
	for (int32 s = 0; s < InMap.Sectors.Num(); ++s)
	{
		if (InMap.Sectors[s].Tag != 0) ByTag.Add(InMap.Sectors[s].Tag, s);
	}
	for (int32 i = 0; i < InMap.Lines.Num(); ++i)
	{
		const FDoomLine& L = InMap.Lines[i];
		const ESpecialKind K = KindOf(L.Special);
		if (K == ESpecialKind::None || K == ESpecialKind::Exit)
		{
			continue;
		}
		const FVector2f Mid = (InMap.V(L.V1) + InMap.V(L.V2)) * 0.5f;
		const float MidZ = L.FrontSector != INDEX_NONE ? InMap.Sectors[L.FrontSector].Floor + 32.f : 32.f;
		if (IsManualDoor(L.Special))
		{
			if (L.BackSector != INDEX_NONE) SectorKind[L.BackSector] = K; // the door is the sector behind
			continue;
		}
		TArray<int32> Targets;
		ByTag.MultiFind(L.Tag, Targets);
		for (int32 T : Targets)
		{
			const FDoomSector& Sec = InMap.Sectors[T];
			// the trigger to the sector it works on, drawn over the walls
			Lines.Emplace(Doom::ToUE(Mid, MidZ), Doom::ToUE(Sec.Center, Sec.Floor + 32.f), ColorOf(K), 0.f, 2.f * S, SDPG_Foreground);
			SectorKind[T] = K;
		}
		if (Targets.Num() == 0 && L.Tag != 0)
		{
			// a tag that points at nothing: a short red cross on the trigger
			const FVector C = Doom::ToUE(Mid, MidZ);
			Lines.Emplace(C + FVector(-16, -16, 0) * S, C + FVector(16, 16, 0) * S, FLinearColor::Red, 0.f, 3.f * S, SDPG_Foreground);
			Lines.Emplace(C + FVector(-16, 16, 0) * S, C + FVector(16, -16, 0) * S, FLinearColor::Red, 0.f, 3.f * S, SDPG_Foreground);
		}
	}
	for (int32 s = 0; s < InMap.Sectors.Num(); ++s)
	{
		const int32 Special = InMap.Sectors[s].Special;
		if (Special == 9)
		{
			Outline(s, FLinearColor(0.85f, 0.2f, 1.f)); // secret
		}
		else if (Special == 4 || Special == 5 || Special == 7 || Special == 11 || Special == 16)
		{
			Outline(s, FLinearColor(0.2f, 1.f, 0.2f)); // damaging floor
		}
		else if (SectorKind[s] != ESpecialKind::None)
		{
			Outline(s, ColorOf(SectorKind[s]));
		}
	}
	// ghosts of where doors and lifts travel (Doom's rules: a door opens to the lowest neighbouring
	// ceiling - 4, a lift lowers to the lowest neighbouring floor): the outline at that height, in a
	// dimmer colour, with a post at each corner from the plane now to the plane then; drawn over the
	// walls, since a lowered lift is under the floor and an open door's top inside the lintel
	GhostLines = 0;
	auto Ghost = [&](int32 Sector, float From, float To, const FLinearColor& C)
	{
		if (FMath::Abs(To - From) < 1.f)
		{
			return;
		}
		const FLinearColor Dim = C * 0.6f;
		const FDoomSector& Sec = InMap.Sectors[Sector];
		TSet<int32> Corners;
		for (int32 L : Sec.Lines)
		{
			const FDoomLine& Li = InMap.Lines[L];
			Lines.Emplace(Doom::ToUE(InMap.V(Li.V1), To), Doom::ToUE(InMap.V(Li.V2), To), Dim, 0.f, 1.5f * S, SDPG_Foreground);
			++GhostLines;
			Corners.Add(Li.V1);
			Corners.Add(Li.V2);
		}
		for (int32 V : Corners)
		{
			Lines.Emplace(Doom::ToUE(InMap.V(V), From), Doom::ToUE(InMap.V(V), To), Dim, 0.f, 1.f * S, SDPG_Foreground);
			++GhostLines;
		}
	};
	for (int32 s = 0; s < InMap.Sectors.Num(); ++s)
	{
		const FDoomSector& Sec = InMap.Sectors[s];
		if (SectorKind[s] == ESpecialKind::Door)
		{
			const float Open = InMap.LowestCeilingSurrounding(s) - 4.f;
			if (Open > Sec.Ceiling) Ghost(s, Sec.Ceiling, Open, ColorOf(ESpecialKind::Door));
		}
		else if (SectorKind[s] == ESpecialKind::Lift)
		{
			const float Low = InMap.LowestFloorSurrounding(s);
			if (Low < Sec.Floor) Ghost(s, Sec.Floor, Low, ColorOf(ESpecialKind::Lift));
		}
	}
	Overlay->DrawLines(Lines);
#endif
}

void ADoomMapActor::ImportThings()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	FDoomWad* W = DoomEditorData::Wad();
	if (!World || !W)
	{
		return;
	}
	FDoomMap Fresh;
	if (!Fresh.Load(*W, MapName))
	{
		return;
	}
	TArray<ADoomThingMarker*> Old;
	for (TActorIterator<ADoomThingMarker> It(World); It; ++It)
	{
		Old.Add(*It);
	}
	for (ADoomThingMarker* M : Old)
	{
		M->Modify();
		World->EditorDestroyActor(M, true);
	}
	for (const FDoomMapThing& T : Fresh.Things)
	{
		const int32 Sec = Fresh.SectorAt(T.P);
		const FVector Feet = Doom::ToUE(T.P, Sec >= 0 ? Fresh.Sectors[Sec].Floor : 0);
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ADoomThingMarker* M = World->SpawnActor<ADoomThingMarker>(ADoomThingMarker::StaticClass(), FTransform(FRotator(0, Doom::AngleToYaw(T.Angle), 0), Feet), SP);
		if (M)
		{
			M->FromThing(T);
			M->SetActorLabel(ADoomThingMarker::TypeName(T.Type));
			M->SetFolderPath(ADoomThingMarker::FolderFor(T.Type));
		}
	}
	UE_LOG(LogDoomEditor, Log, TEXT("%s: %d thing markers"), *MapName, Fresh.Things.Num());
	RequestPreview();
#endif
}

void ADoomMapActor::AddLineEdit()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World || !PreviewMap)
	{
		return;
	}
	const FVector View = World->ViewLocationsRenderedLastFrame.Num() ? World->ViewLocationsRenderedLastFrame[0] : GetActorLocation();
	const int32 Li = LineNearLocation(View);
	if (Li < 0)
	{
		return;
	}
	const FDoomLine& L = PreviewMap->Lines[Li];
	const FVector2f Mid = (PreviewMap->V(L.V1) + PreviewMap->V(L.V2)) * 0.5f;
	const float Floor = L.FrontSector != INDEX_NONE ? PreviewMap->Sectors[L.FrontSector].Floor : 0.f;
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (ADoomLineEdit* E = World->SpawnActor<ADoomLineEdit>(ADoomLineEdit::StaticClass(), FTransform(Doom::ToUE(Mid, Floor + 32.f)), SP))
	{
		E->Line = Li;
		E->ReadFromLine();
		E->SetActorLabel(FString::Printf(TEXT("Line %d"), Li));
		E->SetFolderPath(TEXT("Doom/Line Edits"));
	}
#endif
}

void ADoomMapActor::AddSectorEdit()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World || !PreviewMap)
	{
		return;
	}
	const FVector View = World->ViewLocationsRenderedLastFrame.Num() ? World->ViewLocationsRenderedLastFrame[0] : GetActorLocation();
	int32 Sec = SectorAtLocation(View);
	if (Sec < 0)
	{
		Sec = 0;
	}
	const FDoomSector& S = PreviewMap->Sectors[Sec];
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (ADoomSectorEdit* E = World->SpawnActor<ADoomSectorEdit>(ADoomSectorEdit::StaticClass(), FTransform(Doom::ToUE(S.Center, S.Floor + 32.f)), SP))
	{
		E->ReadFromSector();
		E->SetActorLabel(FString::Printf(TEXT("Sector %d"), Sec));
		E->SetFolderPath(TEXT("Doom/Sector Edits"));
	}
#endif
}

#if WITH_EDITOR
void ADoomMapActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// opening the level (or placing the actor) shows the map
	if (!PreviewMap && GetWorld() && !GetWorld()->IsGameWorld() && !HasAnyFlags(RF_ClassDefaultObject))
	{
		RequestPreview();
	}
}

void ADoomMapActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	RequestPreview();
}

void ADoomMapActor::BeginDestroy()
{
	PreviewMap.Reset();
	Super::BeginDestroy();
}
#endif
