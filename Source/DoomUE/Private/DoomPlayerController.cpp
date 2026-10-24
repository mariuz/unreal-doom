#include "DoomPlayerController.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "DoomWorld.h"
#include "DoomHUD.h"
#include "DoomMenu.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "DoomInput.h"
#include "UserSettings/EnhancedInputUserSettings.h"
#include "GameplayTagContainer.h"
#include "Kismet/KismetSystemLibrary.h"
#include "DoomPlayer.h"
#include "UnrealClient.h"
#include "TimerManager.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"

namespace
{
	// the intermission tic by which the kills, items, secrets and time have all counted up (DoomHUD)
	constexpr int32 IntermissionCountedTic = 150;
}

void ADoomPlayerController::CreateInput()
{
	if (MoveAction)
	{
		return;
	}
	// the saved assets (/Game/Doom/Input, made by the content builder), else the same set built here
	FDoomInputObjects In;
	const bool bAssets = DoomInput::LoadAssets(In);
	if (!bAssets)
	{
		DoomInput::Build([this](UClass* Class, FName Name) { return NewObject<UObject>(this, Class, Name); }, In);
	}
	UE_LOG(LogDoom, Log, TEXT("Input: %s"), bAssets ? TEXT("the assets in /Game/Doom/Input") : TEXT("built in code (no assets in /Game/Doom/Input)"));
	MoveAction = In.Move;
	LookAction = In.Look;
	FireAction = In.Fire;
	UseAction = In.Use;
	RunAction = In.Run;
	AutomapAction = In.Automap;
	AutomapZoomAction = In.AutomapZoom;
	AutomapPanAction = In.AutomapPan;
	AutomapFollowAction = In.AutomapFollow;
	AutomapBigAction = In.AutomapBig;
	AutomapMarkAction = In.AutomapMark;
	AutomapClearAction = In.AutomapClear;
	AutomapGridAction = In.AutomapGrid;
	NextWeaponAction = In.NextWeapon;
	PrevWeaponAction = In.PrevWeapon;
	SlotActions = TArray<TObjectPtr<UInputAction>>(In.Slots);
	MenuUpAction = In.MenuUp;
	MenuDownAction = In.MenuDown;
	MenuLeftAction = In.MenuLeft;
	MenuRightAction = In.MenuRight;
	MenuSelectAction = In.MenuSelect;
	MenuBackAction = In.MenuBack;
	MenuCloseAction = In.MenuClose;
	QuickSaveAction = In.QuickSave;
	QuickLoadAction = In.QuickLoad;
	PageActions = TArray<TObjectPtr<UInputAction>>(In.Pages);
	GameplayContext = In.Gameplay;
	MenuContext = In.Menu;
	AutomapPanContext = In.AutomapPanContext;
}

UEnhancedInputUserSettings* ADoomPlayerController::UserSettings() const
{
	const UEnhancedInputLocalPlayerSubsystem* Sub = GetLocalPlayer() ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()) : nullptr;
	return Sub ? Sub->GetUserSettings() : nullptr;
}

TArray<ADoomPlayerController::FBinding> ADoomPlayerController::GetBindings() const
{
	TArray<FBinding> Out;
	const UEnhancedInputUserSettings* Settings = UserSettings();
	const UEnhancedPlayerMappableKeyProfile* Profile = Settings ? Settings->GetActiveKeyProfile() : nullptr;
	for (const DoomInput::FMappable& M : DoomInput::Mappables())
	{
		FBinding B{ M.Name, M.Label };
		if (Profile)
		{
			// in slot order (first, second...)
			TArray<FPlayerKeyMapping> Rows = Profile->FindKeyMappingRow(M.Name) ? Profile->FindKeyMappingRow(M.Name)->Mappings.Array() : TArray<FPlayerKeyMapping>();
			Rows.Sort([](const FPlayerKeyMapping& A, const FPlayerKeyMapping& C) { return (uint8)A.GetSlot() < (uint8)C.GetSlot(); });
			for (const FPlayerKeyMapping& K : Rows)
			{
				if (K.GetCurrentKey().IsValid()) B.Keys.Add(K.GetCurrentKey());
			}
		}
		else if (GameplayContext)
		{
			// no user settings (disabled): the context's own keys
			for (const FEnhancedActionKeyMapping& K : GameplayContext->GetMappings())
			{
				if (K.IsPlayerMappable() && K.GetMappingName() == M.Name) B.Keys.Add(K.Key);
			}
		}
		Out.Add(B);
	}
	return Out;
}

bool ADoomPlayerController::Rebind(FName Mapping, const FKey& Key)
{
	UEnhancedInputUserSettings* Settings = UserSettings();
	if (!Settings || !Key.IsValid())
	{
		return false;
	}
	FMapPlayerKeyArgs Args;
	Args.MappingName = Mapping;
	Args.Slot = EPlayerMappableKeySlot::First;
	Args.NewKey = Key;
	FGameplayTagContainer Failure;
	Settings->MapPlayerKey(Args, Failure);
	Settings->ApplySettings();
	Settings->SaveSettings();
	return Failure.IsEmpty();
}

void ADoomPlayerController::ResetBindings()
{
	if (UEnhancedInputUserSettings* Settings = UserSettings())
	{
		FGameplayTagContainer Failure;
		if (const UEnhancedPlayerMappableKeyProfile* Profile = Settings->GetActiveKeyProfile())
		{
			Settings->ResetKeyProfileIdToDefault(Profile->GetProfileIdString(), Failure);
		}
		Settings->ApplySettings();
		Settings->SaveSettings();
	}
}

void ADoomPlayerController::BeginPlay()
{
	Super::BeginPlay();
	SetInputMode(FInputModeGameOnly());
	bShowMouseCursor = false;
	ApplyContexts();
	if (IsLocalController() && GetNetMode() == NM_Client && FParse::Param(FCommandLine::Get(), TEXT("nettest")))
	{
		RunClientNetTest();
	}
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
	EIC->BindActionValueLambda(MenuCloseAction, ETriggerEvent::Started, [this](const FInputActionValue& V)
	{
		UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
		if (W && !W->bInputLocked && W->GetState() == EDoomGameState::Menu)
		{
			W->PlaySound(TEXT("SWTCHX"), nullptr);
			W->Menu.Close(*W); // M_ClearMenus
		}
		else
		{
			MenuBack(V); // the title screen and the finale: as before
		}
	});
	// the automap
	auto Map = [this]() -> ADoomHUD*
	{
		UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
		return W && !W->bInputLocked && W->bAutomap && W->GetState() == EDoomGameState::Playing ? Cast<ADoomHUD>(GetHUD()) : nullptr;
	};
	EIC->BindActionValueLambda(AutomapZoomAction, ETriggerEvent::Triggered, [this, Map](const FInputActionValue& V)
	{
		if (ADoomHUD* H = Map()) H->AutomapZoom(V.Get<float>(), GetWorld()->GetDeltaSeconds());
	});
	EIC->BindActionValueLambda(AutomapPanAction, ETriggerEvent::Triggered, [this, Map](const FInputActionValue& V)
	{
		if (ADoomHUD* H = Map()) H->AutomapPan(V.Get<FVector2D>(), GetWorld()->GetDeltaSeconds());
	});
	EIC->BindActionValueLambda(AutomapFollowAction, ETriggerEvent::Started, [Map](const FInputActionValue&) { if (ADoomHUD* H = Map()) H->AutomapToggleFollow(); });
	EIC->BindActionValueLambda(AutomapBigAction, ETriggerEvent::Started, [Map](const FInputActionValue&) { if (ADoomHUD* H = Map()) H->AutomapToggleBig(); });
	EIC->BindActionValueLambda(AutomapMarkAction, ETriggerEvent::Started, [Map](const FInputActionValue&) { if (ADoomHUD* H = Map()) H->AutomapMark(); });
	EIC->BindActionValueLambda(AutomapClearAction, ETriggerEvent::Started, [Map](const FInputActionValue&) { if (ADoomHUD* H = Map()) H->AutomapClearMarks(); });
	EIC->BindActionValueLambda(AutomapGridAction, ETriggerEvent::Started, [Map](const FInputActionValue&) { if (ADoomHUD* H = Map()) H->AutomapToggleGrid(); });
	// F1 Read This!, F2 save, F3 load, F4 sound volume
	const EMenuPage FPages[4] = { EMenuPage::ReadThis, EMenuPage::Save, EMenuPage::Load, EMenuPage::Sound };
	for (int32 i = 0; i < PageActions.Num(); ++i)
	{
		const int32 Page = (int32)FPages[i];
		EIC->BindActionValueLambda(PageActions[i], ETriggerEvent::Started, [this, Page](const FInputActionValue&) { OpenMenuPage(Page); });
	}
	EIC->BindActionValueLambda(QuickSaveAction, ETriggerEvent::Started, [this](const FInputActionValue&)
	{
		if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>()) W->SaveGame(0);
	});
	EIC->BindActionValueLambda(QuickLoadAction, ETriggerEvent::Started, [this](const FInputActionValue&)
	{
		if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>()) W->LoadGame(0);
	});
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
	// the automap with follow mode off takes the arrow keys for panning, ahead of moving and turning
	const ADoomHUD* H = Cast<ADoomHUD>(GetHUD());
	const bool bPan = !bMenu && W->bAutomap && H && !H->AutomapFollowing();
	if (bContextsInitialized && bMenu == bMenuContextActive && bPan == bPanContextActive)
	{
		return;
	}
	if (!bContextsInitialized)
	{
		// its player-mappable keys (and the player's saved changes) join the user settings' key profile
		if (UEnhancedInputUserSettings* Settings = Sub->GetUserSettings())
		{
			Settings->RegisterInputMappingContext(GameplayContext);
		}
	}
	bContextsInitialized = true;
	bMenuContextActive = bMenu;
	bPanContextActive = bPan;
	Sub->ClearAllMappings();
	Sub->AddMappingContext(bMenu ? MenuContext : GameplayContext, 0);
	if (bPan)
	{
		Sub->AddMappingContext(AutomapPanContext, 1);
	}
}

void ADoomPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ApplyContexts();
}

namespace
{
	UDoomWorld* MenuWorld(UWorld* World)
	{
		UDoomWorld* W = World ? World->GetSubsystem<UDoomWorld>() : nullptr;
		return (W && !W->bInputLocked) ? W : nullptr;
	}
}

void ADoomPlayerController::MenuUp(const FInputActionValue& V)
{
	if (UDoomWorld* W = MenuWorld(GetWorld()); W && W->GetState() == EDoomGameState::Menu) W->Menu.Up(*W);
}

void ADoomPlayerController::MenuDown(const FInputActionValue& V)
{
	if (UDoomWorld* W = MenuWorld(GetWorld()); W && W->GetState() == EDoomGameState::Menu) W->Menu.Down(*W);
}

void ADoomPlayerController::MenuLeft(const FInputActionValue& V)
{
	if (UDoomWorld* W = MenuWorld(GetWorld()); W && W->GetState() == EDoomGameState::Menu) W->Menu.Left(*W);
}

void ADoomPlayerController::MenuRight(const FInputActionValue& V)
{
	if (UDoomWorld* W = MenuWorld(GetWorld()); W && W->GetState() == EDoomGameState::Menu) W->Menu.Right(*W);
}

void ADoomPlayerController::MenuSelect(const FInputActionValue& V)
{
	UDoomWorld* W = MenuWorld(GetWorld());
	if (!W)
	{
		return;
	}
	switch (W->GetState())
	{
	case EDoomGameState::Title:
		W->Menu.Open(*W, EMenuPage::Main);
		break;
	case EDoomGameState::Menu:
		W->Menu.Select(*W);
		break;
	case EDoomGameState::Finale:
		if (HasAuthority()) W->FinaleInput();
		else ServerFinaleInput(); // the server runs the story screens; any player moves them on
		break;
	case EDoomGameState::Intermission:
		if (W->Flow().IntermissionTic > Doom::TicRate)
		{
			W->PlaySoundLocal(TEXT("BAREXP"), nullptr);
			if (HasAuthority() && W->Flow().IntermissionTic < IntermissionCountedTic)
			{
				W->Flow().IntermissionTic = IntermissionCountedTic; // the first press finishes the counts
			}
			else if (HasAuthority())
			{
				W->FinishIntermission();
			}
			else
			{
				ServerFinishIntermission(); // any player may go on, as in Doom
			}
		}
		break;
	default:
		break;
	}
}

void ADoomPlayerController::MenuBack(const FInputActionValue& V)
{
	UDoomWorld* W = MenuWorld(GetWorld());
	if (!W)
	{
		return;
	}
	switch (W->GetState())
	{
	case EDoomGameState::Finale: W->FinishFinale(); break;
	case EDoomGameState::Playing:
	case EDoomGameState::Title: W->Menu.Open(*W, EMenuPage::Main); break;
	case EDoomGameState::Menu: W->Menu.Back(*W); break;
	default: break;
	}
}

bool ADoomPlayerController::InputKey(const FInputKeyEventArgs& Params)
{
	UDoomWorld* W = GetWorld() ? GetWorld()->GetSubsystem<UDoomWorld>() : nullptr;
	if (W && W->GetState() == EDoomGameState::Menu && W->Menu.bEditing && !W->bInputLocked)
	{
		if (Params.Event == IE_Pressed || Params.Event == IE_Repeat)
		{
			W->Menu.EditKey(*W, Params.Key);
			return true; // swallowed: no menu navigation or game input while typing
		}
		return Super::InputKey(Params); // releases go through, so no key is left held down
	}
	// the Controls page waiting for a key: the next key or mouse button pressed is the new binding
	if (W && W->GetState() == EDoomGameState::Menu && W->Menu.bCapturing && !W->bInputLocked
		&& Params.Event == IE_Pressed && !Params.Key.IsAxis1D() && !Params.Key.IsAxis2D())
	{
		W->Menu.CaptureKey(*W, Params.Key);
		return true;
	}
	// yes/no prompts (quit, end game, nightmare): only Y, Enter or the gamepad's A confirm; any other
	// key or button cancels, as in Doom, so a stray click can't quit the game
	if (W && W->GetState() == EDoomGameState::Menu && !W->Menu.Prompt().IsEmpty() && !W->bInputLocked && !Params.Key.IsAxis1D() && !Params.Key.IsAxis2D())
	{
		if (Params.Event == IE_Pressed)
		{
			if (Params.Key == EKeys::Y || Params.Key == EKeys::Enter || Params.Key == EKeys::Gamepad_FaceButton_Bottom)
			{
				W->Menu.Select(*W);
			}
			else
			{
				W->Menu.Back(*W);
			}
			return true;
		}
	}
	return Super::InputKey(Params);
}

void ADoomPlayerController::OpenMenuPage(int32 Page)
{
	UDoomWorld* W = MenuWorld(GetWorld());
	if (W && (W->GetState() == EDoomGameState::Playing || W->GetState() == EDoomGameState::Title))
	{
		W->Menu.Open(*W, (EMenuPage)Page);
	}
}

void ADoomPlayerController::ServerFinaleInput_Implementation()
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (W && W->GetState() == EDoomGameState::Finale)
	{
		W->FinaleInput();
	}
}

void ADoomPlayerController::ServerFinishIntermission_Implementation()
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	if (W && W->GetState() == EDoomGameState::Intermission && W->Flow().IntermissionTic > Doom::TicRate)
	{
		W->FinishIntermission();
	}
}

void ADoomPlayerController::RunClientNetTest()
{
	// -nettest on a client: log what arrived from the server, fire through the server RPC, and take
	// screenshots of the other player and of the intermission
	TSharedRef<float> Clock = MakeShared<float>(0.f);
	FTimerHandle T;
	TWeakObjectPtr<ADoomPlayerController> Weak(this);
	GetWorldTimerManager().SetTimer(T, [Weak, Clock]()
	{
		ADoomPlayerController* Self = Weak.Get();
		if (!Self || !Self->GetWorld())
		{
			return; // the connection went away and took this controller with it
		}
		UDoomWorld* W = Self->GetWorld()->GetSubsystem<UDoomWorld>();
		if (!W || !W->GetMap())
		{
			return; // still joining
		}
		*Clock += 0.5f;
		const float Now = *Clock;
		ADoomPlayer* Me = Cast<ADoomPlayer>(Self->GetPawn());
		if (FMath::Fmod(Now, 1.f) < 0.01f)
		{
			UE_LOG(LogDoom, Display, TEXT("Nettest client t=%.0f: %s | me hp%d ammo%d"), Now, *W->NetFingerprint(),
				Me ? Me->Inv().Health : -1, Me ? Me->Inv().Ammo[0] : -1);
		}
		if (Me && FMath::IsNearlyEqual(Now, 3.f)) Me->FirePressed(FInputActionValue(true));
		if (Me && FMath::IsNearlyEqual(Now, 4.f)) Me->FireReleased(FInputActionValue(false));
		// dead (deathmatch: the host fragged us): press use to be reborn at a start
		if (Me && Me->IsDead() && FMath::IsNearlyEqual(Now, 4.5f)) Me->UsePressed(FInputActionValue(true));
		if (FMath::IsNearlyEqual(Now, 8.5f) && Me)
		{
			for (ADoomPlayer* Other : W->GetPlayers())
			{
				if (Other != Me)
				{
					const FRotator Look = (Other->GetActorLocation() - Me->GetActorLocation()).Rotation();
					Self->SetControlRotation(FRotator(0, Look.Yaw, 0));
				}
			}
		}
		if (FMath::IsNearlyEqual(Now, 9.f))
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / TEXT("net_client_play.png"), true, false);
		}
		if (FMath::IsNearlyEqual(Now, 22.f))
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ScreenShotDir() / TEXT("net_client_inter.png"), true, false);
		}
		if (FMath::IsNearlyEqual(Now, 30.f))
		{
			FPlatformMisc::RequestExit(false, TEXT("DoomNettest"));
		}
	}, 0.5f, true);
}
