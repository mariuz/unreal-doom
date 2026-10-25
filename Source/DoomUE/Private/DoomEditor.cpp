// The map editor (see DoomEditor.h): the editing subsystem, the Slate map view and the UMG panels.
#include "DoomEditor.h"
#include "DoomWorld.h"
#include "DoomGameMode.h"
#include "DoomPlayer.h"
#include "DoomMapCheck.h"
#include "DoomMapGen.h"
#include "DoomMapActors.h"
#include "DoomWadWriter.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomUE.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Border.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/SizeBox.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"

// ------------------------------------------------------------------------------------------------
// The editor
// ------------------------------------------------------------------------------------------------

bool UDoomMapEditor::bOpenOnNextPlay = false;

UDoomWorld* UDoomMapEditor::DoomWorld() const
{
	return GetWorld() ? GetWorld()->GetSubsystem<UDoomWorld>() : nullptr;
}

void UDoomMapEditor::LoadFresh(const FString& Name)
{
	UDoomWorld* W = DoomWorld();
	EditMap = MakeUnique<FDoomMap>();
	bool bOk = false;
	uint32 Seed = 0;
	if (DoomMapGen::ParseName(Name, Seed))
	{
		bOk = DoomMapGen::Generate(W->GenOptions(Seed), W->GetGraphics(), *EditMap);
	}
	else if (W->GetWad() && W->GetWad()->FindLump(Name) != INDEX_NONE)
	{
		bOk = EditMap->Load(*W->GetWad(), Name);
	}
	if (bOk)
	{
		// an imported Unreal level's own edits (in Play In Editor) are part of the map being edited
		if (const ADoomMapActor* A = ADoomMapActor::Find(GetWorld()); A && A->MapName.Equals(Name, ESearchCase::IgnoreCase))
		{
			A->ApplyEdits(*EditMap);
		}
	}
	else if (W->GetMap())
	{
		*EditMap = *W->GetMap(); // a map only the editor knows (a new room): as it is
	}
	UndoStack.Reset();
	RedoStack.Reset();
	Selected = Hovered = INDEX_NONE;
	DrawPoints.Reset();
}

void UDoomMapEditor::Open()
{
	UDoomWorld* W = DoomWorld();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (bOpen || !W || !PC || W->IsNetGame())
	{
		return;
	}
	if (!W->GetMap() || W->GetState() != EDoomGameState::Playing)
	{
		W->Menu.Close(*W);
		if (!W->GetMap())
		{
			W->StartNewGame(DoomMapGen::NameForSeed((uint32)FMath::RandRange(1, 0xFFFFF)), W->GetSkill());
			W->StepLoad(MAX_dbl);
		}
	}
	if (!W->GetMap())
	{
		return;
	}
	if (!EditMap.IsValid() || !EditMap->Name.Equals(W->GetMapName(), ESearchCase::IgnoreCase))
	{
		LoadFresh(W->GetMapName());
	}
	bOpen = true;
	if (ADoomGameMode* GM = GetWorld()->GetAuthGameMode<ADoomGameMode>())
	{
		GM->bClockStopped = true;
	}
	// new sectors copy the one under the player, walls its first wall
	const FVector2f At = W->GetPlayer() ? W->GetPlayer()->DoomPos() : FVector2f::ZeroVector;
	const int32 Under = DoomMapEdit::SectorInside(*EditMap, At);
	DrawTemplate = EditMap->Sectors.IsValidIndex(Under) ? EditMap->Sectors[Under] : (EditMap->Sectors.Num() ? EditMap->Sectors[0] : FDoomSector());
	DrawWall = NAME_None;
	for (const FDoomLine& L : EditMap->Lines)
	{
		if (L.Side[1] == INDEX_NONE && L.Side[0] != INDEX_NONE && !EditMap->Sides[L.Side[0]].Middle.IsNone()) { DrawWall = EditMap->Sides[L.Side[0]].Middle; break; }
	}
	Apply(false);
	ViewCenter = At;
	Zoom = 0.6f;
	Widget = CreateWidget<UDoomEditorWidget>(PC, UDoomEditorWidget::StaticClass());
	Widget->Editor = this;
	Widget->AddToViewport(50);
	PC->SetShowMouseCursor(true);
	FInputModeUIOnly InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(InputMode);
	Widget->GetMapView()->FocusView();
	Status = FString::Printf(TEXT("Editing %s. Click to select, 1-4 / D for the mode, Tab for 3D, F5 plays it."), *EditMap->Name);
	Refresh();
	UE_LOG(LogDoomEditor, Log, TEXT("Map editor: editing %s (%d sectors, %d lines, %d things)"), *EditMap->Name, EditMap->Sectors.Num(), EditMap->Lines.Num(), EditMap->Things.Num());
}

void UDoomMapEditor::Close()
{
	if (!bOpen)
	{
		return;
	}
	bOpen = false;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (Widget)
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}
	if (PC)
	{
		PC->SetShowMouseCursor(false);
		PC->SetInputMode(FInputModeGameOnly());
	}
	if (ADoomGameMode* GM = GetWorld()->GetAuthGameMode<ADoomGameMode>())
	{
		GM->bClockStopped = false;
	}
	Apply(false); // a fresh start of the edited map, from where the camera is
	SetPlayerFlying(false);
	UE_LOG(LogDoomEditor, Log, TEXT("Map editor: playing %s"), EditMap.IsValid() ? *EditMap->Name : TEXT("-"));
}

void UDoomMapEditor::SetPlayerFlying(bool bFly)
{
	ADoomPlayer* P = DoomWorld() ? DoomWorld()->GetPlayer() : nullptr;
	if (!P)
	{
		return;
	}
	P->GetCharacterMovement()->StopMovementImmediately();
	P->GetCharacterMovement()->SetMovementMode(bFly ? MOVE_Flying : MOVE_Walking);
	P->SetActorEnableCollision(!bFly); // no pickups or bumps while flying about
}

void UDoomMapEditor::Begin()
{
	UndoStack.Add(DoomMapEdit::Take(*EditMap));
	if (UndoStack.Num() > 100) UndoStack.RemoveAt(0);
	RedoStack.Reset();
}

void UDoomMapEditor::Apply(bool bGeometry)
{
	UDoomWorld* W = DoomWorld();
	if (!W || !EditMap.IsValid())
	{
		return;
	}
	if (bGeometry)
	{
		EditMap->Rebuild();
	}
	ADoomPlayer* P = W->GetPlayer();
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const FVector Loc = P ? P->GetActorLocation() : FVector::ZeroVector;
	const FRotator Rot = PC ? PC->GetControlRotation() : FRotator::ZeroRotator;
	W->LoadEditedMap(*EditMap);
	P = W->GetPlayer();
	if (P)
	{
		// stay where the camera was, inside the map (else the start)
		const int32 S = DoomMapEdit::SectorInside(*EditMap, Doom::ToDoom2D(Loc));
		if (S != INDEX_NONE)
		{
			const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const FDoomSector& Sec = EditMap->Sectors[S];
			const float MinZ = Sec.Floor * Doom::Scale + Half, MaxZ = FMath::Max(MinZ, Sec.Ceiling * Doom::Scale - Half);
			P->SetActorLocation(FVector(Loc.X, Loc.Y, FMath::Clamp(Loc.Z, MinZ, MaxZ)), false, nullptr, ETeleportType::TeleportPhysics);
		}
		if (PC) PC->SetControlRotation(Rot);
	}
	if (bOpen)
	{
		SetPlayerFlying(true);
	}
	Refresh();
}

void UDoomMapEditor::Refresh()
{
	if (Widget)
	{
		Widget->RefreshPanels();
	}
}

void UDoomMapEditor::FitView()
{
	if (!EditMap.IsValid() || !EditMap->Bounds.bIsValid)
	{
		return;
	}
	FVector2D Viewport(1280, 720);
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Viewport);
	ViewCenter = EditMap->Bounds.GetCenter();
	const FVector2f Ext = EditMap->Bounds.GetSize() + FVector2f(256, 256);
	Zoom = FMath::Clamp((float)FMath::Min((Viewport.X - 360) / Ext.X, (Viewport.Y - 120) / Ext.Y), 0.02f, 4.f);
}

void UDoomMapEditor::SetMode(EDoomEditMode NewMode)
{
	Mode = NewMode;
	Selected = Hovered = INDEX_NONE;
	if (NewMode != EDoomEditMode::Draw) DrawPoints.Reset();
	Refresh();
}

void UDoomMapEditor::SelectAt(const FVector2f& P, float Radius)
{
	if (!EditMap.IsValid())
	{
		return;
	}
	switch (Mode)
	{
	case EDoomEditMode::Things: Selected = DoomMapEdit::ThingNear(*EditMap, P, Radius); break;
	case EDoomEditMode::Sectors:
		Selected = DoomMapEdit::SectorInside(*EditMap, P);
		if (Selected != INDEX_NONE) DrawTemplate = EditMap->Sectors[Selected];
		break;
	case EDoomEditMode::Lines: Selected = DoomMapEdit::LineNear(*EditMap, P, Radius); break;
	case EDoomEditMode::Vertices: Selected = DoomMapEdit::VertexNear(*EditMap, P, Radius); break;
	default: break;
	}
	Refresh();
}

bool UDoomMapEditor::SetNumber(EDoomEditField Field, float Value)
{
	if (!EditMap.IsValid())
	{
		return false;
	}
	FDoomMap& M = *EditMap;
	// settings of the editor itself
	switch (Field)
	{
	case EDoomEditField::PlaceType: PlaceType = (int32)Value; Refresh(); return true;
	case EDoomEditField::Grid: Grid = FMath::Clamp(Value, 1.f, 128.f); return true;
	default: break;
	}
	if (Mode == EDoomEditMode::Draw)
	{
		// the template for new sectors
		switch (Field)
		{
		case EDoomEditField::SectorFloor: DrawTemplate.Floor = Value; return true;
		case EDoomEditField::SectorCeiling: DrawTemplate.Ceiling = Value; return true;
		case EDoomEditField::SectorLight: DrawTemplate.Light = DrawTemplate.OriginalLight = (int16)Value; return true;
		default: return false;
		}
	}
	const int32 S = Selected;
	const bool bSector = Mode == EDoomEditMode::Sectors && M.Sectors.IsValidIndex(S);
	const bool bLine = Mode == EDoomEditMode::Lines && M.Lines.IsValidIndex(S);
	const bool bThing = Mode == EDoomEditMode::Things && M.Things.IsValidIndex(S);
	const bool bVertex = Mode == EDoomEditMode::Vertices && M.Vertices.IsValidIndex(S);
	bool bGeometry = false;
	Begin();
	switch (Field)
	{
	case EDoomEditField::SectorFloor: if (!bSector) break; M.Sectors[S].Floor = Value; break;
	case EDoomEditField::SectorCeiling: if (!bSector) break; M.Sectors[S].Ceiling = Value; break;
	case EDoomEditField::SectorLight: if (!bSector) break; M.Sectors[S].Light = M.Sectors[S].OriginalLight = (int16)FMath::Clamp(Value, 0.f, 255.f); break;
	case EDoomEditField::SectorSpecial: if (!bSector) break; M.Sectors[S].Special = (int16)Value; break;
	case EDoomEditField::SectorTag: if (!bSector) break; M.Sectors[S].Tag = (int16)Value; break;
	case EDoomEditField::LineSpecial: if (!bLine) break; M.Lines[S].Special = (int16)Value; break;
	case EDoomEditField::LineTag: if (!bLine) break; M.Lines[S].Tag = (int16)Value; break;
	case EDoomEditField::FrontXOffset: if (!bLine || M.Lines[S].Side[0] == INDEX_NONE) break; M.Sides[M.Lines[S].Side[0]].XOffset = Value; break;
	case EDoomEditField::FrontYOffset: if (!bLine || M.Lines[S].Side[0] == INDEX_NONE) break; M.Sides[M.Lines[S].Side[0]].YOffset = Value; break;
	case EDoomEditField::ThingType: if (!bThing) break; M.Things[S].Type = (int32)Value; break;
	case EDoomEditField::ThingAngle: if (!bThing) break; M.Things[S].Angle = Value; break;
	case EDoomEditField::VertexX: if (!bVertex) break; M.Vertices[S].X = Value; bGeometry = true; break;
	case EDoomEditField::VertexY: if (!bVertex) break; M.Vertices[S].Y = Value; bGeometry = true; break;
	default: break;
	}
	Status = FString::Printf(TEXT("%s = %g"), *StaticEnum<EDoomEditField>()->GetNameStringByValue((int64)Field), Value);
	Apply(bGeometry);
	return true;
}

bool UDoomMapEditor::SetName(EDoomEditField Field, FName Value)
{
	if (!EditMap.IsValid())
	{
		return false;
	}
	FDoomMap& M = *EditMap;
	if (Mode == EDoomEditMode::Draw)
	{
		switch (Field)
		{
		case EDoomEditField::SectorFloorPic: DrawTemplate.FloorPic = Value; return true;
		case EDoomEditField::SectorCeilingPic: DrawTemplate.CeilingPic = Value; return true;
		case EDoomEditField::FrontMiddle: DrawWall = Value; return true;
		default: return false;
		}
	}
	const int32 S = Selected;
	Begin();
	if (Mode == EDoomEditMode::Sectors && M.Sectors.IsValidIndex(S))
	{
		if (Field == EDoomEditField::SectorFloorPic) M.Sectors[S].FloorPic = Value;
		if (Field == EDoomEditField::SectorCeilingPic) M.Sectors[S].CeilingPic = Value;
	}
	else if (Mode == EDoomEditMode::Lines && M.Lines.IsValidIndex(S))
	{
		const bool bBack = Field == EDoomEditField::BackUpper || Field == EDoomEditField::BackMiddle || Field == EDoomEditField::BackLower;
		const int32 Side = M.Lines[S].Side[bBack ? 1 : 0];
		if (Side != INDEX_NONE)
		{
			FDoomSide& Sd = M.Sides[Side];
			// a side may be shared by several lines: this line gets its own copy first
			int32 Users = 0;
			for (const FDoomLine& L : M.Lines) Users += (L.Side[0] == Side) + (L.Side[1] == Side);
			FDoomSide* Target = &Sd;
			if (Users > 1)
			{
				M.Lines[S].Side[bBack ? 1 : 0] = M.Sides.Add(Sd);
				Target = &M.Sides.Last();
			}
			switch (Field)
			{
			case EDoomEditField::FrontUpper: case EDoomEditField::BackUpper: Target->Upper = Value; break;
			case EDoomEditField::FrontMiddle: case EDoomEditField::BackMiddle: Target->Middle = Value; break;
			default: Target->Lower = Value; break;
			}
		}
	}
	Status = FString::Printf(TEXT("%s = %s"), *StaticEnum<EDoomEditField>()->GetNameStringByValue((int64)Field), *Value.ToString());
	Apply(false);
	return true;
}

bool UDoomMapEditor::SetFlag(EDoomEditField Field, int32 Bit, bool bOn)
{
	if (!EditMap.IsValid())
	{
		return false;
	}
	FDoomMap& M = *EditMap;
	const int32 S = Selected;
	Begin();
	if (Field == EDoomEditField::LineFlag && Mode == EDoomEditMode::Lines && M.Lines.IsValidIndex(S))
	{
		M.Lines[S].Flags = bOn ? (M.Lines[S].Flags | Bit) : (M.Lines[S].Flags & ~Bit);
	}
	else if (Field == EDoomEditField::ThingFlag && Mode == EDoomEditMode::Things && M.Things.IsValidIndex(S))
	{
		M.Things[S].Flags = bOn ? (M.Things[S].Flags | Bit) : (M.Things[S].Flags & ~Bit);
	}
	Apply(false);
	return true;
}

bool UDoomMapEditor::FinishDraw()
{
	if (!EditMap.IsValid() || DrawPoints.Num() < 3)
	{
		Status = TEXT("A sector needs at least three corners");
		Refresh();
		return false;
	}
	// drawn inside a sector: it starts as a copy of that one
	FVector2f Mid = FVector2f::ZeroVector;
	for (const FVector2f& P : DrawPoints) Mid += P;
	Mid /= DrawPoints.Num();
	const int32 Inside = DoomMapEdit::SectorInside(*EditMap, Mid);
	const FDoomSector Template = EditMap->Sectors.IsValidIndex(Inside) ? EditMap->Sectors[Inside] : DrawTemplate;
	FName Wall = DrawWall;
	if (Wall.IsNone())
	{
		const FDoomGraphics* Gfx = DoomWorld() ? DoomWorld()->GetGraphics() : nullptr;
		Wall = Gfx && Gfx->TextureNames().Num() > 1 ? Gfx->TextureNames()[1] : FName(TEXT("STARTAN3"));
	}
	Begin();
	FString Error;
	const int32 New = DoomMapEdit::DrawSector(*EditMap, DrawPoints, Template, Wall, Error);
	if (New == INDEX_NONE)
	{
		UndoStack.Pop();
		Status = TEXT("Can't draw that: ") + Error;
		Refresh();
		return false;
	}
	DrawPoints.Reset();
	Apply(true);
	Mode = EDoomEditMode::Sectors;
	Selected = New;
	Status = FString::Printf(TEXT("New sector %d"), New);
	Refresh();
	return true;
}

bool UDoomMapEditor::AddThing(const FVector2f& P, int32 Type, float Angle)
{
	if (!EditMap.IsValid())
	{
		return false;
	}
	Begin();
	FDoomMapThing T;
	T.P = P;
	T.Type = Type;
	T.Angle = Angle;
	T.Flags = 7;
	EditMap->Things.Add(T);
	Mode = EDoomEditMode::Things;
	Selected = EditMap->Things.Num() - 1;
	const FDoomThingDef* Def = DoomInfo::FindThing(Type);
	Status = FString::Printf(TEXT("Added %s at %.0f, %.0f"), Def ? *Def->Name : TEXT("thing"), P.X, P.Y);
	Apply(false);
	return true;
}

bool UDoomMapEditor::DeleteSelected()
{
	if (!EditMap.IsValid())
	{
		return false;
	}
	if (Mode == EDoomEditMode::Things && EditMap->Things.IsValidIndex(Selected))
	{
		Begin();
		EditMap->Things.RemoveAt(Selected);
		Selected = INDEX_NONE;
		Status = TEXT("Thing deleted");
		Apply(false);
		return true;
	}
	if (Mode == EDoomEditMode::Sectors && EditMap->Sectors.IsValidIndex(Selected))
	{
		if (EditMap->Sectors.Num() <= 1)
		{
			Status = TEXT("The last sector stays");
			Refresh();
			return false;
		}
		Begin();
		DoomMapEdit::DeleteSector(*EditMap, Selected);
		Selected = INDEX_NONE;
		Status = TEXT("Sector deleted");
		Apply(true);
		return true;
	}
	Status = TEXT("Select a thing or a sector to delete");
	Refresh();
	return false;
}

bool UDoomMapEditor::MoveSelected(const FVector2f& Delta)
{
	if (!EditMap.IsValid())
	{
		return false;
	}
	if (Mode == EDoomEditMode::Things && EditMap->Things.IsValidIndex(Selected))
	{
		Begin();
		EditMap->Things[Selected].P += Delta;
		Apply(false);
		return true;
	}
	if (Mode == EDoomEditMode::Vertices && EditMap->Vertices.IsValidIndex(Selected))
	{
		Begin();
		EditMap->Vertices[Selected] += Delta;
		Apply(true);
		return true;
	}
	return false;
}

bool UDoomMapEditor::Undo()
{
	if (!EditMap.IsValid() || UndoStack.Num() == 0)
	{
		Status = TEXT("Nothing to undo");
		Refresh();
		return false;
	}
	RedoStack.Add(DoomMapEdit::Take(*EditMap));
	DoomMapEdit::Restore(*EditMap, UndoStack.Pop());
	Selected = INDEX_NONE;
	Status = FString::Printf(TEXT("Undone (%d more)"), UndoStack.Num());
	Apply(false);
	return true;
}

bool UDoomMapEditor::Redo()
{
	if (!EditMap.IsValid() || RedoStack.Num() == 0)
	{
		Status = TEXT("Nothing to redo");
		Refresh();
		return false;
	}
	UndoStack.Add(DoomMapEdit::Take(*EditMap));
	DoomMapEdit::Restore(*EditMap, RedoStack.Pop());
	Selected = INDEX_NONE;
	Status = TEXT("Redone");
	Apply(false);
	return true;
}

int32 UDoomMapEditor::CheckMap()
{
	if (!EditMap.IsValid())
	{
		return -1;
	}
	const TArray<FDoomMapIssue> Issues = DoomMapCheck::Run(*EditMap, DoomWorld() ? DoomWorld()->GetGraphics() : nullptr, EditMap->Name);
	DoomMapCheck::Log(EditMap->Name, Issues);
	const int32 Errors = DoomMapCheck::Count(Issues, FDoomMapIssue::ESeverity::Error);
	const int32 Warnings = DoomMapCheck::Count(Issues, FDoomMapIssue::ESeverity::Warning);
	Status = FString::Printf(TEXT("Map check: %d errors, %d warnings%s%s"), Errors, Warnings, Issues.Num() ? TEXT(". First: ") : TEXT(""), Issues.Num() ? *Issues[0].Text : TEXT(""));
	if (Issues.Num())
	{
		ViewCenter = Issues[0].Where;
		if (Issues[0].Line != INDEX_NONE) { Mode = EDoomEditMode::Lines; Selected = Issues[0].Line; }
		else if (Issues[0].Sector != INDEX_NONE) { Mode = EDoomEditMode::Sectors; Selected = Issues[0].Sector; }
		else if (Issues[0].Thing != INDEX_NONE) { Mode = EDoomEditMode::Things; Selected = Issues[0].Thing; }
	}
	Refresh();
	return Errors;
}

bool UDoomMapEditor::SaveWad(const FString& Path)
{
	UDoomWorld* W = DoomWorld();
	if (!EditMap.IsValid() || !W)
	{
		return false;
	}
	// a slot any port knows: the map's own name if it is one, else the first map of this game
	const FString& Name = EditMap->Name;
	const bool bSlot = (Name.Len() == 4 && Name[0] == 'E' && Name[2] == 'M') || (Name.Len() == 5 && Name.StartsWith(TEXT("MAP")));
	const FString Slot = bSlot ? Name : (W->GetWad() && W->GetWad()->IsDoom2() ? TEXT("MAP01") : TEXT("E1M1"));
	const FString Out = Path.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("Maps") / Name + TEXT(".wad") : Path;
	FString Note;
	const bool bOk = DoomWadWriter::SaveMap(*EditMap, Slot, Out, &Note);
	Status = bOk ? FString::Printf(TEXT("Saved %s as %s (%s)"), *FPaths::ConvertRelativePathToFull(Out), *Slot, *Note) : FString::Printf(TEXT("Could not save %s"), *Out);
	Refresh();
	return bOk;
}

void UDoomMapEditor::NewRandomMap(uint32 Seed)
{
	UDoomWorld* W = DoomWorld();
	if (!W)
	{
		return;
	}
	const FString Name = DoomMapGen::NameForSeed(Seed ? Seed : (uint32)FMath::RandRange(1, 0xFFFFF));
	LoadFresh(Name);
	if (EditMap->Name != Name)
	{
		Status = FString::Printf(TEXT("No map from seed %s"), *Name);
		Refresh();
		return;
	}
	Apply(false);
	if (ADoomPlayer* P = W->GetPlayer())
	{
		// the start, not where the camera was in the old map
		for (const FDoomMapThing& T : EditMap->Things)
		{
			if (T.Type == 1)
			{
				P->SetActorLocation(Doom::ToUE(T.P, EditMap->Sectors[FMath::Max(0, DoomMapEdit::SectorInside(*EditMap, T.P))].Floor + 41.f), false, nullptr, ETeleportType::TeleportPhysics);
				break;
			}
		}
	}
	FitView();
	Status = FString::Printf(TEXT("New random map %s"), *Name);
	Refresh();
}

void UDoomMapEditor::NewEmptyMap()
{
	UDoomWorld* W = DoomWorld();
	const FDoomGraphics* Gfx = W ? W->GetGraphics() : nullptr;
	if (!W || !Gfx)
	{
		return;
	}
	auto Pick = [](const TArray<FName>& Names, const TCHAR* Want)
	{
		return Names.Contains(FName(Want)) ? FName(Want) : (Names.Num() > 1 ? Names[1] : FName(Want));
	};
	EditMap = MakeUnique<FDoomMap>();
	DoomMapEdit::MakeStarterMap(*EditMap, TEXT("NEWMAP"), Pick(Gfx->TextureNames(), TEXT("STARTAN3")), Pick(Gfx->FlatNames(), TEXT("FLOOR4_8")), Pick(Gfx->FlatNames(), TEXT("CEIL3_5")));
	UndoStack.Reset();
	RedoStack.Reset();
	Selected = INDEX_NONE;
	DrawTemplate = EditMap->Sectors[0];
	DrawWall = Pick(Gfx->TextureNames(), TEXT("STARTAN3"));
	if (ADoomPlayer* P = W->GetPlayer())
	{
		P->SetActorLocation(Doom::ToUE(FVector2f(256, 128), 41.f), false, nullptr, ETeleportType::TeleportPhysics);
	}
	Apply(false);
	FitView();
	Status = TEXT("A new room: draw sectors around it (D), add things (1, T), F5 to play");
	Refresh();
}

void UDoomMapEditor::FlyTick(float DeltaSeconds, const FVector& Move, const FVector2f& Look)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	APawn* P = PC ? PC->GetPawn() : nullptr;
	if (!PC || !P)
	{
		return;
	}
	FRotator R = PC->GetControlRotation();
	R.Yaw += Look.X * 0.25f;
	R.Pitch = FMath::ClampAngle(R.Pitch - Look.Y * 0.25f, -85.f, 85.f);
	PC->SetControlRotation(R);
	if (!Move.IsNearlyZero())
	{
		const FVector Fwd = R.Vector(), Right = FRotationMatrix(R).GetUnitAxis(EAxis::Y);
		const FVector D = (Fwd * Move.X + Right * Move.Y + FVector::UpVector * Move.Z) * 400.f * Doom::Scale * DeltaSeconds;
		P->SetActorLocation(P->GetActorLocation() + D, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void UDoomMapEditor::PickInView(const FVector2D& ScreenPos)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !EditMap.IsValid())
	{
		return;
	}
	float MX = 0, MY = 0;
	FVector2D At = ScreenPos;
	if (PC->GetMousePosition(MX, MY)) At = FVector2D(MX, MY);
	FVector From, Dir;
	if (!PC->DeprojectScreenPositionToWorld(At.X, At.Y, From, Dir))
	{
		return;
	}
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomEditorPick), false, PC->GetPawn());
	if (!GetWorld()->LineTraceSingleByChannel(Hit, From, From + Dir * 20000.f, ECC_Visibility, Q))
	{
		return;
	}
	// on a wall, the sector is the one in front of it (towards the camera)
	const FVector2f P = Doom::ToDoom2D(Hit.ImpactPoint - Dir * 2.f);
	SelectAt(P, Mode == EDoomEditMode::Things ? 48.f : 24.f);
	Status = FString::Printf(TEXT("Picked %s %d"), *StaticEnum<EDoomEditMode>()->GetNameStringByValue((int64)Mode), Selected);
	Refresh();
}
