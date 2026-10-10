// A Doom sprite: a unit quad (a 4-vertex procedural mesh) scaled and offset to the
// patch's size and offsets, turned to face the camera around the vertical axis like
// Doom's screen-aligned sprites, picking one of 8 rotations from the viewing angle.
// Lighting comes from the same per-sector light texture as the walls.
#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "DoomSpriteComponent.generated.h"

class UMaterialInstanceDynamic;
class UDoomWorld;

UCLASS(ClassGroup = Doom, meta = (BlueprintSpawnableComponent))
class DOOMUE_API UDoomSpriteComponent : public UProceduralMeshComponent
{
	GENERATED_BODY()

public:
	UDoomSpriteComponent(const FObjectInitializer& ObjectInitializer);

	void Init(UDoomWorld* InWorld, const FString& InSprite);
	void SetSpritePrefix(const FString& InSprite) { Sprite = InSprite; bDirty = true; }

	/** Plays a frame sequence such as "ABCD"; one frame per Tics tics. */
	void PlaySequence(const FString& Frames, int32 Tics, bool bLoop, bool bFullBright = false);
	/** Advances one tic. Returns true when the frame changed. */
	bool AnimateTic();
	bool IsSequenceDone() const { return bDone; }
	TCHAR CurrentFrame() const { return Seq.IsValidIndex(Index) ? Seq[Index] : TEXT('A'); }
	int32 FrameIndex() const { return Index; }
	const FString& CurrentSequence() const { return Seq; }

	/** Doom facing angle (degrees, CCW from east) used to choose rotations. */
	float DoomAngle = 0;
	/** Sector whose light the sprite takes. */
	void SetSector(int32 Sector);
	void SetFullBright(bool b);
	void SetFuzz(bool b);

	/** Save games: sprite prefix, sequence, frame, timing, brightness and fuzz. */
	void DoomSerialize(FArchive& Ar);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void UpdateVisual(const FVector& ViewLocation);

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> MID;
	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;

	FString Sprite;
	FString Seq = TEXT("A");
	int32 Index = 0;
	int32 TicsPerFrame = 8;
	int32 TicCount = 0;
	bool bLoop = true;
	bool bDone = false;
	bool bDirty = true;
	int32 LastRotation = -1;
	int32 Sector = -1;
	const void* LastPicture = nullptr;
	bool bLastMirror = false;
	bool bBright = false;
	bool bFuzzOn = false;
};
