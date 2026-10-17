// Enhanced Input set up entirely in code (no .uasset input actions): a gameplay
// mapping context (WASD/arrows, mouse, Ctrl/LMB fire, E/Space use, 1-7, wheel,
// Tab automap, Shift run) and a menu context swapped in on the title screen.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "InputActionValue.h"
#include "DoomPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;

UCLASS()
class DOOMUE_API ADoomPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void Tick(float DeltaSeconds) override;
	/** While a save name is typed, keys go to the menu instead of Enhanced Input. */
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;

	UPROPERTY() TObjectPtr<UInputAction> MoveAction;
	UPROPERTY() TObjectPtr<UInputAction> LookAction;
	UPROPERTY() TObjectPtr<UInputAction> FireAction;
	UPROPERTY() TObjectPtr<UInputAction> UseAction;
	UPROPERTY() TObjectPtr<UInputAction> RunAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapZoomAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapPanAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapFollowAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapBigAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapMarkAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapClearAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapGridAction;
	UPROPERTY() TObjectPtr<UInputAction> NextWeaponAction;
	UPROPERTY() TObjectPtr<UInputAction> PrevWeaponAction;
	UPROPERTY() TArray<TObjectPtr<UInputAction>> SlotActions;
	UPROPERTY() TObjectPtr<UInputAction> MenuUpAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuDownAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuLeftAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuRightAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuSelectAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuBackAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuCloseAction;
	UPROPERTY() TObjectPtr<UInputAction> QuickSaveAction;
	UPROPERTY() TObjectPtr<UInputAction> QuickLoadAction;
	UPROPERTY() TArray<TObjectPtr<UInputAction>> PageActions;

	UPROPERTY() TObjectPtr<UInputMappingContext> GameplayContext;
	UPROPERTY() TObjectPtr<UInputMappingContext> MenuContext;
	/** The arrow keys pan the automap while follow mode is off (above the gameplay context). */
	UPROPERTY() TObjectPtr<UInputMappingContext> AutomapPanContext;

private:
	void CreateInput();
	void ApplyContexts();
	void MenuUp(const FInputActionValue& V);
	void MenuDown(const FInputActionValue& V);
	void MenuLeft(const FInputActionValue& V);
	void MenuRight(const FInputActionValue& V);
	void MenuSelect(const FInputActionValue& V);
	void MenuBack(const FInputActionValue& V);
	void OpenMenuPage(int32 Page);

	bool bMenuContextActive = false;
	bool bContextsInitialized = false;
	bool bPanContextActive = false;
private:
	/** A client pressed Enter on the intermission screen. */
	UFUNCTION(Server, Reliable)
	void ServerFinishIntermission();
	/** -nettest on a network client. */
	void RunClientNetTest();
};
