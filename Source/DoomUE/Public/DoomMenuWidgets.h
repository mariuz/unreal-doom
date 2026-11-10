// The menu through Epic's CommonUI: a layout with an activatable widget stack, and the menu screen
// pushed on it while the game is in the menu. FDoomMenu stays the menu's logic (pages, rows, sounds);
// the screen draws it in Doom's look (DoomScreenPainter.h) and gives it CommonUI's input handling:
//  * each selectable row is an invisible, unfocusable CommonUI button over the row, for the mouse
//    (hover and click);
//  * the screen keeps the focus and reads the keys itself (MenuKey): each key does what the menu
//    context maps it to, through FDoomMenu's Up / Down / Left / Right / Select / Back, so rows are
//    skipped and sounds play as in Doom; Esc closes the menu, and the stick navigates the same way;
//  * CommonUI's click and back actions are IA_MenuSelect and IA_MenuBack (UDoomUIInputData), for
//    its button prompts;
//  * the screen asks for menu input (a cursor, no game input) and the layout under it for game input.
// Typing a save name, the Controls page's key capture and the yes/no prompts take raw keys.
#pragma once

#include "CoreMinimal.h"
#include "CommonActivatableWidget.h"
#include "CommonButtonBase.h"
#include "CommonInputBaseTypes.h"
#include "DoomMenuWidgets.generated.h"

class UCanvasPanel;
class UCommonActivatableWidgetStack;
class UDoomMenuScreen;
class UDoomWorld;

/** CommonUI's click and back actions: the menu's own select and back input actions. */
UCLASS()
class DOOMUE_API UDoomUIInputData : public UCommonUIInputData
{
	GENERATED_BODY()

public:
	UDoomUIInputData();
};

/** The row buttons' style: nothing drawn in any state (the menu's own graphics show the row). */
UCLASS()
class DOOMUE_API UDoomMenuRowButtonStyle : public UCommonButtonStyle
{
	GENERATED_BODY()

public:
	UDoomMenuRowButtonStyle();
};

/** One row of the menu: an invisible CommonUI button the screen places over the row's graphic. */
UCLASS()
class DOOMUE_API UDoomMenuRowButton : public UCommonButtonBase
{
	GENERATED_BODY()

public:
	int32 Row = INDEX_NONE;
	TWeakObjectPtr<UDoomMenuScreen> Screen;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeOnHovered() override;
	virtual void NativeOnClicked() override;
};

/** The menu while it is open: Doom's graphics, and a row button over each selectable row. */
UCLASS()
class DOOMUE_API UDoomMenuScreen : public UCommonActivatableWidget
{
	GENERATED_BODY()

public:
	UDoomMenuScreen(const FObjectInitializer& ObjectInitializer);

	/** The row buttons now (the play-test checks them). */
	const TArray<TObjectPtr<UDoomMenuRowButton>>& GetRowButtons() const { return Buttons; }
	/** What the input would do, without the input (the play-test drives the screen with these). */
	void RowHovered(int32 Row);
	void RowClicked(int32 Row);
	UWidget* Navigate(int32 Row, EUINavigation Direction);
	bool HandleBack();
	void HandleClose();
	/** The mouse has moved since the menu opened: only then does hovering a row choose it. */
	bool HasMouseMoved() const { return bMouseMoved; }

	// UCommonActivatableWidget
	virtual TOptional<FUIInputConfig> GetDesiredInputConfig() const override;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeOnActivated() override;
	virtual void NativeOnDeactivated() override;
	virtual UWidget* NativeGetDesiredFocusTarget() const override;
	virtual FNavigationReply NativeOnNavigation(const FGeometry& MyGeometry, const FNavigationEvent& InNavigationEvent, const FNavigationReply& InDefaultReply) override;
	virtual bool NativeOnHandleBackAction() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	UDoomWorld* DoomWorld() const;
	/** Typing, capturing a key, a prompt or a Read This! page: keys go to the screen, no rows. */
	bool TakesRawKeys() const;
	/** A key the menu context maps to one of the menu's actions; true if it was one. */
	bool MenuKey(const FKey& Key, bool bRepeat);
	/** A raw key while TakesRawKeys(); true if used. */
	bool RawKey(const FKey& Key);
	/** Rebuilds the row buttons when the page, the visible rows or the screen's size change. */
	void Refresh(const FGeometry& Geometry);
	void FocusSelected();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> Canvas;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UDoomMenuRowButton>> Buttons;
	/** What the buttons were built for (page, rows, scroll, size...). */
	uint32 BuiltFor = 0;
	/** The row the focus was last moved to. */
	int32 FocusedItem = INDEX_NONE;
	bool bMouseMoved = false;
	/** When the row buttons were last rebuilt: focus settling on a new button then is not a choice. */
	uint64 RebuiltFrame = 0;
	/** Slate brushes for the WAD's pictures, made as they are first drawn. */
	mutable TMap<TWeakObjectPtr<class UTexture2D>, FSlateBrush> Brushes;
};

/** The player's UI root: game input, with the menu stack over it (Lyra's layout, reduced). */
UCLASS()
class DOOMUE_API UDoomUILayout : public UCommonActivatableWidget
{
	GENERATED_BODY()

public:
	UDoomUILayout(const FObjectInitializer& ObjectInitializer);

	UCommonActivatableWidgetStack* GetMenuStack() const { return MenuStack; }
	virtual TOptional<FUIInputConfig> GetDesiredInputConfig() const override;

protected:
	virtual void NativeOnInitialized() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UCommonActivatableWidgetStack> MenuStack;
};
