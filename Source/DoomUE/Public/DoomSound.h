// DMX sound lumps (DS*) played through Unreal's audio mixer as procedural waves,
// spatialized with a Doom-like attenuation (full volume within 200 units, silent at 1200).
#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"

class FDoomWad;
class UWorld;
class USoundAttenuation;
class USoundConcurrency;

class DOOMUE_API FDoomSound : public FGCObject
{
public:
	explicit FDoomSound(const FDoomWad& InWad);

	/** Plays DS<Name> at Location (3D), or 2D when Location is null. */
	void Play(UWorld* World, const FString& Name, const FVector* Location, float Volume = 1.f);

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FDoomSound"); }

	float MasterVolume = 0.8f;

private:
	struct FPcm
	{
		TArray<uint8> Samples16;
		int32 Rate = 11025;
	};
	const FPcm* Get(const FString& Name);

	const FDoomWad& Wad;
	TMap<FString, TSharedPtr<FPcm>> Cache;
	TObjectPtr<USoundAttenuation> Attenuation = nullptr;
	TObjectPtr<USoundConcurrency> Concurrency = nullptr;
};
