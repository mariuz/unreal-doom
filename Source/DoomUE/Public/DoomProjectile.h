// Missiles (imp fireballs, rockets, plasma, BFG balls...): a sphere moved by
// UProjectileMovementComponent (swept, so it never tunnels through thin walls),
// a full-bright sprite, and a dynamic point light that Lumen picks up.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DoomProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UDoomSpriteComponent;
class UPointLightComponent;
class UDoomWorld;
struct FDoomProjectileDef;

UCLASS()
class DOOMUE_API ADoomProjectile : public AActor
{
	GENERATED_BODY()

public:
	ADoomProjectile();

	void InitProjectile(UDoomWorld* InWorld, const FDoomProjectileDef* InDef, AActor* InSource, const FVector& Direction, AActor* InTracer);
	void DoomTic();
	/** Save games: age, velocity (Nightmare speed, homing turns) and the sprite's animation. */
	void DoomSerialize(FArchive& Ar);

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USphereComponent> Sphere;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProjectileMovementComponent> Movement;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UDoomSpriteComponent> Sprite;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> Light;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Networking: the missile type; clients show it and follow the replicated movement. */
	UPROPERTY(ReplicatedUsing = OnRep_NetType)
	FString NetType;
	UFUNCTION()
	void OnRep_NetType();

	const FDoomProjectileDef* Def = nullptr;
	TWeakObjectPtr<AActor> Source;
	TWeakObjectPtr<AActor> Tracer;
	bool bExploded = false;

private:
	UFUNCTION()
	void OnStop(const FHitResult& Hit);
	void Explode(AActor* HitActor);
	void BfgSpray();

	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;
	int32 Age = 0;
};
