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
	// Weapons and keys are plain bool arrays (reflected properties cannot be bool arrays); the
	// custom NetSerialize below sends the whole inventory, with these packed into bit masks.
	/** Owned weapons, indexed by EDoomWeapon. */
	bool Weapons[(int32)EDoomWeapon::Count] = { true, true, false, false, false, false, false, false, false };
	EDoomWeapon Ready = EDoomWeapon::Pistol;
	/** Blue card, yellow card, red card, blue skull, yellow skull, red skull. */
	bool Keys[6] = {};

	void Reset() { *this = FDoomInventory(); }
	void Serialize(FArchive& Ar);

	/** Replication: everything, weapons and keys as bit masks. */
	bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess);
	/** Replication compares whole inventories (the bool arrays are not reflected). */
	bool operator==(const FDoomInventory& O) const;
	uint16 WeaponMask() const;
	uint8 KeyMask() const;
};

template<>
struct TStructOpsTypeTraits<FDoomInventory> : public TStructOpsTypeTraitsBase2<FDoomInventory>
{
	enum { WithNetSerializer = true, WithIdenticalViaEquality = true };
};

UCLASS()
class DOOMUE_API ADoomPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	FDoomInventory Inv;

	/** 0..3: which player start (and status bar colour) this player uses; set when they join. */
	UPROPERTY(Replicated, VisibleAnywhere, BlueprintReadOnly, Category = "Doom")
	int32 PlayerNumber = 0;

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
