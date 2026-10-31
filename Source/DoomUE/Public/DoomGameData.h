// The game tables as an asset: every thing, projectile and weapon definition and the game rules,
// editable in the editor (Content/Doom/Data/DA_DoomGameData). The game copies them at start, and
// DeHackEd patches change that copy, never the asset.
//
// Check finds what would break a game (Tools > Validate Data runs it through IsDataValid, and the
// game logs it when it loads the tables).
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DoomInfo.h"
#include "DoomGameData.generated.h"

/** What a check of the tables found: errors break the game, warnings are probably mistakes. */
struct DOOMUE_API FDoomDataProblems
{
	TArray<FText> Errors;
	TArray<FText> Warnings;
	bool IsEmpty() const { return Errors.IsEmpty() && Warnings.IsEmpty(); }
};

UCLASS(BlueprintType)
class DOOMUE_API UDoomGameData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Monsters, pickups and decorations, by DoomEdNum. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Doom", meta = (TitleProperty = "{DoomEdNum} {Name}"))
	TArray<FDoomThingDef> Things;

	/** Missiles, by name (FDoomThingDef::Projectile refers to them). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Doom", meta = (TitleProperty = "Name"))
	TArray<FDoomProjectileDef> Projectiles;

	/** One per player weapon; an entry replaces the built-in weapon of the same kind. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Doom", meta = (TitleProperty = "Weapon"))
	TArray<FDoomWeaponDef> Weapons;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Doom")
	FDoomRules Rules;

	/** Checks the tables the game would start from: an empty array here means the built-in table,
	 *  and weapons replace the built-in ones kind by kind (as DoomInfo::ResetTables does). */
	void Check(FDoomDataProblems& Out) const;

	/** Checks a complete set of tables: names and numbers unique, references (projectiles, drops)
	 *  known, sprites four characters, frame letters A to ], sizes, tics and rules in range. */
	static void CheckTables(TConstArrayView<FDoomThingDef> Things, TConstArrayView<FDoomProjectileDef> Projectiles,
		TConstArrayView<FDoomWeaponDef> Weapons, const FDoomRules& Rules, FDoomDataProblems& Out);

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("DoomGameData"), GetFName());
	}
};
