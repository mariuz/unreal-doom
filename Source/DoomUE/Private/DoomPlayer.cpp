#include "DoomPlayer.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "DoomWorld.h"
#include "DoomMonster.h"
#include "DoomThing.h"
#include "DoomPlayerController.h"
#include "DoomSettings.h"
#include "DoomLevel.h"
#include "Interfaces/MovementBaseInterface.h"
#include "DoomGraphics.h"
#include "DoomUE.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EnhancedInputComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DamageEvents.h"
#include "DoomSpriteComponent.h"
#include "DoomGameState.h"
#include "DoomGameMode.h"
#include "Net/UnrealNetwork.h"

namespace
{
	int32 PlayerRand() { return DoomInfo::Random(); }
	constexpr float WeaponTop = 32.f;
	constexpr float WeaponBottom = 128.f;
	constexpr float RaiseSpeed = 6.f;

	const EDoomWeapon CycleOrder[] = {
		EDoomWeapon::Fist, EDoomWeapon::Chainsaw, EDoomWeapon::Pistol, EDoomWeapon::Shotgun, EDoomWeapon::SuperShotgun,
		EDoomWeapon::Chaingun, EDoomWeapon::Rocket, EDoomWeapon::Plasma, EDoomWeapon::BFG };
}

void UDoomPlayerMovement::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	if (!HasValidData() || HasAnimRootMotion() || DeltaTime < MIN_TICK_TIME)
	{
		return;
	}
	if (!IsMovingOnGround())
	{
		return; // Doom: no thrust and no friction off the ground
	}
	// v' = v * f^t + a / k * (1 - f^t): the exact solution of Doom's per-tic friction plus thrust, so
	// the frame rate doesn't matter. MaxAcceleration is the full-input thrust that settles at MaxWalkSpeed.
	const float K = -FMath::Loge(0.90625f) * Doom::TicRate; // friction rate per second
	const float Keep = FMath::Exp(-K * DeltaTime);
	const FVector Thrust = Acceleration.GetClampedToMaxSize(GetMaxAcceleration()) / GetMaxAcceleration() * GetMaxSpeed();
	const float Z = Velocity.Z;
	Velocity = Velocity * Keep + Thrust * (1.f - Keep);
	Velocity.Z = Z;
	// STOPSPEED: a crawl with no input stops dead; MAXMOVE caps the momentum at 30 units per tic
	if (Acceleration.IsNearlyZero() && Velocity.Size2D() < Doom::TicRate * Doom::Scale / 16.f)
	{
		Velocity.X = Velocity.Y = 0.f;
	}
	const float MaxMove = 30.f * Doom::TicRate * Doom::Scale;
	if (Velocity.Size2D() > MaxMove)
	{
		const FVector Flat = FVector(Velocity.X, Velocity.Y, 0).GetSafeNormal() * MaxMove;
		Velocity = FVector(Flat.X, Flat.Y, Z);
	}
}

ADoomPlayer::ADoomPlayer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UDoomPlayerMovement>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	const float S = Doom::Scale;
	UCapsuleComponent* Cap = GetCapsuleComponent();
	Cap->InitCapsuleSize(16.f * S, 28.f * S);
	Cap->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Cap);
	Camera->SetRelativeLocation(FVector(0, 0, (41.f - 28.f) * S));
	Camera->bUsePawnControlRotation = true;
	Camera->SetFieldOfView(90.f);
	Camera->PostProcessBlendWeight = 1.f;

	MuzzleLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("MuzzleLight"));
	MuzzleLight->SetupAttachment(Camera);
	MuzzleLight->SetRelativeLocation(FVector(24.f * S, 0, -6.f * S));
	MuzzleLight->SetIntensity(0.f);
	MuzzleLight->SetAttenuationRadius(400.f * S);
	MuzzleLight->SetLightColor(FLinearColor(1.f, 0.75f, 0.45f));
	MuzzleLight->SetCastShadows(true);

	Body = CreateDefaultSubobject<UDoomSpriteComponent>(TEXT("Body"));
	Body->SetupAttachment(Cap);
	Body->SetOwnerNoSee(true);

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	if (GetMesh())
	{
		GetMesh()->SetVisibility(false);
	}

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxStepHeight = 24.f * S;
	Move->GravityScale = 3.75f;           // Doom: 1 unit/tic^2
	Move->MaxWalkSpeed = 16.67f * Doom::TicRate * S; // top running speed (UDoomPlayerMovement)
	Move->MaxAcceleration = 4000.f * S;              // only a scale for the input in UDoomPlayerMovement
	Move->AirControl = 0.f;
	Move->JumpZVelocity = 0.f;
	Move->bCanWalkOffLedges = true;
	Move->SetWalkableFloorAngle(50.f);
	Move->PerchRadiusThreshold = 4.f * S;
	Move->bUseFlatBaseForFloorChecks = true; // Doom's square feet: no sliding off step edges
	Move->NavAgentProps.bCanCrouch = false;
}

void ADoomPlayer::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	ADoomPlayerController* PC = Cast<ADoomPlayerController>(GetController());
	if (!EIC || !PC || !PC->MoveAction)
	{
		return;
	}
	EIC->BindAction(PC->MoveAction, ETriggerEvent::Triggered, this, &ADoomPlayer::Move);
	EIC->BindAction(PC->LookAction, ETriggerEvent::Triggered, this, &ADoomPlayer::Look);
	EIC->BindAction(PC->FireAction, ETriggerEvent::Started, this, &ADoomPlayer::FirePressed);
	EIC->BindAction(PC->FireAction, ETriggerEvent::Completed, this, &ADoomPlayer::FireReleased);
	EIC->BindAction(PC->UseAction, ETriggerEvent::Started, this, &ADoomPlayer::UsePressed);
	EIC->BindAction(PC->RunAction, ETriggerEvent::Started, this, &ADoomPlayer::RunPressed);
	EIC->BindAction(PC->RunAction, ETriggerEvent::Completed, this, &ADoomPlayer::RunReleased);
	EIC->BindAction(PC->AutomapAction, ETriggerEvent::Started, this, &ADoomPlayer::ToggleAutomap);
	EIC->BindActionValueLambda(PC->NextWeaponAction, ETriggerEvent::Started, [this](const FInputActionValue&) { CycleWeapon(1); });
	EIC->BindActionValueLambda(PC->PrevWeaponAction, ETriggerEvent::Started, [this](const FInputActionValue&) { CycleWeapon(-1); });
	for (int32 i = 0; i < PC->SlotActions.Num(); ++i)
	{
		EIC->BindActionValueLambda(PC->SlotActions[i], ETriggerEvent::Started, [this, i](const FInputActionValue&) { SelectSlot(i + 1); });
	}
}

void ADoomPlayer::SetBase(FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName, bool bNotifyActor)
{
	const UActorComponent* Base = MovementBaseInterfaceData ? Cast<UActorComponent>(MovementBaseInterfaceData->PhysicsObjectOwner.Get()) : nullptr;
	if (Base && Base->GetOwner() && Base->GetOwner()->IsA<ADoomLevel>())
	{
		MovementBaseInterfaceData = nullptr;
	}
	Super::SetBase(MovementBaseInterfaceData, BoneName, bNotifyActor);
}

void ADoomPlayer::BeginPlay()
{
	Super::BeginPlay();
	if (!DoomWorld)
	{
		DoomWorld = GetWorld()->GetSubsystem<UDoomWorld>();
	}
}

void ADoomPlayer::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(ADoomPlayer, View, COND_OwnerOnly);
}

void ADoomPlayer::InitForLevel(UDoomWorld* InWorld, bool bPistolStart)
{
	DoomWorld = InWorld;
	if (HasAuthority() && DoomWorld->GetGraphics())
	{
		Body->Init(DoomWorld, TEXT("PLAY"));
		BodySeq.Reset();
	}
	if (!HasAuthority())
	{
		// a network client's own pawn: the server owns the inventory and the weapon state
		Camera->SetFieldOfView(GetDefault<UDoomSettings>()->FieldOfView);
		return;
	}
	if (bPistolStart || Inv().Health <= 0)
	{
		Inv().Reset();
		Inv().Health = DoomInfo::Rules().InitialHealth;
		Inv().Ammo[0] = DoomInfo::Rules().InitialBullets;
	}
	for (bool& K : Inv().Keys) K = false;
	Invulnerability = Invisibility = RadSuit = LightAmp = 0;
	bBerserk = false;
	DamageCount = BonusCount = ExtraLight = 0;
	DeathTics = 0;
	ReactionTime = 0;
	PendingWeapon = EDoomWeapon::Count;
	Phase = EWeaponPhase::Raising;
	WeaponY = WeaponBottom;
	FlashFrame = 0;
	SetWeaponSprite();
	LastDoomPos = DoomPos();
	Camera->SetRelativeLocation(FVector(0, 0, (41.f - 28.f) * Doom::Scale));
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	GetCharacterMovement()->StopMovementImmediately();
	const UDoomSettings* Settings = GetDefault<UDoomSettings>();
	Camera->SetFieldOfView(Settings->FieldOfView);
	if (!DoomWorld->IsModern())
	{
		MuzzleLight->SetVisibility(false);
	}
	else
	{
		MuzzleLight->SetVisibility(true);
	}
}

FVector2f ADoomPlayer::DoomPos() const { return Doom::ToDoom2D(GetActorLocation()); }
float ADoomPlayer::DoomZ() const { return Doom::ToDoomZ(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); }
float ADoomPlayer::DoomAngle() const { return Doom::YawToAngle(GetControlRotation().Yaw); }
FVector ADoomPlayer::EyeLocation() const { return Camera->GetComponentLocation(); }

bool ADoomPlayer::HasKey(int32 KeyColor) const
{
	return Inv().Keys[KeyColor] || Inv().Keys[KeyColor + 3];
}

// ------------------------------------------------------------------------------------------------
// Input
// ------------------------------------------------------------------------------------------------

bool ADoomPlayer::InputLocked() const
{
	return DoomWorld && DoomWorld->bInputLocked;
}

void ADoomPlayer::Move(const FInputActionValue& V)
{
	if (InputLocked() || IsDead() || ReactionTime > 0 || !DoomWorld || DoomWorld->GetState() != EDoomGameState::Playing)
	{
		return;
	}
	const FVector2D In = V.Get<FVector2D>();
	const FRotator Yaw(0, GetControlRotation().Yaw, 0);
	const bool bRun = GetDefault<UDoomSettings>()->bAlwaysRun != bRunHeld;
	const float Scale = bRun ? 1.f : 0.5f;
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::X), In.Y * Scale);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), In.X * Scale * (bRun ? 0.8f : 0.96f));
}

void ADoomPlayer::Look(const FInputActionValue& V)
{
	if (InputLocked() || IsDead() || !DoomWorld || DoomWorld->GetState() != EDoomGameState::Playing)
	{
		return;
	}
	const FVector2D In = V.Get<FVector2D>() * GetDefault<UDoomSettings>()->MouseSensitivity;
	AddControllerYawInput(In.X);
	AddControllerPitchInput(-In.Y);
}

void ADoomPlayer::FirePressed(const FInputActionValue& V)
{
	bFireHeld = !InputLocked();
	if (!HasAuthority())
	{
		ServerSetFire(bFireHeld);
	}
}

void ADoomPlayer::FireReleased(const FInputActionValue& V)
{
	bFireHeld = false;
	if (!HasAuthority())
	{
		ServerSetFire(false);
	}
}

void ADoomPlayer::ServerSetFire_Implementation(bool bHeld) { bFireHeld = bHeld; }
void ADoomPlayer::ServerUse_Implementation() { UseLinesOrRespawn(); }
void ADoomPlayer::ServerSelectSlot_Implementation(int32 Slot) { SelectSlotNow(Slot); }
void ADoomPlayer::ServerCycleWeapon_Implementation(int32 Dir) { CycleWeaponNow(Dir); }

void ADoomPlayer::UsePressed(const FInputActionValue& V)
{
	if (!DoomWorld || InputLocked())
	{
		return;
	}
	if (!HasAuthority())
	{
		ServerUse();
		return;
	}
	UseLinesOrRespawn();
}

void ADoomPlayer::UseLinesOrRespawn()
{
	if (!DoomWorld)
	{
		return;
	}
	if (IsDead())
	{
		if (DeathTics > Doom::TicRate)
		{
			if (DoomWorld->IsNetGame())
			{
				// network games: reborn at a start with a pistol; the level carries on
				if (ADoomGameMode* GM = GetWorld()->GetAuthGameMode<ADoomGameMode>())
				{
					GM->RespawnPlayer(GetController());
				}
			}
			else
			{
				DoomWorld->LoadMap(DoomWorld->GetMapName(), true);
			}
		}
		return;
	}
	DoomWorld->UseLines(this, GetActorLocation(), DoomAngle());
}

void ADoomPlayer::Message(const FString& Text)
{
	if (IsLocallyControlled())
	{
		if (DoomWorld) DoomWorld->ShowMessage(Text);
	}
	else if (HasAuthority())
	{
		ClientShowMessage(Text);
	}
}

void ADoomPlayer::ClientShowMessage_Implementation(const FString& Text)
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>()) W->ShowMessage(Text);
}

void ADoomPlayer::PlaySelfSound(const FString& Name, float Volume)
{
	if (!DoomWorld)
	{
		return;
	}
	const FVector L = GetActorLocation();
	if (IsLocallyControlled())
	{
		DoomWorld->PlaySoundLocal(Name, nullptr, Volume);
	}
	else if (HasAuthority())
	{
		ClientPlaySound(Name, Volume);
		if (GetNetMode() == NM_ListenServer)
		{
			DoomWorld->PlaySoundLocal(Name, &L, Volume); // the host hears the other player
		}
	}
	if (DoomWorld->IsNetServer())
	{
		if (ADoomGameState* GS = DoomWorld->GetDoomGameState())
		{
			GS->MulticastSound(Name, L, true, Volume, GetPlayerState()); // everyone but this player
		}
	}
}

void ADoomPlayer::ClientPlaySound_Implementation(const FString& Name, float Volume)
{
	if (UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>()) W->PlaySoundLocal(Name, nullptr, Volume);
}

void ADoomPlayer::PublishView()
{
	View.WeaponSprite = WeaponSprite;
	View.FlashSprite = FlashSprite;
	View.WeaponFrame = (uint16)WeaponFrame;
	View.FlashFrame = (uint16)FlashFrame;
	View.WeaponY = WeaponY;
	View.BobX = BobX;
	View.BobY = BobY;
	View.CameraZ = Camera->GetRelativeLocation().Z;
	View.Invulnerability = Invulnerability;
	View.Invisibility = Invisibility;
	View.RadSuit = RadSuit;
	View.LightAmp = LightAmp;
	View.BerserkTics = BerserkTics;
	View.DamageCount = DamageCount;
	View.BonusCount = BonusCount;
	View.ExtraLight = ExtraLight;
	View.FaceDamageDir = FaceDamageDir;
	View.FaceEvilGrin = FaceEvilGrin;
	View.AttackDown = AttackDown;
	View.DeathTics = DeathTics;
	View.bBerserk = bBerserk;
	View.bGodMode = bGodMode;
}

void ADoomPlayer::OnRep_View()
{
	if (View.ExtraLight > ExtraLight && MuzzleLight && DoomWorld && DoomWorld->IsModern())
	{
		MuzzleLight->SetIntensity(60000.f);
	}
	WeaponSprite = View.WeaponSprite;
	FlashSprite = View.FlashSprite;
	WeaponFrame = (TCHAR)View.WeaponFrame;
	FlashFrame = (TCHAR)View.FlashFrame;
	WeaponY = View.WeaponY;
	BobX = View.BobX;
	BobY = View.BobY;
	Invulnerability = View.Invulnerability;
	Invisibility = View.Invisibility;
	RadSuit = View.RadSuit;
	LightAmp = View.LightAmp;
	BerserkTics = View.BerserkTics;
	DamageCount = View.DamageCount;
	BonusCount = View.BonusCount;
	ExtraLight = View.ExtraLight;
	FaceDamageDir = View.FaceDamageDir;
	FaceEvilGrin = View.FaceEvilGrin;
	AttackDown = View.AttackDown;
	DeathTics = View.DeathTics;
	bBerserk = View.bBerserk;
	bGodMode = View.bGodMode;
	const FVector Rel = Camera->GetRelativeLocation();
	Camera->SetRelativeLocation(FVector(Rel.X, Rel.Y, View.CameraZ));
}

void ADoomPlayer::AnimateBody()
{
	if (!Body)
	{
		return;
	}
	// players 2-4 wear grey, brown and red (Doom's translation tables); the server's value replicates
	if (const ADoomPlayerState* PS = GetPlayerState<ADoomPlayerState>())
	{
		Body->SetTranslation(PS->PlayerNumber & 3);
	}
	// Doom's S_PLAY states: run ABCD, attack E (F while the flash shows), pain G, death H-N, gibs O-W
	FString Seq = TEXT("A");
	int32 Tics = 4;
	bool bLoop = true;
	if (IsDead())
	{
		Seq = Inv().Health < -100 ? TEXT("OPQRSTUVW") : TEXT("HIJKLMN"); // P_KillMobj: below -spawnhealth
		Tics = 5;
		bLoop = false;
	}
	else if (Phase == EWeaponPhase::Firing)
	{
		Seq = FlashFrame ? TEXT("F") : TEXT("E");
	}
	else if (DamageCount > 0 && FaceDamageDir != 0)
	{
		Seq = TEXT("G");
	}
	else if (GetVelocity().Size2D() > 1.f * Doom::Scale * Doom::TicRate)
	{
		Seq = TEXT("ABCD");
	}
	if (Seq != BodySeq)
	{
		BodySeq = Seq;
		Body->PlaySequence(Seq, Tics, bLoop, false);
	}
	Body->DoomAngle = DoomAngle();
	if (DoomWorld)
	{
		Body->SetSector(DoomWorld->SectorAt(GetActorLocation()));
	}
	Body->AnimateTic();
}

void ADoomPlayer::ToggleAutomap(const FInputActionValue& V)
{
	if (DoomWorld)
	{
		DoomWorld->bAutomap = !DoomWorld->bAutomap;
	}
}

void ADoomPlayer::SelectSlot(int32 Slot)
{
	if (IsDead() || InputLocked())
	{
		return;
	}
	if (!HasAuthority())
	{
		ServerSelectSlot(Slot);
		return;
	}
	SelectSlotNow(Slot);
}

void ADoomPlayer::SelectSlotNow(int32 Slot)
{
	if (IsDead())
	{
		return;
	}
	EDoomWeapon W = EDoomWeapon::Count;
	switch (Slot)
	{
	case 1:
		W = (Inv().Weapons[(int32)EDoomWeapon::Chainsaw] && !(Inv().Ready == EDoomWeapon::Chainsaw && bBerserk)) && Inv().Ready != EDoomWeapon::Chainsaw
			? EDoomWeapon::Chainsaw : EDoomWeapon::Fist;
		break;
	case 2: W = EDoomWeapon::Pistol; break;
	case 3:
		W = (Inv().Weapons[(int32)EDoomWeapon::SuperShotgun] && Inv().Ready != EDoomWeapon::SuperShotgun) ? EDoomWeapon::SuperShotgun : EDoomWeapon::Shotgun;
		if (!Inv().Weapons[(int32)W]) W = EDoomWeapon::SuperShotgun;
		break;
	case 4: W = EDoomWeapon::Chaingun; break;
	case 5: W = EDoomWeapon::Rocket; break;
	case 6: W = EDoomWeapon::Plasma; break;
	case 7: W = EDoomWeapon::BFG; break;
	default: break;
	}
	if (W != EDoomWeapon::Count && Inv().Weapons[(int32)W] && W != Inv().Ready)
	{
		PendingWeapon = W;
	}
}

void ADoomPlayer::CycleWeapon(int32 Dir)
{
	if (!HasAuthority())
	{
		ServerCycleWeapon(Dir);
		return;
	}
	CycleWeaponNow(Dir);
}

void ADoomPlayer::CycleWeaponNow(int32 Dir)
{
	int32 Cur = 0;
	for (int32 i = 0; i < UE_ARRAY_COUNT(CycleOrder); ++i) if (CycleOrder[i] == Inv().Ready) Cur = i;
	for (int32 k = 1; k < UE_ARRAY_COUNT(CycleOrder); ++k)
	{
		const EDoomWeapon W = CycleOrder[(Cur + Dir * k + UE_ARRAY_COUNT(CycleOrder) * 2) % UE_ARRAY_COUNT(CycleOrder)];
		const FDoomWeaponDef& D = DoomInfo::Weapon(W);
		if (Inv().Weapons[(int32)W] && (D.Ammo == EDoomAmmo::None || Inv().Ammo[(int32)D.Ammo] >= D.AmmoPerShot))
		{
			PendingWeapon = W;
			return;
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Tic
// ------------------------------------------------------------------------------------------------

void ADoomPlayer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// the view (P_CalcHeight): bob while walking, ease back up after a step or a hard landing
	const float FeetZ = DoomZ();
	if (IsLocallyControlled() && !IsDead() && DoomWorld && DoomWorld->GetState() == EDoomGameState::Playing)
	{
		const float Tics = DeltaSeconds * Doom::TicRate;
		const float Rise = FeetZ - LastFeetZ;
		if (GetCharacterMovement()->IsMovingOnGround() && Rise > 0.5f && Rise <= 24.5f)
		{
			ViewHeightOffset -= Rise; // stepped up: the eye follows over a few tics
		}
		ViewHeightOffset = FMath::Max(ViewHeightOffset + ViewHeightDelta * Tics, -20.5f);
		if (ViewHeightOffset < 0.f)
		{
			ViewHeightDelta += 0.25f * Tics;
		}
		else
		{
			ViewHeightOffset = ViewHeightDelta = 0.f;
		}
		const float Speed = GetVelocity().Size2D() / Doom::Scale / Doom::TicRate;
		const float Bob = GetCharacterMovement()->IsMovingOnGround() ? FMath::Min(Speed * Speed / 4.f, 16.f) : 0.f;
		const float BobZ = Bob * 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * Doom::TicRate * 2.f * PI / 20.f);
		const FVector Rel = Camera->GetRelativeLocation();
		Camera->SetRelativeLocation(FVector(Rel.X, Rel.Y, FMath::Min(41.f + ViewHeightOffset + BobZ, Doom::HalfHeight(this) * 2.f / Doom::Scale - 4.f) * Doom::Scale - Doom::HalfHeight(this)));
	}
	LastFeetZ = FeetZ;
	if (MuzzleLight && MuzzleLight->Intensity > 0)
	{
		MuzzleLight->SetIntensity(FMath::Max(0.f, MuzzleLight->Intensity - DeltaSeconds * 400000.f));
	}
}

void ADoomPlayer::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	// P_ZMovement: a fall faster than 8 units per tic squats the view (and says "oof")
	const float FallSpeed = -GetVelocity().Z / Doom::Scale / Doom::TicRate;
	LastLandingVelocity = FallSpeed;
	if (FallSpeed > 8.f && !IsDead())
	{
		ViewHeightDelta = -FallSpeed / 8.f;
		PlaySelfSound(TEXT("OOF"));
	}
}

void ADoomPlayer::DoomTic()
{
	if (!DoomWorld)
	{
		return;
	}
	if (ReactionTime > 0)
	{
		--ReactionTime;
	}
	const FVector2f Pos = DoomPos();
	if (!IsDead())
	{
		DoomWorld->CheckCrossings(this, LastDoomPos, Pos, false);
	}
	LastDoomPos = DoomPos(); // re-read: a teleport line moves the player
	if (DoomWorld->GetState() != EDoomGameState::Playing)
	{
		return;
	}
	if (IsDead())
	{
		TicDeath();
	}

	// weapon bob from horizontal speed (Doom units per tic)
	const float Speed = GetVelocity().Size2D() / Doom::Scale / Doom::TicRate;
	const float Bob = FMath::Min(Speed * Speed / 4.f, 16.f);
	BobPhase = DoomWorld->GetTic() * 2.f * PI / 64.f;
	if (Phase == EWeaponPhase::Ready)
	{
		// A_WeaponReady moves the weapon; firing, raising and lowering leave it where it was
		BobX = FMath::Cos(BobPhase) * Bob;
		BobY = FMath::Abs(FMath::Sin(BobPhase)) * Bob;
	}

	TicWeapon();
	TicPowerups();
	if (DamageCount > 0) --DamageCount;
	if (BonusCount > 0) --BonusCount;
	if (FaceEvilGrin > 0) --FaceEvilGrin;
	if (AttackDown > 0) --AttackDown;
	AnimateBody();
	if (DoomWorld->IsNetServer())
	{
		PublishView();
	}
}

void ADoomPlayer::TicPowerups()
{
	if (bBerserk) ++BerserkTics;
	if (Invulnerability > 0) --Invulnerability;
	if (Invisibility > 0) --Invisibility;
	if (RadSuit > 0) --RadSuit;
	if (LightAmp > 0) --LightAmp;
}

void ADoomPlayer::TicDeath()
{
	++DeathTics;
	// the view sinks to the floor
	const FVector Rel = Camera->GetRelativeLocation();
	const float Target = (6.f - 28.f) * Doom::Scale;
	if (Rel.Z > Target)
	{
		Camera->SetRelativeLocation(FVector(Rel.X, Rel.Y, FMath::Max(Target, Rel.Z - 1.f * Doom::Scale)));
	}
}

void ADoomPlayer::SetWeaponSprite()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	WeaponSprite = D.Sprite;
	FlashSprite = D.FlashSprite;
	WeaponFrame = D.ReadyLetter();
}

bool ADoomPlayer::CheckAmmo()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	if (D.Ammo == EDoomAmmo::None || Inv().Ammo[(int32)D.Ammo] >= D.AmmoPerShot)
	{
		return true;
	}
	// out of ammo: Doom's preference order
	static const EDoomWeapon Pref[] = { EDoomWeapon::Plasma, EDoomWeapon::SuperShotgun, EDoomWeapon::Chaingun, EDoomWeapon::Shotgun,
		EDoomWeapon::Pistol, EDoomWeapon::Chainsaw, EDoomWeapon::Rocket, EDoomWeapon::BFG, EDoomWeapon::Fist };
	for (EDoomWeapon W : Pref)
	{
		const FDoomWeaponDef& WD = DoomInfo::Weapon(W);
		if (Inv().Weapons[(int32)W] && (WD.Ammo == EDoomAmmo::None || Inv().Ammo[(int32)WD.Ammo] >= WD.AmmoPerShot))
		{
			if (W != Inv().Ready) PendingWeapon = W;
			break;
		}
	}
	return false;
}

void ADoomPlayer::TicWeapon()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	if (FlashTics > 0 && --FlashTics == 0)
	{
		FlashFrame = 0;
		ExtraLight = 0;
	}

	switch (Phase)
	{
	case EWeaponPhase::Raising:
		WeaponY -= RaiseSpeed;
		if (WeaponY <= WeaponTop)
		{
			WeaponY = WeaponTop;
			Phase = EWeaponPhase::Ready;
		}
		break;
	case EWeaponPhase::Lowering:
		WeaponY += RaiseSpeed;
		if (WeaponY >= WeaponBottom)
		{
			WeaponY = WeaponBottom;
			if (IsDead())
			{
				break;
			}
			if (PendingWeapon != EDoomWeapon::Count)
			{
				Inv().Ready = PendingWeapon;
				PendingWeapon = EDoomWeapon::Count;
			}
			SetWeaponSprite();
			Phase = EWeaponPhase::Raising;
			if (Inv().Ready == EDoomWeapon::Chainsaw)
			{
				PlaySelfSound(TEXT("SAWUP"));
			}
		}
		break;
	case EWeaponPhase::Ready:
		if (Inv().Ready == EDoomWeapon::Chainsaw)
		{
			WeaponFrame = ((DoomWorld->GetTic() / 4) & 1) ? 'D' : 'C';
			if ((DoomWorld->GetTic() % 8) == 0) PlaySelfSound(TEXT("SAWIDL"), 0.6f);
		}
		else
		{
			WeaponFrame = D.ReadyLetter();
		}
		if (IsDead() || PendingWeapon != EDoomWeapon::Count)
		{
			Phase = EWeaponPhase::Lowering;
			break;
		}
		if (bFireHeld && CheckAmmo())
		{
			bRefire = false;
			StartFire();
		}
		else if (PendingWeapon != EDoomWeapon::Count)
		{
			Phase = EWeaponPhase::Lowering;
		}
		break;
	case EWeaponPhase::Firing:
		if (--FrameTics > 0)
		{
			break;
		}
		++FireIndex;
		// A_ReFire runs on entering the last frame (when that frame doesn't fire itself), so with the
		// trigger held its tics are skipped: plasma refires every 3 tics, the pistol every 14
		if (FireIndex == D.Fire.Num() - 1 && !D.Fire[FireIndex].bFire &&
			bFireHeld && !IsDead() && PendingWeapon == EDoomWeapon::Count && CheckAmmo())
		{
			bRefire = true;
			StartFire();
			break;
		}
		if (FireIndex >= D.Fire.Num())
		{
			// A_ReFire
			if (bFireHeld && !IsDead() && PendingWeapon == EDoomWeapon::Count && CheckAmmo())
			{
				bRefire = true;
				StartFire();
			}
			else
			{
				bRefire = false;
				Phase = EWeaponPhase::Ready;
				WeaponFrame = D.ReadyLetter();
			}
			break;
		}
		{
			const FDoomWeaponFrame& F = D.Fire[FireIndex];
			WeaponFrame = F.Letter();
			FrameTics = FMath::Max(1, F.Tics);
			if (F.bFire)
			{
				FireWeapon(FireIndex);
			}
			else if (F.Flash >= 0)
			{
				FlashFrame = 'A' + F.Flash; // A_GunFlash on a frame of its own (the BFG's charge-up)
				FlashTics = FMath::Max(4, F.Tics);
			}
			if (Inv().Ready == EDoomWeapon::SuperShotgun)
			{
				if (F.Letter() == 'D') PlaySelfSound(TEXT("DBOPN"));
				if (F.Letter() == 'F') PlaySelfSound(TEXT("DBLOAD"));
				if (F.Letter() == 'H') PlaySelfSound(TEXT("DBCLS"));
			}
			if (F.Tics == 0)
			{
				FrameTics = 1;
			}
		}
		break;
	}
}

void ADoomPlayer::StartFire()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	Phase = EWeaponPhase::Firing;
	FireIndex = 0;
	const FDoomWeaponFrame& F = D.Fire[0];
	WeaponFrame = F.Letter();
	FrameTics = FMath::Max(1, F.Tics);
	if (Inv().Ready == EDoomWeapon::BFG)
	{
		PlaySelfSound(TEXT("BFG"));
	}
	if (F.bFire)
	{
		FireWeapon(0);
	}
	else if (F.Flash >= 0)
	{
		FlashFrame = 'A' + F.Flash;
		FlashTics = FMath::Max(4, F.Tics);
	}
}

void ADoomPlayer::FireWeapon(int32 FrameIndex)
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	const FDoomWeaponFrame& F = D.Fire[FrameIndex];
	if (D.Ammo != EDoomAmmo::None)
	{
		if (Inv().Ammo[(int32)D.Ammo] < D.AmmoPerShot)
		{
			return;
		}
		Inv().Ammo[(int32)D.Ammo] -= D.AmmoPerShot;
	}
	if (F.Flash >= 0)
	{
		FlashFrame = 'A' + (Inv().Ready == EDoomWeapon::Plasma ? (PlayerRand() & 1) : F.Flash);
		FlashTics = Inv().Ready == EDoomWeapon::Pistol ? 7 : 5;
		ExtraLight = (Inv().Ready == EDoomWeapon::Rocket || Inv().Ready == EDoomWeapon::BFG || Inv().Ready == EDoomWeapon::SuperShotgun) ? 2 : 1;
		if (DoomWorld->IsModern())
		{
			MuzzleLight->SetIntensity(Inv().Ready == EDoomWeapon::Plasma ? 30000.f : 60000.f);
			MuzzleLight->SetLightColor(Inv().Ready == EDoomWeapon::Plasma ? FLinearColor(0.4f, 0.6f, 1.f) :
				Inv().Ready == EDoomWeapon::BFG ? FLinearColor(0.4f, 1.f, 0.4f) : FLinearColor(1.f, 0.75f, 0.45f));
		}
	}
	AttackDown = 4;
	DoomWorld->NoiseAlert(this, GetActorLocation());
	if (!D.Sound.IsEmpty() && Inv().Ready != EDoomWeapon::Chainsaw && Inv().Ready != EDoomWeapon::Fist && Inv().Ready != EDoomWeapon::BFG)
	{
		PlaySelfSound(D.Sound);
	}

	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector Fwd = FRotator(0, GetControlRotation().Yaw, 0).Vector();
	switch (Inv().Ready)
	{
	case EDoomWeapon::Fist: MeleeAttack(false); break;
	case EDoomWeapon::Chainsaw: MeleeAttack(true); break;
	case EDoomWeapon::Pistol:
	case EDoomWeapon::Chaingun: HitscanAttack(1, 5.6f, 0.f, 5, 3, !bRefire); break;
	case EDoomWeapon::Shotgun: HitscanAttack(7, 5.6f, 0.f, 5, 3, false); break;
	case EDoomWeapon::SuperShotgun: HitscanAttack(20, 11.2f, 7.f, 5, 3, false); break;
	case EDoomWeapon::Rocket:
	case EDoomWeapon::Plasma:
	case EDoomWeapon::BFG:
	{
		// P_SpawnPlayerMissile: straight ahead, or 5.6 degrees either side if that is where a target is;
		// launched 32 units above the feet
		AActor* AimTarget = nullptr;
		float AimAngle = DoomAngle();
		const float Slope = AimSlope(DoomAngle(), 1024.f, AimTarget, &AimAngle);
		const FVector Flat = FRotator(0, Doom::AngleToYaw(AimAngle), 0).Vector();
		const FVector Dir = (Flat + FVector(0, 0, Slope)).GetSafeNormal();
		const FVector Launch = GetActorLocation() + FVector(0, 0, 4.f * Doom::Scale);
		const TCHAR* Type = Inv().Ready == EDoomWeapon::Rocket ? TEXT("Rocket") : Inv().Ready == EDoomWeapon::Plasma ? TEXT("Plasma") : TEXT("BFGBall");
		// the BFG's sound already played when the trigger was pulled
		DoomWorld->SpawnProjectile(Type, this, Launch + Flat * 20.f * Doom::Scale, Dir, nullptr, Inv().Ready == EDoomWeapon::BFG);
		break;
	}
	default: break;
	}
}

float ADoomPlayer::AimSlope(float AngleDeg, float Range, AActor*& OutTarget, float* OutAngle) const
{
	// P_AimLineAttack: the nearest shootable thing the horizontal aim line passes through,
	// within Doom's vertical aiming window (100/160 up and down); else the view pitch.
	OutTarget = nullptr;
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector2f Origin = DoomPos();
	const UDoomSettings* Settings = GetDefault<UDoomSettings>();
	const float ViewSlope = FMath::Tan(FMath::DegreesToRadians(GetControlRotation().GetNormalized().Pitch));
	if (!Settings->bAutoAim)
	{
		return ViewSlope;
	}
	// whatever can be shot: monsters, barrels, and the other players (P_AimLineAttack takes any
	// MF_SHOOTABLE thing)
	TArray<TPair<AActor*, float>> Candidates;
	for (const TWeakObjectPtr<ADoomMonster>& M : DoomWorld->GetMonsters())
	{
		if (M.IsValid() && !M->IsDead()) Candidates.Add({ M.Get(), M->Def->Radius });
	}
	for (const TWeakObjectPtr<ADoomThing>& T : DoomWorld->GetThings())
	{
		if (T.IsValid() && !T->bDead && T->Def && (T->Def->Flags & EThingFlag::Shootable)) Candidates.Add({ T.Get(), T->Def->Radius });
	}
	for (ADoomPlayer* P : DoomWorld->GetPlayers())
	{
		if (P != this && !P->IsDead()) Candidates.Add({ P, 16.f });
	}
	for (float Offset : { 0.f, 5.6f, -5.6f })
	{
		const float A = FMath::DegreesToRadians(AngleDeg + Offset);
		const FVector2f Dir(FMath::Cos(A), FMath::Sin(A));
		float Best = Range;
		float BestSlope = 0;
		for (const TPair<AActor*, float>& C : Candidates)
		{
			const FVector2f To = Doom::ToDoom2D(C.Key->GetActorLocation()) - Origin;
			const float Along = FVector2f::DotProduct(To, Dir);
			if (Along <= 0 || Along > Best)
			{
				continue;
			}
			const float Side = FMath::Abs(To.X * Dir.Y - To.Y * Dir.X);
			if (Side > C.Value)
			{
				continue;
			}
			const float Dz = (C.Key->GetActorLocation().Z - Eye.Z) / Doom::Scale;
			const float Slope = Dz / FMath::Max(Along, 1.f);
			if (FMath::Abs(Slope) > 100.f / 160.f || !DoomWorld->CheckSight(this, C.Key))
			{
				continue;
			}
			Best = Along;
			BestSlope = Slope;
			OutTarget = C.Key;
		}
		if (OutTarget)
		{
			if (OutAngle) *OutAngle = AngleDeg + Offset;
			return BestSlope;
		}
	}
	return ViewSlope;
}

void ADoomPlayer::HitscanAttack(int32 Pellets, float SpreadYaw, float SpreadPitch, int32 DamageMul, int32 DamageDie, bool bAccurate)
{
	AActor* AimTarget = nullptr;
	const float Slope = AimSlope(DoomAngle(), 1024.f, AimTarget); // P_BulletSlope's range
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const float Range = 2048.f * Doom::Scale;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomPlayerShot), false, this);
	for (int32 i = 0; i < Pellets; ++i)
	{
		const float Yaw = GetControlRotation().Yaw + (bAccurate ? 0.f : (PlayerRand() - PlayerRand()) / 255.f * SpreadYaw);
		const float PelletSlope = Slope + (SpreadPitch > 0 ? FMath::Tan(FMath::DegreesToRadians((PlayerRand() - PlayerRand()) / 255.f * SpreadPitch)) : 0.f);
		const FVector Dir = (FRotator(0, Yaw, 0).Vector() + FVector(0, 0, PelletSlope)).GetSafeNormal();
		FHitResult Hit;
		const FVector End = Eye + Dir * Range;
		const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Eye, End, ECC_Visibility, Q);
		// a few units past the hit: the hit point lies on the wall's line, and the crossing test needs
		// the ends on opposite sides (else shooting G1/GR switches came down to rounding)
		DoomWorld->CheckShootLines(this, Doom::ToDoom2D(Eye), Doom::ToDoom2D(bHit ? Hit.ImpactPoint + Dir * 4.f * Doom::Scale : End));
		if (!bHit)
		{
			continue;
		}
		const int32 Damage = DamageMul * ((PlayerRand() % DamageDie) + 1);
		AActor* A = Hit.GetActor();
		if (A && (A->IsA<ADoomMonster>() || A->IsA<ADoomThing>() || A->IsA<ADoomPlayer>()))
		{
			UGameplayStatics::ApplyPointDamage(A, Damage, Dir, Hit, GetController(), this, nullptr);
			const ADoomMonster* M = Cast<ADoomMonster>(A);
			const bool bBlood = M && !(M->Def->Flags & EThingFlag::NoBlood);
			if (bBlood)
			{
				DoomWorld->SpawnEffect(TEXT("BLUD"), Damage > 12 ? TEXT("ABC") : Damage >= 9 ? TEXT("BC") : TEXT("C"), 8, Hit.ImpactPoint, false);
			}
			else
			{
				DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
			}
			continue;
		}
		// no puffs on the sky
		const int32 Sec = DoomWorld->SectorAt(Hit.ImpactPoint - Dir * 2.f);
		if (Sec >= 0)
		{
			const FDoomSector& S = DoomWorld->GetMap()->Sectors[Sec];
			if (FDoomGraphics::IsSkyFlat(S.CeilingPic) && Hit.ImpactPoint.Z > (S.Ceiling - 4.f) * Doom::Scale)
			{
				continue;
			}
		}
		DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
	}
}

void ADoomPlayer::MeleeAttack(bool bSaw)
{
	int32 Damage = 2 * ((PlayerRand() % 10) + 1);
	if (!bSaw && bBerserk)
	{
		Damage *= 10;
	}
	AActor* AimTarget = nullptr;
	const float Range = bSaw ? 65.f : 64.f;
	const float Slope = AimSlope(DoomAngle(), Range, AimTarget);
	const float Yaw = GetControlRotation().Yaw + (PlayerRand() - PlayerRand()) / 255.f * 5.6f; // A_Punch and A_Saw both spread
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector Dir = (FRotator(0, Yaw, 0).Vector() + FVector(0, 0, Slope)).GetSafeNormal();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomMelee), false, this);
	FHitResult Hit;
	const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Eye, Eye + Dir * Range * Doom::Scale, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(4.f * Doom::Scale), Q);
	if (!bHit)
	{
		if (bSaw) PlaySelfSound(TEXT("SAWFUL"));
		return;
	}
	AActor* A = Hit.GetActor();
	if (A && (A->IsA<ADoomMonster>() || A->IsA<ADoomThing>()))
	{
		UGameplayStatics::ApplyPointDamage(A, Damage, Dir, Hit, GetController(), this, nullptr);
		DoomWorld->SpawnEffect(TEXT("BLUD"), TEXT("BC"), 8, Hit.ImpactPoint, false);
		PlaySelfSound(bSaw ? TEXT("SAWHIT") : TEXT("PUNCH"));
		// a punch turns you to face what it hit; the saw pulls you round 4.5 degrees per hit (ANG90/20)
		if (AController* C = GetController())
		{
			FRotator R = C->GetControlRotation();
			const float Want = (A->GetActorLocation() - GetActorLocation()).Rotation().Yaw;
			R.Yaw = bSaw ? R.Yaw + FMath::Clamp(FMath::FindDeltaAngleDegrees(R.Yaw, Want), -4.5f, 4.5f) : Want;
			C->SetControlRotation(R);
		}
	}
	else
	{
		DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
		if (bSaw) PlaySelfSound(TEXT("SAWFUL"));
	}
}

// ------------------------------------------------------------------------------------------------
// Damage and pickups
// ------------------------------------------------------------------------------------------------

float ADoomPlayer::TakeDamage(float Damage, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (IsDead() || !DoomWorld || DoomWorld->GetState() != EDoomGameState::Playing)
	{
		return 0.f;
	}
	// knockback (P_DamageMobj thrust): away from the inflictor (the missile, the blast or the shooter),
	// damage * 12.5 / mass units per tic, mass 100; also with god mode and from your own rockets
	if (DamageCauser && DamageCauser != this)
	{
		const FVector From = DamageEvent.IsOfType(FRadialDamageEvent::ClassID)
			? static_cast<const FRadialDamageEvent&>(DamageEvent).Origin : DamageCauser->GetActorLocation();
		const ADoomPlayer* Shooter = Cast<ADoomPlayer>(UDoomWorld::DamageSource(DamageCauser));
		const bool bSaw = Shooter && Shooter->Inv().Ready == EDoomWeapon::Chainsaw;
		if (!bSaw)
		{
			const float Thrust = Damage * 12.5f / 100.f * Doom::TicRate * Doom::Scale;
			LaunchCharacter((GetActorLocation() - From).GetSafeNormal2D() * Thrust, false, false);
		}
	}
	// god mode and invulnerability stop everything but a telefrag
	if ((bGodMode || Invulnerability > 0) && Damage < 1000.f)
	{
		return 0.f;
	}
	int32 D = FMath::RoundToInt(Damage);
	if (DoomWorld->GetSkill() == 1)
	{
		D = FMath::Max(1, D / 2);
	}
	if (Inv().ArmorClass > 0)
	{
		int32 Saved = Inv().ArmorClass == 1 ? D / 3 : D / 2;
		if (Inv().Armor <= Saved)
		{
			Saved = Inv().Armor;
			Inv().ArmorClass = 0;
		}
		Inv().Armor -= Saved;
		D -= Saved;
	}
	Inv().Health -= D;
	DamageCount = FMath::Min(DamageCount + D, 100);

	AActor* Source = UDoomWorld::DamageSource(DamageCauser);
	if (Source && Source != this)
	{
		const FVector To = Source->GetActorLocation() - GetActorLocation();
		const float Rel = FMath::FindDeltaAngleDegrees(GetControlRotation().Yaw, To.Rotation().Yaw);
		FaceDamageDir = Rel > 45.f ? 1 : Rel < -45.f ? -1 : 0;
	}
	else
	{
		FaceDamageDir = 0;
	}

	if (Inv().Health <= 0)
	{
		Inv().Health = FMath::Max(Inv().Health, -999); // the status bar shows 0; gibs need the real value
		if (DoomWorld->IsNetGame())
		{
			// frags (P_KillMobj): another player's kill counts for them; your own rocket, or a death with
			// no source at all (slime, crushers), costs you one; monsters change nothing
			ADoomPlayerState* Killer = UDoomWorld::CreditedPlayer(EventInstigator ? (AActor*)EventInstigator : DamageCauser);
			ADoomPlayerState* Mine = GetDoomPlayerState();
			if (Killer && Killer != Mine) ++Killer->Frags;
			else if (Mine && (Killer == Mine || (!EventInstigator && !DamageCauser))) --Mine->Frags;
		}
		PlaySelfSound(Inv().Health < -50 ? TEXT("PDIEHI") : TEXT("PLDETH"));
		GetCharacterMovement()->StopMovementImmediately();
		bFireHeld = false;
		Phase = EWeaponPhase::Lowering;
		DeathTics = 0;
	}
	else
	{
		PlaySelfSound(TEXT("PLPAIN"));
	}
	return Damage;
}

FDoomInventory& ADoomPlayer::Inv()
{
	ADoomPlayerState* PS = GetDoomPlayerState();
	return PS ? PS->Inv : LocalInv;
}

const FDoomInventory& ADoomPlayer::Inv() const
{
	const ADoomPlayerState* PS = GetDoomPlayerState();
	return PS ? PS->Inv : LocalInv;
}

ADoomPlayerState* ADoomPlayer::GetDoomPlayerState() const
{
	return GetPlayerState<ADoomPlayerState>();
}

void ADoomPlayer::DoomSerialize(FArchive& Ar)
{
	FVector Location = GetActorLocation();
	FRotator ViewRotation = GetControlRotation();
	FVector Velocity = GetCharacterMovement()->Velocity;
	Ar << Location << ViewRotation << Velocity;
	// the inventory and this player's level credit, from the player state
	if (ADoomPlayerState* PS = GetDoomPlayerState())
	{
		PS->DoomSerialize(Ar);
	}
	else
	{
		ADoomPlayerState* Defaults = GetMutableDefault<ADoomPlayerState>();
		LocalInv.Serialize(Ar);
		int32 Credit[4] = { Defaults->LevelKills, Defaults->LevelItems, Defaults->LevelSecrets, Defaults->Frags };
		for (int32& C : Credit) Ar << C;
	}
	Ar << Invulnerability << Invisibility << RadSuit << LightAmp << bBerserk;
	if (Ar.IsLoading())
	{
		SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* C = GetController())
		{
			C->SetControlRotation(ViewRotation);
		}
		GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		GetCharacterMovement()->Velocity = Velocity;
		PendingWeapon = EDoomWeapon::Count;
		Phase = EWeaponPhase::Ready;
		WeaponY = WeaponTop;
		FlashFrame = 0;
		bFireHeld = false;
		DamageCount = BonusCount = ExtraLight = 0;
		SetWeaponSprite();
		LastDoomPos = DoomPos();
	}
}

bool ADoomPlayer::TryPickup(const FDoomThingDef& Def, bool bDropped)
{
	const bool bDoubleAmmo = DoomWorld && (DoomWorld->GetSkill() == 1 || DoomWorld->GetSkill() == 5);
	auto GiveAmmo = [&](EDoomAmmo A, int32 Amount) -> bool
	{
		const int32 Max = DoomInfo::MaxAmmo(A, Inv().bBackpack);
		int32& Cur = Inv().Ammo[(int32)A];
		if (Cur >= Max)
		{
			return false;
		}
		if (bDoubleAmmo) Amount *= 2;
		const int32 Old = Cur;
		Cur = FMath::Min(Cur + Amount, Max);
		// P_GiveAmmo: when you had none of it, bullets take you off the fist (chaingun, else pistol),
		// shells and cells off the fist or pistol (shotgun, plasma), rockets off the fist (launcher)
		if (Old == 0)
		{
			const EDoomWeapon Ready = Inv().Ready;
			const bool bFist = Ready == EDoomWeapon::Fist;
			const bool bWeak = bFist || Ready == EDoomWeapon::Pistol;
			EDoomWeapon Want = EDoomWeapon::Count;
			switch (A)
			{
			case EDoomAmmo::Bullets: if (bFist) Want = Inv().Weapons[(int32)EDoomWeapon::Chaingun] ? EDoomWeapon::Chaingun : EDoomWeapon::Pistol; break;
			case EDoomAmmo::Shells: if (bWeak && Inv().Weapons[(int32)EDoomWeapon::Shotgun]) Want = EDoomWeapon::Shotgun; break;
			case EDoomAmmo::Cells: if (bWeak && Inv().Weapons[(int32)EDoomWeapon::Plasma]) Want = EDoomWeapon::Plasma; break;
			case EDoomAmmo::Rockets: if (bFist && Inv().Weapons[(int32)EDoomWeapon::Rocket]) Want = EDoomWeapon::Rocket; break;
			default: break;
			}
			if (Want != EDoomWeapon::Count && Want != Ready)
			{
				PendingWeapon = Want;
			}
		}
		return true;
	};
	auto GiveWeapon = [&](EDoomWeapon W, EDoomAmmo A, int32 Ammo) -> bool
	{
		const bool bHad = Inv().Weapons[(int32)W];
		bool bGot = false;
		if (A != EDoomAmmo::None)
		{
			bGot = GiveAmmo(A, bDropped ? Ammo / 2 : Ammo);
		}
		if (!bHad)
		{
			Inv().Weapons[(int32)W] = true;
			PendingWeapon = W;
			FaceEvilGrin = 2 * Doom::TicRate;
			bGot = true;
		}
		return bGot;
	};

	const FDoomRules& R = DoomInfo::Rules();
	const int32* Clip = R.ClipAmmo;
	bool bTaken = true;
	const TCHAR* Sound = TEXT("ITEMUP");
	switch (Def.Pickup)
	{
	case EPickup::Health:
	{
		const int32 Amount = Def.DoomEdNum == 2011 ? 10 : 25;
		if (Inv().Health >= 100) return false;
		Inv().Health = FMath::Min(Inv().Health + Amount, 100);
		break;
	}
	case EPickup::HealthBonus: Inv().Health = FMath::Min(Inv().Health + 1, R.MaxHealth); break;
	case EPickup::ArmorBonus: Inv().Armor = FMath::Min(Inv().Armor + 1, R.MaxArmor); if (!Inv().ArmorClass) Inv().ArmorClass = 1; break;
	case EPickup::Soulsphere: Inv().Health = FMath::Min(Inv().Health + R.SoulsphereHealth, R.MaxSoulsphere); Sound = TEXT("GETPOW"); break;
	case EPickup::Megasphere: Inv().Health = R.MegasphereHealth; Inv().Armor = 100 * R.BlueArmorClass; Inv().ArmorClass = R.BlueArmorClass; Sound = TEXT("GETPOW"); break;
	case EPickup::GreenArmor: if (Inv().Armor >= 100 * R.GreenArmorClass) return false; Inv().Armor = 100 * R.GreenArmorClass; Inv().ArmorClass = R.GreenArmorClass; break;
	case EPickup::BlueArmor: if (Inv().Armor >= 100 * R.BlueArmorClass) return false; Inv().Armor = 100 * R.BlueArmorClass; Inv().ArmorClass = R.BlueArmorClass; break;
	case EPickup::Invulnerability: Invulnerability = 30 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::Berserk:
		bBerserk = true;
		BerserkTics = 1;
		if (Inv().Health < 100) Inv().Health = 100;
		if (Inv().Ready != EDoomWeapon::Fist) PendingWeapon = EDoomWeapon::Fist;
		Sound = TEXT("GETPOW");
		break;
	case EPickup::Invisibility: Invisibility = 60 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::RadSuit: RadSuit = 60 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::Map: if (DoomWorld->bFullMap) return false; DoomWorld->bFullMap = true; Sound = TEXT("GETPOW"); break;
	case EPickup::LightAmp: LightAmp = 120 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::BlueCard: Inv().Keys[0] = true; break;
	case EPickup::YellowCard: Inv().Keys[1] = true; break;
	case EPickup::RedCard: Inv().Keys[2] = true; break;
	case EPickup::BlueSkull: Inv().Keys[3] = true; break;
	case EPickup::YellowSkull: Inv().Keys[4] = true; break;
	case EPickup::RedSkull: Inv().Keys[5] = true; break;
	case EPickup::Clip: bTaken = GiveAmmo(EDoomAmmo::Bullets, bDropped ? FMath::Max(1, Clip[0] / 2) : Clip[0]); break;
	case EPickup::ClipBox: bTaken = GiveAmmo(EDoomAmmo::Bullets, Clip[0] * 5); break;
	case EPickup::Shells: bTaken = GiveAmmo(EDoomAmmo::Shells, Clip[1]); break;
	case EPickup::ShellBox: bTaken = GiveAmmo(EDoomAmmo::Shells, Clip[1] * 5); break;
	case EPickup::Rocket: bTaken = GiveAmmo(EDoomAmmo::Rockets, Clip[2]); break;
	case EPickup::RocketBox: bTaken = GiveAmmo(EDoomAmmo::Rockets, Clip[2] * 5); break;
	case EPickup::Cell: bTaken = GiveAmmo(EDoomAmmo::Cells, Clip[3]); break;
	case EPickup::CellPack: bTaken = GiveAmmo(EDoomAmmo::Cells, Clip[3] * 5); break;
	case EPickup::Backpack:
		Inv().bBackpack = true;
		GiveAmmo(EDoomAmmo::Bullets, Clip[0]); GiveAmmo(EDoomAmmo::Shells, Clip[1]); GiveAmmo(EDoomAmmo::Rockets, Clip[2]); GiveAmmo(EDoomAmmo::Cells, Clip[3]);
		break;
	case EPickup::Chainsaw: bTaken = GiveWeapon(EDoomWeapon::Chainsaw, EDoomAmmo::None, 0); Sound = TEXT("WPNUP"); break;
	case EPickup::Shotgun: bTaken = GiveWeapon(EDoomWeapon::Shotgun, EDoomAmmo::Shells, Clip[1] * 2); Sound = TEXT("WPNUP"); break;
	case EPickup::SuperShotgun: bTaken = GiveWeapon(EDoomWeapon::SuperShotgun, EDoomAmmo::Shells, Clip[1] * 2); Sound = TEXT("WPNUP"); break;
	case EPickup::Chaingun: bTaken = GiveWeapon(EDoomWeapon::Chaingun, EDoomAmmo::Bullets, Clip[0] * 2); Sound = TEXT("WPNUP"); break;
	case EPickup::RocketLauncher: bTaken = GiveWeapon(EDoomWeapon::Rocket, EDoomAmmo::Rockets, Clip[2] * 2); Sound = TEXT("WPNUP"); break;
	case EPickup::Plasma: bTaken = GiveWeapon(EDoomWeapon::Plasma, EDoomAmmo::Cells, Clip[3] * 2); Sound = TEXT("WPNUP"); break;
	case EPickup::BFG: bTaken = GiveWeapon(EDoomWeapon::BFG, EDoomAmmo::Cells, Clip[3] * 2); Sound = TEXT("WPNUP"); break;
	default: return false;
	}
	if (!bTaken)
	{
		return false;
	}
	BonusCount = FMath::Min(BonusCount + 6, 24);
	Message(Def.Pickup == EPickup::Health
		? DoomInfo::Text(Def.DoomEdNum == 2011 ? TEXT("GOTSTIM") : TEXT("GOTMEDIKIT"), DoomInfo::PickupMessage(Def.Pickup))
		: DoomInfo::PickupMessage(Def.Pickup));
	PlaySelfSound(Sound);
	return true;
}
