// The content builder's data validation (-validate, -validatetest, DoomUE.Editor.DataValidation):
// Tools > Validate Data on the Doom assets, and the checks on broken copies of them.
#include "DoomContentBuilder.h"
#include "DoomContentPrivate.h"
#include "DoomAssetValidator.h"
#include "DoomGameData.h"
#include "DoomSettings.h"
#include "DoomAI.h"
#include "DoomInput.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorValidatorSubsystem.h"
#include "Editor.h"
#include "InputMappingContext.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Misc/DataValidation.h"

namespace DoomContentValidateDetail
{
	/** The folders whose assets the game loads by path (game data, input, AI). */
	TArray<FAssetData> DoomAssets()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.SearchAllAssets(true);
		FARFilter Filter;
		Filter.bRecursivePaths = true;
		for (const TCHAR* Folder : { TEXT("/Game/Doom/Data"), TEXT("/Game/Doom/Input"), TEXT("/Game/Doom/AI") })
		{
			Filter.PackagePaths.Add(Folder);
		}
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		return Assets;
	}

	bool AnyContains(const TArray<FText>& Messages, const TCHAR* Part)
	{
		return Messages.ContainsByPredicate([Part](const FText& T) { return T.ToString().Contains(Part); });
	}

	void LogAll(const TCHAR* What, const TArray<FText>& Messages)
	{
		for (const FText& M : Messages)
		{
			UE_LOG(LogDoomContent, Display, TEXT("  %s: %s"), What, *M.ToString());
		}
	}
}

bool DoomContent::Validate()
{
	using namespace DoomContentValidateDetail;
	UEditorValidatorSubsystem* Validator = GEditor ? GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>() : nullptr;
	if (!Validator)
	{
		UE_LOG(LogDoomContent, Error, TEXT("Validate: no data validation subsystem (is the DataValidation plugin enabled?)"));
		return false;
	}
	int32 Failed = 0, Validated = 0, Warned = 0;
	const TArray<FAssetData> Assets = DoomAssets();
	for (const FAssetData& A : Assets)
	{
		TArray<FText> Errors, Warnings;
		const EDataValidationResult R = Validator->IsAssetValid(A, Errors, Warnings, EDataValidationUsecase::Commandlet);
		Validated += R != EDataValidationResult::NotValidated;
		Failed += R == EDataValidationResult::Invalid;
		Warned += Warnings.Num() > 0;
		for (const FText& E : Errors)
		{
			UE_LOG(LogDoomContent, Error, TEXT("%s: %s"), *A.PackageName.ToString(), *E.ToString());
		}
		for (const FText& W : Warnings)
		{
			UE_LOG(LogDoomContent, Warning, TEXT("%s: %s"), *A.PackageName.ToString(), *W.ToString());
		}
	}
	UE_LOG(LogDoomContent, Display, TEXT("Validate: %d assets, %d validated, %d invalid, %d with warnings"), Assets.Num(), Validated, Failed, Warned);
	return Failed == 0 && Assets.Num() > 0;
}

bool DoomContent::ValidateTest()
{
	using namespace DoomContentValidateDetail;
	bool bPass = true;
	auto Step = [&bPass](const TCHAR* Name, bool bOk, const FString& Detail)
	{
		UE_LOG(LogDoomContent, Display, TEXT("Validatetest %s %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Name, *Detail);
		bPass &= bOk;
	};

	// 1. the built-in tables are clean (no errors and no warnings)
	{
		TArray<FDoomThingDef> Things;
		TArray<FDoomProjectileDef> Projectiles;
		TArray<FDoomWeaponDef> Weapons;
		DoomInfo::BuiltInTables(Things, Projectiles, Weapons);
		FDoomDataProblems P;
		UDoomGameData::CheckTables(Things, Projectiles, Weapons, FDoomRules(), P);
		LogAll(TEXT("built-in error"), P.Errors);
		LogAll(TEXT("built-in warning"), P.Warnings);
		Step(TEXT("builtin"), P.IsEmpty(), FString::Printf(TEXT("%d things, %d projectiles, %d weapons: %d errors, %d warnings"),
			Things.Num(), Projectiles.Num(), Weapons.Num(), P.Errors.Num(), P.Warnings.Num()));
	}

	// 2. the saved assets pass Tools > Validate Data
	Step(TEXT("assets"), Validate(), TEXT("the game data, input and AI assets in /Game/Doom"));

	// 3. a broken copy of the game data: each mistake is reported
	const UDoomGameData* Data = GetDefault<UDoomSettings>()->GameData.LoadSynchronous();
	if (!Data)
	{
		Step(TEXT("gamedata"), false, TEXT("no DA_DoomGameData (run -run=DoomContent -gamedata)"));
	}
	else
	{
		UDoomGameData* Broken = DuplicateObject<UDoomGameData>(Data, GetTransientPackage());
		TArray<FDoomThingDef> BuiltThings;
		TArray<FDoomProjectileDef> BuiltProjectiles;
		TArray<FDoomWeaponDef> BuiltWeapons;
		DoomInfo::BuiltInTables(BuiltThings, BuiltProjectiles, BuiltWeapons);
		if (Broken->Things.IsEmpty()) Broken->Things = BuiltThings;
		if (Broken->Projectiles.IsEmpty()) Broken->Projectiles = BuiltProjectiles;
		const int32 Imp = Broken->Things.IndexOfByPredicate([](const FDoomThingDef& D) { return D.DoomEdNum == 3001; });
		const int32 Zombie = Broken->Things.IndexOfByPredicate([](const FDoomThingDef& D) { return D.DoomEdNum == 3004; });
		const bool bFound = Imp != INDEX_NONE && Zombie != INDEX_NONE && Broken->Projectiles.Num() > 0;
		if (bFound)
		{
			const FDoomThingDef Twin = Broken->Things[Zombie];
			Broken->Things.Add(Twin);                                // a duplicate DoomEdNum
			Broken->Things[Imp].Projectile = TEXT("NoSuchBall");     // unknown projectile
			Broken->Things[Zombie].DropType = 31337;                 // unknown drop
			Broken->Things[Zombie].Run = TEXT("AB?");                // a bad frame letter
			Broken->Projectiles[0].Speed = 0.f;                      // a missile that never moves
			FDoomWeaponDef Shotgun = DoomInfo::Weapon(EDoomWeapon::Shotgun);
			Shotgun.Slot = 9;                                        // no such number key
			Broken->Weapons.Add(Shotgun);
			Broken->Rules.InitialHealth = 0;                         // dead on arrival
		}
		FDoomDataProblems P;
		Broken->Check(P);
		LogAll(TEXT("broken game data"), P.Errors);
		const TCHAR* Expected[] = { TEXT("NoSuchBall"), TEXT("31337"), TEXT("AB?"), TEXT("the number is also"), TEXT("speed must be above 0"),
			TEXT("the slot must be 1 to 7"), TEXT("initial health must be above 0") };
		int32 Seen = 0;
		for (const TCHAR* E : Expected) Seen += AnyContains(P.Errors, E);
		// and the editor's entry point says the same
		FDataValidationContext Context;
		const bool bInvalid = Broken->IsDataValid(Context) == EDataValidationResult::Invalid;
		Step(TEXT("gamedata"), bFound && Seen == UE_ARRAY_COUNT(Expected) && P.Errors.Num() == UE_ARRAY_COUNT(Expected) && bInvalid,
			FString::Printf(TEXT("%d of %d mistakes found, %d errors in all, IsDataValid %s"), Seen, (int32)UE_ARRAY_COUNT(Expected), P.Errors.Num(),
				bInvalid ? TEXT("Invalid") : TEXT("NOT INVALID")));
	}

	// 4. broken copies of the input and AI assets
	FDoomInputObjects Input;
	const FDoomAIAssets& AI = DoomAI::Get();
	if (!DoomInput::LoadAssets(Input) || !AI.Blackboard || !AI.Tree || !AI.AttackSpot)
	{
		Step(TEXT("input+ai"), false, TEXT("the saved input or AI assets are missing"));
		return bPass;
	}
	{
		UInputMappingContext* Ctx = DuplicateObject<UInputMappingContext>(Input.Gameplay, GetTransientPackage());
		Ctx->MapKey(nullptr, EKeys::SpaceBar);
		Ctx->MapKey(Input.Fire, FKey());
		TArray<FText> Errors, Warnings;
		DoomAssetChecks::MappingContext(*Ctx, false, Errors, Warnings);
		TArray<FText> CleanErrors, CleanWarnings;
		DoomAssetChecks::MappingContext(*Input.Gameplay, true, CleanErrors, CleanWarnings);
		LogAll(TEXT("broken input"), Errors);
		LogAll(TEXT("input error"), CleanErrors);
		LogAll(TEXT("input warning"), CleanWarnings);
		Step(TEXT("input"), Errors.Num() == 2 && AnyContains(Errors, TEXT("no input action")) && AnyContains(Errors, TEXT("no valid key")) && CleanErrors.IsEmpty() && CleanWarnings.IsEmpty(),
			FString::Printf(TEXT("broken copy: %d errors (2 expected); saved set: %d errors, %d warnings"), Errors.Num(), CleanErrors.Num(), CleanWarnings.Num()));
	}
	{
		UBlackboardData* BB = DuplicateObject<UBlackboardData>(AI.Blackboard, GetTransientPackage());
		BB->Keys.RemoveAll([](const FBlackboardEntry& E) { return E.EntryName == DoomAIKeys::CanSee; });
		UBehaviorTree* BT = DuplicateObject<UBehaviorTree>(AI.Tree, GetTransientPackage());
		BT->RootNode = nullptr;
		BT->BlackboardAsset = BB;
		UEnvQuery* Q = DuplicateObject<UEnvQuery>(AI.AttackSpot, GetTransientPackage());
		Q->GetOptionsMutable().Reset();
		TArray<FText> BBErrors, BTErrors, QErrors, Clean;
		DoomAssetChecks::Blackboard(*BB, BBErrors);
		DoomAssetChecks::BehaviorTree(*BT, BTErrors);
		DoomAssetChecks::Query(*Q, QErrors);
		DoomAssetChecks::BehaviorTree(*AI.Tree, Clean);
		DoomAssetChecks::Query(*AI.AttackSpot, Clean);
		if (AI.Cover) DoomAssetChecks::Query(*AI.Cover, Clean);
		LogAll(TEXT("broken AI"), BBErrors);
		LogAll(TEXT("broken AI"), BTErrors);
		LogAll(TEXT("broken AI"), QErrors);
		LogAll(TEXT("AI error"), Clean);
		Step(TEXT("ai"), BBErrors.Num() == 1 && AnyContains(BBErrors, TEXT("CanSee")) && BTErrors.Num() == 2 && AnyContains(BTErrors, TEXT("no root node"))
			&& QErrors.Num() == 1 && Clean.IsEmpty(),
			FString::Printf(TEXT("blackboard %d error (1 expected), tree %d (2), query %d (1); saved assets %d"), BBErrors.Num(), BTErrors.Num(), QErrors.Num(), Clean.Num()));
	}
	UE_LOG(LogDoomContent, Display, TEXT("Validatetest %s"), bPass ? TEXT("PASS") : TEXT("FAIL"));
	return bPass;
}
