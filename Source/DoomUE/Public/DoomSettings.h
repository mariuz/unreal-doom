#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DoomSettings.generated.h"

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

	UPROPERTY(Config, EditAnywhere, Category = "Input")
	float MouseSensitivity = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category = "Input")
	bool bAlwaysRun = true;

	/** Doom aims vertically for you; with mouselook you can turn it off. */
	UPROPERTY(Config, EditAnywhere, Category = "Input")
	bool bAutoAim = true;

	UPROPERTY(Config, EditAnywhere, Category = "Audio", meta = (ClampMin = 0, ClampMax = 1))
	float SfxVolume = 0.8f;
};
