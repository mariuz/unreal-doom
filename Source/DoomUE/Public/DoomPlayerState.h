// ADoomPlayerState: what belongs to a player rather than to their body. The inventory (health,
// armor, ammo, weapons, keys) and the player's own share of the level's kills, items and secrets
// live here, so they survive the pawn being replaced and travel with the player between maps.
// See DoomGameState.h for how the gameplay framework classes divide the game.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "DoomInfo.h"
#include "DoomPlayerState.generated.h"

/** Doom's player_t inventory. */
USTRUCT(BlueprintType)
struct FDoomInventory
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 Health = 100;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 Armor = 0;
	/** 0 none, 1 green (absorbs 1/3), 2 blue (1/2). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 ArmorClass = 0;
	/** Bullets, shells, rockets, cells. */
	UPROPERTY(VisibleAnywhere, Category = "Doom")
	int32 Ammo[4] = { 50, 0, 0, 0 };
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	bool bBackpack = false;
	// Weapons and keys are plain bool arrays (reflected properties cannot be bool arrays), so they are
	// saved through Serialize but not replicated yet; co-op would send them as bit masks.
	/** Owned weapons, indexed by EDoomWeapon. */
	bool Weapons[(int32)EDoomWeapon::Count] = { true, true, false, false, false, false, false, false, false };
	EDoomWeapon Ready = EDoomWeapon::Pistol;
	/** Blue card, yellow card, red card, blue skull, yellow skull, red skull. */
	bool Keys[6] = {};

	void Reset() { *this = FDoomInventory(); }
	void Serialize(FArchive& Ar);
};

UCLASS()
class DOOMUE_API ADoomPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	FDoomInventory Inv;

	/** This player's share of the level's counts (the intermission's per-player columns in co-op). */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 LevelKills = 0;
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 LevelItems = 0;
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 LevelSecrets = 0;
	/** Deathmatch frags (other players killed, minus suicides). */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 Frags = 0;

	void StartLevel() { LevelKills = LevelItems = LevelSecrets = 0; }
	void DoomSerialize(FArchive& Ar);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Seamless travel and reconnects keep the inventory, as Doom carries it into the next map. */
	virtual void CopyProperties(APlayerState* PlayerState) override;
};
