// The menu through CommonUI (DoomMenuWidgets.h).
#include "DoomMenuWidgets.h"
#include "DoomScreenPainter.h"
#include "DoomPlayerController.h"
#include "DoomWorld.h"
#include "DoomMenu.h"
#include "DoomInput.h"
#include "DoomUE.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Widgets/CommonActivatableWidgetContainer.h"
#include "Input/CommonUIInputTypes.h"
#include "InputAction.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "UObject/ConstructorHelpers.h"

// --- CommonUI's click and back actions -----------------------------------------------------------

UDoomUIInputData::UDoomUIInputData()
{
	// the menu's own actions (DoomInput::Build), as the content builder saved them
	static ConstructorHelpers::FObjectFinder<UInputAction> Select(TEXT("/Game/Doom/Input/IA_MenuSelect"));
	static ConstructorHelpers::FObjectFinder<UInputAction> Back(TEXT("/Game/Doom/Input/IA_MenuBack"));
	EnhancedInputClickAction = Select.Object;
	EnhancedInputBackAction = Back.Object;
}

// --- a row ---------------------------------------------------------------------------------------

namespace DoomMenuWidgetsDetail
{
	/** Doom's frame on a Slate widget: the screen's paint pass. */
	class FSlatePainter : public FDoomScreenPainter
	{
	public:
		FSlatePainter(const FGeometry& InGeometry, FSlateWindowElementList& InOut, int32 InLayer, TMap<TWeakObjectPtr<UTexture2D>, FSlateBrush>& InBrushes)
			: Geometry(InGeometry), Out(InOut), Layer(InLayer), Brushes(InBrushes) {}

	protected:
		virtual void Picture(UTexture2D* Texture, float X, float Y, float W, float H, FLinearColor Tint) override
		{
			if (!Texture) return;
			FSlateBrush* Brush = Brushes.Find(Texture);
			if (!Brush)
			{
				Brush = &Brushes.Add(Texture);
				Brush->SetResourceObject(Texture);
				Brush->ImageSize = FVector2D(Texture->GetSizeX(), Texture->GetSizeY());
				Brush->DrawAs = ESlateBrushDrawType::Image;
			}
			FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(FVector2f(W, H), FSlateLayoutTransform(FVector2f(X, Y))), Brush, ESlateDrawEffect::None, Tint);
		}
		virtual void Fill(float X, float Y, float W, float H, FLinearColor C) override
		{
			FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(FVector2f(W, H), FSlateLayoutTransform(FVector2f(X, Y))),
				FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")), ESlateDrawEffect::None, C);
		}

	private:
		const FGeometry& Geometry;
		FSlateWindowElementList& Out;
		int32 Layer;
		TMap<TWeakObjectPtr<UTexture2D>, FSlateBrush>& Brushes;
	};

	/** The surface's 320x200 frame (FDoomScreenPainter::Fit), for placing buttons and hit tests. */
	struct FFrame
	{
		float OX = 0, OY = 0, SX = 1, SY = 1;
		explicit FFrame(const FVector2f& Size)
		{
			SX = FMath::Min(Size.X / 320.f, Size.Y / 240.f);
			SY = SX * 1.2f;
			OX = (Size.X - 320.f * SX) * 0.5f;
			OY = (Size.Y - 200.f * SY) * 0.5f;
		}
		FVector2f ToFrame(const FVector2f& Local) const { return FVector2f((Local.X - OX) / SX, (Local.Y - OY) / SY); }
	};
}

UDoomMenuRowButtonStyle::UDoomMenuRowButtonStyle()
{
	for (FSlateBrush* B : { &SingleMaterialBrush, &NormalBase, &NormalHovered, &NormalPressed, &SelectedBase, &SelectedHovered, &SelectedPressed, &Disabled })
	{
		B->DrawAs = ESlateBrushDrawType::NoDrawType;
	}
}

UWidget* UDoomMenuRowButton::Navigate(EUINavigation Direction)
{
	return Screen.IsValid() ? Screen->Navigate(Row, Direction) : nullptr;
}

void UDoomMenuRowButton::NativeOnInitialized()
{
	// an empty, invisible content: UCommonButtonBase wraps the root in its internal button
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		WidgetTree->RootWidget = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	}
	Super::NativeOnInitialized();
	SetStyle(UDoomMenuRowButtonStyle::StaticClass());
}

void UDoomMenuRowButton::NativeOnHovered()
{
	Super::NativeOnHovered();
	if (Screen.IsValid() && Screen->HasMouseMoved()) Screen->RowHovered(Row);
}

void UDoomMenuRowButton::NativeOnClicked()
{
	Super::NativeOnClicked();
	if (Screen.IsValid()) Screen->RowClicked(Row);
}

void UDoomMenuRowButton::HandleFocusReceived()
{
	Super::HandleFocusReceived();
	if (Screen.IsValid()) Screen->RowFocused(Row);
}

// --- the menu screen -----------------------------------------------------------------------------

UDoomMenuScreen::UDoomMenuScreen(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsBackHandler = true;
	SetIsFocusable(true);
}

UDoomWorld* UDoomMenuScreen::DoomWorld() const
{
	return GetWorld() ? GetWorld()->GetSubsystem<UDoomWorld>() : nullptr;
}

TOptional<FUIInputConfig> UDoomMenuScreen::GetDesiredInputConfig() const
{
	// the menu takes the input: a cursor for the mouse, nothing reaches the game
	return FUIInputConfig(ECommonInputMode::Menu, EMouseCaptureMode::NoCapture, false);
}

void UDoomMenuScreen::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
	WidgetTree->RootWidget = Canvas;
	// the menu context's other actions, for the keys Slate's navigation doesn't know (W/A/S/D, Esc);
	// the arrow keys, d-pad and stick navigate between the row buttons first
	if (const ADoomPlayerController* PC = Cast<ADoomPlayerController>(GetOwningPlayer()))
	{
		auto Bind = [this](const UInputAction* Action, TFunction<void()> Do)
		{
			if (Action) RegisterUIActionBinding(FBindUIActionArgs(Action, false, FSimpleDelegate::CreateLambda(MoveTemp(Do))));
		};
		Bind(PC->GetMenuUpAction(), [this]() { Navigate(INDEX_NONE, EUINavigation::Up); });
		Bind(PC->GetMenuDownAction(), [this]() { Navigate(INDEX_NONE, EUINavigation::Down); });
		Bind(PC->GetMenuLeftAction(), [this]() { Navigate(INDEX_NONE, EUINavigation::Left); });
		Bind(PC->GetMenuRightAction(), [this]() { Navigate(INDEX_NONE, EUINavigation::Right); });
		Bind(PC->GetMenuCloseAction(), [this]() { HandleClose(); });
		// Y and Space (Enter and the gamepad's A are the focused row button's own accept keys)
		Bind(PC->GetMenuSelectAction(), [this]()
		{
			UDoomWorld* W = DoomWorld();
			if (W && !W->IsInputLocked() && !TakesRawKeys()) RowClicked(W->Menu.GetItem());
		});
	}
}

void UDoomMenuScreen::NativeOnActivated()
{
	Super::NativeOnActivated();
	if (APlayerController* PC = GetOwningPlayer()) PC->SetShowMouseCursor(true);
	BuiltFor = 0;
	bMouseMoved = false;
}

void UDoomMenuScreen::NativeOnDeactivated()
{
	if (APlayerController* PC = GetOwningPlayer()) PC->SetShowMouseCursor(false);
	Super::NativeOnDeactivated();
}

UWidget* UDoomMenuScreen::NativeGetDesiredFocusTarget() const
{
	const UDoomWorld* W = DoomWorld();
	for (UDoomMenuRowButton* B : Buttons)
	{
		if (W && B->Row == W->Menu.GetItem()) return B;
	}
	return const_cast<UDoomMenuScreen*>(this);
}

bool UDoomMenuScreen::TakesRawKeys() const
{
	const UDoomWorld* W = DoomWorld();
	if (!W) return false;
	const FDoomMenu& M = W->Menu;
	return M.IsEditing() || M.IsCapturing() || !M.Prompt().IsEmpty() || M.GetPage() == EMenuPage::ReadThis;
}

bool UDoomMenuScreen::RawKey(const FKey& Key)
{
	UDoomWorld* W = DoomWorld();
	if (!W || W->IsInputLocked()) return false;
	FDoomMenu& M = W->Menu;
	if (M.IsEditing())
	{
		// the gamepad's B cancels like Escape (it has no other use while typing)
		M.EditKey(*W, Key == EKeys::Gamepad_FaceButton_Right ? EKeys::Escape : Key);
		return true;
	}
	if (M.IsCapturing())
	{
		if (Key.IsAxis1D() || Key.IsAxis2D()) return false;
		M.CaptureKey(*W, Key);
		return true;
	}
	if (!M.Prompt().IsEmpty())
	{
		if (Key.IsAxis1D() || Key.IsAxis2D()) return false;
		// only Y, Enter or the gamepad's A confirm; any other key cancels, as in Doom
		if (Key == EKeys::Y || Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom) M.Select(*W);
		else M.Back(*W);
		return true;
	}
	if (M.GetPage() == EMenuPage::ReadThis && (Key == EKeys::Enter || Key == EKeys::SpaceBar || Key == EKeys::Gamepad_FaceButton_Bottom || Key == EKeys::LeftMouseButton))
	{
		M.Select(*W); // the next page
		return true;
	}
	return false;
}

void UDoomMenuScreen::RowHovered(int32 Row)
{
	UDoomWorld* W = DoomWorld();
	if (!W || W->IsInputLocked() || TakesRawKeys() || Row == W->Menu.GetItem()) return;
	W->Menu.SetItem(Row);
	W->PlaySound(TEXT("STNMOV"), nullptr); // the menu's move sound (FDoomMenu::Up / Down)
	FocusSelected();
}

void UDoomMenuScreen::RowFocused(int32 Row)
{
	UDoomWorld* W = DoomWorld();
	// focus landing on a new button while the page's buttons are rebuilt (going back a page, scrolling)
	// is Slate settling, not the player: the selection stays (DoomUE.Input.MenuKeys saw it jump)
	if (GFrameCounter < RebuiltFrame + 3) return;
	if (W && !TakesRawKeys()) W->Menu.SetItem(Row);
}

void UDoomMenuScreen::RowClicked(int32 Row)
{
	UDoomWorld* W = DoomWorld();
	if (!W || W->IsInputLocked() || TakesRawKeys()) return;
	FDoomMenu& M = W->Menu;
	M.SetItem(Row);
	const TArray<FMenuRow> Rows = M.Rows(*W);
	if (Rows.IsValidIndex(Row) && Rows[Row].Kind == FMenuRow::EKind::Slider)
	{
		// a click on the thermometer moves the slider one notch towards it
		const FVector2f Local = GetCachedGeometry().AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
		const FVector2f At = DoomMenuWidgetsDetail::FFrame(GetCachedGeometry().GetLocalSize()).ToFrame(Local);
		const int32 Notch = FMath::FloorToInt((At.X - M.Origin().X - 8.f) / 8.f);
		if (Notch < Rows[Row].Value) M.Left(*W);
		else if (Notch > Rows[Row].Value) M.Right(*W);
		return;
	}
	M.Select(*W);
}

UWidget* UDoomMenuScreen::Navigate(int32 Row, EUINavigation Direction)
{
	UDoomWorld* W = DoomWorld();
	if (!W || W->IsInputLocked() || TakesRawKeys()) return nullptr;
	FDoomMenu& M = W->Menu;
	switch (Direction)
	{
	case EUINavigation::Up: M.Up(*W); break;
	case EUINavigation::Down: M.Down(*W); break;
	case EUINavigation::Left: M.Left(*W); break;
	case EUINavigation::Right: M.Right(*W); break;
	default: return nullptr;
	}
	// the row FDoomMenu moved to (a scrolled page builds its buttons again next tick)
	for (UDoomMenuRowButton* B : Buttons)
	{
		if (B->Row == M.GetItem()) return B;
	}
	return nullptr;
}

bool UDoomMenuScreen::HandleBack()
{
	UDoomWorld* W = DoomWorld();
	if (W && !W->IsInputLocked()) W->Menu.Back(*W);
	return true;
}

void UDoomMenuScreen::HandleClose()
{
	UDoomWorld* W = DoomWorld();
	if (W && !W->IsInputLocked() && W->GetState() == EDoomGameState::Menu) W->Menu.Close(*W); // M_ClearMenus
}

bool UDoomMenuScreen::NativeOnHandleBackAction()
{
	return HandleBack();
}

FReply UDoomMenuScreen::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (TakesRawKeys() && RawKey(InKeyEvent.GetKey()))
	{
		return FReply::Handled();
	}
	// Esc closes the whole menu, from any page (M_ClearMenus); CommonUI would take it as its back key
	// and go back one page (Backspace and the gamepad's B do that)
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		HandleClose();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UDoomMenuScreen::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (TakesRawKeys())
	{
		RawKey(InMouseEvent.GetEffectingButton());
	}
	// a click between the rows does nothing (it must not reach CommonUI's click action)
	return FReply::Handled();
}

FReply UDoomMenuScreen::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// the cursor appears where it was when the menu opens: a row under it isn't chosen until the mouse
	// moves, or the selection would jump there for a player using the keys (Doom keeps the last row)
	if (!bMouseMoved && !InMouseEvent.GetCursorDelta().IsNearlyZero())
	{
		bMouseMoved = true;
		for (UDoomMenuRowButton* B : Buttons)
		{
			if (B->IsHovered()) RowHovered(B->Row);
		}
	}
	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

FReply UDoomMenuScreen::NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	UDoomWorld* W = DoomWorld();
	if (W && !W->IsInputLocked() && !TakesRawKeys())
	{
		Navigate(INDEX_NONE, InMouseEvent.GetWheelDelta() > 0 ? EUINavigation::Up : EUINavigation::Down);
		FocusSelected();
	}
	return FReply::Handled();
}

void UDoomMenuScreen::FocusSelected()
{
	if (UWidget* Target = NativeGetDesiredFocusTarget())
	{
		if (!Target->HasAnyUserFocus()) Target->SetFocus();
	}
}

void UDoomMenuScreen::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (IsActivated())
	{
		Refresh(MyGeometry);
	}
}

void UDoomMenuScreen::Refresh(const FGeometry& Geometry)
{
	UDoomWorld* W = DoomWorld();
	if (!W || !W->GetGraphics()) return;
	FDoomMenu& M = W->Menu;
	const TArray<FMenuRow> Rows = M.Rows(*W);
	const bool bRaw = TakesRawKeys();
	const FVector2f Size = Geometry.GetLocalSize();
	// what the buttons depend on: the page, its rows (and which are enabled), the scroll, the size
	uint32 For = HashCombine(GetTypeHash((int32)M.GetPage()), GetTypeHash(Rows.Num()));
	For = HashCombine(For, GetTypeHash(M.FirstVisibleRow(Rows.Num())));
	For = HashCombine(For, GetTypeHash(bRaw));
	For = HashCombine(For, GetTypeHash(FIntPoint(FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y))));
	for (const FMenuRow& R : Rows) For = HashCombine(For, GetTypeHash(R.bEnabled));
	const bool bRebuilt = For != BuiltFor;
	if (bRebuilt)
	{
		BuiltFor = For;
		RebuiltFrame = GFrameCounter;
		Canvas->ClearChildren();
		Buttons.Reset();
		if (!bRaw)
		{
			const DoomMenuWidgetsDetail::FFrame Frame(Size);
			for (int32 i = 0; i < Rows.Num(); ++i)
			{
				const FBox2f Box = FDoomScreenPainter::MenuRowBox(*W, i);
				if (!Box.bIsValid || !Rows[i].bEnabled) continue; // gaps, rows out of view, disabled rows (Doom skips them)
				UDoomMenuRowButton* B = CreateWidget<UDoomMenuRowButton>(this, UDoomMenuRowButton::StaticClass());
				B->Row = i;
				B->Screen = this;
				FCustomWidgetNavigationDelegate Nav;
				Nav.BindUFunction(B, GET_FUNCTION_NAME_CHECKED(UDoomMenuRowButton, Navigate));
				for (EUINavigation Dir : { EUINavigation::Up, EUINavigation::Down, EUINavigation::Left, EUINavigation::Right })
				{
					B->SetNavigationRuleCustom(Dir, Nav);
				}
				UCanvasPanelSlot* RowSlot = Canvas->AddChildToCanvas(B);
				RowSlot->SetAutoSize(false);
				RowSlot->SetPosition(FVector2D(Frame.OX + Box.Min.X * Frame.SX, Frame.OY + Box.Min.Y * Frame.SY));
				RowSlot->SetSize(FVector2D(Box.GetSize().X * Frame.SX, Box.GetSize().Y * Frame.SY));
				Buttons.Add(B);
			}
		}
	}
	// the focus follows FDoomMenu's selection when it moves on its own (new pages, scrolling, tests)
	if (bRebuilt || M.GetItem() != FocusedItem)
	{
		FocusedItem = M.GetItem();
		FocusSelected();
	}
}

int32 UDoomMenuScreen::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (UDoomWorld* W = DoomWorld(); W && W->GetGraphics() && W->GetState() == EDoomGameState::Menu)
	{
		// stale brushes (pictures freed when the WADs change) go first
		for (auto It = Brushes.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid()) It.RemoveCurrent();
		}
		DoomMenuWidgetsDetail::FSlatePainter Painter(AllottedGeometry, OutDrawElements, LayerId, Brushes);
		Painter.World = W;
		Painter.Fit(AllottedGeometry.GetLocalSize().X, AllottedGeometry.GetLocalSize().Y);
		Painter.DrawMenu(GetWorld()->GetRealTimeSeconds());
	}
	// the row buttons are invisible: nothing of them to draw over the menu
	return Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId + 1, InWidgetStyle, bParentEnabled);
}

// --- the layout ----------------------------------------------------------------------------------

UDoomUILayout::UDoomUILayout(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
}

TOptional<FUIInputConfig> UDoomUILayout::GetDesiredInputConfig() const
{
	// under the menu: the game has the input, the mouse captured for looking around
	return FUIInputConfig(ECommonInputMode::Game, EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown, true);
}

void UDoomUILayout::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UOverlay* Root = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
		WidgetTree->RootWidget = Root;
		MenuStack = WidgetTree->ConstructWidget<UCommonActivatableWidgetStack>(UCommonActivatableWidgetStack::StaticClass(), TEXT("MenuStack"));
		UOverlaySlot* StackSlot = Root->AddChildToOverlay(MenuStack);
		StackSlot->SetHorizontalAlignment(HAlign_Fill);
		StackSlot->SetVerticalAlignment(VAlign_Fill);
	}
}
