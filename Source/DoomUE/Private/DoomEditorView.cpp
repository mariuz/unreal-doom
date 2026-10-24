// The map editor's top-down view: a Slate leaf widget (drawing the map, mouse and keys) in a UMG
// wrapper (UDoomMapView). The editing itself is UDoomMapEditor, in DoomEditor.cpp.
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

