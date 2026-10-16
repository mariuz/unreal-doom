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
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/Texture2D.h"
#include "Containers/Ticker.h"
#include "Misc/Paths.h"
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
			E.Wad = MakeUnique<FDoomWad>();
			if (E.Wad->AddFile(Resolve(Settings->IWAD)))
			{
				for (const FString& P : Settings->PWADs)
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
	UE_LOG(LogDoom, Log, TEXT("%s: %d sector edits, %d thing markers from the level"), *MapName, Edits, Markers);
}

int32 ADoomMapActor::SectorAtLocation(const FVector& Location) const
{
	return PreviewMap ? PreviewMap->SectorAt(Doom::ToDoom2D(Location)) : INDEX_NONE;
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
