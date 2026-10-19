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
// The map view
// ------------------------------------------------------------------------------------------------

class SDoomMapView : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SDoomMapView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TWeakObjectPtr<UDoomMapEditor> InEditor)
	{
		Editor = InEditor;
		SetCanTick(true);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(200, 200); }
	virtual bool SupportsKeyboardFocus() const override { return true; }

	FVector2f ToLocal(const UDoomMapEditor& E, const FVector2f& P) const
	{
		return FVector2f((P.X - E.ViewCenter.X) * E.Zoom + Size.X * 0.5f, (E.ViewCenter.Y - P.Y) * E.Zoom + Size.Y * 0.5f);
	}
	FVector2f ToMap(const UDoomMapEditor& E, const FVector2f& L) const
	{
		return FVector2f((L.X - Size.X * 0.5f) / E.Zoom + E.ViewCenter.X, E.ViewCenter.Y - (L.Y - Size.Y * 0.5f) / E.Zoom);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& G, const FSlateRect& Cull, FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const override
	{
		Size = FVector2f(G.GetLocalSize());
		const UDoomMapEditor* E = Editor.Get();
		const FDoomMap* M = E ? E->GetMap() : nullptr;
		if (!M)
		{
			return Layer;
		}
		const FSlateBrush* White = FCoreStyle::Get().GetBrush("GenericWhiteBox");
		const FPaintGeometry PG = G.ToPaintGeometry();
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 10);
		if (E->b3D)
		{
			// the game shows through; a cross marks the middle
			const FVector2f C = Size * 0.5f;
			FSlateDrawElement::MakeLines(Out, Layer, PG, { C - FVector2f(8, 0), C + FVector2f(8, 0) }, ESlateDrawEffect::None, FLinearColor(1, 1, 0.3f, 0.8f));
			FSlateDrawElement::MakeLines(Out, Layer, PG, { C - FVector2f(0, 8), C + FVector2f(0, 8) }, ESlateDrawEffect::None, FLinearColor(1, 1, 0.3f, 0.8f));
			FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(FVector2f(600, 20), FSlateLayoutTransform(FVector2f(12, Size.Y - 26))),
				TEXT("3D: WASD fly, Q/E down/up, Shift faster, right mouse to look, click to pick, Tab for the map"), Font, ESlateDrawEffect::None, FLinearColor(0.9f, 0.9f, 0.6f));
			return Layer + 1;
		}
		FSlateDrawElement::MakeBox(Out, Layer, PG, White, ESlateDrawEffect::None, FLinearColor(0.015f, 0.015f, 0.025f, 1.f));
		++Layer;
		// the grid: 64-unit squares when there is room for them
		const float Step = E->Zoom * 64.f >= 10.f ? 64.f : E->Zoom * 512.f >= 10.f ? 512.f : 0.f;
		if (Step > 0)
		{
			const FVector2f Lo = ToMap(*E, FVector2f(0, Size.Y)), Hi = ToMap(*E, FVector2f(Size.X, 0));
			const FLinearColor GridColor(0.05f, 0.06f, 0.09f);
			for (float X = FMath::FloorToFloat(Lo.X / Step) * Step; X <= Hi.X; X += Step)
			{
				const float LX = ToLocal(*E, FVector2f(X, 0)).X;
				FSlateDrawElement::MakeLines(Out, Layer, PG, { FVector2f(LX, 0), FVector2f(LX, Size.Y) }, ESlateDrawEffect::None, GridColor, false);
			}
			for (float Y = FMath::FloorToFloat(Lo.Y / Step) * Step; Y <= Hi.Y; Y += Step)
			{
				const float LY = ToLocal(*E, FVector2f(0, Y)).Y;
				FSlateDrawElement::MakeLines(Out, Layer, PG, { FVector2f(0, LY), FVector2f(Size.X, LY) }, ESlateDrawEffect::None, GridColor, false);
			}
		}
		++Layer;
		// the lines: walls white, steps and openings grey, specials cyan, the selection orange
		const bool bSectorMode = E->Mode == EDoomEditMode::Sectors;
		for (int32 l = 0; l < M->Lines.Num(); ++l)
		{
			const FDoomLine& L = M->Lines[l];
			if (!M->Vertices.IsValidIndex(L.V1) || !M->Vertices.IsValidIndex(L.V2)) continue;
			const FVector2f A = ToLocal(*E, M->V(L.V1)), B = ToLocal(*E, M->V(L.V2));
			FLinearColor C = L.Side[1] == INDEX_NONE ? FLinearColor(0.85f, 0.85f, 0.85f) : FLinearColor(0.42f, 0.42f, 0.5f);
			if (L.Special) C = FLinearColor(0.3f, 0.85f, 1.f);
			float Thick = 1.f;
			const bool bSel = (E->Mode == EDoomEditMode::Lines && E->Selected == l)
				|| (bSectorMode && E->Selected != INDEX_NONE && (L.FrontSector == E->Selected || L.BackSector == E->Selected));
			const bool bHover = (E->Mode == EDoomEditMode::Lines && E->Hovered == l)
				|| (bSectorMode && E->Hovered != INDEX_NONE && (L.FrontSector == E->Hovered || L.BackSector == E->Hovered));
			if (bHover) { C = FLinearColor::LerpUsingHSV(C, FLinearColor(1.f, 0.85f, 0.3f), 0.6f); Thick = 2.f; }
			if (bSel) { C = FLinearColor(1.f, 0.55f, 0.1f); Thick = 2.5f; }
			FSlateDrawElement::MakeLines(Out, Layer, PG, { A, B }, ESlateDrawEffect::None, C, true, Thick);
			if (E->Mode == EDoomEditMode::Lines || bSel)
			{
				// which way the front faces
				const FVector2f Mid = (A + B) * 0.5f, D = (B - A).GetSafeNormal();
				FSlateDrawElement::MakeLines(Out, Layer, PG, { Mid, Mid + FVector2f(-D.Y, D.X) * 6.f }, ESlateDrawEffect::None, C, true, 1.f);
			}
		}
		++Layer;
		if (E->Mode == EDoomEditMode::Vertices)
		{
			TBitArray<> Used(false, M->Vertices.Num());
			for (const FDoomLine& L : M->Lines) { Used[L.V1] = true; Used[L.V2] = true; }
			for (int32 v = 0; v < M->Vertices.Num(); ++v)
			{
				if (!Used[v]) continue;
				const FVector2f P = ToLocal(*E, M->Vertices[v]);
				const float R = v == E->Selected ? 4.f : v == E->Hovered ? 3.5f : 2.5f;
				const FLinearColor C = v == E->Selected ? FLinearColor(1.f, 0.55f, 0.1f) : FLinearColor(0.6f, 0.9f, 0.6f);
				FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(FVector2f(R * 2, R * 2), FSlateLayoutTransform(P - FVector2f(R, R))), White, ESlateDrawEffect::None, C);
			}
		}
		// things: starts green, monsters red, pickups blue, keys yellow, the rest grey
		for (int32 t = 0; t < M->Things.Num(); ++t)
		{
			const FDoomMapThing& T = M->Things[t];
			const FDoomThingDef* Def = DoomInfo::FindThing(T.Type);
			FLinearColor C(0.55f, 0.55f, 0.55f);
			if ((T.Type >= 1 && T.Type <= 4) || T.Type == 11) C = FLinearColor(0.2f, 1.f, 0.3f);
			else if (T.Type == 5 || T.Type == 6 || T.Type == 13 || (T.Type >= 38 && T.Type <= 40)) C = FLinearColor(1.f, 0.95f, 0.2f);
			else if (Def && (Def->Flags & EThingFlag::CountKill)) C = FLinearColor(1.f, 0.25f, 0.2f);
			else if (Def && (Def->Flags & EThingFlag::Pickup)) C = FLinearColor(0.35f, 0.55f, 1.f);
			const float R = FMath::Clamp((Def ? Def->Radius : 16.f) * E->Zoom, 2.f, 40.f);
			const FVector2f P = ToLocal(*E, T.P);
			const bool bSel = E->Mode == EDoomEditMode::Things && E->Selected == t;
			const bool bHover = E->Mode == EDoomEditMode::Things && E->Hovered == t;
			const float Alpha = E->Mode == EDoomEditMode::Things ? 1.f : 0.55f;
			FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(FVector2f(R * 2, R * 2), FSlateLayoutTransform(P - FVector2f(R, R))), White, ESlateDrawEffect::None, C.CopyWithNewOpacity(0.35f * Alpha));
			const FLinearColor Edge = bSel ? FLinearColor(1.f, 0.55f, 0.1f) : bHover ? FLinearColor(1, 1, 1) : C.CopyWithNewOpacity(Alpha);
			FSlateDrawElement::MakeLines(Out, Layer, PG, { P + FVector2f(-R, -R), P + FVector2f(R, -R), P + FVector2f(R, R), P + FVector2f(-R, R), P + FVector2f(-R, -R) },
				ESlateDrawEffect::None, Edge, true, bSel ? 2.f : 1.f);
			const float An = FMath::DegreesToRadians(T.Angle);
			FSlateDrawElement::MakeLines(Out, Layer, PG, { P, P + FVector2f(FMath::Cos(An), -FMath::Sin(An)) * R }, ESlateDrawEffect::None, Edge, true, 1.f);
		}
		// where the player (the camera) is
		if (const APlayerController* PC = E->GetWorld() ? E->GetWorld()->GetFirstPlayerController() : nullptr; PC && PC->GetPawn())
		{
			const FVector2f P = ToLocal(*E, Doom::ToDoom2D(PC->GetPawn()->GetActorLocation()));
			const float Yaw = FMath::DegreesToRadians(PC->GetControlRotation().Yaw);
			const FVector2f D(FMath::Cos(Yaw), FMath::Sin(Yaw));
			FSlateDrawElement::MakeLines(Out, Layer, PG, { P - D * 6.f + FVector2f(-D.Y, D.X) * 5.f, P + D * 9.f, P - D * 6.f - FVector2f(-D.Y, D.X) * 5.f },
				ESlateDrawEffect::None, FLinearColor(0.9f, 0.9f, 0.9f), true, 1.5f);
		}
		++Layer;
		// the outline being drawn, and the next edge to the cursor
		if (E->Mode == EDoomEditMode::Draw)
		{
			TArray<FVector2f> Pts;
			for (const FVector2f& P : E->DrawPoints) Pts.Add(ToLocal(*E, P));
			const FVector2f Cur = ToLocal(*E, E->Cursor);
			if (Pts.Num() > 0)
			{
				Pts.Add(Cur);
				FSlateDrawElement::MakeLines(Out, Layer, PG, Pts, ESlateDrawEffect::None, FLinearColor(1.f, 0.9f, 0.2f), true, 2.f);
			}
			FSlateDrawElement::MakeBox(Out, Layer, G.ToPaintGeometry(FVector2f(5, 5), FSlateLayoutTransform(Cur - FVector2f(2.5f, 2.5f))), White, ESlateDrawEffect::None, FLinearColor(1.f, 0.9f, 0.2f));
		}
		const FString Info = FString::Printf(TEXT("%s   x %.0f  y %.0f   grid %.0f   zoom %.2f   (wheel zoom, right drag pan, Tab 3D, Ctrl+Z undo, Del delete, Enter closes an outline)"),
			*StaticEnum<EDoomEditMode>()->GetNameStringByValue((int64)E->Mode), E->Cursor.X, E->Cursor.Y, E->Grid, E->Zoom);
		FSlateDrawElement::MakeText(Out, Layer, G.ToPaintGeometry(FVector2f(900, 20), FSlateLayoutTransform(FVector2f(12, Size.Y - 26))), Info, Font, ESlateDrawEffect::None, FLinearColor(0.7f, 0.75f, 0.85f));
		return Layer + 1;
	}

	virtual FReply OnMouseButtonDown(const FGeometry& G, const FPointerEvent& Ev) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (!E || !E->GetMap())
		{
			return FReply::Unhandled();
		}
		const FVector2f Local = FVector2f(G.AbsoluteToLocal(Ev.GetScreenSpacePosition()));
		FReply Reply = FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this), EFocusCause::Mouse);
		if (E->b3D)
		{
			if (Ev.GetEffectingButton() == EKeys::RightMouseButton) bLooking = true;
			else if (Ev.GetEffectingButton() == EKeys::LeftMouseButton) E->PickInView(FVector2D(Local) * G.Scale);
			return Reply;
		}
		if (Ev.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			bPanning = true;
			LastMouse = Local;
			return Reply;
		}
		const FVector2f P = ToMap(*E, Local);
		E->Cursor = DoomMapEdit::Snap(P, E->Grid);
		if (E->Mode == EDoomEditMode::Draw)
		{
			// a click on the first corner closes the outline
			if (E->DrawPoints.Num() >= 3 && FVector2f::Distance(ToLocal(*E, E->DrawPoints[0]), Local) < 8.f)
			{
				E->FinishDraw();
			}
			else
			{
				E->DrawPoints.Add(E->Cursor);
			}
			return Reply;
		}
		E->SelectAt(P, 10.f / E->Zoom);
		if ((E->Mode == EDoomEditMode::Things || E->Mode == EDoomEditMode::Vertices) && E->Selected != INDEX_NONE)
		{
			bDragging = true;
			DragStart = E->Cursor;
		}
		return Reply;
	}

	virtual FReply OnMouseButtonUp(const FGeometry& G, const FPointerEvent& Ev) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (E && bDragging && !E->Cursor.Equals(DragStart, 0.01f))
		{
			E->MoveSelected(E->Cursor - DragStart);
		}
		bDragging = bPanning = bLooking = false;
		return FReply::Handled().ReleaseMouseCapture();
	}

	virtual FReply OnMouseMove(const FGeometry& G, const FPointerEvent& Ev) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (!E || !E->GetMap())
		{
			return FReply::Unhandled();
		}
		const FVector2f Local = FVector2f(G.AbsoluteToLocal(Ev.GetScreenSpacePosition()));
		if (E->b3D)
		{
			if (bLooking) LookDelta += FVector2f(Ev.GetCursorDelta());
			return FReply::Handled();
		}
		if (bPanning)
		{
			const FVector2f D = Local - LastMouse;
			E->ViewCenter += FVector2f(-D.X, D.Y) / E->Zoom;
			LastMouse = Local;
		}
		const FVector2f P = ToMap(*E, Local);
		E->Cursor = DoomMapEdit::Snap(P, E->Grid);
		const FDoomMap& M = *E->GetMap();
		const float R = 10.f / E->Zoom;
		switch (E->Mode)
		{
		case EDoomEditMode::Things: E->Hovered = DoomMapEdit::ThingNear(M, P, R); break;
		case EDoomEditMode::Sectors: E->Hovered = DoomMapEdit::SectorInside(M, P); break;
		case EDoomEditMode::Lines: E->Hovered = DoomMapEdit::LineNear(M, P, R); break;
		case EDoomEditMode::Vertices: E->Hovered = DoomMapEdit::VertexNear(M, P, R); break;
		default: E->Hovered = INDEX_NONE; break;
		}
		return FReply::Handled();
	}

	virtual FReply OnMouseWheel(const FGeometry& G, const FPointerEvent& Ev) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (!E || E->b3D)
		{
			return FReply::Unhandled();
		}
		// zoom about the point under the cursor
		const FVector2f Local = FVector2f(G.AbsoluteToLocal(Ev.GetScreenSpacePosition()));
		const FVector2f Before = ToMap(*E, Local);
		E->Zoom = FMath::Clamp(E->Zoom * (Ev.GetWheelDelta() > 0 ? 1.25f : 0.8f), 0.02f, 8.f);
		const FVector2f After = ToMap(*E, Local);
		E->ViewCenter += Before - After;
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonDoubleClick(const FGeometry& G, const FPointerEvent& Ev) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (!E || E->b3D || Ev.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}
		if (E->Mode == EDoomEditMode::Draw && E->DrawPoints.Num() >= 3)
		{
			E->FinishDraw();
		}
		else if (E->Mode == EDoomEditMode::Things && E->Selected == INDEX_NONE)
		{
			E->AddThing(E->Cursor, E->PlaceType);
		}
		return FReply::Handled();
	}

	virtual FReply OnKeyDown(const FGeometry& G, const FKeyEvent& Ev) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (!E)
		{
			return FReply::Unhandled();
		}
		const FKey K = Ev.GetKey();
		Held.Add(K);
		if (Ev.IsControlDown() && K == EKeys::Z) { E->Undo(); return FReply::Handled(); }
		if (Ev.IsControlDown() && K == EKeys::Y) { E->Redo(); return FReply::Handled(); }
		if (Ev.IsControlDown() && K == EKeys::S) { E->SaveWad(); return FReply::Handled(); }
		if (K == EKeys::Tab) { E->b3D = !E->b3D; return FReply::Handled(); }
		if (E->b3D)
		{
			return FReply::Handled(); // the fly keys are read in Tick
		}
		if (K == EKeys::Escape) { E->DrawPoints.Reset(); E->Selected = INDEX_NONE; E->Refresh(); }
		else if (K == EKeys::Enter && E->Mode == EDoomEditMode::Draw) E->FinishDraw();
		else if (K == EKeys::BackSpace && E->Mode == EDoomEditMode::Draw && E->DrawPoints.Num()) E->DrawPoints.Pop();
		else if (K == EKeys::Delete || K == EKeys::BackSpace) E->DeleteSelected();
		else if (K == EKeys::One) E->SetMode(EDoomEditMode::Things);
		else if (K == EKeys::Two) E->SetMode(EDoomEditMode::Sectors);
		else if (K == EKeys::Three) E->SetMode(EDoomEditMode::Lines);
		else if (K == EKeys::Four) E->SetMode(EDoomEditMode::Vertices);
		else if (K == EKeys::D) E->SetMode(EDoomEditMode::Draw);
		else if (K == EKeys::T) E->AddThing(E->Cursor, E->PlaceType);
		else if (K == EKeys::G) { static const float Grids[] = { 1, 8, 16, 32, 64 }; int32 i = 0; while (i < 5 && Grids[i] <= E->Grid) ++i; E->Grid = Grids[i % 5]; }
		else if (K == EKeys::F5) E->Close();
		else return FReply::Unhandled();
		return FReply::Handled();
	}

	virtual FReply OnKeyUp(const FGeometry& G, const FKeyEvent& Ev) override
	{
		Held.Remove(Ev.GetKey());
		return FReply::Handled();
	}

	virtual void Tick(const FGeometry& G, const double CurrentTime, const float DeltaTime) override
	{
		UDoomMapEditor* E = Editor.Get();
		if (!E || !E->b3D)
		{
			LookDelta = FVector2f::ZeroVector;
			return;
		}
		FVector Move = FVector::ZeroVector;
		Move.X += Held.Contains(EKeys::W) - Held.Contains(EKeys::S);
		Move.Y += Held.Contains(EKeys::D) - Held.Contains(EKeys::A);
		Move.Z += Held.Contains(EKeys::E) - Held.Contains(EKeys::Q);
		if (Held.Contains(EKeys::LeftShift)) Move *= 3.f;
		E->FlyTick(DeltaTime, Move, LookDelta);
		LookDelta = FVector2f::ZeroVector;
	}

	virtual FCursorReply OnCursorQuery(const FGeometry& G, const FPointerEvent& Ev) const override
	{
		const UDoomMapEditor* E = Editor.Get();
		return FCursorReply::Cursor(E && E->Mode == EDoomEditMode::Draw && !E->b3D ? EMouseCursor::Crosshairs : EMouseCursor::Default);
	}

private:
	TWeakObjectPtr<UDoomMapEditor> Editor;
	mutable FVector2f Size = FVector2f(1280, 720);
	bool bPanning = false, bDragging = false, bLooking = false;
	FVector2f DragStart = FVector2f::ZeroVector;
	FVector2f LastMouse = FVector2f::ZeroVector;
	FVector2f LookDelta = FVector2f::ZeroVector;
	TSet<FKey> Held;
};

TSharedRef<SWidget> UDoomMapView::RebuildWidget()
{
	View = SNew(SDoomMapView, Editor);
	return View.ToSharedRef();
}

void UDoomMapView::ReleaseSlateResources(bool bReleaseChildren)
{
	Super::ReleaseSlateResources(bReleaseChildren);
	View.Reset();
}

void UDoomMapView::FocusView()
{
	if (View.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetAllUserFocus(View, EFocusCause::SetDirectly);
	}
}

// ------------------------------------------------------------------------------------------------
// Panel controls
// ------------------------------------------------------------------------------------------------

void UDoomEditButton::Bind() { OnClicked.AddUniqueDynamic(this, &UDoomEditButton::Clicked); }
void UDoomEditButton::Clicked() { if (Owner.IsValid()) Owner->OnAction(Action); }
void UDoomEditSpin::Bind() { OnValueCommitted.AddUniqueDynamic(this, &UDoomEditSpin::Committed); }
void UDoomEditSpin::Committed(float InValue, ETextCommit::Type CommitMethod) { if (Owner.IsValid()) Owner->OnNumber(Field, InValue); }
void UDoomEditCombo::Bind() { OnSelectionChanged.AddUniqueDynamic(this, &UDoomEditCombo::Changed); }
void UDoomEditCombo::Changed(FString Item, ESelectInfo::Type SelectionType)
{
	if (Owner.IsValid() && SelectionType != ESelectInfo::Direct) Owner->OnName(Field, Item);
}
void UDoomEditCheck::Bind() { OnCheckStateChanged.AddUniqueDynamic(this, &UDoomEditCheck::Changed); }
void UDoomEditCheck::Changed(bool bIsChecked) { if (Owner.IsValid()) Owner->OnFlag(Field, Bit, bIsChecked); }

// ------------------------------------------------------------------------------------------------
// The panels
// ------------------------------------------------------------------------------------------------

TSharedRef<SWidget> UDoomEditorWidget::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		Build();
	}
	return Super::RebuildWidget();
}

UTextBlock* UDoomEditorWidget::Label(const FString& Text, int32 Size)
{
	UTextBlock* T = WidgetTree->ConstructWidget<UTextBlock>();
	T->SetText(FText::FromString(Text));
	T->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", Size));
	T->SetColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.92f, 0.95f)));
	return T;
}

UDoomEditButton* UDoomEditorWidget::MakeButton(const FString& Text, int32 Action)
{
	UDoomEditButton* B = WidgetTree->ConstructWidget<UDoomEditButton>(UDoomEditButton::StaticClass());
	B->Action = Action;
	B->Owner = this;
	B->Bind();
	B->SetStyle(ButtonStyle(false));
	UTextBlock* T = Label(Text, 9);
	T->SetColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.95f, 0.97f)));
	B->AddChild(T);
	return B;
}

FButtonStyle UDoomEditorWidget::ButtonStyle(bool bActive)
{
	// flat buttons: dark slate, the active mode orange
	FButtonStyle S = FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
	const FLinearColor Base = bActive ? FLinearColor(0.85f, 0.42f, 0.08f) : FLinearColor(0.17f, 0.19f, 0.25f);
	for (FSlateBrush* Brush : { &S.Normal, &S.Hovered, &S.Pressed })
	{
		Brush->DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush->OutlineSettings.CornerRadii = FVector4(3, 3, 3, 3);
		Brush->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		Brush->TintColor = FSlateColor(Base);
	}
	S.Hovered.TintColor = FSlateColor(Base * 1.5f);
	S.Pressed.TintColor = FSlateColor(Base * 0.7f);
	S.NormalPadding = FMargin(8, 3);
	S.PressedPadding = FMargin(8, 4, 8, 2);
	return S;
}

void UDoomEditorWidget::Build()
{
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Root;

	MapView = WidgetTree->ConstructWidget<UDoomMapView>(UDoomMapView::StaticClass());
	MapView->Editor = Editor;
	UCanvasPanelSlot* MS = Root->AddChildToCanvas(MapView);
	MS->SetAnchors(FAnchors(0, 0, 1, 1));
	MS->SetOffsets(FMargin(0));

	// the toolbar
	UBorder* Bar = WidgetTree->ConstructWidget<UBorder>();
	Bar->SetBrushColor(FLinearColor(0.03f, 0.03f, 0.05f, 0.88f));
	Bar->SetPadding(FMargin(6, 4));
	UVerticalBox* BarRows = WidgetTree->ConstructWidget<UVerticalBox>();
	Bar->SetContent(BarRows);
	TitleText = Label(TEXT("DOOM MAP EDITOR"), 12);
	BarRows->AddChildToVerticalBox(TitleText);
	auto AddButtons = [&](TArray<TPair<FString, int32>> Items, bool bModes)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		for (const TPair<FString, int32>& I : Items)
		{
			UDoomEditButton* B = MakeButton(I.Key, I.Value);
			if (bModes) ModeButtons.Add(B);
			UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(B);
			S->SetPadding(FMargin(0, 2, 4, 2));
		}
		BarRows->AddChildToVerticalBox(Row);
	};
	AddButtons({ {TEXT("1 Things"), ModeThings}, {TEXT("2 Sectors"), ModeSectors}, {TEXT("3 Lines"), ModeLines}, {TEXT("4 Vertices"), ModeVertices}, {TEXT("D Draw sector"), ModeDraw} }, true);
	AddButtons({ {TEXT("Undo"), ActUndo}, {TEXT("Redo"), ActRedo}, {TEXT("Tab 2D / 3D"), ActView}, {TEXT("Check map"), ActCheck}, {TEXT("Save WAD"), ActSave},
		{TEXT("Random map"), ActRandom}, {TEXT("New room"), ActEmpty}, {TEXT("F5 Play"), ActPlay} }, false);
	UCanvasPanelSlot* BS = Root->AddChildToCanvas(Bar);
	BS->SetAutoSize(true);
	BS->SetPosition(FVector2D(8, 8));

	// the properties
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrushColor(FLinearColor(0.03f, 0.03f, 0.05f, 0.88f));
	Panel->SetPadding(FMargin(8));
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>();
	Panel->SetContent(Scroll);
	Props = WidgetTree->ConstructWidget<UVerticalBox>();
	Scroll->AddChild(Props);
	UCanvasPanelSlot* PS = Root->AddChildToCanvas(Panel);
	PS->SetAnchors(FAnchors(1, 0, 1, 1));
	PS->SetOffsets(FMargin(-330, 8, 320, 44));
	PS->SetAlignment(FVector2D(0, 0));

	// the status line
	UBorder* StatusBar = WidgetTree->ConstructWidget<UBorder>();
	StatusBar->SetBrushColor(FLinearColor(0.03f, 0.03f, 0.05f, 0.88f));
	StatusBar->SetPadding(FMargin(8, 3));
	StatusText = Label(TEXT(""), 10);
	StatusBar->SetContent(StatusText);
	UCanvasPanelSlot* SS = Root->AddChildToCanvas(StatusBar);
	SS->SetAnchors(FAnchors(0, 1, 1, 1));
	SS->SetOffsets(FMargin(8, -66, 346, 26));
	SS->SetAlignment(FVector2D(0, 0));
}

void UDoomEditorWidget::AddRow(const FString& Name, UWidget* Control)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	USizeBox* LabelBox = WidgetTree->ConstructWidget<USizeBox>();
	LabelBox->SetWidthOverride(110);
	LabelBox->SetContent(Label(Name, 9));
	UHorizontalBoxSlot* LS = Row->AddChildToHorizontalBox(LabelBox);
	LS->SetVerticalAlignment(VAlign_Center);
	UHorizontalBoxSlot* CS = Row->AddChildToHorizontalBox(Control);
	CS->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBoxSlot* VS = Props->AddChildToVerticalBox(Row);
	VS->SetPadding(FMargin(0, 2));
}

void UDoomEditorWidget::AddNumber(const FString& Name, EDoomEditField Field, float Value, float Min, float Max, bool bInt)
{
	UDoomEditSpin* S = WidgetTree->ConstructWidget<UDoomEditSpin>(UDoomEditSpin::StaticClass());
	S->Field = Field;
	S->Owner = this;
	S->SetMinValue(Min);
	S->SetMaxValue(Max);
	S->SetMinSliderValue(Min);
	S->SetMaxSliderValue(Max);
	S->SetMinFractionalDigits(0);
	S->SetMaxFractionalDigits(bInt ? 0 : 1);
	S->SetAlwaysUsesDeltaSnap(bInt);
	S->SetDelta(bInt ? 1.f : 0.f);
	S->SetValue(Value);
	S->SetForegroundColor(FSlateColor(FLinearColor(0.02f, 0.02f, 0.03f)));
	S->Bind();
	AddRow(Name, S);
}

void UDoomEditorWidget::AddNames(const FString& Name, EDoomEditField Field, FName Value, const TArray<FName>& Options)
{
	UDoomEditCombo* C = WidgetTree->ConstructWidget<UDoomEditCombo>(UDoomEditCombo::StaticClass());
	C->Field = Field;
	C->Owner = this;
	C->AddOption(TEXT("-"));
	for (const FName& N : Options) C->AddOption(N.ToString());
	C->SetSelectedOption(Value.IsNone() ? FString(TEXT("-")) : Value.ToString());
	C->Bind();
	AddRow(Name, C);
}

void UDoomEditorWidget::AddCheck(const FString& Name, EDoomEditField Field, int32 Bit, bool bOn)
{
	UDoomEditCheck* C = WidgetTree->ConstructWidget<UDoomEditCheck>(UDoomEditCheck::StaticClass());
	C->Field = Field;
	C->Bit = Bit;
	C->Owner = this;
	C->SetIsChecked(bOn);
	C->Bind();
	AddRow(Name, C);
}

void UDoomEditorWidget::RefreshPanels()
{
	UDoomMapEditor* E = Editor.Get();
	if (!E || !Props)
	{
		return;
	}
	const FDoomMap* M = E->GetMap();
	if (TitleText && M)
	{
		TitleText->SetText(FText::FromString(FString::Printf(TEXT("DOOM MAP EDITOR - %s: %d sectors, %d lines, %d things%s"), *M->Name,
			M->Sectors.Num(), M->Lines.Num(), M->Things.Num(), E->NumUndo() ? *FString::Printf(TEXT(", %d edits"), E->NumUndo()) : TEXT(""))));
	}
	for (int32 i = 0; i < ModeButtons.Num(); ++i)
	{
		ModeButtons[i]->SetStyle(ButtonStyle(i == (int32)E->Mode));
	}
	if (StatusText)
	{
		StatusText->SetText(FText::FromString(E->Status));
	}
	Props->ClearChildren();
	if (!M)
	{
		return;
	}
	const FDoomGraphics* Gfx = E->GetWorld() && E->GetWorld()->GetSubsystem<UDoomWorld>() ? E->GetWorld()->GetSubsystem<UDoomWorld>()->GetGraphics() : nullptr;
	const TArray<FName> NoNames;
	const TArray<FName>& Flats = Gfx ? Gfx->FlatNames() : NoNames;
	const TArray<FName>& Textures = Gfx ? Gfx->TextureNames() : NoNames;
	auto Heading = [&](const FString& Text)
	{
		UVerticalBoxSlot* S = Props->AddChildToVerticalBox(Label(Text, 11));
		S->SetPadding(FMargin(0, 6, 0, 4));
	};
	const int32 Sel = E->Selected;
	switch (E->Mode)
	{
	case EDoomEditMode::Sectors:
		if (M->Sectors.IsValidIndex(Sel))
		{
			const FDoomSector& S = M->Sectors[Sel];
			Heading(FString::Printf(TEXT("Sector %d"), Sel));
			AddNumber(TEXT("Floor"), EDoomEditField::SectorFloor, S.Floor, -32768, 32767);
			AddNumber(TEXT("Ceiling"), EDoomEditField::SectorCeiling, S.Ceiling, -32768, 32767);
			AddNumber(TEXT("Light"), EDoomEditField::SectorLight, S.Light, 0, 255);
			AddNumber(TEXT("Special"), EDoomEditField::SectorSpecial, S.Special, 0, 32767);
			AddNumber(TEXT("Tag"), EDoomEditField::SectorTag, S.Tag, 0, 32767);
			AddNames(TEXT("Floor flat"), EDoomEditField::SectorFloorPic, S.FloorPic, Flats);
			AddNames(TEXT("Ceiling flat"), EDoomEditField::SectorCeilingPic, S.CeilingPic, Flats);
			Props->AddChildToVerticalBox(MakeButton(TEXT("Delete sector"), ActDelete));
		}
		else Heading(TEXT("Click a sector to edit it"));
		break;
	case EDoomEditMode::Lines:
		if (M->Lines.IsValidIndex(Sel))
		{
			const FDoomLine& L = M->Lines[Sel];
			Heading(FString::Printf(TEXT("Line %d (%s)"), Sel, L.Side[1] != INDEX_NONE ? TEXT("two-sided") : TEXT("one-sided")));
			AddNumber(TEXT("Special"), EDoomEditField::LineSpecial, L.Special, 0, 32767);
			AddNumber(TEXT("Tag"), EDoomEditField::LineTag, L.Tag, 0, 32767);
			static const TPair<const TCHAR*, int32> Flags[] = { {TEXT("Impassable"), DoomLineFlags::Blocking}, {TEXT("Blocks monsters"), DoomLineFlags::BlockMonsters},
				{TEXT("Upper unpegged"), DoomLineFlags::DontPegTop}, {TEXT("Lower unpegged"), DoomLineFlags::DontPegBottom}, {TEXT("Secret"), DoomLineFlags::Secret},
				{TEXT("Blocks sound"), DoomLineFlags::SoundBlock}, {TEXT("Not on map"), DoomLineFlags::DontDraw} };
			for (const TPair<const TCHAR*, int32>& F : Flags) AddCheck(F.Key, EDoomEditField::LineFlag, F.Value, (L.Flags & F.Value) != 0);
			for (int32 s = 0; s < 2; ++s)
			{
				if (L.Side[s] == INDEX_NONE) continue;
				const FDoomSide& Sd = M->Sides[L.Side[s]];
				Heading(FString::Printf(TEXT("%s side (sector %d)"), s ? TEXT("Back") : TEXT("Front"), Sd.Sector));
				AddNames(TEXT("Upper"), s ? EDoomEditField::BackUpper : EDoomEditField::FrontUpper, Sd.Upper, Textures);
				AddNames(TEXT("Middle"), s ? EDoomEditField::BackMiddle : EDoomEditField::FrontMiddle, Sd.Middle, Textures);
				AddNames(TEXT("Lower"), s ? EDoomEditField::BackLower : EDoomEditField::FrontLower, Sd.Lower, Textures);
				if (s == 0)
				{
					AddNumber(TEXT("X offset"), EDoomEditField::FrontXOffset, Sd.XOffset, -1024, 1024);
					AddNumber(TEXT("Y offset"), EDoomEditField::FrontYOffset, Sd.YOffset, -1024, 1024);
				}
			}
		}
		else Heading(TEXT("Click a line to edit it"));
		break;
	case EDoomEditMode::Things:
	{
		TArray<FName> Types;
		for (const FDoomThingDef& D : DoomInfo::AllThings()) Types.Add(FName(*FString::Printf(TEXT("%d %s"), D.DoomEdNum, *D.Name)));
		auto TypeName = [](int32 Type)
		{
			const FDoomThingDef* D = DoomInfo::FindThing(Type);
			return FName(*FString::Printf(TEXT("%d %s"), Type, D ? *D->Name : TEXT("?")));
		};
		if (M->Things.IsValidIndex(Sel))
		{
			const FDoomMapThing& T = M->Things[Sel];
			Heading(FString::Printf(TEXT("Thing %d"), Sel));
			AddNames(TEXT("Type"), EDoomEditField::ThingType, TypeName(T.Type), Types);
			AddNumber(TEXT("Angle"), EDoomEditField::ThingAngle, T.Angle, 0, 359);
			static const TPair<const TCHAR*, int32> Flags[] = { {TEXT("Easy"), 1}, {TEXT("Medium"), 2}, {TEXT("Hard"), 4}, {TEXT("Ambush"), 8}, {TEXT("Multiplayer only"), 16} };
			for (const TPair<const TCHAR*, int32>& F : Flags) AddCheck(F.Key, EDoomEditField::ThingFlag, F.Value, (T.Flags & F.Value) != 0);
			Props->AddChildToVerticalBox(MakeButton(TEXT("Delete thing"), ActDelete));
		}
		else Heading(TEXT("Click a thing to edit it"));
		Heading(TEXT("New things"));
		AddNames(TEXT("Place"), EDoomEditField::PlaceType, TypeName(E->PlaceType), Types);
		Props->AddChildToVerticalBox(MakeButton(TEXT("T Add at the cursor (or double-click)"), ActAddThing));
		break;
	}
	case EDoomEditMode::Vertices:
		if (M->Vertices.IsValidIndex(Sel))
		{
			Heading(FString::Printf(TEXT("Vertex %d"), Sel));
			AddNumber(TEXT("X"), EDoomEditField::VertexX, M->Vertices[Sel].X, -32768, 32767);
			AddNumber(TEXT("Y"), EDoomEditField::VertexY, M->Vertices[Sel].Y, -32768, 32767);
		}
		else Heading(TEXT("Click a vertex; drag to move it"));
		break;
	case EDoomEditMode::Draw:
		Heading(TEXT("Draw a sector"));
		Props->AddChildToVerticalBox(Label(TEXT("Click the corners; click the first one again, double-click\nor press Enter to close it. In the void, inside a sector, or\nalong existing walls (they become openings). Backspace\nremoves a corner, Esc starts again."), 9));
		Heading(TEXT("New sectors"));
		AddNumber(TEXT("Floor"), EDoomEditField::SectorFloor, E->DrawTemplate.Floor, -32768, 32767);
		AddNumber(TEXT("Ceiling"), EDoomEditField::SectorCeiling, E->DrawTemplate.Ceiling, -32768, 32767);
		AddNumber(TEXT("Light"), EDoomEditField::SectorLight, E->DrawTemplate.Light, 0, 255);
		AddNames(TEXT("Floor flat"), EDoomEditField::SectorFloorPic, E->DrawTemplate.FloorPic, Flats);
		AddNames(TEXT("Ceiling flat"), EDoomEditField::SectorCeilingPic, E->DrawTemplate.CeilingPic, Flats);
		AddNames(TEXT("Walls"), EDoomEditField::FrontMiddle, E->DrawWall, Textures);
		break;
	}
	Heading(TEXT("Grid"));
	AddNumber(TEXT("Snap to"), EDoomEditField::Grid, E->Grid, 1, 128);
}

void UDoomEditorWidget::OnAction(int32 Action)
{
	UDoomMapEditor* E = Editor.Get();
	if (!E)
	{
		return;
	}
	switch (Action)
	{
	case ModeThings: E->SetMode(EDoomEditMode::Things); break;
	case ModeSectors: E->SetMode(EDoomEditMode::Sectors); break;
	case ModeLines: E->SetMode(EDoomEditMode::Lines); break;
	case ModeVertices: E->SetMode(EDoomEditMode::Vertices); break;
	case ModeDraw: E->SetMode(EDoomEditMode::Draw); break;
	case ActUndo: E->Undo(); break;
	case ActRedo: E->Redo(); break;
	case ActView: E->b3D = !E->b3D; break;
	case ActCheck: E->CheckMap(); break;
	case ActSave: E->SaveWad(); break;
	case ActRandom: E->NewRandomMap(); break;
	case ActEmpty: E->NewEmptyMap(); break;
	case ActPlay: E->Close(); return;
	case ActAddThing: E->AddThing(E->Cursor, E->PlaceType); break;
	case ActDelete: E->DeleteSelected(); break;
	default: break;
	}
	if (MapView) MapView->FocusView();
}

void UDoomEditorWidget::OnNumber(EDoomEditField Field, float Value)
{
	if (UDoomMapEditor* E = Editor.Get()) E->SetNumber(Field, Value);
}

void UDoomEditorWidget::OnName(EDoomEditField Field, const FString& Value)
{
	UDoomMapEditor* E = Editor.Get();
	if (!E)
	{
		return;
	}
	if (Field == EDoomEditField::ThingType || Field == EDoomEditField::PlaceType)
	{
		E->SetNumber(Field, (float)FCString::Atoi(*Value)); // "3001 Imp"
		return;
	}
	E->SetName(Field, Value == TEXT("-") ? NAME_None : FName(*Value));
}

void UDoomEditorWidget::OnFlag(EDoomEditField Field, int32 Bit, bool bOn)
{
	if (UDoomMapEditor* E = Editor.Get()) E->SetFlag(Field, Bit, bOn);
}

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
	UE_LOG(LogDoom, Log, TEXT("Map editor: editing %s (%d sectors, %d lines, %d things)"), *EditMap->Name, EditMap->Sectors.Num(), EditMap->Lines.Num(), EditMap->Things.Num());
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
	UE_LOG(LogDoom, Log, TEXT("Map editor: playing %s"), EditMap.IsValid() ? *EditMap->Name : TEXT("-"));
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
	if (!P)
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
