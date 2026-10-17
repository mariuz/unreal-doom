// Monsters are Characters: the Character Movement Component does the walking,
// stepping (24 units), ledge avoidance and flying, and collides against the level's
// procedural meshes. The decisions follow Doom's p_enemy.c: A_Look, A_Chase with
// 8 movement directions and P_NewChaseDir, melee/missile range checks, pain chance,
// infighting, and the special attacks (lost soul charge, arch-vile fire and
// resurrection, pain elemental spawning, revenant tracers, mancubus spreads).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "DoomInfo.h"
#include "DoomMonster.generated.h"

class UDoomSpriteComponent;
class UPointLightComponent;
class UBoxComponent;
class UDoomWorld;

UENUM()
enum class EMonsterState : uint8 { Idle, Chase, Melee, Missile, Pain, Dying, Dead, Raising, Charging };

UCLASS()
class DOOMUE_API ADoomMonster : public ACharacter
{
	GENERATED_BODY()
	friend class UDoomWorld;    // teleports, telefrags
	friend class ADoomGameMode; // the scripted test runs (-playtest)

public:
	ADoomMonster(const FObjectInitializer& ObjectInitializer);

	void InitMonster(UDoomWorld* InWorld, const FDoomThingDef* InDef, float InDoomAngle, bool bInAmbush);
	void DoomTic();
	virtual void Tick(float DeltaSeconds) override;

	virtual float TakeDamage(float Damage, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;
	virtual void Landed(const FHitResult& Hit) override;
	/** Vanilla mobjinfo mass, for knockback. */
	static float MonsterMass(const FDoomThingDef* D);
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	/** Level geometry is never a movement base: UDoomWorld carries bodies on lifts itself, and the
	    level's runtime mesh components cannot be referenced over the network. */
	virtual void SetBase(FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName = NAME_None, bool bNotifyActor = true) override;
	using ACharacter::SetBase;

	/** Networking: the type (clients set up the same capsule and sprite) and whether it is a corpse. */
	UPROPERTY(ReplicatedUsing = OnRep_NetType)
	int32 NetType = 0;
	UPROPERTY(ReplicatedUsing = OnRep_NetCorpse)
	bool bNetCorpse = false;
	UFUNCTION()
	void OnRep_NetType();
	UFUNCTION()
	void OnRep_NetCorpse();
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

	/** Woken by noise or sight. */
	void WakeUp(AActor* NewTarget, bool bFromSound);
	bool IsDead() const { return State == EMonsterState::Dying || State == EMonsterState::Dead; }
	bool CanBeRaised() const;
	void Resurrect();

	/** Save games: AI state, health, movement and animation (the target is saved by UDoomWorld). */
	void DoomSerialize(FArchive& Ar);

	FVector2f DoomPos() const;
	float DoomZ() const; // feet height in Doom units

	const FDoomThingDef* Def = nullptr;
	int32 Health = 0;
	float DoomAngle = 0;
	bool bAmbush = false;
	EMonsterState State = EMonsterState::Idle;
	TWeakObjectPtr<AActor> Target;

	/** Where the map placed it (Nightmare respawning); bCanRespawn is false for spawned extras. */
	FVector SpawnFeet = FVector::ZeroVector;
	float SpawnAngle = 0;
	bool bCanRespawn = false;
	int32 DeadTics = 0;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UDoomSpriteComponent> Sprite;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> Light;
	/** Doom's full box width for bullets and missiles, on monsters wider than they are tall. */
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UBoxComponent> HitBox;

private:
	void Look();
	void Chase();
	void FaceTarget();
	bool CheckMeleeRange() const;
	bool CheckMissileRange();
	bool TryWalk(int32 Dir);
	bool TestMove(int32 Dir) const;
	void NewChaseDir();
	void StartState(EMonsterState S);
	void DoAttack(bool bMelee);
	void Die(AActor* Killer, bool bGib);
	void MakeCorpse();
	void HitscanAttack(int32 Pellets, float SpreadDeg, int32 DamageMul);
	bool VileTryRaise();
	FVector EyeLocation() const;

	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;

	int32 MoveDir = 8;  // 0..7 = E, NE, N, NW, W, SW, S, SE ; 8 = none
	int32 MoveCount = 0;
	int32 ReactionTime = 8;
	int32 Threshold = 0;
	bool bJustAttacked = false;
	bool bJustHit = false;
	int32 StepTics = 0;
	FVector2f StepStart;
	FVector2f LastPos;
	int32 FireFrameSeen = -1;
	TWeakObjectPtr<ADoomMonster> RaiseTarget;
	FVector ChargeVelocity = FVector::ZeroVector;
	int32 ChargeTics = 0;
};
