// ADoomSectorEdit and ADoomLineEdit: a sector's heights, light, flats and special, and a line's
// flags, special, tag and textures, overridden in an imported level.
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

