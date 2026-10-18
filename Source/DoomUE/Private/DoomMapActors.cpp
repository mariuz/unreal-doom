#include "DoomMapActors.h"
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
	};

	FEditorWad& EditorWad()
	{
		static FEditorWad E;
		if (!E.bTried)
		{
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
			}
			else
			{
				UE_LOG(LogDoom, Warning, TEXT("Doom maps in the editor need the IWAD (%s) in Content/WADs"), *Settings->IWAD);
				E.Wad.Reset();
			}
		}
		return E;
	}

	constexpr int32 PlayerStartTypes[] = { 1, 2, 3, 4, 11 };
	const TCHAR* PlayerStartNames[] = { TEXT("Player1Start"), TEXT("Player2Start"), TEXT("Player3Start"), TEXT("Player4Start"), TEXT("DeathmatchStart") };

	float FloorAt(const FDoomMap* Map, const FVector& Location)
	{
		const int32 Sec = Map ? Map->SectorAt(Doom::ToDoom2D(Location)) : INDEX_NONE;
		return Sec >= 0 ? Map->Sectors[Sec].Floor : Location.Z / Doom::Scale;
	}
}

FDoomWad* DoomEditorData::Wad() { return EditorWad().Wad.Get(); }
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
	UE_LOG(LogDoom, Log, TEXT("%s: %d sector edits, %d line edits, %d thing markers from the level"), *MapName, Edits, LineEdits, Markers);
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
	if (!PreviewMap->Load(*W, MapName))
	{
		UE_LOG(LogDoom, Warning, TEXT("No map %s in the WAD"), *MapName);
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
	UE_LOG(LogDoom, Log, TEXT("%s: %d thing markers"), *MapName, Fresh.Things.Num());
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

// ------------------------------------------------------------------------------------------------
// ADoomThingMarker
// ------------------------------------------------------------------------------------------------

ADoomThingMarker::ADoomThingMarker()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Facing = CreateDefaultSubobject<UArrowComponent>(TEXT("Facing"));
	Facing->SetupAttachment(RootComponent);
	Facing->ArrowSize = 0.5f;
	Facing->SetHiddenInGame(true);
	Facing->bIsEditorOnly = true;
	ThingType = TEXT("Imp");
	SetHidden(true); // markers only carry data in the game
}

FDoomMapThing ADoomThingMarker::ToThing() const
{
	FDoomMapThing T;
	T.P = Doom::ToDoom2D(GetActorLocation());
	T.Angle = FRotator::NormalizeAxis(Doom::YawToAngle(GetActorRotation().Yaw));
	if (T.Angle < 0) T.Angle += 360.f;
	T.Type = DoomEdNum;
	T.Flags = (bEasy ? 1 : 0) | (bMedium ? 2 : 0) | (bHard ? 4 : 0) | (bAmbush ? 8 : 0) | (bMultiplayerOnly ? 16 : 0);
	return T;
}

void ADoomThingMarker::FromThing(const FDoomMapThing& T)
{
	DoomEdNum = T.Type;
	ThingType = FName(*TypeName(T.Type));
	bEasy = (T.Flags & 1) != 0;
	bMedium = (T.Flags & 2) != 0;
	bHard = (T.Flags & 4) != 0;
	bAmbush = (T.Flags & 8) != 0;
	bMultiplayerOnly = (T.Flags & 16) != 0;
	UpdateVisual();
}

TArray<FString> ADoomThingMarker::GetThingTypeNames()
{
	TArray<FString> Names;
	for (const TCHAR* N : PlayerStartNames)
	{
		Names.Add(N);
	}
	for (const FDoomThingDef& D : DoomInfo::AllThings())
	{
		Names.AddUnique(D.Name);
	}
	return Names;
}

FString ADoomThingMarker::TypeName(int32 EdNum)
{
	for (int32 i = 0; i < UE_ARRAY_COUNT(PlayerStartTypes); ++i)
	{
		if (PlayerStartTypes[i] == EdNum) return PlayerStartNames[i];
	}
	const FDoomThingDef* D = DoomInfo::FindThing(EdNum);
	return D ? FString(D->Name) : FString::Printf(TEXT("Thing%d"), EdNum);
}

FName ADoomThingMarker::FolderFor(int32 EdNum)
{
	for (int32 T : PlayerStartTypes)
	{
		if (T == EdNum) return TEXT("Doom/Starts");
	}
	const FDoomThingDef* D = DoomInfo::FindThing(EdNum);
	if (!D) return TEXT("Doom/Other");
	if (D->Flags & EThingFlag::Monster) return TEXT("Doom/Monsters");
	if (D->Flags & EThingFlag::Pickup) return TEXT("Doom/Items");
	return TEXT("Doom/Decorations");
}

void ADoomThingMarker::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateVisual();
}

void ADoomThingMarker::UpdateVisual()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World || World->IsGameWorld() || HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}
	if (!Sprite)
	{
		// transient: its texture is generated from the WAD each session and must not be saved
		Sprite = NewObject<UBillboardComponent>(this, NAME_None, RF_Transient);
		Sprite->SetupAttachment(RootComponent);
		Sprite->bIsEditorOnly = true;
		Sprite->SetHiddenInGame(true);
		Sprite->bIsScreenSizeScaled = false;
		Sprite->RegisterComponent();
	}
	const FDoomThingDef* D = DoomInfo::FindThing(DoomEdNum);
	FDoomGraphics* G = DoomEditorData::Graphics();
	const FDoomPicture* Pic = nullptr;
	if (G)
	{
		const FString Prefix = D ? DoomInfo::SpriteName(D->Sprite) : TEXT("PLAY");
		const TCHAR Frame = (D && !D->Idle.IsEmpty()) ? D->Idle[0] : TEXT('A');
		for (int32 Rot : { 1, 0 })
		{
			const FDoomSpriteFrame F = G->SpriteFrame(Prefix, Frame, Rot);
			if (F.Picture && F.Picture->Texture)
			{
				Pic = F.Picture;
				break;
			}
		}
	}
	if (Pic)
	{
		Sprite->SetSprite(Pic->Texture);
		Sprite->SetRelativeScale3D(FVector(Doom::Scale));
		// Doom hangs the picture TopOffset pixels above the feet; the billboard is centred
		Sprite->SetRelativeLocation(FVector(0, 0, (Pic->TopOffset - Pic->Height * 0.5f) * Doom::Scale));
	}
	else
	{
		Sprite->SetSprite(LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EditorResources/S_Note.S_Note")));
		Sprite->SetRelativeScale3D(FVector(1.f));
		Sprite->SetRelativeLocation(FVector(0, 0, 32.f));
	}
	if (ADoomMapActor* MapActor = ADoomMapActor::Find(World))
	{
		// feet on the floor of the sector the marker stands in, as Doom places things
		if (const FDoomMap* Map = MapActor->GetPreviewMap())
		{
			FVector L = GetActorLocation();
			const float Z = FloorAt(Map, L) * Doom::Scale;
			if (!FMath::IsNearlyEqual(L.Z, Z, 0.5f))
			{
				L.Z = Z;
				SetActorLocation(L);
			}
		}
	}
#endif
}

#if WITH_EDITOR
void ADoomThingMarker::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	const FName Prop = Event.GetPropertyName();
	if (Prop == GET_MEMBER_NAME_CHECKED(ADoomThingMarker, ThingType))
	{
		const FString Wanted = ThingType.ToString();
		for (int32 i = 0; i < UE_ARRAY_COUNT(PlayerStartTypes); ++i)
		{
			if (Wanted == PlayerStartNames[i]) DoomEdNum = PlayerStartTypes[i];
		}
		if (const FDoomThingDef* D = DoomInfo::FindThingByName(Wanted))
		{
			DoomEdNum = D->DoomEdNum;
		}
	}
	else if (Prop == GET_MEMBER_NAME_CHECKED(ADoomThingMarker, DoomEdNum))
	{
		ThingType = FName(*TypeName(DoomEdNum));
	}
	if (Prop == GET_MEMBER_NAME_CHECKED(ADoomThingMarker, ThingType) || Prop == GET_MEMBER_NAME_CHECKED(ADoomThingMarker, DoomEdNum))
	{
		SetActorLabel(TypeName(DoomEdNum));
		SetFolderPath(FolderFor(DoomEdNum));
	}
	Super::PostEditChangeProperty(Event);
	UpdateVisual();
}

void ADoomThingMarker::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished)
	{
		UpdateVisual();
	}
}
#endif

// ------------------------------------------------------------------------------------------------
// ADoomSectorEdit
// ------------------------------------------------------------------------------------------------

ADoomSectorEdit::ADoomSectorEdit()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(RootComponent);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetWorldSize(48.f);
	Label->SetTextRenderColor(FColor(255, 200, 40));
	Label->SetHiddenInGame(true);
	Label->bIsEditorOnly = true;
	SetHidden(true);
#if WITH_EDITORONLY_DATA
	if (UBillboardComponent* Icon = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Icon")))
	{
		Icon->SetupAttachment(RootComponent);
		static ConstructorHelpers::FObjectFinder<UTexture2D> Note(TEXT("/Engine/EditorResources/S_Note"));
		if (Note.Succeeded()) Icon->SetSprite(Note.Object);
	}
#endif
}

void ADoomSectorEdit::Apply(FDoomMap& Map) const
{
	const int32 S = Map.SectorAt(Doom::ToDoom2D(GetActorLocation()));
	if (!Map.Sectors.IsValidIndex(S))
	{
		return;
	}
	FDoomSector& Sec = Map.Sectors[S];
	if (bFloor) Sec.Floor = FloorHeight;
	if (bCeiling) Sec.Ceiling = CeilingHeight;
	if (bLight) Sec.Light = Sec.OriginalLight = (int16)FMath::Clamp(Light, 0, 255);
	if (bFloorFlat && !FloorFlat.IsNone()) Sec.FloorPic = FloorFlat;
	if (bCeilingFlat && !CeilingFlat.IsNone()) Sec.CeilingPic = CeilingFlat;
	if (bSpecial) Sec.Special = (int16)Special;
	if (bTag) Sec.Tag = (int16)Tag;
}

void ADoomSectorEdit::ReadFromSector()
{
	Refresh(false);
	const ADoomMapActor* MapActor = ADoomMapActor::Find(GetWorld());
	const FDoomMap* Map = MapActor ? MapActor->GetPreviewMap() : nullptr;
	if (!Map || !Map->Sectors.IsValidIndex(Sector))
	{
		return;
	}
	// the preview map already has this actor's overrides applied; read the WAD's own values
	FDoomMap Fresh;
	const FDoomSector* S = &Map->Sectors[Sector];
	if (DoomEditorData::Wad() && Fresh.Load(*DoomEditorData::Wad(), MapActor->MapName) && Fresh.Sectors.IsValidIndex(Sector))
	{
		S = &Fresh.Sectors[Sector];
	}
	Modify();
	FloorHeight = S->Floor;
	CeilingHeight = S->Ceiling;
	Light = S->Light;
	FloorFlat = S->FloorPic;
	CeilingFlat = S->CeilingPic;
	Special = S->Special;
	Tag = S->Tag;
}

TArray<FString> ADoomSectorEdit::GetFlatNames()
{
	TArray<FString> Names;
	if (FDoomGraphics* G = DoomEditorData::Graphics())
	{
		for (FName N : G->FlatNames())
		{
			Names.Add(N.ToString());
		}
	}
	return Names;
}

void ADoomSectorEdit::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Refresh(false);
}

void ADoomSectorEdit::Refresh(bool bRebuild)
{
	// editor feedback only (the label is an editor-only component, stripped from game worlds)
	if (!GetWorld() || GetWorld()->IsGameWorld() || !Label)
	{
		return;
	}
	ADoomMapActor* MapActor = ADoomMapActor::Find(GetWorld());
	if (MapActor && MapActor->GetPreviewMap())
	{
		Sector = MapActor->SectorAtLocation(GetActorLocation());
	}
	Label->SetText(FText::FromString(Sector >= 0 ? FString::Printf(TEXT("Sector %d"), Sector) : TEXT("outside the map")));
	if (bRebuild && MapActor)
	{
		MapActor->RequestPreview();
	}
}

#if WITH_EDITOR
void ADoomSectorEdit::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	Refresh(true);
}

void ADoomSectorEdit::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished)
	{
		Refresh(true);
	}
}
#endif

// ------------------------------------------------------------------------------------------------
// ADoomLineEdit
// ------------------------------------------------------------------------------------------------

ADoomLineEdit::ADoomLineEdit()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(RootComponent);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetWorldSize(40.f);
	Label->SetTextRenderColor(FColor(120, 220, 255));
	Label->SetHiddenInGame(true);
	Label->bIsEditorOnly = true;
	SetHidden(true);
#if WITH_EDITORONLY_DATA
	if (UBillboardComponent* Icon = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Icon")))
	{
		Icon->SetupAttachment(RootComponent);
		static ConstructorHelpers::FObjectFinder<UTexture2D> Note(TEXT("/Engine/EditorResources/S_Note"));
		if (Note.Succeeded()) Icon->SetSprite(Note.Object);
	}
#endif
}

void ADoomLineEdit::Apply(FDoomMap& Map) const
{
	if (!Map.Lines.IsValidIndex(Line))
	{
		return;
	}
	FDoomLine& L = Map.Lines[Line];
	if (bSpecial) L.Special = (int16)Special;
	if (bTag) L.Tag = (int16)Tag;
	if (bFlags)
	{
		using namespace DoomLineFlags;
		uint16 F = L.Flags & ~(Blocking | BlockMonsters | Secret | SoundBlock | DontDraw | DontPegTop | DontPegBottom);
		if (bImpassable) F |= Blocking;
		if (bBlockMonsters) F |= BlockMonsters;
		if (bSecret) F |= Secret;
		if (bBlockSound) F |= SoundBlock;
		if (bNotOnAutomap) F |= DontDraw;
		if (bUpperUnpegged) F |= DontPegTop;
		if (bLowerUnpegged) F |= DontPegBottom;
		L.Flags = F;
	}
	// a side shared with other lines (WAD packing) gets its own copy before its textures change
	auto Side = [&Map, this](int32 s) -> FDoomSide*
	{
		int32 Si = Map.Lines[Line].Side[s];
		if (!Map.Sides.IsValidIndex(Si))
		{
			return nullptr;
		}
		for (int32 i = 0; i < Map.Lines.Num(); ++i)
		{
			if (i != Line && (Map.Lines[i].Side[0] == Si || Map.Lines[i].Side[1] == Si))
			{
				const FDoomSide Copy = Map.Sides[Si];
				Si = Map.Sides.Add(Copy);
				Map.Lines[Line].Side[s] = Si;
				Map.Sides[Si].Line = Line;
				break;
			}
		}
		return &Map.Sides[Si];
	};
	auto Tex = [](FName N) { return (N.IsNone() || N == TEXT("-")) ? NAME_None : N; };
	if (bFrontUpper || bFrontMiddle || bFrontLower)
	{
		if (FDoomSide* F = Side(0))
		{
			if (bFrontUpper) F->Upper = Tex(FrontUpper);
			if (bFrontMiddle) F->Middle = Tex(FrontMiddle);
			if (bFrontLower) F->Lower = Tex(FrontLower);
		}
	}
	if (bBackUpper || bBackMiddle || bBackLower)
	{
		if (FDoomSide* B = Side(1))
		{
			if (bBackUpper) B->Upper = Tex(BackUpper);
			if (bBackMiddle) B->Middle = Tex(BackMiddle);
			if (bBackLower) B->Lower = Tex(BackLower);
		}
	}
}

void ADoomLineEdit::ReadFromLine()
{
	const ADoomMapActor* MapActor = ADoomMapActor::Find(GetWorld());
	// the WAD's own values, not the preview's (which has this actor's overrides applied)
	FDoomMap Fresh;
	if (!MapActor || !DoomEditorData::Wad() || !Fresh.Load(*DoomEditorData::Wad(), MapActor->MapName) || !Fresh.Lines.IsValidIndex(Line))
	{
		return;
	}
	Modify();
	const FDoomLine& L = Fresh.Lines[Line];
	Special = L.Special;
	Tag = L.Tag;
	using namespace DoomLineFlags;
	bImpassable = (L.Flags & Blocking) != 0;
	bBlockMonsters = (L.Flags & BlockMonsters) != 0;
	bSecret = (L.Flags & Secret) != 0;
	bBlockSound = (L.Flags & SoundBlock) != 0;
	bNotOnAutomap = (L.Flags & DontDraw) != 0;
	bUpperUnpegged = (L.Flags & DontPegTop) != 0;
	bLowerUnpegged = (L.Flags & DontPegBottom) != 0;
	auto Name = [](FName N) { return N.IsNone() ? FName(TEXT("-")) : N; };
	if (Fresh.Sides.IsValidIndex(L.Side[0]))
	{
		const FDoomSide& F = Fresh.Sides[L.Side[0]];
		FrontUpper = Name(F.Upper); FrontMiddle = Name(F.Middle); FrontLower = Name(F.Lower);
	}
	if (Fresh.Sides.IsValidIndex(L.Side[1]))
	{
		const FDoomSide& B = Fresh.Sides[L.Side[1]];
		BackUpper = Name(B.Upper); BackMiddle = Name(B.Middle); BackLower = Name(B.Lower);
	}
	Refresh(false, false);
}

TArray<FString> ADoomLineEdit::GetTextureNames()
{
	TArray<FString> Names = { TEXT("-") };
	if (FDoomGraphics* G = DoomEditorData::Graphics())
	{
		for (FName N : G->TextureNames())
		{
			Names.Add(N.ToString());
		}
	}
	return Names;
}

void ADoomLineEdit::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Refresh(false, Line == INDEX_NONE);
}

void ADoomLineEdit::Refresh(bool bRebuild, bool bPickLine)
{
	// editor feedback only (the label is an editor-only component, stripped from game worlds)
	if (!GetWorld() || GetWorld()->IsGameWorld() || !Label)
	{
		return;
	}
	ADoomMapActor* MapActor = ADoomMapActor::Find(GetWorld());
	if (bPickLine && MapActor && MapActor->GetPreviewMap())
	{
		Line = MapActor->LineNearLocation(GetActorLocation());
	}
	const int32 ShownSpecial = bSpecial ? Special : -1;
	Label->SetText(FText::FromString(Line < 0 ? FString(TEXT("no line"))
		: ShownSpecial > 0 ? FString::Printf(TEXT("Line %d: special %d"), Line, ShownSpecial) : FString::Printf(TEXT("Line %d"), Line)));
	if (bRebuild && MapActor)
	{
		MapActor->RequestPreview();
	}
}

#if WITH_EDITOR
void ADoomLineEdit::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	Refresh(true, false);
}

void ADoomLineEdit::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (bFinished)
	{
		Refresh(true, true); // moved: it edits the line it now stands by
	}
}
#endif
