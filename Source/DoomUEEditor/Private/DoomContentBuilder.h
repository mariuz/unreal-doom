#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DoomContentBuilder.generated.h"

/**
 * Generates the port's only authored assets in code, so the repository holds no
 * binary .uasset files: the Doom material parameter collection and the four
 * materials (classic world, modern lit world, sky, sprite) whose Custom HLSL
 * nodes implement Doom's light diminishing and sky projection.
 *
 * Runs automatically when the editor starts and the assets are missing, or:
 *   UnrealEditor-Cmd DoomUE.uproject -run=DoomContent [-force]
 */
namespace DoomContent
{
	bool AssetsExist();
	bool Build(bool bForce);
}

UCLASS()
class UDoomContentCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
