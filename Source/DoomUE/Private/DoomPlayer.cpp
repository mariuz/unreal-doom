#include "DoomPlayer.h"
#include "DoomPlayerPrivate.h"
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
	View.Invulnerability = View.Invisibility = View.RadSuit = View.LightAmp = 0;
	View.bBerserk = false;
	View.DamageCount = View.BonusCount = View.ExtraLight = 0;
	View.DeathTics = 0;
	ReactionTime = 0;
	PendingWeapon = EDoomWeapon::Count;
	Phase = EWeaponPhase::Raising;
	View.WeaponY = DoomPlayerDetail::WeaponBottom;
	View.FlashFrame = 0;
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
	return DoomWorld && DoomWorld->IsInputLocked();
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
		if (View.DeathTics > Doom::TicRate)
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
	View.CameraZ = Camera->GetRelativeLocation().Z;
}

void ADoomPlayer::OnRep_View(const FDoomPlayerView& OldView)
{
	if (View.ExtraLight > OldView.ExtraLight && MuzzleLight && DoomWorld && DoomWorld->IsModern())
	{
		MuzzleLight->SetIntensity(60000.f);
	}
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
		Seq = View.FlashFrame ? TEXT("F") : TEXT("E");
	}
	else if (View.DamageCount > 0 && View.FaceDamageDir != 0)
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
		DoomWorld->SetAutomapOpen(!DoomWorld->IsAutomapOpen());
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
	ensureMsgf(HasAuthority(), TEXT("the server runs the marine's tics; the owner gets View"));
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
		View.BobX = FMath::Cos(BobPhase) * Bob;
		View.BobY = FMath::Abs(FMath::Sin(BobPhase)) * Bob;
	}

	TicWeapon();
	TicPowerups();
	if (View.DamageCount > 0) --View.DamageCount;
	if (View.BonusCount > 0) --View.BonusCount;
	if (View.FaceEvilGrin > 0) --View.FaceEvilGrin;
	if (View.AttackDown > 0) --View.AttackDown;
	AnimateBody();
	if (DoomWorld->IsNetServer())
	{
		PublishView();
	}
}

void ADoomPlayer::TicPowerups()
{
	if (View.bBerserk) ++View.BerserkTics;
	if (View.Invulnerability > 0) --View.Invulnerability;
	if (View.Invisibility > 0) --View.Invisibility;
	if (View.RadSuit > 0) --View.RadSuit;
	if (View.LightAmp > 0) --View.LightAmp;
}

void ADoomPlayer::TicDeath()
{
	++View.DeathTics;
	// the view sinks to the floor
	const FVector Rel = Camera->GetRelativeLocation();
	const float Target = (6.f - 28.f) * Doom::Scale;
	if (Rel.Z > Target)
	{
		Camera->SetRelativeLocation(FVector(Rel.X, Rel.Y, FMath::Max(Target, Rel.Z - 1.f * Doom::Scale)));
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
	if ((View.bGodMode || View.Invulnerability > 0) && Damage < 1000.f)
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
	View.DamageCount = FMath::Min(View.DamageCount + D, 100);

	AActor* Source = UDoomWorld::DamageSource(DamageCauser);
	if (Source && Source != this)
	{
		const FVector To = Source->GetActorLocation() - GetActorLocation();
		const float Rel = FMath::FindDeltaAngleDegrees(GetControlRotation().Yaw, To.Rotation().Yaw);
		View.FaceDamageDir = Rel > 45.f ? 1 : Rel < -45.f ? -1 : 0;
	}
	else
	{
		View.FaceDamageDir = 0;
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
		View.DeathTics = 0;
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
	Ar << View.Invulnerability << View.Invisibility << View.RadSuit << View.LightAmp << View.bBerserk;
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
		View.WeaponY = DoomPlayerDetail::WeaponTop;
		View.FlashFrame = 0;
		bFireHeld = false;
		View.DamageCount = View.BonusCount = View.ExtraLight = 0;
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
			View.FaceEvilGrin = 2 * Doom::TicRate;
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
	case EPickup::Invulnerability: View.Invulnerability = 30 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::Berserk:
		View.bBerserk = true;
		View.BerserkTics = 1;
		if (Inv().Health < 100) Inv().Health = 100;
		if (Inv().Ready != EDoomWeapon::Fist) PendingWeapon = EDoomWeapon::Fist;
		Sound = TEXT("GETPOW");
		break;
	case EPickup::Invisibility: View.Invisibility = 60 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::RadSuit: View.RadSuit = 60 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
	case EPickup::Map: if (DoomWorld->HasFullMap()) return false; DoomWorld->SetFullMap(true); Sound = TEXT("GETPOW"); break;
	case EPickup::LightAmp: View.LightAmp = 120 * Doom::TicRate; Sound = TEXT("GETPOW"); break;
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
	View.BonusCount = FMath::Min(View.BonusCount + 6, 24);
	Message(Def.Pickup == EPickup::Health
		? DoomInfo::Text(Def.DoomEdNum == 2011 ? TEXT("GOTSTIM") : TEXT("GOTMEDIKIT"), DoomInfo::PickupMessage(Def.Pickup))
		: DoomInfo::PickupMessage(Def.Pickup));
	PlaySelfSound(Sound);
	return true;
}
