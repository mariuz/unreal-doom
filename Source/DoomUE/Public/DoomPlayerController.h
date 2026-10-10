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

	UPROPERTY() TObjectPtr<UInputAction> MoveAction;
	UPROPERTY() TObjectPtr<UInputAction> LookAction;
	UPROPERTY() TObjectPtr<UInputAction> FireAction;
	UPROPERTY() TObjectPtr<UInputAction> UseAction;
	UPROPERTY() TObjectPtr<UInputAction> RunAction;
	UPROPERTY() TObjectPtr<UInputAction> AutomapAction;
	UPROPERTY() TObjectPtr<UInputAction> NextWeaponAction;
	UPROPERTY() TObjectPtr<UInputAction> PrevWeaponAction;
	UPROPERTY() TArray<TObjectPtr<UInputAction>> SlotActions;
	UPROPERTY() TObjectPtr<UInputAction> MenuUpAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuDownAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuLeftAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuRightAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuSelectAction;
	UPROPERTY() TObjectPtr<UInputAction> MenuBackAction;

	UPROPERTY() TObjectPtr<UInputMappingContext> GameplayContext;
	UPROPERTY() TObjectPtr<UInputMappingContext> MenuContext;

private:
	void CreateInput();
	void ApplyContexts();
	void MenuUp(const FInputActionValue& V);
	void MenuDown(const FInputActionValue& V);
	void MenuLeft(const FInputActionValue& V);
	void MenuRight(const FInputActionValue& V);
	void MenuSelect(const FInputActionValue& V);
	void MenuBack(const FInputActionValue& V);

	bool bMenuContextActive = false;
	bool bContextsInitialized = false;
};
