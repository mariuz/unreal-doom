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

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	const FDoomProjectileDef* GetDef() const { return Def; }
	/** Who fired it (gets the credit, isn't hit by it). */
	AActor* GetSource() const { return Source.Get(); }
	/** A revenant missile's homing target. */
	AActor* GetTracer() const { return Tracer.Get(); }
	bool HasExploded() const { return bExploded; }
	/** The BFG ball's flight direction when it burst: its tracers fan out around it. */
	const TOptional<float>& GetSprayYaw() const { return SprayYaw; }
	UProjectileMovementComponent* GetMovement() const { return Movement; }
	UDoomSpriteComponent* GetSprite() const { return Sprite; }
	UPointLightComponent* GetLight() const { return Light; }

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USphereComponent> Sphere;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProjectileMovementComponent> Movement;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UDoomSpriteComponent> Sprite;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> Light;

	/** Networking: the missile type; clients show it and follow the replicated movement. */
	UPROPERTY(ReplicatedUsing = OnRep_NetType)
	FString NetType;
	UFUNCTION()
	void OnRep_NetType();

	const FDoomProjectileDef* Def = nullptr;
	TWeakObjectPtr<AActor> Source;
	TWeakObjectPtr<AActor> Tracer;
	bool bExploded = false;
	TOptional<float> SprayYaw;

	UFUNCTION()
	void OnStop(const FHitResult& Hit);
	void Explode(AActor* HitActor);
	void BfgSpray();

	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;
	int32 Age = 0;
};
