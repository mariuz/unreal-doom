// The content builder's test of Doom > Draw Sectors (-drawtest=MAP, DoomUE.Editor.DrawSector): rooms
// drawn through the mode's own steps in an imported level, undone and redone, with a sector edit on one.
#include "DoomContentBuilder.h"
#include "DoomContentPrivate.h"
#include "DoomDrawMode.h"
#include "DoomMapActors.h"
#include "DoomMapEdit.h"
#include "DoomMap.h"
#include "DoomUE.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Engine/World.h"

namespace DoomContentDrawDetail
{
	/** Clicks the corners of a square (Doom 2D, at floor height) through the mode; the last click
	 *  lands on the first corner, which closes it. */
	void ClickSquare(UDoomDrawMode* Mode, const FVector2f& Min, float Size, float Floor, bool bCloseWithEnter)
	{
		const FVector2f C[4] = { Min, Min + FVector2f(Size, 0), Min + FVector2f(Size, Size), Min + FVector2f(0, Size) };
		for (const FVector2f& P : C)
		{
			// a ray from above, as a click in the viewport would make
			const FVector At = Doom::ToUE(P, Floor);
			Mode->AddCornerFromRay(At + FVector(0, 0, 4000), FVector(0, 0, -1));
		}
		if (bCloseWithEnter)
		{
			Mode->Finish();
		}
		else
		{
			Mode->AddCornerFromRay(Doom::ToUE(C[0], Floor) + FVector(0, 0, 4000), FVector(0, 0, -1));
		}
	}

	int32 CountDraws(UWorld* World)
	{
		int32 N = 0;
		for (TActorIterator<ADoomSectorDraw> It(World); It; ++It) ++N;
		return N;
	}
}

bool DoomContent::DrawTest(const FString& InMapName)
{
	using namespace DoomContentDrawDetail;
	const FString MapName = InMapName.ToUpper();
	if (!FPackageName::DoesPackageExist(MapPackage(MapName)) && !ImportMap(MapName, false))
	{
		return false;
	}
	UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(FPackageName::LongPackageNameToFilename(MapPackage(MapName), FPackageName::GetMapPackageExtension()));
	ADoomMapActor* MapActor = ADoomMapActor::Find(World);
	if (!MapActor)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Drawtest: no map actor in %s"), *MapPackage(MapName));
		return false;
	}
	MapActor->RebuildPreview();
	const FDoomMap* Map = MapActor->GetPreviewMap();
	ADoomThingMarker* Start = nullptr;
	for (TActorIterator<ADoomThingMarker> It(World); It && !Start; ++It)
	{
		if (It->DoomEdNum == 1) Start = *It;
	}
	if (!Map || !Start)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Drawtest: no preview or no player start"));
		return false;
	}
	const int32 Sectors0 = Map->Sectors.Num();
	FBox2f Bounds(ForceInit);
	for (const FVector2f& V : Map->Vertices) Bounds += V;
	const FVector2f StartAt = Doom::ToDoom2D(Start->GetActorLocation());
	const int32 StartSector = MapActor->SectorAtLocation(Start->GetActorLocation());
	const float StartFloor = Map->Sectors[StartSector].Floor;

	// a commandlet has no undo buffer: give it one, so -drawtest checks undo like the editor does
	if (GEditor && !GEditor->Trans)
	{
		GEditor->Trans = GEditor->CreateTrans();
	}
	// the mode's own steps (its viewport clicks become these rays)
	UDoomDrawMode* Mode = NewObject<UDoomDrawMode>();
	// 1. a room in the void east of the map, closed by clicking its first corner again
	const FVector2f VoidMin(FMath::RoundToFloat(Bounds.Max.X) + 256.f, FMath::RoundToFloat(Bounds.Min.Y));
	ClickSquare(Mode, VoidMin, 256.f, 0.f, false);
	// 2. a room inside the start room, around the player start, closed with Enter
	const FVector2f InsideMin = DoomMapEdit::Snap(StartAt - FVector2f(32.f, 32.f), 8.f);
	ClickSquare(Mode, InsideMin, 64.f, StartFloor, true);
	const int32 Draws = CountDraws(World);
	MapActor->RebuildPreview();
	Map = MapActor->GetPreviewMap();
	const int32 Sectors2 = Map->Sectors.Num();
	ADoomSectorDraw* Inner = nullptr;
	ADoomSectorDraw* Outer = nullptr;
	for (TActorIterator<ADoomSectorDraw> It(World); It; ++It)
	{
		(It->Order == 0 ? Outer : Inner) = *It;
	}
	const int32 InnerSector = Inner ? Inner->Sector : INDEX_NONE;
	const int32 OuterSector = Outer ? Outer->Sector : INDEX_NONE;
	// the inner room: the start sector's floor, two-sided walls to it, and the BSP finds it
	const float InnerFloor = Map->Sectors.IsValidIndex(InnerSector) ? Map->Sectors[InnerSector].Floor : -1.f;
	bool bInner = Map->Sectors.IsValidIndex(InnerSector) && FMath::IsNearlyEqual(InnerFloor, StartFloor)
		&& Map->SectorAt(StartAt) == InnerSector;
	int32 TwoSided = 0;
	if (bInner)
	{
		for (const FDoomLine& L : Map->Lines)
		{
			if (L.FrontSector == InnerSector || L.BackSector == InnerSector) TwoSided += L.Side[1] != INDEX_NONE;
		}
		bInner &= TwoSided == 4;
	}
	const bool bOuter = Map->Sectors.IsValidIndex(OuterSector) && Map->SectorAt(VoidMin + FVector2f(128.f, 128.f)) == OuterSector
		&& Map->Sectors[OuterSector].Floor == 0.f && Map->Sectors[OuterSector].Ceiling == 128.f;

	// 3. undo takes the inner room away, redo brings it back
	int32 AfterUndo = -1, AfterRedo = -1;
	if (GEditor && GEditor->Trans)
	{
		GEditor->UndoTransaction();
		MapActor->RebuildPreview();
		AfterUndo = MapActor->GetPreviewMap()->Sectors.Num();
		GEditor->RedoTransaction();
		MapActor->RebuildPreview();
		AfterRedo = MapActor->GetPreviewMap()->Sectors.Num();
	}
	const bool bUndo = AfterUndo == Sectors0 + 1 && AfterRedo == Sectors0 + 2;

	// 4. a sector edit works on a drawn sector (drawn sectors apply first)
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADoomSectorEdit* Raise = World->SpawnActor<ADoomSectorEdit>(ADoomSectorEdit::StaticClass(), FTransform(Doom::ToUE(StartAt, StartFloor + 64.f)), SP);
	Raise->bFloor = true;
	Raise->FloorHeight = StartFloor + 24.f;
	Raise->SetActorLabel(TEXT("Raised drawn sector"));
	MapActor->RebuildPreview();
	Map = MapActor->GetPreviewMap();
	Inner = nullptr;
	for (TActorIterator<ADoomSectorDraw> It(World); It; ++It)
	{
		if (It->Order == 1) Inner = *It;
	}
	const int32 RaisedSector = Inner ? Inner->Sector : INDEX_NONE;
	const bool bRaised = Map->Sectors.IsValidIndex(RaisedSector) && FMath::IsNearlyEqual(Map->Sectors[RaisedSector].Floor, StartFloor + 24.f)
		&& FMath::IsNearlyEqual(Map->Sectors[StartSector].Floor, StartFloor);

	const FString TestPackage = FString(TEXT("/Game/Doom/Tests/")) + MapName + TEXT("_Draw");
	const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, TestPackage);
	const bool bPass = Draws == 2 && Sectors2 == Sectors0 + 2 && bInner && bOuter && bUndo && bRaised && bSaved;
	UE_LOG(LogDoomContent, Display, TEXT("Drawtest %s: %d drawn sector actors, sectors %d -> %d; room in the void: sector %d %s; room in the start room: sector %d, floor %.0f (start %.0f), %d two-sided walls %s; undo %d, redo %d; a sector edit raises the drawn sector to %.0f %s; saved %s"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), Draws, Sectors0, Sectors2, OuterSector, bOuter ? TEXT("ok") : TEXT("WRONG"),
		InnerSector, InnerFloor, StartFloor, TwoSided, bInner ? TEXT("ok") : TEXT("WRONG"),
		AfterUndo, AfterRedo, Map->Sectors.IsValidIndex(RaisedSector) ? Map->Sectors[RaisedSector].Floor : -1.f, bRaised ? TEXT("ok") : TEXT("WRONG"), *TestPackage);
	return bPass;
}
