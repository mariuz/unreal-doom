#include "DoomPlayerController.h"
#include "DoomWorld.h"
#include "DoomHUD.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Kismet/KismetSystemLibrary.h"

namespace
{
	UInputAction* NewAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* A = NewObject<UInputAction>(Outer, Name);
		A->ValueType = Type;
		return A;
	}
}

void ADoomPlayerController::CreateInput()
{
	if (MoveAction)
	{
		return;
	}
	MoveAction = NewAction(this, TEXT("IA_Move"), EInputActionValueType::Axis2D);
	LookAction = NewAction(this, TEXT("IA_Look"), EInputActionValueType::Axis2D);
	FireAction = NewAction(this, TEXT("IA_Fire"), EInputActionValueType::Boolean);
	UseAction = NewAction(this, TEXT("IA_Use"), EInputActionValueType::Boolean);
	RunAction = NewAction(this, TEXT("IA_Run"), EInputActionValueType::Boolean);
	AutomapAction = NewAction(this, TEXT("IA_Automap"), EInputActionValueType::Boolean);
	NextWeaponAction = NewAction(this, TEXT("IA_NextWeapon"), EInputActionValueType::Boolean);
	PrevWeaponAction = NewAction(this, TEXT("IA_PrevWeapon"), EInputActionValueType::Boolean);
	for (int32 i = 0; i < 7; ++i)
	{
		SlotActions.Add(NewAction(this, *FString::Printf(TEXT("IA_Slot%d"), i + 1), EInputActionValueType::Boolean));
	}
	MenuUpAction = NewAction(this, TEXT("IA_MenuUp"), EInputActionValueType::Boolean);
	MenuDownAction = NewAction(this, TEXT("IA_MenuDown"), EInputActionValueType::Boolean);
	MenuLeftAction = NewAction(this, TEXT("IA_MenuLeft"), EInputActionValueType::Boolean);
	MenuRightAction = NewAction(this, TEXT("IA_MenuRight"), EInputActionValueType::Boolean);
	MenuSelectAction = NewAction(this, TEXT("IA_MenuSelect"), EInputActionValueType::Boolean);
	MenuBackAction = NewAction(this, TEXT("IA_MenuBack"), EInputActionValueType::Boolean);

	GameplayContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Doom"));
	auto Axis = [&](FKey Key, bool bSwizzle, bool bNegate)
	{
		FEnhancedActionKeyMapping& M = GameplayContext->MapKey(MoveAction, Key);
		if (bSwizzle)
		{
			M.Modifiers.Add(NewObject<UInputModifierSwizzleAxis>(GameplayContext));
		}
		if (bNegate)
		{
			M.Modifiers.Add(NewObject<UInputModifierNegate>(GameplayContext));
		}
	};
	// forward/back on Y (swizzled), strafe on X
	Axis(EKeys::W, true, false);
	Axis(EKeys::Up, true, false);
	Axis(EKeys::S, true, true);
	Axis(EKeys::Down, true, true);
	Axis(EKeys::D, false, false);
	Axis(EKeys::A, false, true);
	{
		FEnhancedActionKeyMapping& Pad = GameplayContext->MapKey(MoveAction, EKeys::Gamepad_Left2D);
		(void)Pad;
	}
	GameplayContext->MapKey(LookAction, EKeys::Mouse2D);
	{
		FEnhancedActionKeyMapping& Pad = GameplayContext->MapKey(LookAction, EKeys::Gamepad_Right2D);
		UInputModifierNegate* Neg = NewObject<UInputModifierNegate>(GameplayContext);
		Neg->bX = false; Neg->bY = true; Neg->bZ = false;
		Pad.Modifiers.Add(Neg);
	}
	{
		// arrow keys turn, like Doom
		FEnhancedActionKeyMapping& L = GameplayContext->MapKey(LookAction, EKeys::Left);
		UInputModifierNegate* Neg = NewObject<UInputModifierNegate>(GameplayContext);
		L.Modifiers.Add(Neg);
		UInputModifierScalar* Sc = NewObject<UInputModifierScalar>(GameplayContext);
		Sc->Scalar = FVector(2.5, 2.5, 1);
		L.Modifiers.Add(Sc);
		FEnhancedActionKeyMapping& R = GameplayContext->MapKey(LookAction, EKeys::Right);
		UInputModifierScalar* Sc2 = NewObject<UInputModifierScalar>(GameplayContext);
		Sc2->Scalar = FVector(2.5, 2.5, 1);
		R.Modifiers.Add(Sc2);
	}
	GameplayContext->MapKey(FireAction, EKeys::LeftMouseButton);
	GameplayContext->MapKey(FireAction, EKeys::LeftControl);
	GameplayContext->MapKey(FireAction, EKeys::Gamepad_RightTrigger);
	GameplayContext->MapKey(UseAction, EKeys::E);
	GameplayContext->MapKey(UseAction, EKeys::SpaceBar);
	GameplayContext->MapKey(UseAction, EKeys::RightMouseButton);
	GameplayContext->MapKey(UseAction, EKeys::Gamepad_FaceButton_Bottom);
	GameplayContext->MapKey(RunAction, EKeys::LeftShift);
	GameplayContext->MapKey(AutomapAction, EKeys::Tab);
	GameplayContext->MapKey(AutomapAction, EKeys::Gamepad_Special_Left);
	GameplayContext->MapKey(NextWeaponAction, EKeys::MouseScrollDown);
	GameplayContext->MapKey(NextWeaponAction, EKeys::Gamepad_RightShoulder);
	GameplayContext->MapKey(PrevWeaponAction, EKeys::MouseScrollUp);
	GameplayContext->MapKey(PrevWeaponAction, EKeys::Gamepad_LeftShoulder);
	const FKey Digits[7] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven };
	for (int32 i = 0; i < 7; ++i)
	{
		GameplayContext->MapKey(SlotActions[i], Digits[i]);
	}
	GameplayContext->MapKey(MenuBackAction, EKeys::Escape);
	GameplayContext->MapKey(MenuBackAction, EKeys::Gamepad_Special_Right);

	MenuContext = NewObject<UInputMappingContext>(this, TEXT("IMC_DoomMenu"));
	MenuContext->MapKey(MenuUpAction, EKeys::Up);
	MenuContext->MapKey(MenuUpAction, EKeys::W);
	MenuContext->MapKey(MenuUpAction, EKeys::Gamepad_DPad_Up);
	MenuContext->MapKey(MenuDownAction, EKeys::Down);
	MenuContext->MapKey(MenuDownAction, EKeys::S);
	MenuContext->MapKey(MenuDownAction, EKeys::Gamepad_DPad_Down);
	MenuContext->MapKey(MenuLeftAction, EKeys::Left);
	MenuContext->MapKey(MenuLeftAction, EKeys::A);
	MenuContext->MapKey(MenuLeftAction, EKeys::Gamepad_DPad_Left);
	MenuContext->MapKey(MenuRightAction, EKeys::Right);
	MenuContext->MapKey(MenuRightAction, EKeys::D);
	MenuContext->MapKey(MenuRightAction, EKeys::Gamepad_DPad_Right);
	MenuContext->MapKey(MenuSelectAction, EKeys::Enter);
	MenuContext->MapKey(MenuSelectAction, EKeys::SpaceBar);
	MenuContext->MapKey(MenuSelectAction, EKeys::LeftMouseButton);
	MenuContext->MapKey(MenuSelectAction, EKeys::Gamepad_FaceButton_Bottom);
	MenuContext->MapKey(MenuBackAction, EKeys::Escape);
	MenuContext->MapKey(MenuBackAction, EKeys::Gamepad_Special_Right);
}

void ADoomPlayerController::BeginPlay()
{
	Super::BeginPlay();
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
	ApplyContexts();
}

void ADoomPlayerController::SetupInputComponent()
{
	CreateInput();
	Super::SetupInputComponent();
	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EIC)
	{
		UE_LOG(LogDoom, Error, TEXT("Enhanced Input is not the default input component (Config/DefaultInput.ini)"));
		return;
	}
	EIC->BindAction(MenuUpAction, ETriggerEvent::Started, this, &ADoomPlayerController::MenuUp);
	EIC->BindAction(MenuDownAction, ETriggerEvent::Started, this, &ADoomPlayerController::MenuDown);
	EIC->BindAction(MenuLeftAction, ETriggerEvent::Started, this, &ADoomPlayerController::MenuLeft);
	EIC->BindAction(MenuRightAction, ETriggerEvent::Started, this, &ADoomPlayerController::MenuRight);
	EIC->BindAction(MenuSelectAction, ETriggerEvent::Started, this, &ADoomPlayerController::MenuSelect);
	EIC->BindAction(MenuBackAction, ETriggerEvent::Started, this, &ADoomPlayerController::MenuBack);
}

void ADoomPlayerController::ApplyContexts()
{
	UEnhancedInputLocalPlayerSubsystem* Sub = GetLocalPlayer() ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()) : nullptr;
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!Sub || !W || !GameplayContext)
	{
		return;
	}
	const bool bMenu = W->GetState() != EDoomGameState::Playing;
	if (bContextsInitialized && bMenu == bMenuContextActive)
	{
		return;
	}
	bContextsInitialized = true;
	bMenuContextActive = bMenu;
	Sub->ClearAllMappings();
	Sub->AddMappingContext(bMenu ? MenuContext : GameplayContext, 0);
}

void ADoomPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ApplyContexts();
}

void ADoomPlayerController::MenuUp(const FInputActionValue& V)
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (W && W->GetState() == EDoomGameState::Menu)
	{
		const int32 N = ADoomHUD::MenuEntries(W).Num();
		W->MenuItem = (W->MenuItem + N - 1) % N;
		W->PlaySound(TEXT("PSTOP"), nullptr);
	}
}

void ADoomPlayerController::MenuDown(const FInputActionValue& V)
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (W && W->GetState() == EDoomGameState::Menu)
	{
		const int32 N = ADoomHUD::MenuEntries(W).Num();
		W->MenuItem = (W->MenuItem + 1) % N;
		W->PlaySound(TEXT("PSTOP"), nullptr);
	}
}

void ADoomPlayerController::MenuLeft(const FInputActionValue& V)
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!W || W->GetState() != EDoomGameState::Menu || !W->GetWad())
	{
		return;
	}
	const int32 NumMaps = W->GetWad()->MapNames().Num();
	switch (W->MenuItem)
	{
	case 1: W->MenuMapIndex = (W->MenuMapIndex + NumMaps - 1) % FMath::Max(1, NumMaps); break;
	case 2: W->MenuSkill = FMath::Clamp(W->MenuSkill - 1, 1, 5); break;
	case 3: W->MenuModern = !W->MenuModern; break;
	default: return;
	}
	W->PlaySound(TEXT("STNMOV"), nullptr);
}

void ADoomPlayerController::MenuRight(const FInputActionValue& V)
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!W || W->GetState() != EDoomGameState::Menu || !W->GetWad())
	{
		return;
	}
	const int32 NumMaps = W->GetWad()->MapNames().Num();
	switch (W->MenuItem)
	{
	case 1: W->MenuMapIndex = (W->MenuMapIndex + 1) % FMath::Max(1, NumMaps); break;
	case 2: W->MenuSkill = FMath::Clamp(W->MenuSkill + 1, 1, 5); break;
	case 3: W->MenuModern = !W->MenuModern; break;
	default: return;
	}
	W->PlaySound(TEXT("STNMOV"), nullptr);
}

void ADoomPlayerController::MenuSelect(const FInputActionValue& V)
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!W)
	{
		return;
	}
	switch (W->GetState())
	{
	case EDoomGameState::Title:
		W->SetState(EDoomGameState::Menu);
		W->MenuItem = 0;
		W->PlaySound(TEXT("SWTCHN"), nullptr);
		break;
	case EDoomGameState::Intermission:
		if (W->IntermissionTic > Doom::TicRate)
		{
			W->PlaySound(TEXT("BAREXP"), nullptr);
			W->FinishIntermission();
		}
		break;
	case EDoomGameState::Menu:
	{
		W->PlaySound(TEXT("PISTOL"), nullptr);
		const TArray<FString> Entries = ADoomHUD::MenuEntries(W);
		const FString& Item = Entries.IsValidIndex(W->MenuItem) ? Entries[W->MenuItem] : FString();
		if (W->MenuItem == 0)
		{
			if (W->GetMap())
			{
				W->SetModern(W->MenuModern);
				W->SetState(EDoomGameState::Playing); // resume
			}
			else if (W->GetWad() && W->GetWad()->MapNames().Num())
			{
				W->SetModern(W->MenuModern);
				W->StartNewGame(W->GetWad()->MapNames()[0], W->MenuSkill);
			}
		}
		else if (W->MenuItem == 1 || W->MenuItem == 2 || W->MenuItem == 3)
		{
			if (W->GetWad() && W->GetWad()->MapNames().IsValidIndex(W->MenuMapIndex))
			{
				W->SetModern(W->MenuModern);
				W->StartNewGame(W->GetWad()->MapNames()[W->MenuMapIndex], W->MenuSkill);
			}
		}
		else if (Item.StartsWith(TEXT("QUIT")))
		{
			UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, false);
		}
		break;
	}
	default:
		break;
	}
}

void ADoomPlayerController::MenuBack(const FInputActionValue& V)
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (!W)
	{
		return;
	}
	if (W->GetState() == EDoomGameState::Playing)
	{
		W->SetState(EDoomGameState::Menu);
		W->MenuItem = 0;
		W->PlaySound(TEXT("SWTCHN"), nullptr);
	}
	else if (W->GetState() == EDoomGameState::Menu)
	{
		W->SetState(W->GetMap() ? EDoomGameState::Playing : EDoomGameState::Title);
		W->PlaySound(TEXT("SWTCHX"), nullptr);
	}
}
