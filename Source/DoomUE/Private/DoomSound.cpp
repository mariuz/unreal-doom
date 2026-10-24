#include "DoomSound.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "DoomWad.h"
#include "DoomUE.h"
#include "DoomInfo.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Kismet/GameplayStatics.h"

FDoomSound::FDoomSound(const FDoomWad& InWad)
	: Wad(InWad)
{
	Attenuation = NewObject<USoundAttenuation>(GetTransientPackage());
	FSoundAttenuationSettings& A = Attenuation->Attenuation;
	A.bAttenuate = true;
	A.bSpatialize = true;
	A.AttenuationShape = EAttenuationShape::Sphere;
	A.AttenuationShapeExtents = FVector(200.f * Doom::Scale);
	A.FalloffDistance = 1000.f * Doom::Scale;
	A.DistanceAlgorithm = EAttenuationDistanceModel::Linear;
	A.SpatializationAlgorithm = ESoundSpatializationAlgorithm::SPATIALIZATION_Default;
	A.bEnableOcclusion = false;

	// Doom has 8 channels; let Unreal voice-steal the oldest beyond 24 overlapping sounds.
	Concurrency = NewObject<USoundConcurrency>(GetTransientPackage());
	Concurrency->Concurrency.MaxCount = 24;
	Concurrency->Concurrency.ResolutionRule = EMaxConcurrentResolutionRule::StopOldest;
}

void FDoomSound::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Attenuation);
	Collector.AddReferencedObject(Concurrency);
}

const FDoomSound::FPcm* FDoomSound::Get(const FString& Name)
{
	if (TSharedPtr<FPcm>* Found = Cache.Find(Name))
	{
		return Found->Get();
	}
	TConstArrayView<uint8> D = Wad.LumpData(TEXT("DS") + Name);
	TSharedPtr<FPcm> Pcm;
	if (D.Num() > 8 && WadRead::U16(D.GetData()) == 3)
	{
		Pcm = MakeShared<FPcm>();
		Pcm->Rate = WadRead::U16(&D[2]);
		int32 Count = WadRead::S32(&D[4]);
		int32 Start = 8;
		// DMX pads 16 bytes before and after the samples.
		if (Count > 32 && 8 + Count <= D.Num())
		{
			Start += 16;
			Count -= 32;
		}
		Count = FMath::Clamp(Count, 0, D.Num() - Start);
		Pcm->Samples16.SetNumUninitialized(Count * 2);
		int16* Out = reinterpret_cast<int16*>(Pcm->Samples16.GetData());
		for (int32 i = 0; i < Count; ++i)
		{
			Out[i] = (int16)(((int32)D[Start + i] - 128) << 8);
		}
	}
	const FPcm* Result = Pcm.Get();
	Cache.Add(Name, MoveTemp(Pcm));
	return Result;
}

void FDoomSound::Play(UWorld* World, const FString& Name, const FVector* Location, float Volume)
{
	if (!World || Name.IsEmpty())
	{
		return;
	}
	const FPcm* Pcm = Get(DoomInfo::SoundName(Name));
	if (!Pcm || Pcm->Samples16.Num() == 0)
	{
		return;
	}
	// A procedural wave is a single-voice stream, so each playback gets its own.
	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage());
	Wave->SetSampleRate(Pcm->Rate);
	Wave->NumChannels = 1;
	Wave->Duration = (float)(Pcm->Samples16.Num() / 2) / Pcm->Rate;
	Wave->SoundGroup = SOUNDGROUP_Default;
	Wave->bLooping = false;
	Wave->QueueAudio(Pcm->Samples16.GetData(), Pcm->Samples16.Num());

	const float Vol = Volume * MasterVolume;
	if (Location)
	{
		UGameplayStatics::PlaySoundAtLocation(World, Wave, *Location, FRotator::ZeroRotator, Vol, 1.f, 0.f, Attenuation, Concurrency);
	}
	else
	{
		UGameplayStatics::PlaySound2D(World, Wave, Vol, 1.f, 0.f, Concurrency);
	}
}
