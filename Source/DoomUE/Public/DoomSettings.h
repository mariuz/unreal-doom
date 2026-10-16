#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DoomSettings.generated.h"

class UDoomGameData;

/** Project Settings > Game > Doom. Command line overrides: -iwad= -file= -warp= -skill= -modern / -classic */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Doom"))
class DOOMUE_API UDoomSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UDoomSettings() { CategoryName = TEXT("Game"); }

	/** IWAD file name in Content/WADs, or an absolute path. */
	UPROPERTY(Config, EditAnywhere, Category = "WAD")
	FString IWAD = TEXT("freedoom1.wad");

	/** Extra PWADs loaded on top of the IWAD (Doom's -file). */
	UPROPERTY(Config, EditAnywhere, Category = "WAD")
	TArray<FString> PWADs;

	/** Map to start on; empty shows the title screen. */
	UPROPERTY(Config, EditAnywhere, Category = "Game")
	FString StartMap;

	/**
	 * The editable game tables (things, projectiles, weapons, rules). The game starts from this
	 * asset when it exists, else from the built-in tables; DeHackEd patches apply on top.
	 * -run=DoomContent -gamedata writes it from the built-in tables.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Game")
	TSoftObjectPtr<UDoomGameData> GameData = TSoftObjectPtr<UDoomGameData>(FSoftObjectPath(TEXT("/Game/Doom/Data/DA_DoomGameData.DA_DoomGameData")));

	/** 1 (I'm too young to die) .. 5 (Nightmare!) */
	UPROPERTY(Config, EditAnywhere, Category = "Game", meta = (ClampMin = 1, ClampMax = 5))
	int32 Skill = 3;

	/**
	 * Classic: unlit surfaces with Doom's sector light and distance diminishing (COLORMAP-style).
	 * Modern: lit surfaces, Lumen GI/reflections (hardware ray tracing when available), dynamic
	 * point lights on projectiles, muzzle flashes and light-emitting decorations, volumetric fog, bloom.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Rendering")
	bool bModernRenderer = false;

	UPROPERTY(Config, EditAnywhere, Category = "Rendering", meta = (ClampMin = 60, ClampMax = 130))
	float FieldOfView = 90.f;

	/** Classic renderer: light through the WAD's real COLORMAP rows (Doom's exact colours and
	 *  banding) instead of a smooth darkening. */
	UPROPERTY(Config, EditAnywhere, Category = "Rendering")
	bool bPaletteExact = true;

	UPROPERTY(Config, EditAnywhere, Category = "Input")
	float MouseSensitivity = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Input")
	bool bAlwaysRun = true;

	/** Doom aims vertically for you; with mouselook you can turn it off. */
	UPROPERTY(Config, EditAnywhere, Category = "Input")
	bool bAutoAim = true;

	/** Pickup and door messages at the top of the screen (Options > Messages). */
	UPROPERTY(Config, EditAnywhere, Category = "Game")
	bool bShowMessages = true;

	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = 0, ClampMax = 1))
	float SfxVolume = 0.8f;

	/** OPL-style FM music from the WAD's MUS/MIDI lumps and GENMIDI bank. */
	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = 0, ClampMax = 1))
	float MusicVolume = 0.6f;
};
