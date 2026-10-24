// The map editor's UMG panels, built in code: the toolbar, the selection's properties and the status
// line, and the controls that report to them.
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

