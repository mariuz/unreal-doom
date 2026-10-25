// UDoomWorld: what the player sees and hears besides the level itself: the environment (sky,
// fog, post process), sounds and music, the screen palette, messages and the material globals.
#include "DoomWorld.h"
#include "DoomWorldSpecialTable.h"
#include "Engine/DamageEvents.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "DoomUE.h"
#include "DoomWad.h"
#include "DoomGraphics.h"
#include "DoomSound.h"
#include "DoomLevel.h"
#include "DoomMapTiles.h"
#include "DoomThing.h"
#include "DoomMonster.h"
#include "DoomProjectile.h"
#include "DoomPlayer.h"
#include "DoomSettings.h"
#include "DoomAssets.h"
#include "DoomMapActors.h"
#include "DoomGameMode.h"
#include "EngineUtils.h"
#include "DoomGameState.h"
#include "DoomPlayerState.h"
#include "DoomMusic.h"
#include "DoomMapGen.h"
#include "Components/PointLightComponent.h"
#include "Perception/AISense_Hearing.h"
#include "DoomDehacked.h"
#include "Misc/FileHelper.h"
#include "Components/AudioComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/DirectionalLight.h"
#include "Components/SkyLightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "DoomSpriteComponent.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Tasks/Task.h"

void UDoomWorld::SetupEnvironment()
{
	UWorld* W = GetWorld();
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	PostProcess = W->SpawnActor<APostProcessVolume>(APostProcessVolume::StaticClass(), FTransform::Identity, SP);
	PostProcess->bUnbound = true;
	FPostProcessSettings& PP = PostProcess->Settings;
	PP.bOverride_AutoExposureMethod = true;
	PP.AutoExposureMethod = AEM_Manual;
	PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	PP.AutoExposureApplyPhysicalCameraExposure = false;
	PP.bOverride_MotionBlurAmount = true;
	PP.MotionBlurAmount = 0.f;
	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = 0.f;
	PP.bOverride_LensFlareIntensity = true;
	PP.LensFlareIntensity = 0.f;

	if (!bModern)
	{
		// Classic: the material already outputs Doom's palette colours; show them 1:1.
		PP.bOverride_AutoExposureBias = true;
		PP.AutoExposureBias = 0.263f; // log2(1.2): undo the lens attenuation of manual exposure
		PP.bOverride_ToneCurveAmount = true;
		PP.ToneCurveAmount = 0.f;
		PP.bOverride_ExpandGamut = true;
		PP.ExpandGamut = 0.f;
		PP.bOverride_BlueCorrection = true;
		PP.BlueCorrection = 0.f;
		PP.bOverride_BloomIntensity = true;
		PP.BloomIntensity = 0.f;
		PP.bOverride_AmbientOcclusionIntensity = true;
		PP.AmbientOcclusionIntensity = 0.f;
		PP.bOverride_DynamicGlobalIlluminationMethod = true;
		PP.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
		PP.bOverride_ReflectionMethod = true;
		PP.ReflectionMethod = EReflectionMethod::None;
		return;
	}

	// Modern: Lumen GI + reflections lit by emissive sector light, dynamic lights, fog and bloom.
	PP.bOverride_AutoExposureBias = true;
	PP.AutoExposureBias = 1.0f;
	PP.bOverride_DynamicGlobalIlluminationMethod = true;
	PP.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::Lumen;
	PP.bOverride_ReflectionMethod = true;
	PP.ReflectionMethod = EReflectionMethod::Lumen;
	PP.bOverride_LumenSceneLightingQuality = true;
	PP.LumenSceneLightingQuality = 1.f;
	PP.bOverride_LumenFinalGatherQuality = true;
	PP.LumenFinalGatherQuality = 1.f;
	PP.bOverride_BloomIntensity = true;
	PP.BloomIntensity = 0.5f;
	PP.bOverride_AmbientOcclusionIntensity = true;
	PP.AmbientOcclusionIntensity = 0.5f;

	AExponentialHeightFog* Fog = W->SpawnActor<AExponentialHeightFog>(AExponentialHeightFog::StaticClass(), FTransform(FVector(0, 0, -2000)), SP);
	UExponentialHeightFogComponent* FC = Fog->GetComponent();
	FC->SetFogDensity(0.002f);
	FC->SetFogHeightFalloff(0.02f);
	FC->SetVolumetricFog(true);
	FC->SetFogInscatteringColor(FLinearColor(0.05f, 0.05f, 0.06f));
	EnvironmentActors.Add(Fog);

	ADirectionalLight* Sun = W->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform(FRotator(-55, 35, 0), FVector::ZeroVector), SP);
	Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetLightComponent()->SetIntensity(2.5f);
	Sun->GetLightComponent()->SetLightColor(FLinearColor(1.f, 0.85f, 0.7f));
	Sun->GetLightComponent()->SetVolumetricScatteringIntensity(1.f);
	EnvironmentActors.Add(Sun);

	ASkyLight* Sky = W->SpawnActor<ASkyLight>(ASkyLight::StaticClass(), FTransform::Identity, SP);
	USkyLightComponent* SC = Sky->GetLightComponent();
	SC->SetMobility(EComponentMobility::Movable);
	SC->bRealTimeCapture = true;
	SC->SetIntensity(0.4f);
	SC->RecaptureSky();
	EnvironmentActors.Add(Sky);
}

void UDoomWorld::PlaySound(const FString& Name, const FVector* Location, float Volume)
{
	PlaySoundLocal(Name, Location, Volume);
	if (IsNetServer())
	{
		if (ADoomGameState* GS = GetDoomGameState())
		{
			GS->MulticastSound(Name, Location ? *Location : FVector::ZeroVector, Location != nullptr, Volume, nullptr);
		}
	}
}

void UDoomWorld::PlaySoundLocal(const FString& Name, const FVector* Location, float Volume)
{
	if (Sound.IsValid())
	{
		Sound->Play(GetWorld(), Name, Location, Volume);
	}
}

bool UDoomWorld::IsPaletteExact() const
{
	return !bModern && GetDefault<UDoomSettings>()->bPaletteExact && !FParse::Param(FCommandLine::Get(), TEXT("smooth"));
}

int32 UDoomWorld::ScreenPalette() const
{
	const ADoomPlayer* P = GetPlayer();
	if (!P || !Wad.IsValid() || Flow().Phase != EDoomGameState::Playing)
	{
		return 0;
	}
	static int32 Forced = -1;
	static const bool bForced = FParse::Value(FCommandLine::Get(), TEXT("-forcepalette="), Forced);
	if (bForced)
	{
		return FMath::Clamp(Forced, 0, Wad->NumPalettes() - 1); // screenshots of a given palette
	}
	int32 Palette = 0;
	int32 Hurt = P->GetView().DamageCount;
	if (P->GetView().bBerserk)
	{
		Hurt = FMath::Max(Hurt, 12 - (P->GetView().BerserkTics >> 6)); // berserk's fading red
	}
	if (Hurt > 0)
	{
		Palette = 1 + FMath::Min((Hurt + 7) >> 3, 7);
	}
	else if (P->GetView().BonusCount > 0)
	{
		Palette = 9 + FMath::Min((P->GetView().BonusCount + 7) >> 3, 3);
	}
	else if (P->GetView().RadSuit > 4 * 32 || (P->GetView().RadSuit & 8))
	{
		Palette = 13;
	}
	return FMath::Clamp(Palette, 0, Wad->NumPalettes() - 1);
}

void UDoomWorld::ApplyUserSettings()
{
	const UDoomSettings* S = GetDefault<UDoomSettings>();
	if (Sound.IsValid())
	{
		Sound->MasterVolume = S->SfxVolume;
	}
	if (MusicWave && MusicWave->GetPlayer().IsValid())
	{
		MusicWave->GetPlayer()->Volume = S->MusicVolume;
	}
	if (S->MusicVolume <= 0.f)
	{
		StopMusic();
	}
	else if (!MusicComponent && Map.IsValid())
	{
		const FDoomMapInfo* Info = MapInfo(Flow().CurrentMap);
		PlayMusic((Info && !Info->Music.IsEmpty()) ? Info->Music : MusicForMap(Flow().CurrentMap));
	}
	if (ADoomPlayer* P = GetPlayer())
	{
		P->GetCamera()->SetFieldOfView(S->FieldOfView);
	}
}

void UDoomWorld::StopMusic()
{
	if (MusicComponent)
	{
		MusicComponent->Stop();
		MusicComponent->DestroyComponent();
		MusicComponent = nullptr;
	}
	MusicWave = nullptr;
	CurrentMusic.Reset();
}

void UDoomWorld::PlayTitleMusic()
{
	if (Wad.IsValid())
	{
		PlayMusic(Wad->IsDoom2() ? TEXT("D_DM2TTL") : TEXT("D_INTRO"));
	}
}

void UDoomWorld::PlayMusic(const FString& Lump)
{
	if (Lump == CurrentMusic && MusicComponent)
	{
		return; // same song keeps playing, like Doom
	}
	StopMusic();
	const float Volume = GetDefault<UDoomSettings>()->MusicVolume;
	if (Lump.IsEmpty() || !Wad.IsValid() || Volume <= 0.f || FParse::Param(FCommandLine::Get(), TEXT("nomusic")))
	{
		return;
	}
	TSharedPtr<FDoomMusicPlayer> Player = DoomMusic::CreatePlayer(*Wad, Lump, true);
	if (!Player)
	{
		return;
	}
	Player->Volume = Volume;
	MusicWave = NewObject<UDoomMusicWave>(this);
	MusicWave->Start(Player);
	// a 2D, non-spatialized component that survives map changes inside this world
	MusicComponent = UGameplayStatics::CreateSound2D(GetWorld(), MusicWave, 1.f, 1.f, 0.f, nullptr, true, false);
	if (MusicComponent)
	{
		MusicComponent->bIsMusic = true;
		MusicComponent->Play();
		CurrentMusic = Lump;
	}
}

void UDoomWorld::ShowMessage(const FString& Msg)
{
	Message = Msg;
	MessageTics = 4 * Doom::TicRate;
}

void UDoomWorld::UpdateMaterialGlobals(float SubTic)
{
	// per-frame material globals (UMaterialParameterCollection)
	if (UMaterialParameterCollection* MPC = DoomAssets::Collection())
	{
		ADoomPlayer* P = GetPlayer();
		const float Tic = Flow().LevelTic + SubTic;
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("Tic"), Tic);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("ExtraLight"), P ? P->GetView().ExtraLight : 0);
		float Fixed = -1.f;
		if (P)
		{
			if (P->GetView().Invulnerability > 4 * 32 || (P->GetView().Invulnerability & 8)) Fixed = 32.f;
			else if (P->GetView().LightAmp > 4 * 32 || (P->GetView().LightAmp & 8)) Fixed = 1.f;
		}
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("FixedColormap"), Fixed);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("Modern"), bModern ? 1.f : 0.f);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("Palette"), IsPaletteExact() ? ScreenPalette() : 0);
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("PaletteRows"), Gfx->ColormapRows());
		UKismetMaterialLibrary::SetScalarParameterValue(this, MPC, TEXT("PaletteExact"),
			(GetDefault<UDoomSettings>()->bPaletteExact && !FParse::Param(FCommandLine::Get(), TEXT("smooth"))) ? 1.f : 0.f);
	}
}
