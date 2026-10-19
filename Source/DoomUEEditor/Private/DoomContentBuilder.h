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
 *
 * It also writes the editable game tables (/Game/Doom/Data/DA_DoomGameData) from the built-in ones
 * when they are missing; -gamedata -force rewrites them (losing edits made in the editor).
 */
class ADoomMapActor;

namespace DoomContent
{
	bool AssetsExist();
	bool Build(bool bForce, bool bMaterials = true, bool bEffects = true);
	/** Writes UDoomGameData from the built-in tables when missing (or always with bOverwrite). */
	bool BuildGameData(bool bOverwrite);
	/** Saves the input actions and mapping contexts (DoomInput::Build) in /Game/Doom/Input when missing
	 *  (or always with bOverwrite): editable in the editor, cooked into packaged games. */
	bool BuildInput(bool bOverwrite);
	/** The SMART monsters' blackboard, behavior tree and EQS queries in /Game/Doom/AI. */
	bool BuildAI(bool bOverwrite);
	/** Logs a Niagara system's lightweight emitters and their module properties (-dumpniagara=Path). */
	void DumpNiagara(const FString& Path);
	/** Saves a WAD map as an editable level (/Game/Doom/Maps/<MAP>): map actor, thing markers. */
	bool ImportMap(const FString& MapName, bool bOverwrite);
	/** Bakes a WAD map's static geometry as a tile set (/Game/Doom/Tiles/<WadSet>/<MAP>, UDoomMapTiles)
	 *  for playing it from the WAD in packaged builds (-baketiles=MAP|all). */
	bool BakeWadTiles(const FString& MapName);
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
