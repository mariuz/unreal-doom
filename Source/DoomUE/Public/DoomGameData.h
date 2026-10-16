// The game tables as an asset: every thing, projectile and weapon definition and the game rules,
// editable in the editor (Content/Doom/Data/DA_DoomGameData). The game copies them at start, and
// DeHackEd patches change that copy, never the asset.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DoomInfo.h"
#include "DoomGameData.generated.h"

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

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("DoomGameData"), GetFName());
	}
};
