// ADoomThingMarker: a map thing in an imported level (type, angle, flags), drawn with its sprite.
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
	for (const TCHAR* N : DoomMapActorsDetail::PlayerStartNames)
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
	for (int32 i = 0; i < UE_ARRAY_COUNT(DoomMapActorsDetail::PlayerStartTypes); ++i)
	{
		if (DoomMapActorsDetail::PlayerStartTypes[i] == EdNum) return DoomMapActorsDetail::PlayerStartNames[i];
	}
	const FDoomThingDef* D = DoomInfo::FindThing(EdNum);
	return D ? FString(D->Name) : FString::Printf(TEXT("Thing%d"), EdNum);
}

FName ADoomThingMarker::FolderFor(int32 EdNum)
{
	for (int32 T : DoomMapActorsDetail::PlayerStartTypes)
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
			const float Z = DoomMapActorsDetail::MarkerFloorAt(Map, L) * Doom::Scale;
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
		for (int32 i = 0; i < UE_ARRAY_COUNT(DoomMapActorsDetail::PlayerStartTypes); ++i)
		{
			if (Wanted == DoomMapActorsDetail::PlayerStartNames[i]) DoomEdNum = DoomMapActorsDetail::PlayerStartTypes[i];
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
