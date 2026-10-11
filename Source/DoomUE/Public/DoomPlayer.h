// The marine: an ACharacter (capsule 16 x 56 units, eye at 41, 24-unit steps,
// Doom's heavy gravity and quick acceleration) driven by Enhanced Input, with
// Doom's inventory, weapons state machine (psprites), damage and powerups.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "DoomInfo.h"
#include "DoomPlayerState.h"
#include "DoomPlayer.generated.h"

class UCameraComponent;
class UDoomSpriteComponent;

/** What the owning player's screen needs from the server-run weapon and powerup logic. */
USTRUCT()
struct FDoomPlayerView
{
	GENERATED_BODY()

	UPROPERTY() FString WeaponSprite;
	UPROPERTY() FString FlashSprite;
	UPROPERTY() uint16 WeaponFrame = 'A';
	UPROPERTY() uint16 FlashFrame = 0;
	UPROPERTY() float WeaponY = 128.f;
	UPROPERTY() float BobX = 0;
	UPROPERTY() float BobY = 0;
	UPROPERTY() float CameraZ = 0;
	UPROPERTY() int32 Invulnerability = 0;
	UPROPERTY() int32 Invisibility = 0;
	UPROPERTY() int32 RadSuit = 0;
	UPROPERTY() int32 LightAmp = 0;
	UPROPERTY() int32 BerserkTics = 0;
	UPROPERTY() int32 DamageCount = 0;
	UPROPERTY() int32 BonusCount = 0;
	UPROPERTY() int32 ExtraLight = 0;
	UPROPERTY() int32 FaceDamageDir = 0;
	UPROPERTY() int32 FaceEvilGrin = 0;
	UPROPERTY() int32 AttackDown = 0;
	UPROPERTY() int32 DeathTics = 0;
	UPROPERTY() bool bBerserk = false;
	UPROPERTY() bool bGodMode = false;
};
class UPointLightComponent;
class UDoomWorld;
class UDoomInputConfig;

UCLASS()
class DOOMUE_API ADoomPlayer : public ACharacter
{
	GENERATED_BODY()

public:
	ADoomPlayer(const FObjectInitializer& ObjectInitializer);

	void InitForLevel(UDoomWorld* InWorld, bool bPistolStart);
	void DoomTic();
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual float TakeDamage(float Damage, struct FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	/** Returns true when the item was taken. */
	bool TryPickup(const FDoomThingDef& Def, bool bDropped);

	/** Save games: position, view, velocity, inventory and powerups. */
	void DoomSerialize(FArchive& Ar);

	FVector2f DoomPos() const;
	float DoomZ() const;
	float DoomAngle() const;
	FVector EyeLocation() const;
	bool IsDead() const { return Inv().Health <= 0; }
	bool HasKey(int32 KeyColor) const; // 0 blue 1 yellow 2 red: card or skull

	/** The inventory lives in the player state (ADoomPlayerState::Inv); before the pawn is possessed
	    (or without a Doom player state) a local fallback keeps the game working. */
	FDoomInventory& Inv();
	const FDoomInventory& Inv() const;
	ADoomPlayerState* GetDoomPlayerState() const;
	// powerups, in tics remaining
	int32 Invulnerability = 0, Invisibility = 0, RadSuit = 0, LightAmp = 0;
	bool bBerserk = false;
	int32 BerserkTics = 0;   // counts up from the pickup (the red tinge fades with it)
	int32 DamageCount = 0;   // red tint
	int32 BonusCount = 0;    // gold tint
	int32 ExtraLight = 0;    // gun flash
	int32 AttackDown = 0;
	int32 FaceDamageDir = 0; // -1 left, 1 right
	int32 FaceEvilGrin = 0;
	bool bGodMode = false;

	// psprites (Doom 320x200 coordinates)
	FString WeaponSprite;
	TCHAR WeaponFrame = 'A';
	FString FlashSprite;
	TCHAR FlashFrame = 0;     // 0 = no flash
	float WeaponY = 128.f;    // 32 = up, 128 = down
	float BobX = 0, BobY = 0;
	EDoomWeapon PendingWeapon = EDoomWeapon::Count;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPointLightComponent> MuzzleLight;
	/** The marine as other players see them (PLAY sprites); hidden from the owner's own view. */
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UDoomSpriteComponent> Body;

	// --- networking ---------------------------------------------------------------------------
	/** A message on this player's screen (pickups, locked doors, secrets). */
	void Message(const FString& Text);
	/** A sound this player makes: heard by them in 2D, by everyone else at their position. */
	void PlaySelfSound(const FString& Name, float Volume = 1.f);
	virtual void BeginPlay() override;
	/** Level geometry is never a movement base: UDoomWorld carries bodies on lifts itself, and the
	    level's runtime mesh components cannot be referenced over the network. */
	virtual void SetBase(FMovementBaseInterfaceData* MovementBaseInterfaceData, const FName BoneName = NAME_None, bool bNotifyActor = true) override;
	using ACharacter::SetBase;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// input handlers
	void Move(const FInputActionValue& V);
	void Look(const FInputActionValue& V);
	void FirePressed(const FInputActionValue& V);
	void FireReleased(const FInputActionValue& V);
	void UsePressed(const FInputActionValue& V);
	void RunPressed(const FInputActionValue& V) { bRunHeld = true; }
	void RunReleased(const FInputActionValue& V) { bRunHeld = false; }
	void SelectSlot(int32 Slot);
	void CycleWeapon(int32 Dir);
	void ToggleAutomap(const FInputActionValue& V);
	bool InputLocked() const;

private:
	// input that only the server may act on
	UFUNCTION(Server, Reliable)
	void ServerSetFire(bool bHeld);
	UFUNCTION(Server, Reliable)
	void ServerUse();
	UFUNCTION(Server, Reliable)
	void ServerSelectSlot(int32 Slot);
	UFUNCTION(Server, Reliable)
	void ServerCycleWeapon(int32 Dir);
	UFUNCTION(Client, Reliable)
	void ClientShowMessage(const FString& Text);
	UFUNCTION(Client, Unreliable)
	void ClientPlaySound(const FString& Name, float Volume);
	void UseLinesOrRespawn();
	void SelectSlotNow(int32 Slot);
	void CycleWeaponNow(int32 Dir);
	/** Server: the weapon and powerup state the owner's HUD draws. */
	UPROPERTY(ReplicatedUsing = OnRep_View)
	FDoomPlayerView View;
	UFUNCTION()
	void OnRep_View();
	void PublishView();
	void AnimateBody();
	FString BodySeq;

	enum class EWeaponPhase : uint8 { Raising, Ready, Firing, Lowering };
	void TicWeapon();
	void StartFire();
	void FireWeapon(int32 FrameIndex);
	bool CheckAmmo();
	void BringUpWeapon();
	void SetWeaponSprite();
	void HitscanAttack(int32 Pellets, float SpreadYaw, float SpreadPitch, int32 DamageMul, int32 DamageDie, bool bAccurate);
	void MeleeAttack(bool bSaw);
	float AimSlope(float DoomAngleDeg, float Range, AActor*& OutTarget) const;
	void TicPowerups();
	void TicDeath();

	UPROPERTY()
	TObjectPtr<UDoomWorld> DoomWorld;

	EWeaponPhase Phase = EWeaponPhase::Raising;
	int32 FireIndex = 0;
	int32 FrameTics = 0;
	int32 FlashTics = 0;
	int32 FlashIndex = 0;
	bool bFireHeld = false;
	bool bRunHeld = false;
	bool bRefire = false;
	FVector2f LastDoomPos;
	float BobPhase = 0;
	float ViewHeightOffset = 0;
	int32 DeathTics = 0;
	int32 SawIdle = 0;
	int32 ReactionTime = 0;
	float LastLandingVelocity = 0;
private:
	FDoomInventory LocalInv;
};
