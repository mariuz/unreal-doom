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
 *   UnrealEditor-Cmd DoomUE.uproject -run=DoomContent [-force] [-effects | -materials] [-importmap=E1M1,E1M2|all]
 */
class ADoomMapActor;

namespace DoomContent
{
	bool AssetsExist();
	bool Build(bool bForce, bool bMaterials = true, bool bEffects = true);
	/** Logs a Niagara system's lightweight emitters and their module properties (-dumpniagara=Path). */
	void DumpNiagara(const FString& Path);
	/** Saves a WAD map as an editable level (/Game/Doom/Maps/<MAP>): map actor, thing markers. */
	bool ImportMap(const FString& MapName, bool bOverwrite);
	/** Saves a level's static geometry as static mesh tiles (/Game/Doom/Maps/<MAP>_Tiles). */
	bool BakeMapTiles(ADoomMapActor* MapActor);
	/** Writes a WAD map as a Lyra arena: OBJ tiles, collision ramps and a gameplay JSON (-exportlyra=MAP -out=DIR). */
	bool ExportLyraLevel(const FString& MapName, const FString& OutDir);
	FString MapPackage(const FString& MapName);
	/** Opens an imported level, edits a sector and a marker, saves /Game/Doom/Tests/<MAP>_Edit (-edittest=MAP). */
	bool EditTest(const FString& MapName);
}

UCLASS()
class UDoomContentCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
