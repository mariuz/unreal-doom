#include "DoomDrawMode.h"
#include "DoomMapActors.h"
#include "DoomMapEdit.h"
#include "DoomMap.h"
#include "DoomUE.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "CanvasTypes.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/World.h"
#include "Settings/LevelEditorViewportSettings.h"

#define LOCTEXT_NAMESPACE "DoomDrawMode"

const FEditorModeID UDoomDrawMode::Id = TEXT("EM_DoomDraw");

namespace DoomDrawModeDetail
{
	/** How near an existing corner a click snaps to it, in map units. */
	constexpr float CornerSnap = 8.f;
}

UDoomDrawMode::UDoomDrawMode()
{
	Info = FEditorModeInfo(Id, LOCTEXT("ModeName", "Doom: Draw Sectors"), FSlateIcon(), false);
}

void UDoomDrawMode::Activate()
{
	GLevelEditorModeTools().ActivateMode(Id);
}

UDoomDrawMode* UDoomDrawMode::Get()
{
	return Cast<UDoomDrawMode>(GLevelEditorModeTools().GetActiveScriptableMode(Id));
}

void UDoomDrawMode::Enter()
{
	Super::Enter();
	Clear();
	Status = MapActor() ? LOCTEXT("Start", "Click the corners on the floor; click the first one again or press Enter to close. Backspace: undo a corner. Esc: leave.")
		: LOCTEXT("NoMap", "Open a Doom map level first (Doom > Open / Import Map).");
}

void UDoomDrawMode::Exit()
{
	Corners.Reset();
	Hover.Reset();
	Super::Exit();
}

ADoomMapActor* UDoomDrawMode::MapActor() const
{
	return GEditor ? ADoomMapActor::Find(GEditor->GetEditorWorldContext().World()) : nullptr;
}

bool UDoomDrawMode::RayToWorld(const FVector& Origin, const FVector& Direction, FVector& Out) const
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		return false;
	}
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DoomDraw), true);
	if (World->LineTraceSingleByChannel(Hit, Origin, Origin + Direction * 1000000.0, ECC_Visibility, Params))
	{
		Out = Hit.ImpactPoint;
		return true;
	}
	// nothing there (the void, or a top view past the edge): the height of the last corner
	const double Z = Corners.Num() ? AtFloor(Corners.Last()).Z : 0.0;
	if (FMath::IsNearlyZero(Direction.Z))
	{
		return false;
	}
	const double T = (Z - Origin.Z) / Direction.Z;
	if (T <= 0)
	{
		return false;
	}
	Out = Origin + Direction * T;
	return true;
}

FVector2f UDoomDrawMode::Snap(const FVector2f& P) const
{
	if (const ADoomMapActor* M = MapActor())
	{
		if (const FDoomMap* Map = M->GetPreviewMap())
		{
			const int32 V = DoomMapEdit::VertexNear(*Map, P, DoomDrawModeDetail::CornerSnap);
			if (V != INDEX_NONE)
			{
				return Map->V(V);
			}
		}
	}
	for (const FVector2f& C : Corners)
	{
		if (FVector2f::Distance(C, P) <= DoomDrawModeDetail::CornerSnap)
		{
			return C;
		}
	}
	// the level editor's grid, in map units
	const ULevelEditorViewportSettings* Settings = GetDefault<ULevelEditorViewportSettings>();
	if (GEditor && Settings && Settings->GridEnabled)
	{
		return DoomMapEdit::Snap(P, (float)FMath::Max(1, FMath::RoundToInt((float)GEditor->GetGridSize() / Doom::Scale))); // whole map units: a PWAD stores integers
	}
	return FVector2f(FMath::RoundToFloat(P.X), FMath::RoundToFloat(P.Y));
}

FVector UDoomDrawMode::AtFloor(const FVector2f& P) const
{
	float Floor = 0;
	if (const ADoomMapActor* M = MapActor())
	{
		if (const FDoomMap* Map = M->GetPreviewMap())
		{
			const int32 S = DoomMapEdit::SectorInside(*Map, P);
			if (Map->Sectors.IsValidIndex(S)) Floor = Map->Sectors[S].Floor;
		}
	}
	return Doom::ToUE(P, Floor);
}

bool UDoomDrawMode::AddCornerFromRay(const FVector& Origin, const FVector& Direction)
{
	FVector At;
	if (!RayToWorld(Origin, Direction, At))
	{
		return false;
	}
	AddCorner(At);
	return true;
}

void UDoomDrawMode::AddCorner(const FVector& WorldPoint)
{
	const FVector2f P = Snap(Doom::ToDoom2D(WorldPoint));
	if (Corners.Num() >= 3 && P.Equals(Corners[0], 0.5f))
	{
		Finish();
		return;
	}
	if (Corners.Num() && P.Equals(Corners.Last(), 0.5f))
	{
		return; // the same corner twice
	}
	Corners.Add(P);
	Status = FText::Format(LOCTEXT("Corners", "{0} corners ({1}, {2}). Click the first one again or press Enter to close."),
		Corners.Num(), DoomText::Number(FMath::RoundToInt(P.X)), DoomText::Number(FMath::RoundToInt(P.Y)));
}

void UDoomDrawMode::RemoveLastCorner()
{
	if (Corners.Num())
	{
		Corners.Pop();
	}
	Status = FText::Format(LOCTEXT("CornersLeft", "{0} corners"), Corners.Num());
}

void UDoomDrawMode::Clear()
{
	Corners.Reset();
	Hover.Reset();
}

ADoomSectorDraw* UDoomDrawMode::Finish()
{
	ADoomMapActor* M = MapActor();
	const FDoomMap* Preview = M ? M->GetPreviewMap() : nullptr;
	if (!M || !Preview)
	{
		Status = LOCTEXT("NoMap", "Open a Doom map level first (Doom > Open / Import Map).");
		return nullptr;
	}
	if (Corners.Num() < 3)
	{
		Status = LOCTEXT("NeedThree", "A sector needs at least three corners.");
		return nullptr;
	}
	// try it on a copy of the preview first: a sector that can't be drawn keeps its corners
	FDoomMap Test = *Preview;
	FString Error;
	if (DoomMapEdit::DrawSector(Test, Corners, FDoomSector(), TEXT("STARTAN3"), Error) == INDEX_NONE)
	{
		Status = FText::Format(LOCTEXT("CantDraw", "Can't draw that: {0}"), FText::FromString(Error));
		return nullptr;
	}
	UWorld* World = M->GetWorld();
	FVector2f Mid = FVector2f::ZeroVector;
	for (const FVector2f& C : Corners) Mid += C;
	Mid /= Corners.Num();
	int32 Order = 0;
	for (TActorIterator<ADoomSectorDraw> It(World); It; ++It)
	{
		Order = FMath::Max(Order, It->Order + 1);
	}
	ADoomSectorDraw* D = nullptr;
	{
		// one undo step: the actor and its corners (the preview rebuild after it stays out of it)
		const FScopedTransaction Transaction(LOCTEXT("DrawSectorUndo", "Draw Doom Sector"));
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		D = World->SpawnActor<ADoomSectorDraw>(ADoomSectorDraw::StaticClass(), FTransform(AtFloor(Mid) + FVector(0, 0, 16.0 * Doom::Scale)), SP);
		if (!D)
		{
			return nullptr;
		}
		// corners relative to the actor, which may sit off the exact middle after spawning
		const FVector2f Origin = Doom::ToDoom2D(D->GetActorLocation());
		for (const FVector2f& C : Corners)
		{
			D->Corners.Add(FVector2D(C - Origin));
		}
		D->Order = Order;
		// a sector drawn in the void gets the walls of the sector nearest to it, else the default
		const int32 Near = DoomMapEdit::LineNear(*Preview, Corners[0], 1024.f);
		if (Near != INDEX_NONE && Preview->Lines[Near].Side[0] != INDEX_NONE && !Preview->Sides[Preview->Lines[Near].Side[0]].Middle.IsNone())
		{
			D->WallTexture = Preview->Sides[Preview->Lines[Near].Side[0]].Middle;
		}
		D->SetActorLabel(FString::Printf(TEXT("Drawn sector %d"), Order + 1));
		D->SetFolderPath(TEXT("Doom/Drawn Sectors"));
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(D, true, true);
	}
	Corners.Reset();
	M->RebuildPreview();
	Status = D->Sector != INDEX_NONE
		? FText::Format(LOCTEXT("Drawn", "New sector {0}. Its heights, flats and light are in the Details panel."), DoomText::Number(D->Sector))
		: LOCTEXT("DrawnFailed", "The sector could not be drawn with the level's edits (see the Output Log).");
	return D;
}

bool UDoomDrawMode::HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click)
{
	if (Click.GetKey() != EKeys::LeftMouseButton)
	{
		return false;
	}
	if (Click.GetEvent() == IE_DoubleClick)
	{
		Finish();
		return true;
	}
	AddCornerFromRay(Click.GetOrigin(), Click.GetDirection());
	return true; // the click draws; it doesn't select
}

bool UDoomDrawMode::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (Event != IE_Pressed)
	{
		return Key == EKeys::Enter || Key == EKeys::BackSpace || Key == EKeys::Escape;
	}
	if (Key == EKeys::Enter)
	{
		Finish();
		return true;
	}
	if (Key == EKeys::BackSpace)
	{
		RemoveLastCorner();
		return true;
	}
	if (Key == EKeys::Escape)
	{
		if (Corners.Num())
		{
			Clear();
			Status = LOCTEXT("Cleared", "Corners cleared. Esc again leaves the mode.");
		}
		else
		{
			GLevelEditorModeTools().DeactivateMode(Id);
		}
		return true;
	}
	return false;
}

bool UDoomDrawMode::MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 X, int32 Y)
{
	// the corner the next click would add, for the rubber band
	FSceneViewFamilyContext Family(FSceneViewFamily::ConstructionValues(Viewport, ViewportClient->GetScene(), ViewportClient->EngineShowFlags).SetRealtimeUpdate(true));
	FSceneView* View = ViewportClient->CalcSceneView(&Family);
	const FViewportCursorLocation Cursor(View, ViewportClient, X, Y);
	FVector At;
	if (RayToWorld(Cursor.GetOrigin(), Cursor.GetDirection(), At))
	{
		Hover = Snap(Doom::ToDoom2D(At));
	}
	else
	{
		Hover.Reset();
	}
	return false;
}

void UDoomDrawMode::Render(const FSceneView* View, FViewport* Viewport, FPrimitiveDrawInterface* PDI)
{
	Super::Render(View, Viewport, PDI);
	const FLinearColor Line(1.f, 0.85f, 0.2f);
	const FVector Up(0, 0, 2.0 * Doom::Scale);
	for (int32 i = 0; i < Corners.Num(); ++i)
	{
		const FVector A = AtFloor(Corners[i]) + Up;
		PDI->DrawPoint(A, i == 0 ? FLinearColor(0.3f, 1.f, 0.3f) : Line, 10.f, SDPG_Foreground);
		if (i + 1 < Corners.Num())
		{
			PDI->DrawLine(A, AtFloor(Corners[i + 1]) + Up, Line, SDPG_Foreground, 3.f);
		}
	}
	if (Hover.IsSet())
	{
		const FVector H = AtFloor(*Hover) + Up;
		PDI->DrawPoint(H, FLinearColor::White, 8.f, SDPG_Foreground);
		if (Corners.Num())
		{
			PDI->DrawLine(AtFloor(Corners.Last()) + Up, H, FLinearColor(1.f, 1.f, 1.f, 0.6f), SDPG_Foreground, 1.5f);
		}
		if (Corners.Num() >= 2)
		{
			PDI->DrawLine(H, AtFloor(Corners[0]) + Up, FLinearColor(1.f, 1.f, 1.f, 0.3f), SDPG_Foreground, 1.f);
		}
	}
}

void UDoomDrawMode::DrawHUD(FEditorViewportClient* ViewportClient, FViewport* Viewport, const FSceneView* View, FCanvas* Canvas)
{
	Super::DrawHUD(ViewportClient, Viewport, View, Canvas);
	if (Canvas && GEngine)
	{
		const float Scale = Canvas->GetDPIScale();
		Canvas->DrawShadowedString(12.f / Scale, 40.f / Scale, *(LOCTEXT("Title", "Doom: Draw Sectors").ToString() + TEXT("  -  ") + Status.ToString()),
			GEngine->GetSmallFont(), FLinearColor(1.f, 0.85f, 0.2f));
	}
}

#undef LOCTEXT_NAMESPACE
