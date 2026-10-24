// ADoomMonster: attacks (hitscan, missiles, melee, the special ones), taking damage, dying, the pain
// elemental's souls and corpses.
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
	if (!Def->AttackSound.IsEmpty())
	{
		DoomWorld->PlaySound(Def->AttackSound, &From);
	}
	for (int32 i = 0; i < Pellets; ++i)
	{
		const float Yaw = (DoomMonsterDetail::MonsterRand() - DoomMonsterDetail::MonsterRand()) / 255.f * SpreadDeg;
		const FVector Dir = Base.RotateAngleAxis(Yaw, FVector::UpVector);
		FHitResult Hit;
		const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, From, From + Dir * Range, ECC_Visibility, Q);
		// a monster's bullet can open the GR door 46 (P_ShootSpecialLine); ActivateSpecial filters the rest
		DoomWorld->CheckShootLines(this, Doom::ToDoom2D(From), Doom::ToDoom2D(bHit ? Hit.ImpactPoint + Dir * 4.f * Doom::Scale : From + Dir * Range));
		if (!bHit)
		{
			continue;
		}
		const int32 Damage = ((DoomMonsterDetail::MonsterRand() % 5) + 1) * DamageMul;
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
			DoomMonsterDetail::Is(Def, TEXT("Revenant")) ? Target.Get() : nullptr);
	};
	auto MeleeHit = [&](int32 Die)
	{
		if (CheckMeleeRange())
		{
			const int32 Damage = ((DoomMonsterDetail::MonsterRand() % Die) + 1) * Def->MeleeDamage;
			UGameplayStatics::ApplyDamage(Target.Get(), Damage, nullptr, this, nullptr);
			const FVector L = GetActorLocation();
			DoomWorld->PlaySound(DoomMonsterDetail::Is(Def, TEXT("Imp")) ? TEXT("CLAW") : DoomMonsterDetail::Is(Def, TEXT("Revenant")) ? TEXT("SKEPCH") : TEXT(""), &L);
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
	case EDoomAttack::Melee: MeleeHit(DoomMonsterDetail::Is(Def, TEXT("Demon")) || DoomMonsterDetail::Is(Def, TEXT("Spectre")) ? 10 : 8); break;
	case EDoomAttack::MeleeOrMissile:
		if (bMelee || CheckMeleeRange())
		{
			MeleeHit(DoomMonsterDetail::Is(Def, TEXT("Cacodemon")) ? 6 : 8);
		}
		else
		{
			Missile(*Def->Projectile, 0);
		}
		break;
	case EDoomAttack::Missile: Missile(*Def->Projectile, 0); break;
	case EDoomAttack::Cyber: Missile(TEXT("Rocket"), 0); break;
	case EDoomAttack::Revenant:
		if (bMelee) MeleeHit(10);
		else Missile(TEXT("Tracer"), 0);
		break;
	case EDoomAttack::Mancubus:
	{
		// A_FatAttack1/2/3 (the H frames of GHIGHIGH): one shot straight at the target with one 11.25
		// degrees to one side, then 22.5 to the other, then a pair 5.6 degrees either side
		static const float Volleys[3][2] = { { 0.f, 11.25f }, { 0.f, -22.5f }, { -5.625f, 5.625f } };
		const float* V = Volleys[FMath::Clamp(Sprite->FrameIndex() / 3, 0, 2)];
		Missile(TEXT("FatShot"), V[0]);
		Missile(TEXT("FatShot"), V[1]);
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
		ShootSkull(Doom::YawToAngle(FVector(ToTarget.X, ToTarget.Y, 0).Rotation().Yaw), Target.Get());
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
	if ((Def->Flags & EThingFlag::Boss) && DamageEvent.IsOfType(FRadialDamageEvent::ClassID) && !DoomMonsterDetail::Is(Def, TEXT("BossBrain")))
	{
		return 0.f; // cyberdemon and mastermind shrug off splash
	}
	AActor* Source = UDoomWorld::DamageSource(DamageCauser);
	Health -= FMath::RoundToInt(Damage);

	// knockback (P_DamageMobj thrust): away from the inflictor (missile, blast or shooter),
	// damage * 12.5 / mass units per tic; no push from a player's chainsaw
	const ADoomPlayer* SawPlayer = Cast<ADoomPlayer>(Source);
	if (DamageCauser && !(Def->Flags & EThingFlag::Hanging) && !(SawPlayer && SawPlayer->Inv().Ready == EDoomWeapon::Chainsaw))
	{
		const FVector From = DamageEvent.IsOfType(FRadialDamageEvent::ClassID)
			? static_cast<const FRadialDamageEvent&>(DamageEvent).Origin : DamageCauser->GetActorLocation();
		const FVector Push = (GetActorLocation() - From).GetSafeNormal2D() * (Damage * 12.5f / MonsterMass(Def) * Doom::TicRate * Doom::Scale);
		// added to the velocity rather than launched: launching switches to falling, which drops flyers and
		// throws walkers off ledges, where Doom's dropoff rule (P_TryMove) stops a pushed monster at the edge
		GetCharacterMovement()->Velocity += Push;
	}

	if (Health <= 0)
	{
		Die(Source, Health < -Def->Health);
		return Damage;
	}
	if (DoomMonsterDetail::MonsterRand() < Def->PainChance && !Def->Pain.IsEmpty() && State != EMonsterState::Charging)
	{
		bJustHit = true;
		StartState(EMonsterState::Pain);
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->PainSound, &L);
	}
	ReactionTime = 0;
	// infighting: hit by another monster -> go after it; an arch-vile's victims never fight back, and
	// anyone already chasing an arch-vile switches regardless of the threshold (vanilla)
	const ADoomMonster* SourceMonster = Cast<ADoomMonster>(Source);
	const bool bSourceVile = SourceMonster && SourceMonster->Def && DoomMonsterDetail::Is(SourceMonster->Def, TEXT("ArchVile"));
	const ADoomMonster* TargetMonster = Cast<ADoomMonster>(Target.Get());
	const bool bTargetVile = TargetMonster && TargetMonster->Def && DoomMonsterDetail::Is(TargetMonster->Def, TEXT("ArchVile"));
	if (Source && Source != this && (Threshold <= 0 || bTargetVile) && !bSourceVile)
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
	if (bSmart)
	{
		ClearSmartGoal();
		if (const AAIController* C = Cast<AAIController>(Controller); C && C->GetBrainComponent())
		{
			C->GetBrainComponent()->StopLogic(TEXT("dead"));
		}
	}
	LastTarget = Target; // a dying pain elemental's souls go after whoever it was fighting
	Target.Reset();
	MoveDir = DoomMonsterDetail::DirNone;
	ChargeVelocity = FVector::ZeroVector;
	MakeCorpse();
	UCapsuleComponent* Cap = GetCapsuleComponent();
	const bool bXDeath = bGib && !Def->XDeath.IsEmpty();
	Sprite->PlaySequence(bXDeath ? Def->XDeath : Def->Death, 5, false, (Def->Flags & EThingFlag::FullBright) != 0);
	Sprite->SetFuzz((Def->Flags & EThingFlag::Shadow) != 0);
	const FVector L = GetActorLocation();
	DoomWorld->PlaySound(bXDeath ? TEXT("SLOP") : Def->DeathSound, &L);
	if (Def->Flags & EThingFlag::CountKill)
	{
		DoomWorld->CreditKill(Killer);
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
	if (DoomMonsterDetail::Is(Def, TEXT("PainElemental")))
	{
		// A_PainDie: three souls at 90, 180 and 270 degrees, by the same rules as its attack
		for (int32 i = 0; i < 3; ++i)
		{
			ShootSkull(DoomAngle + 90.f * (i + 1), LastTarget.Get());
		}
	}
	DoomWorld->OnMonsterDied(this);
}

void ADoomMonster::ShootSkull(float Angle, AActor* SoulTarget)
{
	// A_PainShootSkull: no more than 20 lost souls on the map; the soul appears just past both bodies'
	// edges (4 + 3/2 of the radii), only if the way there is clear, and charges at once
	int32 Souls = 0;
	for (const TWeakObjectPtr<ADoomMonster>& M : DoomWorld->GetMonsters())
	{
		if (M.IsValid() && DoomMonsterDetail::Is(M->Def, TEXT("LostSoul")) && !M->IsDead()) ++Souls;
	}
	if (Souls > 20)
	{
		return;
	}
	const FDoomThingDef* SoulDef = DoomInfo::FindThing(3006);
	const float SoulRadius = SoulDef ? SoulDef->Radius : 16.f;
	const float Prestep = 4.f + 1.5f * (Def->Radius + SoulRadius);
	const float Rad = FMath::DegreesToRadians(Angle);
	const FVector Dir = Doom::ToUE(FMath::Cos(Rad), FMath::Sin(Rad), 0).GetSafeNormal();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Feet = GetActorLocation() - FVector(0, 0, Half) + Dir * Prestep * Doom::Scale + FVector(0, 0, 8.f * Doom::Scale);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomPainSpawn), false, this);
	const FVector Body(0, 0, (SoulDef ? SoulDef->Height * 0.5f : 28.f) * Doom::Scale);
	if (GetWorld()->SweepTestByChannel(GetActorLocation(), Feet + Body, FQuat::Identity, ECC_DoomMonster,
		FCollisionShape::MakeSphere(SoulRadius * 0.9f * Doom::Scale), Q))
	{
		return; // vanilla spawns it and kills it at once; nothing is left either way
	}
	if (ADoomMonster* Soul = DoomWorld->SpawnMonster(3006, Feet, Angle))
	{
		Soul->Target = SoulTarget;
		if (SoulTarget)
		{
			Soul->StartState(EMonsterState::Missile);
		}
	}
}

void ADoomMonster::MakeCorpse()
{
	Sprite->bSleepWhenDone = true; // once the death frames are over (UDoomSpriteComponent)
	if (HasAuthority())
	{
		bNetCorpse = true;
	}
	UCapsuleComponent* Cap = GetCapsuleComponent();
	// corpses keep resting on the world but stop blocking bodies and bullets
	Cap->SetCollisionResponseToAllChannels(ECR_Ignore);
	Cap->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Cap->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	if (HitBox)
	{
		HitBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (Def->Flags & EThingFlag::Float)
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Falling); // flyers drop
	}
	if (Light)
	{
		Light->SetVisibility(false);
	}
}
