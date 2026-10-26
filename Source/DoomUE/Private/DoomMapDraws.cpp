// ADoomSectorDraw: a sector drawn in the level editor's viewport (Doom > Draw Sectors), applied to
// the map before the sector and line edits (ADoomMapActor::ApplyEdits).
#include "DoomMapActors.h"
#include "DoomMapEdit.h"
#include "DoomGraphics.h"
#include "DoomUE.h"
#include "Components/TextRenderComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

#define LOCTEXT_NAMESPACE "DoomMapDraws"

ADoomSectorDraw::ADoomSectorDraw()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(RootComponent);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetWorldSize(48.f);
	Label->SetTextRenderColor(FColor(140, 255, 120));
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

TArray<FVector2f> ADoomSectorDraw::MapCorners() const
{
	const FVector2f Origin = Doom::ToDoom2D(GetActorLocation());
	TArray<FVector2f> Out;
	for (const FVector2D& C : Corners)
	{
		Out.Add(Origin + FVector2f(C));
	}
	return Out;
}

int32 ADoomSectorDraw::Apply(FDoomMap& Map, FString& OutError) const
{
	const TArray<FVector2f> Points = MapCorners();
	if (Points.Num() < 3)
	{
		OutError = TEXT("fewer than three corners");
		return INDEX_NONE;
	}
	// it starts as a copy of the sector it is drawn in: heights, flats and light, but not its special
	// or tag (a room drawn inside a door sector is no door)
	FVector2f Mid = FVector2f::ZeroVector;
	for (const FVector2f& P : Points) Mid += P;
	Mid /= Points.Num();
	const int32 Inside = DoomMapEdit::SectorInside(Map, Mid);
	FDoomSector T;
	if (Map.Sectors.IsValidIndex(Inside))
	{
		const FDoomSector& S = Map.Sectors[Inside];
		T.Floor = S.Floor;
		T.Ceiling = S.Ceiling;
		T.FloorPic = S.FloorPic;
		T.CeilingPic = S.CeilingPic;
		T.Light = T.OriginalLight = S.Light;
	}
	else
	{
		T.Floor = 0;
		T.Ceiling = 128;
		T.FloorPic = TEXT("FLOOR4_8");
		T.CeilingPic = TEXT("CEIL3_5");
	}
	if (bFloor) T.Floor = FloorHeight;
	if (bCeiling) T.Ceiling = CeilingHeight;
	if (bLight) T.Light = T.OriginalLight = (int16)FMath::Clamp(Light, 0, 255);
	if (bFloorFlat && !FloorFlat.IsNone()) T.FloorPic = FloorFlat;
	if (bCeilingFlat && !CeilingFlat.IsNone()) T.CeilingPic = CeilingFlat;
	if (bSpecial) T.Special = (int16)Special;
	if (bTag) T.Tag = (int16)Tag;
	const FName Wall = WallTexture.IsNone() ? FName(TEXT("STARTAN3")) : WallTexture;
	return DoomMapEdit::DrawSector(Map, Points, T, Wall, OutError);
}

TArray<FString> ADoomSectorDraw::GetTextureNames()
{
	return ADoomLineEdit::GetTextureNames();
}

TArray<FString> ADoomSectorDraw::GetFlatNames()
{
	return ADoomSectorEdit::GetFlatNames();
}

void ADoomSectorDraw::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Refresh(false);
}

void ADoomSectorDraw::Refresh(bool bRebuild)
{
	// editor feedback only (the label and outline are editor-only, stripped from game worlds)
	UWorld* World = GetWorld();
	if (!World || World->IsGameWorld() || !Label)
	{
		return;
	}
	Label->SetText(Sector >= 0 ? FText::Format(LOCTEXT("DrawnSector", "Drawn sector {0}"), DoomText::Number(Sector))
		: LOCTEXT("NotDrawn", "Drawn sector (could not be drawn: see the Output Log)"));
	if (!Outline)
	{
		Outline = NewObject<ULineBatchComponent>(this, TEXT("Outline"), RF_Transient);
		Outline->SetupAttachment(RootComponent);
		Outline->bIsEditorOnly = true;
		Outline->SetHiddenInGame(true);
		Outline->RegisterComponent();
	}
	Outline->Flush();
	const TArray<FVector2f> Points = MapCorners();
	const float Z = GetActorLocation().Z / Doom::Scale + 2.f;
	const FLinearColor Color = Sector >= 0 ? FLinearColor(0.5f, 1.f, 0.45f) : FLinearColor(1.f, 0.25f, 0.2f);
	TArray<FBatchedLine> Lines;
	for (int32 i = 0; i < Points.Num(); ++i)
	{
		Lines.Emplace(Doom::ToUE(Points[i], Z), Doom::ToUE(Points[(i + 1) % Points.Num()], Z), Color, 0.f, 4.f, SDPG_Foreground);
	}
	Outline->DrawLines(Lines);
	if (bRebuild)
	{
		if (ADoomMapActor* MapActor = ADoomMapActor::Find(World))
		{
			MapActor->RequestPreview();
		}
	}
}

#if WITH_EDITOR
void ADoomSectorDraw::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	Refresh(true);
}

void ADoomSectorDraw::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	Refresh(bFinished);
}
#endif

#undef LOCTEXT_NAMESPACE
