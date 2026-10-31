// UDoomAssetValidator: Tools > Validate Data for the Doom input and AI assets (DoomAssetValidator.h).
#include "DoomAssetValidator.h"
#include "DoomAI.h"
#include "DoomInput.h"
#include "DoomUE.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "UserSettings/EnhancedInputUserSettings.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "Misc/DataValidation.h"

#define LOCTEXT_NAMESPACE "DoomAssetValidator"

void DoomAssetChecks::MappingContext(const UInputMappingContext& Context, bool bWholeSet, TArray<FText>& OutErrors, TArray<FText>& OutWarnings)
{
	TSet<FName> Known;
	for (const DoomInput::FMappable& M : DoomInput::Mappables())
	{
		Known.Add(M.Name);
	}
	TSet<TPair<const UInputAction*, FKey>> Seen;
	const TArray<FEnhancedActionKeyMapping>& Mappings = Context.GetMappings();
	for (int32 i = 0; i < Mappings.Num(); ++i)
	{
		const FEnhancedActionKeyMapping& M = Mappings[i];
		const FText Who = FText::Format(LOCTEXT("Mapping", "Mapping {0}"), DoomText::Number(i));
		if (!M.Action)
		{
			OutErrors.Add(FText::Format(LOCTEXT("NoAction", "{0} ({1}): no input action"), Who, M.Key.GetDisplayName()));
			continue;
		}
		if (!M.Key.IsValid())
		{
			OutErrors.Add(FText::Format(LOCTEXT("NoKey", "{0} ({1}): no valid key"), Who, FText::FromName(M.Action->GetFName())));
		}
		if (Seen.Contains({ M.Action.Get(), M.Key }))
		{
			OutWarnings.Add(FText::Format(LOCTEXT("DupMapping", "{0}: {1} is mapped to {2} twice"), Who, M.Key.GetDisplayName(), FText::FromName(M.Action->GetFName())));
		}
		Seen.Add({ M.Action.Get(), M.Key });
		if (M.IsPlayerMappable() && !Known.Contains(M.GetMappingName()))
		{
			OutWarnings.Add(FText::Format(LOCTEXT("UnknownMappable", "{0}: rebindable as \"{1}\", which isn't a row of the Controls page (DoomInput::Mappables)"),
				Who, FText::FromName(M.GetMappingName())));
		}
	}
	if (!bWholeSet)
	{
		return;
	}
	FDoomInputObjects Set;
	if (!DoomInput::LoadAssets(Set))
	{
		OutErrors.Add(FText::Format(LOCTEXT("SetMissing", "the input set in {0} is incomplete, so the game builds its controls in code (run -run=DoomContent -input -force)"),
			FText::FromString(DoomInput::AssetFolder)));
		return;
	}
	TSet<FName> Mapped;
	for (const UInputMappingContext* C : { Set.Gameplay, Set.Menu, Set.AutomapPanContext })
	{
		for (const FEnhancedActionKeyMapping& M : C->GetMappings())
		{
			if (M.IsPlayerMappable()) Mapped.Add(M.GetMappingName());
		}
	}
	for (const DoomInput::FMappable& M : DoomInput::Mappables())
	{
		if (!Mapped.Contains(M.Name))
		{
			OutErrors.Add(FText::Format(LOCTEXT("RowUnmapped", "the Controls row \"{0}\" ({1}) has no rebindable mapping in the input set"), M.Label, FText::FromName(M.Name)));
		}
	}
}

void DoomAssetChecks::Blackboard(const UBlackboardData& Blackboard, TArray<FText>& OutErrors)
{
	const TPair<FName, UClass*> Wanted[] = {
		{ DoomAIKeys::Target, UBlackboardKeyType_Object::StaticClass() },
		{ DoomAIKeys::LastKnown, UBlackboardKeyType_Vector::StaticClass() },
		{ DoomAIKeys::CanSee, UBlackboardKeyType_Bool::StaticClass() },
		{ DoomAIKeys::MoveGoal, UBlackboardKeyType_Vector::StaticClass() },
	};
	for (const TPair<FName, UClass*>& W : Wanted)
	{
		const FBlackboardEntry* Found = nullptr;
		for (const UBlackboardData* B = &Blackboard; B && !Found; B = B->Parent)
		{
			Found = B->GetKeys().FindByPredicate([&W](const FBlackboardEntry& E) { return E.EntryName == W.Key; });
		}
		if (!Found)
		{
			OutErrors.Add(FText::Format(LOCTEXT("KeyMissing", "no key \"{0}\" (the monsters' AI reads it)"), FText::FromName(W.Key)));
		}
		else if (!Found->KeyType || !Found->KeyType->IsA(W.Value))
		{
			OutErrors.Add(FText::Format(LOCTEXT("KeyType", "key \"{0}\" must be a {1}"), FText::FromName(W.Key), W.Value->GetDisplayNameText()));
		}
	}
}

void DoomAssetChecks::BehaviorTree(const UBehaviorTree& Tree, TArray<FText>& OutErrors)
{
	if (!Tree.BlackboardAsset)
	{
		OutErrors.Add(LOCTEXT("NoBlackboard", "no blackboard"));
	}
	else
	{
		TArray<FText> KeyErrors;
		Blackboard(*Tree.BlackboardAsset, KeyErrors);
		for (const FText& E : KeyErrors)
		{
			OutErrors.Add(FText::Format(LOCTEXT("TreeBlackboard", "its blackboard {0}: {1}"), FText::FromName(Tree.BlackboardAsset->GetFName()), E));
		}
	}
	if (!Tree.RootNode)
	{
		OutErrors.Add(LOCTEXT("NoRoot", "no root node, so monsters would stand still"));
	}
}

void DoomAssetChecks::Query(const UEnvQuery& Query, TArray<FText>& OutErrors)
{
	if (Query.GetOptions().IsEmpty())
	{
		OutErrors.Add(LOCTEXT("NoOptions", "no options, so it never finds a place"));
	}
	for (int32 i = 0; i < Query.GetOptions().Num(); ++i)
	{
		const UEnvQueryOption* O = Query.GetOptions()[i];
		if (!O || !O->Generator)
		{
			OutErrors.Add(FText::Format(LOCTEXT("NoGenerator", "option {0} has no generator"), DoomText::Number(i)));
		}
	}
}

bool UDoomAssetValidator::CanValidateAsset_Implementation(const FAssetData& InAssetData, UObject* InObject, FDataValidationContext& InContext) const
{
	if (!InObject)
	{
		return false;
	}
	const FString Path = InObject->GetPathName();
	const bool bInput = Path.StartsWith(DoomInput::AssetFolder) && InObject->IsA<UInputMappingContext>();
	const bool bAI = Path.StartsWith(DoomAI::AssetFolder) && (InObject->IsA<UBlackboardData>() || InObject->IsA<UBehaviorTree>() || InObject->IsA<UEnvQuery>());
	return bInput || bAI;
}

EDataValidationResult UDoomAssetValidator::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset, FDataValidationContext& Context)
{
	TArray<FText> Errors, Warnings;
	if (const UInputMappingContext* C = Cast<UInputMappingContext>(InAsset))
	{
		// the gameplay context also checks the set as a whole (once, not per context)
		DoomAssetChecks::MappingContext(*C, C->GetFName() == TEXT("IMC_Doom"), Errors, Warnings);
	}
	else if (const UBlackboardData* B = Cast<UBlackboardData>(InAsset))
	{
		DoomAssetChecks::Blackboard(*B, Errors);
	}
	else if (const UBehaviorTree* T = Cast<UBehaviorTree>(InAsset))
	{
		DoomAssetChecks::BehaviorTree(*T, Errors);
	}
	else if (const UEnvQuery* Q = Cast<UEnvQuery>(InAsset))
	{
		DoomAssetChecks::Query(*Q, Errors);
	}
	for (const FText& E : Errors)
	{
		AssetFails(InAsset, E);
	}
	for (const FText& W : Warnings)
	{
		AssetWarning(InAsset, W);
	}
	if (Errors.IsEmpty())
	{
		AssetPasses(InAsset);
	}
	return GetValidationResult();
}

#undef LOCTEXT_NAMESPACE
