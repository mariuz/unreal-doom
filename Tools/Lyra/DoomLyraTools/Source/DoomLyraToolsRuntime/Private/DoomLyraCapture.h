// -doomshot=<seconds> [-doomquit=<seconds>]: in a game world, save in-engine screenshots
// (Saved/Screenshots/<platform>/DoomLyra_<map>_<n>.png) at that time and 15 s later, then quit.
// Used to check DoomUE's Lyra levels without capturing the desktop.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DoomLyraCapture.generated.h"

UCLASS()
class UDoomLyraCapture : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& World) override;
};
