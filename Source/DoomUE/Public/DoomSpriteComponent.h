// A Doom sprite: a unit quad (a 4-vertex procedural mesh) scaled and offset to the
// patch's size and offsets, turned to face the camera around the vertical axis like
// Doom's screen-aligned sprites, picking one of 8 rotations from the viewing angle.
// Lighting comes from the same per-sector light texture as the walls.
//
// Networking: the server's animation state (prefix, sequence, timing, brightness, fuzz,
// facing) replicates in FDoomSpriteNet; clients restart the sequence when its serial changes
// and animate it themselves at 35 Hz, so frames cost no bandwidth.
#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "DoomSpriteComponent.generated.h"

class UMaterialInstanceDynamic;
class UDoomWorld;

/** What clients need to show a server-animated sprite. */
USTRUCT()
struct FDoomSpriteNet
{
	GENERATED_BODY()

	UPROPERTY() FString Sprite;
	UPROPERTY() FString Seq;
	UPROPERTY() uint8 Tics = 8;
	UPROPERTY() bool bLoop = true;
	UPROPERTY() bool bBright = false;
	UPROPERTY() bool bFuzz = false;
	/** Bumped by every PlaySequence, so replaying the same frames restarts them. */
	UPROPERTY() uint8 Serial = 0;
	/** Facing, whole degrees. */
	UPROPERTY() uint16 Angle = 0;
};

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

	/** Corpses: once a non-looping sequence (the death frames) is over, stop this component's tick and
	 *  let UDoomWorld turn it to the camera in one batched pass, only while it is on screen. */
	bool bSleepWhenDone = false;
	bool IsSleeping() const { return bSleeping; }
	/** Back to ticking every frame (a new sequence, a resurrection, a corpse carried by a floor). */
	void Wake();
	/** The batched pass: face the camera and pick the rotation, as the tick would. */
	void RefreshFacing(const FVector& ViewLocation) { UpdateVisual(ViewLocation); }

	/** Save games: sprite prefix, sequence, frame, timing, brightness and fuzz. */
	void DoomSerialize(FArchive& Ar);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_Net)
	FDoomSpriteNet Net;
	UFUNCTION()
	void OnRep_Net();
	/** True where this sprite's state is decided (server, standalone, or a client-only effect). */
	bool IsSimulated() const;
	/** Points the material at the level's sector light texture once the level exists. */
	void BindLevel();
	uint8 LastSerial = 0;
	float ClientTicTime = 0;
	bool bLevelBound = false;
	bool bSleeping = false;

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
