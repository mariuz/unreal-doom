// SMART monsters (Options > Monster AI): Unreal's AI framework drives where monsters go, while Doom's
// own code still decides when they attack, what the attacks do and every frame they show.
//
//  * ADoomAIController possesses each monster: AI Perception (sight in front, hearing, damage) wakes
//    it and remembers where the target was last seen; a Behavior Tree with a Blackboard picks what
//    to do next.
//  * The tree's custom nodes: a service that tracks the target, conditions, a task that runs an EQS
//    query for a spot to move to (a firing position that sees the target at the monster's preferred
//    range, away from other monsters; cover when hurt), and a task that walks there.
//  * Walking follows paths over the map's convex subsectors (FDoomNav): around walls and pillars,
//    up steps and through doors monsters can open, as Doom's own movement allows.
//
// The content builder saves the blackboard, the tree and the EQS queries in /Game/Doom/AI (editable
// in the Behavior Tree and EQS editors); the game builds the same set in code when they are missing.
// CLASSIC (the default) keeps vanilla's A_Chase.
#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTDecorator.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "Perception/AIPerceptionTypes.h"
#include "DoomAI.generated.h"

class UBehaviorTree;
class UBlackboardData;
class UEnvQuery;
class UAIPerceptionComponent;
class ADoomMonster;
class FDoomMap;
struct FDoomThingDef;

/** The blackboard's keys. */
namespace DoomAIKeys
{
	inline const FName Target(TEXT("Target"));       // Object: the actor being hunted
	inline const FName LastKnown(TEXT("LastKnown")); // Vector: where it was last seen or heard
	inline const FName CanSee(TEXT("CanSee"));       // Bool: in sight now
	inline const FName MoveGoal(TEXT("MoveGoal"));   // Vector: where the tree wants to go
}

/** Paths for walking monsters: the map's subsectors (convex, so a straight line inside one is clear)
 *  joined where their polygons share an edge, crossed where Doom's movement allows. */
class DOOMUE_API FDoomNav
{
public:
	void Build(const FDoomMap& Map);
	bool IsBuilt() const { return Portals.Num() > 0 || Adjacent.Num() > 0; }

	/** Waypoints (Doom 2D) from From to To for a mover of this radius and height; false if no way.
	 *  bFloat: flyers ignore steps. Closed doors with a monster-usable special count as open. */
	bool FindPath(const FDoomMap& Map, const FVector2f& From, const FVector2f& To, float Radius, float Height, bool bFloat,
		TArray<FVector2f>& Out, int32 MaxNodes = 4000) const;

	/** Whether a mover of this radius fits at P (inside the map, clear of the walls of its sector). */
	static bool IsClear(const FDoomMap& Map, const FVector2f& P, float Radius);

	int32 NumPortals() const { return Portals.Num(); }
	/** Subsectors reachable from P: over any portal (bRules false), or as FindPath walks (tests). */
	int32 CountReachable(const FDoomMap& Map, const FVector2f& P, bool bRules, float Radius, float Height) const;

private:
	struct FPortal
	{
		int32 A = INDEX_NONE, B = INDEX_NONE; // subsectors
		FVector2f P0, P1;                     // the shared edge
		int32 Line = INDEX_NONE;              // the two-sided line it lies on, if any
	};
	TArray<FPortal> Portals;
	TArray<TArray<int32>> Adjacent; // per subsector: portal indices
	bool CanCross(const FDoomMap& Map, const FPortal& P, int32 FromSs, float Radius, float Height, bool bFloat) const;
};

UCLASS()
class DOOMUE_API ADoomAIController : public AAIController
{
	GENERATED_BODY()

public:
	ADoomAIController(const FObjectInitializer& ObjectInitializer);

	ADoomMonster* GetMonster() const;
	/** Perception events so far (the play-test checks the senses work). */
	int32 NumSensed = 0;

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	UFUNCTION()
	void OnPerceived(AActor* Actor, FAIStimulus Stimulus);

	UPROPERTY(VisibleAnywhere, Category = "Doom")
	TObjectPtr<UAIPerceptionComponent> Senses;
};

/** Keeps the blackboard current: the monster's target, whether it is in sight (Doom's own sight
 *  check), and where it was last seen. */
UCLASS(meta = (DisplayName = "Doom: Track Target"))
class DOOMUE_API UBTService_DoomTrackTarget : public UBTService
{
	GENERATED_BODY()
public:
	UBTService_DoomTrackTarget();
protected:
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
};

UENUM()
enum class EDoomAICondition : uint8
{
	CanSeeTarget,   // the target is in sight
	HasTarget,      // hunting someone (Doom's monsters always know where their target is)
	KnowsWhere,     // somewhere to look: a last known position
	MeleeOnly,      // no missile or hitscan attack (demons, spectres, lost souls)
	Hurt,           // below a third of its health
};

UCLASS(meta = (DisplayName = "Doom: Condition"))
class DOOMUE_API UBTDecorator_DoomCondition : public UBTDecorator
{
	GENERATED_BODY()
public:
	UBTDecorator_DoomCondition();

	UPROPERTY(EditAnywhere, Category = "Doom")
	EDoomAICondition Condition = EDoomAICondition::CanSeeTarget;
	/** Passes when the condition is false. */
	UPROPERTY(EditAnywhere, Category = "Doom")
	bool bNot = false;

protected:
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;
	virtual FString GetStaticDescription() const override;
};

/** Runs an EQS query from the monster and writes the best point into a blackboard vector. */
UCLASS(meta = (DisplayName = "Doom: Find Spot (EQS)"))
class DOOMUE_API UBTTask_DoomFindSpot : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UBTTask_DoomFindSpot();

	UPROPERTY(EditAnywhere, Category = "Doom")
	TObjectPtr<UEnvQuery> Query;
	UPROPERTY(EditAnywhere, Category = "Doom")
	FName ResultKey = DoomAIKeys::MoveGoal;

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual uint16 GetInstanceMemorySize() const override { return sizeof(int32); }
	virtual FString GetStaticDescription() const override;
};

/** Walks the monster to a blackboard location (or the target actor) along an FDoomNav path. The
 *  monster keeps its Doom states: it stops to attack or flinch, then walks on. Succeeds on arrival
 *  or when MaxSeconds run out (the tree then thinks again); fails when there is no way. */
UCLASS(meta = (DisplayName = "Doom: Move To"))
class DOOMUE_API UBTTask_DoomMoveTo : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UBTTask_DoomMoveTo();

	/** A vector key, or Target for the target actor itself. */
	UPROPERTY(EditAnywhere, Category = "Doom")
	FName GoalKey = DoomAIKeys::MoveGoal;
	UPROPERTY(EditAnywhere, Category = "Doom")
	float AcceptRadius = 40.f;
	UPROPERTY(EditAnywhere, Category = "Doom")
	float MaxSeconds = 2.f;
	/** Forget the last known position on arrival (it has been searched). */
	UPROPERTY(EditAnywhere, Category = "Doom")
	bool bClearLastKnownOnArrival = false;

protected:
	struct FMemory { float Elapsed = 0; };
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FMemory); }
	virtual FString GetStaticDescription() const override;
	bool GoalOf(UBehaviorTreeComponent& OwnerComp, FVector2f& Out) const;
};

/** The target in the blackboard (EQS context). */
UCLASS(meta = (DisplayName = "Doom: Target"))
class DOOMUE_API UEnvQueryContext_DoomTarget : public UEnvQueryContext
{
	GENERATED_BODY()
public:
	virtual void ProvideContext(FEnvQueryInstance& QueryInstance, FEnvQueryContextData& ContextData) const override;
};

/** Points in rings around the monster where it fits, standing on the floor of their sector. */
UCLASS(meta = (DisplayName = "Doom: Spots Around"))
class DOOMUE_API UEnvQueryGenerator_DoomSpots : public UEnvQueryGenerator
{
	GENERATED_BODY()
public:
	UEnvQueryGenerator_DoomSpots();

	UPROPERTY(EditDefaultsOnly, Category = "Doom")
	float MinRadius = 64.f;
	UPROPERTY(EditDefaultsOnly, Category = "Doom")
	float MaxRadius = 384.f;
	UPROPERTY(EditDefaultsOnly, Category = "Doom")
	int32 Rings = 3;
	UPROPERTY(EditDefaultsOnly, Category = "Doom")
	int32 PointsPerRing = 12;

	virtual void GenerateItems(FEnvQueryInstance& QueryInstance) const override;
	virtual FText GetDescriptionTitle() const override;
};

/** Whether the target is in sight from the point (at the monster's eye height). */
UCLASS(meta = (DisplayName = "Doom: Sees Target"))
class DOOMUE_API UEnvQueryTest_DoomSight : public UEnvQueryTest
{
	GENERATED_BODY()
public:
	UEnvQueryTest_DoomSight();
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override;
	virtual FText GetDescriptionDetails() const override;
};

/** How near the point is to the monster's preferred range from the target (close for melee). */
UCLASS(meta = (DisplayName = "Doom: Preferred Range"))
class DOOMUE_API UEnvQueryTest_DoomRange : public UEnvQueryTest
{
	GENERATED_BODY()
public:
	UEnvQueryTest_DoomRange();
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override;
};

/** Room from the other monsters (and where they are heading): spreads a group out. */
UCLASS(meta = (DisplayName = "Doom: Away From Crowd"))
class DOOMUE_API UEnvQueryTest_DoomCrowd : public UEnvQueryTest
{
	GENERATED_BODY()
public:
	UEnvQueryTest_DoomCrowd();
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override;
};

/** Whether the monster can walk there (an FDoomNav path), scored by how short the walk is. */
UCLASS(meta = (DisplayName = "Doom: Reachable"))
class DOOMUE_API UEnvQueryTest_DoomReachable : public UEnvQueryTest
{
	GENERATED_BODY()
public:
	UEnvQueryTest_DoomReachable();
	virtual void RunTest(FEnvQueryInstance& QueryInstance) const override;
};

struct DOOMUE_API FDoomAIAssets
{
	UBlackboardData* Blackboard = nullptr;
	UBehaviorTree* Tree = nullptr;
	UEnvQuery* AttackSpot = nullptr;
	UEnvQuery* Cover = nullptr;
	TArray<UObject*> All() const;
};

namespace DoomAI
{
	constexpr const TCHAR* AssetFolder = TEXT("/Game/Doom/AI/");

	/** Creates the set: New(Class, Name) makes each asset. */
	DOOMUE_API void Build(TFunctionRef<UObject*(UClass*, FName)> New, FDoomAIAssets& Out);
	/** The saved assets, or the same set built in code (once) when they are missing. */
	DOOMUE_API const FDoomAIAssets& Get();

	/** Where a monster likes to fight from (Doom units): 0 for melee-only monsters. */
	DOOMUE_API float PreferredRange(const FDoomThingDef* Def);
	DOOMUE_API bool IsMeleeOnly(const FDoomThingDef* Def);
}
