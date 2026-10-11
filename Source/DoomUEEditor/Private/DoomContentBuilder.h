#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DoomContentBuilder.generated.h"

/**
 * Generates the port's only authored assets in code, so the repository holds no
 * binary .uasset files: the Doom material parameter collection and the four
 * materials (classic world, modern lit world, sky, sprite) whose Custom HLSL
 * nodes implement Doom's light diminishing and sky projection, and the Niagara
 * particle bursts of the modern renderer (copies of an engine lightweight template).
 *
 * Runs automatically when the editor starts and the assets are missing, or:
 *   UnrealEditor-Cmd DoomUE.uproject -run=DoomContent [-force] [-effects | -materials]
 */
namespace DoomContent
{
	bool AssetsExist();
	bool Build(bool bForce, bool bMaterials = true, bool bEffects = true);
	/** Logs a Niagara system's lightweight emitters and their module properties (-dumpniagara=Path). */
	void DumpNiagara(const FString& Path);
}

UCLASS()
class UDoomContentCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
