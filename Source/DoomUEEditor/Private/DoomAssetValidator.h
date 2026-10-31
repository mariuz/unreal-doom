// Tools > Validate Data for the Doom assets whose classes are the engine's: the input mapping contexts
// in /Game/Doom/Input and the blackboard, behavior tree and EQS queries in /Game/Doom/AI. (The game
// tables, UDoomGameData, check themselves in IsDataValid.)
#pragma once

#include "CoreMinimal.h"
#include "EditorValidatorBase.h"
#include "DoomAssetValidator.generated.h"

class UInputMappingContext;
class UBlackboardData;
class UBehaviorTree;
class UEnvQuery;

/** The checks, one per asset kind; each adds what it finds (the content builder's -validatetest
 *  runs them on broken copies). */
namespace DoomAssetChecks
{
	/** Every mapping has an action and a key; rebindable mappings are on the Controls page. With
	 *  bWholeSet, also that the saved input set loads and every Controls row is mapped somewhere. */
	void MappingContext(const UInputMappingContext& Context, bool bWholeSet, TArray<FText>& OutErrors, TArray<FText>& OutWarnings);
	/** The keys the monsters' AI reads (DoomAIKeys), with the right types. */
	void Blackboard(const UBlackboardData& Blackboard, TArray<FText>& OutErrors);
	/** A blackboard that has the keys, and a root node. */
	void BehaviorTree(const UBehaviorTree& Tree, TArray<FText>& OutErrors);
	/** At least one option, each with a generator. */
	void Query(const UEnvQuery& Query, TArray<FText>& OutErrors);
}

UCLASS()
class UDoomAssetValidator : public UEditorValidatorBase
{
	GENERATED_BODY()

protected:
	virtual bool CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject, FDataValidationContext& InContext) const override;
	virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context) override;
};
