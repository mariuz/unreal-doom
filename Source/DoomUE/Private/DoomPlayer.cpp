#include "DoomPlayer.h"
#include "DoomWorld.h"
#include "DoomMonster.h"
#include "DoomThing.h"
#include "DoomPlayerController.h"
#include "DoomSettings.h"
#include "DoomLevel.h"
#include "DoomGraphics.h"
#include "DoomUE.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EnhancedInputComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DamageEvents.h"

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

ADoomPlayer::ADoomPlayer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
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

	bUseControllerRotationYaw = true;
	bUseControllerRotationPitch = false;
	if (GetMesh())
	{
		GetMesh()->SetVisibility(false);
	}

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxStepHeight = 24.f * S;
	Move->GravityScale = 3.75f;           // Doom: 1 unit/tic^2
	Move->MaxWalkSpeed = 16.6f * Doom::TicRate * S;
	Move->MaxAcceleration = 4000.f * S;
	Move->BrakingDecelerationWalking = 2600.f * S;
	Move->GroundFriction = 7.f;
	Move->AirControl = 0.05f;
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

void ADoomPlayer::InitForLevel(UDoomWorld* InWorld, bool bPistolStart)
{
	DoomWorld = InWorld;
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
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), In.X * Scale);
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

void ADoomPlayer::UsePressed(const FInputActionValue& V)
{
	if (!DoomWorld || InputLocked())
	{
		return;
	}
	if (IsDead())
	{
		if (DeathTics > Doom::TicRate)
		{
			DoomWorld->LoadMap(DoomWorld->GetMapName(), true);
		}
		return;
	}
	DoomWorld->UseLines(this, GetActorLocation(), DoomAngle());
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
	if (MuzzleLight && MuzzleLight->Intensity > 0)
	{
		MuzzleLight->SetIntensity(FMath::Max(0.f, MuzzleLight->Intensity - DeltaSeconds * 400000.f));
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
	LastDoomPos = Pos;
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
	BobX = FMath::Cos(BobPhase) * Bob;
	BobY = FMath::Abs(FMath::Sin(BobPhase)) * Bob;

	TicWeapon();
	TicPowerups();
	if (DamageCount > 0) --DamageCount;
	if (BonusCount > 0) --BonusCount;
	if (FaceEvilGrin > 0) --FaceEvilGrin;
	if (AttackDown > 0) --AttackDown;
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
	const float Target = (8.f - 28.f) * Doom::Scale;
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
	WeaponFrame = D.ReadyFrame;
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
				DoomWorld->PlaySound(TEXT("SAWUP"), nullptr);
			}
		}
		break;
	case EWeaponPhase::Ready:
		if (Inv().Ready == EDoomWeapon::Chainsaw)
		{
			WeaponFrame = ((DoomWorld->GetTic() / 4) & 1) ? 'D' : 'C';
			if ((DoomWorld->GetTic() % 16) == 0) DoomWorld->PlaySound(TEXT("SAWIDL"), nullptr, 0.6f);
		}
		else
		{
			WeaponFrame = D.ReadyFrame;
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
				WeaponFrame = D.ReadyFrame;
			}
			break;
		}
		{
			const FDoomWeaponFrame& F = D.Fire[FireIndex];
			WeaponFrame = F.Frame;
			FrameTics = FMath::Max(1, F.Tics);
			if (F.bFire)
			{
				FireWeapon(FireIndex);
			}
			if (Inv().Ready == EDoomWeapon::SuperShotgun)
			{
				if (F.Frame == 'D') DoomWorld->PlaySound(TEXT("DBOPN"), nullptr);
				if (F.Frame == 'F') DoomWorld->PlaySound(TEXT("DBLOAD"), nullptr);
				if (F.Frame == 'H') DoomWorld->PlaySound(TEXT("DBCLS"), nullptr);
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
	WeaponFrame = F.Frame;
	FrameTics = FMath::Max(1, F.Tics);
	if (Inv().Ready == EDoomWeapon::BFG)
	{
		DoomWorld->PlaySound(TEXT("BFG"), nullptr);
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
	if (*D.Sound && Inv().Ready != EDoomWeapon::Chainsaw && Inv().Ready != EDoomWeapon::Fist && Inv().Ready != EDoomWeapon::BFG)
	{
		DoomWorld->PlaySound(D.Sound, nullptr);
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
		AActor* AimTarget = nullptr;
		const float Slope = AimSlope(DoomAngle(), 1024.f, AimTarget);
		const FVector Dir = (Fwd + FVector(0, 0, Slope)).GetSafeNormal();
		const TCHAR* Type = Inv().Ready == EDoomWeapon::Rocket ? TEXT("Rocket") : Inv().Ready == EDoomWeapon::Plasma ? TEXT("Plasma") : TEXT("BFGBall");
		DoomWorld->SpawnProjectile(Type, this, Eye + Fwd * 20.f * Doom::Scale, Dir);
		break;
	}
	default: break;
	}
}

float ADoomPlayer::AimSlope(float AngleDeg, float Range, AActor*& OutTarget) const
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
	for (float Offset : { 0.f, 5.6f, -5.6f })
	{
		const float A = FMath::DegreesToRadians(AngleDeg + Offset);
		const FVector2f Dir(FMath::Cos(A), FMath::Sin(A));
		float Best = Range;
		float BestSlope = 0;
		for (const TWeakObjectPtr<ADoomMonster>& M : DoomWorld->GetMonsters())
		{
			if (!M.IsValid() || M->IsDead())
			{
				continue;
			}
			const FVector2f To = M->DoomPos() - Origin;
			const float Along = FVector2f::DotProduct(To, Dir);
			if (Along <= 0 || Along > Best)
			{
				continue;
			}
			const float Side = FMath::Abs(To.X * Dir.Y - To.Y * Dir.X);
			if (Side > M->Def->Radius)
			{
				continue;
			}
			const float Dz = (M->GetActorLocation().Z - Eye.Z) / Doom::Scale;
			const float Slope = Dz / FMath::Max(Along, 1.f);
			if (FMath::Abs(Slope) > 100.f / 160.f || !DoomWorld->CheckSight(this, M.Get()))
			{
				continue;
			}
			Best = Along;
			BestSlope = Slope;
			OutTarget = M.Get();
		}
		if (OutTarget)
		{
			return BestSlope;
		}
	}
	return ViewSlope;
}

void ADoomPlayer::HitscanAttack(int32 Pellets, float SpreadYaw, float SpreadPitch, int32 DamageMul, int32 DamageDie, bool bAccurate)
{
	AActor* AimTarget = nullptr;
	const float Slope = AimSlope(DoomAngle(), 2048.f, AimTarget);
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
		DoomWorld->CheckShootLines(this, Doom::ToDoom2D(Eye), Doom::ToDoom2D(bHit ? Hit.ImpactPoint : End));
		if (!bHit)
		{
			continue;
		}
		const int32 Damage = DamageMul * ((PlayerRand() % DamageDie) + 1);
		AActor* A = Hit.GetActor();
		if (A && (A->IsA<ADoomMonster>() || A->IsA<ADoomThing>()))
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
	const float Yaw = GetControlRotation().Yaw + (bSaw ? (PlayerRand() - PlayerRand()) / 255.f * 5.6f : 0.f);
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector Dir = (FRotator(0, Yaw, 0).Vector() + FVector(0, 0, Slope)).GetSafeNormal();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomMelee), false, this);
	FHitResult Hit;
	const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Eye, Eye + Dir * Range * Doom::Scale, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(4.f * Doom::Scale), Q);
	if (!bHit)
	{
		if (bSaw) DoomWorld->PlaySound(TEXT("SAWFUL"), nullptr);
		return;
	}
	AActor* A = Hit.GetActor();
	if (A && (A->IsA<ADoomMonster>() || A->IsA<ADoomThing>()))
	{
		UGameplayStatics::ApplyPointDamage(A, Damage, Dir, Hit, GetController(), this, nullptr);
		DoomWorld->SpawnEffect(TEXT("BLUD"), TEXT("BC"), 8, Hit.ImpactPoint, false);
		DoomWorld->PlaySound(bSaw ? TEXT("SAWHIT") : TEXT("PUNCH"), nullptr);
		// punches and the saw turn you towards what they hit
		if (AController* C = GetController())
		{
			FRotator R = C->GetControlRotation();
			R.Yaw = (A->GetActorLocation() - GetActorLocation()).Rotation().Yaw;
			C->SetControlRotation(R);
		}
	}
	else
	{
		DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
		if (bSaw) DoomWorld->PlaySound(TEXT("SAWFUL"), nullptr);
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
	if (bGodMode || Invulnerability > 0)
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

	AActor* Source = DamageCauser ? (DamageCauser->GetOwner() ? DamageCauser->GetOwner() : DamageCauser) : nullptr;
	if (Source && Source != this)
	{
		const FVector To = Source->GetActorLocation() - GetActorLocation();
		const float Rel = FMath::FindDeltaAngleDegrees(GetControlRotation().Yaw, To.Rotation().Yaw);
		FaceDamageDir = Rel > 30.f ? 1 : Rel < -30.f ? -1 : 0;
		// knockback
		const float Thrust = FMath::Min(Damage * 12.5f, 400.f) * Doom::Scale;
		LaunchCharacter(-To.GetSafeNormal2D() * Thrust, false, false);
	}
	else
	{
		FaceDamageDir = 0;
	}

	if (Inv().Health <= 0)
	{
		Inv().Health = FMath::Max(Inv().Health, -99);
		DoomWorld->PlaySound(Inv().Health < -50 ? TEXT("PDIEHI") : TEXT("PLDETH"), nullptr);
		GetCharacterMovement()->StopMovementImmediately();
		bFireHeld = false;
		Phase = EWeaponPhase::Lowering;
		DeathTics = 0;
	}
	else
	{
		DoomWorld->PlaySound(TEXT("PLPAIN"), nullptr);
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
	FRotator View = GetControlRotation();
	FVector Velocity = GetCharacterMovement()->Velocity;
	Ar << Location << View << Velocity;
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
			C->SetControlRotation(View);
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
		// switch away from fist/pistol when ammo for something better arrives
		if (Old == 0 && (Inv().Ready == EDoomWeapon::Fist || Inv().Ready == EDoomWeapon::Pistol))
		{
			static const EDoomWeapon ByAmmo[4][2] = {
				{ EDoomWeapon::Chaingun, EDoomWeapon::Pistol }, { EDoomWeapon::SuperShotgun, EDoomWeapon::Shotgun },
				{ EDoomWeapon::Rocket, EDoomWeapon::Count }, { EDoomWeapon::Plasma, EDoomWeapon::Count } };
			for (EDoomWeapon W : ByAmmo[(int32)A])
			{
				if (W != EDoomWeapon::Count && Inv().Weapons[(int32)W] && W != Inv().Ready && !(A == EDoomAmmo::Rockets))
				{
					PendingWeapon = W;
					break;
				}
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
	case EPickup::Map: DoomWorld->bFullMap = true; Sound = TEXT("GETPOW"); break;
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
	DoomWorld->ShowMessage(Def.Pickup == EPickup::Health
		? DoomInfo::Text(Def.DoomEdNum == 2011 ? TEXT("GOTSTIM") : TEXT("GOTMEDIKIT"), DoomInfo::PickupMessage(Def.Pickup))
		: DoomInfo::PickupMessage(Def.Pickup));
	DoomWorld->PlaySound(Sound, nullptr);
	return true;
}
