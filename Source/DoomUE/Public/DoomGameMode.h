#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DoomGameMode.generated.h"

/** Boots the WADs, then shows the title screen or warps straight to -warp= / StartMap. */
UCLASS()
class DOOMUE_API ADoomGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADoomGameMode();
	virtual void StartPlay() override;
	/** The Entry map has no PlayerStart: spawn anywhere, LoadMap moves the marine to thing type 1. */
	virtual void RestartPlayer(AController* NewPlayer) override;

	/** Console: DoomMap E1M2 */
	UFUNCTION(Exec)
	void DoomMap(const FString& MapName);

	/** Console: DoomGod */
	UFUNCTION(Exec)
	void DoomGod();

	/** Console: DoomGiveAll (weapons, ammo, keys) */
	UFUNCTION(Exec)
	void DoomGiveAll();
};
