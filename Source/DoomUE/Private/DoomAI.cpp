// SMART monsters: the AI controller, the Behavior Tree's nodes, the EQS pieces and FDoomNav
// (see DoomAI.h).
#include "DoomAI.h"
#include "DoomMonster.h"
#include "DoomPlayer.h"
#include "DoomWorld.h"
#include "DoomMap.h"
#include "DoomInfo.h"
#include "DoomUE.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Point.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_Actor.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Engine/World.h"

// ------------------------------------------------------------------------------------------------
// FDoomNav
// ------------------------------------------------------------------------------------------------

void FDoomNav::Build(const FDoomMap& Map)
{
	Portals.Reset();
	Adjacent.Reset();
	Adjacent.SetNum(Map.Subsectors.Num());
	// subsector polygon edges, binned by the line they lie on; neighbours share an edge running the
	// other way
	struct FEdge { int32 Ss; FVector2f A, B; float T0, T1; bool bFlip; };
	TMap<FIntPoint, TArray<int32>> Bins;
	TArray<FEdge> Edges;
	for (int32 s = 0; s < Map.Subsectors.Num(); ++s)
	{
		const TArray<FVector2f>& Poly = Map.Subsectors[s].Polygon;
		for (int32 v = 0; v < Poly.Num(); ++v)
		{
			const FVector2f A = Poly[v], B = Poly[(v + 1) % Poly.Num()];
			FVector2f D = B - A;
			const float Len = D.Size();
			if (Len < 1.f) continue;
			D /= Len;
			// one direction per line: the edge's own, or flipped
			const bool bFlip = D.X < -1e-4f || (FMath::Abs(D.X) <= 1e-4f && D.Y < 0);
			const FVector2f C = bFlip ? -D : D;
			const float Offset = C.X * A.Y - C.Y * A.X;
			const float TA = FVector2f::DotProduct(A, C), TB = FVector2f::DotProduct(B, C);
			const int32 Index = Edges.Add({ s, A, B, FMath::Min(TA, TB), FMath::Max(TA, TB), bFlip });
			const FIntPoint Key(FMath::RoundToInt(FMath::Atan2(C.Y, C.X) * 2000.f), FMath::RoundToInt(Offset));
			Bins.FindOrAdd(Key).Add(Index);
		}
	}
	TSet<uint64> Seen;
	for (int32 e = 0; e < Edges.Num(); ++e)
	{
		const FEdge& E = Edges[e];
		FVector2f D = (E.B - E.A).GetSafeNormal();
		const FVector2f C = E.bFlip ? -D : D;
		const float Offset = C.X * E.A.Y - C.Y * E.A.X;
		const int32 AngleKey = FMath::RoundToInt(FMath::Atan2(C.Y, C.X) * 2000.f);
		for (int32 da = -1; da <= 1; ++da)
		{
			for (int32 dofs = -1; dofs <= 1; ++dofs)
			{
				const TArray<int32>* Bin = Bins.Find(FIntPoint(AngleKey + da, FMath::RoundToInt(Offset) + dofs));
				if (!Bin) continue;
				for (int32 o : *Bin)
				{
					const FEdge& F = Edges[o];
					if (o <= e || F.Ss == E.Ss || F.bFlip == E.bFlip) continue;
					// the same line (to within half a unit) and an overlap of at least a unit
					const FVector2f FC = F.bFlip ? -(F.B - F.A).GetSafeNormal() : (F.B - F.A).GetSafeNormal();
					if (FMath::Abs((FC.X * F.A.Y - FC.Y * F.A.X) - Offset) > 0.5f) continue;
					const float T0 = FMath::Max(E.T0, F.T0), T1 = FMath::Min(E.T1, F.T1);
					if (T1 - T0 < 1.f) continue;
					const uint64 PairKey = ((uint64)FMath::Min(E.Ss, F.Ss) << 32) | (uint32)FMath::Max(E.Ss, F.Ss);
					FPortal P;
					P.A = E.Ss;
					P.B = F.Ss;
					// the overlap's ends, back on the line
					// a point at t along C and s across has Cross(C, P) = s: the line's offset
					const FVector2f N(-C.Y, C.X);
					P.P0 = C * T0 + N * Offset;
					P.P1 = C * T1 + N * Offset;
					const int32 SecA = Map.Subsectors[P.A].Sector, SecB = Map.Subsectors[P.B].Sector;
					if (SecA != SecB && Map.Sectors.IsValidIndex(SecA))
					{
						// the two-sided line it lies on
						const FVector2f Mid = (P.P0 + P.P1) * 0.5f;
						for (int32 L : Map.Sectors[SecA].Lines)
						{
							const FDoomLine& Ln = Map.Lines[L];
							if (Ln.BackSector == INDEX_NONE || (Ln.FrontSector != SecB && Ln.BackSector != SecB)) continue;
							if (Map.DistanceToLine(Mid, L) < 1.f) { P.Line = L; break; }
						}
						if (P.Line == INDEX_NONE) continue; // a wall between them (one-sided lines are no way through)
					}
					if (Seen.Contains(PairKey) && SecA == SecB) continue;
					Seen.Add(PairKey);
					const int32 Pi = Portals.Add(P);
					Adjacent[P.A].Add(Pi);
					Adjacent[P.B].Add(Pi);
				}
			}
		}
	}
}

bool FDoomNav::CanCross(const FDoomMap& Map, const FPortal& P, int32 FromSs, float Radius, float Height, bool bFloat) const
{
	const int32 ToSs = P.A == FromSs ? P.B : P.A;
	const int32 SecA = Map.Subsectors[FromSs].Sector, SecB = Map.Subsectors[ToSs].Sector;
	if (SecA == SecB)
	{
		return true;
	}
	if (P.Line == INDEX_NONE || !Map.Sectors.IsValidIndex(SecA) || !Map.Sectors.IsValidIndex(SecB))
	{
		return false;
	}
	const FDoomLine& L = Map.Lines[P.Line];
	if (L.Flags & (DoomLineFlags::Blocking | DoomLineFlags::BlockMonsters))
	{
		return false;
	}
	const FDoomSector& A = Map.Sectors[SecA];
	const FDoomSector& B = Map.Sectors[SecB];
	// a door a monster opens by walking into it (DR, no key): into it, or out of it, open or shut
	const bool bDoor = L.Special == 1;
	if (!bDoor && FMath::Min(A.Ceiling, B.Ceiling) - FMath::Max(A.Floor, B.Floor) < Height)
	{
		return false;
	}
	if (!bFloat && FMath::Abs(B.Floor - A.Floor) > 24.f)
	{
		return false; // steps up and drop-offs over 24 (P_TryMove)
	}
	return true;
}

bool FDoomNav::FindPath(const FDoomMap& Map, const FVector2f& From, const FVector2f& To, float Radius, float Height, bool bFloat,
	TArray<FVector2f>& Out, int32 MaxNodes) const
{
	Out.Reset();
	const int32 Start = Map.SubsectorAt(From), Goal = Map.SubsectorAt(To);
	if (!Map.Subsectors.IsValidIndex(Start) || !Map.Subsectors.IsValidIndex(Goal) || Adjacent.Num() != Map.Subsectors.Num())
	{
		return false;
	}
	if (Start == Goal)
	{
		Out.Add(To);
		return true;
	}
	struct FOpen { float F; int32 Ss; };
	TArray<float> G;
	G.Init(TNumericLimits<float>::Max(), Map.Subsectors.Num());
	TArray<int32> Via;      // portal used to enter each subsector
	Via.Init(INDEX_NONE, Map.Subsectors.Num());
	TArray<FVector2f> At;   // where the walk entered each subsector
	At.SetNum(Map.Subsectors.Num());
	TArray<FOpen> Open;
	auto Less = [](const FOpen& X, const FOpen& Y) { return X.F < Y.F; };
	G[Start] = 0;
	At[Start] = From;
	Open.HeapPush({ FVector2f::Distance(From, To), Start }, Less);
	int32 Expanded = 0;
	bool bFound = false;
	while (Open.Num() && Expanded < MaxNodes)
	{
		FOpen Cur;
		Open.HeapPop(Cur, Less);
		if (Cur.Ss == Goal) { bFound = true; break; }
		if (Cur.F - FVector2f::Distance(At[Cur.Ss], To) > G[Cur.Ss] + 0.01f) continue; // stale
		++Expanded;
		for (int32 Pi : Adjacent[Cur.Ss])
		{
			const FPortal& P = Portals[Pi];
			if (!CanCross(Map, P, Cur.Ss, Radius, Height, bFloat)) continue;
			const int32 Next = P.A == Cur.Ss ? P.B : P.A;
			// cross at the point of the portal nearest to where we are, kept a radius from its ends
			const FVector2f D = P.P1 - P.P0;
			const float Len = D.Size();
			const float Margin = FMath::Min(Radius + 2.f, Len * 0.5f);
			const float T = FMath::Clamp(FVector2f::DotProduct(At[Cur.Ss] - P.P0, D / FMath::Max(Len, 0.001f)), Margin, Len - Margin);
			const FVector2f Cross = P.P0 + D / FMath::Max(Len, 0.001f) * T;
			const float NewG = G[Cur.Ss] + FVector2f::Distance(At[Cur.Ss], Cross);
			if (NewG >= G[Next]) continue;
			G[Next] = NewG;
			Via[Next] = Pi;
			At[Next] = Cross;
			Open.HeapPush({ NewG + FVector2f::Distance(Cross, To), Next }, Less);
		}
	}
	if (!bFound)
	{
		return false;
	}
	for (int32 S = Goal; S != Start && Via[S] != INDEX_NONE; )
	{
		Out.Insert(At[S], 0);
		const FPortal& P = Portals[Via[S]];
		S = P.A == S ? P.B : P.A;
	}
	Out.Add(To);
	return true;
}

int32 FDoomNav::CountReachable(const FDoomMap& Map, const FVector2f& P, bool bRules, float Radius, float Height) const
{
	const int32 Start = Map.SubsectorAt(P);
	if (!Map.Subsectors.IsValidIndex(Start) || Adjacent.Num() != Map.Subsectors.Num()) return 0;
	TArray<uint8> Seen;
	Seen.Init(0, Map.Subsectors.Num());
	TArray<int32> Open = { Start };
	Seen[Start] = 1;
	int32 Count = 1;
	while (Open.Num())
	{
		const int32 S = Open.Pop(EAllowShrinking::No);
		for (int32 Pi : Adjacent[S])
		{
			const FPortal& Po = Portals[Pi];
			const int32 N = Po.A == S ? Po.B : Po.A;
			if (Seen[N] || (bRules && !CanCross(Map, Po, S, Radius, Height, false))) continue;
			Seen[N] = 1;
			++Count;
			Open.Add(N);
		}
	}
	return Count;
}

bool FDoomNav::IsClear(const FDoomMap& Map, const FVector2f& P, float Radius)
{
	const int32 Ss = Map.SubsectorAt(P);
	if (!Map.Subsectors.IsValidIndex(Ss) || !Map.Sectors.IsValidIndex(Map.Subsectors[Ss].Sector))
	{
		return false;
	}
	// inside the subsector's polygon (the BSP names a subsector for points outside the map too)
	const TArray<FVector2f>& Poly = Map.Subsectors[Ss].Polygon;
	int32 Sign = 0;
	for (int32 v = 0; v < Poly.Num(); ++v)
	{
		const FVector2f E = Poly[(v + 1) % Poly.Num()] - Poly[v], Q = P - Poly[v];
		const float C = E.X * Q.Y - E.Y * Q.X;
		const int32 Sv = C > 0.01f ? 1 : (C < -0.01f ? -1 : 0);
		if (Sv && Sign && Sv != Sign) return false;
		if (Sv) Sign = Sv;
	}
	for (int32 L : Map.Sectors[Map.Subsectors[Ss].Sector].Lines)
	{
		const FDoomLine& Ln = Map.Lines[L];
		const bool bWall = Ln.BackSector == INDEX_NONE || (Ln.Flags & (DoomLineFlags::Blocking | DoomLineFlags::BlockMonsters));
		if (bWall && Map.DistanceToLine(P, L) < Radius) return false;
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
// The controller
// ------------------------------------------------------------------------------------------------

ADoomAIController::ADoomAIController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Senses = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("Senses"));
	UAISenseConfig_Sight* Sight = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("Sight"));
	Sight->SightRadius = 2048.f * Doom::Scale;
	Sight->LoseSightRadius = 2300.f * Doom::Scale;
	Sight->PeripheralVisionAngleDegrees = 90.f; // Doom's monsters see the half in front of them
	Sight->DetectionByAffiliation.bDetectEnemies = true;
	Sight->DetectionByAffiliation.bDetectNeutrals = true;
	Sight->DetectionByAffiliation.bDetectFriendlies = true;
	Sight->SetMaxAge(5.f);
	Senses->ConfigureSense(*Sight);
	UAISenseConfig_Hearing* Hearing = CreateDefaultSubobject<UAISenseConfig_Hearing>(TEXT("Hearing"));
	Hearing->HearingRange = 1200.f * Doom::Scale;
	Hearing->DetectionByAffiliation.bDetectEnemies = true;
	Hearing->DetectionByAffiliation.bDetectNeutrals = true;
	Hearing->DetectionByAffiliation.bDetectFriendlies = true;
	Hearing->SetMaxAge(3.f);
	Senses->ConfigureSense(*Hearing);
	Senses->SetDominantSense(UAISense_Sight::StaticClass());
	SetPerceptionComponent(*Senses);
	bStartAILogicOnPossess = true;
}

ADoomMonster* ADoomAIController::GetMonster() const
{
	return Cast<ADoomMonster>(GetPawn());
}

void ADoomAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	Senses->OnTargetPerceptionUpdated.AddUniqueDynamic(this, &ADoomAIController::OnPerceived);
	if (UBehaviorTree* Tree = DoomAI::Get().Tree)
	{
		RunBehaviorTree(Tree);
	}
}

void ADoomAIController::OnUnPossess()
{
	if (BrainComponent)
	{
		BrainComponent->StopLogic(TEXT("unpossessed"));
	}
	Super::OnUnPossess();
}

void ADoomAIController::OnPerceived(AActor* Actor, FAIStimulus Stimulus)
{
	ADoomMonster* M = GetMonster();
	ADoomPlayer* P = Cast<ADoomPlayer>(Actor);
	if (!M || !P || P->IsDead() || M->IsDead())
	{
		return;
	}
	UBlackboardComponent* BB = GetBlackboardComponent();
	const bool bHeard = Stimulus.Type == UAISense::GetSenseID<UAISense_Hearing>();
	if (!Stimulus.WasSuccessfullySensed())
	{
		// out of sight: remember where it went
		if (BB && M->Target.Get() == P) BB->SetValueAsVector(DoomAIKeys::LastKnown, Stimulus.StimulusLocation);
		return;
	}
	++NumSensed;
	if (M->State == EMonsterState::Idle)
	{
		// sight wakes a monster (Doom's own noise flood still wakes it by sound)
		if (!bHeard) M->WakeUp(P, false);
		return;
	}
	if (BB && (M->Target.Get() == P || !M->Target.IsValid()))
	{
		// an awake monster follows the sound of the fight, or the last sight of it
		BB->SetValueAsVector(DoomAIKeys::LastKnown, Stimulus.StimulusLocation);
	}
}

// ------------------------------------------------------------------------------------------------
// Behavior Tree nodes
// ------------------------------------------------------------------------------------------------

namespace
{
	ADoomMonster* MonsterOf(const UBehaviorTreeComponent& OwnerComp)
	{
		const AAIController* C = Cast<AAIController>(OwnerComp.GetOwner());
		return C ? Cast<ADoomMonster>(C->GetPawn()) : nullptr;
	}
	bool IsAwake(const ADoomMonster* M)
	{
		return M && !M->IsDead() && M->State != EMonsterState::Idle;
	}
}

UBTService_DoomTrackTarget::UBTService_DoomTrackTarget()
{
	NodeName = TEXT("Track Target");
	Interval = 0.2f;
	RandomDeviation = 0.05f;
	bNotifyTick = true;
}

void UBTService_DoomTrackTarget::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);
	ADoomMonster* M = MonsterOf(OwnerComp);
	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!M || !BB)
	{
		return;
	}
	AActor* T = M->IsDead() ? nullptr : M->Target.Get();
	BB->SetValueAsObject(DoomAIKeys::Target, T);
	const bool bSee = T && M->GetDoomWorld() && M->GetDoomWorld()->CheckSight(M, T);
	BB->SetValueAsBool(DoomAIKeys::CanSee, bSee);
	if (bSee)
	{
		BB->SetValueAsVector(DoomAIKeys::LastKnown, T->GetActorLocation());
	}
}

UBTDecorator_DoomCondition::UBTDecorator_DoomCondition()
{
	NodeName = TEXT("Doom Condition");
}

bool UBTDecorator_DoomCondition::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const ADoomMonster* M = MonsterOf(OwnerComp);
	const UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	bool bValue = false;
	if (M && BB && M->Def)
	{
		switch (Condition)
		{
		case EDoomAICondition::CanSeeTarget: bValue = BB->GetValueAsBool(DoomAIKeys::CanSee); break;
		case EDoomAICondition::HasTarget: bValue = BB->GetValueAsObject(DoomAIKeys::Target) != nullptr; break;
		case EDoomAICondition::KnowsWhere: bValue = BB->IsVectorValueSet(DoomAIKeys::LastKnown); break;
		case EDoomAICondition::MeleeOnly: bValue = DoomAI::IsMeleeOnly(M->Def); break;
		case EDoomAICondition::Hurt: bValue = M->Health * 3 < M->Def->Health; break;
		}
	}
	return bValue != bNot;
}

FString UBTDecorator_DoomCondition::GetStaticDescription() const
{
	const FString Name = StaticEnum<EDoomAICondition>()->GetNameStringByValue((int64)Condition);
	return bNot ? TEXT("not ") + Name : Name;
}

UBTTask_DoomFindSpot::UBTTask_DoomFindSpot()
{
	NodeName = TEXT("Find Spot");
}

EBTNodeResult::Type UBTTask_DoomFindSpot::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	ADoomMonster* M = MonsterOf(OwnerComp);
	if (!IsAwake(M) || !Query)
	{
		return EBTNodeResult::Failed;
	}
	TWeakObjectPtr<UBehaviorTreeComponent> Comp(&OwnerComp);
	const FName Key = ResultKey;
	FEnvQueryRequest Request(Query, M);
	int32& Id = *reinterpret_cast<int32*>(NodeMemory);
	Id = Request.Execute(EEnvQueryRunMode::SingleResult, FQueryFinishedSignature::CreateLambda([this, Comp, Key](TSharedPtr<FEnvQueryResult> Result)
	{
		if (!Comp.IsValid())
		{
			return;
		}
		const bool bOk = Result.IsValid() && Result->IsSuccessful() && Result->Items.Num() > 0;
		if (bOk)
		{
			Comp->GetBlackboardComponent()->SetValueAsVector(Key, Result->GetItemAsLocation(0));
		}
		FinishLatentTask(*Comp, bOk ? EBTNodeResult::Succeeded : EBTNodeResult::Failed);
	}));
	return Id != INDEX_NONE ? EBTNodeResult::InProgress : EBTNodeResult::Failed;
}

EBTNodeResult::Type UBTTask_DoomFindSpot::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	if (UEnvQueryManager* EQS = UEnvQueryManager::GetCurrent(OwnerComp.GetWorld()))
	{
		EQS->AbortQuery(*reinterpret_cast<int32*>(NodeMemory));
	}
	return EBTNodeResult::Aborted;
}

FString UBTTask_DoomFindSpot::GetStaticDescription() const
{
	return FString::Printf(TEXT("%s -> %s"), Query ? *Query->GetName() : TEXT("(no query)"), *ResultKey.ToString());
}

UBTTask_DoomMoveTo::UBTTask_DoomMoveTo()
{
	NodeName = TEXT("Move To");
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

bool UBTTask_DoomMoveTo::GoalOf(UBehaviorTreeComponent& OwnerComp, FVector2f& Out) const
{
	const UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB)
	{
		return false;
	}
	if (GoalKey == DoomAIKeys::Target)
	{
		const AActor* T = Cast<AActor>(BB->GetValueAsObject(DoomAIKeys::Target));
		if (!T) return false;
		Out = Doom::ToDoom2D(T->GetActorLocation());
		return true;
	}
	if (!BB->IsVectorValueSet(GoalKey))
	{
		return false;
	}
	Out = Doom::ToDoom2D(BB->GetValueAsVector(GoalKey));
	return true;
}

EBTNodeResult::Type UBTTask_DoomMoveTo::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	ADoomMonster* M = MonsterOf(OwnerComp);
	FVector2f Goal;
	if (!IsAwake(M) || !GoalOf(OwnerComp, Goal))
	{
		return EBTNodeResult::Failed;
	}
	new (NodeMemory) FMemory();
	if (!M->SetSmartGoal(Goal, GoalKey == DoomAIKeys::Target ? FMath::Max(AcceptRadius, 64.f) : AcceptRadius))
	{
		// nowhere to go from here: forget a last known position nobody can reach
		if (GoalKey == DoomAIKeys::LastKnown) OwnerComp.GetBlackboardComponent()->ClearValue(DoomAIKeys::LastKnown);
		return EBTNodeResult::Failed;
	}
	return EBTNodeResult::InProgress;
}

void UBTTask_DoomMoveTo::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FMemory& Mem = *reinterpret_cast<FMemory*>(NodeMemory);
	ADoomMonster* M = MonsterOf(OwnerComp);
	if (!M || M->IsDead())
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}
	const float Before = Mem.Elapsed;
	Mem.Elapsed += DeltaSeconds;
	// a moving target: plan again twice a second
	FVector2f Goal;
	if (GoalKey == DoomAIKeys::Target && FMath::FloorToInt(Before * 2.f) != FMath::FloorToInt(Mem.Elapsed * 2.f) && GoalOf(OwnerComp, Goal))
	{
		M->SetSmartGoal(Goal, FMath::Max(AcceptRadius, 64.f));
	}
	switch (M->GetSmartMove())
	{
	case ADoomMonster::ESmartMove::Arrived:
		if (bClearLastKnownOnArrival) OwnerComp.GetBlackboardComponent()->ClearValue(DoomAIKeys::LastKnown);
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	case ADoomMonster::ESmartMove::Failed:
		if (bClearLastKnownOnArrival) OwnerComp.GetBlackboardComponent()->ClearValue(DoomAIKeys::LastKnown);
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	default:
		break;
	}
	if (Mem.Elapsed >= MaxSeconds)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded); // think again
	}
}

void UBTTask_DoomMoveTo::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

FString UBTTask_DoomMoveTo::GetStaticDescription() const
{
	return FString::Printf(TEXT("to %s (within %.0f, up to %.1f s)"), *GoalKey.ToString(), AcceptRadius, MaxSeconds);
}

// ------------------------------------------------------------------------------------------------
// EQS
// ------------------------------------------------------------------------------------------------

namespace
{
	ADoomMonster* QueryMonster(const FEnvQueryInstance& QueryInstance)
	{
		return Cast<ADoomMonster>(QueryInstance.Owner.Get());
	}
	const FDoomMap* QueryMap(const ADoomMonster* M)
	{
		return M && M->GetDoomWorld() ? M->GetDoomWorld()->GetMap() : nullptr;
	}
}

void UEnvQueryContext_DoomTarget::ProvideContext(FEnvQueryInstance& QueryInstance, FEnvQueryContextData& ContextData) const
{
	if (const ADoomMonster* M = QueryMonster(QueryInstance))
	{
		if (AActor* T = M->Target.Get())
		{
			UEnvQueryItemType_Actor::SetContextHelper(ContextData, T);
		}
	}
}

UEnvQueryGenerator_DoomSpots::UEnvQueryGenerator_DoomSpots()
{
	ItemType = UEnvQueryItemType_Point::StaticClass();
}

void UEnvQueryGenerator_DoomSpots::GenerateItems(FEnvQueryInstance& QueryInstance) const
{
	const ADoomMonster* M = QueryMonster(QueryInstance);
	const FDoomMap* Map = QueryMap(M);
	if (!Map || !M->Def)
	{
		return;
	}
	const FVector2f Pos = M->DoomPos();
	const float Radius = M->Def->Radius + 4.f;
	// where it stands counts too: staying put is a choice
	QueryInstance.AddItemData<UEnvQueryItemType_Point>(M->GetActorLocation());
	const int32 NRings = FMath::Max(1, Rings);
	for (int32 r = 0; r < NRings; ++r)
	{
		const float Dist = NRings > 1 ? FMath::Lerp(MinRadius, MaxRadius, r / float(NRings - 1)) : MinRadius;
		for (int32 k = 0; k < PointsPerRing; ++k)
		{
			const float An = (k + (r & 1) * 0.5f) * 2.f * UE_PI / PointsPerRing;
			const FVector2f P = Pos + FVector2f(FMath::Cos(An), FMath::Sin(An)) * Dist;
			if (!FDoomNav::IsClear(*Map, P, Radius)) continue;
			const int32 Sec = Map->SectorAt(P);
			QueryInstance.AddItemData<UEnvQueryItemType_Point>(Doom::ToUE(P, Map->Sectors[Sec].Floor));
		}
	}
}

FText UEnvQueryGenerator_DoomSpots::GetDescriptionTitle() const
{
	return FText::FromString(FString::Printf(TEXT("Doom spots: %d rings of %d, %.0f to %.0f units"), Rings, PointsPerRing, MinRadius, MaxRadius));
}

UEnvQueryTest_DoomSight::UEnvQueryTest_DoomSight()
{
	Cost = EEnvTestCost::Medium;
	ValidItemType = UEnvQueryItemType_VectorBase::StaticClass();
	TestPurpose = EEnvTestPurpose::Filter;
	FilterType = EEnvTestFilterType::Match;
	BoolValue.DefaultValue = true;
	SetWorkOnFloatValues(false);
}

void UEnvQueryTest_DoomSight::RunTest(FEnvQueryInstance& QueryInstance) const
{
	const ADoomMonster* M = QueryMonster(QueryInstance);
	const AActor* T = M ? M->Target.Get() : nullptr;
	UObject* Owner = QueryInstance.Owner.Get();
	BoolValue.BindData(Owner, QueryInstance.QueryID);
	const bool bWanted = BoolValue.GetValue();
	if (!M || !T || !M->Def)
	{
		for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It) It.ForceItemState(EEnvItemStatus::Failed);
		return;
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomEqsSight), false, M);
	Q.AddIgnoredActor(T);
	const FVector Eye = T->GetActorLocation();
	const float EyeUp = M->Def->Height * 0.75f * Doom::Scale;
	for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
	{
		const FVector P = GetItemLocation(QueryInstance, It.GetIndex()) + FVector(0, 0, EyeUp);
		FHitResult Hit;
		const bool bSees = !M->GetWorld()->LineTraceSingleByChannel(Hit, P, Eye, ECC_Visibility, Q);
		It.SetScore(TestPurpose, FilterType, bSees, bWanted);
	}
}

FText UEnvQueryTest_DoomSight::GetDescriptionDetails() const
{
	return FText::FromString(BoolValue.DefaultValue ? TEXT("keep spots that see the target") : TEXT("keep spots hidden from the target"));
}

UEnvQueryTest_DoomRange::UEnvQueryTest_DoomRange()
{
	Cost = EEnvTestCost::Low;
	ValidItemType = UEnvQueryItemType_VectorBase::StaticClass();
	TestPurpose = EEnvTestPurpose::Score;
	ScoringEquation = EEnvTestScoreEquation::Linear;
	ScoringFactor.DefaultValue = 2.f;
}

void UEnvQueryTest_DoomRange::RunTest(FEnvQueryInstance& QueryInstance) const
{
	const ADoomMonster* M = QueryMonster(QueryInstance);
	const AActor* T = M ? M->Target.Get() : nullptr;
	const float Want = M ? DoomAI::PreferredRange(M->Def) : 0.f;
	for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
	{
		float Score = 0.f;
		if (T)
		{
			const float D = FVector2f::Distance(Doom::ToDoom2D(GetItemLocation(QueryInstance, It.GetIndex())), Doom::ToDoom2D(T->GetActorLocation()));
			Score = 1.f - FMath::Min(FMath::Abs(D - Want) / 384.f, 1.f);
		}
		It.SetScore(TestPurpose, FilterType, Score, 0.f, 1.f);
	}
}

UEnvQueryTest_DoomCrowd::UEnvQueryTest_DoomCrowd()
{
	Cost = EEnvTestCost::Low;
	ValidItemType = UEnvQueryItemType_VectorBase::StaticClass();
	TestPurpose = EEnvTestPurpose::Score;
	ScoringEquation = EEnvTestScoreEquation::Linear;
}

void UEnvQueryTest_DoomCrowd::RunTest(FEnvQueryInstance& QueryInstance) const
{
	const ADoomMonster* M = QueryMonster(QueryInstance);
	TArray<FVector2f> Others;
	if (M && M->GetDoomWorld())
	{
		for (const TWeakObjectPtr<ADoomMonster>& O : M->GetDoomWorld()->GetMonsters())
		{
			if (O.IsValid() && O.Get() != M && !O->IsDead() && O->State != EMonsterState::Idle)
			{
				Others.Add(O->DoomPos());
				if (O->GetSmartMove() == ADoomMonster::ESmartMove::Moving) Others.Add(O->GetSmartGoal());
			}
		}
	}
	for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
	{
		const FVector2f P = Doom::ToDoom2D(GetItemLocation(QueryInstance, It.GetIndex()));
		float Nearest = 160.f;
		for (const FVector2f& O : Others) Nearest = FMath::Min(Nearest, FVector2f::Distance(P, O));
		It.SetScore(TestPurpose, FilterType, Nearest / 160.f, 0.f, 1.f);
	}
}

UEnvQueryTest_DoomReachable::UEnvQueryTest_DoomReachable()
{
	Cost = EEnvTestCost::High;
	ValidItemType = UEnvQueryItemType_VectorBase::StaticClass();
	TestPurpose = EEnvTestPurpose::FilterAndScore;
	FilterType = EEnvTestFilterType::Minimum;
	FloatValueMin.DefaultValue = 0.05f;
	ScoringEquation = EEnvTestScoreEquation::Linear;
	ScoringFactor.DefaultValue = 0.5f;
}

void UEnvQueryTest_DoomReachable::RunTest(FEnvQueryInstance& QueryInstance) const
{
	const ADoomMonster* M = QueryMonster(QueryInstance);
	const FDoomMap* Map = QueryMap(M);
	const FDoomNav* Nav = M && M->GetDoomWorld() ? &M->GetDoomWorld()->GetNav() : nullptr;
	UObject* Owner = QueryInstance.Owner.Get();
	FloatValueMin.BindData(Owner, QueryInstance.QueryID);
	const float Min = FloatValueMin.GetValue();
	const bool bFloat = M && M->Def && (M->Def->Flags & EThingFlag::Float);
	TArray<FVector2f> Path;
	for (FEnvQueryInstance::ItemIterator It(this, QueryInstance); It; ++It)
	{
		float Score = 0.f;
		if (Map && Nav && M->Def)
		{
			const FVector2f From = M->DoomPos(), To = Doom::ToDoom2D(GetItemLocation(QueryInstance, It.GetIndex()));
			if (Nav->FindPath(*Map, From, To, M->Def->Radius, M->Def->Height, bFloat, Path, 600))
			{
				float Len = 0.f;
				FVector2f Prev = From;
				for (const FVector2f& W : Path) { Len += FVector2f::Distance(Prev, W); Prev = W; }
				Score = Len < 1.f ? 1.f : FMath::Clamp(FVector2f::Distance(From, To) / Len, 0.f, 1.f);
				Score = FMath::Max(Score, 0.06f); // reachable at all passes the filter
			}
		}
		It.SetScore(TestPurpose, FilterType, Score, Min, 1.f);
	}
}

// ------------------------------------------------------------------------------------------------
// The assets
// ------------------------------------------------------------------------------------------------

TArray<UObject*> FDoomAIAssets::All() const
{
	return { Blackboard, AttackSpot, Cover, Tree };
}

float DoomAI::PreferredRange(const FDoomThingDef* Def)
{
	if (!Def || IsMeleeOnly(Def))
	{
		return 0.f;
	}
	switch (Def->DoomEdNum)
	{
	case 3004: case 9: case 65: case 84: return 320.f;  // hitscanners: close enough to hit
	case 3001: case 66: return 384.f;                   // imps, revenants
	case 3005: case 71: return 448.f;                   // cacodemons, pain elementals
	default: return 512.f;                              // barons, knights, mancubi, arachnotrons...
	}
}

bool DoomAI::IsMeleeOnly(const FDoomThingDef* Def)
{
	return Def && (Def->DoomEdNum == 3006 || (Def->Attack.IsEmpty() && !Def->Melee.IsEmpty()));
}

void DoomAI::Build(TFunctionRef<UObject*(UClass*, FName)> New, FDoomAIAssets& Out)
{
	UBlackboardData* BB = CastChecked<UBlackboardData>(New(UBlackboardData::StaticClass(), TEXT("BB_DoomMonster")));
	auto AddKey = [BB](FName Name, UClass* Type)
	{
		FBlackboardEntry E;
		E.EntryName = Name;
		E.KeyType = NewObject<UBlackboardKeyType>(BB, Type);
		if (UBlackboardKeyType_Object* O = Cast<UBlackboardKeyType_Object>(E.KeyType)) O->BaseClass = AActor::StaticClass();
		BB->Keys.Add(E);
	};
	AddKey(DoomAIKeys::Target, UBlackboardKeyType_Object::StaticClass());
	AddKey(DoomAIKeys::LastKnown, UBlackboardKeyType_Vector::StaticClass());
	AddKey(DoomAIKeys::CanSee, UBlackboardKeyType_Bool::StaticClass());
	AddKey(DoomAIKeys::MoveGoal, UBlackboardKeyType_Vector::StaticClass());
	BB->UpdateParentKeys();
	BB->UpdateKeyIDs();

	auto MakeQuery = [&](FName Name, bool bSeeTarget, float MaxRadius)
	{
		UEnvQuery* Q = CastChecked<UEnvQuery>(New(UEnvQuery::StaticClass(), Name));
		UEnvQueryOption* Opt = NewObject<UEnvQueryOption>(Q);
		UEnvQueryGenerator_DoomSpots* Gen = NewObject<UEnvQueryGenerator_DoomSpots>(Opt);
		Gen->MaxRadius = MaxRadius;
		Opt->Generator = Gen;
		UEnvQueryTest_DoomSight* Sight = NewObject<UEnvQueryTest_DoomSight>(Opt);
		Sight->BoolValue.DefaultValue = bSeeTarget;
		Opt->Tests.Add(Sight);
		if (bSeeTarget) Opt->Tests.Add(NewObject<UEnvQueryTest_DoomRange>(Opt));
		Opt->Tests.Add(NewObject<UEnvQueryTest_DoomCrowd>(Opt));
		Opt->Tests.Add(NewObject<UEnvQueryTest_DoomReachable>(Opt));
		for (int32 i = 0; i < Opt->Tests.Num(); ++i) Opt->Tests[i]->TestOrder = i;
		Q->GetOptionsMutable().Add(Opt);
		return Q;
	};
	Out.AttackSpot = MakeQuery(TEXT("EQ_DoomAttackSpot"), true, 384.f);
	Out.Cover = MakeQuery(TEXT("EQ_DoomCover"), false, 320.f);

	UBehaviorTree* BT = CastChecked<UBehaviorTree>(New(UBehaviorTree::StaticClass(), TEXT("BT_DoomMonster")));
	BT->BlackboardAsset = BB;
	auto Cond = [BT](EDoomAICondition C, bool bNot = false)
	{
		UBTDecorator_DoomCondition* D = NewObject<UBTDecorator_DoomCondition>(BT);
		D->Condition = C;
		D->bNot = bNot;
		return D;
	};
	auto Move = [BT](FName Key, float Seconds, float Accept, bool bClear = false)
	{
		UBTTask_DoomMoveTo* T = NewObject<UBTTask_DoomMoveTo>(BT);
		T->GoalKey = Key;
		T->MaxSeconds = Seconds;
		T->AcceptRadius = Accept;
		T->bClearLastKnownOnArrival = bClear;
		return T;
	};
	auto Find = [BT](UEnvQuery* Q)
	{
		UBTTask_DoomFindSpot* T = NewObject<UBTTask_DoomFindSpot>(BT);
		T->Query = Q;
		return T;
	};
	auto Wait = [BT](float Seconds)
	{
		UBTTask_Wait* T = NewObject<UBTTask_Wait>(BT);
		T->WaitTime = FValueOrBBKey_Float(Seconds);
		T->RandomDeviation = FValueOrBBKey_Float(Seconds * 0.25f);
		return T;
	};
	auto Seq = [BT](const TCHAR* Name, TArray<UBTDecorator*> Decorators, TArray<UBTTaskNode*> Tasks, UBTCompositeNode*& OutNode)
	{
		UBTComposite_Sequence* S = NewObject<UBTComposite_Sequence>(BT);
		S->NodeName = Name;
		for (UBTTaskNode* T : Tasks)
		{
			FBTCompositeChild& C = S->Children.AddDefaulted_GetRef();
			C.ChildTask = T;
		}
		OutNode = S;
		return Decorators;
	};
	UBTComposite_Selector* Root = NewObject<UBTComposite_Selector>(BT);
	Root->NodeName = TEXT("Doom Monster");
	Root->Services.Add(NewObject<UBTService_DoomTrackTarget>(BT));
	auto AddBranch = [Root](UBTCompositeNode* Node, const TArray<UBTDecorator*>& Decorators)
	{
		FBTCompositeChild& C = Root->Children.AddDefaulted_GetRef();
		C.ChildComposite = Node;
		for (UBTDecorator* D : Decorators) C.Decorators.Add(D);
	};
	UBTCompositeNode* Node = nullptr;
	// hurt: duck out of sight for a moment (not melee monsters: they charge on)
	UBTDecorator_Cooldown* CoverCooldown = NewObject<UBTDecorator_Cooldown>(BT);
	CoverCooldown->CoolDownTime = FValueOrBBKey_Float(8.f);
	TArray<UBTDecorator*> D = Seq(TEXT("Take Cover"), { Cond(EDoomAICondition::Hurt), Cond(EDoomAICondition::MeleeOnly, true), Cond(EDoomAICondition::CanSeeTarget), CoverCooldown },
		{ Find(Out.Cover), Move(DoomAIKeys::MoveGoal, 2.5f, 24.f), Wait(1.2f) }, Node);
	AddBranch(Node, D);
	// melee monsters go for the target by the shortest way, seen or not
	D = Seq(TEXT("Charge"), { Cond(EDoomAICondition::MeleeOnly), Cond(EDoomAICondition::HasTarget) }, { Move(DoomAIKeys::Target, 1.f, 48.f) }, Node);
	AddBranch(Node, D);
	// the rest find a firing position: in sight, at their range, away from the others
	D = Seq(TEXT("Fight"), { Cond(EDoomAICondition::CanSeeTarget) }, { Find(Out.AttackSpot), Move(DoomAIKeys::MoveGoal, 1.5f, 24.f) }, Node);
	AddBranch(Node, D);
	// out of sight: walk round to the target (Doom's monsters always head for it; these find the way)
	D = Seq(TEXT("Hunt"), { Cond(EDoomAICondition::HasTarget) }, { Move(DoomAIKeys::Target, 2.f, 64.f) }, Node);
	AddBranch(Node, D);
	// no target: go where something was last seen or heard
	D = Seq(TEXT("Investigate"), { Cond(EDoomAICondition::KnowsWhere) }, { Move(DoomAIKeys::LastKnown, 4.f, 48.f, true) }, Node);
	AddBranch(Node, D);
	FBTCompositeChild& Idle = Root->Children.AddDefaulted_GetRef();
	Idle.ChildTask = Wait(0.5f);
	BT->RootNode = Root;

	Out.Blackboard = BB;
	Out.Tree = BT;
}

const FDoomAIAssets& DoomAI::Get()
{
	static FDoomAIAssets Assets;
	static bool bDone = false;
	if (bDone)
	{
		return Assets;
	}
	bDone = true;
	auto Load = [](const TCHAR* Name) { return LoadObject<UObject>(nullptr, *FString::Printf(TEXT("%s%s.%s"), AssetFolder, Name, Name), nullptr, LOAD_NoWarn | LOAD_Quiet); };
	Assets.Blackboard = Cast<UBlackboardData>(Load(TEXT("BB_DoomMonster")));
	Assets.Tree = Cast<UBehaviorTree>(Load(TEXT("BT_DoomMonster")));
	Assets.AttackSpot = Cast<UEnvQuery>(Load(TEXT("EQ_DoomAttackSpot")));
	Assets.Cover = Cast<UEnvQuery>(Load(TEXT("EQ_DoomCover")));
	if (!Assets.Blackboard || !Assets.Tree || !Assets.AttackSpot || !Assets.Cover)
	{
		Assets = FDoomAIAssets();
		Build([](UClass* Class, FName Name) { UObject* O = NewObject<UObject>(GetTransientPackage(), Class, Name); O->AddToRoot(); return O; }, Assets);
		UE_LOG(LogDoom, Log, TEXT("Monster AI: no assets in %s, built the tree and queries in code"), AssetFolder);
	}
	else
	{
		for (UObject* O : Assets.All()) O->AddToRoot();
	}
	return Assets;
}
