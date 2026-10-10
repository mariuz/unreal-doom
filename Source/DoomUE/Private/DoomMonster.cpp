#include "DoomMonster.h"
#include "DoomWorld.h"
#include "DoomPlayer.h"
#include "DoomThing.h"
#include "DoomLevel.h"
#include "DoomSpriteComponent.h"
#include "DoomUE.h"
#include "AIController.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DamageEvents.h"

namespace
{
	constexpr int32 DirNone = 8;
	const int32 Opposite[9] = { 4, 5, 6, 7, 0, 1, 2, 3, DirNone };

	FVector2f DirVector(int32 Dir)
	{
		const float A = FMath::DegreesToRadians(Dir * 45.f);
		return FVector2f(FMath::Cos(A), FMath::Sin(A));
	}

	int32 MonsterRand() { return DoomInfo::Random(); }

	bool Is(const FDoomThingDef* D, const TCHAR* Name) { return D && FCString::Strcmp(D->Name, Name) == 0; }
}

ADoomMonster::ADoomMonster(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	AIControllerClass = AAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::Spawned;
	bUseControllerRotationYaw = false;

	UCapsuleComponent* Cap = GetCapsuleComponent();
	Cap->SetCollisionObjectType(ECC_DoomMonster);
	Cap->SetCollisionResponseToAllChannels(ECR_Block);
	Cap->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Cap->SetCanEverAffectNavigation(false);

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->bOrientRotationToMovement = false;
	Move->bCanWalkOffLedges = false;
	Move->MaxStepHeight = 24.f * Doom::Scale;
	Move->GravityScale = 3.75f;
	Move->MaxAcceleration = 20000.f;
	Move->BrakingDecelerationWalking = 20000.f;
	Move->GroundFriction = 12.f;
	Move->bRunPhysicsWithNoController = true;
	Move->SetWalkableFloorAngle(50.f);
	Move->bUseRVOAvoidance = false;
	Move->MaxFlySpeed = 600.f;
	Move->BrakingDecelerationFlying = 8000.f;

	if (GetMesh())
	{
		GetMesh()->SetVisibility(false);
		GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	Sprite = CreateDefaultSubobject<UDoomSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(Cap);
}

void ADoomMonster::InitMonster(UDoomWorld* InWorld, const FDoomThingDef* InDef, float InDoomAngle, bool bInAmbush)
{
	DoomWorld = InWorld;
	Def = InDef;
	DoomAngle = InDoomAngle;
	bAmbush = bInAmbush;
	Health = Def->Health;
	const float S = Doom::Scale;
	GetCapsuleComponent()->SetCapsuleSize(FMath::Min(Def->Radius, Def->Height * 0.5f) * S, Def->Height * 0.5f * S);
	// Doom's radius is a box half-width; big monsters keep a full-width capsule for walls
	if (Def->Radius > Def->Height * 0.5f)
	{
		GetCapsuleComponent()->SetCapsuleSize(Def->Radius * S * 0.9f, Def->Radius * S * 0.9f);
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = Def->Speed / FMath::Max(1, Def->RunTics) * Doom::TicRate * S;
	Move->MaxFlySpeed = Move->MaxWalkSpeed;
	if (Def->Flags & EThingFlag::Float)
	{
		Move->SetMovementMode(MOVE_Flying);
	}
	if (Def->Flags & EThingFlag::Hanging)
	{
		Move->SetMovementMode(MOVE_None);
	}

	Sprite->Init(DoomWorld, Def->Sprite);
	Sprite->DoomAngle = DoomAngle;
	Sprite->SetFuzz((Def->Flags & EThingFlag::Shadow) != 0);
	Sprite->PlaySequence(Def->Idle, Def->IdleTics, true, (Def->Flags & EThingFlag::FullBright) != 0);
	Sprite->SetSector(DoomWorld->SectorAt(GetActorLocation()));
	LastPos = StepStart = DoomPos();
	ReactionTime = DoomWorld->GetSkill() >= 5 ? 0 : 8;

	if (DoomWorld->IsModern() && Def->LightColor.A > 0)
	{
		Light = NewObject<UPointLightComponent>(this, TEXT("Light"));
		Light->SetupAttachment(GetCapsuleComponent());
		Light->SetLightColor(Def->LightColor);
		Light->SetIntensity(2500.f);
		Light->SetAttenuationRadius(160.f * S);
		Light->SetCastShadows(false);
		Light->RegisterComponent();
	}
}

FVector2f ADoomMonster::DoomPos() const { return Doom::ToDoom2D(GetActorLocation()); }
float ADoomMonster::DoomZ() const { return Doom::ToDoomZ(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); }
FVector ADoomMonster::EyeLocation() const { return GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale); }

void ADoomMonster::StartState(EMonsterState S)
{
	State = S;
	FireFrameSeen = -1;
	const bool bBright = (Def->Flags & EThingFlag::FullBright) != 0;
	switch (S)
	{
	case EMonsterState::Idle: Sprite->PlaySequence(Def->Idle, Def->IdleTics, true, bBright); break;
	case EMonsterState::Chase: Sprite->PlaySequence(*Def->Run ? Def->Run : Def->Idle, Def->RunTics, true, bBright); StepStart = DoomPos(); break;
	case EMonsterState::Melee: Sprite->PlaySequence(Def->Melee, 6, false, bBright); FaceTarget(); break;
	case EMonsterState::Missile:
		Sprite->PlaySequence(Def->Attack, Is(Def, TEXT("ArchVile")) ? 5 : Is(Def, TEXT("Chaingunner")) || Is(Def, TEXT("SpiderMastermind")) ? 4 : 8, false, true);
		FaceTarget();
		if (Is(Def, TEXT("ArchVile")))
		{
			const FVector L = GetActorLocation();
			DoomWorld->PlaySound(TEXT("VILATK"), &L);
		}
		break;
	case EMonsterState::Pain: Sprite->PlaySequence(Def->Pain, 4, false, bBright); break;
	case EMonsterState::Raising: Sprite->PlaySequence(TEXT("[\\]"), 10, false, true); break;
	case EMonsterState::Charging: Sprite->PlaySequence(TEXT("CD"), 4, true, true); break;
	default: break;
	}
}

void ADoomMonster::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Def || IsDead())
	{
		return;
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (State == EMonsterState::Charging)
	{
		Move->Velocity = ChargeVelocity;
		return;
	}
	if (State == EMonsterState::Chase && MoveDir != DirNone)
	{
		const FVector2f D = DirVector(MoveDir);
		FVector Input(D.X, -D.Y, 0);
		if ((Def->Flags & EThingFlag::Float) && Target.IsValid())
		{
			// rise and sink towards the target like P_ZMovement's float logic
			const float Dz = Target->GetActorLocation().Z - GetActorLocation().Z;
			const float Dist = FVector::Dist2D(Target->GetActorLocation(), GetActorLocation());
			if (FMath::Abs(Dz) > 8.f * Doom::Scale && Dist < FMath::Abs(Dz) * 3.f)
			{
				Input.Z = FMath::Sign(Dz) * 0.5f;
			}
		}
		AddMovementInput(Input);
	}
}

void ADoomMonster::DoomTic()
{
	if (!Def)
	{
		return;
	}
	const bool bFrameChanged = Sprite->AnimateTic();
	const int32 Sec = DoomWorld->SectorAt(GetActorLocation());
	Sprite->SetSector(Sec);
	Sprite->DoomAngle = DoomAngle;
	SetActorRotation(FRotator(0, Doom::AngleToYaw(DoomAngle), 0));

	// walk-over lines (teleporters, lifts) for monsters
	const FVector2f Pos = DoomPos();
	if (!IsDead())
	{
		DoomWorld->CheckCrossings(this, LastPos, Pos, true);
	}
	LastPos = Pos;

	if (ReactionTime > 0)
	{
		--ReactionTime;
	}

	switch (State)
	{
	case EMonsterState::Idle:
		if ((DoomWorld->GetTic() & 3) == 0)
		{
			Look();
		}
		break;
	case EMonsterState::Chase:
		if (bFrameChanged || Sprite->CurrentSequence().Len() == 1 && (DoomWorld->GetTic() % FMath::Max(1, Def->RunTics)) == 0)
		{
			Chase();
		}
		break;
	case EMonsterState::Melee:
	case EMonsterState::Missile:
	{
		const FString& Seq = Sprite->CurrentSequence();
		const TCHAR FireLetter = Seq.Len() ? Seq[Seq.Len() - 1] : 'A';
		if (Sprite->CurrentFrame() == FireLetter && FireFrameSeen != Sprite->FrameIndex())
		{
			FireFrameSeen = Sprite->FrameIndex();
			FaceTarget();
			DoAttack(State == EMonsterState::Melee);
			if (State == EMonsterState::Charging || IsDead())
			{
				break;
			}
		}
		if (Sprite->IsSequenceDone())
		{
			StartState(EMonsterState::Chase);
		}
		break;
	}
	case EMonsterState::Pain:
		if (Sprite->IsSequenceDone())
		{
			StartState(EMonsterState::Chase);
		}
		break;
	case EMonsterState::Raising:
		if (Sprite->IsSequenceDone())
		{
			StartState(EMonsterState::Chase);
		}
		break;
	case EMonsterState::Charging:
		if (--ChargeTics <= 0)
		{
			ChargeVelocity = FVector::ZeroVector;
			GetCharacterMovement()->StopMovementImmediately();
			StartState(EMonsterState::Chase);
		}
		break;
	case EMonsterState::Dying:
		if (Sprite->IsSequenceDone())
		{
			State = EMonsterState::Dead;
		}
		break;
	case EMonsterState::Dead:
		break;
	}
}

void ADoomMonster::WakeUp(AActor* NewTarget, bool bFromSound)
{
	if (IsDead() || State != EMonsterState::Idle || !NewTarget)
	{
		return;
	}
	if (bAmbush && bFromSound && !DoomWorld->CheckSight(this, NewTarget))
	{
		return;
	}
	Target = NewTarget;
	if (*Def->SeeSound)
	{
		const bool bBoss = Is(Def, TEXT("Cyberdemon")) || Is(Def, TEXT("SpiderMastermind"));
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->SeeSound, bBoss ? nullptr : &L);
	}
	StartState(EMonsterState::Chase);
}

void ADoomMonster::Look()
{
	ADoomPlayer* P = DoomWorld->GetPlayer();
	if (!P || P->IsDead())
	{
		return;
	}
	// heard a noise in this sector?
	const int32 Sec = DoomWorld->SectorAt(GetActorLocation());
	if (Sec >= 0)
	{
		const FDoomSector& S = DoomWorld->GetMap()->Sectors[Sec];
		if (S.SoundTarget.IsValid() && (!bAmbush || DoomWorld->CheckSight(this, S.SoundTarget.Get())))
		{
			WakeUp(S.SoundTarget.Get(), false);
			return;
		}
	}
	if (!DoomWorld->CheckSight(this, P))
	{
		return;
	}
	// only in front, unless very close
	const FVector2f D = P->DoomPos() - DoomPos();
	const float An = FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X)) - DoomAngle + 720.f, 360.f);
	if (An > 90.f && An < 270.f && D.Size() > 64.f)
	{
		return;
	}
	WakeUp(P, false);
}

void ADoomMonster::FaceTarget()
{
	if (!Target.IsValid())
	{
		return;
	}
	const FVector2f D = Doom::ToDoom2D(Target->GetActorLocation()) - DoomPos();
	DoomAngle = FMath::RadiansToDegrees(FMath::Atan2(D.Y, D.X));
	if (const ADoomPlayer* P = Cast<ADoomPlayer>(Target.Get()))
	{
		if (P->Invisibility > 0)
		{
			DoomAngle += (MonsterRand() - MonsterRand()) * 0.09f; // spectral fuzz throws the aim off
		}
	}
}

bool ADoomMonster::CheckMeleeRange() const
{
	if (!Target.IsValid())
	{
		return false;
	}
	float TargetRadius = 16.f;
	if (const ADoomMonster* M = Cast<ADoomMonster>(Target.Get())) TargetRadius = M->Def->Radius;
	const float Dist = FVector2f::Distance(Doom::ToDoom2D(Target->GetActorLocation()), DoomPos());
	return Dist < 64.f - 20.f + TargetRadius && DoomWorld->CheckSight(this, Target.Get());
}

bool ADoomMonster::CheckMissileRange()
{
	if (!Target.IsValid() || !DoomWorld->CheckSight(this, Target.Get()))
	{
		return false;
	}
	if (bJustHit)
	{
		bJustHit = false;
		return true;
	}
	if (ReactionTime > 0)
	{
		return false;
	}
	float Dist = FVector2f::Distance(Doom::ToDoom2D(Target->GetActorLocation()), DoomPos()) - 64.f;
	if (!*Def->Melee)
	{
		Dist -= 128.f;
	}
	if (Is(Def, TEXT("ArchVile")) && Dist > 14 * 64)
	{
		return false;
	}
	if (Is(Def, TEXT("Revenant")))
	{
		if (Dist < 196) return false;
		Dist *= 0.5f;
	}
	if (Is(Def, TEXT("Cyberdemon")) || Is(Def, TEXT("SpiderMastermind")) || Is(Def, TEXT("LostSoul")))
	{
		Dist *= 0.5f;
	}
	Dist = FMath::Min(Dist, Is(Def, TEXT("Cyberdemon")) ? 160.f : 200.f);
	return MonsterRand() >= Dist;
}

bool ADoomMonster::TestMove(int32 Dir) const
{
	if (Dir == DirNone)
	{
		return false;
	}
	const float S = Doom::Scale;
	const FVector2f D = DirVector(Dir);
	const FVector Dir3(D.X, -D.Y, 0);
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector Feet = GetActorLocation() - FVector(0, 0, Half);
	const float Step = (Def->Flags & EThingFlag::Float) ? 0.f : 24.f * S;
	const float BodyHalf = FMath::Max((Def->Height * S - Step) * 0.5f, Radius);
	const FVector From = Feet + FVector(0, 0, Step + BodyHalf + 1.f);
	const FVector To = From + Dir3 * FMath::Max(Def->Speed, 8.f) * 2.f * S;

	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomMonsterMove), false, this);
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius * 0.95f, BodyHalf);
	if (GetWorld()->SweepTestByChannel(From, To, FQuat::Identity, ECC_DoomMonster, Shape, Q))
	{
		return false;
	}
	if (Def->Flags & EThingFlag::Float)
	{
		return true;
	}
	// floor at the destination: no climbing above 24, no dropping below 24 (Doom's dropoff)
	FHitResult Floor;
	const FVector Down = To - FVector(0, 0, BodyHalf);
	if (!GetWorld()->LineTraceSingleByChannel(Floor, Down, Down - FVector(0, 0, 200.f * S), ECC_DoomMonster, Q))
	{
		return false;
	}
	const float FloorZ = Floor.ImpactPoint.Z;
	return FloorZ <= Feet.Z + 24.f * S + 1.f && FloorZ >= Feet.Z - 24.f * S - 1.f;
}

bool ADoomMonster::TryWalk(int32 Dir)
{
	if (Dir == DirNone)
	{
		return false;
	}
	// did the last step actually move? (Character Movement does the real walking)
	const float Moved = FVector2f::Distance(DoomPos(), StepStart);
	StepStart = DoomPos();
	if (Moved < Def->Speed * 0.25f || !TestMove(Dir))
	{
		// blocked: maybe it is a door we can open
		const FVector2f Ahead = DoomPos() + DirVector(Dir) * (Def->Radius + 24.f);
		DoomWorld->TryMonsterUse(this, DoomPos(), Ahead);
		return false;
	}
	return true;
}

void ADoomMonster::NewChaseDir()
{
	if (!Target.IsValid())
	{
		MoveDir = DirNone;
		return;
	}
	const int32 OldDir = MoveDir;
	const int32 TurnAround = Opposite[OldDir];
	const FVector2f Delta = Doom::ToDoom2D(Target->GetActorLocation()) - DoomPos();
	int32 D1 = Delta.X > 10 ? 0 : Delta.X < -10 ? 4 : DirNone;
	int32 D2 = Delta.Y < -10 ? 6 : Delta.Y > 10 ? 2 : DirNone;

	auto Try = [&](int32 Dir)
	{
		if (Dir == DirNone || !TestMove(Dir))
		{
			return false;
		}
		MoveDir = Dir;
		MoveCount = MonsterRand() & 15;
		StepStart = DoomPos();
		return true;
	};

	if (D1 != DirNone && D2 != DirNone)
	{
		const int32 Diag = D1 == 0 ? (D2 == 2 ? 1 : 7) : (D2 == 2 ? 3 : 5);
		if (Diag != TurnAround && Try(Diag)) return;
	}
	if (MonsterRand() > 200 || FMath::Abs(Delta.Y) > FMath::Abs(Delta.X))
	{
		Swap(D1, D2);
	}
	if (D1 == TurnAround) D1 = DirNone;
	if (D2 == TurnAround) D2 = DirNone;
	if (Try(D1) || Try(D2)) return;
	if (OldDir != DirNone && Try(OldDir)) return;
	if (MonsterRand() & 1)
	{
		for (int32 d = 0; d < 8; ++d) if (d != TurnAround && Try(d)) return;
	}
	else
	{
		for (int32 d = 7; d >= 0; --d) if (d != TurnAround && Try(d)) return;
	}
	if (TurnAround != DirNone && Try(TurnAround)) return;
	MoveDir = DirNone;
}

bool ADoomMonster::VileTryRaise()
{
	for (const TWeakObjectPtr<ADoomMonster>& M : DoomWorld->GetMonsters())
	{
		if (!M.IsValid() || M.Get() == this || !M->CanBeRaised())
		{
			continue;
		}
		if (FVector2f::Distance(M->DoomPos(), DoomPos()) > Def->Radius + M->Def->Radius + 24.f)
		{
			continue;
		}
		Target = M.Get();
		FaceTarget();
		M->Resurrect();
		const FVector L = M->GetActorLocation();
		DoomWorld->PlaySound(TEXT("SLOP"), &L);
		StartState(EMonsterState::Raising);
		return true;
	}
	return false;
}

bool ADoomMonster::CanBeRaised() const
{
	return State == EMonsterState::Dead && !Is(Def, TEXT("LostSoul")) && !Is(Def, TEXT("Cyberdemon"))
		&& !Is(Def, TEXT("SpiderMastermind")) && !Is(Def, TEXT("ArchVile")) && !Is(Def, TEXT("PainElemental")) && !Is(Def, TEXT("BossBrain"))
		&& !Is(Def, TEXT("CommanderKeen"));
}

void ADoomMonster::Resurrect()
{
	Health = Def->Health;
	UCapsuleComponent* Cap = GetCapsuleComponent();
	Cap->SetCollisionResponseToAllChannels(ECR_Block);
	Cap->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	FString Reverse = Def->Death;
	Reverse.ReverseString();
	Sprite->PlaySequence(Reverse, 5, false, false);
	State = EMonsterState::Raising;
	Target.Reset();
}

void ADoomMonster::Chase()
{
	if (Threshold > 0)
	{
		--Threshold;
	}
	// turn towards the movement direction in 45 degree steps
	if (MoveDir != DirNone)
	{
		const float Want = MoveDir * 45.f;
		float Delta = FMath::Fmod(Want - DoomAngle + 540.f, 360.f) - 180.f;
		DoomAngle += FMath::Clamp(Delta, -45.f, 45.f);
	}

	bool bTargetOk = Target.IsValid();
	if (bTargetOk)
	{
		if (const ADoomPlayer* P = Cast<ADoomPlayer>(Target.Get())) bTargetOk = !P->IsDead();
		else if (const ADoomMonster* M = Cast<ADoomMonster>(Target.Get())) bTargetOk = !M->IsDead();
	}
	if (!bTargetOk)
	{
		Target.Reset();
		ADoomPlayer* P = DoomWorld->GetPlayer();
		if (P && !P->IsDead() && DoomWorld->CheckSight(this, P))
		{
			Target = P;
		}
		else
		{
			MoveDir = DirNone;
			StartState(EMonsterState::Idle);
			return;
		}
	}

	if (bJustAttacked)
	{
		bJustAttacked = false;
		if (DoomWorld->GetSkill() < 5)
		{
			NewChaseDir();
		}
		return;
	}

	if (Is(Def, TEXT("ArchVile")) && VileTryRaise())
	{
		return;
	}
	if (*Def->Melee && CheckMeleeRange())
	{
		if (*Def->AttackSound && !Is(Def, TEXT("Revenant")))
		{
			const FVector L = GetActorLocation();
			DoomWorld->PlaySound(Def->AttackSound, &L);
		}
		StartState(EMonsterState::Melee);
		return;
	}
	if (*Def->Attack && (DoomWorld->GetSkill() >= 5 || MoveCount <= 0) && CheckMissileRange())
	{
		StartState(EMonsterState::Missile);
		bJustAttacked = true;
		return;
	}

	if (--MoveCount < 0 || !TryWalk(MoveDir))
	{
		NewChaseDir();
	}

	if (*Def->ActiveSound && MonsterRand() < 3)
	{
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->ActiveSound, &L);
	}
}

void ADoomMonster::HitscanAttack(int32 Pellets, float SpreadDeg, int32 DamageMul)
{
	if (!Target.IsValid())
	{
		return;
	}
	const FVector From = EyeLocation();
	const FVector Aim = Target->GetActorLocation();
	const FVector Base = (Aim - From).GetSafeNormal();
	const float Range = 2048.f * Doom::Scale;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomMonsterShot), false, this);
	if (*Def->AttackSound)
	{
		DoomWorld->PlaySound(Def->AttackSound, &From);
	}
	for (int32 i = 0; i < Pellets; ++i)
	{
		const float Yaw = (MonsterRand() - MonsterRand()) / 255.f * SpreadDeg;
		const FVector Dir = Base.RotateAngleAxis(Yaw, FVector::UpVector);
		FHitResult Hit;
		if (!GetWorld()->LineTraceSingleByChannel(Hit, From, From + Dir * Range, ECC_Visibility, Q))
		{
			continue;
		}
		const int32 Damage = ((MonsterRand() % 5) + 1) * DamageMul;
		if (Hit.GetActor() && (Hit.GetActor()->IsA<ADoomPlayer>() || Hit.GetActor()->IsA<ADoomMonster>() || Hit.GetActor()->IsA<ADoomThing>()))
		{
			UGameplayStatics::ApplyPointDamage(Hit.GetActor(), Damage, Dir, Hit, nullptr, this, nullptr);
			if (!Hit.GetActor()->IsA<ADoomThing>())
			{
				DoomWorld->SpawnEffect(TEXT("BLUD"), TEXT("CBA"), 8, Hit.ImpactPoint, false);
			}
		}
		else
		{
			DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
		}
	}
}

void ADoomMonster::DoAttack(bool bMelee)
{
	if (!Target.IsValid())
	{
		return;
	}
	const FVector Center = GetActorLocation() + FVector(0, 0, 4.f * Doom::Scale);
	const FVector TargetCenter = Target->GetActorLocation();
	const FVector ToTarget = (TargetCenter - Center).GetSafeNormal();
	auto Missile = [&](const TCHAR* Type, float YawOffset)
	{
		DoomWorld->SpawnProjectile(Type, this, Center + ToTarget * Def->Radius * Doom::Scale * 0.5f, ToTarget.RotateAngleAxis(YawOffset, FVector::UpVector),
			Is(Def, TEXT("Revenant")) ? Target.Get() : nullptr);
	};
	auto MeleeHit = [&](int32 Die)
	{
		if (CheckMeleeRange())
		{
			const int32 Damage = ((MonsterRand() % Die) + 1) * Def->MeleeDamage;
			UGameplayStatics::ApplyDamage(Target.Get(), Damage, nullptr, this, nullptr);
			const FVector L = GetActorLocation();
			DoomWorld->PlaySound(Is(Def, TEXT("Imp")) ? TEXT("CLAW") : Is(Def, TEXT("Revenant")) ? TEXT("SKEPCH") : TEXT(""), &L);
			return true;
		}
		return false;
	};

	switch (Def->Attack_)
	{
	case EDoomAttack::Hitscan: HitscanAttack(1, 22.5f, 3); break;
	case EDoomAttack::Shotgun: HitscanAttack(3, 22.5f, 3); break;
	case EDoomAttack::Chaingun: HitscanAttack(1, 22.5f, 3); break;
	case EDoomAttack::Spider: HitscanAttack(3, 22.5f, 3); break;
	case EDoomAttack::Melee: MeleeHit(Is(Def, TEXT("Demon")) || Is(Def, TEXT("Spectre")) ? 10 : 8); break;
	case EDoomAttack::MeleeOrMissile:
		if (bMelee || CheckMeleeRange())
		{
			MeleeHit(Is(Def, TEXT("Cacodemon")) ? 6 : 8);
		}
		else
		{
			Missile(Def->Projectile, 0);
		}
		break;
	case EDoomAttack::Missile: Missile(Def->Projectile, 0); break;
	case EDoomAttack::Cyber: Missile(TEXT("Rocket"), 0); break;
	case EDoomAttack::Revenant:
		if (bMelee) MeleeHit(10);
		else Missile(TEXT("Tracer"), 0);
		break;
	case EDoomAttack::Mancubus:
	{
		const float Spread = 11.25f * (1 + Sprite->FrameIndex() / 3);
		Missile(TEXT("FatShot"), Spread);
		Missile(TEXT("FatShot"), -Spread);
		break;
	}
	case EDoomAttack::Skull:
	{
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->AttackSound, &L);
		ChargeVelocity = (TargetCenter - GetActorLocation()).GetSafeNormal() * 20.f * Doom::TicRate * Doom::Scale;
		ChargeTics = Doom::TicRate;
		GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		StartState(EMonsterState::Charging);
		break;
	}
	case EDoomAttack::PainSpawn:
	{
		int32 Souls = 0;
		for (const TWeakObjectPtr<ADoomMonster>& M : DoomWorld->GetMonsters())
		{
			if (M.IsValid() && Is(M->Def, TEXT("LostSoul")) && !M->IsDead()) ++Souls;
		}
		if (Souls < 21)
		{
			const FVector Spawn = GetActorLocation() + FVector(ToTarget.X, ToTarget.Y, 0).GetSafeNormal() * (Def->Radius + 24.f) * Doom::Scale
				- FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomPainSpawn), false, this);
			if (!GetWorld()->SweepTestByChannel(GetActorLocation(), Spawn + FVector(0, 0, 28 * Doom::Scale), FQuat::Identity, ECC_DoomMonster, FCollisionShape::MakeSphere(16 * Doom::Scale), Q))
			{
				if (ADoomMonster* Soul = DoomWorld->SpawnMonster(3006, Spawn, DoomAngle))
				{
					Soul->Target = Target;
					Soul->StartState(EMonsterState::Missile);
				}
			}
		}
		break;
	}
	case EDoomAttack::Vile:
		if (DoomWorld->CheckSight(this, Target.Get()))
		{
			DoomWorld->SpawnEffect(TEXT("FIRE"), TEXT("ABCDEFGH"), 3, TargetCenter, true);
			DoomWorld->PlaySound(TEXT("BAREXP"), &TargetCenter);
			UGameplayStatics::ApplyDamage(Target.Get(), 20.f, nullptr, this, nullptr);
			DoomWorld->RadiusAttack(this, this, TargetCenter, 70.f);
			if (ACharacter* C = Cast<ACharacter>(Target.Get()))
			{
				C->LaunchCharacter(FVector(0, 0, 1000.f * Doom::Scale / 3.f), false, true);
			}
		}
		break;
	default:
		break;
	}
}

float ADoomMonster::TakeDamage(float Damage, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (IsDead() || Damage <= 0 || !Def)
	{
		return 0.f;
	}
	if ((Def->Flags & EThingFlag::Boss) && DamageEvent.IsOfType(FRadialDamageEvent::ClassID) && !Is(Def, TEXT("BossBrain")))
	{
		return 0.f; // cyberdemon and mastermind shrug off splash
	}
	AActor* Source = DamageCauser ? (DamageCauser->GetOwner() ? DamageCauser->GetOwner() : DamageCauser) : nullptr;
	Health -= FMath::RoundToInt(Damage);

	// knockback (P_DamageMobj thrust): stronger for light monsters
	if (Source && !(Def->Flags & EThingFlag::Hanging))
	{
		const FVector Away = (GetActorLocation() - Source->GetActorLocation()).GetSafeNormal2D();
		const float Mass = FMath::Max(100.f, Def->Health * 1.f);
		const float Thrust = FMath::Min(Damage * 12.5f * 100.f / Mass, 600.f) * Doom::Scale;
		LaunchCharacter(Away * Thrust, false, false);
	}

	if (Health <= 0)
	{
		Die(Source, Health < -Def->Health);
		return Damage;
	}
	if (MonsterRand() < Def->PainChance && *Def->Pain && State != EMonsterState::Charging)
	{
		bJustHit = true;
		StartState(EMonsterState::Pain);
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->PainSound, &L);
	}
	ReactionTime = 0;
	// infighting: hit by another monster -> go after it
	if (Source && Source != this && (Threshold <= 0 || Is(Def, TEXT("ArchVile"))) && !Is(Def, TEXT("ArchVile")))
	{
		if (Source->IsA<ADoomPlayer>() || Source->IsA<ADoomMonster>())
		{
			Target = Source;
			Threshold = 100;
			if (State == EMonsterState::Idle)
			{
				StartState(EMonsterState::Chase);
			}
		}
	}
	return Damage;
}

void ADoomMonster::Die(AActor* Killer, bool bGib)
{
	State = EMonsterState::Dying;
	Target.Reset();
	MoveDir = DirNone;
	ChargeVelocity = FVector::ZeroVector;
	UCapsuleComponent* Cap = GetCapsuleComponent();
	// corpses keep resting on the world but stop blocking bodies and bullets
	Cap->SetCollisionResponseToAllChannels(ECR_Ignore);
	Cap->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Cap->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	if (Def->Flags & EThingFlag::Float)
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Falling); // flyers drop
	}
	const bool bXDeath = bGib && *Def->XDeath;
	Sprite->PlaySequence(bXDeath ? Def->XDeath : Def->Death, 5, false, (Def->Flags & EThingFlag::FullBright) != 0);
	Sprite->SetFuzz((Def->Flags & EThingFlag::Shadow) != 0);
	const FVector L = GetActorLocation();
	DoomWorld->PlaySound(bXDeath ? TEXT("SLOP") : Def->DeathSound, &L);
	if (Def->Flags & EThingFlag::CountKill)
	{
		++DoomWorld->Kills;
	}
	if (Def->DropType)
	{
		const FVector Feet = L - FVector(0, 0, Cap->GetScaledCapsuleHalfHeight());
		DoomWorld->SpawnThing(Def->DropType, Feet, 0, true);
	}
	if (Light)
	{
		Light->SetVisibility(false);
	}
	if (Is(Def, TEXT("LostSoul")))
	{
		Destroy();
		return;
	}
	if (Is(Def, TEXT("PainElemental")))
	{
		for (int32 i = 0; i < 3; ++i)
		{
			const float A = DoomAngle + 90.f * (i + 1);
			const FVector Off = Doom::ToUE(FMath::Cos(FMath::DegreesToRadians(A)), FMath::Sin(FMath::DegreesToRadians(A)), 0) * 40.f;
			DoomWorld->SpawnMonster(3006, L + Off - FVector(0, 0, Cap->GetScaledCapsuleHalfHeight()), A);
		}
	}
	DoomWorld->OnMonsterDied(this);
}

void ADoomMonster::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
}

void ADoomMonster::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
	if (State != EMonsterState::Charging)
	{
		return;
	}
	// a charging lost soul slams into whatever it hits
	if (Other && (Other->IsA<ADoomPlayer>() || Other->IsA<ADoomMonster>()))
	{
		UGameplayStatics::ApplyDamage(Other, ((MonsterRand() % 8) + 1) * 3, nullptr, this, nullptr);
	}
	ChargeVelocity = FVector::ZeroVector;
	ChargeTics = 0;
	GetCharacterMovement()->StopMovementImmediately();
	StartState(EMonsterState::Chase);
}
