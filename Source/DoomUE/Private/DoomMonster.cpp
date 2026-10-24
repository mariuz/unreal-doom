#include "DoomMonster.h"
#include "DoomMonsterPrivate.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "DoomWorld.h"
#include "DoomPlayer.h"
#include "DoomThing.h"
#include "DoomLevel.h"
#include "Interfaces/MovementBaseInterface.h"
#include "DoomSpriteComponent.h"
#include "DoomUE.h"
#include "AIController.h"
#include "BrainComponent.h"
#include "DoomAI.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DamageEvents.h"

float ADoomMonster::MonsterMass(const FDoomThingDef* D)
{
	// vanilla mobjinfo masses (knockback divides by these)
	switch (D ? D->DoomEdNum : 0)
	{
	case 3006: return 50.f;                                  // lost soul
	case 3002: case 58: case 3005: case 71: return 400.f;    // demon, spectre, cacodemon, pain elemental
	case 66: case 64: return 500.f;                          // revenant, arch-vile
	case 68: return 600.f;                                   // arachnotron
	case 3003: case 69: case 67: case 16: case 7: return 1000.f; // barons, mancubus, cyberdemon, mastermind
	case 72: case 88: return 10000000.f;                     // Keen, the boss brain
	default: return 100.f;
	}
}

ADoomMonster::ADoomMonster(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	// CLASSIC monsters need no controller (Doom's own code moves them, bRunPhysicsWithNoController);
	// SMART ones get an ADoomAIController (SetSmartAI)
	AIControllerClass = nullptr;
	AutoPossessAI = EAutoPossessAI::Disabled;
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

void ADoomMonster::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADoomMonster, NetType);
	DOREPLIFETIME(ADoomMonster, bNetCorpse);
}

void ADoomMonster::SetBase(FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName, bool bNotifyActor)
{
	const UActorComponent* Base = MovementBaseInterfaceData ? Cast<UActorComponent>(MovementBaseInterfaceData->PhysicsObjectOwner.Get()) : nullptr;
	if (Base && Base->GetOwner() && Base->GetOwner()->IsA<ADoomLevel>())
	{
		MovementBaseInterfaceData = nullptr;
	}
	Super::SetBase(MovementBaseInterfaceData, BoneName, bNotifyActor);
}

void ADoomMonster::OnRep_NetType()
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	const FDoomThingDef* D = DoomInfo::FindThing(NetType);
	if (W && D && W->EnsureWads())
	{
		InitMonster(W, D, Sprite->DoomAngle, false);
	}
}

void ADoomMonster::OnRep_NetCorpse()
{
	if (bNetCorpse && Def)
	{
		MakeCorpse();
	}
	else if (!bNetCorpse && Sprite)
	{
		Sprite->bSleepWhenDone = false; // raised by an arch-vile on the server
		Sprite->Wake();
	}
}

void ADoomMonster::InitMonster(UDoomWorld* InWorld, const FDoomThingDef* InDef, float InDoomAngle, bool bInAmbush)
{
	DoomWorld = InWorld;
	Def = InDef;
	if (HasAuthority())
	{
		NetType = Def->DoomEdNum;
	}
	DoomAngle = InDoomAngle;
	bAmbush = bInAmbush;
	Health = Def->Health;
	const float S = Doom::Scale;
	// the capsule keeps the monster's real height (so it fits under the ceilings it fits under in Doom
	// and spawns on the floor); a capsule can't be wider than it is tall, so wide monsters (spider,
	// arachnotron, mancubus, demons) also get a box of Doom's full width that catches bullets and missiles
	// (a hair shorter than Doom's height: a 56-unit monster fits a 56-unit opening in Doom, and a capsule
	// exactly that tall grazes the ceiling, which Character Movement takes for being stuck)
	const float HalfHeight = Def->Height * 0.5f * S - 1.f;
	GetCapsuleComponent()->SetCapsuleSize(FMath::Min(Def->Radius * S, HalfHeight), HalfHeight);
	if (Def->Radius > Def->Height * 0.5f && !HitBox)
	{
		HitBox = NewObject<UBoxComponent>(this, TEXT("HitBox"));
		HitBox->SetupAttachment(GetCapsuleComponent());
		HitBox->SetBoxExtent(FVector(Def->Radius, Def->Radius, Def->Height * 0.5f) * S);
		HitBox->SetCollisionObjectType(ECC_DoomMonster);
		HitBox->SetCollisionResponseToAllChannels(ECR_Ignore);
		HitBox->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		HitBox->SetCollisionResponseToChannel(ECC_DoomMissile, ECR_Block);
		HitBox->SetCanEverAffectNavigation(false);
		HitBox->RegisterComponent();
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxWalkSpeed = Def->Speed / FMath::Max(1, RunTics()) * Doom::TicRate * S;
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
	if (HasAuthority() && DoomWorld->IsSmartAI())
	{
		SetSmartAI(true);
	}
	else if (HasAuthority() && Doom::PerfBaseline() && !Controller)
	{
		AIControllerClass = AAIController::StaticClass();
		SpawnDefaultController();
	}

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
	if (S != EMonsterState::Idle)
	{
		WakeIdle();
	}
	State = S;
	FireFrameSeen = -1;
	const bool bBright = (Def->Flags & EThingFlag::FullBright) != 0;
	switch (S)
	{
	case EMonsterState::Idle: Sprite->PlaySequence(Def->Idle, Def->IdleTics, true, bBright); break;
	case EMonsterState::Chase: Sprite->PlaySequence(!Def->Run.IsEmpty() ? Def->Run : Def->Idle, RunTics(), true, bBright); StepStart = DoomPos(); break;
	case EMonsterState::Melee: Sprite->PlaySequence(Def->Melee, IsFastDemon() ? 3 : 6, false, bBright); FaceTarget(); break;
	case EMonsterState::Missile:
		Sprite->PlaySequence(Def->Attack, DoomMonsterDetail::Is(Def, TEXT("ArchVile")) ? 5 : DoomMonsterDetail::Is(Def, TEXT("Chaingunner")) || DoomMonsterDetail::Is(Def, TEXT("SpiderMastermind")) ? 4 : 8, false, true);
		FaceTarget();
		if (DoomMonsterDetail::Is(Def, TEXT("ArchVile")))
		{
			const FVector L = GetActorLocation();
			DoomWorld->PlaySound(TEXT("VILATK"), &L);
		}
		break;
	case EMonsterState::Pain: Sprite->PlaySequence(Def->Pain, IsFastDemon() ? 2 : 4, false, bBright); break;
	case EMonsterState::Raising: Sprite->PlaySequence(TEXT("[\\]"), 10, false, true); break;
	case EMonsterState::Charging: Sprite->PlaySequence(TEXT("CD"), 4, true, true); break;
	default: break;
	}
}

void ADoomMonster::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Def || IsDead() || !HasAuthority())
	{
		return;
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (State == EMonsterState::Charging)
	{
		Move->Velocity = ChargeVelocity;
		return;
	}
	if (State == EMonsterState::Chase && MoveDir != DoomMonsterDetail::DirNone)
	{
		// SMART: straight along the path, not in Doom's eight directions
		const FVector2f D = (bSmart && SmartMove == ESmartMove::Moving && !SmartDir.IsNearlyZero()) ? SmartDir : DoomMonsterDetail::DirVector(MoveDir);
		FVector Input(D.X, -D.Y, 0);
		if ((Def->Flags & EThingFlag::Float) && FloatGoalTics > 0)
		{
			const float Dz = FloatGoalZ - GetActorLocation().Z;
			if (FMath::Abs(Dz) > 1.f * Doom::Scale)
			{
				Input.Z = FMath::Sign(Dz); // FLOATSPEED: up or down into the opening
			}
		}
		else if ((Def->Flags & EThingFlag::Float) && Target.IsValid())
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
	LastPos = DoomPos(); // re-read: a teleport line moves the monster
	if (FloatGoalTics > 0)
	{
		--FloatGoalTics;
	}

	switch (State)
	{
	case EMonsterState::Idle:
		if ((DoomWorld->GetTic() & 3) == 0)
		{
			Look();
		}
		// waiting for the player, at rest for a second on a still floor: no movement or actor tick until
		// it wakes (StartState) or its floor lowers (UDoomWorld's movers); Look and the sprite run on Doom's tics
		if (!bIdleAsleep && State == EMonsterState::Idle && HasAuthority() && !Doom::PerfBaseline())
		{
			const UCharacterMovementComponent* Move = GetCharacterMovement();
			const bool bStill = GetVelocity().IsNearlyZero(1.f) && (Move->IsMovingOnGround() || Move->MovementMode == MOVE_Flying || Move->MovementMode == MOVE_None);
			IdleRestTics = bStill && !DoomWorld->IsSectorMoving(Sec) ? IdleRestTics + 1 : 0;
			if (IdleRestTics >= Doom::TicRate)
			{
				bIdleAsleep = true;
				GetCharacterMovement()->StopMovementImmediately();
				GetCharacterMovement()->SetComponentTickEnabled(false);
				SetActorTickEnabled(false);
			}
		}
		break;
	case EMonsterState::Chase:
		if (bFrameChanged || Sprite->CurrentSequence().Len() == 1 && (DoomWorld->GetTic() % FMath::Max(1, RunTics())) == 0)
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
			// A_CPosRefire / A_SpidRefire: chaingunners, SS, arachnotrons and the mastermind keep
			// firing while they can see a live target (and now and then regardless)
			const int32 Num = Def->DoomEdNum;
			const bool bRefires = State == EMonsterState::Missile && (Num == 65 || Num == 84 || Num == 68 || Num == 7);
			const ADoomPlayer* TP = Cast<ADoomPlayer>(Target.Get());
			const ADoomMonster* TM = Cast<ADoomMonster>(Target.Get());
			const bool bTargetAlive = (TP && !TP->IsDead()) || (TM && !TM->IsDead());
			if (bRefires && (DoomMonsterDetail::MonsterRand() < (Num == 68 || Num == 7 ? 10 : 40) || (bTargetAlive && DoomWorld->CheckSight(this, Target.Get()))))
			{
				FaceTarget();
				StartState(EMonsterState::Missile);
				break;
			}
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
			if (DoomMonsterDetail::Is(Def, TEXT("LostSoul")))
			{
				Destroy(); // burnt out: lost souls leave no corpse
				return;
			}
		}
		break;
	case EMonsterState::Dead:
		++DeadTics;
		// at rest on the floor for a second, with nothing moving its sector: nothing moves a corpse until
		// its floor lowers or it rises again. (A moving floor's collision is cooked a few tics behind the
		// floor itself; a corpse that slept on the lagging collision would hang in the air.)
		if (!bCorpseAsleep && GetCharacterMovement()->IsMovingOnGround() && GetVelocity().IsNearlyZero(1.f)
			&& !DoomWorld->IsSectorMoving(DoomWorld->SectorAt(GetActorLocation())))
		{
			++CorpseRestTics;
		}
		else
		{
			CorpseRestTics = 0;
		}
		if (!bCorpseAsleep && CorpseRestTics >= Doom::TicRate)
		{
			bCorpseAsleep = true;
			GetCharacterMovement()->StopMovementImmediately();
			GetCharacterMovement()->SetComponentTickEnabled(false);
			SetActorTickEnabled(false);
		}
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
	if (!Def->SeeSound.IsEmpty())
	{
		const bool bBoss = DoomMonsterDetail::Is(Def, TEXT("Cyberdemon")) || DoomMonsterDetail::Is(Def, TEXT("SpiderMastermind"));
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->SeeSound, bBoss ? nullptr : &L);
	}
	StartState(EMonsterState::Chase);
}

void ADoomMonster::Look()
{
	// P_LookForPlayers: any living player (all of them in a network game)
	if (!DoomWorld->NearestPlayer(this, false))
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
	// the nearest player in sight
	ADoomPlayer* P = DoomWorld->NearestPlayer(this, true);
	if (!P)
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
			DoomAngle += (DoomMonsterDetail::MonsterRand() - DoomMonsterDetail::MonsterRand()) * 0.09f; // spectral fuzz throws the aim off
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
	if (Def->Melee.IsEmpty())
	{
		Dist -= 128.f;
	}
	if (DoomMonsterDetail::Is(Def, TEXT("ArchVile")) && Dist > 14 * 64)
	{
		return false;
	}
	if (DoomMonsterDetail::Is(Def, TEXT("Revenant")))
	{
		if (Dist < 196) return false;
		Dist *= 0.5f;
	}
	if (DoomMonsterDetail::Is(Def, TEXT("Cyberdemon")) || DoomMonsterDetail::Is(Def, TEXT("SpiderMastermind")) || DoomMonsterDetail::Is(Def, TEXT("LostSoul")))
	{
		Dist *= 0.5f;
	}
	Dist = FMath::Min(Dist, DoomMonsterDetail::Is(Def, TEXT("Cyberdemon")) ? 160.f : 200.f);
	return DoomMonsterDetail::MonsterRand() >= Dist;
}

bool ADoomMonster::TestMove(int32 Dir) const
{
	if (Dir == DoomMonsterDetail::DirNone)
	{
		return false;
	}
	const float S = Doom::Scale;
	const FVector2f D = DoomMonsterDetail::DirVector(Dir);
	const FVector Dir3(D.X, -D.Y, 0);
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector Feet = GetActorLocation() - FVector(0, 0, Half);
	const float Step = (Def->Flags & EThingFlag::Float) ? 0.f : 24.f * S;
	// the body above the step height, no taller than the monster: a taller test shape started inside
	// low ceilings and failed in every direction, freezing monsters in low rooms and doorways
	const float BodyHalf = FMath::Max((Def->Height * S - Step) * 0.5f - 1.f, 1.f);
	const FVector From = Feet + FVector(0, 0, Step + BodyHalf + 1.f);
	const FVector To = From + Dir3 * FMath::Max(Def->Speed, 8.f) * 2.f * S;

	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomMonsterMove), false, this);
	// a box, like Doom's: as wide as the walking capsule (a capsule that short would have to be thinner
	// than the body, and then promised paths the body couldn't take), slightly inset for corners
	const FCollisionShape Shape = FCollisionShape::MakeBox(FVector(Radius * 0.9f, Radius * 0.9f, BodyHalf));
	// P_Move's floatok: a flyer blocked at its height may still fit the opening ahead higher up or lower
	// down; then it rises or sinks towards it (FloatGoalZ) and counts the move as made
	auto FloatOk = [&]() -> bool
	{
		if (!(Def->Flags & EThingFlag::Float))
		{
			return false;
		}
		for (int32 k = 1; k <= 12; ++k)
		{
			for (float Sign : { 1.f, -1.f })
			{
				const FVector Off(0, 0, Sign * k * 8.f * S);
				if (!GetWorld()->SweepTestByChannel(From, From + Off, FQuat::Identity, ECC_DoomMonster, Shape, Q)
					&& !GetWorld()->SweepTestByChannel(From + Off, To + Off, FQuat::Identity, ECC_DoomMonster, Shape, Q))
				{
					FloatGoalZ = GetActorLocation().Z + Off.Z;
					FloatGoalTics = 8;
					return true;
				}
			}
		}
		return false;
	};
	FHitResult Block;
	if (GetWorld()->SweepSingleByChannel(Block, From, To, FQuat::Identity, ECC_DoomMonster, Shape, Q))
	{
		if (!Block.bStartPenetrating)
		{
			return FloatOk();
		}
		// the box's corner already touches something (a wall we lean on, a neighbour): moving away from
		// it is fine; test again from just outside it
		const FVector Out = Block.Normal.GetSafeNormal2D();
		if (FVector::DotProduct(Out, Dir3) < -0.1f)
		{
			return false;
		}
		const FVector Free = From + Out * (Block.PenetrationDepth + 1.f);
		if (GetWorld()->SweepTestByChannel(Free, To, FQuat::Identity, ECC_DoomMonster, Shape, Q))
		{
			return FloatOk();
		}
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
	if (Dir == DoomMonsterDetail::DirNone)
	{
		return false;
	}
	// did the last step actually move? (Character Movement does the real walking)
	const float Moved = FVector2f::Distance(DoomPos(), StepStart);
	StepStart = DoomPos();
	if (Moved < Def->Speed * 0.25f || !TestMove(Dir))
	{
		// blocked: maybe it is a door we can open
		const FVector2f Ahead = DoomPos() + DoomMonsterDetail::DirVector(Dir) * (Def->Radius + 24.f);
		DoomWorld->TryMonsterUse(this, DoomPos(), Ahead);
		return false;
	}
	return true;
}

void ADoomMonster::NewChaseDir()
{
	if (!Target.IsValid())
	{
		MoveDir = DoomMonsterDetail::DirNone;
		return;
	}
	const int32 OldDir = MoveDir;
	const int32 TurnAround = DoomMonsterDetail::Opposite[OldDir];
	const FVector2f Delta = Doom::ToDoom2D(Target->GetActorLocation()) - DoomPos();
	int32 D1 = Delta.X > 10 ? 0 : Delta.X < -10 ? 4 : DoomMonsterDetail::DirNone;
	int32 D2 = Delta.Y < -10 ? 6 : Delta.Y > 10 ? 2 : DoomMonsterDetail::DirNone;

	auto Try = [&](int32 Dir)
	{
		if (Dir == DoomMonsterDetail::DirNone || !TestMove(Dir))
		{
			return false;
		}
		MoveDir = Dir;
		MoveCount = DoomMonsterDetail::MonsterRand() & 15;
		StepStart = DoomPos();
		return true;
	};

	if (D1 != DoomMonsterDetail::DirNone && D2 != DoomMonsterDetail::DirNone)
	{
		const int32 Diag = D1 == 0 ? (D2 == 2 ? 1 : 7) : (D2 == 2 ? 3 : 5);
		if (Diag != TurnAround && Try(Diag)) return;
	}
	if (DoomMonsterDetail::MonsterRand() > 200 || FMath::Abs(Delta.Y) > FMath::Abs(Delta.X))
	{
		Swap(D1, D2);
	}
	if (D1 == TurnAround) D1 = DoomMonsterDetail::DirNone;
	if (D2 == TurnAround) D2 = DoomMonsterDetail::DirNone;
	if (Try(D1) || Try(D2)) return;
	if (OldDir != DoomMonsterDetail::DirNone && Try(OldDir)) return;
	if (DoomMonsterDetail::MonsterRand() & 1)
	{
		for (int32 d = 0; d < 8; ++d) if (d != TurnAround && Try(d)) return;
	}
	else
	{
		for (int32 d = 7; d >= 0; --d) if (d != TurnAround && Try(d)) return;
	}
	if (TurnAround != DoomMonsterDetail::DirNone && Try(TurnAround)) return;
	MoveDir = DoomMonsterDetail::DirNone;
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
		// P_CheckPosition: no raising a corpse that something alive is standing on
		bool bOccupied = false;
		auto Blocks = [&](const AActor* A, float Radius)
		{
			const FVector2f D = Doom::ToDoom2D(A->GetActorLocation()) - M->DoomPos();
			return FMath::Abs(D.X) < Radius + M->Def->Radius && FMath::Abs(D.Y) < Radius + M->Def->Radius;
		};
		for (ADoomPlayer* P : DoomWorld->GetPlayers())
		{
			bOccupied |= !P->IsDead() && Blocks(P, 16.f);
		}
		for (const TWeakObjectPtr<ADoomMonster>& O : DoomWorld->GetMonsters())
		{
			bOccupied |= O.IsValid() && O != M && !O->IsDead() && Blocks(O.Get(), O->Def->Radius);
		}
		if (bOccupied)
		{
			continue;
		}
		// face the corpse for the raise, then go back after whoever we were fighting (vanilla keeps the
		// target; turning on the monster just raised made arch-viles shoot their own work)
		const TWeakObjectPtr<AActor> Fighting = Target;
		Target = M.Get();
		FaceTarget();
		Target = Fighting;
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
	return State == EMonsterState::Dead && !DoomMonsterDetail::Is(Def, TEXT("LostSoul")) && !DoomMonsterDetail::Is(Def, TEXT("Cyberdemon"))
		&& !DoomMonsterDetail::Is(Def, TEXT("SpiderMastermind")) && !DoomMonsterDetail::Is(Def, TEXT("ArchVile")) && !DoomMonsterDetail::Is(Def, TEXT("BossBrain"))
		&& !DoomMonsterDetail::Is(Def, TEXT("CommanderKeen"));
}

void ADoomMonster::Resurrect()
{
	Health = Def->Health;
	if (HasAuthority())
	{
		bNetCorpse = false; // clients raise it too (OnRep_NetCorpse)
	}
	UCapsuleComponent* Cap = GetCapsuleComponent();
	Cap->SetCollisionResponseToAllChannels(ECR_Block);
	Cap->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	if (HitBox)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}
	Sprite->bSleepWhenDone = false;
	WakeCorpse();
	FString Reverse = Def->Death;
	Reverse.ReverseString();
	Sprite->PlaySequence(Reverse, 5, false, false);
	State = EMonsterState::Raising;
	Target.Reset();
	if (bSmart)
	{
		if (const AAIController* C = Cast<AAIController>(Controller); C && C->GetBrainComponent())
		{
			C->GetBrainComponent()->RestartLogic();
		}
	}
	if (Def->Flags & EThingFlag::Float)
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	}
}

void ADoomMonster::Chase()
{
	if (ReactionTime > 0)
	{
		--ReactionTime; // once per A_Chase, not every tic
	}
	if (Threshold > 0)
	{
		--Threshold;
	}
	// turn towards the movement direction in 45 degree steps
	if (MoveDir != DoomMonsterDetail::DirNone)
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
		ADoomPlayer* P = DoomWorld->NearestPlayer(this, true);
		if (P)
		{
			Target = P;
		}
		else
		{
			MoveDir = DoomMonsterDetail::DirNone;
			StartState(EMonsterState::Idle);
			return;
		}
	}

	if (bJustAttacked)
	{
		bJustAttacked = false;
		if (!DoomWorld->IsFast())
		{
			NewChaseDir();
		}
		return;
	}

	if (DoomMonsterDetail::Is(Def, TEXT("ArchVile")) && VileTryRaise())
	{
		return;
	}
	if (!Def->Melee.IsEmpty() && CheckMeleeRange())
	{
		if (!Def->AttackSound.IsEmpty() && !DoomMonsterDetail::Is(Def, TEXT("Revenant")))
		{
			const FVector L = GetActorLocation();
			DoomWorld->PlaySound(Def->AttackSound, &L);
		}
		StartState(EMonsterState::Melee);
		return;
	}
	if (!Def->Attack.IsEmpty() && (DoomWorld->IsFast() || MoveCount <= 0) && CheckMissileRange() && !(bSmart && FriendInLineOfFire()))
	{
		StartState(EMonsterState::Missile);
		bJustAttacked = true;
		return;
	}

	if (bSmart && SmartMove == ESmartMove::Moving)
	{
		// the tree's path; MoveCount still paces the missile attacks as in A_Chase
		if (--MoveCount < 0) MoveCount = DoomMonsterDetail::MonsterRand() & 15;
		SmartStep();
	}
	else if (--MoveCount < 0 || !TryWalk(MoveDir))
	{
		NewChaseDir();
	}

	if (!Def->ActiveSound.IsEmpty() && DoomMonsterDetail::MonsterRand() < 3)
	{
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->ActiveSound, &L);
	}
}

void ADoomMonster::WakeIdle()
{
	if (bIdleAsleep)
	{
		bIdleAsleep = false;
		GetCharacterMovement()->SetComponentTickEnabled(true);
		SetActorTickEnabled(true);
	}
	IdleRestTics = 0;
}

void ADoomMonster::WakeCorpse()
{
	if (bCorpseAsleep)
	{
		bCorpseAsleep = false;
		GetCharacterMovement()->SetComponentTickEnabled(true);
		SetActorTickEnabled(true);
	}
	Sprite->Wake(); // its sprite follows it while it moves, and sleeps again once it rests
}

void ADoomMonster::DoomSerialize(FArchive& Ar)
{
	uint8 S = (uint8)State;
	FVector Velocity = GetCharacterMovement()->Velocity;
	Ar << Health << DoomAngle << bAmbush << S << MoveDir << MoveCount << ReactionTime << Threshold
		<< bJustAttacked << bJustHit << ChargeVelocity << ChargeTics << FireFrameSeen << Velocity
		<< SpawnFeet << SpawnAngle << bCanRespawn << DeadTics;
	Sprite->DoomSerialize(Ar);
	if (Ar.IsLoading())
	{
		State = (EMonsterState)S;
		if (IsDead())
		{
			MakeCorpse();
		}
		else
		{
			// a corpse in the scrambled world may be alive in the save: tick again
			Sprite->bSleepWhenDone = false;
			WakeCorpse();
			GetCharacterMovement()->Velocity = Velocity;
		}
		LastPos = StepStart = DoomPos();
		Sprite->DoomAngle = DoomAngle;
	}
}

void ADoomMonster::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	if (Def && (Def->Flags & EThingFlag::Float) && !IsDead())
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Flying); // a live flyer that was thrown down takes off again
	}
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
		UGameplayStatics::ApplyDamage(Other, ((DoomMonsterDetail::MonsterRand() % 8) + 1) * 3, nullptr, this, nullptr);
	}
	ChargeVelocity = FVector::ZeroVector;
	ChargeTics = 0;
	GetCharacterMovement()->StopMovementImmediately();
	StartState(EMonsterState::Chase);
}
