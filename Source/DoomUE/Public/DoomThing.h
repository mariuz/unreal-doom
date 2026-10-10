// Non-monster things: decorations, pickups, barrels, corpses and short-lived
// effects (puffs, blood, teleport fog). A capsule for collision/overlaps, a
// sprite, and (modern renderer) a point light for light-emitting things.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DoomInfo.h"
#include "DoomThing.generated.h"

class UCapsuleComponent;
class UDoomSpriteComponent;
class UPointLightComponent;
class UDoomWorld;

UCLASS()
class DOOMUE_API ADoomThing : public AActor
{
	GENERATED_BODY()

public:
	ADoomThing();

	void InitThing(UDoomWorld* InWorld, const FDoomThingDef* InDef, float InDoomAngle, bool bInDropped);
	void InitEffect(UDoomWorld* InWorld, const FString& Sprite, const FString& Frames, int32 Tics, bool bFullBright);

	/** 35 Hz update from UDoomWorld. */
	void DoomTic();

	/** Keeps the thing on its sector's floor (or ceiling for hanging things). */
	void SnapToPlanes();

	/** Save games: health, death and animation. */
	void DoomSerialize(FArchive& Ar);

	virtual float TakeDamage(float Damage, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	const FDoomThingDef* Def = nullptr;
	bool bDropped = false;
	bool bEffect = false;
	bool bDead = false;
	int32 Health = 0;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCapsuleComponent> Capsule;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UDoomSpriteComponent> Sprite;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> Light;

	/** Who shot the barrel (gets the credit for the explosion). */
	TWeakObjectPtr<AActor> LastAttacker;

protected:
	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* Other, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& Sweep);

	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;
	int32 ExplodeDelay = -1;
};
