// ADoomMonster: -fast demons, and SMART mode's side (the AI controller, walking FDoomNav paths,
// holding fire when a friend is in the way); the controller and its Behavior Tree are in DoomAI.cpp.
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

bool ADoomMonster::IsFastDemon() const
{
	// G_InitNew: with -fast or on Nightmare the demons' run, attack and pain frames take half the time
	return DoomWorld && DoomWorld->IsFast() && Def && (Def->DoomEdNum == 3002 || Def->DoomEdNum == 58);
}

int32 ADoomMonster::RunTics() const
{
	return IsFastDemon() ? FMath::Max(1, Def->RunTics / 2) : Def->RunTics;
}

void ADoomMonster::SetSmartAI(bool bOn)
{
	bSmart = bOn;
	ClearSmartGoal();
	if (!HasAuthority() || !GetWorld() || IsActorBeingDestroyed())
	{
		return;
	}
	if (bOn == (Controller && Controller->IsA<ADoomAIController>()))
	{
		return;
	}
	if (AController* Old = Controller)
	{
		Old->UnPossess();
		Old->Destroy();
	}
	if (!bOn)
	{
		return; // CLASSIC: no controller at all
	}
	AIControllerClass = ADoomAIController::StaticClass();
	SpawnDefaultController();
	if (IsDead())
	{
		if (const AAIController* C = Cast<AAIController>(Controller); C && C->GetBrainComponent())
		{
			C->GetBrainComponent()->StopLogic(TEXT("dead"));
		}
	}
}

bool ADoomMonster::SetSmartGoal(const FVector2f& Goal, float AcceptRadius)
{
	const FDoomMap* Map = DoomWorld ? DoomWorld->GetMap() : nullptr;
	if (!Map || !Def)
	{
		return false;
	}
	TArray<FVector2f> Path;
	if (!DoomWorld->GetNav().FindPath(*Map, DoomPos(), Goal, Def->Radius, Def->Height, (Def->Flags & EThingFlag::Float) != 0, Path))
	{
		SmartMove = ESmartMove::Failed;
		return false;
	}
	SmartPath = MoveTemp(Path);
	SmartIndex = 0;
	SmartGoal = Goal;
	SmartAccept = AcceptRadius;
	SmartMove = ESmartMove::Moving;
	SmartStuck = 0;
	StepStart = DoomPos();
	return true;
}

void ADoomMonster::ClearSmartGoal()
{
	SmartMove = ESmartMove::None;
	SmartPath.Reset();
	SmartIndex = 0;
	SmartDir = FVector2f::ZeroVector;
}

void ADoomMonster::SmartStep()
{
	const FVector2f Pos = DoomPos();
	const float Moved = FVector2f::Distance(Pos, StepStart);
	StepStart = Pos;
	const float Reach = FMath::Max(Def->Radius, 16.f);
	while (SmartIndex < SmartPath.Num() - 1 && FVector2f::Distance(Pos, SmartPath[SmartIndex]) < Reach)
	{
		++SmartIndex;
	}
	if (!SmartPath.IsValidIndex(SmartIndex) || FVector2f::Distance(Pos, SmartGoal) < SmartAccept)
	{
		SmartMove = ESmartMove::Arrived;
		SmartDir = FVector2f::ZeroVector;
		MoveDir = DoomMonsterDetail::DirNone;
		return;
	}
	SmartDir = (SmartPath[SmartIndex] - Pos).GetSafeNormal();
	if (SmartDir.IsNearlyZero())
	{
		return;
	}
	// the nearest of Doom's eight directions: the monster turns to face it as in A_Chase
	MoveDir = (FMath::RoundToInt(FMath::RadiansToDegrees(FMath::Atan2(SmartDir.Y, SmartDir.X)) / 45.f) + 8) % 8;
	if (Moved < Def->Speed * 0.25f)
	{
		// blocked: maybe a door to open; after a while the tree thinks again
		DoomWorld->TryMonsterUse(this, Pos, Pos + SmartDir * (Def->Radius + 24.f));
		if (++SmartStuck > 16)
		{
			SmartMove = ESmartMove::Failed;
			SmartDir = FVector2f::ZeroVector;
		}
	}
	else
	{
		SmartStuck = 0;
	}
}

bool ADoomMonster::FriendInLineOfFire() const
{
	if (!Target.IsValid())
	{
		return false;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomFriendlyFire), false, this);
	Q.AddIgnoredActor(Target.Get());
	FHitResult Hit;
	if (!GetWorld()->LineTraceSingleByObjectType(Hit, EyeLocation(), Target->GetActorLocation(), FCollisionObjectQueryParams(ECC_DoomMonster), Q))
	{
		return false;
	}
	const ADoomMonster* M = Cast<ADoomMonster>(Hit.GetActor());
	return M && !M->IsDead();
}
